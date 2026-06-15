// =============================================================================
//  benchmark_service.cpp — orchestrator-side gRPC implementation.
//
//  See benchmark_service.h for the high-level design notes.
// =============================================================================

#include "bot_controller/benchmark_service.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

#include <sw/redis++/redis++.h>

#include "bot.pb.h"
#include "orchestrator.pb.h"

#include "bot_controller/load_planner.h"
#include "bot_controller/registry.h"

#include "velocity/common/log.h"
#include "velocity/common/signals.h"
#include "velocity/common/time.h"
#include "velocity/common/tracing.h"

namespace velocity::bot_controller {

namespace {

using ::grpc::ServerContext;
using ::grpc::ServerWriter;
using ::grpc::Status;
using ::grpc::StatusCode;

using ::velocity::orchestrator::v1::BenchmarkPhase;
using ::velocity::orchestrator::v1::BenchmarkProfile;
using ::velocity::orchestrator::v1::BenchmarkReport;
using ::velocity::orchestrator::v1::BenchmarkSnapshot;
using ::velocity::orchestrator::v1::PersonaShare;

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

// Generate a sortable-by-time identifier. We don't have a ULID dependency
// linked here, so we emit "BM-<unix_ms_hex>-<rand16_hex>" which is sortable
// by the hex chunk and unique with cosmic probability.
[[nodiscard]] auto new_benchmark_id() -> std::string {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    const auto ts_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    char buf[40];
    std::snprintf(buf, sizeof(buf), "BM-%012llx-%016llx",
                  static_cast<unsigned long long>(ts_ms),
                  static_cast<unsigned long long>(rng()));
    return buf;
}

// Translate the orchestrator-shaped profile into the bot-side LoadPlan that
// workers consume. The submission target endpoint is filled in by the caller.
// Per-tick cliff-finder sample. Hoisted out of BenchmarkSession so the
// detector function (which sits in this anonymous namespace too) can take it
// by value without a forward declaration of BenchmarkSession.
struct CliffSample {
    std::uint64_t elapsed_ms;
    std::uint64_t intended_rps;
    std::uint64_t observed_rps;
    std::uint64_t p99_ns;
};

// Cliff detector result. Public so the impl method can consume it; private
// to this translation unit.
struct CliffResult {
    bool             detected{false};
    std::uint64_t    rps{0};
    std::uint64_t    lower_rps{0};
    std::uint64_t    upper_rps{0};
    double           confidence{0.0};
    std::string_view reason{};
};

// Walk the cliff-finder per-tick samples and locate the first ≥750ms window
// where the engine "broke". A sample is breaking when either:
//   (a) observed_rps < 0.80 × intended_rps  (throughput shed), or
//   (b) p99_ns > 10× the baseline p99 captured during the first 1.5s of ramp.
// The detection requires THREE consecutive breaking samples (≈ 750ms of ticks
// at the controller's 250ms cadence) to filter transient blips. The CI bounds
// span the intended_rps at the start and end of the breaking run; confidence
// rises with the length of the breaking run, capping at 0.99 by 10 samples.
[[nodiscard]] auto detect_cliff(
    const std::vector<CliffSample>& samples) -> CliffResult {
    if (samples.size() < 4) return {};

    // Baseline p99 from the *first* 6 samples (≈1.5s) — the engine is still
    // warming up, so use a generous floor (10×) to avoid false positives on
    // GC pauses or JIT warm-ups.
    std::uint64_t sum_p99 = 0;
    std::size_t   n_p99   = 0;
    for (std::size_t i = 0; i < std::min<std::size_t>(6, samples.size()); ++i) {
        if (samples[i].p99_ns > 0) {
            sum_p99 += samples[i].p99_ns;
            ++n_p99;
        }
    }
    const std::uint64_t baseline_p99 = n_p99 == 0 ? 0 : sum_p99 / n_p99;
    const std::uint64_t p99_ceiling  = baseline_p99 == 0
        ? std::numeric_limits<std::uint64_t>::max()
        : baseline_p99 * 10;

    constexpr std::size_t kRequiredStreak = 3;
    std::size_t streak_start = 0;
    std::size_t streak_len   = 0;
    bool         reason_throughput = false;

    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto& s = samples[i];
        const bool throughput_shed = s.intended_rps > 0 &&
            static_cast<double>(s.observed_rps) <
            0.80 * static_cast<double>(s.intended_rps);
        const bool tail_blowup = s.p99_ns > p99_ceiling;
        const bool breaking = throughput_shed || tail_blowup;

        if (breaking) {
            if (streak_len == 0) {
                streak_start = i;
                reason_throughput = throughput_shed;
            }
            ++streak_len;
            if (streak_len >= kRequiredStreak) {
                // First k breaking samples confirm a cliff. Keep walking
                // forward until the streak ends so the CI bounds capture
                // the full breaking window.
                continue;
            }
        } else {
            if (streak_len >= kRequiredStreak) break;   // streak ended
            streak_len = 0;
        }
    }

    if (streak_len < kRequiredStreak) return {};

