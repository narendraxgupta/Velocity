// =============================================================================
//  GET /v1/audit — proxy to audit-log's read API.
//
//  We deliberately don't hit QuestDB from the gateway. The audit-log
//  service owns the query path so SQL formatting, parameter binding,
//  and result shaping all live in one place. The gateway just forwards
//  the query string and the JSON response.
//
//  RBAC: the rbac.cpp table puts /v1/audit at ADMIN.
// =============================================================================

#include <cstdlib>

#include <drogon/HttpClient.h>
#include <drogon/HttpController.h>
#include <nlohmann/json.hpp>

#include "velocity/common/log.h"

namespace velocity::api_gateway::routes {

namespace {

auto upstream() -> std::string {
    if (const auto* env = std::getenv("AUDIT_LOG_URL"); env && *env) {
        return env;
    }
    return "http://audit-log.velocity-control.svc.cluster.local:8081";
}

}  // namespace

class Audit : public drogon::HttpController<Audit> {
public:
    METHOD_LIST_BEGIN
        METHOD_ADD(Audit::list, "/v1/audit", drogon::Get);
    METHOD_LIST_END

    auto list(const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& callback) const
        -> void {
        auto client = drogon::HttpClient::newHttpClient(upstream());
        auto upstream_req = drogon::HttpRequest::newHttpRequest();
        upstream_req->setMethod(drogon::Get);
        upstream_req->setPath("/v1/audit");
        // Forward query string verbatim.
        for (const auto& [k, v] : req->parameters()) {
            upstream_req->setParameter(k, v);
        }

        // `client` must be captured into the lambda — it owns the
        // connection pool for this in-flight request, and Drogon
        // tears down the request if the HttpClient shared_ptr is
        // released before the callback completes.
        client->sendRequest(upstream_req,
            [callback, client](drogon::ReqResult result,
                               const drogon::HttpResponsePtr& resp) {
                if (result != drogon::ReqResult::Ok || !resp) {
                    nlohmann::json err{{"error", "audit-log unreachable"}};
                    auto out = drogon::HttpResponse::newHttpJsonResponse(err.dump());
                    out->setStatusCode(drogon::k502BadGateway);
                    callback(out);
                    return;
                }
                auto out = drogon::HttpResponse::newHttpResponse();
                out->setStatusCode(resp->statusCode());
                out->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                out->setBody(std::string{resp->getBody()});
                callback(out);
            }, 10.0);
    }
};

}  // namespace velocity::api_gateway::routes
