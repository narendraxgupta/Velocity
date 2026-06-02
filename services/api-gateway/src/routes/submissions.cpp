// =============================================================================
//  /v1/submissions — submission artefact upload + lifecycle.
//
//  POST   /v1/submissions
//    Body: raw artefact bytes (tarball, Dockerfile, or pre-built image tar)
//    Query: ?team=&display=&kind=DOCKERFILE|SOURCE_TAR|BINARY|OCI_IMAGE&filename=
//    Returns: { submission_id, artefact_object_key, sha256 }
//    Flow:
//      1. Stream-upload to the Submission Engine via Upload() bidi stream.
//      2. Engine writes to MinIO, returns the object key + sha256.
//      3. Gateway calls Register() with the key.
//      4. Gateway returns the assigned submission_id.
//
//  GET    /v1/submissions/{id}
//    Returns metadata from the engine via WatchSubmission (first snapshot).
//
//  POST   /v1/submissions/{id}/teardown
//    Calls Teardown().
//
//  POST   /v1/submissions/{id}/build
//    Calls Build().
//
//  POST   /v1/submissions/{id}/deploy
//    Calls Deploy() and returns the endpoint.
// =============================================================================

#include <chrono>
#include <memory>
#include <string>

#include <drogon/HttpController.h>
#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>

#include "api_gateway/clients.h"
#include "orchestrator.grpc.pb.h"

#include "velocity/common/log.h"
#include "velocity/common/tracing.h"

namespace velocity::api_gateway::routes {

namespace {

[[nodiscard]] auto kind_from(std::string_view s) -> velocity::orchestrator::v1::ArtefactKind {
    using K = velocity::orchestrator::v1::ArtefactKind;
    if (s == "DOCKERFILE")  return K::ARTEFACT_KIND_DOCKERFILE;
    if (s == "SOURCE_TAR")  return K::ARTEFACT_KIND_SOURCE_TAR;
    if (s == "BINARY")      return K::ARTEFACT_KIND_BINARY;
    if (s == "OCI_IMAGE")   return K::ARTEFACT_KIND_OCI_IMAGE;
    if (s == "PCAP_REPLAY") return K::ARTEFACT_KIND_PCAP_REPLAY;
    return K::ARTEFACT_KIND_UNSPECIFIED;
}

[[nodiscard]] auto json_error(drogon::HttpStatusCode code, std::string_view msg) {
    nlohmann::json body{{"error", msg}};
    auto resp = [](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(body.dump());
    resp->setStatusCode(code);
    return resp;
}

// Mirror of benchmarks.cpp's upstream_context / stamp_traceparent helpers.
// Kept inline here (rather than promoted to a shared header) so each route
// file stays self-contained — they're four lines apiece. If we add a third
// caller we'll consolidate to api_gateway/tracing_util.h.
[[nodiscard]] auto upstream_context(const drogon::HttpRequestPtr& req)
    -> velocity::common::tracing::Context {
    if (const auto h = req->getHeader("traceparent"); !h.empty()) {
        if (auto parsed = velocity::common::tracing::parse_traceparent(h)) {
            return *parsed;
        }
    }
    return velocity::common::tracing::new_context();
}

auto stamp_traceparent(grpc::ClientContext& ctx,
                       const velocity::common::tracing::Context& parent) -> void {
    ctx.AddMetadata("traceparent",
                    velocity::common::tracing::to_traceparent(parent));
}

}  // namespace

class Submissions : public drogon::HttpController<Submissions> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(Submissions::create,     "/v1/submissions",                  drogon::Post);
        ADD_METHOD_TO(Submissions::list,       "/v1/submissions",                  drogon::Get);
        ADD_METHOD_TO(Submissions::detail,     "/v1/submissions/{id}",             drogon::Get);
        ADD_METHOD_TO(Submissions::build,      "/v1/submissions/{id}/build",       drogon::Post);
        ADD_METHOD_TO(Submissions::deploy,     "/v1/submissions/{id}/deploy",      drogon::Post);
        ADD_METHOD_TO(Submissions::teardown,   "/v1/submissions/{id}/teardown",    drogon::Post);
        ADD_METHOD_TO(Submissions::flamegraph, "/v1/submissions/{id}/flamegraph",  drogon::Get);
    METHOD_LIST_END

