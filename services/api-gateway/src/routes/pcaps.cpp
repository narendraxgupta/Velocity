// =============================================================================
//  /v1/pcaps/* and /v1/recorder/* — pass-through to pcap-replayer + pcap-recorder.
//
//  Same pattern as chaos.cpp: the gateway proxies; the actual logic lives
//  in the dedicated Go services that own RBAC + MinIO + the gopacket
//  parser. Doing this keeps the C++ image small (no pcap parser at the
//  edge) and confines tcpdump-adjacent privileges to one bounded service.
//
//  Routes:
//
//    POST /v1/pcaps/replay            → pcap-replayer
//    GET  /v1/pcaps                   → pcap-replayer (list)
//    GET  /v1/pcaps/{id}/state        → pcap-replayer (watch)
//    POST /v1/pcaps/{id}/cancel       → pcap-replayer
//    POST /v1/recorder/start          → pcap-recorder
//    POST /v1/recorder/stop           → pcap-recorder
//    GET  /v1/recorder/state          → pcap-recorder
// =============================================================================
#include <memory>
#include <string>

#include <drogon/HttpClient.h>
#include <drogon/HttpController.h>

#include "velocity/common/log.h"

namespace velocity::api_gateway::routes {

namespace {

auto replayer_client() -> std::shared_ptr<drogon::HttpClient> {
    static auto inst = []() {
        const auto* env = std::getenv("VELOCITY_REPLAYER_URL");
        const std::string url = (env && *env)
            ? env
            : "http://pcap-replayer.velocity-control.svc.cluster.local:8092";
        return drogon::HttpClient::newHttpClient(url);
    }();
    return inst;
}

auto recorder_client() -> std::shared_ptr<drogon::HttpClient> {
    static auto inst = []() {
        const auto* env = std::getenv("VELOCITY_RECORDER_URL");
        const std::string url = (env && *env)
            ? env
            : "http://pcap-recorder.velocity-control.svc.cluster.local:8091";
        return drogon::HttpClient::newHttpClient(url);
    }();
    return inst;
}

// forward proxies the inbound HTTP request as-is to `client` at
// `upstream_path`, preserving method + body + query string.
auto forward(const std::shared_ptr<drogon::HttpClient>& client,
             const drogon::HttpRequestPtr& req,
             const std::string& upstream_path,
             std::function<void(const drogon::HttpResponsePtr&)>&& cb,
             const std::string& service_label) -> void {
    auto out = drogon::HttpRequest::newHttpRequest();
    out->setPath(upstream_path);
    out->setMethod(req->getMethod());
    // Preserve query parameters (e.g. ?limit=200 for ListPcaps).
    for (const auto& [k, v] : req->getParameters()) {
        out->setParameter(k, v);
    }
    if (!req->getBody().empty()) {
        out->setBody(std::string(req->getBody()));
        out->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    }
    client->sendRequest(out,
        [cb = std::move(cb), service_label](
            drogon::ReqResult r, const drogon::HttpResponsePtr& resp) {
            if (r != drogon::ReqResult::Ok || !resp) {
                auto err = [](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(
                    R"({"error":")" + service_label + R"( unreachable"})");
                err->setStatusCode(drogon::k503ServiceUnavailable);
                cb(err);
                return;
            }
            cb(resp);
        },
        /*timeout=*/30.0);
}

}  // namespace

class Pcaps : public drogon::HttpController<Pcaps> {
public:
    METHOD_LIST_BEGIN
        // pcap-replayer surface
        ADD_METHOD_TO(Pcaps::startReplay,    "/v1/pcaps/replay",        drogon::Post);
        ADD_METHOD_TO(Pcaps::listPcaps,      "/v1/pcaps",               drogon::Get);
        ADD_METHOD_TO(Pcaps::watchReplay,    "/v1/pcaps/{id}/state",    drogon::Get);
        ADD_METHOD_TO(Pcaps::cancelReplay,   "/v1/pcaps/{id}/cancel",   drogon::Post);
        // pcap-recorder surface
        ADD_METHOD_TO(Pcaps::startRecording, "/v1/recorder/start",      drogon::Post);
        ADD_METHOD_TO(Pcaps::stopRecording,  "/v1/recorder/stop",       drogon::Post);
        ADD_METHOD_TO(Pcaps::recorderState,  "/v1/recorder/state",      drogon::Get);
        // Convenience shim: GET /v1/benchmarks/{id}/pcap → presigned MinIO URL.
        // We forward to the recorder which is the only service holding the
        // MinIO credentials for the pcap bucket prefix.
        ADD_METHOD_TO(Pcaps::pcapDownload,   "/v1/benchmarks/{id}/pcap", drogon::Get);
    METHOD_LIST_END

    auto startReplay(const drogon::HttpRequestPtr& r,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) const -> void {
        forward(replayer_client(), r, "/v1/pcaps/replay", std::move(cb), "pcap-replayer");
    }
    auto listPcaps(const drogon::HttpRequestPtr& r,
                   std::function<void(const drogon::HttpResponsePtr&)>&& cb) const -> void {
        forward(replayer_client(), r, "/v1/pcaps", std::move(cb), "pcap-replayer");
    }
    auto watchReplay(const drogon::HttpRequestPtr& r,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                     const std::string& id) const -> void {
        forward(replayer_client(), r, "/v1/pcaps/" + id + "/state", std::move(cb),
                "pcap-replayer");
    }
    auto cancelReplay(const drogon::HttpRequestPtr& r,
                      std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                      const std::string& id) const -> void {
        forward(replayer_client(), r, "/v1/pcaps/" + id + "/cancel", std::move(cb),
                "pcap-replayer");
    }
    auto startRecording(const drogon::HttpRequestPtr& r,
                        std::function<void(const drogon::HttpResponsePtr&)>&& cb) const -> void {
        forward(recorder_client(), r, "/v1/recorder/start", std::move(cb), "pcap-recorder");
    }
    auto stopRecording(const drogon::HttpRequestPtr& r,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb) const -> void {
        forward(recorder_client(), r, "/v1/recorder/stop", std::move(cb), "pcap-recorder");
    }
    auto recorderState(const drogon::HttpRequestPtr& r,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb) const -> void {
        forward(recorder_client(), r, "/v1/recorder/state", std::move(cb), "pcap-recorder");
    }
    auto pcapDownload(const drogon::HttpRequestPtr& r,
                      std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                      const std::string& id) const -> void {
        forward(recorder_client(), r, "/v1/recorder/pcap/" + id, std::move(cb),
                "pcap-recorder");
    }
};

}  // namespace velocity::api_gateway::routes
