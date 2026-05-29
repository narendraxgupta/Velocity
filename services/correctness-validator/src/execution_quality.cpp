// =============================================================================
//  execution_quality.cpp — full implementation.
// =============================================================================

#include "correctness_validator/execution_quality.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace velocity::correctness_validator {

namespace {

struct InflightOrder {
    std::int64_t  decision_mid{0};
    std::int64_t  first_seen_ns{0};
    std::int64_t  last_fill_ns{0};
    // VWAP accumulator. Stored as (sum_px_x_qty, sum_qty) to dodge
    // floating-point precision artifacts when accumulating thousands of
    // fills on the same order.
    __int128      sum_px_x_qty{0};
    std::uint64_t sum_qty{0};
};

struct Completed {
    double slippage_bps{0};
    double is_bps{0};
    double reversion_bps{0};
    bool   reversion_marked{false};
};

// Rolling reservoir of the most recent N observations per metric.
// Bounded at 4096 — sufficient for stable p50/p95 estimates without
// unbounded memory.
constexpr std::size_t kReservoirCap = 4096;

// An order is treated as "done filling" once it has at least one fill AND
// has been quiet (no new fill) for this long. Completing on the FIRST fill
// instead meant a partial fill that kept executing had its VWAP frozen at
// the first slice — on_fill drops fills once an order leaves `inflight`.
constexpr std::int64_t kFillSettleNs = 1'000'000'000;  // 1s
// Orders that never fill are dropped after this long.
constexpr std::int64_t kInflightMaxAgeNs = 30LL * 1'000'000'000;  // 30s

[[nodiscard]] auto percentile(std::vector<double> v, double q) -> double {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const auto idx = std::clamp<std::size_t>(
        static_cast<std::size_t>(q * static_cast<double>(v.size() - 1)),
        0, v.size() - 1);
    return v[idx];
}

[[nodiscard]] auto mean(const std::vector<double>& v) -> double {
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (auto x : v) s += x;
    return s / static_cast<double>(v.size());
}

}  // namespace

struct ExecutionQualityTracker::Impl {
    std::int64_t   reversion_window_ns{0};
    mutable std::mutex                          mu;
    std::unordered_map<std::uint64_t, InflightOrder> inflight;
    // Awaiting-mark queue: orders that fully filled and are now waiting
    // on the reversion window to lapse. We keep them in insertion order
    // for FIFO mark-out. Stored vwap is computed at completion time so
    // we don't need to re-traverse inflight entries.
    struct Awaiting {
        std::int64_t last_fill_ns;
        double       vwap;
    };
    std::deque<Awaiting> awaiting;
    // Aggregates as bounded reservoirs.
    std::vector<double> slippage;
    std::vector<double> is_;
    std::vector<double> reversion;

    std::uint64_t orders_observed{0};
    std::uint64_t fully_marked{0};

    static auto observe(std::vector<double>& res, double x) -> void {
        if (res.size() < kReservoirCap) res.push_back(x);
        else {
            // Reservoir replace at hash position so we don't drift toward
            // any temporal bias. Simple modulo of size is fine for stats.
            static std::size_t cursor = 0;
            res[cursor++ % kReservoirCap] = x;
        }
    }
};

ExecutionQualityTracker::ExecutionQualityTracker(std::int64_t reversion_window_ms)
    : impl_(std::make_unique<Impl>()) {
    impl_->reversion_window_ns = reversion_window_ms * 1'000'000;
}

ExecutionQualityTracker::~ExecutionQualityTracker() = default;

auto ExecutionQualityTracker::on_order_arrival(std::uint64_t order_id,
                                                std::int64_t decision_mid,
                                                std::int64_t ts_ns) -> void {
    if (decision_mid <= 0) return;  // no mid yet — book was empty
    std::lock_guard lk(impl_->mu);
    auto& o = impl_->inflight[order_id];
    o.decision_mid   = decision_mid;
    o.first_seen_ns  = ts_ns;
    impl_->orders_observed++;
}

