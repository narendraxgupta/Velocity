// =============================================================================
//  reactor.cpp — per-thread bot-driving loop.
//
//  Design summary
//  --------------
//  We hold N persona-state objects in a flat vector (no heap pointers in the
//  hot path). At each tick we:
//
//    1. Ask the scheduler for the next intended send time.
//    2. Spin / yield until that time arrives.
//    3. Pick a bot (round-robin), ask it for a Decision, send it.
//    4. Push an Event into the publisher's SPSC ring buffer.
//    5. Drain transport completions; on each ack, push an updated Event.
//
//  This is open-loop: step (2)'s sleep is governed by intended_ts, not by
//  the previous response — so coordinated omission is impossible.
// =============================================================================

#include "bot_worker/reactor.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "bot_worker/event.h"
#include "bot_worker/persona.h"
#include "bot_worker/plan.h"
#include "bot_worker/publisher.h"
#include "bot_worker/scheduler.h"
#include "bot_worker/transport.h"

#include "velocity/common/log.h"
#include "velocity/common/signals.h"
#include "velocity/common/time.h"

namespace velocity::bot_worker {

struct Reactor::Impl {
    ReactorConfig                 cfg;
    const LoadPlan*               plan{nullptr};
    std::unique_ptr<Transport>    transport;
    Publisher*                    publisher{nullptr};
    Scheduler                     sched;
    std::vector<Persona>          bots;
    std::atomic<std::uint64_t>    orders_sent{0};
    std::atomic<std::uint64_t>    orders_acked{0};
    std::atomic<std::uint64_t>    orders_errored{0};
    std::atomic<std::uint64_t>    publish_drops{0};
    std::atomic<std::int64_t>     skew{0};
    std::atomic<bool>             stop{false};
    std::jthread                  thread;
    std::array<char, 26>          submission_id_arr{};

    // Correlation id derivation: a 64-bit value that encodes
    //   [reactor_id:8 | bot_idx:24 | sequence:32]
    // so it's globally unique and self-describing. `bot_idx` is the
    // reactor-local index into `bots`, not the globally-namespaced
    // `bot_id` — encoding the full 32-bit bot_id (which already embeds
    // reactor_id in its top byte) would waste the high byte.
    [[nodiscard]] static auto make_correlation_id(std::uint8_t reactor_id,
                                                  std::uint32_t bot_idx,
                                                  std::uint32_t seq) noexcept -> std::uint64_t {
        return (static_cast<std::uint64_t>(reactor_id) << 56) |
               (static_cast<std::uint64_t>(bot_idx & 0xFFFFFFu) << 32) |
               static_cast<std::uint64_t>(seq);
    }

    Impl(ReactorConfig c, const LoadPlan* p, std::unique_ptr<Transport> t, Publisher* pub) noexcept
        : cfg(c), plan(p), transport(std::move(t)), publisher(pub),
          sched(c.start_mono_ns, c.initial_rps) {

        bots.reserve(c.bots_per_reactor);
        for (std::uint32_t i = 0; i < c.bots_per_reactor; ++i) {
            const auto bot_id = (static_cast<std::uint32_t>(c.reactor_id) << 24) | i;
            const auto kind   = persona_for_bot(bot_id, *plan);
            bots.emplace_back(kind, bot_id, plan);
            // Only ADAPTIVE personas read the policy at next(); we bind
            // unconditionally because the shared_ptr increment is sub-ns
            // and it keeps the loop branchless.
            if (c.rl_policy) {
                bots.back().bind_policy(c.rl_policy);
            }
        }
        if (!plan->submission_id.empty()) {
            // Fixed-width 26-byte submission id (ULID). Pad with NUL if
            // the wire id is shorter; truncate if longer. We avoid
            // strncpy to make the no-NUL-terminator contract explicit.
            const auto& sid = plan->submission_id;
            const auto copy_len = std::min(sid.size(), submission_id_arr.size());
            std::memcpy(submission_id_arr.data(), sid.data(), copy_len);
            if (copy_len < submission_id_arr.size()) {
                std::memset(submission_id_arr.data() + copy_len, 0,
                            submission_id_arr.size() - copy_len);
            }
        }
    }

