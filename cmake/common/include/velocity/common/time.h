// =============================================================================
//  velocity/common/time.h
//
//  Time primitives used across the platform.
//
//  Why our own header instead of `<chrono>` everywhere?
//
//  Because every nanosecond in a benchmarking platform is a load-bearing
//  abstraction. We standardize on:
//
//    * `clock_gettime(CLOCK_MONOTONIC_RAW)` for measurement deltas.
//    * `clock_gettime(CLOCK_REALTIME)` for wall-clock stamps the outside
//      world will see.
//    * A single anchor pair captured at process start to translate from
//      MONOTONIC_RAW to wall-clock nanos.
//
//  Anything else (NTP-slewing `CLOCK_MONOTONIC`, `std::chrono::system_clock`
//  with sub-microsecond promises) is a foot-gun and is forbidden in this
//  codebase.
// =============================================================================

#pragma once

#include <chrono>
#include <cstdint>

namespace velocity::time {

// Monotonic-raw nanos since an arbitrary epoch on this host. Use these for
// computing durations and ordering events on a single host.
[[nodiscard]] auto monotonic_ns() noexcept -> std::int64_t;

// Real-time nanos since Unix epoch. Use these only when crossing host
// boundaries or for human-facing logging.
[[nodiscard]] auto realtime_ns() noexcept -> std::int64_t;

// Translate a host monotonic timestamp to wall-clock nanos using the anchor
// captured at process start. Accurate to ~1 µs on a steady host.
[[nodiscard]] auto monotonic_to_wallclock_ns(std::int64_t mono_ns) noexcept -> std::int64_t;

// One-shot initialization — captures the (MONOTONIC_RAW, REALTIME) anchor.
// Called by `velocity::time::init()` early in main(). Idempotent.
auto init() -> void;

// Pretty-format an integer of nanoseconds — used by the latency-tracking
// utilities. Returns a small heap-free string view, e.g. "832ns", "1.23µs",
// "12.4ms". For logging only; do not parse.
[[nodiscard]] auto pretty_ns(std::int64_t ns) noexcept -> std::string;

}  // namespace velocity::time
