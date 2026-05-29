// =============================================================================
//  correctness_validator/execution_quality.h
//
//  Per-submission execution quality tracker. For every aggressive order
//  we record:
//
//    * decision_price — the mid quote at the instant the order arrived.
//    * vwap          — the volume-weighted average price the order
//                       actually traded at (sum(px*qty) / sum(qty) over
//                       the order's fills).
//    * reversion_mid — the mid `reversion_window_ms` after the last
//                       fill. Used to compute reversion = (vwap −
//                       reversion_mid) / vwap × 1e4 (in bps).
//
//  Derived metrics (computed and rolled into the running aggregates):
//
//    * slippage_bps      = (vwap − decision_price) / decision_price × 1e4
//                          (signed: positive = paid up for buys / received
//                           less for sells; negative = price-improved)
//    * implementation_shortfall_bps
//                        = same formula as slippage; semantically the
//                          "cost of execution" — what you sacrificed by
//                          NOT being able to trade at the decision price.
//                          We keep them separate from slippage so the
//                          frontend can present both with the standard
//                          industry framing.
//    * reversion_bps     = (vwap − reversion_mid) / vwap × 1e4
//                          (sign: positive = price reverted in our favour
//                           after we traded; large negative = adverse
//                           selection — we bought before a dip).
//
//  Aggregation: we surface a struct of summary stats (count, mean,
//  median, p95) over each metric, periodically dumped to Redis as JSON.
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace velocity::correctness_validator {

struct ExecQualityStats {
    std::uint64_t orders_observed{0};
    std::uint64_t fully_marked{0};   // reversion observed (post-window passed)

    // All values in basis points (1e-4).
    double slippage_mean_bps{0};
    double slippage_p50_bps{0};
    double slippage_p95_bps{0};

    double is_mean_bps{0};
    double is_p50_bps{0};
    double is_p95_bps{0};

    double reversion_mean_bps{0};
    double reversion_p50_bps{0};
    double reversion_p95_bps{0};
};

class ExecutionQualityTracker {
public:
    explicit ExecutionQualityTracker(std::int64_t reversion_window_ms = 5'000);
    ~ExecutionQualityTracker();

    ExecutionQualityTracker(const ExecutionQualityTracker&)            = delete;
    ExecutionQualityTracker& operator=(const ExecutionQualityTracker&) = delete;

    // Pre-trade: stamp the decision mid for an incoming aggressive
    // order. We accept any incoming order (passive too) and only emit
    // metrics for those that actually fill — the cheapest API surface
    // is "tell us about every order, we'll filter".
    auto on_order_arrival(std::uint64_t order_id,
                          std::int64_t  decision_mid,
                          std::int64_t  ts_ns) -> void;

    // For each fill produced by an order, accumulate the running vwap
    // and the fill timestamp (latest wins as the "last fill" mark).
    auto on_fill(std::uint64_t order_id,
                 std::int64_t  fill_price,
                 std::uint64_t fill_qty,
                 std::int64_t  ts_ns) -> void;

    // Tick the clock — used to advance pending reversion windows. Pass
    // the current mid; orders whose last-fill + window_ms ≤ now_ns get
    // marked with this mid and rolled into the aggregate stats.
    auto tick(std::int64_t now_ns, std::int64_t current_mid) -> void;

    // Lock-free snapshot of the running aggregates.
    [[nodiscard]] auto snapshot() const -> ExecQualityStats;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace velocity::correctness_validator
