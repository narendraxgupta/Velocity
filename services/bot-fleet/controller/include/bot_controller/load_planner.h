// =============================================================================
//  bot_controller/load_planner.h
//
//  Takes a "global" benchmark recipe (target RPS, persona mix, ramp) and a
//  set of healthy worker capacities, and produces per-worker LoadPlans that
//  sum to the global recipe.
//
//  Distribution algorithm: weighted by worker.cpu_count. Workers are
//  partitioned into deciles to avoid letting a single beefy box dominate.
// =============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "bot.pb.h"

namespace velocity::bot_controller {

struct WorkerCapacity {
    std::string   worker_id;
    std::uint32_t cpu_count{1};
};

// Compute per-worker plans summing to `global`. `target_total_rps` overrides
// the highest waypoint in the schedule (or the schedule is left as-is if 0).
[[nodiscard]] auto distribute(const velocity::bot::v1::LoadPlan& global,
                              const std::vector<WorkerCapacity>& workers)
    -> std::unordered_map<std::string, velocity::bot::v1::LoadPlan>;

}  // namespace velocity::bot_controller
