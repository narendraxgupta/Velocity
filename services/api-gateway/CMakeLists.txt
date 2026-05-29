# =============================================================================
#  velocity-api-gateway
#
#  Public HTTP/WebSocket entry point. Drogon for HTTP because (a) it's one of
#  the fastest C++ HTTP frameworks in the TechEmpower benchmarks, (b) it has
#  excellent WebSocket support that we'll need for live benchmark streams.
# =============================================================================

add_executable(velocity-api-gateway
    src/main.cpp
    src/server.cpp
    src/clients.cpp
    src/tenant.cpp
    src/rbac.cpp
    src/audit.cpp
    src/routes/health.cpp
    src/routes/auth.cpp
    src/routes/audit.cpp
    src/routes/submissions.cpp
    src/routes/benchmarks.cpp
    src/routes/fleet.cpp
    src/routes/leaderboard.cpp
    src/routes/chaos.cpp
    src/routes/pcaps.cpp
    src/routes/orderbook.cpp
    src/routes/marketdata.cpp
    src/routes/critiques.cpp
    src/routes/health_badges.cpp
    src/routes/adaptive_profile.cpp
    src/routes/plugins.cpp
    src/routes/share.cpp
)

target_include_directories(velocity-api-gateway
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

find_package(OpenSSL REQUIRED COMPONENTS Crypto)

target_link_libraries(velocity-api-gateway
    PRIVATE
        velocity::common
        velocity::compile_options
        velocity::proto
        drogon::drogon
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        gRPC::grpc++
        redis++::redis++_static
        # JWT verification uses HMAC-SHA256 from libcrypto. Linking
        # against the static target keeps the runtime image small.
        OpenSSL::Crypto
)

install(TARGETS velocity-api-gateway RUNTIME DESTINATION bin)