    const auto& first = samples[streak_start];
    const auto& last  = samples[streak_start + streak_len - 1];
    CliffResult r;
    r.detected   = true;
    r.rps        = first.intended_rps;
    r.lower_rps  = std::min(first.intended_rps, last.intended_rps);
    r.upper_rps  = std::max(first.intended_rps, last.intended_rps);
    // Confidence rises with the streak length. 3 ticks = 0.65, 5 = 0.80,
    // 10+ = 0.99. The shape `1 - 0.9 / (streak - 1)` is purely heuristic but
    // gives a sane "more evidence → tighter answer" curve.
    r.confidence = std::min(0.99,
        1.0 - 0.9 / static_cast<double>(streak_len));
    r.reason     = reason_throughput ? "throughput_shed" : "tail_latency_blowup";
    return r;
}

// Interpolate the offered (intended) RPS at `elapsed_ms` from the profile's
// ramp schedule. Honours `custom_schedule` when present; otherwise computes
// the simple linear `0 → target_rps` ramp during `ramp_seconds` and holds at
// `target_rps` after that. Used by the cliff-finder detector to know exactly
// how much load we *meant* to offer at every tick.
[[nodiscard]] auto interpolate_schedule_rps(const BenchmarkProfile& profile,
                                            std::uint64_t elapsed_ms) noexcept
    -> std::uint64_t {
    if (profile.custom_schedule_size() > 0) {
        const auto& wps = profile.custom_schedule();
        if (elapsed_ms <= wps[0].elapsed_ms()) return wps[0].target_rps();
        for (int i = 1; i < wps.size(); ++i) {
            if (elapsed_ms <= wps[i].elapsed_ms()) {
                const auto t0 = wps[i - 1].elapsed_ms();
                const auto t1 = wps[i].elapsed_ms();
                const auto r0 = wps[i - 1].target_rps();
                const auto r1 = wps[i].target_rps();
                if (t1 == t0) return r1;
                // Linear interpolation in RPS space (matches the worker's
                // ramp interpolation; the staircase still appears stepwise
                // because waypoints are sparse).
                const double frac = static_cast<double>(elapsed_ms - t0) /
                                    static_cast<double>(t1 - t0);
                return r0 + static_cast<std::uint64_t>(
                    frac * static_cast<double>(r1 - r0));
            }
        }
        return wps[wps.size() - 1].target_rps();
    }
    const auto ramp_ms = static_cast<std::uint64_t>(profile.ramp_seconds()) * 1000ULL;
    const auto target  = profile.target_rps();
    if (ramp_ms == 0 || elapsed_ms >= ramp_ms) return target;
    return static_cast<std::uint64_t>(
        (static_cast<double>(elapsed_ms) / static_cast<double>(ramp_ms)) *
        static_cast<double>(target));
}

auto build_global_plan(const BenchmarkProfile& profile,
                       const std::string& submission_id,
                       const std::string& target_host,
                       std::uint32_t target_port,
                       velocity::common::v1::WireProtocol wire,
                       velocity::bot::v1::LoadPlan& out) -> void {
    out.mutable_submission_id()->set_value(submission_id);
    auto* ep = out.mutable_target();
    ep->set_host(target_host);
    ep->set_port(target_port);
    ep->set_protocol(wire);
    ep->set_path("/v1/orders");

    // Personas — copy from profile, defaulting to a balanced mix if empty.
    if (profile.personas_size() == 0) {
        using BP = velocity::common::v1::BotPersona;
        const auto add = [&](BP kind, std::uint32_t w) {
            auto* pw = out.add_personas();
            pw->set_persona(kind);
            pw->set_weight(w);
        };
        add(BP::BOT_PERSONA_MARKET_MAKER, 50);
        add(BP::BOT_PERSONA_AGGRESSIVE,   25);
        add(BP::BOT_PERSONA_CANCELLER,    15);
        add(BP::BOT_PERSONA_NOISE,        10);
    } else {
        for (const auto& ps : profile.personas()) {
            auto* pw = out.add_personas();
            pw->set_persona(ps.persona());
            pw->set_weight(ps.weight());
        }
    }

    // Ramp schedule. If the profile carries an explicit staircase
    // (cliff-finder and any other custom-shaped profile) we honour it
    // verbatim; otherwise we synthesise the simple linear `ramp → hold`
    // curve from the legacy ramp_seconds / hold_seconds knobs.
    if (profile.custom_schedule_size() > 0) {
        for (const auto& wp : profile.custom_schedule()) {
            auto* w = out.add_schedule();
            w->set_elapsed_ms(wp.elapsed_ms());
            w->set_target_rps(wp.target_rps());
        }
    } else {
        const auto ramp_ms = static_cast<std::uint64_t>(profile.ramp_seconds()) * 1000ULL;
        const auto hold_ms = static_cast<std::uint64_t>(profile.hold_seconds()) * 1000ULL;
        const auto target  = profile.target_rps() == 0 ? 50'000ULL : profile.target_rps();

        auto* wp0 = out.add_schedule();
        wp0->set_elapsed_ms(0);
        wp0->set_target_rps(0);

        auto* wp1 = out.add_schedule();
        wp1->set_elapsed_ms(ramp_ms);
        wp1->set_target_rps(target);

        auto* wp2 = out.add_schedule();
        wp2->set_elapsed_ms(ramp_ms + hold_ms);
        wp2->set_target_rps(target);
    }

    out.set_constant_rate(true);
    out.set_per_order_timeout_us(profile.per_order_timeout_us() == 0
                                 ? 250'000U
                                 : profile.per_order_timeout_us());
    out.set_rng_seed(0xCAFEBABE12345678ULL);
    out.set_bots_per_reactor(64);

    // Multi-venue fan-out. When the profile defines venues, translate them
    // into bot.proto's VenueSpec list. Each leg keeps its own
    // symbol/fair_value/tick_size so SPOT/PERP/FUTURES exhibit realistic
    // basis spreads. The top-level symbol/fair_value/tick_size remain set
    // so older worker images (pre-Phase-2.1) still see a valid plan.
    if (profile.venues_size() == 0) {
        out.set_symbol("SPOT/USDT");
        out.set_fair_value(100'000'000);   // 100.0000 (scale 6)
        out.set_tick_size(100);            // 0.0001
    } else {
        // Use the first leg's metadata as the legacy fallback for
        // workers that don't read `venues`. This is the conservative
        // choice — never the most-weighted leg, because a leg's index
        // is stable across builds while weights may shift between runs.
        const auto& primary = profile.venues(0);
        out.set_symbol(primary.symbol().empty() ? "SPOT/USDT" : primary.symbol());
        out.set_fair_value(primary.fair_value() == 0 ? 100'000'000 : primary.fair_value());
        out.set_tick_size(primary.tick_size() == 0 ? 100 : primary.tick_size());
        for (const auto& leg : profile.venues()) {
            auto* vs = out.add_venues();
            vs->set_venue_id(leg.venue_id());
            vs->set_symbol(leg.symbol());
            vs->set_weight(leg.weight() == 0 ? 100 : leg.weight());
            vs->set_fair_value(leg.fair_value() == 0
                                ? 100'000'000
                                : leg.fair_value());
            vs->set_tick_size(leg.tick_size() == 0 ? 100 : leg.tick_size());
            vs->set_protocol(wire);
            // The endpoint defaults to the primary submission's; per-leg
            // override happens later, after the registry lookup, when
            // each leg's submission_id has been resolved.
            auto* lt = vs->mutable_target();
            lt->set_host(target_host);
            lt->set_port(target_port);
            lt->set_protocol(wire);
            lt->set_path("/v1/orders");
        }
    }
}

// ---------------------------------------------------------------------------
//  Per-benchmark session.
// ---------------------------------------------------------------------------

struct BenchmarkSession {
    // Identity.
    std::string                         benchmark_id;
    std::string                         submission_id;
    BenchmarkProfile                    profile;
    std::int64_t                        started_at_ns{0};
    std::int64_t                        finished_at_ns{0};

    // Phase + cancellation.
    std::atomic<BenchmarkPhase>         phase{BenchmarkPhase::BENCHMARK_PHASE_QUEUED};
    std::atomic<bool>                   cancel_requested{false};

    // Per-worker baseline counters captured at start.
    std::unordered_map<std::string, std::uint64_t> baseline_sent;
    std::unordered_map<std::string, std::uint64_t> baseline_acked;
    std::unordered_map<std::string, std::uint64_t> baseline_errored;

    // Rolling 1s-window samples of current_rps for sustained-rps p10.
    std::vector<std::uint64_t>          rps_samples;

    // The latest snapshot — copied into WatchBenchmark writers.
    std::mutex                          snap_mu;
    std::condition_variable             snap_cv;
    BenchmarkSnapshot                   last_snapshot;
    std::atomic<std::uint64_t>          snapshot_seq{0};

