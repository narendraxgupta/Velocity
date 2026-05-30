// =============================================================================
//  /v1/critiques/* and /v1/submissions/{id}/critique — strategy reviewer.
//
//  This route proxies to the critique-service, which talks to a local
//  LLM (Ollama). The gateway is intentionally thin: it doesn't hold
//  prompts, doesn't pre-format the source, doesn't touch Ollama
//  directly. All it does is:
//
//    1. Forward POSTs verbatim — submitters/operators pass the
//       benchmark report context they want critiqued.
//    2. Forward GETs by critique_id or submission_id.
//    3. Translate transport-level errors into clean JSON for the UI.
//
//  Authorisation note: critique creation is gated upstream by the
//  RBAC middleware (Submitter/Operator/Admin can create for their own
//  submissions; only Operator/Admin can target arbitrary IDs). We keep
//  that policy in middleware so the route file stays orthogonal.
// =============================================================================
#include <memory>
#include <string>

#include <drogon/HttpClient.h>
#include <drogon/HttpController.h>

#include "velocity/common/log.h"

namespace velocity::api_gateway::routes {

namespace {

auto critique_client() -> std::shared_ptr<drogon::HttpClient> {
    static auto inst = []() {
        const auto* env = std::getenv("VELOCITY_CRITIQUE_URL");
        const std::string url = (env && *env)
            ? env
            : "http://critique-service.velocity-control.svc.cluster.local:8094";
        return drogon::HttpClient::newHttpClient(url);
    }();
    return inst;
}

auto forward(const drogon::HttpRequestPtr& req,
             const std::string& upstream_path,
             std::function<void(const drogon::HttpResponsePtr&)>&& cb) -> void {
    auto out = drogon::HttpRequest::newHttpRequest();
    out->setPath(upstream_path);
    out->setMethod(req->getMethod());
    for (const auto& [k, v] : req->getParameters()) {
        out->setParameter(k, v);
    }
    if (!req->getBody().empty()) {
        out->setBody(std::string(req->getBody()));
        out->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    }
    critique_client()->sendRequest(
        out,
        [cb = std::move(cb)](drogon::ReqResult r,
                             const drogon::HttpResponsePtr& resp) {
            if (r != drogon::ReqResult::Ok || !resp) {
                auto err = [](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(
                    R"({"error":"critique-service unreachable"})");
                err->setStatusCode(drogon::k503ServiceUnavailable);
                cb(err);
                return;
            }
            cb(resp);
        },
        // Critique calls take seconds (LLM generation); 120s ceiling
        // matches the per-call ollama timeout in the service.
        /*timeout=*/120.0);
}

}  // namespace

class Critiques : public drogon::HttpController<Critiques> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(Critiques::create,             "/v1/critiques",                              drogon::Post);
        ADD_METHOD_TO(Critiques::getByID,            "/v1/critiques/{id}",                         drogon::Get);
        ADD_METHOD_TO(Critiques::getBySubmission,    "/v1/submissions/{id}/critique",              drogon::Get);
    METHOD_LIST_END

    auto create(const drogon::HttpRequestPtr& r,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb) const -> void {
        forward(r, "/v1/critiques", std::move(cb));
    }
    auto getByID(const drogon::HttpRequestPtr& r,
                 std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                 const std::string& id) const -> void {
        forward(r, "/v1/critiques/" + id, std::move(cb));
    }
    auto getBySubmission(const drogon::HttpRequestPtr& r,
                         std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                         const std::string& id) const -> void {
        forward(r, "/v1/critiques/by-submission/" + id, std::move(cb));
    }
};

}  // namespace velocity::api_gateway::routes
