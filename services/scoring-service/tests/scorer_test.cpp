// =============================================================================
//  scorer_test.cpp — sanity-check the pure scoring formulas.
//
//  We test the formula helpers, not the full Kafka/Redis loop. The loop is
//  exercised end-to-end by the platform smoke test under
//  `tests/e2e/smoke.sh`.
// =============================================================================

#include "scoring_service/scorer.h"

#include <gtest/gtest.h>

namespace v = velocity::scoring_service;

TEST(ThroughputScore, ExactlyMatchesTargetCaps100) {
    EXPECT_NEAR(v::throughput_score(50'000, 50'000), 100.0, 1e-9);
}

TEST(ThroughputScore, OverTargetStillCaps100) {
    EXPECT_NEAR(v::throughput_score(200'000, 50'000), 100.0, 1e-9);
}

TEST(ThroughputScore, HalfTargetGivesHalfScore) {
    EXPECT_NEAR(v::throughput_score(25'000, 50'000), 50.0, 1e-9);
}

TEST(ThroughputScore, ZeroTargetIsZero) {
    EXPECT_NEAR(v::throughput_score(50'000, 0), 0.0, 1e-9);
}

TEST(LatencyScore, BelowBaselineIsPerfect) {
    EXPECT_NEAR(v::latency_score(20'000, 30'000), 100.0, 1e-9);
}

TEST(LatencyScore, ExactlyBaselineIsPerfect) {
    EXPECT_NEAR(v::latency_score(30'000, 30'000), 100.0, 1e-9);
}

TEST(LatencyScore, TwiceBaselineGivesZero) {
    // 100 - 100 * (60-30)/30 = 0
    EXPECT_NEAR(v::latency_score(60'000, 30'000), 0.0, 1e-9);
}

TEST(LatencyScore, BeyondTwiceBaselineClampsZero) {
    EXPECT_NEAR(v::latency_score(120'000, 30'000), 0.0, 1e-9);
}

TEST(LatencyScore, ZeroP99IsNoDataNotPerfect) {
    // An empty HdrHistogram yields p99 == 0 (no orders completed). That must
    // score 0, not a perfect 100 — otherwise a submission that never acks a
    // single order would top the latency component.
    EXPECT_NEAR(v::latency_score(0, 30'000), 0.0, 1e-9);
}

TEST(Composite, WeightsApplyCorrectly) {
    // Worked example from docs/scoring.md §8.
    const auto s = v::composite(95.0, 0.0, 99.98, 6.0);
    EXPECT_NEAR(s, 0.40 * 95.0 + 0.35 * 0.0 + 0.25 * 99.98 - 6.0, 1e-9);
}

TEST(Composite, ClampsAtZero) {
    EXPECT_EQ(v::composite(0.0, 0.0, 0.0, 50.0), 0.0);
}
