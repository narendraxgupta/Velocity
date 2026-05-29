# =============================================================================
#  velocity-scoring-service
#
#  Joins the per-submission latency stream (`metrics.latency.1s`) with the
#  per-submission correctness stream (`telemetry.fills`) and computes the
#  composite leaderboard score documented in `docs/scoring.md`.
#
#  Output sinks:
#    * Redis HASH  `scores:<submission_id>`   — current per-submission summary
#                                                 (read by bot-controller's
#                                                 BenchmarkService).
#    * Redis PUBSUB `leaderboard.global`      — JSON `LeaderboardDelta`s for
#                                                 the leaderboard-ws fan-out.
#    * Redis ZSET  `leaderboard:composite`    — sorted ranking for HTTP reads.
# =============================================================================

add_executable(velocity-scoring-service
    src/main.cpp
    src/scorer.cpp
    src/leaderboard_publisher.cpp
)

target_include_directories(velocity-scoring-service
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(velocity-scoring-service
    PRIVATE
        velocity::common
        velocity::compile_options
        velocity::proto
        spdlog::spdlog
        RdKafka::rdkafka++
        redis++::redis++_static
        nlohmann_json::nlohmann_json
)

install(TARGETS velocity-scoring-service RUNTIME DESTINATION bin)

if(VELOCITY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
