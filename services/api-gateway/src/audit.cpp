// =============================================================================
//  audit.cpp — Redis-stream emitter for audit events.
//
//  Wire format on Redis:
//
//      XADD audit:events MAXLEN ~ 1000000 *
//          v 1
//          payload <single JSON string>
//
//  The audit-log consumer reads with `XREAD BLOCK` and decodes the
//  `payload` field. We use MAXLEN ~ to bound memory usage; capped
//  approximate streams are perfect for "drain me ASAP" workloads.
//
//  EventID generation: ULID (Crockford base32). We hand-roll instead of
//  pulling a library because ULID is 90 lines and we already have the
//  monotonic time helper.
// =============================================================================

#include "api_gateway/audit.h"

#include <array>
#include <atomic>
#include <chrono>
#include <random>
#include <string>

#include <nlohmann/json.hpp>
#include <sw/redis++/redis++.h>

#include "api_gateway/clients.h"
#include "velocity/common/log.h"

namespace velocity::api_gateway::audit {

namespace {

constexpr std::string_view STREAM_KEY = "audit:events";
constexpr std::size_t STREAM_MAXLEN = 1'000'000;

auto make_ulid() -> std::string {
    static std::atomic<std::uint64_t> last_ms{0};
    static thread_local std::mt19937_64 rng{std::random_device{}()};

    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    last_ms.store(static_cast<std::uint64_t>(now_ms), std::memory_order_relaxed);

    static constexpr char ALPHA[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";  // Crockford
    std::array<char, 26> out{};

    // 10 chars of timestamp (48 bits), 16 chars of randomness (80 bits)
    std::uint64_t t = static_cast<std::uint64_t>(now_ms);
    for (int i = 9; i >= 0; --i) {
        out[i] = ALPHA[t & 0x1F];
        t >>= 5;
    }
    std::uint64_t a = rng();
    std::uint64_t b = rng();
    for (int i = 25; i >= 18; --i) {
        out[i] = ALPHA[a & 0x1F];
        a >>= 5;
    }
    for (int i = 17; i >= 10; --i) {
        out[i] = ALPHA[b & 0x1F];
        b >>= 5;
    }
    return std::string{out.data(), out.size()};
}

}  // namespace

auto outcome_to_string(Outcome o) noexcept -> std::string_view {
    switch (o) {
        case Outcome::ALLOW: return "allow";
        case Outcome::DENY:  return "deny";
        case Outcome::ERROR: return "error";
    }
    return "unknown";
}

auto emit(const Event& ev) -> void {
    auto* redis = clients::RedisClient::get();
    if (redis == nullptr) {
        // Fall back to a structured log line so the event is at least
        // recoverable from container logs. Better than swallowing it
        // silently in a dev/test environment without Redis.
        VLOG_WARN("audit: redis unavailable; tenant={} action={} outcome={}",
                  ev.tenant_id, ev.action, outcome_to_string(ev.outcome));
        return;
    }

    nlohmann::json body{
        {"v",              1},
        {"event_id",       make_ulid()},
        {"tenant_id",      ev.tenant_id.empty() ? "unknown" : ev.tenant_id},
        {"subject",        ev.subject},
        {"role",           ev.role},
        {"source",         "api-gateway"},
        {"action",         ev.action},
        {"resource_type",  ev.resource_type},
        {"resource_id",    ev.resource_id},
        {"outcome",        outcome_to_string(ev.outcome)},
        {"status_code",    ev.status_code},
        {"occurred_at_ns",
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()},
        {"remote_ip",      ev.remote_ip},
        {"request_id",     ev.request_id},
    };
    const auto payload = body.dump();

    try {
        // MAXLEN ~ N is the cheap variant: trims approximately, which
        // keeps the XADD O(1) instead of O(N). XADD's reply is the new
        // entry id (a bulk-string of shape `<ms>-<seq>`), so the
        // generic command<> template must be instantiated with
        // std::string — `long long` would throw ProtoError at runtime.
        // We discard the id; the audit-log consumer derives ordering
        // from the stream itself.
        (void)redis->command<std::string>(
            "XADD", STREAM_KEY, "MAXLEN", "~", std::to_string(STREAM_MAXLEN),
            "*", "v", "1", "payload", payload);
    } catch (const std::exception& e) {
        VLOG_WARN("audit: XADD failed: {} (event dropped)", e.what());
    }
}

}  // namespace velocity::api_gateway::audit
