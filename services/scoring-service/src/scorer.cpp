// =============================================================================
//  scorer.cpp — the join / score / publish loop.
//
//  Source topics:
//    * metrics.latency.1s : LatencyBucket  (per-submission rolling p50/p90/p99)
//    * telemetry.fills    : CorrectnessReport (per-submission fill accuracy)
//
//  We index the latest message per submission by `submission_id` and emit a
//  scored record whenever EITHER side ticks. The first-ever publish for a
//  submission waits until at least one of each kind has arrived, so we never
//  publish a half-formed entry to the leaderboard.
// =============================================================================

#include "scoring_service/scorer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <librdkafka/rdkafkacpp.h>

#include "common.pb.h"
#include "telemetry.pb.h"

#include "scoring_service/leaderboard_publisher.h"

#include "velocity/common/log.h"
#include "velocity/common/signals.h"
#include "velocity/common/time.h"

namespace velocity::scoring_service {

// ---------------------------------------------------------------------------
//  Formula helpers
// ---------------------------------------------------------------------------

auto throughput_score(std::uint64_t sustained_rps, std::uint64_t target_rps) noexcept
    -> double {
    if (target_rps == 0) return 0.0;
    const double ratio = static_cast<double>(sustained_rps) /
                         static_cast<double>(target_rps);
    return 100.0 * std::min(1.0, ratio);
}

auto latency_score(std::uint64_t p99_ns, std::uint64_t baseline_ns) noexcept -> double {
    if (baseline_ns == 0) return 0.0;
    // p99_ns == 0 means the HdrHistogram had ZERO samples — i.e. the
    // submission completed no orders in the window, not that it responded
    // infinitely fast. Treat "no data" as no credit; otherwise a submission
    // that never acks a single order wins the latency component outright.
    if (p99_ns == 0) return 0.0;
    if (p99_ns <= baseline_ns) return 100.0;
    const double delta = static_cast<double>(p99_ns - baseline_ns);
    const double base  = static_cast<double>(baseline_ns);
    return std::max(0.0, 100.0 - 100.0 * (delta / base));
}

auto composite(double t, double l, double c, double p) noexcept -> double {
    return std::max(0.0, 0.40 * t + 0.35 * l + 0.25 * c - p);
}

namespace {

// Per-submission joined state.
struct State {
    SubmissionScore        score;
    bool                   has_latency{false};
    bool                   has_correctness{false};
    std::vector<std::uint64_t> rps_samples;   // for sustained-rps p10
};

[[nodiscard]] auto compute_penalty(const SubmissionScore& s) noexcept -> double {
    double p = 0.0;
    p += 5.0 * static_cast<double>(s.price_violations);
    p += 3.0 * static_cast<double>(s.priority_violations);
    p += 5.0 * static_cast<double>(s.phantom_fills);
    p += 2.0 * static_cast<double>(s.missing_fills);
    return std::min(50.0, p);
}

}  // namespace

// ---------------------------------------------------------------------------
//  Impl
// ---------------------------------------------------------------------------
struct Scorer::Impl {
    ScorerConfig                            cfg;
    std::unique_ptr<RdKafka::KafkaConsumer> consumer;
    std::unique_ptr<LeaderboardPublisher>   publisher;
    std::unordered_map<std::string, State>  states;
    std::unordered_map<std::string, SubmissionScore> dirty;
    std::int64_t                            last_flush_ns{0};

    explicit Impl(ScorerConfig c) : cfg(std::move(c)) {
        std::string errstr;
        std::unique_ptr<RdKafka::Conf> cc{RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL)};
        cc->set("bootstrap.servers",   cfg.brokers,  errstr);
        cc->set("group.id",            cfg.group_id, errstr);
        cc->set("enable.auto.commit",  "true",       errstr);
        cc->set("auto.commit.interval.ms", "2000",   errstr);
        cc->set("fetch.min.bytes",     "16384",      errstr);
        cc->set("fetch.wait.max.ms",   "50",         errstr);
        cc->set("auto.offset.reset",   "latest",     errstr);
        consumer.reset(RdKafka::KafkaConsumer::create(cc.get(), errstr));
        if (!consumer) throw std::runtime_error("consumer failed: " + errstr);
        const auto sub = consumer->subscribe({cfg.latency_topic, cfg.correctness_topic});
        if (sub != RdKafka::ERR_NO_ERROR) {
            throw std::runtime_error("subscribe failed: " + RdKafka::err2str(sub));
        }

        publisher = std::make_unique<LeaderboardPublisher>(
            cfg.redis_addr, cfg.leaderboard_channel, cfg.leaderboard_zset);

        last_flush_ns = velocity::time::realtime_ns();
        VLOG_INFO("scorer subscribed to '{}' + '{}'; redis={}",
                  cfg.latency_topic, cfg.correctness_topic, cfg.redis_addr);
    }

