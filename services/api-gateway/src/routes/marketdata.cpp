// =============================================================================
//  /v1/marketdata/* — stochastic reference market data API.
//
//  The marketdata-generator service publishes a fixed-point mid price for
//  every configured symbol under the Redis key:
//
//      marketdata:<symbol>:mid_units    (TTL 60s)
//
//  Two endpoints expose that to the world:
//
//    GET /v1/marketdata/snapshot
//      Read every key matching `marketdata:*:mid_units` and return:
//        { "ts_ms": 1718000000123,
//          "symbols": { "SPOT/USDT": { "mid_units": 100123456, "price_scale": 1000000 }, ... } }
//      The snapshot is a point-in-time view; consumers wanting a live
//      stream use the SSE endpoint below.
//
//    GET /v1/marketdata/stream   (text/event-stream)
//      Long-poll loop that emits a JSON snapshot every 100ms. The frontend
//      uses this for the live mid-price ticker. Heartbeats every 10s keep
//      the connection alive through corporate proxies.
//
//  We intentionally don't proxy to the generator's /snapshot HTTP endpoint
//  — Redis is one network hop closer to every gateway pod and the data
//  has the same TTL semantics either way.
// =============================================================================
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
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

// Default fixed-point scale matches the generator's PriceScale.
// Falls back to 1_000_000 when a symbol publishes without a sibling
// price_scale key — generator always sets it but we keep the fallback
// to avoid hard-failing on legacy publishers.
constexpr std::int64_t kDefaultScale = 1'000'000;

// Build the snapshot JSON by reading every `marketdata:*:mid_units` key.
// SCAN > KEYS for production (KEYS is O(N) blocking); we use SCAN with a
// 100-key batch which is bounded.
[[nodiscard]] auto build_snapshot(sw::redis::Redis& redis) -> nlohmann::json {
    nlohmann::json out;
    out["symbols"] = nlohmann::json::object();
    out["ts_ms"]   = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    long long cursor = 0;
    std::vector<std::string> keys;
    do {
        cursor = redis.scan(cursor, "marketdata:*:mid_units", 100,
                            std::back_inserter(keys));
    } while (cursor != 0);

    for (const auto& key : keys) {
        if (auto v = redis.get(key)) {
            std::string symbol;
            constexpr std::size_t prefix_len = sizeof("marketdata:") - 1;
            constexpr std::size_t suffix_len = sizeof(":mid_units")  - 1;
            if (key.size() > prefix_len + suffix_len) {
                symbol = key.substr(prefix_len,
                                    key.size() - prefix_len - suffix_len);
            }
            if (symbol.empty()) continue;
            try {
                const auto mid_units = std::stoll(*v);
                nlohmann::json entry;
                entry["mid_units"]   = mid_units;
                entry["price_scale"] = kDefaultScale;
                entry["mid"]         = static_cast<double>(mid_units) /
                                       static_cast<double>(kDefaultScale);
                out["symbols"][symbol] = std::move(entry);
            } catch (...) { /* skip malformed value */ }
        }
    }
    return out;
}

}  // namespace

class Marketdata : public drogon::HttpController<Marketdata> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(Marketdata::snapshot, "/v1/marketdata/snapshot", drogon::Get);
        ADD_METHOD_TO(Marketdata::stream,   "/v1/marketdata/stream",   drogon::Get);
    METHOD_LIST_END

    auto snapshot(const drogon::HttpRequestPtr&,
                  std::function<void(const drogon::HttpResponsePtr&)>&& cb) const
        -> void {
        auto* redis = clients::RedisClient::get();
        if (!redis) {
            cb(json_error(drogon::k503ServiceUnavailable, "redis unavailable"));
            return;
        }
        try {
            auto body = build_snapshot(*redis);
            cb([](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(body.dump()));
        } catch (const std::exception& e) {
            cb(json_error(drogon::k502BadGateway, e.what()));
        }
    }

    // SSE stream: emit one snapshot per 100ms tick + heartbeats every 10s.
    // Implementation note: Drogon's async response writer keeps the loop
    // off the worker threads; we hold a weak ref to the response stream
    // so a disconnected client tears the loop down promptly.
    auto stream(const drogon::HttpRequestPtr&,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb) const
        -> void {
        auto* redis = clients::RedisClient::get();
        if (!redis) {
            cb(json_error(drogon::k503ServiceUnavailable, "redis unavailable"));
            return;
        }
        auto resp = drogon::HttpResponse::newAsyncStreamResponse(
            [](drogon::ResponseStreamPtr stream) {
                auto* redis = clients::RedisClient::get();
                if (!redis) {
                    stream->close();
                    return;
                }
                // We pump on a Drogon loop timer so we don't block any
                // worker thread on Redis I/O for the snapshot window.
                auto running = std::make_shared<std::atomic_bool>(true);
                auto last_hb = std::make_shared<std::chrono::steady_clock::time_point>(
                    std::chrono::steady_clock::now());

                // ResponseStreamPtr is a std::unique_ptr, which can't be
                // captured by copy into the runEvery() std::function. Promote
                // it to a shared_ptr so the repeating timer callback can own it.
                auto stream_sp =
                    std::shared_ptr<drogon::ResponseStream>(std::move(stream));

                auto tick = [stream_sp, running, last_hb]() {
                    if (!*running) return;
                    auto* r = clients::RedisClient::get();
                    if (!r) { stream_sp->close(); *running = false; return; }
                    try {
                        const auto body = build_snapshot(*r).dump();
                        const auto msg = "event: snapshot\ndata: " + body + "\n\n";
                        if (!stream_sp->send(msg)) {
                            *running = false;
                            stream_sp->close();
                            return;
                        }
                    } catch (...) {
                        // Single tick failures are non-fatal; the client
                        // will pick up the next one.
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (now - *last_hb > std::chrono::seconds(10)) {
                        stream_sp->send(": heartbeat\n\n");
                        *last_hb = now;
                    }
                };
                drogon::app().getLoop()->runEvery(0.10, tick);
            });
        resp->setContentTypeCodeAndCustomString(drogon::CT_CUSTOM,
                                                "text/event-stream");
        resp->addHeader("Cache-Control", "no-cache");
        resp->addHeader("Connection",    "keep-alive");
        resp->addHeader("X-Accel-Buffering", "no");
        cb(resp);
    }
};

}  // namespace velocity::api_gateway::routes