auto ExecutionQualityTracker::on_fill(std::uint64_t order_id,
                                       std::int64_t fill_price,
                                       std::uint64_t fill_qty,
                                       std::int64_t ts_ns) -> void {
    if (fill_qty == 0) return;
    std::lock_guard lk(impl_->mu);
    auto it = impl_->inflight.find(order_id);
    if (it == impl_->inflight.end()) return;  // passive maker — no decision_mid
    auto& o = it->second;
    o.sum_px_x_qty += static_cast<__int128>(fill_price) *
                      static_cast<__int128>(fill_qty);
    o.sum_qty      += fill_qty;
    o.last_fill_ns  = ts_ns;
}

auto ExecutionQualityTracker::tick(std::int64_t now_ns,
                                    std::int64_t current_mid) -> void {
    std::lock_guard lk(impl_->mu);

    // Move newly-completed orders into the awaiting queue. We do this on
    // every tick so the awaiting deque stays in insertion-time order. An
    // order is "complete" once it has fills AND has gone quiet for the
    // settle window — that way an order still actively filling (its
    // last_fill_ns keeps advancing) stays inflight and accumulates the full
    // VWAP before we freeze it.
    for (auto it = impl_->inflight.begin(); it != impl_->inflight.end(); ) {
        auto& o = it->second;
        const bool has_fills = o.sum_qty > 0;
        const bool settled   = has_fills &&
                               (now_ns - o.last_fill_ns) >= kFillSettleNs;
        if (settled) {
            const auto vwap_x = o.sum_px_x_qty /
                                static_cast<__int128>(o.sum_qty);
            const double vwap = static_cast<double>(static_cast<std::int64_t>(vwap_x));
            const double mid  = static_cast<double>(o.decision_mid);
            if (mid > 0) {
                const double slip = (vwap - mid) / mid * 10'000.0;
                Impl::observe(impl_->slippage, slip);
                Impl::observe(impl_->is_,      slip);
            }
            impl_->awaiting.push_back({o.last_fill_ns, vwap});
            it = impl_->inflight.erase(it);
        } else if (!has_fills && now_ns - o.first_seen_ns > kInflightMaxAgeNs) {
            it = impl_->inflight.erase(it);
        } else {
            ++it;
        }
    }

    // Mark out the awaiting queue once enough wall time has passed.
    // reversion_bps = (vwap − post_mid) / vwap × 1e4
    // Positive reversion = mid moved AGAINST our direction after we
    // printed — i.e. we "bought the dip" / "sold the rip", a good print.
    // Negative reversion = adverse selection.
    if (current_mid > 0) {
        while (!impl_->awaiting.empty()) {
            const auto& head = impl_->awaiting.front();
            if (now_ns - head.last_fill_ns < impl_->reversion_window_ns) break;
            const double post_mid = static_cast<double>(current_mid);
            const double rev = head.vwap > 0
                ? (head.vwap - post_mid) / head.vwap * 10'000.0
                : 0.0;
            Impl::observe(impl_->reversion, rev);
            impl_->fully_marked++;
            impl_->awaiting.pop_front();
        }
    }
}

auto ExecutionQualityTracker::snapshot() const -> ExecQualityStats {
    std::lock_guard lk(impl_->mu);
    ExecQualityStats out;
    out.orders_observed     = impl_->orders_observed;
    out.fully_marked        = impl_->fully_marked;
    out.slippage_mean_bps   = mean(impl_->slippage);
    out.slippage_p50_bps    = percentile(impl_->slippage, 0.50);
    out.slippage_p95_bps    = percentile(impl_->slippage, 0.95);
    out.is_mean_bps         = mean(impl_->is_);
    out.is_p50_bps          = percentile(impl_->is_, 0.50);
    out.is_p95_bps          = percentile(impl_->is_, 0.95);
    out.reversion_mean_bps  = mean(impl_->reversion);
    out.reversion_p50_bps   = percentile(impl_->reversion, 0.50);
    out.reversion_p95_bps   = percentile(impl_->reversion, 0.95);
    return out;
}

}  // namespace velocity::correctness_validator
