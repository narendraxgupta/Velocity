// =============================================================================
//  api_gateway/tenant.h
//
//  TenantContext + per-request tenant resolution.
//
//  Every request the gateway handles must end up with a `tenant_id`. We
//  derive it from one of three sources, in priority order:
//
//    1. A signed JWT `Authorization: Bearer <token>` whose `tid` claim
//       is a non-empty string. This is the production path.
//    2. The legacy `X-Velocity-Tenant` header — a temporary affordance
//       so existing clients can opt in without rolling new credentials.
//    3. The fallback `"default"` tenant — only allowed when the gateway
//       is configured with VELOCITY_AUTH_REQUIRE=false. Anything else
//       is a hard 401.
//
//  We deliberately keep the verification minimal in this header — full
//  JWKS rotation and audience-checking lives in `tenant.cpp`. The point
//  of the header is to give routes a clean way to ask "who is this?":
//
//      auto t = tenant::current(req);
//      if (!t) return cb(json_error(k401Unauthorized, "auth required"));
//      const auto key = tenant::scoped_key(t->id, "scores:" + submission_id);
//
//  All tenant-scoped Redis keys, Kafka topics, and MinIO prefixes route
//  through `scoped_key`, `scoped_topic`, and `scoped_object`.
// =============================================================================

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include <drogon/HttpRequest.h>

namespace velocity::api_gateway::tenant {

// Set of normalised roles. Strings are stable on the wire and over-the-
// air; the enum stays internal.
enum class Role : std::uint8_t {
    SUBMITTER = 0,
    OPERATOR  = 1,
    ADMIN     = 2,
};

[[nodiscard]] auto role_from_string(std::string_view s) noexcept -> Role;
[[nodiscard]] auto role_to_string(Role r) noexcept -> std::string_view;

// TenantContext is what routes consume. It's POD-like; copy freely.
struct Context {
    std::string                              id;           // tenant id ("acme")
    std::string                              subject;      // JWT sub claim, if any
    Role                                     role{Role::SUBMITTER};
    std::chrono::system_clock::time_point    issued_at{};
    std::chrono::system_clock::time_point    expires_at{};
};

// Configure verification keys / required flags. Safe to call at startup
// only; not thread-safe to reconfigure mid-flight.
struct Config {
    // Base64-encoded HS256 key, or empty to disable JWT verification (
    // when X-Velocity-Tenant is the only auth path).
    std::string hs256_secret_b64;
    // When true the gateway returns 401 for any request without a
    // resolvable tenant. When false the "default" tenant is granted.
    bool require_auth{false};
    // Comma-separated list of routes that bypass auth (e.g. /healthz).
    // GET /v1/share/{token} is also bypassed — the URL token IS the
    // authn for that path; see routes/share.cpp.
    std::string bypass_paths{"/healthz,/readyz,/metrics,/v1/leaderboard,/v1/leaderboard/health,/v1/share/"};
};

auto configure(Config cfg) -> void;
auto configuration() noexcept -> const Config&;

// Resolve the tenant for a request. Returns std::nullopt when no tenant
// could be determined AND auth is required; "default" otherwise.
[[nodiscard]] auto resolve(const drogon::HttpRequestPtr& req) -> std::optional<Context>;

// Build a tenant-scoped key from a raw key. Idempotent: if the input
// already begins with the tenant prefix the function returns it
// unchanged.
[[nodiscard]] auto scoped_key(std::string_view tenant_id,
                              std::string_view raw_key) -> std::string;

// Same as scoped_key but for Kafka/Redpanda topic names. Topic names
// have stricter characters (no ':'), so we use a '.' separator.
[[nodiscard]] auto scoped_topic(std::string_view tenant_id,
                                std::string_view raw_topic) -> std::string;

// Same for MinIO object keys / prefixes — '/' separator.
[[nodiscard]] auto scoped_object(std::string_view tenant_id,
                                 std::string_view raw_object) -> std::string;

}  // namespace velocity::api_gateway::tenant
