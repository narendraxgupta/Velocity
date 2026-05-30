// =============================================================================
//  /v1/submissions/{id}/orderbook* — order book replay viewer endpoints.
//
//  Routes:
//
//    GET /v1/submissions/{id}/orderbook/timeline
//      → { "samples": [ elapsed_ms_0, elapsed_ms_1, ... ] }
//      Lists every snapshot the validator has written for this submission.
//      The frontend uses this to size its scrubber and bisect to find the
//      nearest-in-time snapshot when the user drags.
//
//    GET /v1/submissions/{id}/orderbook?elapsed_ms=NNN
//      → { "elapsed_ms": NNN, "bids": [{price, qty}, ...], "asks": [...] }
//      Returns the orderbook snapshot at elapsed_ms (or 404 if no
//      snapshot is within ±200ms of the requested time).
//
//  Both routes read directly from Redis. The validator publishes
//  snapshots under `orderbook:<submission_id>:t:<elapsed_ms>` keys and
//  maintains an index list at `orderbook:<submission_id>:index`. See
//  services/correctness-validator/src/validator.cpp::push_orderbook_snapshot.
// =============================================================================
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
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

// Snap to the closest snapshot within ±200ms — beyond that we'd be
// fabricating data the validator didn't capture.
constexpr std::int64_t kSnapToleranceMs = 200;

}  // namespace

class Orderbook : public drogon::HttpController<Orderbook> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(Orderbook::timeline,    "/v1/submissions/{id}/orderbook/timeline", drogon::Get);
        ADD_METHOD_TO(Orderbook::snapshot,    "/v1/submissions/{id}/orderbook",          drogon::Get);
        ADD_METHOD_TO(Orderbook::execQuality, "/v1/submissions/{id}/exec-quality",       drogon::Get);
    METHOD_LIST_END

    auto timeline(const drogon::HttpRequestPtr&,
                  std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                  const std::string& id) const -> void {
        auto* redis = clients::RedisClient::get();
        if (!redis) {
            cb(json_error(drogon::k503ServiceUnavailable, "redis unavailable"));
            return;
        }
        try {
            const auto key = "orderbook:" + id + ":index";
            std::vector<std::string> entries;
            redis->lrange(key, 0, -1, std::back_inserter(entries));
            // Redis LPUSH puts newest at head; the viewer wants ascending
            // elapsed_ms so we sort. (We can't trust LPUSH order anyway —
            // late-arriving events would clobber any insertion-time
            // assumption.)
            std::vector<std::int64_t> samples;
            samples.reserve(entries.size());
            for (const auto& s : entries) {
                try { samples.push_back(std::stoll(s)); }
                catch (...) { /* skip */ }
            }
            std::sort(samples.begin(), samples.end());
            samples.erase(std::unique(samples.begin(), samples.end()), samples.end());

            nlohmann::json body;
            body["samples"] = std::move(samples);
            cb([](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(body.dump()));
        } catch (const std::exception& e) {
            cb(json_error(drogon::k502BadGateway, e.what()));
        }
    }

    auto snapshot(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                  const std::string& id) const -> void {
        auto* redis = clients::RedisClient::get();
        if (!redis) {
            cb(json_error(drogon::k503ServiceUnavailable, "redis unavailable"));
            return;
        }
        std::int64_t requested_ms = 0;
        if (const auto p = req->getParameter("elapsed_ms"); !p.empty()) {
            try { requested_ms = std::stoll(p); }
            catch (...) {
                cb(json_error(drogon::k400BadRequest, "invalid elapsed_ms"));
                return;
            }
        }
        try {
            // Try the exact key first — cheapest path.
            const auto exact_key = "orderbook:" + id + ":t:" + std::to_string(requested_ms);
            if (auto v = redis->get(exact_key)) {
                cb([](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(*v));
                return;
            }
            // Bisect via the index.
            const auto idx_key = "orderbook:" + id + ":index";
            std::vector<std::string> entries;
            redis->lrange(idx_key, 0, -1, std::back_inserter(entries));
            std::vector<std::int64_t> samples;
            samples.reserve(entries.size());
            for (const auto& s : entries) {
                try { samples.push_back(std::stoll(s)); }
                catch (...) { /* skip */ }
            }
            if (samples.empty()) {
                cb(json_error(drogon::k404NotFound, "no snapshots for submission"));
                return;
            }
            std::sort(samples.begin(), samples.end());
            // Lower-bound, then pick whichever neighbour is closer.
            auto it = std::lower_bound(samples.begin(), samples.end(), requested_ms);
            std::int64_t chosen = 0;
            if (it == samples.end()) {
                chosen = samples.back();
            } else if (it == samples.begin()) {
                chosen = *it;
            } else {
                const auto hi = *it;
                const auto lo = *std::prev(it);
                chosen = (requested_ms - lo) <= (hi - requested_ms) ? lo : hi;
            }
            if (std::abs(chosen - requested_ms) > kSnapToleranceMs) {
                cb(json_error(drogon::k404NotFound, "no snapshot within ±200ms"));
                return;
            }
            const auto key = "orderbook:" + id + ":t:" + std::to_string(chosen);
            if (auto v = redis->get(key)) {
                cb([](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(*v));
                return;
            }
            cb(json_error(drogon::k404NotFound, "snapshot key vanished"));
        } catch (const std::exception& e) {
            cb(json_error(drogon::k502BadGateway, e.what()));
        }
    }

    // GET /v1/submissions/{id}/exec-quality
    //
    // Returns the latest execution-quality snapshot the validator wrote
    // to `exec_quality:<submission_id>`. 404 when no run has produced
    // exec-quality data yet (e.g. nothing has filled). The JSON body is
    // exactly what the validator emits; we don't reshape it on the
    // gateway to keep the surface area small and the formats aligned.
    auto execQuality(const drogon::HttpRequestPtr&,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                     const std::string& id) const -> void {
        auto* redis = clients::RedisClient::get();
        if (!redis) {
            cb(json_error(drogon::k503ServiceUnavailable, "redis unavailable"));
            return;
        }
        try {
            const auto k = "exec_quality:" + id;
            if (auto v = redis->get(k)) {
                cb([](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(*v));
                return;
            }
            cb(json_error(drogon::k404NotFound, "no exec-quality data yet"));
        } catch (const std::exception& e) {
            cb(json_error(drogon::k502BadGateway, e.what()));
        }
    }
};

}  // namespace velocity::api_gateway::routes
