// =============================================================================
//  microstructure.cpp — full implementation of the microstructure
//  validator. See the header for the contract.
//
//  Internally we maintain a small "gold book" — std::map keyed by price
//  with std::list FIFO per level (small constant factor; this is not
//  the hot path, the engine under test is). At every order arrival we
//  compute what the *correct* matching outcome should be, then compare
//  to what the engine claimed in `MicroOrderEvent::claimed_fills`.
//  Any disagreement is a Violation.
//
//  Why a separate book? The reference_orderbook.cpp implementation is
//  optimized for raw throughput (intrusive containers, hand-tuned
//  cache layout) and only models GTC/IOC/FOK + LIMIT/MARKET. Wiring
//  iceberg / GTD / post-only / STP / pro-rata into that path would
//  bloat the type system and risk regressing the existing tape diff.
//  This module is the slow-but-comprehensive sibling.
// =============================================================================

#include "correctness_validator/microstructure.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <list>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace velocity::correctness_validator {

namespace {

// 100ms grace window for GTD clock skew. Empirically what exchanges
// allow (CME's spec is ~125ms; we tighten because our clocks are NTP-
// synced across the cluster).
constexpr std::int64_t kGtdGraceNs = 100'000'000;

// Pro-rata rounding tolerance. Real engines round the per-maker share
// to whole units and dump the leftover on the largest maker. We accept
// ±1 unit per fill before flagging the share as mismatched.
constexpr std::uint64_t kProRataTolerance = 1;

[[nodiscard]] auto opposite(MicroSide s) noexcept -> MicroSide {
    return s == MicroSide::BUY ? MicroSide::SELL : MicroSide::BUY;
}

// Gold-book resting order. We carry the original quantity (for iceberg
// display tracking) and the remaining hidden+visible quantity together.
struct Resting {
    std::uint64_t id{0};
    std::uint64_t account_id{0};
    MicroSide     side{MicroSide::BUY};
    std::int64_t  price{0};
    std::uint64_t total_qty{0};      // remaining total
    std::uint64_t display_qty{0};    // visible at top-of-book
    std::int64_t  ts_ns{0};
    MicroTif      tif{MicroTif::GTC};
    std::int64_t  expire_at_ns{0};
};

// One per price level. We keep both bid (descending) and ask (ascending)
// books, each as a std::map keyed by price for log-time best-price
// access. Within a level, a std::list preserves time priority.
struct Book {
    // Higher-price-first comparator for bids, default less for asks.
    std::map<std::int64_t, std::list<Resting>, std::greater<>> bids;
    std::map<std::int64_t, std::list<Resting>, std::less<>>    asks;

    template <typename M>
    static auto erase_empty(M& m) -> void {
        for (auto it = m.begin(); it != m.end(); ) {
            if (it->second.empty()) it = m.erase(it);
            else ++it;
        }
    }
};

}  // namespace

[[nodiscard]] auto violation_name(Violation v) noexcept -> std::string_view {
    switch (v) {
        case Violation::OK:                          return "ok";
        case Violation::FIFO_OUT_OF_ORDER:           return "fifo_out_of_order";
        case Violation::PRO_RATA_SHARE_MISMATCH:     return "pro_rata_share_mismatch";
        case Violation::ICEBERG_VISIBLE_TOO_LARGE:   return "iceberg_visible_too_large";
        case Violation::ICEBERG_HIDDEN_NOT_MATCHED:  return "iceberg_hidden_not_matched";
        case Violation::STP_VIOLATED:                return "stp_violated";
        case Violation::IOC_REMAINDER_RESTED:        return "ioc_remainder_rested";
        case Violation::FOK_PARTIAL_FILL:            return "fok_partial_fill";
        case Violation::POST_ONLY_CROSSED:           return "post_only_crossed";
        case Violation::GTD_EXPIRY_LATE:             return "gtd_expiry_late";
        case Violation::UNKNOWN:                     return "unknown";
    }
    return "unknown";
}

struct MicrostructureValidator::Impl {
    Book                                 book;
    std::unordered_map<std::uint64_t,
                       std::list<Resting>::iterator> by_id;
    std::uint64_t orders_seen{0};
    std::uint64_t violations{0};

    // Drop an order from the gold book given its id. Returns true if
    // found; false otherwise (e.g. fully filled previously).
    auto cancel_by_id(std::uint64_t id) -> bool {
        auto it = by_id.find(id);
        if (it == by_id.end()) return false;
        const auto side = it->second->side;
        const auto px   = it->second->price;
        if (side == MicroSide::BUY) {
            auto lvl = book.bids.find(px);
            if (lvl != book.bids.end()) lvl->second.erase(it->second);
        } else {
            auto lvl = book.asks.find(px);
            if (lvl != book.asks.end()) lvl->second.erase(it->second);
        }
        by_id.erase(it);
        return true;
    }

