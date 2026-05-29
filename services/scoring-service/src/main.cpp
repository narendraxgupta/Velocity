// =============================================================================
//  velocity-scoring-service — entrypoint.
//
//  Environment:
//    VELOCITY_KAFKA_BROKERS         (required)  e.g. "redpanda:9092"
//    VELOCITY_REDIS_ADDR            (required)  e.g. "tcp://redis:6379"
//    VELOCITY_LATENCY_TOPIC         (default: "metrics.latency.1s")
//    VELOCITY_CORRECTNESS_TOPIC     (default: "telemetry.fills")
//    VELOCITY_LEADERBOARD_CHANNEL   (default: "leaderboard.global")
//    VELOCITY_LEADERBOARD_ZSET      (default: "leaderboard:composite")
//    VELOCITY_GROUP_ID              (default: "velocity-scoring")
//    VELOCITY_BASELINE_LATENCY_NS   (default: 30000)
//    VELOCITY_DEFAULT_TARGET_RPS    (default: 50000)
// =============================================================================

#include <cstdlib>
#include <exception>

#include "scoring_service/scorer.h"
#include "velocity/common/env.h"
#include "velocity/common/log.h"
#include "velocity/common/signals.h"
#include "velocity/common/time.h"

using velocity::scoring_service::Scorer;
using velocity::scoring_service::ScorerConfig;

auto main(int /*argc*/, char* /*argv*/[]) -> int {
    velocity::log::init("scoring-service");
    velocity::time::init();
    velocity::signals::install_shutdown();

    try {
        ScorerConfig cfg{
            .brokers              = velocity::env::required<std::string>("VELOCITY_KAFKA_BROKERS"),
            .group_id             = velocity::env::optional<std::string>("VELOCITY_GROUP_ID",
                                                                          "velocity-scoring"),
            .latency_topic        = velocity::env::optional<std::string>("VELOCITY_LATENCY_TOPIC",
                                                                          "metrics.latency.1s"),
            .correctness_topic    = velocity::env::optional<std::string>("VELOCITY_CORRECTNESS_TOPIC",
                                                                          "telemetry.fills"),
            .redis_addr           = velocity::env::required<std::string>("VELOCITY_REDIS_ADDR"),
            .leaderboard_channel  = velocity::env::optional<std::string>("VELOCITY_LEADERBOARD_CHANNEL",
                                                                          "leaderboard.global"),
            .leaderboard_zset     = velocity::env::optional<std::string>("VELOCITY_LEADERBOARD_ZSET",
                                                                          "leaderboard:composite"),
            .flush_interval_ms    = velocity::env::optional<std::uint32_t>("VELOCITY_FLUSH_INTERVAL_MS", 500),
            .baseline_latency_ns  = velocity::env::optional<std::uint64_t>("VELOCITY_BASELINE_LATENCY_NS", 30'000),
            .default_target_rps   = velocity::env::optional<std::uint64_t>("VELOCITY_DEFAULT_TARGET_RPS", 50'000),
        };

        VLOG_INFO("velocity-scoring-service starting; brokers={} redis={}",
                  cfg.brokers, cfg.redis_addr);

        Scorer s{std::move(cfg)};
        s.run();
        VLOG_INFO("shutdown complete");
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        VLOG_FATAL("fatal: {}", e.what());
        return EXIT_FAILURE;
    }
}
