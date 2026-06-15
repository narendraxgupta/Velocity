// =============================================================================
//  worker.cpp — Phase 2 implementation.
//
//  The Worker:
//    * Connects to the bot-controller via a bidirectional gRPC stream.
//    * Sends Hello / Heartbeats; receives StartLoad / UpdateRamp / Stop.
//    * On StartLoad, builds a LoadPlan, instantiates a Publisher and
//      `reactor_threads` reactors, and starts them.
//    * Forwards rate updates from UpdateRamp into all reactors.
//    * Tears down on Stop or shutdown_requested().
// =============================================================================

#include "bot_worker/worker.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "bot.grpc.pb.h"
#include "bot.pb.h"
#include "common.pb.h"

#include "bot_worker/ebpf_probe.h"
#include "bot_worker/plan.h"
#include "bot_worker/publisher.h"
#include "bot_worker/reactor.h"
#include "bot_worker/rl_policy.h"
#include "bot_worker/transport.h"

#include "velocity/common/log.h"
#include "velocity/common/signals.h"
#include "velocity/common/time.h"
#include "velocity/common/tracing.h"

namespace velocity::bot_worker {

namespace {

[[nodiscard]] auto make_transport_for(const velocity::bot::v1::LoadPlan& wire,
                                      const std::string& host,
                                      std::uint16_t port,
                                      const std::string& symbol)
    -> std::unique_ptr<Transport> {
    using WP = velocity::common::v1::WireProtocol;
    switch (wire.target().protocol()) {
        case WP::WIRE_PROTOCOL_WEBSOCKET:
            return make_ws_transport("ws://" + host + ":" + std::to_string(port));
        case WP::WIRE_PROTOCOL_FIX_44:
            return make_fix_transport(host, port,
                                      /*sender_comp_id=*/"VELOCITY-BOT",
                                      /*target_comp_id=*/"SUBMISSION",
                                      symbol.empty() ? std::string{"SPOT/USDT"} : symbol,
                                      /*price_scale=*/0);
        case WP::WIRE_PROTOCOL_REST:
        default:
            return make_rest_transport(host, port, /*connection_count=*/8);
    }
}

[[nodiscard]] auto translate_plan(const velocity::bot::v1::LoadPlan& wire) -> LoadPlan {
    LoadPlan p;
    p.submission_id    = wire.submission_id().value();
    p.target_host      = wire.target().host();
    p.target_port      = static_cast<std::uint16_t>(wire.target().port());
    p.bots_per_reactor = wire.bots_per_reactor() > 0 ? wire.bots_per_reactor() : 1024;
    p.symbol           = wire.symbol();
    p.fair_value       = wire.fair_value();
    p.tick_size        = wire.tick_size() > 0 ? wire.tick_size() : 1;

    // Multi-venue legs. Each leg keeps its own anchor; bots are
    // distributed deterministically across legs by the scheduler using
    // the weight totals here (so a 50/30/20 split places 50 % of the
    // bot population on leg 0, 30 % on leg 1, 20 % on leg 2).
    for (const auto& v : wire.venues()) {
        VenueLeg leg;
        leg.venue_id    = v.venue_id();
        leg.symbol      = v.symbol().empty() ? wire.symbol() : v.symbol();
        leg.weight      = v.weight() == 0 ? 100 : v.weight();
        leg.fair_value  = v.fair_value() == 0 ? wire.fair_value() : v.fair_value();
        leg.tick_size   = v.tick_size() == 0
                            ? (wire.tick_size() > 0 ? wire.tick_size() : 1)
                            : v.tick_size();
        if (v.has_target()) {
            leg.target_host = v.target().host();
            leg.target_port = static_cast<std::uint16_t>(v.target().port());
        }
        p.venues.push_back(std::move(leg));
    }

    for (const auto& slice : wire.personas()) {
        PersonaKind k = PersonaKind::NOISE;
        switch (slice.persona()) {
            case velocity::common::v1::BotPersona::BOT_PERSONA_MARKET_MAKER: k = PersonaKind::MARKET_MAKER;     break;
            case velocity::common::v1::BotPersona::BOT_PERSONA_AGGRESSIVE:   k = PersonaKind::AGGRESSIVE_TAKER; break;
            case velocity::common::v1::BotPersona::BOT_PERSONA_CANCELLER:    k = PersonaKind::CANCELLER;        break;
            case velocity::common::v1::BotPersona::BOT_PERSONA_SPOOFER:      k = PersonaKind::SPOOFER;          break;
            case velocity::common::v1::BotPersona::BOT_PERSONA_ADAPTIVE:     k = PersonaKind::ADAPTIVE;         break;
            default:                                                         k = PersonaKind::NOISE;            break;
        }
        p.personas.push_back(PersonaSlice{k, static_cast<float>(slice.weight())});
    }
    for (const auto& wp : wire.schedule()) {
        p.ramp.push_back(RampWaypoint{
            static_cast<std::int64_t>(wp.elapsed_ms()) * 1'000'000LL,
            static_cast<std::uint64_t>(wp.target_rps()),
        });
    }
    return p;
}

// Split a worker-wide target RPS across reactors so that the SUM of the
// per-reactor rates equals the target. Each reactor runs its own open-loop
// scheduler, so handing every reactor the full target made the worker offer
// `target × reactor_threads` — an N× overshoot that corrupted every
// throughput/latency/cliff measurement. The first `target % n` reactors carry
// one extra order/s to keep the aggregate exact.
[[nodiscard]] auto per_reactor_rps(std::uint64_t total_rps, std::size_t n,
                                   std::size_t idx) noexcept -> std::uint64_t {
    if (n <= 1) return total_rps;
    const auto base = total_rps / n;
    const auto rem  = total_rps % n;
    return base + (idx < rem ? 1ULL : 0ULL);
}

}  // namespace

struct Worker::Impl {
    WorkerConfig                                cfg;
    std::unique_ptr<Publisher>                  publisher;
    std::vector<std::unique_ptr<Reactor>>       reactors;
    std::unique_ptr<LoadPlan>                   plan;
    // Monotonic start time of the active run; atomic because the ramp
    // ticker thread observes it concurrently with start_run / stop_run.
    std::atomic<std::int64_t>                   run_started_mono_ns{0};
    std::unique_ptr<velocity::common::tracing::Span> run_span;
    mutable std::mutex                          state_mu;

