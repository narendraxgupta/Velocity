// =============================================================================
//  GET /v1/leaderboard           — top-N composite ranking (HTTP read path).
//  GET /v1/submissions/:id/score — current per-submission score breakdown.
//
//  Both endpoints read straight from Redis structures the scoring-service
//  maintains. WebSocket clients still use leaderboard-ws for streaming
//  updates; this is the cold-start / SEO / cURL-friendly path.
// =============================================================================

#include <string>
#include <vector>

#include <drogon/HttpController.h>
#include <nlohmann/json.hpp>
#include <sw/redis++/redis++.h>

#include "api_gateway/clients.h"
#include "velocity/common/log.h"

namespace velocity::api_gateway::routes {

class LeaderboardRead : public drogon::HttpController<LeaderboardRead> {
public:
    METHOD_LIST_BEGIN
        METHOD_ADD(LeaderboardRead::top,         "/v1/leaderboard",                drogon::Get);
        METHOD_ADD(LeaderboardRead::detail,      "/v1/submissions/{id}/score",     drogon::Get);
        METHOD_ADD(LeaderboardRead::mismatches,  "/v1/submissions/{id}/mismatches", drogon::Get);
        METHOD_ADD(LeaderboardRead::buildLogs,   "/v1/submissions/{id}/build/logs", drogon::Get);
    METHOD_LIST_END

    auto top(const drogon::HttpRequestPtr& req,
             std::function<void(const drogon::HttpResponsePtr&)>&& callback) const -> void {
        auto* redis = clients::RedisClient::get();
        if (!redis) return send_503(callback);

        const auto n_str  = req->getOptionalParameter<std::string>("n").value_or("50");
        std::size_t n = 50;
        try { n = std::min<std::size_t>(500, std::stoul(n_str)); } catch (...) {}

        nlohmann::json out;
        out["entries"] = nlohmann::json::array();
        try {
            // Highest composite first (ZREVRANGE).
            std::vector<std::string> ids;
            redis->zrevrange("leaderboard:composite", 0, static_cast<long>(n - 1),
                             std::back_inserter(ids));
            std::size_t rank = 1;
            for (const auto& id : ids) {
                const auto h = "scores:" + id;
                std::unordered_map<std::string, std::string> entry;
                redis->hgetall(h, std::inserter(entry, entry.begin()));
                if (entry.empty()) continue;
                nlohmann::json row;
                row["rank"]             = rank++;
                row["submission_id"]    = id;
                row["display_name"]     = entry["display_name"];
                row["team_name"]        = entry["team_name"];
                row["benchmark_id"]     = entry["benchmark_id"];
                row["composite_score"]  = stod_or(entry["composite_score"], 0.0);
                row["throughput_score"] = stod_or(entry["throughput_score"], 0.0);
                row["latency_score"]    = stod_or(entry["latency_score"], 0.0);
                row["correctness_score"] = stod_or(entry["correctness_score"], 0.0);
                row["sustained_rps"]    = stou_or(entry["sustained_rps"], 0);
                row["p50_ns"]           = stou_or(entry["p50_ns"], 0);
                row["p99_ns"]           = stou_or(entry["p99_ns"], 0);
                row["updated_at_ns"]    = stou_or(entry["updated_at_ns"], 0);
                out["entries"].push_back(std::move(row));
            }
        } catch (const std::exception& e) {
            VLOG_WARN("leaderboard read failed: {}", e.what());
            auto r = drogon::HttpResponse::newHttpJsonResponse(
                R"({"entries":[],"error":"redis_failed"})");
            r->setStatusCode(drogon::k500InternalServerError);
            callback(r);
            return;
        }
        auto resp = drogon::HttpResponse::newHttpJsonResponse(out.dump());
        resp->addHeader("Cache-Control", "public, max-age=2");
        callback(resp);
    }

    auto detail(const drogon::HttpRequestPtr& /*req*/,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                std::string id) const -> void {
        auto* redis = clients::RedisClient::get();
        if (!redis) return send_503(callback);

        std::unordered_map<std::string, std::string> entry;
        try {
            redis->hgetall("scores:" + id,
                           std::inserter(entry, entry.begin()));
        } catch (const std::exception& e) {
            VLOG_WARN("score detail read failed: {}", e.what());
        }
        if (entry.empty()) {
            auto r = drogon::HttpResponse::newHttpJsonResponse(R"({"error":"not_found"})");
            r->setStatusCode(drogon::k404NotFound);
            callback(r);
            return;
        }

        nlohmann::json out;
        for (const auto& [k, v] : entry) out[k] = v;
        callback(drogon::HttpResponse::newHttpJsonResponse(out.dump()));
    }

