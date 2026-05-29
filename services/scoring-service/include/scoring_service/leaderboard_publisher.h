// =============================================================================
//  scoring_service/leaderboard_publisher.h
//
//  Thin wrapper around sw::redis::Redis that knows how to:
//    * write per-submission summary hashes (`scores:<id>`)
//    * publish JSON deltas to `leaderboard.global`
//    * maintain the sorted set used by HTTP /v1/leaderboard endpoints
//
//  All writes are non-blocking from the caller's view and best-effort —
//  the scoring loop keeps going even if Redis is briefly unavailable. We
//  warn-once and continue.
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace sw::redis { class Redis; }

namespace velocity::scoring_service {

struct SubmissionScore {
    std::string   submission_id;
    std::string   display_name;
    std::string   team_name;
    std::string   benchmark_id;

    std::uint64_t sustained_rps{0};
    std::uint64_t target_rps{0};
    std::uint64_t p50_ns{0};
    std::uint64_t p90_ns{0};
    std::uint64_t p99_ns{0};
    std::uint64_t p999_ns{0};
    std::uint64_t max_ns{0};

    std::uint64_t expected_fills{0};
    std::uint64_t actual_fills{0};
    std::uint64_t correct_fills{0};
    std::uint64_t priority_violations{0};
    std::uint64_t price_violations{0};
    std::uint64_t phantom_fills{0};
    std::uint64_t missing_fills{0};

    double throughput_score{0.0};
    double latency_score{0.0};
    double correctness_score{0.0};
    double penalty{0.0};
    double composite_score{0.0};

    std::int64_t updated_at_ns{0};
};

class LeaderboardPublisher {
public:
    LeaderboardPublisher(std::string redis_addr,
                         std::string channel,
                         std::string zset);
    ~LeaderboardPublisher();

    auto upsert(const SubmissionScore& s) -> void;

    // Flush a `LeaderboardDelta`-shaped JSON payload to the pubsub channel.
    auto publish_delta(const std::unordered_map<std::string, SubmissionScore>& upserts)
        -> void;

private:
    std::unique_ptr<sw::redis::Redis> redis_;
    std::string                       channel_;
    std::string                       zset_;
};

}  // namespace velocity::scoring_service
