// =============================================================================
//  velocity-bot-worker — entrypoint.
//
//  Boots one Worker instance per process. Multiple workers are achieved by
//  scaling pod replicas (`docker compose up --scale bot-worker=N` or a K8s
//  Deployment with replicas: N).
// =============================================================================

#include <cstdlib>
#include <exception>
#include <string>
#include <thread>

#include <curl/curl.h>

#if defined(_WIN32)
#  include <process.h>
#  define velocity_getpid() static_cast<int>(::_getpid())
#else
#  include <unistd.h>
#  define velocity_getpid() static_cast<int>(::getpid())
#endif

#include "bot_worker/worker.h"
#include "velocity/common/env.h"
#include "velocity/common/log.h"
#include "velocity/common/signals.h"
#include "velocity/common/time.h"
#include "velocity/common/tracing.h"

using velocity::bot_worker::Worker;
using velocity::bot_worker::WorkerConfig;

namespace {

[[nodiscard]] auto load_config() -> WorkerConfig {
    return WorkerConfig{
        .worker_id              = velocity::env::optional<std::string>(
            "VELOCITY_WORKER_ID", "bw-" + std::to_string(velocity_getpid())),
        .controller_grpc_addr   = velocity::env::required<std::string>("VELOCITY_CONTROLLER_GRPC"),
        .redpanda_brokers       = velocity::env::required<std::string>("VELOCITY_REDPANDA_BROKERS"),
        .telemetry_topic        = velocity::env::optional<std::string>(
            "VELOCITY_TELEMETRY_TOPIC", "telemetry.raw"),
        .reactor_threads        = velocity::env::optional<std::uint32_t>(
            "VELOCITY_REACTOR_THREADS",
            static_cast<std::uint32_t>(std::thread::hardware_concurrency())),
        .publish_buffer_capacity = velocity::env::optional<std::uint32_t>(
            "VELOCITY_PUBLISH_BUFFER_CAPACITY", 65'536),
    };
}

}  // namespace

auto main(int /*argc*/, char* /*argv*/[]) -> int {
    velocity::log::init("bot-worker");
    velocity::time::init();
    velocity::signals::install_shutdown();

    // libcurl requires curl_global_init() to be called before any other
    // libcurl function and from a single thread. The REST transport relies
    // on this, so we initialise once here and always pair with cleanup.
    if (curl_global_init(CURL_GLOBAL_ALL) != 0) {
        VLOG_FATAL("curl_global_init() failed");
        return EXIT_FAILURE;
    }

    velocity::common::tracing::init(
        "bot-worker",
        velocity::env::optional<std::string>("VELOCITY_OTLP_ENDPOINT", ""));

    int exit_code = EXIT_SUCCESS;
    try {
        auto cfg = load_config();
        VLOG_INFO(
            "velocity-bot-worker {} starting; controller={} brokers={} reactors={}",
            cfg.worker_id, cfg.controller_grpc_addr, cfg.redpanda_brokers, cfg.reactor_threads);

        Worker w{std::move(cfg)};
        w.run();
        VLOG_INFO("shutdown complete");
    } catch (const std::exception& e) {
        VLOG_FATAL("fatal: {}", e.what());
        exit_code = EXIT_FAILURE;
    }
    velocity::common::tracing::shutdown();
    curl_global_cleanup();
    return exit_code;
}
