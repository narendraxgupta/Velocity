// =============================================================================
//  controller_service.cpp
//
//  Owns:
//    * A gRPC server hosting BOTH:
//        - BotControl::Control()       (bidi worker stream, defined here)
//        - BenchmarkService::*         (orchestrator API, defined in
//                                       benchmark_service.cpp)
//    * A worker registry — workers self-register via their Hello message.
//    * A snapshot aggregator — every 200 ms it sums StatusUpdate counters
//      into a `BenchmarkSnapshot` published by the benchmark service.
//
//  Concurrency model: one stream-handler thread per worker (gRPC's
//  synchronous server model). A shared mutex guards the worker registry.
// =============================================================================

#include "bot_controller/controller_service.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>

#include <nlohmann/json.hpp>
#include <sw/redis++/redis++.h>

#include "bot.grpc.pb.h"
#include "bot.pb.h"

#include "bot_controller/benchmark_service.h"
#include "bot_controller/load_planner.h"
#include "bot_controller/registry.h"

#include "velocity/common/env.h"
#include "velocity/common/log.h"
#include "velocity/common/signals.h"
#include "velocity/common/time.h"

namespace velocity::bot_controller {

namespace {

using grpc::ServerContext;
using grpc::ServerReaderWriter;
using grpc::Status;
using grpc::StatusCode;

class BotControlImpl final : public velocity::bot::v1::BotControl::Service {
public:
    explicit BotControlImpl(Registry* reg) : reg_(reg) {}

    auto Control(
        ServerContext* ctx,
        ServerReaderWriter<velocity::bot::v1::ControllerToWorker,
                           velocity::bot::v1::WorkerToController>* stream)
        -> Status override {

        // First message MUST be Hello.
        velocity::bot::v1::WorkerToController first;
        if (!stream->Read(&first) || first.payload_case() !=
            velocity::bot::v1::WorkerToController::kHello) {
            return Status(StatusCode::INVALID_ARGUMENT, "expected Hello as first message");
        }
        if (first.hello().worker_id().empty()) {
            return Status(StatusCode::INVALID_ARGUMENT, "Hello missing worker_id");
        }

        auto session = std::make_shared<WorkerSession>();
        session->worker_id = first.hello().worker_id();
        session->hostname  = first.hello().hostname();
        session->cpu_count = std::max<std::uint32_t>(1U, first.hello().cpu_count());
        session->stream    = stream;
        reg_->add(session);
        VLOG_INFO("worker {} ({}) connected (cpu={})",
                  session->worker_id, session->hostname, session->cpu_count);

        // Service the rest of the inbound stream until the worker
        // disconnects or the client cancels.
        velocity::bot::v1::WorkerToController msg;
        while (!ctx->IsCancelled() && stream->Read(&msg)) {
            switch (msg.payload_case()) {
                case velocity::bot::v1::WorkerToController::kStatus: {
                    const auto& st = msg.status();
                    session->last_sent_total.store(st.sent_total(),
                                                   std::memory_order_relaxed);
                    session->last_acked_total.store(st.acked_total(),
                                                    std::memory_order_relaxed);
                    session->last_errored_total.store(st.errored_total(),
                                                      std::memory_order_relaxed);
                    session->last_current_rps.store(st.current_rps(),
                                                    std::memory_order_relaxed);
                    session->last_active_bots.store(st.active_bots(),
                                                    std::memory_order_relaxed);
                    session->last_cpu_percent.store(st.cpu_percent(),
                                                    std::memory_order_relaxed);
                    session->last_heartbeat_ns.store(
                        static_cast<std::int64_t>(st.sent_ts_ns()),
                        std::memory_order_relaxed);
                    // Kernel-side latency (zero when worker probe is inactive).
                    session->last_kernel_p50_ns.store(st.kernel_p50_ns(),
                                                      std::memory_order_relaxed);
                    session->last_kernel_p99_ns.store(st.kernel_p99_ns(),
                                                      std::memory_order_relaxed);
                    session->last_kernel_p999_ns.store(st.kernel_p999_ns(),
                                                       std::memory_order_relaxed);
                    session->last_kernel_samples.store(st.kernel_samples(),
                                                       std::memory_order_relaxed);
                    break;
                }
                case velocity::bot::v1::WorkerToController::kError:
                    VLOG_WARN("worker {} error: kind={} detail={}",
                              session->worker_id,
                              static_cast<int>(msg.error().kind()),
                              msg.error().detail());
                    break;
                case velocity::bot::v1::WorkerToController::kGoodbyeAck:
                    VLOG_INFO("worker {} sent GoodbyeAck", session->worker_id);
                    break;
                default:
                    break;
            }
        }

        VLOG_INFO("worker {} disconnected", session->worker_id);
        reg_->remove(session->worker_id);
        return Status::OK;
    }

private:
    Registry* reg_;
};

}  // namespace

// -----------------------------------------------------------------------------
//  Impl
// -----------------------------------------------------------------------------
struct ControllerService::Impl {
    ControllerConfig                       cfg;
    Registry                               registry;
    std::unique_ptr<BotControlImpl>        bot_service;
    std::unique_ptr<BenchmarkServiceImpl>  bench_service;
    std::unique_ptr<grpc::Server>          server;
    std::unique_ptr<sw::redis::Redis>      redis;
    std::thread                            fleet_publisher;
    std::atomic<bool>                      fleet_stop{false};

