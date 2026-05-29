// =============================================================================
//  leaderboard_publisher.cpp — Redis write paths.
// =============================================================================

#include "scoring_service/leaderboard_publisher.h"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <sw/redis++/redis++.h>

#include "velocity/common/log.h"

namespace velocity::scoring_service {

namespace {

// Compose the per-submission HSET payload. We use HSET (a hash) so individual
// fields can be read by other services without parsing JSON.
auto submission_hash(const SubmissionScore& s)
    -> std::unordered_map<std::string, std::string> {
    return {
        {"submission_id",        s.submission_id},
        {"display_name",         s.display_name},
        {"team_name",            s.team_name},
        {"benchmark_id",         s.benchmark_id},

        {"sustained_rps",        std::to_string(s.sustained_rps)},
        {"target_rps",           std::to_string(s.target_rps)},
        {"p50_ns",               std::to_string(s.p50_ns)},
        {"p90_ns",               std::to_string(s.p90_ns)},
        {"p99_ns",               std::to_string(s.p99_ns)},
        {"p999_ns",              std::to_string(s.p999_ns)},
        {"max_ns",               std::to_string(s.max_ns)},

        {"expected_fills",       std::to_string(s.expected_fills)},
        {"actual_fills",         std::to_string(s.actual_fills)},
        {"correct_fills",        std::to_string(s.correct_fills)},
        {"priority_violations",  std::to_string(s.priority_violations)},
        {"price_violations",     std::to_string(s.price_violations)},
        {"phantom_fills",        std::to_string(s.phantom_fills)},
        {"missing_fills",        std::to_string(s.missing_fills)},

        {"throughput_score",     std::to_string(s.throughput_score)},
        {"latency_score",        std::to_string(s.latency_score)},
        {"correctness_score",    std::to_string(s.correctness_score)},
        {"penalty",              std::to_string(s.penalty)},
        {"composite_score",      std::to_string(s.composite_score)},

        {"updated_at_ns",        std::to_string(s.updated_at_ns)},
    };
}

}  // namespace

LeaderboardPublisher::LeaderboardPublisher(std::string redis_addr,
                                           std::string channel,
                                           std::string zset)
    : channel_(std::move(channel)), zset_(std::move(zset)) {
    redis_ = std::make_unique<sw::redis::Redis>(redis_addr);
    redis_->ping();  // Fast-fail if the connection is bad.
}

LeaderboardPublisher::~LeaderboardPublisher() = default;

auto LeaderboardPublisher::upsert(const SubmissionScore& s) -> void {
    try {
        const auto key = "scores:" + s.submission_id;
        const auto h   = submission_hash(s);
        redis_->hmset(key, h.begin(), h.end());
        redis_->expire(key, std::chrono::hours{6});   // GC abandoned runs

        // Sorted set keyed by composite score (descending = highest first via -score).
        redis_->zadd(zset_, s.submission_id, s.composite_score);
    } catch (const std::exception& e) {
        static std::atomic<int> warn{0};
        if ((warn++ % 64) == 0) VLOG_WARN("redis upsert failed: {}", e.what());
    }
}

auto LeaderboardPublisher::publish_delta(
    const std::unordered_map<std::string, SubmissionScore>& upserts) -> void {
    if (upserts.empty()) return;

    nlohmann::json j;
    j["type"] = "leaderboard.delta";
    j["ts_ns"] = std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::system_clock::now().time_since_epoch()).count();

    auto& arr = j["upserts"];
    for (const auto& [_, s] : upserts) {
        arr.push_back({
            {"submission_id",     s.submission_id},
            {"display_name",      s.display_name},
            {"team_name",         s.team_name},
            {"benchmark_id",      s.benchmark_id},
            {"composite_score",   s.composite_score},
            {"throughput_score",  s.throughput_score},
            {"latency_score",     s.latency_score},
            {"correctness_score", s.correctness_score},
            {"sustained_rps",     s.sustained_rps},
            {"p50_ns",            s.p50_ns},
            {"p99_ns",            s.p99_ns},
            {"updated_at_ns",     s.updated_at_ns},
        });
    }

    try {
        const auto payload = j.dump();
        redis_->publish(channel_, payload);
    } catch (const std::exception& e) {
        static std::atomic<int> warn{0};
        if ((warn++ % 64) == 0) VLOG_WARN("redis publish failed: {}", e.what());
    }
}

}  // namespace velocity::scoring_service