    auto loop() -> void {
        VLOG_INFO("reactor {} entering loop bots={} rps={}",
                  cfg.reactor_id, cfg.bots_per_reactor, cfg.initial_rps);

        // Hook the transport ack callback once. On each ack we resolve the
        // pending Event and push a completed version to the publisher.
        transport->set_ack_callback(
            [this](std::uint64_t cid, Outcome out, std::int64_t ack_ts,
                   std::int64_t fp, std::uint64_t fq) {
                // Real per-order outcome accounting for controller heartbeats.
                switch (out) {
                    case Outcome::ACK:
                    case Outcome::FILLED:
                    case Outcome::PARTIAL:
                        orders_acked.fetch_add(1, std::memory_order_relaxed);
                        break;
                    case Outcome::REJECT:
                    case Outcome::TIMEOUT:
                        orders_errored.fetch_add(1, std::memory_order_relaxed);
                        break;
                    default:
                        break;  // UNKNOWN / CANCELLED: neither acked nor errored
                }
                Event ev{};
                ev.submission_id  = submission_id_arr;
                ev.correlation_id = cid;
                ev.ack_ts_ns      = ack_ts;
                ev.fill_price     = fp;
                ev.fill_quantity  = fq;
                // The ack event mirrors the original request's kind; we
                // record NEW because the ingester pairs by correlation_id
                // and inherits the original intent.
                ev.kind           = Kind::NEW;
                ev.outcome        = out;
                ev.reactor_id     = cfg.reactor_id;
                // Bot id is the middle 24 bits of the correlation id;
                // restore the global namespace by ORing in reactor_id.
                ev.bot_id         =
                    (static_cast<std::uint32_t>(cfg.reactor_id) << 24) |
                    static_cast<std::uint32_t>((cid >> 32) & 0xFFFFFFu);
                if (!publisher->queue_for(cfg.reactor_id).push(ev)) {
                    publish_drops.fetch_add(1, std::memory_order_relaxed);
                }
            });

        std::uint32_t bot_cursor = 0;
        std::uint32_t seq        = 0;
        bool          was_paused = false;

        while (!stop.load(std::memory_order_acquire) &&
               !velocity::signals::shutdown_requested()) {

            // 0. Paused (target RPS == 0): generate zero load. Keep draining
            //    transport completions so in-flight acks still resolve, then
            //    sleep briefly and re-check.
            if (sched.paused()) {
                was_paused = true;
                transport->poll(16);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            // Resuming from a pause: reset the intended-time cursor to now so
            // we don't burst-send to "catch up" on intended timestamps that
            // elapsed while paused.
            if (was_paused) {
                sched.resync(velocity::time::monotonic_ns());
                was_paused = false;
            }

            // 1. Next intended send time.
            const auto intended = sched.next();

            // 2. Wait until intended_ts arrives. Use a hybrid spin/sleep:
            //    sleep down to within 100µs of the target, then busy-wait.
            for (;;) {
                const auto now = velocity::time::monotonic_ns();
                const auto gap = intended - now;
                if (gap <= 0) {
                    skew.store(sched.skew_ns(now), std::memory_order_relaxed);
                    break;
                }
                if (gap > 100'000) {
                    std::this_thread::sleep_for(std::chrono::nanoseconds(gap - 100'000));
                } else {
                    // 100µs busy-wait. Service transport completions while we wait.
                    transport->poll(8);
                }
            }

            // 3. Pick the next bot, get a decision. Capture the index
            //    *before* advancing the cursor so the correlation id
            //    encodes the bot that produced the decision, not the
            //    next one.
            const auto bot_idx = bot_cursor;
            bot_cursor = (bot_cursor + 1) % bots.size();
            auto& bot  = bots[bot_idx];
            const auto decision = bot.next();
            if (decision.do_nothing) continue;

            // 4. Mint a correlation id and dispatch. The cid encodes
            //    reactor_id + the reactor-local bot_idx + a monotonic
            //    sequence so the combined value is unique cluster-wide
            //    and self-attributing.
            const auto cid = make_correlation_id(cfg.reactor_id, bot_idx, seq++);
            if (decision.kind == Kind::NEW) {
                bot.register_order(cid);
            }
            const auto sent_at = velocity::time::monotonic_ns();
            const auto ok      = transport->send(cid, decision);

            // 5. Push the *intent* event immediately. The transport will
            //    later raise the ack callback which emits a paired
            //    completion event sharing the same correlation_id.
            Event ev{};
            ev.submission_id  = submission_id_arr;
            ev.correlation_id = cid;
            ev.intended_ts_ns = intended;
            ev.sent_ts_ns     = sent_at;
            ev.ack_ts_ns      = 0;
            ev.price          = decision.price;
            ev.quantity       = decision.quantity;
            ev.kind           = decision.kind;
            ev.side           = decision.side;
            ev.outcome        = ok ? Outcome::UNKNOWN : Outcome::REJECT;
            ev.reactor_id     = cfg.reactor_id;
            ev.bot_id         = bot.bot_id();
            if (!publisher->queue_for(cfg.reactor_id).push(ev)) {
                publish_drops.fetch_add(1, std::memory_order_relaxed);
            }

            if (ok) {
                orders_sent.fetch_add(1, std::memory_order_relaxed);
            }

            // 6. Drain any completions that became ready while we were
            //    setting up. Limits queue-depth growth.
            transport->poll(16);
        }

        VLOG_INFO("reactor {} draining transport", cfg.reactor_id);
        // Give in-flight requests a final chance to land.
        for (int i = 0; i < 50; ++i) {
            transport->poll(64);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};

Reactor::Reactor(ReactorConfig cfg, const LoadPlan* plan,
                 std::unique_ptr<Transport> transport, Publisher* publisher) noexcept
    : impl_(std::make_unique<Impl>(cfg, plan, std::move(transport), publisher)) {}

Reactor::~Reactor() { stop(); }

auto Reactor::start() -> void {
    impl_->thread = std::jthread([this]() { impl_->loop(); });
}

auto Reactor::stop() noexcept -> void {
    if (!impl_) return;
    impl_->stop.store(true, std::memory_order_release);
    if (impl_->thread.joinable()) {
        impl_->thread.request_stop();
        // Synchronously join so the caller knows the loop has exited and
        // the transport is no longer being driven. Without this, the
        // publisher could be torn down before the reactor stops pushing
        // events into it.
        impl_->thread.join();
    }
}

auto Reactor::set_rate(std::uint64_t rps) noexcept -> void {
    impl_->sched.set_rate(rps);
}

auto Reactor::orders_sent() const noexcept -> std::uint64_t {
    return impl_->orders_sent.load(std::memory_order_relaxed);
}
auto Reactor::orders_acked() const noexcept -> std::uint64_t {
    return impl_->orders_acked.load(std::memory_order_relaxed);
}
auto Reactor::orders_errored() const noexcept -> std::uint64_t {
    return impl_->orders_errored.load(std::memory_order_relaxed);
}
auto Reactor::skew_ns() const noexcept -> std::int64_t {
    return impl_->skew.load(std::memory_order_relaxed);
}

}  // namespace velocity::bot_worker
