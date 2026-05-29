// =============================================================================
//  plan.cpp — ramp interpolation helper.
// =============================================================================

#include "bot_worker/plan.h"

namespace velocity::bot_worker {

auto interp_rps(const std::vector<RampWaypoint>& ramp,
                std::int64_t elapsed_ns) noexcept -> std::uint64_t {
    if (ramp.empty()) return 0;
    if (elapsed_ns <= ramp.front().offset_ns) return ramp.front().target_rps;
    if (elapsed_ns >= ramp.back().offset_ns)  return ramp.back().target_rps;

    // Binary search for the bracketing pair.
    auto lo = ramp.begin();
    auto hi = ramp.end() - 1;
    while (hi - lo > 1) {
        const auto mid = lo + (hi - lo) / 2;
        if (mid->offset_ns <= elapsed_ns) {
            lo = mid;
        } else {
            hi = mid;
        }
    }

    const auto span = static_cast<double>(hi->offset_ns - lo->offset_ns);
    if (span <= 0) return lo->target_rps;

    const auto t = static_cast<double>(elapsed_ns - lo->offset_ns) / span;
    return static_cast<std::uint64_t>(
        static_cast<double>(lo->target_rps) +
        t * (static_cast<double>(hi->target_rps) - static_cast<double>(lo->target_rps)));
}

}  // namespace velocity::bot_worker