    // Atomically-published live bot count so the heartbeat thread can
    // read it without locking state_mu on the hot path.
    std::atomic<std::uint32_t> active_bots_atomic{0};

    // Kernel-level latency probe. Always constructed; the underlying
    // eBPF program is only attached if VELOCITY_ENABLE_EBPF=ON and the
    // kernel accepts the kprobes. Failure is non-fatal — the heartbeat
    // simply reports kernel_*_ns = 0 and the frontend hides the series.
    EbpfProbe ebpf;

    // Shared RL policy. Loaded once at worker start from
    // VELOCITY_RL_MODEL_PATH (default /var/lib/velocity/rl/policy.onnx).
    // nullptr → ADAPTIVE personas degrade to MARKET_MAKER. We hold the
    // policy at worker scope (not per-run) because models are large to
    // memory-map; we explicitly do not reload on every benchmark.
    std::shared_ptr<RLPolicy> rl_policy;

    explicit Impl(WorkerConfig c) noexcept : cfg(std::move(c)) {
        if (cfg.reactor_threads == 0) {
            const auto hw = std::thread::hardware_concurrency();
            cfg.reactor_threads = hw == 0 ? 1u : hw;
        }
        if (cfg.publish_buffer_capacity == 0) {
            cfg.publish_buffer_capacity = 65'536u;
        }
        // Best-effort load of the RL policy. Path is env-overridable so
        // operators can A/B different models without rebuilding the
        // worker image.
        const char* rl_path = std::getenv("VELOCITY_RL_MODEL_PATH");
        const std::string path = (rl_path && *rl_path)
            ? rl_path
            : "/var/lib/velocity/rl/policy.onnx";
        rl_policy = RLPolicy::load(path);
        if (rl_policy && rl_policy->enabled()) {
            VLOG_INFO("RL policy loaded: {}", path);
        } else {
            VLOG_INFO("RL policy not available (path={}); ADAPTIVE personas "
                      "will fall back to MARKET_MAKER", path);
        }
    }

