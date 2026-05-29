// =============================================================================
//  scoring_service/scorer.h
//
//  The scoring service owns one Kafka consumer (multi-topic subscription on
//  `metrics.latency.1s` + `telemetry.fills`) and one Redis client. It keeps
//  per-submission state in memory (the live leaderboard is small —
//  hundreds of teams at most) and republishes a `LeaderboardDelta` on every
//  meaningful change.
//
//  The formula lives in `compute_*` free functions for trivial unit testing.
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace velocity::scoring_service {

struct ScorerConfig {
    std::string   brokers;
    std::string   group_id;
    std::string   latency_topic;       // "metrics.latency.1s"
    std::string   correctness_topic;   // "telemetry.fills"
    std::string   redis_addr;          // "tcp://redis:6379"
    std::string   leaderboard_channel; // "leaderboard.global"
    std::string   leaderboard_zset;    // "leaderboard:composite"
    std::uint32_t flush_interval_ms{500};

    // Score parameters (mirroring docs/scoring.md so tests can vary them).
    std::uint64_t baseline_latency_ns{30'000};   // 30 µs platform self-latency
    std::uint64_t default_target_rps{50'000};
};

class Scorer {
public:
    explicit Scorer(ScorerConfig cfg);
    ~Scorer();

    Scorer(const Scorer&)            = delete;
    Scorer& operator=(const Scorer&) = delete;

    auto run() -> void;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
//  Pure formula helpers — exposed for unit tests.
// ---------------------------------------------------------------------------

[[nodiscard]] auto throughput_score(std::uint64_t sustained_rps,
                                    std::uint64_t target_rps) noexcept -> double;

[[nodiscard]] auto latency_score(std::uint64_t p99_ns,
                                 std::uint64_t baseline_ns) noexcept -> double;

[[nodiscard]] auto composite(double throughput,
                             double latency,
                             double correctness,
                             double penalty) noexcept -> double;

}  // namespace velocity::scoring_service
