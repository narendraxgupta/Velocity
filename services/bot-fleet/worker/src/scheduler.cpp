// =============================================================================
//  scheduler.cpp — open-loop driver with intended-send-time accounting.
// =============================================================================

#include "bot_worker/scheduler.h"

#include <algorithm>

namespace velocity::bot_worker {

namespace {
// 1 ns is the minimum meaningful step; smaller would let the cursor
// advance by zero and the reactor would spin without ever exiting the
// "intended time has arrived" check. We clamp here rather than in the
// reactor so the contract lives with the scheduler.
[[nodiscard]] constexpr auto step_for(std::uint64_t rps) noexcept -> std::int64_t {
    if (rps == 0) return 1'000'000'000;  // pretend 1 Hz when paused
    const auto step = static_cast<std::int64_t>(1'000'000'000ULL / rps);
    return step <= 0 ? 1 : step;
}
}  // namespace

Scheduler::Scheduler(std::int64_t start_mono_ns, std::uint64_t initial_rps) noexcept
    : next_intended_ns_(start_mono_ns),
      step_ns_(step_for(initial_rps)) {}

auto Scheduler::next() noexcept -> std::int64_t {
    const auto out = next_intended_ns_;
    next_intended_ns_ += step_ns_.load(std::memory_order_relaxed);
    return out;
}

auto Scheduler::set_rate(std::uint64_t rps) noexcept -> void {
    step_ns_.store(step_for(rps), std::memory_order_relaxed);
}

auto Scheduler::skew_ns(std::int64_t now_mono_ns) const noexcept -> std::int64_t {
    return std::max<std::int64_t>(0, now_mono_ns - next_intended_ns_);
}

}  // namespace velocity::bot_worker
