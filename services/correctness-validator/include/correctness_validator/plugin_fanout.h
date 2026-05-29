// =============================================================================
//  correctness_validator/plugin_fanout.h
//
//  Resolves the active plugin list for a (tenant, submission) tuple
//  via Redis (plugins:registry:<tenant>) and exposes a gRPC client
//  that fans out a batch of events to every enabled plugin.
//
//  Deliberately small:
//
//      auto pfo = PluginFanout::create(redis_addr);
//      auto plugins = pfo->resolve("acme");
//      for (const auto& batch : event_batches) {
//          auto verdicts = pfo->validate(plugins, batch);
//          ...
//      }
//
//  Failure semantics
//  -----------------
//    * Redis unreachable      → empty plugin list; first-party
//                               validators still run.
//    * Plugin gRPC unreachable → that plugin's verdicts are skipped;
//                                others still run. A counter is bumped.
//    * Plugin returns error    → same as above.
//    * Plugin returns >max_violations → truncated and logged.
//
//  Per-tenant fan-out: we never call a plugin registered for tenant A
//  on a submission owned by tenant B. The fanout key is tenant-scoped
//  in Redis; cross-tenant calls would be a security violation.
// =============================================================================

#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace velocity::correctness_validator {

struct PluginEntry {
    std::string id;           // "acme/stp-strict/1.2.0"
    std::string endpoint;     // "host:port"
    bool        enabled{true};
    std::vector<std::string> events;  // "EVENT_KIND_FILL", ...
};

struct PluginViolation {
    std::string plugin_id;
    std::string event_id;
    int         severity{0};
    std::string code;
    std::string message;
    std::string detail_json;
    std::int64_t raised_at_ns{0};
};

class PluginFanout {
public:
    static auto create(const std::string& redis_addr) -> std::unique_ptr<PluginFanout>;
    virtual ~PluginFanout() = default;

    // Resolve the active plugin list for a tenant. Empty when Redis
    // is unreachable or the key is missing — the caller treats that
    // as "no plugins registered" and proceeds with first-party checks
    // only.
    [[nodiscard]] virtual auto resolve(const std::string& tenant_id)
        -> std::vector<PluginEntry> = 0;

    // Send a JSON-serialised event batch to every plugin in `plugins`
    // and merge their violation lists. The deadline applies per plugin
    // call; slow plugins don't starve the rest.
    [[nodiscard]] virtual auto validate(const std::vector<PluginEntry>& plugins,
                                        std::string_view submission_id,
                                        std::string_view benchmark_id,
                                        std::string_view batch_json,
                                        std::chrono::milliseconds deadline)
        -> std::vector<PluginViolation> = 0;
};

}  // namespace velocity::correctness_validator
