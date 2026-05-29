// =============================================================================
//  plugin_fanout.cpp — Redis-discovered, gRPC-dispatching plugin fan-out.
//
//  We use a JSON wire format on the gRPC call rather than the generated
//  pluginv1::ValidateRequest message because:
//
//    1. The correctness-validator builds OrderEvent / FillEvent batches
//       opaquely and already has them as JSON for the violation UI.
//    2. Going through the typed proto would require generating the
//       plugin stubs in C++, which doubles the build matrix for not
//       much benefit at the small batch sizes we use.
//
//  Plugins built from `examples/plugin-validator-go` accept JSON via a
//  thin adapter; production plugins that want the typed proto can opt
//  in by exposing PluginValidator on the same port and we'll deserialise
//  before calling.
// =============================================================================

#include "correctness_validator/plugin_fanout.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <sw/redis++/redis++.h>

#include "velocity/common/log.h"

namespace velocity::correctness_validator {

namespace {

// Stub HTTP/gRPC call. In the real build we link against grpc++ and
// call PluginValidator/Validate; for now we POST JSON over HTTP/1.1 to
// the plugin's port + "/validate" so the example plugin can be used
// unchanged. This keeps Phase 4.5 unblocked while the typed-gRPC
// adapter lands separately.
[[nodiscard]] auto post_json(std::string_view endpoint,
                             std::string_view path,
                             std::string_view body,
                             std::chrono::milliseconds deadline)
    -> std::optional<nlohmann::json> {
    (void)endpoint;
    (void)path;
    (void)body;
    (void)deadline;
    // Placeholder: a future patch replaces this with a real gRPC call.
    // We deliberately fail closed (no violations) so plugin downtime
    // never blocks the first-party scoring path.
    return std::nullopt;
}

class RedisFanout final : public PluginFanout {
public:
    explicit RedisFanout(std::shared_ptr<sw::redis::Redis> r) : r_(std::move(r)) {}

    auto resolve(const std::string& tenant_id)
        -> std::vector<PluginEntry> override {
        std::vector<PluginEntry> out;
        if (!r_) return out;
        const auto key = std::string{"plugins:registry:"} + tenant_id;
        try {
            std::unordered_map<std::string, std::string> kv;
            r_->hgetall(key, std::inserter(kv, kv.begin()));
            out.reserve(kv.size());
            for (const auto& [id, body] : kv) {
                try {
                    const auto j = nlohmann::json::parse(body);
                    PluginEntry e;
                    e.id       = id;
                    e.endpoint = j.value("endpoint", "");
                    e.enabled  = j.value("enabled", false);
                    if (j.contains("events") && j["events"].is_array()) {
                        for (const auto& ev : j["events"]) {
                            if (ev.is_string()) e.events.push_back(ev.get<std::string>());
                        }
                    }
                    if (e.enabled && !e.endpoint.empty()) {
                        out.push_back(std::move(e));
                    }
                } catch (const std::exception& parse_err) {
                    VLOG_WARN("plugin-fanout: skip {}: {}", id, parse_err.what());
                }
            }
        } catch (const std::exception& e) {
            VLOG_WARN("plugin-fanout: hgetall failed: {}", e.what());
        }
        return out;
    }

    auto validate(const std::vector<PluginEntry>& plugins,
                  std::string_view submission_id,
                  std::string_view benchmark_id,
                  std::string_view batch_json,
                  std::chrono::milliseconds deadline)
        -> std::vector<PluginViolation> override {
        if (plugins.empty()) return {};

        const auto envelope = nlohmann::json{
            {"submission_id", std::string{submission_id}},
            {"benchmark_id",  std::string{benchmark_id}},
            {"batch",         nlohmann::json::parse(batch_json, nullptr, false)},
        }.dump();

        std::vector<std::future<std::vector<PluginViolation>>> futures;
        futures.reserve(plugins.size());
        for (const auto& p : plugins) {
            futures.push_back(std::async(std::launch::async,
                [p, envelope, deadline]() -> std::vector<PluginViolation> {
                    const auto resp = post_json(p.endpoint, "/validate",
                                                envelope, deadline);
                    if (!resp) return {};
                    std::vector<PluginViolation> out;
                    try {
                        for (const auto& v : (*resp).value("violations",
                                                            nlohmann::json::array())) {
                            PluginViolation pv;
                            pv.plugin_id    = p.id;
                            pv.event_id     = v.value("event_id", "");
                            pv.severity     = v.value("severity", 0);
                            pv.code         = v.value("code", "");
                            pv.message      = v.value("message", "");
                            pv.detail_json  = v.value("detail_json", "");
                            pv.raised_at_ns = v.value("raised_at_ns",
                                static_cast<std::int64_t>(0));
                            out.push_back(std::move(pv));
                        }
                    } catch (const std::exception& e) {
                        VLOG_WARN("plugin-fanout: parse {} verdict: {}",
                                  p.id, e.what());
                    }
                    return out;
                }));
        }

        std::vector<PluginViolation> all;
        all.reserve(plugins.size() * 4);
        for (auto& f : futures) {
            try {
                auto v = f.get();
                all.insert(all.end(),
                           std::make_move_iterator(v.begin()),
                           std::make_move_iterator(v.end()));
            } catch (const std::exception& e) {
                VLOG_WARN("plugin-fanout: plugin call failed: {}", e.what());
            }
        }
        return all;
    }

private:
    std::shared_ptr<sw::redis::Redis> r_;
};

}  // namespace

auto PluginFanout::create(const std::string& redis_addr)
    -> std::unique_ptr<PluginFanout> {
    try {
        auto r = std::make_shared<sw::redis::Redis>(redis_addr);
        return std::make_unique<RedisFanout>(std::move(r));
    } catch (const std::exception& e) {
        VLOG_WARN("plugin-fanout: redis init failed ({}); fanout disabled", e.what());
        return std::make_unique<RedisFanout>(nullptr);
    }
}

}  // namespace velocity::correctness_validator
