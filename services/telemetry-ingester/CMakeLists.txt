# =============================================================================
#  velocity-telemetry-ingester
#
#  Consumes the `telemetry.raw` topic from Redpanda, drops every OrderEvent
#  into an in-process HdrHistogram keyed by submission_id, writes each event
#  to QuestDB via ILP, and periodically flushes histogram deltas to Redis.
# =============================================================================

add_executable(velocity-telemetry-ingester
    src/main.cpp
    src/ingester.cpp
    src/histograms.cpp
    src/questdb_writer.cpp
)

target_include_directories(velocity-telemetry-ingester
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(velocity-telemetry-ingester
    PRIVATE
        velocity::common
        velocity::compile_options
        velocity::proto
        spdlog::spdlog
        RdKafka::rdkafka++
        hdr_histogram::hdr_histogram_static
        redis++::redis++_static
)

install(TARGETS velocity-telemetry-ingester RUNTIME DESTINATION bin)