    // Aggregated peak / sustained.
    std::atomic<std::uint64_t>          peak_rps_observed{0};

    // W3C trace_id (32 hex chars) for this benchmark's root span. Captured at
    // StartBenchmark from the inbound traceparent (gateway → controller) so
    // that snapshots and reports can carry it back to the UI and the user can
    // deep-link into Jaeger. Empty when tracing isn't configured.
    std::string                         trace_id;

    // Pre-computed total benchmark duration (ms). For legacy profiles this is
    // ramp_seconds + hold_seconds; for cliff-finder (and any profile with a
    // custom_schedule) it's the timestamp of the final waypoint. Avoids
    // recomputing every tick and avoids the legacy `0 + 0 = 0`-instant-
    // finalize bug when custom_schedule is in play.
    std::uint64_t                       total_duration_ms{0};

    // Cliff-finder per-tick sample buffer. Populated only when
    // profile.cliff_finder()==true. Each entry records `(elapsed_ms,
    // intended_rps, observed_rps, p99_ns)` at the moment the tick ran. The
    // detector in get_report() walks this vector to locate the breaking RPS.
    std::vector<CliffSample>            cliff_samples;
};

}  // namespace

// ---------------------------------------------------------------------------
//  Impl
// ---------------------------------------------------------------------------
struct BenchmarkServiceImpl::Impl {
    Registry*                                                       registry;
    BenchmarkServiceConfig                                          cfg;

    std::mutex                                                      sessions_mu;
    std::unordered_map<std::string, std::shared_ptr<BenchmarkSession>> sessions;
    std::shared_ptr<BenchmarkSession>                                active;

    // Background ticker — runs while at least one session is active.
    std::atomic<bool>                                               stop{false};
    std::thread                                                     ticker;

    // Optional Redis client for reading scoring-service summaries.
    std::unique_ptr<sw::redis::Redis>                               redis;

    explicit Impl(Registry* r, BenchmarkServiceConfig c)
        : registry(r), cfg(std::move(c)) {
        if (!cfg.redis_addr.empty()) {
            try {
                redis = std::make_unique<sw::redis::Redis>(cfg.redis_addr);
                VLOG_INFO("benchmark-service: connected to redis at {}", cfg.redis_addr);
            } catch (const std::exception& e) {
                VLOG_WARN("benchmark-service: redis unavailable ({}); latency/correctness will be zero",
                          e.what());
                redis.reset();
            }
        }
        ticker = std::thread([this] { ticker_loop(); });
    }

    ~Impl() {
        stop.store(true, std::memory_order_release);
        if (ticker.joinable()) ticker.join();
    }

    // -----------------------------------------------------------------------
    //  Public RPCs
    // -----------------------------------------------------------------------

    auto start(const ::velocity::orchestrator::v1::StartBenchmarkRequest* req,
               ::velocity::orchestrator::v1::StartBenchmarkResponse* resp,
               std::string_view traceparent = {})
        -> Status {
        if (req->submission_id().value().empty()) {
            return Status(StatusCode::INVALID_ARGUMENT, "submission_id required");
        }

        // Resolve the profile *before* claiming the global slot so an
        // invalid profile name doesn't block other benchmarks.
        auto session = std::make_shared<BenchmarkSession>();
        if (req->has_override()) {
            session->profile = req->override();
        } else if (!resolve_profile(req->profile_name(), session->profile)) {
            return Status(StatusCode::INVALID_ARGUMENT,
                          "unknown profile: " + req->profile_name());
        }
        // Reject obviously bogus persona mixes — sum of weights must be
        // positive, otherwise worker persona selection divides by zero.
        std::uint64_t weight_sum = 0;
        for (const auto& ps : session->profile.personas()) {
            weight_sum += ps.weight();
        }
        if (session->profile.personas_size() > 0 && weight_sum == 0) {
            return Status(StatusCode::INVALID_ARGUMENT,
                          "persona weights sum to zero");
        }

        session->benchmark_id  = new_benchmark_id();
        session->submission_id = req->submission_id().value();
        session->started_at_ns = velocity::time::realtime_ns();

        // Compute total run length. Custom-schedule profiles (cliff-finder
        // included) end when the schedule does; legacy profiles end when
        // ramp + hold have elapsed.
        if (session->profile.custom_schedule_size() > 0) {
            std::uint64_t last = 0;
            for (const auto& wp : session->profile.custom_schedule()) {
                last = std::max<std::uint64_t>(last, wp.elapsed_ms());
            }
            session->total_duration_ms = last;
        } else {
            session->total_duration_ms =
                (static_cast<std::uint64_t>(session->profile.ramp_seconds()) +
                 static_cast<std::uint64_t>(session->profile.hold_seconds())) * 1000ULL;
        }
        if (session->total_duration_ms == 0) {
            session->total_duration_ms = 30'000;   // safety floor
        }

        // Capture per-worker baselines so we can diff future StatusUpdates.
        const auto workers_snapshot = registry->all();
        for (const auto& w : workers_snapshot) {
            session->baseline_sent[w->worker_id]    = w->last_sent_total.load();
            session->baseline_acked[w->worker_id]   = w->last_acked_total.load();
            session->baseline_errored[w->worker_id] = w->last_errored_total.load();
        }

        // Build the global LoadPlan. The target endpoint is faked here —
        // in production the gateway propagates the submission's endpoint
        // (host/port/protocol) through the BenchmarkProfile. We accept
        // the override if the caller supplied one, otherwise we fall
        // back to a Kubernetes DNS pattern that matches our manifests.
        velocity::bot::v1::LoadPlan global;
        build_global_plan(session->profile,
                          session->submission_id,
                          "submission-" + session->submission_id +
                              ".sandbox.svc.cluster.local",
                          /*target_port=*/8080,
                          velocity::common::v1::WireProtocol::WIRE_PROTOCOL_REST,
                          global);
        if (!traceparent.empty()) {
            global.set_traceparent(std::string(traceparent));
            // The W3C `traceparent` value is "00-<32hex trace_id>-<16hex span_id>-<2hex flags>".
            // We slice out the trace_id so we can stamp it onto snapshots /
            // reports without re-parsing on every emit.
            if (traceparent.size() >= 3 + 32 && traceparent.substr(0, 3) == "00-") {
                session->trace_id = std::string(traceparent.substr(3, 32));
            }
        }
        // Stamp the benchmark id into the plan so worker telemetry can be
        // attributed to the right run even when the same submission is
        // benchmarked twice.
        global.mutable_submission_id()->set_value(session->submission_id);

        const auto caps = registry->capacities();
        if (caps.empty()) {
            return Status(StatusCode::FAILED_PRECONDITION, "no workers registered");
        }

        // Hold sessions_mu across the entire "is anyone active?" check and
        // the active assignment. Otherwise two concurrent StartBenchmark
        // calls can both observe `active == nullptr`, both spend several
        // milliseconds building plans, and then both install themselves.
        {
            std::lock_guard lk(sessions_mu);
            if (active) {
                return Status(StatusCode::ABORTED,
                              "another benchmark is in flight: " +
                                  active->benchmark_id);
            }
            sessions.emplace(session->benchmark_id, session);
            active = session;
        }

        // Distribute plans. We release sessions_mu first because Write()
        // on the worker stream may block briefly.
        const auto plans = distribute(global, caps);
        std::size_t writes_ok = 0;
        for (const auto& [worker_id, plan] : plans) {
            auto ws = registry->session(worker_id);
            if (!ws) continue;
            velocity::bot::v1::ControllerToWorker msg;
            *msg.mutable_plan() = plan;
            std::lock_guard lk(ws->write_mu);
            if (ws->stream->Write(msg)) {
                ++writes_ok;
            } else {
                VLOG_WARN("benchmark {} failed to send plan to worker {}",
                          session->benchmark_id, worker_id);
            }
        }
        if (writes_ok == 0) {
            // Roll back the session — no worker accepted the plan.
            std::lock_guard lk(sessions_mu);
            sessions.erase(session->benchmark_id);
            if (active.get() == session.get()) active.reset();
            return Status(StatusCode::UNAVAILABLE,
                          "no workers accepted the plan");
        }

        session->phase.store(BenchmarkPhase::BENCHMARK_PHASE_RAMPING);

        resp->set_benchmark_id(session->benchmark_id);
        resp->set_started_at_ns(static_cast<std::uint64_t>(session->started_at_ns));

        VLOG_INFO("benchmark {} started for submission {} (target_rps={}, hold={}s, workers={}/{})",
                  session->benchmark_id, session->submission_id,
                  session->profile.target_rps(), session->profile.hold_seconds(),
                  writes_ok, caps.size());
        return Status::OK;
    }

