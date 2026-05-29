// =============================================================================
//  GET  /v1/plugins/{tenant}
//  POST /v1/plugins/{tenant}/reconcile
//
//  Thin proxy to plugin-orchestrator. RBAC: admin-only at the rbac.cpp
//  table. The reconcile POST accepts a manifest body (text/yaml or
//  application/yaml) and forwards it as-is.
// =============================================================================

#include <cstdlib>
#include <string>

#include <drogon/HttpClient.h>
#include <drogon/HttpController.h>
#include <nlohmann/json.hpp>

namespace velocity::api_gateway::routes {

namespace {

auto upstream() -> std::string {
    if (const auto* env = std::getenv("PLUGIN_ORCHESTRATOR_URL"); env && *env) {
        return env;
    }
    return "http://plugin-orchestrator.velocity-control.svc.cluster.local:8082";
}

}  // namespace

class Plugins : public drogon::HttpController<Plugins> {
public:
    METHOD_LIST_BEGIN
        METHOD_ADD(Plugins::list,      "/v1/plugins/{tenant}",           drogon::Get);
        METHOD_ADD(Plugins::reconcile, "/v1/plugins/{tenant}/reconcile", drogon::Post);
    METHOD_LIST_END

    auto list(const drogon::HttpRequestPtr& /*req*/,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb,
              const std::string& tenant) const -> void {
        auto client = drogon::HttpClient::newHttpClient(upstream());
        auto upstream_req = drogon::HttpRequest::newHttpRequest();
        upstream_req->setMethod(drogon::Get);
        upstream_req->setPath("/v1/plugins/" + tenant);
        // Capture `client` by value into the lambda so the shared_ptr
        // outlives this stack frame. Drogon's HttpClient owns the
        // connection pool used by sendRequest; if it's destroyed
        // before the callback runs, the in-flight request is torn down
        // and the callback fires with ReqResult::BadResponse.
        client->sendRequest(upstream_req,
            [cb, client](drogon::ReqResult r,
                         const drogon::HttpResponsePtr& resp) {
                forward(r, resp, cb);
            }, 5.0);
    }

    auto reconcile(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                   const std::string& tenant) const -> void {
        auto client = drogon::HttpClient::newHttpClient(upstream());
        auto upstream_req = drogon::HttpRequest::newHttpRequest();
        upstream_req->setMethod(drogon::Post);
        upstream_req->setPath("/v1/plugins/" + tenant + "/reconcile");
        // Forward the upstream's content type so the orchestrator can
        // pick application/yaml vs text/yaml. Drogon strips this on
        // newHttpRequest() unless we copy it explicitly.
        if (const auto& ct = req->getHeader("content-type"); !ct.empty()) {
            upstream_req->addHeader("Content-Type", ct);
        }
        upstream_req->setBody(std::string{req->getBody()});
        client->sendRequest(upstream_req,
            [cb, client](drogon::ReqResult r,
                         const drogon::HttpResponsePtr& resp) {
                forward(r, resp, cb);
            }, 30.0);
    }

private:
    static auto forward(drogon::ReqResult result,
                        const drogon::HttpResponsePtr& resp,
                        const std::function<void(const drogon::HttpResponsePtr&)>& cb)
        -> void {
        if (result != drogon::ReqResult::Ok || !resp) {
            nlohmann::json err{{"error", "plugin-orchestrator unreachable"}};
            auto out = drogon::HttpResponse::newHttpJsonResponse(err.dump());
            out->setStatusCode(drogon::k502BadGateway);
            cb(out);
            return;
        }
        auto out = drogon::HttpResponse::newHttpResponse();
        out->setStatusCode(resp->statusCode());
        out->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        out->setBody(std::string{resp->getBody()});
        cb(out);
    }
};

}  // namespace velocity::api_gateway::routes
