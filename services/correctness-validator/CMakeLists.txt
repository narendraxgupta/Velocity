# =============================================================================
#  velocity-correctness-validator
#
#  Replays the bot's order stream through a reference matching engine and
#  compares to the submission's reported fills.
# =============================================================================

add_executable(velocity-correctness-validator
    src/main.cpp
    src/validator.cpp
    src/reference_orderbook.cpp
    src/microstructure.cpp
    src/execution_quality.cpp
    src/plugin_fanout.cpp
)

target_include_directories(velocity-correctness-validator
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(velocity-correctness-validator
    PRIVATE
        velocity::common
        velocity::compile_options
        velocity::proto
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        redis++::redis++_static
        RdKafka::rdkafka++
        Boost::headers   # Boost.Intrusive for the orderbook
)

if(VELOCITY_BUILD_TESTS)
    add_subdirectory(tests)
endif()

install(TARGETS velocity-correctness-validator RUNTIME DESTINATION bin)
