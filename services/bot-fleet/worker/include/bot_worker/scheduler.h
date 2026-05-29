// =============================================================================
//  bot_worker/scheduler.h
//
//  Open-loop scheduler with intended-send-time accounting (coordinated-
//  omission correction). See docs/adr/004-coordinated-omission.md.
//
//  The scheduler maintains a monotonically increasing "intended_ts" cursor
//  that advances by 1/λ regardless of whether the previous send completed.
//  This is what gives us honest tail latency.
// =============================================================================

#pragma once

#include <atomic>
#include <cstdint>

namespace velocity::bot_worker {

class Scheduler {
public:
    Scheduler(std::int64_t start_mono_ns, std::uint64_t initial_rps) noexcept;

    // Returns the next intended monotonic timestamp at which the reactor
    // should issue a request. Advances internal state by 1/λ.
    auto next() noexcept -> std::int64_t;

    // Update the current target RPS (called when the ramp interpolation
    // changes the rate). Safe to call from a different thread than next().
    auto set_rate(std::uint64_t rps) noexcept -> void;

    // Schedule skew = max(0, monotonic_now - last_intended). The higher this
    // is, the more behind we are — surfaced as a Prometheus metric.
    [[nodiscard]] auto skew_ns(std::int64_t now_mono_ns) const noexcept -> std::int64_t;

private:
    // `next_intended_ns_` is touched only from the reactor thread.
    std::int64_t                next_intended_ns_;
    // `step_ns_` is written by the worker thread (ramp updates) and read
    // by the reactor thread on every tick. Relaxed-ordering atomic is
    // sufficient: the value only needs to *eventually* converge to the
    // new target rate; an in-flight tick may use the previous step
    // without correctness impact.
    std::atomic<std::int64_t>   step_ns_;
};

}  // namespace velocity::bot_worker