    auto start_run(const velocity::bot::v1::LoadPlan& wire_plan) -> void {
        std::lock_guard lk(state_mu);
        stop_run_locked();

        plan = std::make_unique<LoadPlan>(translate_plan(wire_plan));
        run_started_mono_ns.store(velocity::time::monotonic_ns(),
                                  std::memory_order_release);
        publisher = std::make_unique<Publisher>(cfg.redpanda_brokers,
                                                cfg.telemetry_topic,
                                                cfg.reactor_threads,
                                                cfg.publish_buffer_capacity);

        const auto initial_rps = interp_rps(plan->ramp, 0);
        VLOG_INFO("worker starting run submission={} target={}:{} initial_rps={}",
                  plan->submission_id, plan->target_host, plan->target_port, initial_rps);

        // Honour the W3C trace context the controller stamped into the plan.
        // We emit one span per LoadPlan; per-order spans would saturate any
        // collector at the volumes we drive. The span ends when the run does.
        if (!wire_plan.traceparent().empty()) {
            if (auto parent =
                    velocity::common::tracing::parse_traceparent(wire_plan.traceparent())) {
                run_span = std::make_unique<velocity::common::tracing::Span>(
                    velocity::common::tracing::start_span(
                        "bot-worker.run", *parent));
                run_span->set_attribute("worker_id",     cfg.worker_id);
                run_span->set_attribute("submission_id", plan->submission_id);
                run_span->set_attribute("target",        plan->target_host);
            }
        }

        const auto started_mono_ns = run_started_mono_ns.load(std::memory_order_acquire);

        // Build the per-reactor venue assignment. In single-venue mode
        // every reactor shares the LoadPlan's primary endpoint/symbol;
        // in multi-venue mode we partition `cfg.reactor_threads` across
        // legs proportionally to the leg weights, using the largest-
        // remainder allocation so each leg ends up with at least one
        // reactor when the weights are nonzero. This makes
        // bots-per-leg ≈ (weight / Σweights) × bots_per_reactor ×
        // reactor_threads, which matches what build_global_plan's
        // weights describe.
        struct LegAssign { std::string host; std::uint16_t port;
                           std::string symbol; std::string venue_id; };
        std::vector<LegAssign> per_reactor;
        per_reactor.reserve(cfg.reactor_threads);
        if (plan->venues.empty()) {
            for (std::uint32_t i = 0; i < cfg.reactor_threads; ++i) {
                per_reactor.push_back({
                    plan->target_host, plan->target_port,
                    plan->symbol, /*venue_id=*/""});
            }
        } else {
            std::uint64_t w_sum = 0;
            for (const auto& l : plan->venues) w_sum += l.weight == 0 ? 100 : l.weight;
            if (w_sum == 0) w_sum = 1;
            // Largest-remainder method (a.k.a. Hare quota).
            std::vector<std::uint32_t> base(plan->venues.size(), 0);
            std::vector<double>        rem(plan->venues.size(), 0.0);
            std::uint32_t allocated = 0;
            for (std::size_t i = 0; i < plan->venues.size(); ++i) {
                const double share = static_cast<double>(plan->venues[i].weight) /
                                     static_cast<double>(w_sum) *
                                     static_cast<double>(cfg.reactor_threads);
                base[i] = static_cast<std::uint32_t>(share);
                rem[i]  = share - base[i];
                allocated += base[i];
            }
            // Distribute the leftovers to the legs with the largest
            // remainders. With reactor_threads ≥ venues this also
            // guarantees every leg gets ≥ 1 reactor.
            while (allocated < cfg.reactor_threads) {
                std::size_t idx = 0;
                double best = -1.0;
                for (std::size_t i = 0; i < rem.size(); ++i) {
                    if (rem[i] > best) { best = rem[i]; idx = i; }
                }
                base[idx]++;
                rem[idx] = -1.0;     // already topped up
                allocated++;
            }
            for (std::size_t i = 0; i < plan->venues.size(); ++i) {
                const auto& leg = plan->venues[i];
                const auto host = leg.target_host.empty()
                                      ? plan->target_host : leg.target_host;
                const auto port = leg.target_port == 0
                                      ? plan->target_port : leg.target_port;
                for (std::uint32_t k = 0; k < base[i]; ++k) {
                    per_reactor.push_back({host, port, leg.symbol, leg.venue_id});
                }
            }
        }

        for (std::uint32_t i = 0; i < cfg.reactor_threads; ++i) {
            ReactorConfig rc{
                static_cast<std::uint8_t>(i),
                plan->bots_per_reactor,
                started_mono_ns,
                per_reactor_rps(initial_rps, cfg.reactor_threads, i),
                rl_policy,
            };
            const auto& assign = per_reactor[i];
            auto transport = make_transport_for(wire_plan,
                                                assign.host,
                                                assign.port,
                                                assign.symbol);
            auto reactor = std::make_unique<Reactor>(rc, plan.get(),
                                                     std::move(transport), publisher.get());
            reactor->start();
            reactors.push_back(std::move(reactor));
        }
        active_bots_atomic.store(
            static_cast<std::uint32_t>(reactors.size()) * plan->bots_per_reactor,
            std::memory_order_release);
    }