    // -------------------------------------------------------------------------
    //  POST /v1/submissions — upload + register
    // -------------------------------------------------------------------------
    auto create(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb) const -> void {
        const auto team_name    = req->getParameter("team");
        const auto display_name = req->getParameter("display");
        const auto kind_str     = req->getParameter("kind");
        const auto filename     = req->getParameter("filename");

        if (display_name.empty() || team_name.empty() || filename.empty()) {
            cb(json_error(drogon::k400BadRequest, "missing team/display/filename"));
            return;
        }

        const auto parent_ctx = upstream_context(req);
        auto span = velocity::common::tracing::start_span(
            "POST /v1/submissions", parent_ctx);
        span.set_attribute("team",        team_name);
        span.set_attribute("display",     display_name);
        span.set_attribute("kind",        kind_str);
        span.set_attribute("body.size",   std::to_string(req->body().size()));

        // Stream the body through to the Submission Engine.
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::minutes(5));
        stamp_traceparent(ctx, span.context());
        velocity::orchestrator::v1::UploadResponse upload_resp;

        auto stream = clients::GrpcClients::submission()->Upload(&ctx, &upload_resp);
        if (!stream) {
            cb(json_error(drogon::k503ServiceUnavailable, "submission engine unreachable"));
            return;
        }

        // Header.
        velocity::orchestrator::v1::UploadRequest header_msg;
        auto* hdr = header_msg.mutable_header();
        hdr->set_filename(filename);
        hdr->set_kind(kind_from(kind_str));
        hdr->set_display_name(display_name);
        hdr->set_team_name(team_name);
        hdr->set_total_bytes(req->body().size());
        if (!stream->Write(header_msg)) {
            cb(json_error(drogon::k503ServiceUnavailable, "engine refused upload header"));
            return;
        }

        // Chunked body — 256 KiB chunks.
        constexpr std::size_t kChunk = 256 * 1024;
        const auto body = req->body();
        for (std::size_t off = 0; off < body.size(); off += kChunk) {
            velocity::orchestrator::v1::UploadRequest chunk_msg;
            auto* c = chunk_msg.mutable_chunk();
            const auto take = std::min(kChunk, body.size() - off);
            c->set_data(body.data() + off, take);
            if (!stream->Write(chunk_msg)) {
                cb(json_error(drogon::k502BadGateway, "upload stream broke mid-flight"));
                return;
            }
        }
        stream->WritesDone();
        const auto status = stream->Finish();
        if (!status.ok()) {
            VLOG_WARN("Upload() failed: {}", status.error_message());
            cb(json_error(drogon::k502BadGateway, "engine Upload failed"));
            return;
        }

        // Register.
        grpc::ClientContext reg_ctx;
        reg_ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        stamp_traceparent(reg_ctx, span.context());
        velocity::orchestrator::v1::RegisterRequest reg_req;
        reg_req.set_display_name(display_name);
        reg_req.set_team_name(team_name);
        reg_req.set_artefact_object_key(upload_resp.artefact_object_key());
        reg_req.set_artefact_sha256(upload_resp.sha256());
        reg_req.set_kind(kind_from(kind_str));
        velocity::orchestrator::v1::RegisterResponse reg_resp;
        const auto reg_status =
            clients::GrpcClients::submission()->Register(&reg_ctx, reg_req, &reg_resp);
        if (!reg_status.ok()) {
            span.set_status(false, reg_status.error_message());
            cb(json_error(drogon::k502BadGateway,
                          "engine Register failed: " + reg_status.error_message()));
            return;
        }
        span.set_attribute("submission_id", reg_resp.submission_id().value());

        nlohmann::json body_out{
            {"submission_id",       reg_resp.submission_id().value()},
            {"artefact_object_key", upload_resp.artefact_object_key()},
            {"sha256",              upload_resp.sha256()},
            {"received_bytes",      upload_resp.received_bytes()},
        };
        auto resp = [](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(body_out.dump());
        resp->addHeader("traceparent",
                        velocity::common::tracing::to_traceparent(span.context()));
        resp->setStatusCode(drogon::k201Created);
        cb(resp);
    }

