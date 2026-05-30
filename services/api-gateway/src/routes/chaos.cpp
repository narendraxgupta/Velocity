// =============================================================================
//  /v1/chaos/* — pass-through to the chaos-orchestrator HTTP control-plane.
//
//  The gateway does NOT enforce auth on these endpoints today (RBAC lands
//  in Phase 4.2 of the roadmap). The chaos-orchestrator's RBAC binding
//  is the hard guarantee — it can only delete / mutate pods in the
//  CHAOS_NAMESPACES list, which by default is velocity-sandbox + velocity-load.
//
//  We proxy rather than re-implement so the chaos surface stays in one place
//  (the orchestrator), and so the gateway never needs the K8s client SDK
//  in its image.
// =============================================================================
#include <chrono>
#include <memory>
#include <string>

#include <drogon/HttpClient.h>
#include <drogon/HttpController.h>

#include "velocity/common/log.h"

namespace velocity::api_gateway::routes {

namespace {

// Single shared HTTP client per gateway process, pointed at the
// chaos-orchestrator service. The endpoint is configurable so the same
// binary works in docker-compose ("http://chaos-orchestrator:8090") and
// in Kubernetes ("http://chaos-orchestrator.velocity-control:8090").
auto chaos_client() -> std::shared_ptr<drogon::HttpClient> {
    static auto inst = []() {
        const auto* env = std::getenv("VELOCITY_CHAOS_URL");
        const std::string url = (env && *env)
            ? env
            : "http://chaos-orchestrator.velocity-control.svc.cluster.local:8090";
        return drogon::HttpClient::newHttpClient(url);
    }();
    return inst;
}

// Forward `req` to the chaos-orchestrator, preserving method + body. Returns
// whatever the upstream returns; on connection error we synthesise a 503.
auto forward(const drogon::HttpRequestPtr& req,
             const std::string& upstream_path,
             std::function<void(const drogon::HttpResponsePtr&)>&& cb) -> void {
    auto out = drogon::HttpRequest::newHttpRequest();
    out->setPath(upstream_path);
    out->setMethod(req->getMethod());
    if (!req->getBody().empty()) {
        out->setBody(std::string(req->getBody()));
        out->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    }
    chaos_client()->sendRequest(out,
        [cb = std::move(cb)](drogon::ReqResult r, const drogon::HttpResponsePtr& resp) {
            if (r != drogon::ReqResult::Ok || !resp) {
                auto err = [](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(
                    R"({"error":"chaos-orchestrator unreachable"})");
                err->setStatusCode(drogon::k503ServiceUnavailable);
                cb(err);
                return;
            }
            cb(resp);
        },
        /*timeout=*/15.0);
}

}  // namespace

class Chaos : public drogon::HttpController<Chaos> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(Chaos::status,      "/v1/chaos/status",         drogon::Get);
        ADD_METHOD_TO(Chaos::podKill,     "/v1/chaos/pod-kill",       drogon::Post);
        ADD_METHOD_TO(Chaos::tcLatency,   "/v1/chaos/tc-latency",     drogon::Post);
        ADD_METHOD_TO(Chaos::tcLoss,      "/v1/chaos/tc-loss",        drogon::Post);
        ADD_METHOD_TO(Chaos::cpuThrottle, "/v1/chaos/cpu-throttle",   drogon::Post);
        ADD_METHOD_TO(Chaos::partition,   "/v1/chaos/partition",      drogon::Post);
    METHOD_LIST_END

    auto status     (const drogon::HttpRequestPtr& r,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) const -> void {
        forward(r, "/v1/chaos/status", std::move(cb));
    }
    auto podKill    (const drogon::HttpRequestPtr& r,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) const -> void {
        forward(r, "/v1/chaos/pod-kill", std::move(cb));
    }
    auto tcLatency  (const drogon::HttpRequestPtr& r,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) const -> void {
        forward(r, "/v1/chaos/tc-latency", std::move(cb));
    }
    auto tcLoss     (const drogon::HttpRequestPtr& r,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) const -> void {
        forward(r, "/v1/chaos/tc-loss", std::move(cb));
    }
    auto cpuThrottle(const drogon::HttpRequestPtr& r,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) const -> void {
        forward(r, "/v1/chaos/cpu-throttle", std::move(cb));
    }
    auto partition  (const drogon::HttpRequestPtr& r,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) const -> void {
        forward(r, "/v1/chaos/partition", std::move(cb));
    }
};

}  // namespace velocity::api_gateway::routes