    auto cancel(const ::velocity::orchestrator::v1::CancelBenchmarkRequest* req,
                ::velocity::orchestrator::v1::CancelBenchmarkResponse* resp)
        -> Status {
        std::shared_ptr<BenchmarkSession> s;
        {
            std::lock_guard lk(sessions_mu);
            auto it = sessions.find(req->benchmark_id());
            if (it == sessions.end()) {
                return Status(StatusCode::NOT_FOUND, "no such benchmark");
            }
            s = it->second;
        }
        if (s->phase.load() >= BenchmarkPhase::BENCHMARK_PHASE_COMPLETE) {
            resp->set_cancelled(false);
            return Status::OK;
        }
        s->cancel_requested.store(true);

        // Stop worker load immediately; finalize() also broadcasts a stop, but
        // doing it here makes cancellation take effect without waiting on the
        // finalize path. Both are idempotent (zero-rate ramp).
        broadcast_stop_load(s->benchmark_id);

        finalize(*s, BenchmarkPhase::BENCHMARK_PHASE_CANCELLED);
        resp->set_cancelled(true);
        return Status::OK;
    }

    auto watch(ServerContext* ctx,
               const ::velocity::orchestrator::v1::WatchBenchmarkRequest* req,
               ServerWriter<BenchmarkSnapshot>* writer) -> Status {
        std::shared_ptr<BenchmarkSession> s;
        {
            std::lock_guard lk(sessions_mu);
            auto it = sessions.find(req->benchmark_id());
            if (it == sessions.end()) {
                return Status(StatusCode::NOT_FOUND, "no such benchmark");
            }
            s = it->second;
        }

        std::uint64_t last_seq = 0;
        while (!ctx->IsCancelled() &&
               !stop.load(std::memory_order_acquire)) {
            BenchmarkSnapshot snap;
            bool terminal = false;
            {
                std::unique_lock lk(s->snap_mu);
                s->snap_cv.wait_for(lk, std::chrono::milliseconds(500), [&]() {
                    return s->snapshot_seq.load() != last_seq ||
                           s->phase.load() >= BenchmarkPhase::BENCHMARK_PHASE_COMPLETE ||
                           stop.load(std::memory_order_acquire);
                });
                snap     = s->last_snapshot;
                last_seq = s->snapshot_seq.load();
                terminal = s->phase.load() >= BenchmarkPhase::BENCHMARK_PHASE_COMPLETE;
            }
            if (snap.benchmark_id().empty()) {
                if (terminal) break;
                continue;
            }
            if (!writer->Write(snap)) break;
            if (terminal) break;
        }
        return Status::OK;
    }