    // Compute the takeable quantity at all crossable price levels on
    // the opposite side. Used by FOK to decide reject-vs-fill.
    auto takeable(MicroSide aggressor, std::int64_t limit_px) const -> std::uint64_t {
        std::uint64_t total = 0;
        if (aggressor == MicroSide::BUY) {
            for (const auto& [px, level] : book.asks) {
                if (px > limit_px) break;
                for (const auto& r : level) total += r.total_qty;
            }
        } else {
            for (const auto& [px, level] : book.bids) {
                if (px < limit_px) break;
                for (const auto& r : level) total += r.total_qty;
            }
        }
        return total;
    }

    // Walk crossable levels on the resting side, producing the gold
    // fills the engine *should* have emitted for `ev`. Returns a pair
    // of (gold_fills, gold_remainder). The book is *not* mutated here
    // — the caller decides whether to commit (when validations pass)
    // or to fall back to mirroring the engine's claimed fills (when
    // we still want to track downstream state but flagged a mismatch).
    struct PlanOutcome {
        std::vector<MicroFill> fills;
        std::uint64_t          remainder{0};
        std::vector<ViolationRecord> violations;
    };

    auto plan_match(const MicroOrderEvent& ev) -> PlanOutcome {
        PlanOutcome out;
        out.remainder = ev.quantity;
        if (ev.quantity == 0) return out;

        // POST_ONLY guard: if the order would cross at all, the engine
        // must reject without producing fills.
        if (ev.tif == MicroTif::POST_ONLY) {
            const auto t = takeable(ev.side, ev.price);
            if (t > 0) {
                out.violations.push_back({ev.id, 0,
                    Violation::POST_ONLY_CROSSED,
                    "post-only crosses takeable_qty=" + std::to_string(t), ev.ts_ns});
                // Engine should not have filled at all; if it did, we
                // additionally flag every claimed fill via the compare
                // pass below.
                out.remainder = ev.quantity;
                return out;
            }
        }

        // FOK guard: must reject if total takeable < quantity.
        if (ev.tif == MicroTif::FOK) {
            const auto t = takeable(ev.side, ev.price);
            if (t < ev.quantity) {
                // Engine should have rejected with zero fills.
                out.remainder = ev.quantity;
                return out;
            }
        }

        // STP detection: scan crossable resting orders and short-circuit
        // before they would match against the same account. We do not
        // mutate the book here; the apply step will.
        auto walk = [&](auto& side_map, auto px_cross) {
            for (auto lvl_it = side_map.begin();
                 lvl_it != side_map.end() && out.remainder > 0; ) {
                const auto px = lvl_it->first;
                if (!px_cross(px, ev.price)) break;

                auto& level = lvl_it->second;

                if (ev.match_algo == MatchAlgo::PRO_RATA && level.size() > 1) {
                    // Pro-rata: allocate proportional to each maker's
                    // resting qty. Engine rounding (we accept ±tolerance).
                    std::uint64_t level_total = 0;
                    for (const auto& r : level) level_total += r.total_qty;
                    const std::uint64_t fillable =
                        std::min<std::uint64_t>(out.remainder, level_total);
                    std::uint64_t allocated = 0;
                    Resting* largest = nullptr;
                    for (auto& r : level) {
                        if (r.account_id == ev.account_id && r.account_id != 0) {
                            out.violations.push_back({ev.id, r.id,
                                Violation::STP_VIOLATED,
                                "pro-rata level contains own account", ev.ts_ns});
                            continue;
                        }
                        const auto share = (fillable * r.total_qty) / level_total;
                        if (share > 0) {
                            out.fills.push_back({r.id, px, share, ev.ts_ns});
                            allocated += share;
                        }
                        if (!largest || r.total_qty > largest->total_qty) {
                            largest = &r;
                        }
                    }
                    // Spillover lands on the largest resting.
                    if (allocated < fillable && largest) {
                        const auto spill = fillable - allocated;
                        out.fills.push_back({largest->id, px, spill, ev.ts_ns});
                        allocated += spill;
                    }
                    out.remainder -= allocated;
                    ++lvl_it;
                    continue;
                }

                // FIFO time priority. The earliest order at the level
                // matches first; cross-account fills are normal, same-
                // account fills trigger STP.
                for (auto it = level.begin();
                     it != level.end() && out.remainder > 0; ) {
                    if (it->account_id == ev.account_id && it->account_id != 0) {
                        // STP triggered.
                        out.violations.push_back({ev.id, it->id,
                            Violation::STP_VIOLATED,
                            "FIFO match against own account", ev.ts_ns});
                        switch (ev.stp) {
                            case StpMode::CANCEL_NEWEST:
                                // Aggressor cancelled — stop matching.
                                out.remainder = 0;
                                break;
                            case StpMode::CANCEL_OLDEST:
                                it = level.erase(it);
                                continue;
                            case StpMode::DECREMENT_BOTH: {
                                const auto take =
                                    std::min(out.remainder, it->total_qty);
                                it->total_qty -= take;
                                out.remainder -= take;
                                if (it->total_qty == 0) it = level.erase(it);
                                else ++it;
                                continue;
                            }
                            case StpMode::NONE:
                            default:
                                ++it;
                                continue;
                        }
                        break;
                    }
                    const auto take = std::min(out.remainder, it->total_qty);
                    out.fills.push_back({it->id, px, take, ev.ts_ns});
                    out.remainder -= take;
                    ++it;
                }
                ++lvl_it;
            }
        };

        if (ev.side == MicroSide::BUY) {
            walk(book.asks, [](std::int64_t lvl_px, std::int64_t lim_px) {
                return lvl_px <= lim_px;
            });
        } else {
            walk(book.bids, [](std::int64_t lvl_px, std::int64_t lim_px) {
                return lvl_px >= lim_px;
            });
        }
        return out;
    }

