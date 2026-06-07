#include "velocity/replay_engine.h"

#include <gtest/gtest.h>

using namespace velocity;

class TestReplayEngine : public ReplayEngine {
protected:
    std::vector<OrderPlacedEvent> LoadBenchmarkEvents(const std::string& benchmark_id) override {
        if (benchmark_id == "valid-benchmark") {
            return {
                OrderPlacedEvent{
                                 .order_id = "ask-1",
                                 .submission_id = "sub-1",
                                 .instrument = "XBT/USD",
                                 .is_buy = false,
                                 .price_ticks = 10000,
                                 .quantity = 10,
                                 .intended_send_ns = 1,
                                 },
                OrderPlacedEvent{
                                 .order_id = "bid-1",
                                 .submission_id = "sub-2",
                                 .instrument = "XBT/USD",
                                 .is_buy = true,
                                 .price_ticks = 10050,
                                 .quantity = 6,
                                 .intended_send_ns = 2,
                                 },
            };
        }

        if (benchmark_id == "invalid-benchmark") {
            return {
                OrderPlacedEvent{
                                 .order_id = "bad-1",
                                 .submission_id = "sub-3",
                                 .instrument = "XBT/USD",
                                 .is_buy = true,
                                 .price_ticks = 0,
                                 .quantity = 5,
                                 .intended_send_ns = 3,
                                 },
            };
        }

        return {};
    }
};

TEST(ReplayEngineTest, ReplaysValidBenchmarkWithoutViolations) {
    TestReplayEngine engine;
    int violation_count = 0;
    bool passed = engine.ReplayBenchmark("valid-benchmark", [&](const std::string& payload) {
        (void)payload;
        violation_count++;
    });

    EXPECT_TRUE(passed);
    EXPECT_EQ(violation_count, 0);
}

TEST(ReplayEngineTest, ReportsViolationForInvalidBenchmark) {
    TestReplayEngine engine;
    int violation_count = 0;
    bool passed = engine.ReplayBenchmark("invalid-benchmark", [&](const std::string& payload) {
        EXPECT_FALSE(payload.empty());
        violation_count++;
    });

    EXPECT_FALSE(passed);
    EXPECT_GE(violation_count, 1);
}
