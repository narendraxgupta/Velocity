// =============================================================================
//  bot_worker/reactor.h
//
//  A reactor owns:
//    * one transport (REST, WS, or FIX) to the target submission
//    * N bot personas
//    * one scheduler shared across all its bots
//    * a producer-side handle to the publisher's SPSC queue
//
//  Reactors are run on dedicated threads (jthread) pinned to a CPU. They
//  yield only inside the transport's poll() — never via sleep(), never via
//  condition variables — so the loop is responsive to single-µs ticks.
// =============================================================================

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace velocity::bot_worker {

class  Publisher;
struct LoadPlan;
class  Transport;
class  RLPolicy;

struct ReactorConfig {
    std::uint8_t  reactor_id;
    std::uint32_t bots_per_reactor;
    std::int64_t  start_mono_ns;
    std::uint64_t initial_rps;
    // Optional shared RL policy. Reactors hold a copy of the shared
    // pointer (cheap atomic increment) and pass it to ADAPTIVE
    // personas. nullptr → personas fall back to MARKET_MAKER.
    std::shared_ptr<const RLPolicy> rl_policy;
};

class Reactor {
public:
    Reactor(ReactorConfig cfg,
            const LoadPlan* plan,
            std::unique_ptr<Transport> transport,
            Publisher* publisher) noexcept;
    ~Reactor();

    Reactor(const Reactor&)            = delete;
    Reactor& operator=(const Reactor&) = delete;

    // Spawn the reactor's worker thread and run until stop() is called.
    auto start() -> void;
    auto stop() noexcept -> void;

    // Update target RPS mid-flight (called from the controller bidi stream).
    auto set_rate(std::uint64_t rps) noexcept -> void;

    // Counters — surfaced to Prometheus / controller heartbeats.
    [[nodiscard]] auto orders_sent() const noexcept -> std::uint64_t;
    // Completions observed via the transport ack callback. `acked` counts
    // ACK/FILLED/PARTIAL outcomes; `errored` counts REJECT/TIMEOUT.
    [[nodiscard]] auto orders_acked() const noexcept -> std::uint64_t;
    [[nodiscard]] auto orders_errored() const noexcept -> std::uint64_t;
    [[nodiscard]] auto skew_ns() const noexcept -> std::int64_t;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace velocity::bot_worker