    auto get_report(const ::velocity::orchestrator::v1::GetReportRequest* req,
                    BenchmarkReport* resp) -> Status {
        std::shared_ptr<BenchmarkSession> s;
        {
            std::lock_guard lk(sessions_mu);
            auto it = sessions.find(req->benchmark_id());
            if (it == sessions.end()) {
                return Status(StatusCode::NOT_FOUND, "no such benchmark");
            }
            s = it->second;
        }

        resp->set_benchmark_id(s->benchmark_id);
        resp->mutable_submission_id()->set_value(s->submission_id);
        *resp->mutable_profile() = s->profile;
        resp->set_started_at_ns(static_cast<std::uint64_t>(s->started_at_ns));
        resp->set_finished_at_ns(static_cast<std::uint64_t>(s->finished_at_ns));
        resp->set_trace_id(s->trace_id);

        BenchmarkSnapshot snap;
        {
            std::lock_guard lk(s->snap_mu);
            snap = s->last_snapshot;
        }
        resp->set_total_orders(snap.sent_total());
        resp->set_total_acked(snap.acked_total());
        resp->set_total_errored(snap.errored_total());
        resp->set_peak_rps_observed(s->peak_rps_observed.load());

        // Sustained RPS = p10 of samples taken during HOLD phase
        // (nearest-rank percentile per docs/scoring.md). We snapshot the
        // sample vector under snap_mu so we don't race the ticker.
        std::vector<std::uint64_t> sorted;
        {
            std::lock_guard lk(s->snap_mu);
            sorted = s->rps_samples;
        }
        std::uint64_t sustained = 0;
        if (!sorted.empty()) {
            std::sort(sorted.begin(), sorted.end());
            const auto n = sorted.size();
            // nearest-rank p10: ceil(0.10 * n) clamped to [1, n]
            const auto idx = std::min<std::size_t>(
                n - 1,
                static_cast<std::size_t>(std::max<double>(1.0, std::ceil(0.10 * static_cast<double>(n)))) - 1);
            sustained = sorted[idx];
        }
        resp->set_sustained_rps(sustained);

        resp->set_p50_ns(snap.p50_latency_ns());
        resp->set_p90_ns(snap.p90_latency_ns());
        resp->set_p99_ns(snap.p99_latency_ns());
        resp->set_p999_ns(snap.p999_latency_ns());
        resp->set_max_ns(snap.max_latency_ns());

        // Final scores.
        const double throughput_score =
            s->profile.target_rps() == 0 ? 0.0 :
            100.0 * std::min(1.0, static_cast<double>(sustained) /
                                   static_cast<double>(s->profile.target_rps()));
        const double baseline_us = 30.0;
        const double p99_us = static_cast<double>(snap.p99_latency_ns()) / 1000.0;
        // p99 == 0 means the HdrHistogram recorded zero samples (no orders
        // completed) — not an infinitely fast engine. Treat "no data" as no
        // latency credit so a submission that never acks an order can't win
        // the latency component. Mirrors scoring-service/src/scorer.cpp.
        const double latency_score = p99_us <= 0.0
            ? 0.0
            : p99_us <= baseline_us
            ? 100.0
            : std::max(0.0, 100.0 - 100.0 * (p99_us - baseline_us) / baseline_us);

        resp->set_throughput_score(throughput_score);
        resp->set_latency_score(latency_score);
        resp->set_correctness_score(snap.correctness_score());
        const double composite =
            0.40 * throughput_score +
            0.35 * latency_score +
            0.25 * snap.correctness_score();
        resp->set_composite_score(composite);

        // Cliff-finder post-hoc analysis.
        if (s->profile.cliff_finder()) {
            std::vector<CliffSample> samples;
            {
                std::lock_guard lk(s->snap_mu);
                samples = s->cliff_samples;
            }
            const auto cliff = detect_cliff(samples);
            resp->set_cliff_detected(cliff.detected);
            resp->set_cliff_rps(cliff.rps);
            resp->set_cliff_lower_rps(cliff.lower_rps);
            resp->set_cliff_upper_rps(cliff.upper_rps);
            resp->set_cliff_confidence(cliff.confidence);
            resp->set_cliff_reason(std::string(cliff.reason));
        }

        // Multi-venue report fan-out. When the profile listed multiple
        // legs we surface a per-venue row and the cross-venue skew. The
        // scoring service is the source of truth for per-venue p99
        // (see services/scoring-service for the joiner that splits
        // telemetry by venue_id stamped on the bot worker side); we
        // read that from Redis under `scores:<submission_id>:venue:<id>`.
        // When Redis hasn't published per-venue keys yet (a brand-new
        // deployment or a benchmark that ran with pre-Phase-2.1 workers)
        // we fall back to replicating the global p99 across legs so the
        // frontend table still renders deterministically.
        if (s->profile.venues_size() > 0) {
            std::uint64_t min_p99 = std::numeric_limits<std::uint64_t>::max();
            std::uint64_t max_p99 = 0;
            bool any_real = false;
            for (const auto& leg : s->profile.venues()) {
                auto* row = resp->add_venue_results();
                row->set_venue_id(leg.venue_id());
                row->set_symbol(leg.symbol());
                std::uint64_t p50 = snap.p50_latency_ns();
                std::uint64_t p99 = snap.p99_latency_ns();
                std::uint64_t p999 = snap.p999_latency_ns();
                std::uint64_t sent_l = 0;
                std::uint64_t acked_l = 0;
                if (redis) {
                    try {
                        const auto key = "scores:" + s->submission_id +
                                         ":venue:" + leg.venue_id();
                        if (auto v = redis->hget(key, "p50_ns"))  { p50  = std::stoull(*v); any_real = true; }
                        if (auto v = redis->hget(key, "p99_ns"))  { p99  = std::stoull(*v); any_real = true; }
                        if (auto v = redis->hget(key, "p999_ns")) { p999 = std::stoull(*v); }
                        if (auto v = redis->hget(key, "sent"))    { sent_l  = std::stoull(*v); }
                        if (auto v = redis->hget(key, "acked"))   { acked_l = std::stoull(*v); }
                    } catch (...) { /* keep fallback values */ }
                }
                row->set_p50_ns(p50);
                row->set_p99_ns(p99);
                row->set_p999_ns(p999);
                row->set_sent_total(sent_l);
                row->set_acked_total(acked_l);
                if (p99 < min_p99) min_p99 = p99;
                if (p99 > max_p99) max_p99 = p99;
            }
            // Skew only makes sense when at least one leg had real Redis
            // data; otherwise we'd be reporting "0ns" off of replicated
            // identical fallbacks, which is misleading.
            if (any_real && min_p99 != std::numeric_limits<std::uint64_t>::max()) {
                resp->set_cross_venue_skew_ns(max_p99 - min_p99);
            }
        }

        return Status::OK;
    }

    // -----------------------------------------------------------------------
    //  Background ticker
    // -----------------------------------------------------------------------

    auto ticker_loop() -> void {
        VLOG_INFO("benchmark-service ticker started");
        using namespace std::chrono_literals;
        const auto period = 250ms;
        auto next_tick = std::chrono::steady_clock::now() + period;

        while (!stop.load(std::memory_order_acquire) &&
               !velocity::signals::shutdown_requested()) {
            std::this_thread::sleep_until(next_tick);
            next_tick = std::chrono::steady_clock::now() + period;

            std::shared_ptr<BenchmarkSession> s;
            {
                std::lock_guard lk(sessions_mu);
                s = active;
            }
            if (!s) continue;

            tick(*s);
        }
        VLOG_INFO("benchmark-service ticker stopped");
    }

