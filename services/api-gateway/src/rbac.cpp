// =============================================================================
//  rbac.cpp — Route ↔ role table + decision engine.
//
//  Design notes
//  ------------
//  The map is a static `std::vector<RoutePolicy>` evaluated by linear
//  scan with longest-prefix-wins. We benchmarked map / unordered_map /
//  trie alternatives and at ~35 entries the linear scan is the fastest
//  AND the easiest to audit. The vector is sorted by descending path
//  length at startup so the first match is the most specific one.
//
//  Fail-closed policy: anything not in the table requires ADMIN. This
//  makes the gateway scream loudly the moment a new route is added
//  without an explicit policy entry — much better than silently
//  inheriting submitter-level permissions.
// =============================================================================

#include "api_gateway/rbac.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "velocity/common/log.h"

namespace velocity::api_gateway::rbac {

namespace {

struct RoutePolicy {
    std::string    method;          // empty == any
    std::string    path_prefix;
    tenant::Role   min_role;
};

// Single source of truth. Edit here when adding a route — do NOT
// scatter rbac checks across handlers. Order doesn't matter at
// edit-time; we sort at startup.
const std::vector<RoutePolicy>& policies() {
    static const std::vector<RoutePolicy> v = [] {
        std::vector<RoutePolicy> p = {
            // ----- unauthenticated infrastructure probes -----
            // Kubernetes liveness/readiness and Prometheus scrapes carry
            // no JWT. They resolve to the synthetic "default" SUBMITTER
            // context via tenant.cpp's bypass list, so a SUBMITTER minimum
            // lets them through while everything else stays fail-closed.
            // Without an explicit row here required_role() falls back to
            // ADMIN and the gateway 403s its own probes — never becoming
            // Ready and CrashLooping under the kubelet.
            {"GET",  "/healthz",                 tenant::Role::SUBMITTER},
            {"GET",  "/readyz",                  tenant::Role::SUBMITTER},
            {"GET",  "/metrics",                 tenant::Role::SUBMITTER},

            // ----- public-ish reads (still need a tenant) -----
            {"GET",  "/v1/auth/me",              tenant::Role::SUBMITTER},
            {"GET",  "/v1/leaderboard",          tenant::Role::SUBMITTER},
            {"GET",  "/v1/leaderboard/health",   tenant::Role::SUBMITTER},
            {"GET",  "/v1/marketdata",           tenant::Role::SUBMITTER},

            // ----- submitter scope -----
            {"GET",  "/v1/submissions",          tenant::Role::SUBMITTER},
            {"POST", "/v1/submissions",          tenant::Role::SUBMITTER},
            {"GET",  "/v1/benchmarks",           tenant::Role::SUBMITTER},
            {"POST", "/v1/benchmarks",           tenant::Role::SUBMITTER},
            {"GET",  "/v1/critiques",            tenant::Role::SUBMITTER},
            {"POST", "/v1/critiques",            tenant::Role::SUBMITTER},
            {"GET",  "/v1/orderbook",            tenant::Role::SUBMITTER},
            {"GET",  "/v1/adaptive-profile",     tenant::Role::SUBMITTER},
            {"POST", "/v1/adaptive-profile",     tenant::Role::SUBMITTER},
            {"POST", "/v1/share",                tenant::Role::SUBMITTER},
            {"DELETE", "/v1/share",              tenant::Role::SUBMITTER},
            // GET /v1/share/{token} runs as the synthetic SUBMITTER
            // context produced by tenant.cpp's bypass path. We still
            // need an explicit row here — required_role() falls back
            // to ADMIN if nothing matches, which would 403 every
            // public-share viewer.
            {"GET",  "/v1/share",                tenant::Role::SUBMITTER},

            // ----- operator scope -----
            // Chaos injection touches the kernel and can affect other
            // tenants on the same node. Submitter must NOT trigger.
            {"POST", "/v1/chaos",                tenant::Role::OPERATOR},
            {"DELETE", "/v1/chaos",              tenant::Role::OPERATOR},
            // Pcap recording is a privacy-sensitive operation.
            {"POST", "/v1/pcaps/record",         tenant::Role::OPERATOR},
            // Fleet introspection across all tenants.
            {"GET",  "/v1/fleet",                tenant::Role::OPERATOR},

            // ----- admin scope -----
            {"",     "/v1/admin",                tenant::Role::ADMIN},
            {"",     "/v1/audit",                tenant::Role::ADMIN},
            {"",     "/v1/tenants",              tenant::Role::ADMIN},
            {"",     "/v1/plugins",              tenant::Role::ADMIN},
        };
        std::sort(p.begin(), p.end(), [](const auto& a, const auto& b) {
            return a.path_prefix.size() > b.path_prefix.size();
        });
        return p;
    }();
    return v;
}

}  // namespace

auto required_role(std::string_view method, std::string_view path) noexcept
    -> tenant::Role {
    for (const auto& p : policies()) {
        if (!path.starts_with(p.path_prefix)) continue;
        if (!p.method.empty() && p.method != method) continue;
        return p.min_role;
    }
    return tenant::Role::ADMIN;  // fail closed
}

auto check(const tenant::Context* ctx, std::string_view method,
           std::string_view path) noexcept -> Decision {
    const auto need = required_role(method, path);
    if (ctx == nullptr) return Decision::DENY_UNAUTHENTICATED;
    if (static_cast<std::uint8_t>(ctx->role) <
        static_cast<std::uint8_t>(need)) {
        return Decision::DENY_FORBIDDEN;
    }
    return Decision::ALLOW;
}

auto deny_response(Decision d) -> drogon::HttpResponsePtr {
    nlohmann::json body;
    drogon::HttpStatusCode code = drogon::k200OK;
    switch (d) {
        case Decision::DENY_UNAUTHENTICATED:
            body = {{"error", "authentication required"}};
            code = drogon::k401Unauthorized;
            break;
        case Decision::DENY_FORBIDDEN:
            body = {{"error", "insufficient role"}};
            code = drogon::k403Forbidden;
            break;
        default:
            return nullptr;
    }
    auto resp = [](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(body.dump());
    resp->setStatusCode(code);
    if (d == Decision::DENY_UNAUTHENTICATED) {
        resp->addHeader("WWW-Authenticate", "Bearer");
    }
    return resp;
}

auto context_from(const drogon::HttpRequestPtr& req)
    -> std::shared_ptr<tenant::Context> {
    auto attrs = req->attributes();
    if (!attrs || !attrs->find("velocity.tenant_id")) return nullptr;
    auto ctx = std::make_shared<tenant::Context>();
    ctx->id      = attrs->get<std::string>("velocity.tenant_id");
    ctx->subject = attrs->find("velocity.tenant_subject")
                     ? attrs->get<std::string>("velocity.tenant_subject")
                     : std::string{};
    ctx->role    = attrs->find("velocity.tenant_role")
                     ? tenant::role_from_string(
                           attrs->get<std::string>("velocity.tenant_role"))
                     : tenant::Role::SUBMITTER;
    return ctx;
}

}  // namespace velocity::api_gateway::rbac
