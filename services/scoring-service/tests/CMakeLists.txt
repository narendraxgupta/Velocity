add_executable(velocity_scorer_tests
    scorer_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/scorer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/leaderboard_publisher.cpp
)

target_link_libraries(velocity_scorer_tests
    PRIVATE
        velocity::compile_options
        velocity::common
        velocity::proto
        RdKafka::rdkafka++
        redis++::redis++_static
        nlohmann_json::nlohmann_json
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(velocity_scorer_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

gtest_discover_tests(velocity_scorer_tests)