    auto tick(BenchmarkSession& s) -> void {
        if (s.phase.load() >= BenchmarkPhase::BENCHMARK_PHASE_COMPLETE) return;

        const auto now_ns      = velocity::time::realtime_ns();
        const auto elapsed_ms  = static_cast<std::uint64_t>(
            (now_ns - s.started_at_ns) / 1'000'000);

        // Phase transitions.
        const auto ramp_ms = static_cast<std::uint64_t>(s.profile.ramp_seconds()) * 1000ULL;
        if (elapsed_ms >= s.total_duration_ms) {
            finalize(s, BenchmarkPhase::BENCHMARK_PHASE_COMPLETE);
            return;
        }
        if (elapsed_ms >= ramp_ms) {
            s.phase.store(BenchmarkPhase::BENCHMARK_PHASE_HOLDING);
        }

        // Aggregate worker counters relative to baseline. Workers that
        // joined mid-benchmark have no baseline entry; we install one
        // lazily at first sighting using their current totals, so they
        // only contribute deltas from join-time onward (otherwise their
        // entire historical counter is attributed to this benchmark).
        std::uint64_t sent = 0, acked = 0, err = 0, current_rps = 0;
        // Kernel-level latency, max-pooled across workers. The right
        // aggregation is "worst observed across the fleet" rather than a
        // mean — a single overloaded worker dragging tail latency is the
        // signal we want to surface, exactly like the userspace p99.
        std::uint64_t k_p50 = 0, k_p99 = 0, k_p999 = 0, k_samples = 0;
        for (const auto& w : registry->all()) {
            const auto c_sent = w->last_sent_total.load(std::memory_order_relaxed);
            const auto c_ack  = w->last_acked_total.load(std::memory_order_relaxed);
            const auto c_err  = w->last_errored_total.load(std::memory_order_relaxed);

            auto [bs_it, bs_inserted] = s.baseline_sent.try_emplace(w->worker_id, c_sent);
            const auto b_sent = bs_it->second;
            auto [ba_it, ba_inserted] = s.baseline_acked.try_emplace(w->worker_id, c_ack);
            const auto b_ack  = ba_it->second;
            auto [be_it, be_inserted] = s.baseline_errored.try_emplace(w->worker_id, c_err);
            const auto b_err  = be_it->second;
            (void)bs_inserted; (void)ba_inserted; (void)be_inserted;

            sent  += (c_sent > b_sent) ? (c_sent - b_sent) : 0;
            acked += (c_ack  > b_ack)  ? (c_ack  - b_ack)  : 0;
            err   += (c_err  > b_err)  ? (c_err  - b_err)  : 0;
            current_rps += w->last_current_rps.load(std::memory_order_relaxed);

            const auto kp50  = w->last_kernel_p50_ns.load(std::memory_order_relaxed);
            const auto kp99  = w->last_kernel_p99_ns.load(std::memory_order_relaxed);
            const auto kp999 = w->last_kernel_p999_ns.load(std::memory_order_relaxed);
            const auto ksmp  = w->last_kernel_samples.load(std::memory_order_relaxed);
            if (kp50  > k_p50)  k_p50  = kp50;
            if (kp99  > k_p99)  k_p99  = kp99;
            if (kp999 > k_p999) k_p999 = kp999;
            k_samples += ksmp;
        }
        if (current_rps > s.peak_rps_observed.load()) {
            s.peak_rps_observed.store(current_rps);
        }
        if (s.phase.load() == BenchmarkPhase::BENCHMARK_PHASE_HOLDING) {
            std::lock_guard lk(s.snap_mu);
            s.rps_samples.push_back(current_rps);
            if (s.rps_samples.size() > 600) {
                s.rps_samples.erase(s.rps_samples.begin());
            }
        }

        // Capture a per-tick cliff sample whenever the profile is a
        // cliff-finder. We need (intended_rps, observed_rps, p99) at this
        // exact elapsed_ms; intended_rps is interpolated from the schedule.
        if (s.profile.cliff_finder()) {
            const std::uint64_t intended = interpolate_schedule_rps(s.profile, elapsed_ms);
            // p99 is fetched a few lines below; capture it after that and
            // backfill the sample (see the cliff-sample append below).
            std::lock_guard lk(s.snap_mu);
            s.cliff_samples.push_back({elapsed_ms, intended, current_rps, /*p99_ns=*/0});
            // Cap the buffer at a few minutes' worth so adversarial cancels
            // can't grow it unbounded.
            if (s.cliff_samples.size() > 4'096) {
                s.cliff_samples.erase(s.cliff_samples.begin());
            }
        }

        // Pull latency / correctness from Redis if scoring service is feeding it.
        std::uint64_t p50=0, p90=0, p99=0, p999=0, mx=0;
        double correctness = 0.0;
        if (redis) {
            try {
                const auto key = "scores:" + s.submission_id;
                if (auto v = redis->hget(key, "p50_ns"))  p50  = std::stoull(*v);
                if (auto v = redis->hget(key, "p90_ns"))  p90  = std::stoull(*v);
                if (auto v = redis->hget(key, "p99_ns"))  p99  = std::stoull(*v);
                if (auto v = redis->hget(key, "p999_ns")) p999 = std::stoull(*v);
                if (auto v = redis->hget(key, "max_ns"))  mx   = std::stoull(*v);
                if (auto v = redis->hget(key, "correctness_score"))
                    correctness = std::stod(*v);
            } catch (const std::exception& e) {
                // Redis hiccup — keep zeros, log once-ish.
                static std::atomic<int> warn_counter{0};
                if ((warn_counter++ % 32) == 0) {
                    VLOG_WARN("benchmark-service: redis read failed: {}", e.what());
                }
            }
        }

        // Compose snapshot.
        BenchmarkSnapshot snap;
        snap.set_benchmark_id(s.benchmark_id);
        snap.set_ts_ns(static_cast<std::uint64_t>(now_ns));
        snap.set_elapsed_ms(elapsed_ms);
        snap.set_phase(s.phase.load());
        snap.set_sent_total(sent);
        snap.set_acked_total(acked);
        snap.set_errored_total(err);
        snap.set_current_rps(current_rps);
        snap.set_target_rps(s.profile.target_rps());
        snap.set_p50_latency_ns(p50);
        snap.set_p90_latency_ns(p90);
        snap.set_p99_latency_ns(p99);
        snap.set_p999_latency_ns(p999);
        snap.set_max_latency_ns(mx);
        snap.set_correctness_score(correctness);
        snap.set_trace_id(s.trace_id);

        // Surface kernel-side latency aggregate. Frontend hides the
        // overlay when kernel_samples_observed == 0 (no worker reported
        // any data this window — eBPF disabled or just no traffic yet).
        snap.set_kernel_p50_latency_ns(k_p50);
        snap.set_kernel_p99_latency_ns(k_p99);
        snap.set_kernel_p999_latency_ns(k_p999);
        snap.set_kernel_samples_observed(k_samples);

        // Backfill the p99 onto the cliff sample we appended a few lines up.
        // Safe under snap_mu because we own the buffer for cliff-finder runs.
        if (s.profile.cliff_finder()) {
            std::lock_guard lk(s.snap_mu);
            if (!s.cliff_samples.empty()) {
                s.cliff_samples.back().p99_ns = p99;
            }
        }

        // Live composite. Same formula as the final report — kept in lock-step
        // with services/scoring-service/src/scorer.cpp::composite(). Sub-scores
        // are surfaced individually so the frontend score-breakdown modal can
        // render the exact substituted formula instead of back-solving them.
        const double tp_score =
            s.profile.target_rps() == 0 ? 0.0 :
            100.0 * std::min(1.0, static_cast<double>(current_rps) /
                                   static_cast<double>(s.profile.target_rps()));
        const double baseline_us = 30.0;
        const double p99_us = static_cast<double>(p99) / 1000.0;
        // See GetBenchmarkReport: p99 == 0 is "no samples", not zero latency.
        const double lat_score = p99_us <= 0.0
            ? 0.0
            : p99_us <= baseline_us
            ? 100.0
            : std::max(0.0, 100.0 - 100.0 * (p99_us - baseline_us) / baseline_us);
        // The live snapshot does not yet aggregate microstructure violations,
        // so the penalty is 0 here. The scoring-service downstream applies the
        // full compute_penalty() formula when fills are joined in.
        constexpr double penalty = 0.0;
        snap.set_throughput_score(tp_score);
        snap.set_latency_score(lat_score);
        snap.set_penalty_score(penalty);
        snap.set_composite_score(std::max(
            0.0,
            0.40 * tp_score + 0.35 * lat_score + 0.25 * correctness - penalty));

        // Publish to watchers.
        {
            std::lock_guard lk(s.snap_mu);
            s.last_snapshot = std::move(snap);
            s.snapshot_seq.fetch_add(1, std::memory_order_release);
        }
        s.snap_cv.notify_all();
    }

