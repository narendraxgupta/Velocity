// =============================================================================
//  persona.cpp — Persona base implementation + per-kind dispatch.
//
//  Each persona's tick is a few lines — the interest is in the *kinds* of
//  behaviour we mix together. The interesting work happens at the scheduler
//  and transport layers; the persona just sketches what to send.
// =============================================================================

#include "bot_worker/persona.h"

#include <algorithm>
#include <cmath>

namespace velocity::bot_worker {

namespace {

// Slide a small random amount around the fair value to produce a price.
[[nodiscard]] auto jitter_price(std::int64_t fair, std::int64_t tick, int ticks_offset,
                                std::mt19937& rng) noexcept -> std::int64_t {
    std::uniform_int_distribution<int> jitter(-2, 2);
    return fair + tick * (ticks_offset + jitter(rng));
}

[[nodiscard]] auto random_qty(std::mt19937& rng, std::uint64_t lo, std::uint64_t hi) noexcept
    -> std::uint64_t {
    std::uniform_int_distribution<std::uint64_t> d(lo, hi);
    return d(rng);
}

}  // namespace

// -----------------------------------------------------------------------------
//  Persona assignment.
// -----------------------------------------------------------------------------
auto persona_for_bot(std::uint32_t bot_id, const LoadPlan& plan) noexcept -> PersonaKind {
    if (plan.personas.empty()) return PersonaKind::NOISE;

    // Compute cumulative weights; pick by bot_id modulo a 10,000-bucket
    // partition so the mapping is deterministic.
    float total = 0;
    for (const auto& p : plan.personas) total += p.weight;
    if (total <= 0) return plan.personas.front().kind;

    const float r = static_cast<float>(bot_id % 10000) / 10000.0F;
    float acc = 0;
    for (const auto& p : plan.personas) {
        acc += p.weight / total;
        if (r < acc) return p.kind;
    }
    return plan.personas.back().kind;
}

// -----------------------------------------------------------------------------
//  Persona ctor.
// -----------------------------------------------------------------------------
Persona::Persona(PersonaKind kind, std::uint32_t bot_id, const LoadPlan* plan) noexcept
    : kind_(kind), bot_id_(bot_id), plan_(plan),
      // Seed mixes the bot id with a small constant for stable but distinct
      // streams across bots.
      rng_(std::mt19937{static_cast<std::uint32_t>(bot_id) ^ 0x9E3779B9U}) {}

auto Persona::register_order(std::uint64_t correlation_id) noexcept -> void {
    last_order_id_ = correlation_id;
}

auto Persona::on_fill(Side side, std::int64_t price, std::uint64_t qty) noexcept -> void {
    // Only the ADAPTIVE persona needs this state; the rule-based
    // personas operate stateless on plan-scope inputs. We update
    // unconditionally — the math is six ops and keeps the data hot if
    // we ever extend other personas to consume it.
    const std::int64_t signed_qty = static_cast<std::int64_t>(qty) *
                                    (side == Side::BUY ? 1 : -1);
    // Cash flow mirrors the training env (velocity_market_env.step): a BUY
    // pays cash out, a SELL takes cash in. The ADAPTIVE observation feeds
    // CASH at feature index 2, so it must be tracked exactly as the policy
    // was trained — feeding realized PnL there was a train/serve skew.
    cash_units_ += -signed_qty * price;
    // Realized PnL: when we close any of our existing inventory, the
    // P&L is (fill_price - position_avg_price) * closed_qty. We track
    // a simple sign-flip-aware approximation since we don't carry the
    // average cost separately.
    if ((position_units_ > 0 && side == Side::SELL) ||
        (position_units_ < 0 && side == Side::BUY)) {
        const std::int64_t closed = std::min<std::int64_t>(
            std::abs(position_units_), static_cast<std::int64_t>(qty));
        // Cheap proxy: assume previous fills landed at fair_value -
        // tick_size for bids, +tick_size for asks. Good enough for the
        // RL agent's reward signal during training and inference.
        if (plan_) {
            const std::int64_t baseline = plan_->fair_value;
            realized_pnl_units_ += closed * (side == Side::SELL
                ? (price - baseline) : (baseline - price));
        }
    }
    position_units_ += signed_qty;
}

// -----------------------------------------------------------------------------
//  Persona dispatch.
// -----------------------------------------------------------------------------
auto Persona::next() noexcept -> Decision {
    ++tick_;
    if (!plan_) return Decision{Kind::NEW, Side::BUY, 0, 0, 0, true};

    const auto fair = plan_->fair_value;
    const auto tick = plan_->tick_size;
    const Side bias = (bot_id_ % 2 == 0) ? Side::BUY : Side::SELL;

    switch (kind_) {
        // ----------------- Market maker ------------------------------------
        // Posts at fair ± 1 tick alternately; cancels its previous order
        // every 8th tick to keep the book churning.
        case PersonaKind::MARKET_MAKER: {
            if (last_order_id_ != 0 && tick_ % 8 == 0) {
                Decision d{
                    .kind       = Kind::CANCEL,
                    .side       = bias,
                    .price      = 0,
                    .quantity   = 0,
                    .cancel_id  = last_order_id_,
                    .do_nothing = false,
                };
                last_order_id_ = 0;
                return d;
            }
            return Decision{
                Kind::NEW,
                bias,
                bias == Side::BUY ? fair - tick : fair + tick,
                random_qty(rng_, 1, 10),
                0,
                false,
            };
        }

        // ----------------- Aggressive taker --------------------------------
        // Always crosses the spread by 2 ticks → guaranteed match (assuming
        // a healthy market-maker fleet is also live).
        case PersonaKind::AGGRESSIVE_TAKER: {
            const Side s = (tick_ % 2 == 0) ? Side::BUY : Side::SELL;
            return Decision{
                Kind::NEW,
                s,
                s == Side::BUY ? fair + tick * 2 : fair - tick * 2,
                random_qty(rng_, 1, 5),
                0,
                false,
            };
        }

        // ----------------- Canceller ---------------------------------------
        // Posts then cancels on the *very next* tick. Stress-tests the
        // submission's cancel pipeline.
        case PersonaKind::CANCELLER: {
            if (last_order_id_ != 0) {
                Decision d{
                    .kind       = Kind::CANCEL,
                    .side       = bias,
                    .price      = 0,
                    .quantity   = 0,
                    .cancel_id  = last_order_id_,
                    .do_nothing = false,
                };
                last_order_id_ = 0;
                return d;
            }
            return Decision{
                Kind::NEW,
                bias,
                jitter_price(fair, tick, bias == Side::BUY ? -3 : 3, rng_),
                random_qty(rng_, 1, 3),
                0,
                false,
            };
        }

        // ----------------- Spoofer -----------------------------------------
        // Posts 10 deep, far from fair; then bulk cancels. Tests queue depth.
        case PersonaKind::SPOOFER: {
            const bool cancel_phase = (tick_ % 16) >= 8;
            if (cancel_phase && last_order_id_ != 0) {
                Decision d{
                    .kind       = Kind::CANCEL,
                    .side       = bias,
                    .price      = 0,
                    .quantity   = 0,
                    .cancel_id  = last_order_id_,
                    .do_nothing = false,
                };
                last_order_id_ = 0;
                return d;
            }
            return Decision{
                Kind::NEW,
                bias,
                jitter_price(fair, tick, bias == Side::BUY ? -10 : 10, rng_),
                random_qty(rng_, 50, 200),  // big resting size
                0,
                false,
            };
        }

        // ----------------- Adaptive (RL) -----------------------------------
        //
        // Builds a 12-feature observation, asks the loaded ONNX policy
        // for an action, then maps the action back onto our Decision
        // schema. If the policy isn't loaded (e.g. dev build without
        // ONNX Runtime, or the model file is missing) we silently fall
        // back to market-maker behaviour — there's no correct way to
        // hard-fail at the per-bot scope.
        case PersonaKind::ADAPTIVE: {
            if (!policy_ || !policy_->enabled()) {
                // Same posting logic as the market_maker branch above
                // but inlined here so callers can A/B the kinds without
                // worrying about fallthrough semantics.
                return Decision{
                    Kind::NEW,
                    bias,
                    bias == Side::BUY ? fair - tick : fair + tick,
                    random_qty(rng_, 1, 10),
                    0,
                    false,
                };
            }

            // Build the observation from per-bot state plus broad
            // plan-scope metrics. Vol/queue/imbalance are
            // best-effort proxies — the worker doesn't see the
            // submitter's book live, but the policy was trained
            // against a similar surrogate.
            const auto mid = fair;
            const auto move_ticks = (last_mid_seen_ == 0)
                ? 0.0F
                : static_cast<float>((mid - last_mid_seen_) / std::max<int64_t>(tick, 1));
            // EWMA vol tracks the per-step move, matching the training
            // env's _vol_ema update.
            vol_ewma_ = 0.92F * vol_ewma_ + 0.08F * std::abs(move_ticks);
            last_mid_seen_ = mid;

            // Windowed trend over up to kMidHistory steps, matching the
            // training env's (mids[-1] - mids[0]) / tick. Push the current
            // mid, then diff against the oldest retained sample. A single-
            // step delta (the previous behaviour) is a different feature
            // than the policy was trained on.
            mid_hist_[mid_hist_pos_] = mid;
            mid_hist_pos_ = (mid_hist_pos_ + 1) % kMidHistory;
            if (mid_hist_count_ < kMidHistory) ++mid_hist_count_;
            const std::int64_t oldest_mid =
                (mid_hist_count_ == kMidHistory) ? mid_hist_[mid_hist_pos_]
                                                 : mid_hist_[0];
            const float trend_ticks = static_cast<float>(
                (mid - oldest_mid) / std::max<std::int64_t>(tick, 1));

            const RLObservation obs{
                /*fair_price*/    static_cast<float>(std::log(std::max<int64_t>(mid, 1)) / 100.0),
                /*position*/      static_cast<float>(position_units_) / 100.0F,
                /*cash*/          static_cast<float>(cash_units_) / 100.0F,
                /*book_imbalance*/ 0.0F,
                /*spread_ticks*/   2.0F,
                /*recent_fills*/   0.5F,
                /*recent_cancels*/ 0.5F,
                /*queue_position*/ 0.5F,
                /*trend_ticks*/    trend_ticks,
                /*vol_ema*/        vol_ewma_,
                /*order_age_ms*/   std::clamp(
                                       static_cast<float>((tick_ - last_post_tick_) * 10),
                                       0.0F, 5000.0F),
                /*tick_phase*/     static_cast<float>(tick_ % 1024) / 1024.0F,
            };
            const auto act = policy_->infer(obs);
            const auto sprd = std::max<std::int64_t>(
                1, static_cast<std::int64_t>(act.spread_ticks * static_cast<float>(tick)));
            const auto qty = std::max<std::uint64_t>(
                1, static_cast<std::uint64_t>(act.size_scale * 10.0F));

            switch (act.kind) {
                case RLAction::Kind::POST_BID:
                    last_post_tick_ = tick_;
                    return Decision{Kind::NEW, Side::BUY,  fair - sprd, qty, 0, false};
                case RLAction::Kind::POST_ASK:
                    last_post_tick_ = tick_;
                    return Decision{Kind::NEW, Side::SELL, fair + sprd, qty, 0, false};
                case RLAction::Kind::CANCEL:
                    if (last_order_id_ != 0) {
                        Decision d{Kind::CANCEL, bias, 0, 0, last_order_id_, false};
                        last_order_id_ = 0;
                        return d;
                    }
                    return Decision{Kind::NEW, bias, 0, 0, 0, true};
                case RLAction::Kind::NOOP:
                default:
                    return Decision{Kind::NEW, bias, 0, 0, 0, true};
            }
        }

        // ----------------- Noise -------------------------------------------
        case PersonaKind::NOISE:
        default: {
            std::uniform_int_distribution<int> side_dist(0, 1);
            const Side s = side_dist(rng_) == 0 ? Side::BUY : Side::SELL;
            return Decision{
                Kind::NEW,
                s,
                jitter_price(fair, tick, s == Side::BUY ? -1 : 1, rng_),
                random_qty(rng_, 1, 4),
                0,
                false,
            };
        }
    }
}

}  // namespace velocity::bot_worker
