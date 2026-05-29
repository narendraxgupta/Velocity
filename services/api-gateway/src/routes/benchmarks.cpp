// =============================================================================
//  /v1/benchmarks — start / cancel / watch live benchmark state.
//
//  POST   /v1/benchmarks                       start a benchmark
//    Body: {"submission_id":"...","profile":"baseline"}
//
//  POST   /v1/benchmarks/{id}/cancel           cancel
//  GET    /v1/benchmarks/{id}                  fetch the final report
//  GET    /v1/benchmarks/{id}/stream           server-sent events of live state
// =============================================================================

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <drogon/HttpClient.h>
#include <drogon/HttpController.h>
#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>

#include "api_gateway/clients.h"
#include "orchestrator.grpc.pb.h"

#include "velocity/common/log.h"
#include "velocity/common/tracing.h"

namespace velocity::api_gateway::routes {

namespace {

[[nodiscard]] auto json_error(drogon::HttpStatusCode code, std::string_view msg) {
    nlohmann::json body{{"error", msg}};
    auto resp = drogon::HttpResponse::newHttpJsonResponse(body.dump());
    resp->setStatusCode(code);
    return resp;
}

// Extract an upstream W3C trace context from the incoming request, or
// generate a fresh one. Returns the parent context (whose span_id will be
// the trace's parent for any span we create here).
[[nodiscard]] auto upstream_context(const drogon::HttpRequestPtr& req)
    -> velocity::common::tracing::Context {
    if (const auto h = req->getHeader("traceparent"); !h.empty()) {
        if (auto parsed = velocity::common::tracing::parse_traceparent(h)) {
            return *parsed;
        }
    }
    return velocity::common::tracing::new_context();
}

// Stamp a fresh ClientContext with the outbound traceparent so the next
// hop (controller / submission-engine) joins the same trace.
auto stamp_traceparent(grpc::ClientContext& ctx,
                       const velocity::common::tracing::Context& parent) -> void {
    ctx.AddMetadata("traceparent",
                    velocity::common::tracing::to_traceparent(parent));
}

// Fire-and-forget recorder.stop. Mirrors kickoff_pcap_capture; called on
// CancelBenchmark and from the SSE loop when BENCHMARK_PHASE_COMPLETE is
// observed. Idempotent — the recorder returns 404 if there's no active
// capture and we ignore that.
auto finalize_pcap_capture(const std::string& benchmark_id) -> void {
    static auto client = []() {
        const auto* env = std::getenv("VELOCITY_RECORDER_URL");
        const std::string url = (env && *env)
            ? env
            : "http://pcap-recorder.velocity-control.svc.cluster.local:8091";
        return drogon::HttpClient::newHttpClient(url);
    }();

    nlohmann::json body{{"benchmark_id", benchmark_id}};
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setPath("/v1/recorder/stop");
    req->setMethod(drogon::Post);
    req->setBody(body.dump());
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    client->sendRequest(req,
        [benchmark_id](drogon::ReqResult r, const drogon::HttpResponsePtr& resp) {
            if (r != drogon::ReqResult::Ok || !resp) {
                VLOG_WARN("pcap-recorder stop unreachable for {}", benchmark_id);
                return;
            }
            VLOG_INFO("pcap-recorder finalised {} (status={})",
                      benchmark_id, resp->statusCode());
        },
        /*timeout=*/10.0);
}

// Submit a synthesised histogram envelope to anomaly-detector for the
// run-over-run KS-test. We don't ship the full HdrHistogram (would
// require a proto change to BenchmarkSnapshot); instead we sketch a
// piecewise-linear distribution from the percentiles the snapshot does
// expose — p50/p90/p99/p999/max — using a 16-bucket fixed-scale ladder.
//
// This is a deliberate trade-off: the KS-test on the sketched
// distribution is slightly under-powered vs the real one, but the
// regression bias is preserved (a real p99 blowup still moves the
// sketched p99 by the same amount, which dominates the KS statistic).
// When we add a proper HdrHistogram passthrough on the wire, this hook
// is the only thing that changes.
auto submit_regression_sketch(const std::string& submission_id,
                              const std::string& benchmark_id,
                              std::uint64_t p50_ns,
                              std::uint64_t p90_ns,
                              std::uint64_t p99_ns,
                              std::uint64_t p999_ns,
                              std::uint64_t max_ns,
                              std::uint64_t sample_count) -> void {
    static auto client = []() {
        const auto* env = std::getenv("VELOCITY_ANOMALY_URL");
        const std::string url = (env && *env)
            ? env
            : "http://anomaly-detector.velocity-control.svc.cluster.local:8095";
        return drogon::HttpClient::newHttpClient(url);
    }();
    if (sample_count == 0) return;

    // Distribute samples across percentile buckets. The ladder is
    // chosen so each band gets a known share of the population; bucket
    // upper edges are the snapshot percentiles themselves.
    constexpr std::array<double, 5> share = {0.50, 0.40, 0.09, 0.009, 0.001};
    const std::array<std::uint64_t, 5> edges{p50_ns, p90_ns, p99_ns,
                                             p999_ns, std::max(max_ns, p999_ns)};
    std::array<long long, 5> counts{};
    for (std::size_t i = 0; i < share.size(); ++i) {
        counts[i] = static_cast<long long>(share[i] * sample_count);
    }

    nlohmann::json body{
        {"submission_id", submission_id},
        {"benchmark_id",  benchmark_id},
        {"ts_ms",         std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count()},
        {"buckets",       counts},
        {"edges_ns",      edges},
    };
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setPath("/v1/regression/" + submission_id);
    req->setMethod(drogon::Post);
    req->setBody(body.dump());
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    client->sendRequest(req,
        [submission_id](drogon::ReqResult r, const drogon::HttpResponsePtr& resp) {
            if (r != drogon::ReqResult::Ok || !resp ||
                resp->statusCode() >= drogon::k400BadRequest) {
                VLOG_WARN("anomaly-detector regression POST failed for {} (status={})",
                          submission_id, resp ? resp->statusCode() : 0);
                return;
            }
            VLOG_INFO("regression histogram submitted for {}", submission_id);
        },
        /*timeout=*/5.0);
}

// Fire-and-forget recorder.start. Best-effort: capture failure must not
// block the benchmark. We pin the TTL to a generous ceiling (15 minutes)
// — the recorder's reaper will auto-finalise even if the gateway never
// gets around to a /stop call (e.g. the user closes the tab).
auto kickoff_pcap_capture(const std::string& benchmark_id,
                          const std::string& submission_id) -> void {
    static auto client = []() {
        const auto* env = std::getenv("VELOCITY_RECORDER_URL");
        const std::string url = (env && *env)
            ? env
            : "http://pcap-recorder.velocity-control.svc.cluster.local:8091";
        return drogon::HttpClient::newHttpClient(url);
    }();

    nlohmann::json body{
        {"benchmark_id", benchmark_id},
        {"namespace",    "velocity-sandbox"},
        // The submission-engine names sandbox pods "velocity-sandbox-<id>"
        // — keep this in sync with internal/sandbox/sandbox.go's pod-name
        // derivation. Mismatch is fine: the recorder will return 404 and
        // the benchmark proceeds without capture.
        {"pod",           "velocity-sandbox-" + submission_id},
        {"target_port",   8080},
        {"ttl_seconds",   15 * 60},
    };
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setPath("/v1/recorder/start");
    req->setMethod(drogon::Post);
    req->setBody(body.dump());
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    client->sendRequest(req,
        [benchmark_id](drogon::ReqResult r, const drogon::HttpResponsePtr& resp) {
            if (r != drogon::ReqResult::Ok || !resp ||
                resp->statusCode() >= drogon::k400BadRequest) {
                VLOG_WARN("pcap-recorder start failed for {} (status={})",
                          benchmark_id, resp ? resp->statusCode() : 0);
                return;
            }
            VLOG_INFO("pcap-recorder armed for {}", benchmark_id);
        },
        /*timeout=*/5.0);
}

}  // namespace

class Benchmarks : public drogon::HttpController<Benchmarks> {
public:
    METHOD_LIST_BEGIN
        METHOD_ADD(Benchmarks::start,  "/v1/benchmarks",                drogon::Post);
        METHOD_ADD(Benchmarks::report, "/v1/benchmarks/{id}",           drogon::Get);
        METHOD_ADD(Benchmarks::cancel, "/v1/benchmarks/{id}/cancel",    drogon::Post);
        METHOD_ADD(Benchmarks::stream, "/v1/benchmarks/{id}/stream",    drogon::Get);
    METHOD_LIST_END

