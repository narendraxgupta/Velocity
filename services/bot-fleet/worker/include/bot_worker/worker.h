// =============================================================================
//  bot_worker/worker.h
//
//  Top-level worker process abstraction.
//
//  A worker hosts N reactor threads (one per available core), each
//  thread running thousands of trader state machines that talk to the
//  submission container using the configured transport (REST, WebSocket,
//  FIX 4.4). Each thread owns an SPSC ring buffer that drains into the
//  shared Redpanda publisher. Reactor thread count is taken from
//  `reactor_threads`; the worker substitutes `hardware_concurrency()`
//  when it sees zero.
//
//  Lifetime
//  --------
//  Construction parses config and dials the controller. `run()` enters the
//  bidirectional gRPC stream, services LoadPlan messages from the controller,
//  and exits on graceful Shutdown or shutdown_requested().
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace velocity::bot_worker {

struct WorkerConfig {
    std::string worker_id;             // ULID, persistent across restarts
    std::string controller_grpc_addr;  // host:port of the bot-controller
    std::string redpanda_brokers;      // comma-separated bootstrap servers
    std::string telemetry_topic;       // default "telemetry.raw"
    std::uint32_t reactor_threads;     // 0 = auto = std::thread::hardware_concurrency()
    std::uint32_t publish_buffer_capacity;  // SPSC ring-buffer size per thread
};

class Worker {
public:
    explicit Worker(WorkerConfig cfg);
    ~Worker();

    Worker(const Worker&)            = delete;
    Worker& operator=(const Worker&) = delete;

    auto run() -> void;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace velocity::bot_worker
