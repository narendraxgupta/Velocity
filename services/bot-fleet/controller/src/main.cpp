// =============================================================================
//  velocity-bot-controller — entrypoint.
// =============================================================================

#include <cstdlib>
#include <exception>

#include "bot_controller/controller_service.h"
#include "velocity/common/env.h"
#include "velocity/common/log.h"
#include "velocity/common/signals.h"
#include "velocity/common/time.h"
#include "velocity/common/tracing.h"

using velocity::bot_controller::ControllerConfig;
using velocity::bot_controller::ControllerService;

auto main(int /*argc*/, char* /*argv*/[]) -> int {
    velocity::log::init("bot-controller");
    velocity::time::init();
    velocity::signals::install_shutdown();

    velocity::common::tracing::init(
        "bot-controller",
        velocity::env::optional<std::string>("VELOCITY_OTLP_ENDPOINT", ""));

    try {
        ControllerConfig cfg{
            .listen_host  = velocity::env::optional<std::string>("VELOCITY_GRPC_HOST", "0.0.0.0"),
            .grpc_port    = velocity::env::optional<std::uint16_t>("VELOCITY_GRPC_PORT", 7002),
            .metrics_port = velocity::env::optional<std::uint16_t>("VELOCITY_METRICS_PORT", 9093),
        };

        VLOG_INFO("velocity-bot-controller starting on {}:{}", cfg.listen_host, cfg.grpc_port);

        ControllerService svc{std::move(cfg)};
        svc.run();
        VLOG_INFO("shutdown complete");
        velocity::common::tracing::shutdown();
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        VLOG_FATAL("fatal: {}", e.what());
        velocity::common::tracing::shutdown();
        return EXIT_FAILURE;
    }
}
