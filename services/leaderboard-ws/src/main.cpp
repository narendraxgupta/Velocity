// =============================================================================
//  velocity-leaderboard-ws — entrypoint.
// =============================================================================

#include <cstdlib>
#include <exception>

#include "leaderboard_ws/server.h"
#include "velocity/common/env.h"
#include "velocity/common/log.h"
#include "velocity/common/signals.h"
#include "velocity/common/time.h"

using velocity::leaderboard_ws::Server;
using velocity::leaderboard_ws::ServerConfig;

auto main(int /*argc*/, char* /*argv*/[]) -> int {
    velocity::log::init("leaderboard-ws");
    velocity::time::init();
    velocity::signals::install_shutdown();

    try {
        ServerConfig cfg{
            .listen_host   = velocity::env::optional<std::string>("VELOCITY_WS_HOST", "0.0.0.0"),
            .ws_port       = velocity::env::optional<std::uint16_t>("VELOCITY_WS_PORT", 8090),
            .metrics_port  = velocity::env::optional<std::uint16_t>("VELOCITY_METRICS_PORT", 9097),
            .redis_addr    = velocity::env::required<std::string>("VELOCITY_REDIS_ADDR"),
            .channel_pattern = velocity::env::optional<std::string>(
                "VELOCITY_REDIS_CHANNEL_PATTERN", "leaderboard.*"),
        };

        VLOG_INFO("velocity-leaderboard-ws starting on {}:{}", cfg.listen_host, cfg.ws_port);

        Server s{std::move(cfg)};
        s.run();
        VLOG_INFO("shutdown complete");
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        VLOG_FATAL("fatal: {}", e.what());
        return EXIT_FAILURE;
    }
}
