// =============================================================================
//  GET /v1/auth/me — return the resolved tenant + role of the caller.
//
//  This is the bridge between the gateway's RBAC table and the
//  frontend. The frontend uses the response to:
//    - Show / hide admin-only controls (e.g. chaos buttons).
//    - Display the active tenant in the header chrome.
//    - Decide whether to prompt for re-auth when a 401 lands.
//
//  We deliberately return a single role string AND the (sorted)
//  capabilities array. The capabilities array lets the frontend feature-
//  gate without keeping its own role→capability map in sync with the
//  backend. Today the capabilities map is hard-coded; once Phase 4.5
//  lands the plugin system the map becomes dynamic and the frontend
//  picks up new features automatically.
// =============================================================================

#include <array>
#include <string_view>

#include <drogon/HttpController.h>
#include <nlohmann/json.hpp>

#include "api_gateway/rbac.h"
#include "api_gateway/tenant.h"

namespace velocity::api_gateway::routes {

class Auth : public drogon::HttpController<Auth> {
public:
    METHOD_LIST_BEGIN
        METHOD_ADD(Auth::me, "/v1/auth/me", drogon::Get);
    METHOD_LIST_END

    auto me(const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback) const
        -> void {
        auto ctx = rbac::context_from(req);
        if (!ctx) {
            // Defensive: the RBAC advice should have already returned
            // 401 here, but if /v1/auth/me ever winds up on a bypass
            // list this keeps the contract honest.
            nlohmann::json body{{"error", "unauthenticated"}};
            auto resp = drogon::HttpResponse::newHttpJsonResponse(body.dump());
            resp->setStatusCode(drogon::k401Unauthorized);
            callback(resp);
            return;
        }

        nlohmann::json caps = nlohmann::json::array();
        for (const auto& [role, cap_list] : capabilities()) {
            if (static_cast<std::uint8_t>(ctx->role) >=
                static_cast<std::uint8_t>(role)) {
                // Cap is std::string_view; nlohmann::json gained an
                // implicit string_view ctor only at 3.10. The
                // monorepo's pinned version isn't guaranteed to be
                // that new, so we materialise an owning std::string
                // for the push_back. The copy is one alloc per cap
                // per /v1/auth/me, which is fine.
                for (const auto& c : cap_list) caps.push_back(std::string{c});
            }
        }

        nlohmann::json body{
            {"tenant_id",     ctx->id},
            {"subject",       ctx->subject},
            {"role",          tenant::role_to_string(ctx->role)},
            {"capabilities",  caps},
        };
        callback(drogon::HttpResponse::newHttpJsonResponse(body.dump()));
    }

private:
    using Cap = std::string_view;
    using CapBundle = std::vector<Cap>;

    // ROLE → newly-granted capabilities (cumulative). Higher roles
    // inherit everything from below. Kept close to the RBAC table in
    // rbac.cpp; if you add a route there, add the capability here.
    static const std::vector<std::pair<tenant::Role, CapBundle>>& capabilities() {
        static const std::vector<std::pair<tenant::Role, CapBundle>> v = {
            {tenant::Role::SUBMITTER, {
                "submissions:read", "submissions:write",
                "benchmarks:read",  "benchmarks:write",
                "leaderboard:read", "critiques:read", "critiques:write",
                "orderbook:read",   "marketdata:read",
                "adaptive:read",    "adaptive:write",
            }},
            {tenant::Role::OPERATOR, {
                "chaos:inject",     "pcaps:record",
                "fleet:read",       "tenants:read",
            }},
            {tenant::Role::ADMIN, {
                "tenants:write",    "audit:read",
                "plugins:write",
            }},
        };
        return v;
    }
};

}  // namespace velocity::api_gateway::routes
