// =============================================================================
//  /v1/submissions/{id}/adaptive-profile — surfaces anomaly-detector's
//  profile-picker verdict to the frontend.
//
//  Two endpoints:
//
//    POST /v1/submissions/{id}/adaptive-profile
//      Forwards to anomaly-detector with the (optional) last benchmark
//      report body. Returns the proposed profile name + reason.
//
//    GET  /v1/submissions/{id}/adaptive-profile
//      Returns the cached pick, if any. 404 when nothing has been
//      computed yet — frontend hides the panel in that case.
// =============================================================================
#include <memory>
#include <string>

#include <drogon/HttpClient.h>
#include <drogon/HttpController.h>

#include "velocity/common/log.h"

namespace velocity::api_gateway::routes {

namespace {

auto anomaly_client() -> std::shared_ptr<drogon::HttpClient> {
    static auto inst = []() {
        const auto* env = std::getenv("VELOCITY_ANOMALY_URL");
        const std::string url = (env && *env)
            ? env
            : "http://anomaly-detector.velocity-control.svc.cluster.local:8095";
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
    if (!req->getBody().empty()) {
        out->setBody(std::string(req->getBody()));
        out->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    }
    anomaly_client()->sendRequest(out,
        [cb = std::move(cb)](drogon::ReqResult r,
                             const drogon::HttpResponsePtr& resp) {
            if (r != drogon::ReqResult::Ok || !resp) {
                auto err = [](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(
                    R"({"error":"anomaly-detector unreachable"})");
                err->setStatusCode(drogon::k503ServiceUnavailable);
                cb(err);
                return;
            }
            cb(resp);
        },
        /*timeout=*/15.0);
}

}  // namespace

class AdaptiveProfile : public drogon::HttpController<AdaptiveProfile> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(AdaptiveProfile::propose, "/v1/submissions/{id}/adaptive-profile", drogon::Post);
        ADD_METHOD_TO(AdaptiveProfile::cached,  "/v1/submissions/{id}/adaptive-profile", drogon::Get);
    METHOD_LIST_END

    auto propose(const drogon::HttpRequestPtr& r,
                 std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                 const std::string& id) const -> void {
        forward(r, "/v1/adaptive-profile/" + id, std::move(cb));
    }
    auto cached(const drogon::HttpRequestPtr& r,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& id) const -> void {
        forward(r, "/v1/adaptive-profile/" + id, std::move(cb));
    }
};

}  // namespace velocity::api_gateway::routes
