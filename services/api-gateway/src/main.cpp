// =============================================================================
//  velocity-api-gateway — entrypoint.
//
//  The HTTP/WebSocket facade in front of the platform. Translates outside-
//  world REST/WS into typed gRPC calls against the Submission Engine and the
//  Bot Controller.
//
//  Configuration is read entirely from the environment. We refuse to boot if
//  required variables are missing.
// =============================================================================

#include <cstdlib>
#include <exception>

#include "api_gateway/server.h"
#include "velocity/common/env.h"
#include "velocity/common/log.h"
#include "velocity/common/signals.h"
#include "velocity/common/time.h"
#include "velocity/common/tracing.h"

using velocity::api_gateway::Server;
using velocity::api_gateway::ServerConfig;

namespace {

[[nodiscard]] auto load_config() -> ServerConfig {
    return ServerConfig{
        .listen_host           = velocity::env::optional<std::string>("VELOCITY_HTTP_HOST", "0.0.0.0"),
        .http_port             = velocity::env::optional<std::uint16_t>("VELOCITY_HTTP_PORT", 8080),
        .metrics_port          = velocity::env::optional<std::uint16_t>("VELOCITY_METRICS_PORT", 9091),
        .submission_engine_grpc = velocity::env::required<std::string>("VELOCITY_SUBMISSION_ENGINE_GRPC"),
        .bot_controller_grpc    = velocity::env::required<std::string>("VELOCITY_BOT_CONTROLLER_GRPC"),
        .redis_addr             = velocity::env::required<std::string>("VELOCITY_REDIS_ADDR"),
        .minio_endpoint         = velocity::env::required<std::string>("VELOCITY_MINIO_ENDPOINT"),
        .minio_access_key       = velocity::env::required<std::string>("VELOCITY_MINIO_ACCESS_KEY"),
        .minio_secret_key       = velocity::env::required<std::string>("VELOCITY_MINIO_SECRET_KEY"),
    };
}

}  // namespace

auto main(int /*argc*/, char* /*argv*/[]) -> int {
    velocity::log::init("api-gateway");
    velocity::time::init();
    velocity::signals::install_shutdown();

    // OTLP / Jaeger tracing — endpoint may be empty in dev (export disabled).
    velocity::common::tracing::init(
        "api-gateway",
        velocity::env::optional<std::string>("VELOCITY_OTLP_ENDPOINT", ""));

    try {
        auto cfg = load_config();
        VLOG_INFO("velocity-api-gateway starting on {}:{}", cfg.listen_host, cfg.http_port);

        Server server{std::move(cfg)};
        server.run();
        VLOG_INFO("shutdown complete");
        velocity::common::tracing::shutdown();
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        VLOG_FATAL("fatal: {}", e.what());
        velocity::common::tracing::shutdown();
        return EXIT_FAILURE;
    }
}