    auto update_ramp(std::uint64_t new_target_rps) -> void {
        std::lock_guard lk(state_mu);
        for (std::size_t i = 0; i < reactors.size(); ++i) {
            reactors[i]->set_rate(per_reactor_rps(new_target_rps, reactors.size(), i));
        }
    }

    // Replace the remaining waypoints in the plan with `incoming`, starting
    // at `from_elapsed_ms`. Implements the contract of `RampUpdate` from
    // bot.proto and lets the periodic ramp tick keep interpolating against
    // the updated schedule.
    auto replace_ramp(std::uint64_t from_elapsed_ms,
                      const google::protobuf::RepeatedPtrField<
                          velocity::bot::v1::RampWaypoint>& incoming) -> void {
        std::lock_guard lk(state_mu);
        if (!plan) return;
        std::vector<RampWaypoint> updated;
        updated.reserve(plan->ramp.size() + static_cast<std::size_t>(incoming.size()));
        const auto cutoff_ns = static_cast<std::int64_t>(from_elapsed_ms) * 1'000'000LL;
        for (const auto& wp : plan->ramp) {
            if (wp.offset_ns < cutoff_ns) updated.push_back(wp);
        }
        for (const auto& wp : incoming) {
            updated.push_back(RampWaypoint{
                static_cast<std::int64_t>(wp.elapsed_ms()) * 1'000'000LL,
                static_cast<std::uint64_t>(wp.target_rps()),
            });
        }
        plan->ramp = std::move(updated);
    }

    // Sample the current ramp at `elapsed_ns` since the run started and
    // push that rate into every reactor. Called from a 4 Hz background
    // ticker so long ramps progress without controller intervention.
    auto tick_ramp(std::int64_t elapsed_ns) -> void {
        std::lock_guard lk(state_mu);
        if (!plan || reactors.empty()) return;
        const auto rps = interp_rps(plan->ramp, elapsed_ns);
        for (std::size_t i = 0; i < reactors.size(); ++i) {
            reactors[i]->set_rate(per_reactor_rps(rps, reactors.size(), i));
        }
    }

    auto stop_run() -> void {
        std::lock_guard lk(state_mu);
        stop_run_locked();
    }

    auto stop_run_locked() -> void {
        if (reactors.empty() && !publisher) return;
        VLOG_INFO("worker stopping run");
        active_bots_atomic.store(0, std::memory_order_release);
        run_started_mono_ns.store(0, std::memory_order_release);
        for (auto& r : reactors) r->stop();
        reactors.clear();
        if (publisher) {
            publisher->stop();
            publisher.reset();
        }
        plan.reset();
        run_span.reset();   // emits the worker's run span to OTLP
    }

    auto aggregate_orders_sent() const -> std::uint64_t {
        std::lock_guard lk(state_mu);
        std::uint64_t total = 0;
        for (const auto& r : reactors) total += r->orders_sent();
        return total;
    }

    auto aggregate_orders_acked() const -> std::uint64_t {
        std::lock_guard lk(state_mu);
        std::uint64_t total = 0;
        for (const auto& r : reactors) total += r->orders_acked();
        return total;
    }

