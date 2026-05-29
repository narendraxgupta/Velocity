// =============================================================================
//  bot_controller/benchmark_service.h
//
//  Implements the `velocity.orchestrator.v1.BenchmarkService` gRPC contract:
//
//      StartBenchmark   (unary)
//      CancelBenchmark  (unary)
//      WatchBenchmark   (server-streaming)
//      GetReport        (unary)
//
//  Lifecycle model
//  ---------------
//  At-most-one active benchmark per controller. A second StartBenchmark
//  while another is in flight returns ABORTED — this matches the singleton
//  K8s Deployment topology and avoids the (currently unmodeled) problem
//  of attributing worker StatusUpdates to one of several active benchmarks.
//
//  Tick loop
//  ---------
//  A single background "ticker" thread runs at 4 Hz. On each tick it:
//    1. Walks the worker Registry, diffs StatusUpdate counters against the
//       baseline captured at StartBenchmark, computes RPS.
//    2. Reads the latest latency / correctness summary from Redis (populated
//       by the scoring service). Falls back to zeros if absent.
//    3. Builds a BenchmarkSnapshot and stores it on the session.
//    4. Notifies any WatchBenchmark RPCs blocking on the session.
//
//  Profile catalogue
//  -----------------
//  Profiles are hard-coded here for now (baseline / spike / fire-hose /
//  adversarial). When the operator UI lands they will move to a YAML
//  loader under infra/profiles/.
// =============================================================================

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

#include <grpcpp/grpcpp.h>

#include "orchestrator.grpc.pb.h"
#include "orchestrator.pb.h"

namespace sw::redis { class Redis; }

namespace velocity::bot_controller {

class Registry;

struct BenchmarkServiceConfig {
    // Optional Redis address — used to read scoring-service summaries.
    // Empty string disables Redis enrichment (latency/correctness stay zero).
    std::string redis_addr;
};

class BenchmarkServiceImpl final
    : public velocity::orchestrator::v1::BenchmarkService::Service {
public:
    BenchmarkServiceImpl(Registry* registry, BenchmarkServiceConfig cfg);
    ~BenchmarkServiceImpl() override;

    BenchmarkServiceImpl(const BenchmarkServiceImpl&)            = delete;
    BenchmarkServiceImpl& operator=(const BenchmarkServiceImpl&) = delete;

    // gRPC handlers.
    auto StartBenchmark(grpc::ServerContext* ctx,
                        const velocity::orchestrator::v1::StartBenchmarkRequest* req,
                        velocity::orchestrator::v1::StartBenchmarkResponse* resp)
        -> grpc::Status override;

    auto CancelBenchmark(grpc::ServerContext* ctx,
                         const velocity::orchestrator::v1::CancelBenchmarkRequest* req,
                         velocity::orchestrator::v1::CancelBenchmarkResponse* resp)
        -> grpc::Status override;

    auto WatchBenchmark(grpc::ServerContext* ctx,
                        const velocity::orchestrator::v1::WatchBenchmarkRequest* req,
                        grpc::ServerWriter<velocity::orchestrator::v1::BenchmarkSnapshot>* writer)
        -> grpc::Status override;

    auto GetReport(grpc::ServerContext* ctx,
                   const velocity::orchestrator::v1::GetReportRequest* req,
                   velocity::orchestrator::v1::BenchmarkReport* resp)
        -> grpc::Status override;

    // Drive any final shutdown logic before the gRPC server stops accepting.
    auto stop() -> void;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Look up the canonical profile by name. Returns false if unknown.
[[nodiscard]] auto resolve_profile(std::string_view name,
                                   velocity::orchestrator::v1::BenchmarkProfile& out) noexcept
    -> bool;

}  // namespace velocity::bot_controller
