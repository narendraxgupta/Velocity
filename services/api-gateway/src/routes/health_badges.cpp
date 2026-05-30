// =============================================================================
//  /v1/leaderboard/health* — anomaly-detector verdict surface.
//
//  Verdicts are written to Redis by the anomaly-detector service under
//  ``health:<submission_id>``. We expose two read paths:
//
//    GET /v1/leaderboard/health
//      → { "submissions": [ { submission_id, health, rank_pct, reason }, ... ] }
//      Returns every cached verdict (limited to ~256 by anomaly-detector
//      window size). The leaderboard frontend reads this once on mount
//      and merges by submission_id with the live leaderboard rows.
//
//    GET /v1/leaderboard/health/{id}
//      → single verdict (404 if no row cached for the submission).
//
//  We do NOT proxy POST /v1/observe — that's an internal contract
//  between bot-controller and anomaly-detector and explicitly not
//  client-callable. The detector itself is on the apps network and not
//  reachable from outside the cluster.
// =============================================================================
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <drogon/HttpController.h>
#include <nlohmann/json.hpp>

#include "api_gateway/clients.h"
#include "velocity/common/log.h"

#include <sw/redis++/redis++.h>

namespace velocity::api_gateway::routes {

namespace {

[[nodiscard]] auto json_error(drogon::HttpStatusCode code, std::string_view msg) {
    nlohmann::json body{{"error", msg}};
    auto resp = [](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(body.dump());
    resp->setStatusCode(code);
    return resp;
}

}  // namespace

class HealthBadges : public drogon::HttpController<HealthBadges> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(HealthBadges::all,        "/v1/leaderboard/health",       drogon::Get);
        ADD_METHOD_TO(HealthBadges::one,        "/v1/leaderboard/health/{id}",  drogon::Get);
        ADD_METHOD_TO(HealthBadges::regression, "/v1/submissions/{id}/regression", drogon::Get);
    METHOD_LIST_END

    auto all(const drogon::HttpRequestPtr&,
             std::function<void(const drogon::HttpResponsePtr&)>&& cb) const -> void {
        auto* redis = clients::RedisClient::get();
        if (!redis) {
            cb(json_error(drogon::k503ServiceUnavailable, "redis unavailable"));
            return;
        }
        try {
            // SCAN over `health:*`. Window is small (≤ 256 entries in
            // practice) so we drain in one pass; the SCAN cursor keeps
            // us safe against the unbounded KEYS pattern.
            std::vector<std::string> keys;
            long long cursor = 0;
            do {
                cursor = redis->scan(cursor, "health:*", 100,
                                     std::back_inserter(keys));
            } while (cursor != 0);

            nlohmann::json body;
            body["submissions"] = nlohmann::json::array();
            for (const auto& key : keys) {
                if (auto v = redis->get(key)) {
                    try {
                        body["submissions"].push_back(nlohmann::json::parse(*v));
                    } catch (...) { /* skip malformed */ }
                }
            }
            cb([](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(body.dump()));
        } catch (const std::exception& e) {
            cb(json_error(drogon::k502BadGateway, e.what()));
        }
    }

    auto one(const drogon::HttpRequestPtr&,
             std::function<void(const drogon::HttpResponsePtr&)>&& cb,
             const std::string& id) const -> void {
        auto* redis = clients::RedisClient::get();
        if (!redis) {
            cb(json_error(drogon::k503ServiceUnavailable, "redis unavailable"));
            return;
        }
        try {
            if (auto v = redis->get("health:" + id)) {
                cb([](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(*v));
                return;
            }
            cb(json_error(drogon::k404NotFound, "no verdict cached"));
        } catch (const std::exception& e) {
            cb(json_error(drogon::k502BadGateway, e.what()));
        }
    }

    // GET /v1/submissions/{id}/regression — latest KS-test verdict.
    //
    // The anomaly-detector writes the cached KS report to
    // `regression:<submission_id>`. We read straight from Redis (one
    // hop) rather than proxying to the Python service so the gateway
    // path stays fast and dependency-light.
    auto regression(const drogon::HttpRequestPtr&,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                    const std::string& id) const -> void {
        auto* redis = clients::RedisClient::get();
        if (!redis) {
            cb(json_error(drogon::k503ServiceUnavailable, "redis unavailable"));
            return;
        }
        try {
            if (auto v = redis->get("regression:" + id)) {
                cb([](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(*v));
                return;
            }
            cb(json_error(drogon::k404NotFound, "no regression data cached"));
        } catch (const std::exception& e) {
            cb(json_error(drogon::k502BadGateway, e.what()));
        }
    }
};

}  // namespace velocity::api_gateway::routes
