// =============================================================================
//  bot_worker/rl_policy.h
//
//  RLPolicy — thin wrapper around an ONNX Runtime session for adaptive
//  persona decision-making.
//
//  Why ONNX?
//  ---------
//  We train in Python with stable-baselines3 (PPO) because the RL
//  tooling there is best-in-class. Running Python inference inside the
//  bot-worker hot path is a non-starter — GIL, allocator hostility,
//  and 3-OoM more latency than the rest of the persona dispatch. ONNX
//  lets us export the trained policy as a static graph and execute it
//  with ORT's bare-C++ runtime, which lives entirely on the stack-like
//  Arena allocator we configure at startup.
//
//  Build-time switch
//  -----------------
//  When the worker is built WITHOUT onnxruntime (e.g. dev images where
//  the dep is heavy and unwanted), `VELOCITY_HAS_ONNXRUNTIME` is left
//  undefined and the symbols here become no-op stubs that always
//  return a flat "do nothing" decision. The ADAPTIVE persona then
//  silently degrades to a market_maker (see persona.cpp).
// =============================================================================

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace velocity::bot_worker {

// Compact 12-feature observation vector consumed by the policy. Keep in
// sync with `tools/rl-bot/velocity_market_env.py::Observation`.
//
// Order is fixed so we can pass it as a contiguous float buffer to ORT.
struct RLObservation {
    float fair_price;       // log(fair / 100), normalised
    float position;         // current inventory, units / 100
    float pnl;              // mark-to-market PnL, units / 100
    float book_imbalance;   // (bid_qty - ask_qty) / (bid_qty + ask_qty), [-1,1]
    float spread_ticks;     // (best_ask - best_bid) / tick, clamped to 32
    float recent_fills;     // fills in last 100ms, normalised by max
    float recent_cancels;   // cancels in last 100ms, normalised by max
    float queue_position;   // estimated position in queue, [0, 1]
    float trend_ticks;      // mid Δ over last 1s in ticks
    float vol_ema;          // EWMA std of mid moves, ticks
    float order_age_ms;     // ms since last resting order (capped 5000)
    float tick_phase;       // mod-1024 tick counter as fraction
};
static_assert(sizeof(RLObservation) == 12 * sizeof(float),
              "RLObservation layout must be packed");

// Policy outputs a 3-element categorical (POST_BID / POST_ASK / CANCEL)
// plus a continuous "spread_ticks" suggestion in [0,8].
struct RLAction {
    enum class Kind : std::uint8_t { POST_BID = 0, POST_ASK = 1, CANCEL = 2, NOOP = 3 };
    Kind  kind;
    float spread_ticks;     // distance from fair, in ticks
    float size_scale;       // qty scale factor [0,1]
};

// RLPolicy is shared across reactor threads (it's read-only after load
// and ORT sessions are thread-safe for inference). One per worker.
class RLPolicy {
public:
    // Loads the policy graph from disk. Returns nullptr (not throws)
    // when ONNX Runtime is disabled at build time or the file is
    // missing; callers are expected to gracefully degrade.
    [[nodiscard]] static auto load(const std::string& onnx_path)
        -> std::shared_ptr<RLPolicy>;

    [[nodiscard]] auto infer(const RLObservation& obs) const noexcept -> RLAction;

    [[nodiscard]] auto enabled() const noexcept -> bool;

    ~RLPolicy();

    // Implementation details live behind a pimpl so callers don't need
    // to drag onnxruntime headers into their TUs.
    struct Impl;

private:
    explicit RLPolicy(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace velocity::bot_worker
