// =============================================================================
//  api_gateway/audit.h
//
//  Thin C++ emitter for Velocity audit events. The audit pipeline is
//  owned by services/audit-log; the gateway just *publishes* events to
//  a Redis Stream (`audit:events`) and the consumer drains them. This
//  keeps the gateway's dependency surface tiny (no librdkafka here) and
//  centralises the schema/chain-hashing concerns in one Go process.
//
//  Usage in a route handler:
//
//      audit::emit({
//          .tenant_id    = ctx->id,
//          .subject      = ctx->subject,
//          .role         = std::string{tenant::role_to_string(ctx->role)},
//          .action       = "submission.create",
//          .resource_type= "submission",
//          .resource_id  = submission_id,
//          .outcome      = audit::Outcome::ALLOW,
//          .status_code  = 201,
//      });
//
//  Failure modes: best-effort. If Redis is unreachable we log a WARN
//  and drop the event — by design, audit emission must NEVER fail the
//  enclosing request.
// =============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace velocity::api_gateway::audit {

enum class Outcome : std::uint8_t {
    ALLOW = 0,
    DENY  = 1,
    ERROR = 2,
};

[[nodiscard]] auto outcome_to_string(Outcome o) noexcept -> std::string_view;

struct Event {
    std::string tenant_id;
    std::string subject;
    std::string role;
    std::string action;          // e.g. "submission.create"
    std::string resource_type;   // e.g. "submission"
    std::string resource_id;
    Outcome     outcome{Outcome::ALLOW};
    int         status_code{0};
    std::string remote_ip;
    std::string request_id;
};

// Fire-and-forget. Allocates a small JSON string and XADDs it.
auto emit(const Event& ev) -> void;

}  // namespace velocity::api_gateway::audit
