# =============================================================================
#  velocity-leaderboard-ws
#
#  uWebSockets-backed WebSocket broadcast service. Subscribes to Redis
#  `leaderboard.updates` PUBSUB and relays JSON-encoded deltas to all
#  connected browsers. Single-threaded loop scales to ~100k concurrent
#  connections per core thanks to uSockets/io_uring.
# =============================================================================

add_executable(velocity-leaderboard-ws
    src/main.cpp
    src/server.cpp
    src/redis_subscriber.cpp
)

target_include_directories(velocity-leaderboard-ws
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(velocity-leaderboard-ws
    PRIVATE
        velocity::common
        velocity::compile_options
        velocity::proto
        spdlog::spdlog
        uwebsockets::uwebsockets
        usockets::usockets
        redis++::redis++_static
        nlohmann_json::nlohmann_json
)

install(TARGETS velocity-leaderboard-ws RUNTIME DESTINATION bin)
