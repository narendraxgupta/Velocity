// =============================================================================
//  GET /healthz, /readyz — Kubernetes-style probes.
//
//  /healthz is "the process is alive". /readyz adds "I can talk to all of my
//  dependencies." We start with the cheap one; /readyz gets richer as the
//  upstream-client wiring lands in Phase 2.
// =============================================================================

#include <drogon/HttpController.h>
#include <nlohmann/json.hpp>

#include "velocity/common/time.h"

namespace velocity::api_gateway::routes {

class Health : public drogon::HttpController<Health> {
public:
    METHOD_LIST_BEGIN
        METHOD_ADD(Health::live,  "/healthz", drogon::Get);
        METHOD_ADD(Health::ready, "/readyz",  drogon::Get);
    METHOD_LIST_END

    auto live(const drogon::HttpRequestPtr& /*req*/,
              std::function<void(const drogon::HttpResponsePtr&)>&& callback) const -> void {
        nlohmann::json body{
            {"status",      "ok"},
            {"service",     "api-gateway"},
            {"ts_ns",       velocity::time::realtime_ns()},
        };
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body.dump());
        callback(resp);
    }

    auto ready(const drogon::HttpRequestPtr& /*req*/,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback) const -> void {
        // Phase 2: probe downstream gRPC channels and Redis here.
        nlohmann::json body{
            {"status", "ok"},
            {"checks", nlohmann::json::object()},
        };
        callback(drogon::HttpResponse::newHttpJsonResponse(body.dump()));
    }
};

}  // namespace velocity::api_gateway::routes
