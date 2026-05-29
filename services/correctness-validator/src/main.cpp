// =============================================================================
//  velocity-correctness-validator — entrypoint.
// =============================================================================

#include <cstdlib>
#include <exception>

#include "correctness_validator/validator.h"
#include "velocity/common/env.h"
#include "velocity/common/log.h"
#include "velocity/common/signals.h"
#include "velocity/common/time.h"

auto main(int /*argc*/, char* /*argv*/[]) -> int {
    velocity::log::init("correctness-validator");
    velocity::time::init();
    velocity::signals::install_shutdown();

    try {
        velocity::correctness_validator::ValidatorConfig cfg{
            .brokers          = velocity::env::required<std::string>("VELOCITY_REDPANDA_BROKERS"),
            .group_id         = velocity::env::optional<std::string>(
                "VELOCITY_KAFKA_GROUP_ID", "velocity-validator"),
            .telemetry_topic  = velocity::env::optional<std::string>(
                "VELOCITY_TELEMETRY_TOPIC", "telemetry.raw"),
            .fills_topic      = velocity::env::optional<std::string>(
                "VELOCITY_FILLS_TOPIC", "telemetry.fills"),
            .questdb_host     = velocity::env::required<std::string>("VELOCITY_QUESTDB_HOST"),
            .questdb_ilp_port = velocity::env::optional<std::uint16_t>(
                "VELOCITY_QUESTDB_ILP_PORT", 9009),
            .metrics_port     = velocity::env::optional<std::uint16_t>(
                "VELOCITY_METRICS_PORT", 9096),
            .redis_addr       = velocity::env::optional<std::string>(
                "VELOCITY_REDIS_ADDR", ""),
            .mismatches_cap   = velocity::env::optional<std::uint32_t>(
                "VELOCITY_VALIDATOR_MISMATCHES_CAP", 200),
        };

        VLOG_INFO("correctness-validator starting; brokers={}", cfg.brokers);
        velocity::correctness_validator::run(std::move(cfg));
        VLOG_INFO("shutdown complete");
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        VLOG_FATAL("fatal: {}", e.what());
        return EXIT_FAILURE;
    }
}
