// =============================================================================
//  env_test.cpp — covers velocity::env type-safe parsing.
// =============================================================================

#include "velocity/common/env.h"

#include <cstdlib>

#include <gtest/gtest.h>

using velocity::env::bad_env;
using velocity::env::missing_env;

namespace {

class EnvFixture : public ::testing::Test {
protected:
    auto set(const char* k, const char* v) -> void { ::setenv(k, v, 1); }
    auto unset(const char* k) -> void { ::unsetenv(k); }
};

TEST_F(EnvFixture, RequiredReturnsParsedInteger) {
    set("VELOCITY_TEST_INT", "42");
    EXPECT_EQ(velocity::env::required<std::int32_t>("VELOCITY_TEST_INT"), 42);
    unset("VELOCITY_TEST_INT");
}

TEST_F(EnvFixture, RequiredThrowsOnMissing) {
    unset("VELOCITY_TEST_MISSING");
    EXPECT_THROW(velocity::env::required<std::string>("VELOCITY_TEST_MISSING"), missing_env);
}

TEST_F(EnvFixture, RequiredThrowsOnBadInteger) {
    set("VELOCITY_TEST_BAD", "not-a-number");
    EXPECT_THROW(velocity::env::required<std::int32_t>("VELOCITY_TEST_BAD"), bad_env);
    unset("VELOCITY_TEST_BAD");
}

TEST_F(EnvFixture, OptionalReturnsDefaultWhenUnset) {
    unset("VELOCITY_TEST_UNSET");
    EXPECT_EQ(velocity::env::optional<std::int32_t>("VELOCITY_TEST_UNSET", 7), 7);
}

TEST_F(EnvFixture, BoolAcceptsCommonForms) {
    set("VELOCITY_TEST_BOOL", "yes");
    EXPECT_TRUE(velocity::env::required<bool>("VELOCITY_TEST_BOOL"));
    set("VELOCITY_TEST_BOOL", "false");
    EXPECT_FALSE(velocity::env::required<bool>("VELOCITY_TEST_BOOL"));
    unset("VELOCITY_TEST_BOOL");
}

}  // namespace