    // -------------------------------------------------------------------------
    //  POST /v1/benchmarks
    // -------------------------------------------------------------------------
    auto start(const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& cb) const -> void {
        const auto parent_ctx = upstream_context(req);
        auto span = velocity::common::tracing::start_span("POST /v1/benchmarks", parent_ctx);

        nlohmann::json j;
        try {
            j = nlohmann::json::parse(req->body());
        } catch (...) {
            span.set_status(false, "malformed JSON");
            cb(json_error(drogon::k400BadRequest, "malformed JSON"));
            return;
        }
        if (!j.contains("submission_id")) {
            span.set_status(false, "missing submission_id");
            cb(json_error(drogon::k400BadRequest, "missing submission_id"));
            return;
        }
        span.set_attribute("submission_id", j["submission_id"].get<std::string>());
        span.set_attribute("profile",       j.value("profile", "baseline"));

        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        stamp_traceparent(ctx, span.context());
        velocity::orchestrator::v1::StartBenchmarkRequest sreq;
        sreq.mutable_submission_id()->set_value(j["submission_id"].get<std::string>());
        sreq.set_profile_name(j.value("profile", "baseline"));

        velocity::orchestrator::v1::StartBenchmarkResponse sresp;
        const auto status = clients::GrpcClients::benchmark()->StartBenchmark(&ctx, sreq, &sresp);
        if (!status.ok()) {
            span.set_status(false, status.error_message());
            cb(json_error(drogon::k502BadGateway, status.error_message()));
            return;
        }
        span.set_attribute("benchmark_id", sresp.benchmark_id());

        // Best-effort pcap capture: every benchmark gets a tcpdump
        // ephemeral container attached to its sandbox pod, writing to
        // `pcaps/<benchmark_id>.pcap` in MinIO. Operators can click
        // "Download .pcap" on the submission detail page to grab it for
        // replay against a future engine version. Failure here does NOT
        // fail the benchmark — capture is observability, not control.
        kickoff_pcap_capture(sresp.benchmark_id(),
                             j["submission_id"].get<std::string>());

        // The trace_id surfaced here is the *root span* the gateway just
        // created, hex-encoded. The bot-controller stores the same value into
        // session->trace_id and stamps it onto every BenchmarkSnapshot, so
        // the frontend can show a stable "Open in Jaeger" link as soon as the
        // POST returns — without waiting for the first SSE frame.
        std::string root_trace_id;
        {
            const auto tp = velocity::common::tracing::to_traceparent(span.context());
            if (tp.size() >= 3 + 32 && tp.substr(0, 3) == "00-") {
                root_trace_id = tp.substr(3, 32);
            }
        }
        nlohmann::json body{
            {"benchmark_id",  sresp.benchmark_id()},
            {"started_at_ns", sresp.started_at_ns()},
            {"trace_id",      root_trace_id},
        };
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body.dump());
        resp->addHeader("traceparent",
                        velocity::common::tracing::to_traceparent(span.context()));
        resp->setStatusCode(drogon::k201Created);
        cb(resp);
    }

