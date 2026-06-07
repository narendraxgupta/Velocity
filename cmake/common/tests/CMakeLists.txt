# =============================================================================
#  libvelocity_common — unit tests
# =============================================================================

add_executable(velocity_common_tests
    env_test.cpp
    time_test.cpp
    orderbook_test.cpp
    replay_engine_test.cpp
)

target_link_libraries(velocity_common_tests
    PRIVATE
        velocity::common
        velocity::compile_options
        GTest::gtest
        GTest::gtest_main
)

gtest_discover_tests(velocity_common_tests)
