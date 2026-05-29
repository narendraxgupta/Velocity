// =============================================================================
//  time.cpp — monotonic-raw clock helpers and wall-clock anchor.
//
//  Implementation notes
//  --------------------
//  We deliberately avoid `<chrono>` in the public API of this module because
//  `chrono` does not let us choose `CLOCK_MONOTONIC_RAW` portably. We go
//  through `clock_gettime` directly and expose plain int64 nanoseconds.
//
//  The anchor pair is captured once at startup. On Linux this is fast
//  (vDSO-backed `clock_gettime`), so we re-capture it lazily if the process
//  has been alive longer than 1 hour — this caps the drift of our
//  monotonic→wallclock translation. In practice benchmarks run for minutes,
//  so this is rare.
// =============================================================================

#include "velocity/common/time.h"

#include <atomic>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>

#include <fmt/format.h>

namespace velocity::time {
namespace {

struct Anchor {
    std::int64_t mono_ns;
    std::int64_t real_ns;
    std::int64_t captured_at_mono_ns;  // when we took this anchor
};

constexpr std::int64_t kAnchorRefreshNs = 60LL * 60 * 1'000'000'000;  // 1 hour

std::once_flag g_once;
std::atomic<Anchor> g_anchor{};  // copy-on-update, lock-free reads

[[nodiscard]] auto raw_monotonic_ns() noexcept -> std::int64_t {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000
         + static_cast<std::int64_t>(ts.tv_nsec);
}

[[nodiscard]] auto raw_realtime_ns() noexcept -> std::int64_t {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000
         + static_cast<std::int64_t>(ts.tv_nsec);
}

auto capture_anchor() noexcept -> void {
    // Sandwich the realtime read between two monotonic reads and take the
    // mid-point. Standard trick — bounds error to <1 µs on modern Linux.
    const std::int64_t mono_before = raw_monotonic_ns();
    const std::int64_t real        = raw_realtime_ns();
    const std::int64_t mono_after  = raw_monotonic_ns();
    const std::int64_t mono_mid    = mono_before + (mono_after - mono_before) / 2;

    Anchor a{mono_mid, real, mono_mid};
    g_anchor.store(a, std::memory_order_release);
}

}  // namespace

auto init() -> void {
    std::call_once(g_once, capture_anchor);
}

auto monotonic_ns() noexcept -> std::int64_t {
    return raw_monotonic_ns();
}

auto realtime_ns() noexcept -> std::int64_t {
    return raw_realtime_ns();
}

auto monotonic_to_wallclock_ns(std::int64_t mono_ns) noexcept -> std::int64_t {
    Anchor a = g_anchor.load(std::memory_order_acquire);
    if (a.mono_ns == 0) {
        // Anchor not captured yet; fall back to a fresh realtime read.
        return raw_realtime_ns();
    }

    // Refresh the anchor if it's stale.
    const std::int64_t now_mono = raw_monotonic_ns();
    if (now_mono - a.captured_at_mono_ns > kAnchorRefreshNs) {
        capture_anchor();
        a = g_anchor.load(std::memory_order_acquire);
    }

    return a.real_ns + (mono_ns - a.mono_ns);
}

auto pretty_ns(std::int64_t ns) noexcept -> std::string {
    if (ns < 1'000)                 return fmt::format("{}ns", ns);
    if (ns < 1'000'000)             return fmt::format("{:.2f}µs", static_cast<double>(ns) / 1e3);
    if (ns < 1'000'000'000)         return fmt::format("{:.2f}ms", static_cast<double>(ns) / 1e6);
    return fmt::format("{:.2f}s", static_cast<double>(ns) / 1e9);
}

}  // namespace velocity::time
