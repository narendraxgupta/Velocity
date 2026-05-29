# =============================================================================
#  velocity-bot-worker
#
#  The load generator. C++20 + io_uring + uWebSockets, per-core thread model,
#  open-loop scheduler with intended-send-time correction (see ADR-004).
#
#  This is THE perf-critical binary. Everything else exists to make this one
#  work.
# =============================================================================

add_executable(velocity-bot-worker
    src/main.cpp
    src/worker.cpp
    src/scheduler.cpp
    src/plan.cpp
    src/persona.cpp
    src/personas/market_maker.cpp
    src/personas/aggressive_taker.cpp
    src/personas/canceller.cpp
    src/personas/spoofer.cpp
    src/personas/noise.cpp
    src/transports/rest.cpp
    src/transports/websocket.cpp
    src/transports/fix.cpp
    src/publishers/redpanda_publisher.cpp
    src/reactor.cpp
    src/ebpf_probe.cpp
    src/rl_policy.cpp
)

target_include_directories(velocity-bot-worker
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(velocity-bot-worker
    PRIVATE
        velocity::common
        velocity::compile_options
        velocity::proto
        spdlog::spdlog
        # liburing — io_uring wrapper (Linux only)
        $<$<PLATFORM_ID:Linux>:uring>
        # boost::lockfree::spsc_queue for SPSC publish queues
        Boost::headers
        # libcurl for the portable REST transport (multi-handle)
        CURL::libcurl
        # uWebSockets + uSockets for the WS transport
        uwebsockets::uwebsockets
        usockets::usockets
        # Kafka producer
        RdKafka::rdkafka++
        # gRPC for controller comms
        gRPC::grpc++
        # Histograms recorded per worker
        hdr_histogram::hdr_histogram_static
        # nlohmann/json for REST payload encoding
        nlohmann_json::nlohmann_json
)

# io_uring is Linux-only; make that visible at compile time so we can `#ifdef`
# fallbacks in dev builds on macOS.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(velocity-bot-worker PRIVATE VELOCITY_HAS_IO_URING=1)
else()
    target_compile_definitions(velocity-bot-worker PRIVATE VELOCITY_HAS_IO_URING=0)
endif()

# =============================================================================
#  Kernel-level latency probe (eBPF)
# =============================================================================
#
# Off by default — the toolchain needs clang ≥ 12 + bpftool + libbpf-dev, and
# the production runtime needs Linux ≥ 5.4 with CAP_BPF / CAP_PERFMON. When
# disabled, ebpf_probe.cpp compiles to a no-op stub so the worker still
# builds + links on macOS dev boxes and on hosts without BPF support.
#
# Enable with `cmake -DVELOCITY_ENABLE_EBPF=ON`. The CI image
# (`platform/infra/docker/bot-worker.Dockerfile`) sets this in its release
# build.
option(VELOCITY_ENABLE_EBPF "Build kernel-level TCP latency kprobes" OFF)

if(VELOCITY_ENABLE_EBPF)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR
            "VELOCITY_ENABLE_EBPF requires Linux; current platform is "
            "${CMAKE_SYSTEM_NAME}")
    endif()

    find_program(CLANG_BPF_EXEC NAMES clang REQUIRED)
    find_program(BPFTOOL_EXEC   NAMES bpftool REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBBPF REQUIRED IMPORTED_TARGET libbpf)

    set(BPF_SRC ${CMAKE_CURRENT_SOURCE_DIR}/ebpf/latency_probe.bpf.c)
    set(BPF_OBJ ${CMAKE_CURRENT_BINARY_DIR}/latency_probe.bpf.o)
    set(BPF_SKEL ${CMAKE_CURRENT_BINARY_DIR}/latency_probe.skel.h)

    # 1. Compile the BPF C → object file using clang + the BPF target.
    add_custom_command(
        OUTPUT ${BPF_OBJ}
        COMMAND ${CLANG_BPF_EXEC}
                -O2 -g -target bpf
                -D__TARGET_ARCH_$ENV{CPU}
                -I${CMAKE_CURRENT_SOURCE_DIR}/ebpf
                -c ${BPF_SRC}
                -o ${BPF_OBJ}
        DEPENDS ${BPF_SRC}
        COMMENT "Compiling eBPF object: latency_probe.bpf.o"
        VERBATIM
    )

    # 2. Generate the skeleton header so C++ can refer to maps + progs by
    #    name (skel->maps.events, skel->progs.tcp_sendmsg_entry, ...).
    add_custom_command(
        OUTPUT ${BPF_SKEL}
        COMMAND ${BPFTOOL_EXEC} gen skeleton ${BPF_OBJ} > ${BPF_SKEL}
        DEPENDS ${BPF_OBJ}
        COMMENT "Generating BPF skeleton header: latency_probe.skel.h"
        VERBATIM
    )

    add_custom_target(velocity-bot-worker-bpf DEPENDS ${BPF_SKEL})
    add_dependencies(velocity-bot-worker velocity-bot-worker-bpf)

    target_include_directories(velocity-bot-worker
        PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

    target_compile_definitions(velocity-bot-worker
        PRIVATE VELOCITY_ENABLE_EBPF=1)
    target_link_libraries(velocity-bot-worker
        PRIVATE PkgConfig::LIBBPF)
else()
    target_compile_definitions(velocity-bot-worker
        PRIVATE VELOCITY_ENABLE_EBPF=0)
endif()

# =============================================================================
#  ONNX Runtime — adaptive (RL-trained) persona inference.
# =============================================================================
#
# Off by default because the dep ships a ~30 MB shared lib that we don't
# want in every dev image. The CI image for prod-bot-worker turns this
# on; without it the ADAPTIVE persona falls back to market_maker at
# runtime (see persona.cpp).
#
# Enable with `cmake -DVELOCITY_ENABLE_ONNXRUNTIME=ON`. On systems where
# the package isn't discoverable we soft-fail with a status message
# rather than aborting the configure step — keeps the dev loop fast.
option(VELOCITY_ENABLE_ONNXRUNTIME "Build with ONNX Runtime for adaptive RL persona" OFF)

if(VELOCITY_ENABLE_ONNXRUNTIME)
    find_package(onnxruntime CONFIG QUIET)
    if(onnxruntime_FOUND)
        target_compile_definitions(velocity-bot-worker
            PRIVATE VELOCITY_HAS_ONNXRUNTIME=1)
        target_link_libraries(velocity-bot-worker
            PRIVATE onnxruntime::onnxruntime)
        message(STATUS "Adaptive RL persona: ONNX Runtime ENABLED")
    else()
        message(WARNING
            "VELOCITY_ENABLE_ONNXRUNTIME=ON but onnxruntime not found via "
            "find_package; building with no-op stubs. Install the SDK or "
            "pin a Conan/vcpkg port to enable the ADAPTIVE persona.")
    endif()
endif()

install(TARGETS velocity-bot-worker RUNTIME DESTINATION bin)
