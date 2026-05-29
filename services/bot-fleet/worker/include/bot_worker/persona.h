// =============================================================================
//  bot_worker/persona.h
//
//  Personas are the trading behaviors a bot exhibits. Each persona owns a
//  small state machine and generates Decisions on each scheduler tick.
//
//  The scheduler does NOT know how a persona produces a Decision — it just
//  asks for "what's the next order?" and treats the result uniformly.
//
//  This decoupling makes adding personas trivial (just implement the
//  Persona interface) and lets us A/B individual personas under the same
//  scheduler.
// =============================================================================

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <random>

#include "bot_worker/event.h"
#include "bot_worker/plan.h"
#include "bot_worker/rl_policy.h"

namespace velocity::bot_worker {

struct Decision {
    Kind          kind;
    Side          side;
    std::int64_t  price;       // ignored for CANCEL
    std::uint64_t quantity;
    std::uint64_t cancel_id;   // only meaningful for CANCEL
    bool          do_nothing;  // skip this tick entirely
};

// Persona is small enough to live on the reactor's per-bot state vector;
// it must be trivially relocatable. We achieve this by storing the random
// engine and a small handful of integers — no heap pointers.
class Persona {
public:
    // The plan is borrowed; lives for the duration of the benchmark.
    Persona(PersonaKind kind, std::uint32_t bot_id, const LoadPlan* plan) noexcept;

    [[nodiscard]] auto next() noexcept -> Decision;

    // The bot tracks the id of its most recent resting order so a
    // canceller / market-maker can later cancel it.
    auto register_order(std::uint64_t correlation_id) noexcept -> void;

    [[nodiscard]] auto kind() const noexcept -> PersonaKind { return kind_; }

    [[nodiscard]] auto bot_id() const noexcept -> std::uint32_t { return bot_id_; }

    // Attach a shared RL policy (one per worker process). Personas of
    // kind ADAPTIVE without a bound policy degrade to MARKET_MAKER
    // behaviour at runtime.
    auto bind_policy(std::shared_ptr<const RLPolicy> p) noexcept -> void {
        policy_ = std::move(p);
    }

    // Update inventory / pnl bookkeeping. Called by the worker after
    // each ack/fill so the next ADAPTIVE observation reflects current
    // state. Cheap no-op for non-adaptive personas.
    auto on_fill(Side side, std::int64_t price, std::uint64_t qty) noexcept -> void;

private:
    PersonaKind     kind_;
    std::uint32_t   bot_id_;
    const LoadPlan* plan_;
    std::mt19937    rng_;
    std::uint64_t   last_order_id_{0};
    std::uint32_t   tick_{0};

    // ADAPTIVE persona state. Kept inline (no heap) so the per-bot
    // Persona object stays trivially relocatable.
    std::shared_ptr<const RLPolicy> policy_{};
    std::int64_t  position_units_{0};
    std::int64_t  realized_pnl_units_{0};
    // Signed cash flow (BUY pays out, SELL takes in). This is feature
    // index 2 of the RL observation — the training env feeds CASH there,
    // not realized PnL, so the serving observation must match.
    std::int64_t  cash_units_{0};
    std::int64_t  last_post_tick_{0};
    float         vol_ewma_{0.5F};
    std::int64_t  last_mid_seen_{0};

    // Bounded mid-price history for the windowed trend feature. The
    // training env computes trend over up to 32 steps ((mids[-1] -
    // mids[0]) / tick); a single-step delta is a different signal. Fixed
    // size keeps Persona heap-free / trivially relocatable.
    static constexpr std::size_t kMidHistory = 32;
    std::array<std::int64_t, kMidHistory> mid_hist_{};
    std::uint32_t mid_hist_count_{0};
    std::uint32_t mid_hist_pos_{0};
};

// Decide which persona a given bot should be, based on the plan's
// PersonaSlice weights. Deterministic given (bot_id, plan).
[[nodiscard]] auto persona_for_bot(std::uint32_t bot_id, const LoadPlan& plan) noexcept
    -> PersonaKind;

}  // namespace velocity::bot_worker