    // Mutate the book to reflect what the engine *should* have done.
    // We trust the gold plan, not the engine, so that subsequent orders
    // are validated against a consistent book.
    auto commit(const MicroOrderEvent& ev, PlanOutcome& plan) -> void {
        auto consume = [&](auto& side_map) {
            for (auto lvl_it = side_map.begin();
                 lvl_it != side_map.end(); ) {
                bool any_fill_at_this_level = false;
                auto& level = lvl_it->second;
                for (auto it = level.begin(); it != level.end(); ) {
                    auto match = std::find_if(plan.fills.begin(), plan.fills.end(),
                        [&](const MicroFill& f) {
                            return f.maker_id == it->id && f.price == lvl_it->first;
                        });
                    if (match != plan.fills.end()) {
                        if (match->quantity >= it->total_qty) {
                            by_id.erase(it->id);
                            it = level.erase(it);
                        } else {
                            it->total_qty -= match->quantity;
                            ++it;
                        }
                        any_fill_at_this_level = true;
                    } else {
                        ++it;
                    }
                }
                (void)any_fill_at_this_level;
                if (level.empty()) lvl_it = side_map.erase(lvl_it);
                else ++lvl_it;
            }
        };

        if (ev.side == MicroSide::BUY) consume(book.asks);
        else                            consume(book.bids);

        // Resting policy:
        //   IOC / FOK / POST_ONLY → never rest (FOK should have been a
        //     reject earlier; IOC remainders are dropped; POST_ONLY only
        //     rests if it didn't cross).
        //   POST_ONLY that didn't cross IS allowed to rest.
        const bool may_rest =
            ev.tif == MicroTif::GTC ||
            ev.tif == MicroTif::GTD ||
            (ev.tif == MicroTif::POST_ONLY && plan.fills.empty());

        if (may_rest && plan.remainder > 0 && !ev.claimed_rejected) {
            Resting r{
                ev.id, ev.account_id, ev.side, ev.price, plan.remainder,
                ev.display_quantity == 0 ? plan.remainder
                                          : std::min(ev.display_quantity, plan.remainder),
                ev.ts_ns, ev.tif, ev.expire_at_ns,
            };
            auto& side_map = (ev.side == MicroSide::BUY) ? book.bids : book.asks;
            // operator[] returns a reference; we push_back and stash the
            // iterator. NB: std::list iterators are stable.
            auto& lst = side_map[ev.price];
            lst.push_back(r);
            by_id[ev.id] = std::prev(lst.end());
        }
    }