    auto aggregate_orders_errored() const -> std::uint64_t {
        std::lock_guard lk(state_mu);
        std::uint64_t total = 0;
        for (const auto& r : reactors) total += r->orders_errored();
        return total;
    }

    [[nodiscard]] auto active_bot_count() const noexcept -> std::uint32_t {
        return active_bots_atomic.load(std::memory_order_acquire);
    }
};

Worker::Worker(WorkerConfig cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}
Worker::~Worker() { if (impl_) impl_->stop_run(); }

auto Worker::run() -> void {
    // Arm the kernel-level latency probe as early as possible — before
    // any sockets are opened, so we don't miss the first few sends. If
    // start() fails (probe disabled, kernel too old, capability denied)
    // we keep going with userspace-only timing; the probe object is a
    // cheap no-op in that case.
    if (impl_->ebpf.start()) {
        VLOG_INFO("kernel TCP latency probe active");
    } else if (ebpf_supported()) {
        VLOG_WARN("kernel TCP latency probe failed to attach — running userspace-only");
    }

    // Connect to the controller.
    const auto addr = impl_->cfg.controller_grpc_addr;
    auto channel = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
    auto stub    = velocity::bot::v1::BotControl::NewStub(channel);

    grpc::ClientContext ctx;
    auto stream = stub->Control(&ctx);
    if (!stream) {
        throw std::runtime_error("failed to open Control() bidi stream to " + addr);
    }

    // Hello (first message on the stream).
    velocity::bot::v1::WorkerToController hello;
    auto* h = hello.mutable_hello();
    h->set_worker_id(impl_->cfg.worker_id);
    h->set_cpu_count(impl_->cfg.reactor_threads);
    h->add_supported_protocols(velocity::common::v1::WireProtocol::WIRE_PROTOCOL_REST);
    h->add_supported_protocols(velocity::common::v1::WireProtocol::WIRE_PROTOCOL_WEBSOCKET);
    h->add_supported_protocols(velocity::common::v1::WireProtocol::WIRE_PROTOCOL_FIX_44);
    using BP = velocity::common::v1::BotPersona;
    h->add_supported_personas(BP::BOT_PERSONA_MARKET_MAKER);
    h->add_supported_personas(BP::BOT_PERSONA_AGGRESSIVE);
    h->add_supported_personas(BP::BOT_PERSONA_CANCELLER);
    h->add_supported_personas(BP::BOT_PERSONA_SPOOFER);
    h->add_supported_personas(BP::BOT_PERSONA_NOISE);
    if (!stream->Write(hello)) {
        // Stream broke before we even said hello — abort cleanly so the
        // process supervisor can reschedule us against another controller.
        const auto status = stream->Finish();
        throw std::runtime_error(
            "controller stream rejected Hello: " +
            std::to_string(static_cast<int>(status.error_code())) +
            " " + status.error_message());
    }
    VLOG_INFO("sent Hello to controller as worker={}", impl_->cfg.worker_id);

    // Background heartbeat / status reporter. Runs at 5 Hz so the
    // controller's per-benchmark snapshot loop (4 Hz) always has fresh
    // counters to diff. We send the cumulative `sent_total` plus the
    // smoothed RPS computed from monotonic-clock deltas (realtime can
    // jump under NTP slew and break the rate estimate).
    //
    // acked_total / errored_total are aggregated from the per-reactor transport
    // ack callbacks (ACK/FILLED/PARTIAL count as acked; REJECT/TIMEOUT as
    // errored), so the controller sees real outcome rates rather than a
    // hard-coded 100% ack / 0 error placeholder.
    std::atomic<bool> hb_stop{false};
    std::atomic<bool> stream_alive{true};
    std::thread hb_thread([&]() {
        constexpr auto kInterval = std::chrono::milliseconds(200);
        std::int64_t last_mono_ns = 0;
        std::uint64_t last_sent = 0;
        while (!hb_stop.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(kInterval);
            const auto mono_ns = velocity::time::monotonic_ns();
            const auto wall_ns = velocity::time::realtime_ns();
            const auto sent    = impl_->aggregate_orders_sent();
            const auto acked   = impl_->aggregate_orders_acked();
            const auto errored = impl_->aggregate_orders_errored();
            const auto dt_ns   = last_mono_ns == 0 ? 0 : (mono_ns - last_mono_ns);
            const auto d_sent  = sent - last_sent;
            const auto rps     = dt_ns > 0
                                    ? (d_sent * 1'000'000'000ULL) /
                                          static_cast<std::uint64_t>(dt_ns)
                                    : 0ULL;
            last_sent    = sent;
            last_mono_ns = mono_ns;

            velocity::bot::v1::WorkerToController msg;
            auto* status = msg.mutable_status();
            status->set_worker_id(impl_->cfg.worker_id);
            status->set_sent_total(sent);
            status->set_acked_total(acked);
            status->set_errored_total(errored);
            status->set_current_rps(rps);
            status->set_active_bots(impl_->active_bot_count());
            status->set_sent_ts_ns(static_cast<std::uint64_t>(wall_ns));

            // Kernel-side latency from the eBPF probe. snapshot() is
            // lock-free and returns zeroed fields when the probe is
            // inactive — perfectly fine to publish unconditionally;
            // the controller hides the kernel series when
            // kernel_samples == 0.
            if (const auto k = impl_->ebpf.snapshot(); k.samples_observed > 0) {
                status->set_kernel_p50_ns(k.p50_ns);
                status->set_kernel_p99_ns(k.p99_ns);
                status->set_kernel_p999_ns(k.p999_ns);
                status->set_kernel_samples(k.samples_observed);
            }
            if (!stream->Write(msg)) {
                stream_alive.store(false, std::memory_order_release);
                return;
            }
        }
    });

    // Independent ramp interpolator. We sample `plan->ramp` every 250 ms
    // and push the interpolated rate into every reactor. This is the only
    // way long ramps (e.g. 5-minute linear builds) progress between
    // controller-initiated `RampUpdate` events.
    std::thread ramp_thread([&]() {
        constexpr auto kInterval = std::chrono::milliseconds(250);
        while (!hb_stop.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(kInterval);
            const auto start_ns = impl_->run_started_mono_ns.load(std::memory_order_acquire);
            if (start_ns == 0) continue;
            const auto elapsed = velocity::time::monotonic_ns() - start_ns;
            impl_->tick_ramp(elapsed);
        }
    });

    // Inbound loop.
    velocity::bot::v1::ControllerToWorker in;
    while (!velocity::signals::shutdown_requested() &&
           stream_alive.load(std::memory_order_acquire) &&
           stream->Read(&in)) {
        switch (in.payload_case()) {
            case velocity::bot::v1::ControllerToWorker::kPlan:
                impl_->start_run(in.plan());
                break;
            case velocity::bot::v1::ControllerToWorker::kRamp:
                // Replace the remaining schedule with the controller's
                // updated waypoints. The periodic ramp ticker will pick up
                // the new rate on its next iteration; we also push the
                // immediate-target rate now so we don't lose the 250 ms
                // tick window when the update arrived between ticks.
                if (in.ramp().waypoints_size() > 0) {
                    impl_->replace_ramp(in.ramp().from_elapsed_ms(),
                                        in.ramp().waypoints());
                    const auto start_ns =
                        impl_->run_started_mono_ns.load(std::memory_order_acquire);
                    const auto elapsed = start_ns == 0
                                             ? std::int64_t{0}
                                             : velocity::time::monotonic_ns() - start_ns;
                    impl_->tick_ramp(elapsed);
                }
                break;
            case velocity::bot::v1::ControllerToWorker::kShutdown:
                impl_->stop_run();
                break;
            case velocity::bot::v1::ControllerToWorker::kHeartbeat:
                // Drop on the floor; the stream itself is the heartbeat.
                break;
            default:
                break;
        }
    }

    hb_stop.store(true, std::memory_order_release);
    if (hb_thread.joinable())   hb_thread.join();
    if (ramp_thread.joinable()) ramp_thread.join();
    stream->WritesDone();
    auto status = stream->Finish();
    if (!status.ok()) {
        VLOG_WARN("controller stream closed: {} {}", static_cast<int>(status.error_code()),
                  status.error_message());
    }
    impl_->stop_run();
}

}  // namespace velocity::bot_worker