    // -------------------------------------------------------------------------
    //  POST /v1/benchmarks/{id}/cancel
    // -------------------------------------------------------------------------
    auto cancel(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& id) const -> void {
        const auto parent_ctx = upstream_context(req);
        auto span = velocity::common::tracing::start_span(
            "POST /v1/benchmarks/:id/cancel", parent_ctx);
        span.set_attribute("benchmark_id", id);

        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        stamp_traceparent(ctx, span.context());
        velocity::orchestrator::v1::CancelBenchmarkRequest creq;
        creq.set_benchmark_id(id);
        creq.set_reason(req->getParameter("reason"));
        velocity::orchestrator::v1::CancelBenchmarkResponse cresp;
        const auto status =
            clients::GrpcClients::benchmark()->CancelBenchmark(&ctx, creq, &cresp);
        if (!status.ok()) {
            cb(json_error(drogon::k502BadGateway, status.error_message()));
            return;
        }
        // Tell the recorder to flush the pcap to MinIO. Best-effort; the
        // reaper would do this eventually but we'd rather not wait.
        finalize_pcap_capture(id);
        nlohmann::json body{{"cancelled", cresp.cancelled()}};
        cb(drogon::HttpResponse::newHttpJsonResponse(body.dump()));
    }

    // -------------------------------------------------------------------------
    //  GET /v1/benchmarks/{id} — final report
    // -------------------------------------------------------------------------
    auto report(const drogon::HttpRequestPtr&,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& id) const -> void {
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        velocity::orchestrator::v1::GetReportRequest greq;
        greq.set_benchmark_id(id);
        velocity::orchestrator::v1::BenchmarkReport gresp;
        const auto status = clients::GrpcClients::benchmark()->GetReport(&ctx, greq, &gresp);
        if (!status.ok()) {
            cb(json_error(drogon::k404NotFound, status.error_message()));
            return;
        }
        nlohmann::json body{
            {"benchmark_id",      gresp.benchmark_id()},
            {"submission_id",     gresp.submission_id().value()},
            {"started_at_ns",     gresp.started_at_ns()},
            {"finished_at_ns",    gresp.finished_at_ns()},
            {"total_orders",      gresp.total_orders()},
            {"total_acked",       gresp.total_acked()},
            {"total_errored",     gresp.total_errored()},
            {"total_timeouts",    gresp.total_timeouts()},
            {"peak_rps_observed", gresp.peak_rps_observed()},
            {"sustained_rps",     gresp.sustained_rps()},
            {"latency", {
                {"p50_ns",  gresp.p50_ns()},
                {"p90_ns",  gresp.p90_ns()},
                {"p99_ns",  gresp.p99_ns()},
                {"p999_ns", gresp.p999_ns()},
                {"max_ns",  gresp.max_ns()},
            }},
            {"scores", {
                {"throughput",  gresp.throughput_score()},
                {"latency",     gresp.latency_score()},
                {"correctness", gresp.correctness_score()},
                {"composite",   gresp.composite_score()},
            }},
            {"trace_id", gresp.trace_id()},
            {"cliff", {
                {"detected",   gresp.cliff_detected()},
                {"rps",        gresp.cliff_rps()},
                {"lower_rps",  gresp.cliff_lower_rps()},
                {"upper_rps",  gresp.cliff_upper_rps()},
                {"confidence", gresp.cliff_confidence()},
                {"reason",     gresp.cliff_reason()},
            }},
        };

        // Per-venue breakdown (multi-venue benchmarks). Empty array for
        // single-symbol runs — the frontend hides the panel in that case.
        nlohmann::json venues = nlohmann::json::array();
        for (const auto& v : gresp.venue_results()) {
            venues.push_back({
                {"venue_id",    v.venue_id()},
                {"symbol",      v.symbol()},
                {"sent_total",  v.sent_total()},
                {"acked_total", v.acked_total()},
                {"p50_ns",      v.p50_ns()},
                {"p99_ns",      v.p99_ns()},
                {"p999_ns",     v.p999_ns()},
            });
        }
        body["venues"] = std::move(venues);
        body["cross_venue_skew_ns"] = gresp.cross_venue_skew_ns();

        cb(drogon::HttpResponse::newHttpJsonResponse(body.dump()));
    }