    explicit Impl(ControllerConfig c) : cfg(std::move(c)) {}

    ~Impl() {
        fleet_stop.store(true, std::memory_order_release);
        if (fleet_publisher.joinable()) fleet_publisher.join();
    }

    auto run() -> void {
        const auto addr = cfg.listen_host + ':' + std::to_string(cfg.grpc_port);

        grpc::EnableDefaultHealthCheckService(true);
        grpc::reflection::InitProtoReflectionServerBuilderPlugin();

        bot_service = std::make_unique<BotControlImpl>(&registry);

        const auto redis_addr = velocity::env::optional<std::string>("VELOCITY_REDIS_ADDR", "");
        if (!redis_addr.empty()) {
            try {
                redis = std::make_unique<sw::redis::Redis>(redis_addr);
                redis->ping();
                VLOG_INFO("controller: connected to redis {}", redis_addr);
                fleet_publisher = std::thread([this]() { publish_fleet_loop(); });
            } catch (const std::exception& e) {
                VLOG_WARN("controller: redis unavailable ({}); fleet snapshots disabled",
                          e.what());
                redis.reset();
            }
        }

        BenchmarkServiceConfig bench_cfg{ .redis_addr = redis_addr };
        bench_service = std::make_unique<BenchmarkServiceImpl>(&registry,
                                                               std::move(bench_cfg));

        grpc::ServerBuilder builder;
        builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
        builder.RegisterService(bot_service.get());
        builder.RegisterService(bench_service.get());
        builder.SetMaxReceiveMessageSize(16 * 1024 * 1024);

        server = builder.BuildAndStart();
        if (!server) {
            throw std::runtime_error("failed to bind gRPC listener on " + addr);
        }
        VLOG_INFO("controller gRPC ready on {} (BotControl + BenchmarkService)", addr);

        // Drive the supervisor loop until shutdown.
        while (!velocity::signals::shutdown_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        VLOG_INFO("controller draining; broadcasting Shutdown");
        // Stop the benchmark service first so its ticker stops touching
        // worker sessions; then send the worker-level Shutdown so workers
        // know to drain.
        bench_service->stop();
        broadcast_shutdown();
        server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(5));
        server->Wait();
        fleet_stop.store(true, std::memory_order_release);
    }

    // Periodically (1 Hz) emit a JSON snapshot of the worker registry to
    // Redis. The API gateway reads this for /v1/fleet.
    auto publish_fleet_loop() -> void {
        using namespace std::chrono_literals;
        while (!fleet_stop.load(std::memory_order_acquire) &&
               !velocity::signals::shutdown_requested()) {
            try {
                nlohmann::json j;
                j["ts_ns"] = velocity::time::realtime_ns();
                auto& arr = j["workers"];
                arr = nlohmann::json::array();
                for (const auto& s : registry.all()) {
                    arr.push_back({
                        {"worker_id",       s->worker_id},
                        {"hostname",        s->hostname},
                        {"cpu_count",       s->cpu_count},
                        {"sent_total",      s->last_sent_total.load()},
                        {"acked_total",     s->last_acked_total.load()},
                        {"errored_total",   s->last_errored_total.load()},
                        {"current_rps",     s->last_current_rps.load()},
                        {"active_bots",     s->last_active_bots.load()},
                        {"cpu_percent",     s->last_cpu_percent.load()},
                        {"last_heartbeat_ns", s->last_heartbeat_ns.load()},
                    });
                }
                redis->set("fleet:workers", j.dump(), std::chrono::seconds{15});
            } catch (const std::exception& e) {
                static std::atomic<int> warn{0};
                if ((warn++ % 60) == 0) {
                    VLOG_WARN("controller fleet publish failed: {}", e.what());
                }
            }
            std::this_thread::sleep_for(1s);
        }
    }

    // Send Shutdown to every connected worker.
    auto broadcast_shutdown() -> void {
        for (auto& s : registry.all()) {
            velocity::bot::v1::ControllerToWorker msg;
            auto* sh = msg.mutable_shutdown();
            sh->set_reason("controller shutting down");
            sh->set_drain(true);
            sh->set_drain_timeout_ms(2000);
            std::lock_guard lk(s->write_mu);
            if (!s->stream->Write(msg)) {
                VLOG_WARN("controller: shutdown write to worker {} failed",
                          s->worker_id);
            }
        }
    }
};

ControllerService::ControllerService(ControllerConfig cfg)
    : impl_(std::make_unique<Impl>(std::move(cfg))) {}
ControllerService::~ControllerService() = default;

auto ControllerService::run() -> void { impl_->run(); }

}  // namespace velocity::bot_controller