    auto run() -> void {
        const auto period = std::chrono::milliseconds(cfg.flush_interval_ms);
        auto next_flush = std::chrono::steady_clock::now() + period;

        while (!velocity::signals::shutdown_requested()) {
            std::unique_ptr<RdKafka::Message> msg{consumer->consume(100)};
            if (msg && msg->err() == RdKafka::ERR_NO_ERROR) {
                handle_(*msg);
            } else if (msg && msg->err() != RdKafka::ERR__TIMED_OUT &&
                       msg->err() != RdKafka::ERR__PARTITION_EOF) {
                VLOG_WARN("consume error: {}", msg->errstr());
            }
            if (std::chrono::steady_clock::now() >= next_flush) {
                flush_();
                next_flush = std::chrono::steady_clock::now() + period;
            }
        }
        flush_();
        consumer->close();
    }

    auto handle_(RdKafka::Message& msg) -> void {
        if (msg.topic_name() == cfg.latency_topic) {
            velocity::telemetry::v1::LatencyBucket lb;
            if (!lb.ParseFromArray(msg.payload(), static_cast<int>(msg.len()))) {
                VLOG_WARN("scorer: malformed LatencyBucket on {}", cfg.latency_topic);
                return;
            }
            const auto& sid = lb.submission_id().value();
            auto& st = states[sid];
            st.score.submission_id = sid;
            st.score.p50_ns  = lb.p50_ns();
            st.score.p90_ns  = lb.p90_ns();
            st.score.p99_ns  = lb.p99_ns();
            st.score.p999_ns = lb.p999_ns();
            st.score.max_ns  = lb.max_ns();

            // Convert the bucket's sample `count` to a real RPS by
            // dividing by the window width. The window is published in
            // window_{start,end}_ns (proto telemetry.proto §LatencyBucket).
            // Without the window math we previously treated 12,000 samples
            // in a 250 ms window as "12,000 RPS" instead of ~48,000 RPS.
            const auto win_ns = lb.window_end_ns() > lb.window_start_ns()
                ? lb.window_end_ns() - lb.window_start_ns()
                : 1'000'000'000LL;  // assume 1s if window missing
            const auto rps = win_ns > 0
                ? static_cast<std::uint64_t>(
                      (static_cast<__int128>(lb.count()) * 1'000'000'000LL) / win_ns)
                : 0ULL;
            st.score.sustained_rps = rps;
            st.rps_samples.push_back(rps);
            if (st.rps_samples.size() > 120) {
                st.rps_samples.erase(st.rps_samples.begin());
            }
            st.has_latency = true;
            recompute_(sid, st);
        } else if (msg.topic_name() == cfg.correctness_topic) {
            velocity::telemetry::v1::CorrectnessReport cr;
            if (!cr.ParseFromArray(msg.payload(), static_cast<int>(msg.len()))) {
                VLOG_WARN("scorer: malformed CorrectnessReport on {}",
                          cfg.correctness_topic);
                return;
            }
            const auto& sid = cr.submission_id().value();
            auto& st = states[sid];
            st.score.submission_id        = sid;
            st.score.expected_fills       = cr.expected_fills();
            st.score.actual_fills         = cr.actual_fills();
            st.score.correct_fills        = cr.correct_fills();
            st.score.priority_violations  = cr.priority_violations();
            st.score.price_violations     = cr.price_violations();
            st.score.phantom_fills        = cr.phantom_fills();
            st.score.missing_fills        = cr.missing_fills();
            st.score.correctness_score    = cr.correctness_score();
            st.has_correctness = true;
            recompute_(sid, st);
        }
    }

    auto recompute_(const std::string& sid, State& st) -> void {
        // Sustained = p10 (nearest-rank) of recent 1s samples — matches
        // docs/scoring.md §6.
        std::uint64_t sustained = st.score.sustained_rps;
        if (!st.rps_samples.empty()) {
            std::vector<std::uint64_t> s = st.rps_samples;
            std::sort(s.begin(), s.end());
            const auto n = s.size();
            const auto idx = std::min<std::size_t>(
                n - 1,
                static_cast<std::size_t>(std::max<double>(1.0, std::ceil(0.10 * static_cast<double>(n)))) - 1);
            sustained = s[idx];
        }
        st.score.sustained_rps   = sustained;
        st.score.target_rps      = cfg.default_target_rps;
        st.score.throughput_score = throughput_score(sustained, cfg.default_target_rps);
        st.score.latency_score    = latency_score(st.score.p99_ns, cfg.baseline_latency_ns);
        st.score.penalty          = compute_penalty(st.score);
        st.score.composite_score  = composite(st.score.throughput_score,
                                              st.score.latency_score,
                                              st.score.correctness_score,
                                              st.score.penalty);
        st.score.updated_at_ns = velocity::time::realtime_ns();

        if (st.has_latency && st.has_correctness) {
            dirty[sid] = st.score;
        }
    }

    auto flush_() -> void {
        if (dirty.empty()) return;
        for (const auto& [_, s] : dirty) {
            publisher->upsert(s);
        }
        publisher->publish_delta(dirty);
        dirty.clear();
    }
};

Scorer::Scorer(ScorerConfig cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}
Scorer::~Scorer() = default;
auto Scorer::run() -> void { impl_->run(); }

}  // namespace velocity::scoring_service