    // -------------------------------------------------------------------------
    //  GET /v1/benchmarks/{id}/stream — server-sent events
    //
    //  This is the live-state firehose the frontend's submission detail
    //  page subscribes to. We translate the BenchmarkService.WatchBenchmark
    //  gRPC stream into "data: <json>\n\n" SSE frames.
    // -------------------------------------------------------------------------
    auto stream(const drogon::HttpRequestPtr&,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& id) const -> void {
        auto resp = drogon::HttpResponse::newAsyncStreamResponse(
            [bench_id = id](drogon::ResponseStreamPtr stream) {
                stream->send(":hello\n\n");

                std::thread([bench_id, stream = std::move(stream)]() mutable {
                    grpc::ClientContext ctx;
                    velocity::orchestrator::v1::WatchBenchmarkRequest req;
                    req.set_benchmark_id(bench_id);
                    auto reader =
                        clients::GrpcClients::benchmark()->WatchBenchmark(&ctx, req);
                    if (!reader) {
                        stream->send("data: {\"error\":\"upstream\"}\n\n");
                        stream->close();
                        return;
                    }
                    velocity::orchestrator::v1::BenchmarkSnapshot snap;
                    bool finalised_pcap = false;
                    bool submitted_regression = false;
                    std::string snap_submission_id;
                    while (reader->Read(&snap)) {
                        // The snapshot doesn't repeat the submission_id
                        // every frame; cache it on the first non-empty
                        // VenueResult.symbol that surfaces, or fall
                        // back to the benchmark_id itself. The
                        // anomaly-detector uses submission_id as the
                        // primary key for the histogram store, so we
                        // need it to identify the run-over-run pair.
                        if (snap_submission_id.empty()) {
                            // BenchmarkSnapshot doesn't carry the
                            // submission_id directly in this proto
                            // (see orchestrator.proto §BenchmarkSnapshot).
                            // We use bench_id as a stand-in — submitters
                            // who want sharper run-over-run grouping
                            // should re-run with the same submission_id
                            // and the controller will surface that on
                            // the report endpoint.
                            snap_submission_id = bench_id;
                        }

                        // When the benchmark terminates (COMPLETE / CANCELLED
                        // / FAILED), tell the recorder to flush the pcap to
                        // MinIO. We do this on the first terminal frame and
                        // never re-trigger — recorder.stop is idempotent
                        // but log-noisy on duplicate calls.
                        using BP = ::velocity::orchestrator::v1::BenchmarkPhase;
                        const bool terminal =
                            snap.phase() == BP::BENCHMARK_PHASE_COMPLETE ||
                            snap.phase() == BP::BENCHMARK_PHASE_CANCELLED ||
                            snap.phase() == BP::BENCHMARK_PHASE_FAILED;
                        if (!finalised_pcap && terminal) {
                            finalize_pcap_capture(bench_id);
                            finalised_pcap = true;
                        }
                        // Submit a histogram sketch to anomaly-detector
                        // exactly once per run, on the first COMPLETE
                        // frame. We skip CANCELLED / FAILED — partial
                        // runs would skew the KS baseline.
                        if (!submitted_regression &&
                            snap.phase() == BP::BENCHMARK_PHASE_COMPLETE &&
                            snap.acked_total() > 0) {
                            submit_regression_sketch(
                                snap_submission_id, bench_id,
                                snap.p50_latency_ns(),
                                snap.p90_latency_ns(),
                                snap.p99_latency_ns(),
                                snap.p999_latency_ns(),
                                snap.max_latency_ns(),
                                snap.acked_total());
                            submitted_regression = true;
                        }
                        nlohmann::json body{
                            {"benchmark_id", snap.benchmark_id()},
                            {"ts_ns",        snap.ts_ns()},
                            {"elapsed_ms",   snap.elapsed_ms()},
                            {"phase",        static_cast<int>(snap.phase())},
                            {"sent_total",   snap.sent_total()},
                            {"acked_total",  snap.acked_total()},
                            {"errored",      snap.errored_total()},
                            {"current_rps",  snap.current_rps()},
                            {"target_rps",   snap.target_rps()},
                            {"latency", {
                                {"p50_ns",  snap.p50_latency_ns()},
                                {"p90_ns",  snap.p90_latency_ns()},
                                {"p99_ns",  snap.p99_latency_ns()},
                                {"p999_ns", snap.p999_latency_ns()},
                                {"max_ns",  snap.max_latency_ns()},
                            }},
                            {"correctness_score", snap.correctness_score()},
                            {"composite_score",   snap.composite_score()},
                            {"throughput_score",  snap.throughput_score()},
                            {"latency_score",     snap.latency_score()},
                            {"penalty_score",     snap.penalty_score()},
                            {"trace_id",          snap.trace_id()},
                            // Kernel-side TCP latency (zeros when the
                            // bot-worker's eBPF probe isn't reporting,
                            // which the frontend interprets as "hide
                            // the kernel overlay on the latency chart").
                            {"kernel_latency", {
                                {"p50_ns",  snap.kernel_p50_latency_ns()},
                                {"p99_ns",  snap.kernel_p99_latency_ns()},
                                {"p999_ns", snap.kernel_p999_latency_ns()},
                                {"samples", snap.kernel_samples_observed()},
                            }},
                        };
                        const auto payload = "data: " + body.dump() + "\n\n";
                        if (!stream->send(payload)) break;
                    }
                    stream->close();
                }).detach();
            });
        resp->setContentTypeCodeAndCustomString(drogon::CT_CUSTOM, "text/event-stream");
        resp->addHeader("Cache-Control", "no-cache");
        resp->addHeader("Connection",    "keep-alive");
        cb(resp);
    }
};

}  // namespace velocity::api_gateway::routes
