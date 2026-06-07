# =============================================================================
#  libvelocity_common
#
#  Every C++ service links against this. Pure-utility code only: logging,
#  time, environment-variable parsing, signal handling. No service-specific
#  logic.
# =============================================================================

set(_module_name velocity_common)

add_library(${_module_name} STATIC
    src/log.cpp
    src/time.cpp
    src/env.cpp
    src/signals.cpp
    src/tracing.cpp
    src/orderbook.cpp
    src/replay_engine.cpp
)
add_library(velocity::common ALIAS ${_module_name})

target_include_directories(${_module_name}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(${_module_name}
    PUBLIC
        velocity::compile_options
        spdlog::spdlog
        fmt::fmt
        nlohmann_json::nlohmann_json
    PRIVATE
        absl::strings
        CURL::libcurl
)

# time.cpp stores a 24-byte std::atomic<Anchor>; objects > 16 bytes lower to
# libatomic calls (__atomic_load/__atomic_store) on GCC/Clang, so consumers
# must link libatomic. PUBLIC so every service that links velocity_common
# inherits it.
if(UNIX AND NOT APPLE)
    target_link_libraries(${_module_name} PUBLIC atomic)
endif()

set_target_properties(${_module_name} PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

if(VELOCITY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