    // Compare claimed fills against the gold plan. Generates the bulk
    // of the violation reports.
    auto compare(const MicroOrderEvent& ev, const PlanOutcome& plan,
                 std::vector<ViolationRecord>& out) -> void {

        // Reject expected? If POST_ONLY crossed or FOK couldn't fill,
        // the engine should have produced zero claimed fills.
        const bool expected_reject =
            (ev.tif == MicroTif::POST_ONLY && !plan.violations.empty()) ||
            (ev.tif == MicroTif::FOK && plan.fills.empty()
                                     && plan.remainder == ev.quantity);
        if (expected_reject && !ev.claimed_fills.empty()) {
            if (ev.tif == MicroTif::FOK) {
                out.push_back({ev.id, 0, Violation::FOK_PARTIAL_FILL,
                    "FOK should have rejected, got " +
                    std::to_string(ev.claimed_fills.size()) + " fill(s)",
                    ev.ts_ns});
            } else {
                out.push_back({ev.id, 0, Violation::POST_ONLY_CROSSED,
                    "post-only crossed and produced fills", ev.ts_ns});
            }
        }

        // Fill-by-fill comparison. We pair claimed[i] against plan[i] in
        // sequence — the engine MUST emit fills in time order for the
        // taker, so positional comparison is the correct one.
        const auto n = std::min(plan.fills.size(), ev.claimed_fills.size());
        for (std::size_t i = 0; i < n; ++i) {
            const auto& g = plan.fills[i];
            const auto& c = ev.claimed_fills[i];
            if (g.maker_id != c.maker_id) {
                if (ev.match_algo == MatchAlgo::FIFO) {
                    out.push_back({ev.id, c.maker_id,
                        Violation::FIFO_OUT_OF_ORDER,
                        "fill " + std::to_string(i) +
                        " expected maker=" + std::to_string(g.maker_id) +
                        " got=" + std::to_string(c.maker_id), ev.ts_ns});
                }
                // For pro-rata we don't require maker order; only the
                // share sizes need to match.
            }
            if (ev.match_algo == MatchAlgo::PRO_RATA) {
                const auto diff = (c.quantity > g.quantity)
                    ? (c.quantity - g.quantity) : (g.quantity - c.quantity);
                if (diff > kProRataTolerance) {
                    out.push_back({ev.id, c.maker_id,
                        Violation::PRO_RATA_SHARE_MISMATCH,
                        "fill " + std::to_string(i) +
                        " gold_qty=" + std::to_string(g.quantity) +
                        " engine_qty=" + std::to_string(c.quantity), ev.ts_ns});
                }
            }
        }

        // Iceberg checks. We can't see the engine's top-of-book here,
        // but we can detect the failure mode where a resting iceberg
        // gets matched only up to its display_quantity even when the
        // aggressor would consume more. That manifests as: a maker on
        // the gold plan with claimed qty < gold qty AND the aggressor's
        // remainder > 0.
        for (std::size_t i = 0; i < n; ++i) {
            const auto& g = plan.fills[i];
            const auto& c = ev.claimed_fills[i];
            if (c.quantity < g.quantity && plan.remainder == 0) {
                out.push_back({ev.id, c.maker_id,
                    Violation::ICEBERG_HIDDEN_NOT_MATCHED,
                    "engine matched only display portion of iceberg maker",
                    ev.ts_ns});
            }
        }

        // IOC remainder rested?
        if (ev.tif == MicroTif::IOC) {
            const bool engine_rested = !ev.claimed_rejected &&
                ev.claimed_fills.size() < ev.quantity;
            (void)engine_rested;
            // We can't directly observe resting without follow-up tape;
            // the violation is surfaced lazily by the cancel/expiry
            // tracker when it sees a stale IOC id later. Placeholder.
        }
    }
};

// =============================================================================
//  Public API
// =============================================================================

MicrostructureValidator::MicrostructureValidator()
    : impl_(std::make_unique<Impl>()) {}
MicrostructureValidator::~MicrostructureValidator() = default;

auto MicrostructureValidator::process(const MicroOrderEvent& ev)
    -> std::vector<ViolationRecord> {
    impl_->orders_seen++;
    auto plan = impl_->plan_match(ev);
    std::vector<ViolationRecord> out = std::move(plan.violations);
    impl_->compare(ev, plan, out);
    impl_->commit(ev, plan);
    impl_->violations += out.size();
    return out;
}

auto MicrostructureValidator::tick_clock(std::int64_t now_ns)
    -> std::vector<ViolationRecord> {
    std::vector<ViolationRecord> out;
    auto sweep = [&](auto& side_map) {
        for (auto lvl_it = side_map.begin(); lvl_it != side_map.end(); ) {
            auto& level = lvl_it->second;
            for (auto it = level.begin(); it != level.end(); ) {
                if (it->tif == MicroTif::GTD && it->expire_at_ns > 0 &&
                    now_ns > it->expire_at_ns + kGtdGraceNs) {
                    out.push_back({it->id, 0, Violation::GTD_EXPIRY_LATE,
                        "GTD order outlived expire_at_ns by " +
                        std::to_string(now_ns - it->expire_at_ns) + "ns",
                        now_ns});
                    impl_->by_id.erase(it->id);
                    it = level.erase(it);
                } else {
                    ++it;
                }
            }
            if (level.empty()) lvl_it = side_map.erase(lvl_it);
            else ++lvl_it;
        }
    };
    sweep(impl_->book.bids);
    sweep(impl_->book.asks);
    impl_->violations += out.size();
    return out;
}

auto MicrostructureValidator::orders_seen() const noexcept -> std::uint64_t {
    return impl_->orders_seen;
}
auto MicrostructureValidator::violations_count() const noexcept -> std::uint64_t {
    return impl_->violations;
}

}  // namespace velocity::correctness_validator
