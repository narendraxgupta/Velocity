// =============================================================================
//  api_gateway/rbac.h
//
//  Role-based access control for the gateway.
//
//  Three roles, listed in order of escalating privilege:
//
//      SUBMITTER  — competition participant. Can manage their own
//                   submissions, run benchmarks against their own code,
//                   read public leaderboards.
//      OPERATOR   — competition admin. Everything a submitter can do,
//                   plus chaos injection, pcap recording, adaptive
//                   profile triggers, viewing all tenants' data.
//      ADMIN      — platform owner. Everything, plus tenant
//                   provisioning, audit log access, plugin registration.
//
//  We hard-code the route → required role map at startup so every
//  request gets an O(log n) lookup (sorted prefix table) and there is
//  ONE source of truth. Anything missing from the map is treated as
//  ADMIN-only — fail closed.
//
//  Wire format note: the role string the JWT carries is one of
//  "submitter" | "operator" | "admin" (matches Role values lowercased).
// =============================================================================

#pragma once

#include <string_view>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include "api_gateway/tenant.h"

namespace velocity::api_gateway::rbac {

enum class Decision : std::uint8_t {
    ALLOW              = 0,
    DENY_UNAUTHENTICATED = 1,   // → 401
    DENY_FORBIDDEN       = 2,   // → 403
};

// Look up the minimum role required to access a given path/method.
// Returns SUBMITTER as the default "logged-in is enough" — admin-only
// paths must be explicitly registered.
[[nodiscard]] auto required_role(std::string_view method,
                                 std::string_view path) noexcept -> tenant::Role;

// Decide whether the given context may access a path/method. Uses
// `required_role` internally; the split lets routes call check()
// without first synthesising a TenantContext.
[[nodiscard]] auto check(const tenant::Context* ctx,
                         std::string_view method,
                         std::string_view path) noexcept -> Decision;

// Drogon-friendly: assembles a JSON error response for a denied
// decision. ALLOW returns nullptr.
[[nodiscard]] auto deny_response(Decision d) -> drogon::HttpResponsePtr;

// Returns the TenantContext we stashed on the request in server.cpp's
// sync advice. nullptr when unauthenticated.
[[nodiscard]] auto context_from(const drogon::HttpRequestPtr& req)
    -> std::shared_ptr<tenant::Context>;

}  // namespace velocity::api_gateway::rbac