    auto mismatches(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                    std::string id) const -> void {
        auto* redis = clients::RedisClient::get();
        if (!redis) return send_503(callback);

        // Pull both aggregate counts (from `scores:<id>`) and the sample
        // examples (from `mismatches:<id>` — a LIST the validator pushes to
        // as it observes violations). LIST entries are JSON-encoded.
        std::size_t n = 50;
        if (const auto p = req->getOptionalParameter<std::string>("n")) {
            try { n = std::min<std::size_t>(500, std::stoul(*p)); } catch (...) {}
        }

        nlohmann::json out;
        try {
            std::unordered_map<std::string, std::string> agg;
            redis->hgetall("scores:" + id, std::inserter(agg, agg.begin()));
            out["taxonomy"] = {
                {"priority_violations", stou_or(agg["priority_violations"], 0)},
                {"price_violations",    stou_or(agg["price_violations"],    0)},
                {"phantom_fills",       stou_or(agg["phantom_fills"],       0)},
                {"missing_fills",       stou_or(agg["missing_fills"],       0)},
                {"expected_fills",      stou_or(agg["expected_fills"],      0)},
                {"actual_fills",        stou_or(agg["actual_fills"],        0)},
                {"correct_fills",       stou_or(agg["correct_fills"],       0)},
            };

            std::vector<std::string> samples;
            redis->lrange("mismatches:" + id, 0, static_cast<long>(n - 1),
                          std::back_inserter(samples));
            auto& arr = out["samples"];
            arr = nlohmann::json::array();
            for (const auto& s : samples) {
                try { arr.push_back(nlohmann::json::parse(s)); }
                catch (...) { arr.push_back({{"raw", s}}); }
            }
        } catch (const std::exception& e) {
            VLOG_WARN("mismatches read failed: {}", e.what());
        }
        callback(drogon::HttpResponse::newHttpJsonResponse(out.dump()));
    }

    // GET /v1/submissions/:id/build/logs?since=<index>&n=<count>
    //
    // Returns a slice of the Redis LIST `build:<submission_id>` populated
    // by the submission-engine while Kaniko runs. Polling is cheap; we cap
    // each response at 500 lines.
    auto buildLogs(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                   std::string id) const -> void {
        auto* redis = clients::RedisClient::get();
        if (!redis) return send_503(callback);

        long since = 0;
        long n = 500;
        if (const auto p = req->getOptionalParameter<std::string>("since")) {
            try { since = std::max<long>(0, std::stol(*p)); } catch (...) {}
        }
        if (const auto p = req->getOptionalParameter<std::string>("n")) {
            try { n = std::min<long>(2000, std::max<long>(1, std::stol(*p))); } catch (...) {}
        }
        const long stop = since + n - 1;

        nlohmann::json out;
        out["from"] = since;
        out["lines"] = nlohmann::json::array();
        try {
            std::vector<std::string> lines;
            redis->lrange("build:" + id, since, stop, std::back_inserter(lines));
            for (auto& l : lines) out["lines"].push_back(std::move(l));
            const auto total = redis->llen("build:" + id);
            out["total"] = total;
        } catch (const std::exception& e) {
            VLOG_WARN("build logs read failed: {}", e.what());
        }
        auto resp = drogon::HttpResponse::newHttpJsonResponse(out.dump());
        resp->addHeader("Cache-Control", "no-cache");
        callback(resp);
    }

private:
    static auto send_503(const std::function<void(const drogon::HttpResponsePtr&)>& cb) -> void {
        auto r = drogon::HttpResponse::newHttpJsonResponse(
            R"({"entries":[],"error":"redis_unavailable"})");
        r->setStatusCode(drogon::k503ServiceUnavailable);
        cb(r);
    }
    [[nodiscard]] static auto stod_or(const std::string& s, double dflt) noexcept -> double {
        try { return s.empty() ? dflt : std::stod(s); } catch (...) { return dflt; }
    }
    [[nodiscard]] static auto stou_or(const std::string& s, std::uint64_t dflt) noexcept
        -> std::uint64_t {
        try { return s.empty() ? dflt : std::stoull(s); } catch (...) { return dflt; }
    }
};

}  // namespace velocity::api_gateway::routes