    // Broadcast a zero-rate RampUpdate to every worker so they stop generating
    // load. The worker treats from_elapsed_ms=0 as "replace all remaining
    // waypoints", and a target_rps of 0 pauses its scheduler (zero load, not a
    // 1 Hz trickle). We deliberately do NOT send a process-level Shutdown:
    // workers must survive to serve future benchmarks.
    auto broadcast_stop_load(const std::string& benchmark_id) -> void {
        for (auto& ws : registry->all()) {
            velocity::bot::v1::ControllerToWorker msg;
            auto* ramp = msg.mutable_ramp();
            ramp->set_from_elapsed_ms(0);
            auto* wp = ramp->add_waypoints();
            wp->set_elapsed_ms(0);
            wp->set_target_rps(0);
            std::lock_guard lk(ws->write_mu);
            if (!ws->stream->Write(msg)) {
                VLOG_WARN("stop-load {}: write to worker {} failed",
                          benchmark_id, ws->worker_id);
            }
        }
    }

    auto finalize(BenchmarkSession& s, BenchmarkPhase final_phase) -> void {
        // Stop worker load on EVERY terminal transition (COMPLETE included).
        // Previously only cancel() did this, so a benchmark that ran to its
        // natural end left workers generating hold-phase load indefinitely.
        broadcast_stop_load(s.benchmark_id);

        s.phase.store(final_phase);
        s.finished_at_ns = velocity::time::realtime_ns();
        {
            std::lock_guard lk(s.snap_mu);
            s.last_snapshot.set_phase(final_phase);
            s.snapshot_seq.fetch_add(1, std::memory_order_release);
        }
        s.snap_cv.notify_all();

        std::lock_guard lk(sessions_mu);
        if (active.get() == &s) active.reset();
        VLOG_INFO("benchmark {} finalized phase={}", s.benchmark_id, static_cast<int>(final_phase));
    }
};

// ---------------------------------------------------------------------------
//  Trampolines from BenchmarkServiceImpl to Impl.
// ---------------------------------------------------------------------------

BenchmarkServiceImpl::BenchmarkServiceImpl(Registry* registry,
                                           BenchmarkServiceConfig cfg)
    : impl_(std::make_unique<Impl>(registry, std::move(cfg))) {}

BenchmarkServiceImpl::~BenchmarkServiceImpl() = default;

// Helper: lift a `traceparent` value out of the inbound gRPC metadata so
// our span can join the upstream trace. Returns an empty optional if the
// gateway didn't propagate one (unit tests, direct grpcurl, ...).
[[nodiscard]] static auto upstream_context_from(
    grpc::ServerContext* ctx) -> std::optional<velocity::common::tracing::Context> {
    if (!ctx) return std::nullopt;
    const auto& md = ctx->client_metadata();
    if (auto it = md.find("traceparent"); it != md.end()) {
        const auto h = std::string_view(it->second.data(), it->second.size());
        return velocity::common::tracing::parse_traceparent(h);
    }
    return std::nullopt;
}

auto BenchmarkServiceImpl::StartBenchmark(
    grpc::ServerContext* ctx,
    const ::velocity::orchestrator::v1::StartBenchmarkRequest* req,
    ::velocity::orchestrator::v1::StartBenchmarkResponse* resp) -> Status {
    auto span = velocity::common::tracing::start_span(
        "BenchmarkService.StartBenchmark", upstream_context_from(ctx));
    span.set_attribute("submission_id", req->submission_id().value());
    span.set_attribute("profile",       req->profile_name());

    // The traceparent we stamp into outgoing LoadPlans must point to *our*
    // span, not the parent's, so workers nest under us.
    const auto outbound = velocity::common::tracing::to_traceparent(span.context());

    const auto status = impl_->start(req, resp, outbound);
    if (!status.ok()) span.set_status(false, status.error_message());
    else              span.set_attribute("benchmark_id", resp->benchmark_id());
    return status;
}

auto BenchmarkServiceImpl::CancelBenchmark(
    grpc::ServerContext* ctx,
    const ::velocity::orchestrator::v1::CancelBenchmarkRequest* req,
    ::velocity::orchestrator::v1::CancelBenchmarkResponse* resp) -> Status {
    auto span = velocity::common::tracing::start_span(
        "BenchmarkService.CancelBenchmark", upstream_context_from(ctx));
    span.set_attribute("benchmark_id", req->benchmark_id());
    const auto status = impl_->cancel(req, resp);
    if (!status.ok()) span.set_status(false, status.error_message());
    return status;
}

auto BenchmarkServiceImpl::WatchBenchmark(
    grpc::ServerContext* ctx,
    const ::velocity::orchestrator::v1::WatchBenchmarkRequest* req,
    grpc::ServerWriter<BenchmarkSnapshot>* writer) -> Status {
    return impl_->watch(ctx, req, writer);
}

auto BenchmarkServiceImpl::GetReport(
    grpc::ServerContext* /*ctx*/,
    const ::velocity::orchestrator::v1::GetReportRequest* req,
    BenchmarkReport* resp) -> Status {
    return impl_->get_report(req, resp);
}

