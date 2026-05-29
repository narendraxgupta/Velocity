// =============================================================================
//  time_test.cpp — covers monotonic clock helpers.
// =============================================================================

#include "velocity/common/time.h"

#include <thread>

#include <gtest/gtest.h>

TEST(Time, MonotonicIsMonotonic) {
    const auto a = velocity::time::monotonic_ns();
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    const auto b = velocity::time::monotonic_ns();
    EXPECT_GT(b, a);
}

TEST(Time, WallclockTranslationIsReasonable) {
    velocity::time::init();
    const auto mono = velocity::time::monotonic_ns();
    const auto wc   = velocity::time::monotonic_to_wallclock_ns(mono);
    const auto raw  = velocity::time::realtime_ns();
    // Within 1ms tolerance — the anchor capture has microsecond-level
    // bounded error, generous tolerance here.
    EXPECT_NEAR(static_cast<double>(wc), static_cast<double>(raw), 1'000'000.0);
}

TEST(Time, PrettyFormatPicksRightUnit) {
    using velocity::time::pretty_ns;
    EXPECT_EQ(pretty_ns(500),               "500ns");
    EXPECT_NE(pretty_ns(1'500).find("µs"),  std::string::npos);
    EXPECT_NE(pretty_ns(1'500'000).find("ms"), std::string::npos);
    EXPECT_NE(pretty_ns(2'500'000'000).find("s"), std::string::npos);
}
