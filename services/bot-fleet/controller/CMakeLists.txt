# =============================================================================
#  velocity-bot-controller
#
#  Singleton service that:
#    * Listens for StartBenchmark RPCs from the API gateway.
#    * Holds a bidirectional gRPC stream with every healthy bot-worker.
#    * Divides a global LoadPlan into per-worker plans.
#    * Aggregates worker StatusUpdate messages into a BenchmarkSnapshot.
# =============================================================================

add_executable(velocity-bot-controller
    src/main.cpp
    src/controller_service.cpp
    src/benchmark_service.cpp
    src/load_planner.cpp
)

target_include_directories(velocity-bot-controller
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(velocity-bot-controller
    PRIVATE
        velocity::common
        velocity::compile_options
        velocity::proto
        gRPC::grpc++
        gRPC::grpc++_reflection
        redis++::redis++_static
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)

install(TARGETS velocity-bot-controller RUNTIME DESTINATION bin)