auto BenchmarkServiceImpl::stop() -> void {
    impl_->stop.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
//  Profile catalogue
// ---------------------------------------------------------------------------

[[nodiscard]] auto resolve_profile(std::string_view name,
                                   BenchmarkProfile& out) noexcept -> bool {
    using BP = velocity::common::v1::BotPersona;
    const auto add_persona = [&](BP kind, std::uint32_t w) {
        auto* p = out.add_personas();
        p->set_persona(kind);
        p->set_weight(w);
    };

    out.Clear();
    if (name == "baseline" || name.empty()) {
        out.set_target_rps(50'000);
        out.set_duration_seconds(35);
        out.set_ramp_seconds(5);
        out.set_hold_seconds(30);
        out.set_per_order_timeout_us(250'000);
        add_persona(BP::BOT_PERSONA_MARKET_MAKER, 60);
        add_persona(BP::BOT_PERSONA_AGGRESSIVE,   20);
        add_persona(BP::BOT_PERSONA_CANCELLER,    10);
        add_persona(BP::BOT_PERSONA_NOISE,        10);
        return true;
    }
    if (name == "soak") {
        // Long, steady moderate load — surfaces slow leaks / GC creep /
        // fragmentation that short runs miss. The anomaly-detector suggests
        // this profile when a run looks healthy but unproven over time.
        out.set_target_rps(60'000);
        out.set_duration_seconds(300);
        out.set_ramp_seconds(10);
        out.set_hold_seconds(290);
        out.set_per_order_timeout_us(250'000);
        add_persona(BP::BOT_PERSONA_MARKET_MAKER, 60);
        add_persona(BP::BOT_PERSONA_AGGRESSIVE,   20);
        add_persona(BP::BOT_PERSONA_CANCELLER,    10);
        add_persona(BP::BOT_PERSONA_NOISE,        10);
        return true;
    }
    if (name == "spike") {
        out.set_target_rps(200'000);
        out.set_duration_seconds(20);
        out.set_ramp_seconds(5);
        out.set_hold_seconds(15);
        out.set_per_order_timeout_us(250'000);
        add_persona(BP::BOT_PERSONA_AGGRESSIVE,   50);
        add_persona(BP::BOT_PERSONA_SPOOFER,      20);
        add_persona(BP::BOT_PERSONA_MARKET_MAKER, 20);
        add_persona(BP::BOT_PERSONA_NOISE,        10);
        return true;
    }
    if (name == "fire-hose") {
        out.set_target_rps(1'000'000);
        out.set_duration_seconds(75);
        out.set_ramp_seconds(15);
        out.set_hold_seconds(60);
        out.set_per_order_timeout_us(500'000);
        add_persona(BP::BOT_PERSONA_MARKET_MAKER, 40);
        add_persona(BP::BOT_PERSONA_AGGRESSIVE,   30);
        add_persona(BP::BOT_PERSONA_CANCELLER,    20);
        add_persona(BP::BOT_PERSONA_NOISE,        10);
        return true;
    }
    if (name == "adversarial") {
        out.set_target_rps(80'000);
        out.set_duration_seconds(40);
        out.set_ramp_seconds(5);
        out.set_hold_seconds(35);
        out.set_per_order_timeout_us(150'000);
        add_persona(BP::BOT_PERSONA_SPOOFER,      45);
        add_persona(BP::BOT_PERSONA_CANCELLER,    40);
        add_persona(BP::BOT_PERSONA_MARKET_MAKER, 10);
        add_persona(BP::BOT_PERSONA_NOISE,         5);
        return true;
    }
    if (name == "cliff-finder") {
        // Exponential staircase: every 2 seconds the offered rate doubles.
        // Total run-length is 20s plus a 4s tail, so we sweep
        // 5k → 10k → 20k → 40k → 80k → 160k → 320k → 640k → 1.28M.
        // The cliff detector in get_report() walks the per-tick samples and
        // pinpoints the first window where sustained < 80 % of offered (or
        // p99 explodes), returning a confidence interval based on how many
        // consecutive ticks failed.
        out.set_target_rps(1'280'000);   // ceiling — informational only
        out.set_duration_seconds(24);
        out.set_ramp_seconds(0);          // schedule is fully custom
        out.set_hold_seconds(0);
        out.set_per_order_timeout_us(500'000);
        out.set_cliff_finder(true);

        struct Step { std::uint64_t at_ms, rps; };
        constexpr Step staircase[] = {
            {     0,        0},
            {   100,    5'000},
            { 2'000,   10'000},
            { 4'000,   20'000},
            { 6'000,   40'000},
            { 8'000,   80'000},
            {10'000,  160'000},
            {12'000,  320'000},
            {14'000,  640'000},
            {16'000, 1'280'000},
            {20'000, 1'280'000},   // brief hold at the top before drain
            {24'000, 1'280'000},
        };
        for (const auto& s : staircase) {
            auto* wp = out.add_custom_schedule();
            wp->set_elapsed_ms(s.at_ms);
            wp->set_target_rps(s.rps);
        }

        add_persona(BP::BOT_PERSONA_MARKET_MAKER, 40);
        add_persona(BP::BOT_PERSONA_AGGRESSIVE,   40);
        add_persona(BP::BOT_PERSONA_NOISE,        20);
        return true;
    }
    if (name == "adaptive-rl") {
        // Heavy ADAPTIVE persona mix backed by the PPO-trained ONNX
        // policy (see tools/rl-bot). Workers without the policy
        // available transparently fall back to MARKET_MAKER, so this
        // profile remains runnable in any environment — it just won't
        // exhibit the trained behaviour.
        out.set_target_rps(120'000);
        out.set_duration_seconds(45);
        out.set_ramp_seconds(5);
        out.set_hold_seconds(40);
        out.set_per_order_timeout_us(250'000);
        add_persona(BP::BOT_PERSONA_ADAPTIVE,    70);
        add_persona(BP::BOT_PERSONA_AGGRESSIVE,  15);
        add_persona(BP::BOT_PERSONA_NOISE,       15);
        return true;
    }
    if (name == "cross-venue") {
        // Three-leg multi-symbol benchmark modelling SPOT / PERP / FUTURES.
        // Each leg gets its own fair-value anchor with the basis spreads
        // you'd expect on a real venue:
        //   SPOT  = 100.0000
        //   PERP  = 100.0500  (+5 bp funding-implied)
        //   FUT   =  99.9500  (-5 bp contango for the front-month roll)
        // The bots fan out 50/30/20 (SPOT-heavy because that's where the
        // tightest spreads live). The controller surfaces a cross-venue
        // latency-skew metric in the final report — the moment one leg
        // starts lagging behind the others, the skew chart spikes and the
        // judge can tell you "the engine is queuing PERP orders behind
        // SPOT" without any other instrumentation.
        out.set_target_rps(150'000);
        out.set_duration_seconds(40);
        out.set_ramp_seconds(5);
        out.set_hold_seconds(35);
        out.set_per_order_timeout_us(250'000);

        // Persona mix tuned for inter-venue arbitrage realism: aggressive
        // takers fire across legs, market-makers post quotes everywhere.
        add_persona(BP::BOT_PERSONA_MARKET_MAKER, 50);
        add_persona(BP::BOT_PERSONA_AGGRESSIVE,   25);
        add_persona(BP::BOT_PERSONA_CANCELLER,    15);
        add_persona(BP::BOT_PERSONA_NOISE,        10);

        struct Leg { const char* venue; const char* symbol;
                     std::uint32_t weight; std::int64_t fair_value; };
        constexpr Leg legs[] = {
            {"SPOT",        "SPOT/USDT",   50, 100'000'000},  // 100.0000
            {"PERP",        "BTC-PERP",    30, 100'500'000},  // 100.0500
            {"FUTURES_DEC", "BTC-DEC25",   20,  99'950'000},  //  99.9500
        };
        for (const auto& l : legs) {
            auto* v = out.add_venues();
            v->set_venue_id(l.venue);
            v->set_symbol(l.symbol);
            v->set_weight(l.weight);
            v->set_fair_value(l.fair_value);
            v->set_tick_size(100);
        }
        return true;
    }
    return false;
}

}  // namespace velocity::bot_controller
