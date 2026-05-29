// =============================================================================
//  velocity-telemetry-ingester — entrypoint.
// =============================================================================

#include <cstdlib>
#include <exception>

#include "telemetry_ingester/ingester.h"
#include "velocity/common/env.h"
#include "velocity/common/log.h"
#include "velocity/common/signals.h"
#include "velocity/common/time.h"

using velocity::telemetry_ingester::Ingester;
using velocity::telemetry_ingester::IngesterConfig;

auto main(int /*argc*/, char* /*argv*/[]) -> int {
    velocity::log::init("telemetry-ingester");
    velocity::time::init();
    velocity::signals::install_shutdown();

    try {
        IngesterConfig cfg{
            .brokers          = velocity::env::required<std::string>("VELOCITY_REDPANDA_BROKERS"),
            .group_id         = velocity::env::optional<std::string>(
                "VELOCITY_KAFKA_GROUP_ID", "velocity-ingester"),
            .telemetry_topic  = velocity::env::optional<std::string>(
                "VELOCITY_TELEMETRY_TOPIC", "telemetry.raw"),
            .latency_topic    = velocity::env::optional<std::string>(
                "VELOCITY_LATENCY_TOPIC", "metrics.latency.1s"),
            .questdb_host     = velocity::env::required<std::string>("VELOCITY_QUESTDB_HOST"),
            .questdb_ilp_port = velocity::env::optional<std::uint16_t>(
                "VELOCITY_QUESTDB_ILP_PORT", 9009),
            .redis_addr       = velocity::env::required<std::string>("VELOCITY_REDIS_ADDR"),
            .metrics_port     = velocity::env::optional<std::uint16_t>(
                "VELOCITY_METRICS_PORT", 9095),
            .flush_interval_ms = velocity::env::optional<std::uint32_t>(
                "VELOCITY_FLUSH_INTERVAL_MS", 250),
        };

        VLOG_INFO(
            "telemetry-ingester starting; brokers={} questdb={}:{}",
            cfg.brokers, cfg.questdb_host, cfg.questdb_ilp_port);

        Ingester ing{std::move(cfg)};
        ing.run();
        VLOG_INFO("shutdown complete");
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        VLOG_FATAL("fatal: {}", e.what());
        return EXIT_FAILURE;
    }
}