    // -------------------------------------------------------------------------
    //  GET /v1/submissions
    // -------------------------------------------------------------------------
    auto list(const drogon::HttpRequestPtr&,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) const -> void {
        // Listing is owned by the submission-engine via its database, not
        // surfaced over gRPC yet — return an empty array until then.
        cb([](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(R"({"items":[]})"));
    }

    // -------------------------------------------------------------------------
    //  GET /v1/submissions/{id}
    // -------------------------------------------------------------------------
    auto detail(const drogon::HttpRequestPtr&,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& id) const -> void {
        // Open WatchSubmission stream, grab the first snapshot, close.
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        velocity::orchestrator::v1::WatchSubmissionRequest req;
        req.mutable_submission_id()->set_value(id);
        auto reader = clients::GrpcClients::submission()->WatchSubmission(&ctx, req);
        if (!reader) {
            cb(json_error(drogon::k503ServiceUnavailable, "engine unreachable"));
            return;
        }

        velocity::orchestrator::v1::SubmissionStatus snap;
        if (!reader->Read(&snap)) {
            // Read can fail because the submission genuinely doesn't exist
            // (clean stream end / NOT_FOUND → 404) OR because the engine is
            // down/slow. Inspect the final status so an outage isn't reported
            // to the client as a misleading 404.
            const auto status = reader->Finish();
            drogon::HttpStatusCode code = drogon::k404NotFound;
            switch (status.error_code()) {
                case grpc::StatusCode::OK:
                case grpc::StatusCode::NOT_FOUND:         code = drogon::k404NotFound;          break;
                case grpc::StatusCode::UNAVAILABLE:       code = drogon::k503ServiceUnavailable; break;
                case grpc::StatusCode::DEADLINE_EXCEEDED: code = drogon::k504GatewayTimeout;     break;
                default:                                  code = drogon::k502BadGateway;         break;
            }
            cb(json_error(code, status.ok() ? "no such submission"
                                            : status.error_message()));
            return;
        }
        ctx.TryCancel();
        (void)reader->Finish();

        nlohmann::json body{
            {"submission_id", snap.submission_id().value()},
            {"phase",         static_cast<int>(snap.phase())},
            {"detail",        snap.detail()},
            {"ts_ns",         snap.ts_ns()},
        };
        cb([](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(body.dump()));
    }

    // -------------------------------------------------------------------------
    //  POST /v1/submissions/{id}/build
    // -------------------------------------------------------------------------
    auto build(const drogon::HttpRequestPtr& http_req,
               std::function<void(const drogon::HttpResponsePtr&)>&& cb,
               const std::string& id) const -> void {
        const auto parent_ctx = upstream_context(http_req);
        auto span = velocity::common::tracing::start_span(
            "POST /v1/submissions/:id/build", parent_ctx);
        span.set_attribute("submission_id", id);
        // Kaniko builds can take minutes; match the engine-side cap of
        // 20 minutes plus a small margin so we don't surface a 504 in
        // front of a build that's still healthy from the engine's POV.
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::minutes(25));
        stamp_traceparent(ctx, span.context());
        velocity::orchestrator::v1::BuildRequest req;
        req.mutable_submission_id()->set_value(id);
        velocity::orchestrator::v1::BuildResponse resp;
        const auto status = clients::GrpcClients::submission()->Build(&ctx, req, &resp);
        if (!status.ok()) {
            span.set_status(false, status.error_message());
            cb(json_error(drogon::k502BadGateway, status.error_message()));
            return;
        }
        span.set_attribute("image_ref", resp.image_ref());
        nlohmann::json body{{"submission_id", id}, {"image_ref", resp.image_ref()}};
        auto out = [](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(body.dump());
        out->setStatusCode(drogon::k202Accepted);
        cb(out);
    }

    // -------------------------------------------------------------------------
    //  POST /v1/submissions/{id}/deploy
    // -------------------------------------------------------------------------
    auto deploy(const drogon::HttpRequestPtr& http_req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& id) const -> void {
        const auto parent_ctx = upstream_context(http_req);
        auto span = velocity::common::tracing::start_span(
            "POST /v1/submissions/:id/deploy", parent_ctx);
        span.set_attribute("submission_id", id);
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(60));
        stamp_traceparent(ctx, span.context());
        velocity::orchestrator::v1::DeployRequest req;
        req.mutable_submission_id()->set_value(id);
        auto* lim = req.mutable_limits();
        lim->set_cpu_cores(4);
        lim->set_mem_bytes(4ULL * 1024 * 1024 * 1024);
        lim->set_ephemeral_bytes(2ULL * 1024 * 1024 * 1024);
        lim->set_lifetime_seconds(20 * 60);
        velocity::orchestrator::v1::DeployResponse resp;
        const auto status = clients::GrpcClients::submission()->Deploy(&ctx, req, &resp);
        if (!status.ok()) {
            span.set_status(false, status.error_message());
            cb(json_error(drogon::k502BadGateway, status.error_message()));
            return;
        }
        span.set_attribute("endpoint.host", resp.endpoint().host());
        span.set_attribute("endpoint.port", std::to_string(resp.endpoint().port()));
        span.set_attribute("pod_name",      resp.pod_name());
        nlohmann::json body{
            {"submission_id", id},
            {"endpoint", {
                {"host", resp.endpoint().host()},
                {"port", resp.endpoint().port()},
            }},
            {"pod_name",  resp.pod_name()},
            {"namespace", resp.namespace_()},
        };
        cb([](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(body.dump()));
    }

    // -------------------------------------------------------------------------
    //  GET /v1/submissions/{id}/flamegraph
    //
    //  Returns the folded-stack flamegraph for this submission, recorded by
    //  the perf-profiler sidecar while the engine ran. Body is JSON so the
    //  frontend can pluck the metadata; the `folded` field is the multi-line
    //  Brendan Gregg format directly consumable by d3-flamegraph.
    // -------------------------------------------------------------------------
    auto flamegraph(const drogon::HttpRequestPtr&,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                    const std::string& id) const -> void {
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(15));
        velocity::orchestrator::v1::GetFlamegraphRequest req;
        req.mutable_submission_id()->set_value(id);
        velocity::orchestrator::v1::GetFlamegraphResponse resp;
        const auto status =
            clients::GrpcClients::submission()->GetFlamegraph(&ctx, req, &resp);
        if (!status.ok()) {
            // The most common failure path here is "no flamegraph yet" — map
            // it to a 404 instead of a generic 502 so the frontend can hide
            // the panel cleanly.
            const auto code = status.error_code() == grpc::NOT_FOUND
                ? drogon::k404NotFound
                : drogon::k502BadGateway;
            cb(json_error(code, status.error_message()));
            return;
        }
        nlohmann::json body{
            {"submission_id",    id},
            {"folded",           resp.folded()},
            {"recorded_at_ns",   resp.recorded_at_ns()},
            {"sample_freq_hz",   resp.sample_freq_hz()},
            {"duration_seconds", resp.duration_seconds()},
        };
        cb([](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(body.dump()));
    }

    // -------------------------------------------------------------------------
    //  POST /v1/submissions/{id}/teardown
    // -------------------------------------------------------------------------
    auto teardown(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                  const std::string& id) const -> void {
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        velocity::orchestrator::v1::TeardownRequest treq;
        treq.mutable_submission_id()->set_value(id);
        treq.set_force(req->getParameter("force") == "1");
        velocity::orchestrator::v1::TeardownResponse tresp;
        const auto status = clients::GrpcClients::submission()->Teardown(&ctx, treq, &tresp);
        if (!status.ok()) {
            cb(json_error(drogon::k502BadGateway, status.error_message()));
            return;
        }
        nlohmann::json body{
            {"submission_id", id},
            {"teardown_complete", tresp.teardown_complete()},
        };
        cb([](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(body.dump()));
    }
};

}  // namespace velocity::api_gateway::routes
