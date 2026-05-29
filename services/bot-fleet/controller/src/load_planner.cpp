// =============================================================================
//  load_planner.cpp — weighted split of a global plan across workers.
// =============================================================================

#include "bot_controller/load_planner.h"

#include <algorithm>
#include <numeric>

namespace velocity::bot_controller {

auto distribute(const velocity::bot::v1::LoadPlan& global,
                const std::vector<WorkerCapacity>& workers)
    -> std::unordered_map<std::string, velocity::bot::v1::LoadPlan> {

    std::unordered_map<std::string, velocity::bot::v1::LoadPlan> out;
    if (workers.empty()) return out;

    // Compute the total cpu weight across workers.
    std::uint64_t total_cpu = 0;
    for (const auto& w : workers) total_cpu += w.cpu_count;
    if (total_cpu == 0) total_cpu = workers.size();

    for (const auto& w : workers) {
        velocity::bot::v1::LoadPlan plan = global;  // copy entire plan
        const double share = static_cast<double>(w.cpu_count) /
                             static_cast<double>(total_cpu);

        // Rescale ramp targets by this worker's share.
        for (auto& wp : *plan.mutable_schedule()) {
            wp.set_target_rps(static_cast<std::uint64_t>(
                static_cast<double>(wp.target_rps()) * share));
        }
        // bots_per_reactor stays per-thread; total bots scale with reactor_threads.
        out.emplace(w.worker_id, std::move(plan));
    }
    return out;
}

}  // namespace velocity::bot_controller
