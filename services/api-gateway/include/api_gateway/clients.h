// =============================================================================
//  api_gateway/clients.h — gRPC client singletons.
//
//  The HTTP handlers call into these to translate user actions into typed
//  gRPC calls against the Submission Engine (Go) and the Bot Controller (C++).
//
//  Singletons live for the lifetime of the process; channels reconnect
//  automatically under the hood.
// =============================================================================

#pragma once

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "orchestrator.grpc.pb.h"

namespace sw::redis { class Redis; }

namespace velocity::api_gateway::clients {

class GrpcClients {
public:
    static auto init(const std::string& submission_engine_addr,
                     const std::string& bot_controller_addr) -> void;

    [[nodiscard]] static auto submission()
        -> velocity::orchestrator::v1::SubmissionService::Stub*;

    [[nodiscard]] static auto benchmark()
        -> velocity::orchestrator::v1::BenchmarkService::Stub*;

private:
    static inline std::shared_ptr<grpc::Channel> submission_channel_;
    static inline std::shared_ptr<grpc::Channel> benchmark_channel_;
    static inline std::unique_ptr<velocity::orchestrator::v1::SubmissionService::Stub> submission_stub_;
    static inline std::unique_ptr<velocity::orchestrator::v1::BenchmarkService::Stub>  benchmark_stub_;
};

// Singleton wrapper around sw::redis::Redis — used by HTTP routes that
// read leaderboard ZSET / fleet snapshots written by the controller +
// scoring service. Returns nullptr if Redis is unavailable so handlers
// can degrade gracefully (used to return 503 with a hint).
class RedisClient {
public:
    static auto init(const std::string& addr) -> void;
    [[nodiscard]] static auto get() -> sw::redis::Redis*;
};

}  // namespace velocity::api_gateway::clients
