// =============================================================================
//  clients.cpp — process-singleton gRPC stubs.
// =============================================================================

#include "api_gateway/clients.h"

#include <atomic>
#include <memory>
#include <mutex>

#include <sw/redis++/redis++.h>

#include "velocity/common/log.h"

namespace velocity::api_gateway::clients {

namespace {
std::unique_ptr<sw::redis::Redis> g_redis;
std::once_flag                    g_redis_once;
}  // namespace

auto RedisClient::init(const std::string& addr) -> void {
    std::call_once(g_redis_once, [&]() {
        try {
            g_redis = std::make_unique<sw::redis::Redis>(addr);
            g_redis->ping();
            VLOG_INFO("gateway: connected to redis {}", addr);
        } catch (const std::exception& e) {
            VLOG_WARN("gateway: redis unavailable ({}); fleet/leaderboard routes degraded",
                      e.what());
            g_redis.reset();
        }
    });
}

auto RedisClient::get() -> sw::redis::Redis* { return g_redis.get(); }

auto GrpcClients::init(const std::string& submission_engine_addr,
                       const std::string& bot_controller_addr) -> void {
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(64 * 1024 * 1024);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS,             20'000);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS,          10'000);
    args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);

    submission_channel_ = grpc::CreateCustomChannel(
        submission_engine_addr, grpc::InsecureChannelCredentials(), args);
    submission_stub_ = velocity::orchestrator::v1::SubmissionService::NewStub(submission_channel_);

    benchmark_channel_ = grpc::CreateCustomChannel(
        bot_controller_addr, grpc::InsecureChannelCredentials(), args);
    benchmark_stub_ = velocity::orchestrator::v1::BenchmarkService::NewStub(benchmark_channel_);
}

auto GrpcClients::submission() -> velocity::orchestrator::v1::SubmissionService::Stub* {
    return submission_stub_.get();
}

auto GrpcClients::benchmark() -> velocity::orchestrator::v1::BenchmarkService::Stub* {
    return benchmark_stub_.get();
}

}  // namespace velocity::api_gateway::clients
