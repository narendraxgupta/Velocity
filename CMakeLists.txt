# =============================================================================
# Velocity — Top-level CMake configuration
#
# This file is the entry point for the *entire* C++ side of the platform.
# All C++ services are CMake subdirectories so a single `cmake --build .`
# from `build/` produces every binary.
#
# Conan provides third-party libraries; Conan also generates the toolchain
# file that CMake reads via `CMAKE_TOOLCHAIN_FILE`. Typical workflow:
#
#     conan install . --build=missing -s build_type=Release
#     cmake --preset conan-release
#     cmake --build --preset conan-release
#
# Layout
# ------
#   /                                  ← project root (this file)
#   /cmake/                            ← shared CMake modules
#   /proto/                            ← .proto files; gen/cpp/* is git-ignored
#   /services/api-gateway/             ← per-service subdir
#   /services/bot-fleet/{controller,worker}/
#   /services/telemetry-ingester/
#   /services/correctness-validator/
#   /services/leaderboard-ws/
#   /scripts/sample-exchange/          ← reference engine used in tests
# =============================================================================

cmake_minimum_required(VERSION 3.27)

# ---- Project ----------------------------------------------------------------
project(velocity
    VERSION      0.1.0
    DESCRIPTION  "Distributed benchmarking platform for trading infrastructure"
    LANGUAGES    CXX
)

# ---- Output discipline ------------------------------------------------------
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# ---- Options ----------------------------------------------------------------
option(VELOCITY_BUILD_TESTS    "Build unit & integration tests"      ON)
option(VELOCITY_BUILD_BENCHES  "Build microbenchmarks"               OFF)
option(VELOCITY_ENABLE_ASAN    "Build with AddressSanitizer"         OFF)
option(VELOCITY_ENABLE_TSAN    "Build with ThreadSanitizer"          OFF)
option(VELOCITY_ENABLE_UBSAN   "Build with UndefinedBehaviorSan"     OFF)
option(VELOCITY_USE_LTO        "Link-time optimization (Release)"    ON)
option(VELOCITY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

# ---- Shared compile / link policy ------------------------------------------
include(VelocityCompileOptions)
include(VelocitySanitizers)

# ---- Third-party dependencies (Conan-provided) ------------------------------
find_package(Protobuf  CONFIG REQUIRED)
find_package(gRPC      CONFIG REQUIRED)
find_package(absl      CONFIG REQUIRED)
find_package(spdlog    CONFIG REQUIRED)
find_package(fmt       CONFIG REQUIRED)
find_package(Boost     CONFIG REQUIRED)
find_package(prometheus-cpp CONFIG REQUIRED)
find_package(opentelemetry-cpp CONFIG REQUIRED)
find_package(RdKafka   CONFIG)        # provided by conan; optional for some services
find_package(hiredis   CONFIG)        # optional for some services
find_package(nlohmann_json CONFIG REQUIRED)
find_package(CURL      CONFIG REQUIRED)
find_package(hdr_histogram CONFIG REQUIRED)
find_package(redis++   CONFIG)
find_package(uwebsockets CONFIG)
find_package(usockets    CONFIG)
find_package(Drogon    CONFIG REQUIRED)

# ---- Code generation from proto ---------------------------------------------
include(VelocityProtoGen)
velocity_add_proto_library(
    NAME    velocity_proto
    PROTOS
        ${CMAKE_SOURCE_DIR}/proto/common.proto
        ${CMAKE_SOURCE_DIR}/proto/telemetry.proto
        ${CMAKE_SOURCE_DIR}/proto/bot.proto
        ${CMAKE_SOURCE_DIR}/proto/orchestrator.proto
        ${CMAKE_SOURCE_DIR}/proto/scoring.proto
    PROTO_ROOT
        ${CMAKE_SOURCE_DIR}/proto
)

# ---- Tests setup ------------------------------------------------------------
# Must run BEFORE the add_subdirectory() calls below so the test subdirectories
# can use the GTest::* targets and the gtest_discover_tests() command that
# include(GoogleTest) provides.
if(VELOCITY_BUILD_TESTS)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    include(GoogleTest)
endif()

# ---- Common library (used by every service) ---------------------------------
add_subdirectory(cmake/common)        # libvelocity_common: logging, time, config

# ---- Services ---------------------------------------------------------------
add_subdirectory(services/api-gateway)
add_subdirectory(services/bot-fleet/controller)
add_subdirectory(services/bot-fleet/worker)
add_subdirectory(services/telemetry-ingester)
add_subdirectory(services/correctness-validator)
add_subdirectory(services/leaderboard-ws)
add_subdirectory(services/scoring-service)

# ---- Reference exchange (used in CI to validate the platform) ---------------
add_subdirectory(scripts/sample-exchange)

# ---- Summary ----------------------------------------------------------------
message(STATUS "")
message(STATUS "Velocity build configuration")
message(STATUS "  Version:        ${velocity_VERSION}")
message(STATUS "  Build type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard:   ${CMAKE_CXX_STANDARD}")
message(STATUS "  Tests:          ${VELOCITY_BUILD_TESTS}")
message(STATUS "  Benchmarks:     ${VELOCITY_BUILD_BENCHES}")
message(STATUS "  LTO:            ${VELOCITY_USE_LTO}")
message(STATUS "  Sanitizers:     ASAN=${VELOCITY_ENABLE_ASAN} TSAN=${VELOCITY_ENABLE_TSAN} UBSAN=${VELOCITY_ENABLE_UBSAN}")
message(STATUS "")
