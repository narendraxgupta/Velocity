// =============================================================================
//  validator.cpp — the live correctness-validation pipeline.
//
//  Pulls OrderEvent records off `telemetry.raw`, replays each event through
//  a per-submission ReferenceOrderbook, compares the submission's reported
//  fills against the reference fills, and emits a CorrectnessReport every
//  second (or every 10,000 events, whichever comes first) to `telemetry.fills`.
//
//  Concurrency model
//  -----------------
//  Single thread per consumer. Orders for the same submission_id always
//  land on the same partition (the bot worker uses submission_id as the
//  Kafka partition key), so per-submission state is partition-local and
//  needs no locking.
// =============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <librdkafka/rdkafkacpp.h>
#include <nlohmann/json.hpp>
#include <sw/redis++/redis++.h>

#include "correctness_validator/execution_quality.h"
#include "correctness_validator/microstructure.h"
#include "correctness_validator/reference_orderbook.h"
#include "correctness_validator/validator.h"
#include "common.pb.h"
#include "telemetry.pb.h"

#include "velocity/common/log.h"
#include "velocity/common/signals.h"
#include "velocity/common/time.h"

namespace velocity::correctness_validator {
namespace {

// An order awaiting its completion event. The bot-worker emits two
// OrderEvents per order sharing a correlation id: an INTENT (real
// price/qty/side, ack==0) and a COMPLETION (the submission's reported
// outcome + fills, ack>0). The reference book must be driven by intents in
// send order, but the submission's reported fills only arrive with the
// completion — so we replay+stash on the intent and reconcile on the
// completion. Without this the validator submitted BOTH events (the
// completion as a bogus price=0 order) and reconciled garbage.
struct PendingOrder {
    std::vector<Fill> reference_fills;  // what our book produced at submit
    Side          side{Side::BUY};
    std::int64_t  price{0};
    std::uint64_t quantity{0};
    TimeInForce   tif{TimeInForce::GTC};
    std::int64_t  ts_ns{0};
    std::int64_t  arrived_ns{0};        // realtime clock — for TTL eviction
};

// Orders whose completion never arrives (submission hung / dropped) are
// evicted after this long and counted as missing so the map stays bounded.
constexpr std::int64_t kPendingTtlNs = 30LL * 1'000'000'000;   // 30s
constexpr std::size_t  kMaxPending   = 2'000'000;

// One submission's accumulator: the reference book plus running counters.
struct SubmissionState {
    ReferenceOrderbook book;
    // Orders submitted to the reference book, keyed by correlation id,
    // awaiting their completion event for reconciliation.
    std::unordered_map<std::uint64_t, PendingOrder> pending;
    // Parallel microstructure validator — runs the same tape through a
    // separate gold book that understands iceberg / GTD / post-only /
    // STP / pro-rata. Counters surfaced under a distinct Redis key so
    // judges can drill in without one validator masking the other.
    MicrostructureValidator micro;
    std::uint64_t micro_violations{0};
    // Execution-quality tracker: per-order vwap → slippage / IS /
    // reversion (5s post-trade window). Runs alongside the orderbook
    // submit so we always have a fresh decision_mid.
    ExecutionQualityTracker exec_quality{/*reversion_window_ms=*/5'000};
    std::uint64_t expected_fills{0};       // fills our reference book produced
    std::uint64_t actual_fills{0};         // fills the submission reported
    std::uint64_t correct_fills{0};        // both sides agreed on a fill
    std::uint64_t priority_violations{0};  // out-of-order at the same price
    std::uint64_t price_violations{0};     // filled better than the book
    std::uint64_t phantom_fills{0};        // submission reported a fill we didn't
    std::uint64_t missing_fills{0};        // we expected a fill, submission didn't
    std::int64_t  window_start_ns{0};
    std::int64_t  last_report_ns{0};
    // Last orderbook snapshot push (mono ns). Snapshots are written to
    // Redis every 100ms so the frontend's replay viewer can scrub through
    // the run at sub-second granularity without spamming Redis. See
    // push_orderbook_snapshot below.
    std::int64_t  last_snapshot_ns{0};
};

// Push an L2 depth snapshot to Redis under
// `orderbook:<submission_id>:t:<elapsed_ms>` (HSET of bids/asks JSON
// arrays). The viewer's scrubber binary-searches the list of elapsed-ms
// keys, so we also LPUSH the elapsed_ms onto an index list
// `orderbook:<submission_id>:index` (capped at 600 entries = 60 seconds
// at 10 Hz, which comfortably covers every standard benchmark profile).
auto push_orderbook_snapshot(sw::redis::Redis* redis,
                              const std::string& submission_id,
                              const SubmissionState& s,
                              std::int64_t elapsed_ms) -> void {
    if (!redis) return;
    try {
        nlohmann::json bids = nlohmann::json::array();
        for (const auto& row : s.book.top_bids(20)) {
            bids.push_back({{"price", row.price}, {"qty", row.quantity}});
        }
        nlohmann::json asks = nlohmann::json::array();
        for (const auto& row : s.book.top_asks(20)) {
            asks.push_back({{"price", row.price}, {"qty", row.quantity}});
        }
        nlohmann::json snap{
            {"elapsed_ms", elapsed_ms},
            {"bids", std::move(bids)},
            {"asks", std::move(asks)},
        };
        const auto key = "orderbook:" + submission_id +
                         ":t:" + std::to_string(elapsed_ms);
        redis->set(key, snap.dump());
        redis->expire(key, 30 * 60);  // 30-minute TTL — same as the run
        // Index for scrubber: LPUSH bounded to 600 entries.
        const auto idx_key = "orderbook:" + submission_id + ":index";
        redis->lpush(idx_key, std::to_string(elapsed_ms));
        redis->ltrim(idx_key, 0, 599);
        redis->expire(idx_key, 30 * 60);
    } catch (const std::exception& e) {
        static std::atomic<int> warn{0};
        if ((warn++ % 32) == 0) {
            VLOG_WARN("orderbook snapshot push failed: {}", e.what());
        }
    }
}

[[nodiscard]] auto map_side(velocity::common::v1::Side wire_side) noexcept -> Side {
    return wire_side == velocity::common::v1::Side::SIDE_BUY ? Side::BUY : Side::SELL;
}

[[nodiscard]] auto map_type(velocity::common::v1::OrderType t) noexcept -> OrderType {
    using OT = velocity::common::v1::OrderType;
    return (t == OT::ORDER_TYPE_MARKET) ? OrderType::MARKET : OrderType::LIMIT;
}

[[nodiscard]] auto map_tif(velocity::common::v1::OrderType t) noexcept -> TimeInForce {
    using OT = velocity::common::v1::OrderType;
    switch (t) {
        case OT::ORDER_TYPE_IOC: return TimeInForce::IOC;
        case OT::ORDER_TYPE_FOK: return TimeInForce::FOK;
        default:                 return TimeInForce::GTC;
    }
}

// Outcome of a single reconciliation pass — used to drive the mismatch
// emitter as well as the running counters.
enum class MismatchKind : std::uint8_t {
    NONE,
    PHANTOM,
    MISSING,
    PRICE,
};

// Compare the submission-reported fills against the reference fills.
auto reconcile(SubmissionState& s,
               const std::vector<Fill>& reference,
               const velocity::telemetry::v1::OrderEvent& event) -> MismatchKind {
    using OC = velocity::telemetry::v1::Outcome;
    const bool reported = event.outcome() == OC::OUTCOME_FILLED ||
                          event.outcome() == OC::OUTCOME_PARTIAL_FILL;

    s.expected_fills += reference.size();
    if (reported) ++s.actual_fills;

    if (reference.empty() && !reported) return MismatchKind::NONE;
    if (reference.empty() &&  reported) { ++s.phantom_fills; return MismatchKind::PHANTOM; }
    if (!reference.empty() && !reported){ ++s.missing_fills; return MismatchKind::MISSING; }

    // Both produced fills. Compare aggregate notional in __int128 so we
    // don't silently overflow at extreme prices/quantities.
    std::uint64_t ref_qty = 0;
#if defined(__SIZEOF_INT128__)
    __int128 ref_notional = 0;
#else
    std::int64_t ref_notional = 0;
#endif
    for (const auto& f : reference) {
        ref_qty      += f.quantity;
        ref_notional += static_cast<decltype(ref_notional)>(f.price) *
                        static_cast<decltype(ref_notional)>(f.quantity);
    }
    const auto reported_qty =
        static_cast<std::uint64_t>(event.fill_quantity().units());
    const auto reported_notional =
        static_cast<decltype(ref_notional)>(event.fill_price().units()) *
        static_cast<decltype(ref_notional)>(reported_qty);

    if (reported_qty != ref_qty || reported_notional != ref_notional) {
        ++s.price_violations;
        return MismatchKind::PRICE;
    }
    // The aggregate reported fill matches the reference book's fills for
    // this order. `expected_fills` is counted per reference fill (an order
    // can sweep N maker levels), so credit `correct_fills` by the same N —
    // counting it as a single fill made a perfectly-correct sweep score
    // reference.size()/1, i.e. correctness ≈ 1/N instead of 100%.
    s.correct_fills += reference.size();
    return MismatchKind::NONE;
}

// Evict pending orders whose completion never arrived (submission hung or
// the completion was dropped). A reference fill we expected but never saw
// the submission report is, by definition, a missing fill.
auto evict_stale_pending(SubmissionState& s, std::int64_t now_ns) -> void {
    for (auto it = s.pending.begin(); it != s.pending.end();) {
        if (now_ns - it->second.arrived_ns > kPendingTtlNs) {
            const auto refs = it->second.reference_fills.size();
            s.expected_fills += refs;
            if (refs > 0) ++s.missing_fills;
            it = s.pending.erase(it);
        } else {
            ++it;
        }
    }
}

// Push a JSON-serialised mismatch sample into Redis so the API gateway
// can render it under `/v1/submissions/{id}/mismatches`. We cap the list
// to `cap` entries with LTRIM so an out-of-control submission can't
// fill Redis with mismatches. Silently no-ops when `redis` is null.
auto push_mismatch(sw::redis::Redis* redis,
                   const std::string& submission_id,
                   MismatchKind kind,
                   const velocity::telemetry::v1::OrderEvent& event,
                   const std::vector<Fill>& reference,
                   std::uint32_t cap) -> void {
    if (!redis || kind == MismatchKind::NONE) return;
    try {
        nlohmann::json sample{
            {"ts_ns",          velocity::time::realtime_ns()},
            {"correlation_id", event.correlation_id().low()},
            {"kind",
             kind == MismatchKind::PHANTOM ? "phantom"
             : kind == MismatchKind::MISSING ? "missing"
             :                                  "price"},
            {"reported", {
                {"quantity", static_cast<std::uint64_t>(event.fill_quantity().units())},
                {"price",    static_cast<std::int64_t>(event.fill_price().units())},
                {"outcome",  static_cast<int>(event.outcome())},
            }},
        };
        auto& ref = sample["reference"];
        ref = nlohmann::json::array();
        for (const auto& f : reference) {
            ref.push_back({
                {"price",    f.price},
                {"quantity", f.quantity},
            });
        }
        const auto key = "mismatches:" + submission_id;
        redis->lpush(key, sample.dump());
        redis->ltrim(key, 0, static_cast<long long>(cap) - 1);
        // Expire after a day so empty submissions don't leak keys.
        redis->expire(key, std::chrono::seconds{86'400});
    } catch (const std::exception& e) {
        static std::atomic<int> warn{0};
        if ((warn++ % 64) == 0) {
            VLOG_WARN("validator: mismatch push failed: {}", e.what());
        }
    }
}

// Emit a snapshot report for one submission. We publish to `telemetry.fills`
// so the leaderboard/scorer can consume it.
auto emit_report(const std::string& submission_id,
                 SubmissionState& s,
                 RdKafka::Producer& producer,
                 const std::string& topic) -> void {
    velocity::telemetry::v1::CorrectnessReport rpt;
    rpt.mutable_submission_id()->set_value(submission_id);
    const auto now_ns = velocity::time::realtime_ns();
    rpt.set_window_start_ns(s.window_start_ns);
    rpt.set_window_end_ns(now_ns);
    rpt.set_expected_fills(s.expected_fills);
    rpt.set_actual_fills(s.actual_fills);
    rpt.set_correct_fills(s.correct_fills);
    rpt.set_priority_violations(s.priority_violations);
    rpt.set_price_violations(s.price_violations);
    rpt.set_phantom_fills(s.phantom_fills);
    rpt.set_missing_fills(s.missing_fills);

    // Per docs/scoring.md §4: correctness = 100 * correct / max(1, expected).
    // Using expected (not max(expected,actual)) so that a submission can't
    // mask missing fills by over-reporting fills it didn't actually make
    // — the phantom fills are penalised separately.
    const auto denom = std::max<std::uint64_t>(1, s.expected_fills);
    rpt.set_correctness_score(
        100.0 * static_cast<double>(s.correct_fills) / static_cast<double>(denom));

    s.window_start_ns = now_ns;

    std::string payload;
    rpt.SerializeToString(&payload);

    auto err = producer.produce(
        topic,
        RdKafka::Topic::PARTITION_UA,
        RdKafka::Producer::RK_MSG_COPY,
        payload.data(), payload.size(),
        submission_id.data(), submission_id.size(),
        0, nullptr, nullptr);
    if (err != RdKafka::ERR_NO_ERROR) {
        VLOG_WARN("correctness produce failed: {}", RdKafka::err2str(err));
    }
}

}  // namespace

// -----------------------------------------------------------------------------
//  Entry point used by main.cpp.
// -----------------------------------------------------------------------------
auto run(ValidatorConfig cfg) -> void {
    std::string errstr;

    // Optional Redis client for mismatch samples. We don't fail startup
    // if Redis is unreachable — the validator still produces correctness
    // reports; only the mismatch drilldown UI becomes empty.
    std::unique_ptr<sw::redis::Redis> redis;
    if (!cfg.redis_addr.empty()) {
        try {
            redis = std::make_unique<sw::redis::Redis>(cfg.redis_addr);
            redis->ping();
            VLOG_INFO("validator: connected to redis at {}", cfg.redis_addr);
        } catch (const std::exception& e) {
            VLOG_WARN("validator: redis unavailable ({}); mismatches disabled",
                      e.what());
            redis.reset();
        }
    }

    // ---- Consumer setup. Commits are driven manually so that we only
    // ack offsets after we've emitted (or buffered) the corresponding
    // CorrectnessReport. This makes restarts deterministic — replayed
    // events don't double-count violations.
    std::unique_ptr<RdKafka::Conf> consumer_conf{
        RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL)};
    consumer_conf->set("bootstrap.servers",  cfg.brokers,           errstr);
    consumer_conf->set("group.id",           cfg.group_id,          errstr);
    consumer_conf->set("enable.auto.commit", "false",               errstr);
    consumer_conf->set("auto.offset.reset",  "latest",              errstr);
    consumer_conf->set("fetch.min.bytes",    "65536",               errstr);
    consumer_conf->set("fetch.wait.max.ms",  "50",                  errstr);

    std::unique_ptr<RdKafka::KafkaConsumer> consumer{
        RdKafka::KafkaConsumer::create(consumer_conf.get(), errstr)};
    if (!consumer) {
        throw std::runtime_error("failed to create Kafka consumer: " + errstr);
    }
    const auto sub_err = consumer->subscribe({cfg.telemetry_topic});
    if (sub_err != RdKafka::ERR_NO_ERROR) {
        throw std::runtime_error("subscribe failed: " + RdKafka::err2str(sub_err));
    }

    // ---- Producer setup
    std::unique_ptr<RdKafka::Conf> producer_conf{
        RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL)};
    producer_conf->set("bootstrap.servers", cfg.brokers,    errstr);
    producer_conf->set("linger.ms",         "5",            errstr);
    producer_conf->set("acks",              "1",            errstr);
    producer_conf->set("compression.type",  "lz4",          errstr);

    std::unique_ptr<RdKafka::Producer> producer{
        RdKafka::Producer::create(producer_conf.get(), errstr)};
    if (!producer) {
        throw std::runtime_error("failed to create Kafka producer: " + errstr);
    }

    // ---- State
    std::unordered_map<std::string, SubmissionState> state;
    constexpr auto kReportEveryNs = std::int64_t{1'000'000'000};  // 1 sec
    int commit_tick = 0;

    VLOG_INFO("validator consuming from '{}' → reports to '{}'",
              cfg.telemetry_topic, cfg.fills_topic);

    while (!velocity::signals::shutdown_requested()) {
        std::unique_ptr<RdKafka::Message> msg{consumer->consume(100 /*ms*/)};
        if (!msg) continue;

        switch (msg->err()) {
            case RdKafka::ERR__TIMED_OUT:
                break;
            case RdKafka::ERR_NO_ERROR: {
                velocity::telemetry::v1::OrderEvent event;
                if (!event.ParseFromArray(msg->payload(), static_cast<int>(msg->len()))) {
                    VLOG_WARN("dropped malformed OrderEvent on partition {} offset {}",
                              msg->partition(), msg->offset());
                    break;
                }

                const auto& submission_id = event.submission_id().value();
                auto& s = state[submission_id];
                if (s.window_start_ns == 0) s.window_start_ns = velocity::time::realtime_ns();

                using EK = velocity::telemetry::v1::EventKind;
                using OC = velocity::telemetry::v1::Outcome;
                const auto cid = event.correlation_id().low();
                // The completion (ack) event carries the submission's
                // reported outcome/fills; the intent carries the order.
                const bool is_completion = event.ack_received_ts_ns() > 0;

                if (!is_completion) {
                    // ---- INTENT: replay the order through the reference
                    //      book exactly once, in send order. Stash the
                    //      reference fills until the completion arrives.
                    Order ord{
                        .id       = cid,
                        .price    = static_cast<std::int64_t>(event.price().units()),
                        .quantity = static_cast<std::uint64_t>(event.quantity().units()),
                        .ts_ns    = static_cast<std::int64_t>(event.sent_ts_ns()),
                        .side     = map_side(event.side()),
                        .type     = map_type(event.order_type()),
                        .tif      = map_tif(event.order_type()),
                    };
                    if (event.event_kind() == EK::EVENT_KIND_CANCEL) {
                        (void)s.book.cancel(ord.id);
                    } else {
                        // Decision mid for execution-quality is the
                        // pre-trade book midpoint. Compute it BEFORE we
                        // mutate the book with submit().
                        const auto bb = s.book.best_bid();
                        const auto ba = s.book.best_ask();
                        const std::int64_t decision_mid =
                            (bb && ba) ? (*bb + *ba) / 2 : 0;
                        s.exec_quality.on_order_arrival(ord.id, decision_mid,
                                                        ord.ts_ns);

                        auto r = s.book.submit(ord);
                        if (s.pending.size() < kMaxPending) {
                            s.pending.emplace(cid, PendingOrder{
                                std::move(r.fills), ord.side, ord.price,
                                ord.quantity, ord.tif, ord.ts_ns,
                                velocity::time::realtime_ns()});
                        }
                    }
                } else {
                    // ---- COMPLETION: reconcile our reference fills against
                    //      the submission's REPORTED outcome/fills.
                    auto it = s.pending.find(cid);
                    if (it != s.pending.end()) {
                        auto& po = it->second;
                        const auto mismatch = reconcile(s, po.reference_fills, event);
                        push_mismatch(redis.get(), submission_id, mismatch,
                                      event, po.reference_fills, cfg.mismatches_cap);

                        // Execution quality uses the submission's ACTUAL
                        // reported fill, not the reference book's.
                        if (event.outcome() == OC::OUTCOME_FILLED ||
                            event.outcome() == OC::OUTCOME_PARTIAL_FILL) {
                            s.exec_quality.on_fill(
                                cid,
                                static_cast<std::int64_t>(event.fill_price().units()),
                                static_cast<std::uint64_t>(event.fill_quantity().units()),
                                po.ts_ns);
                        }

                        // Microstructure check: the claimed fills are the
                        // submission's REPORTED fills — feeding the reference
                        // book's own fills back here (the previous behaviour)
                        // compared the gold book against itself and could
                        // never flag a violation. NOTE: the wire OrderEvent
                        // only carries the aggregate reported fill, and the
                        // gold book is advanced in completion order rather
                        // than strict send order; per-maker fills and exact
                        // ordering arrive with OrderEvent v2 (see
                        // proto/telemetry.proto).
                        MicroOrderEvent mev;
                        mev.id         = cid;
                        mev.account_id = 0;  // pre-v2 events have no account_id
                        mev.side       = po.side == Side::BUY
                                            ? MicroSide::BUY : MicroSide::SELL;
                        mev.tif        = po.tif == TimeInForce::IOC ? MicroTif::IOC
                                        : po.tif == TimeInForce::FOK ? MicroTif::FOK
                                        : MicroTif::GTC;
                        mev.price      = po.price;
                        mev.quantity   = po.quantity;
                        mev.ts_ns      = po.ts_ns;
                        mev.claimed_rejected = event.outcome() == OC::OUTCOME_REJECTED;
                        if (event.outcome() == OC::OUTCOME_FILLED ||
                            event.outcome() == OC::OUTCOME_PARTIAL_FILL) {
                            mev.claimed_fills.push_back({
                                /*maker_id=*/0,
                                static_cast<std::int64_t>(event.fill_price().units()),
                                static_cast<std::uint64_t>(event.fill_quantity().units()),
                                po.ts_ns});
                        }
                        auto violations = s.micro.process(mev);
                        s.micro_violations += violations.size();

                        s.pending.erase(it);
                    }
                    // A completion with no matching intent (intent dropped)
                    // is ignored — we cannot reconstruct the order, and
                    // counting it would fabricate a phantom fill.
                }

                // Flush a report when our 1-second window elapses for this submission.
                const auto now = velocity::time::monotonic_ns();
                if (now - s.last_report_ns >= kReportEveryNs) {
                    s.last_report_ns = now;
                    // Sweep orders whose completion never came before we
                    // snapshot the counters, so timeouts count as missing.
                    evict_stale_pending(s, velocity::time::realtime_ns());
                    emit_report(submission_id, s, *producer, cfg.fills_topic);
                }
                // 10 Hz orderbook snapshot for the replay viewer. We
                // intentionally don't tie the cadence to the report
                // window — the viewer wants finer granularity than the
                // scoring path's 1 Hz emit.
                constexpr std::int64_t kSnapshotEveryNs = 100'000'000;  // 100 ms
                if (now - s.last_snapshot_ns >= kSnapshotEveryNs) {
                    s.last_snapshot_ns = now;
                    const auto elapsed_ms =
                        s.window_start_ns == 0 ? std::int64_t{0}
                            : (velocity::time::realtime_ns() - s.window_start_ns)
                                / 1'000'000;
                    push_orderbook_snapshot(redis.get(), submission_id, s, elapsed_ms);

                    // Tick the exec-quality tracker against the *current*
                    // mid (after this order resolved) — that's the
                    // post-trade mark for the reversion window.
                    const auto post_bb = s.book.best_bid();
                    const auto post_ba = s.book.best_ask();
                    const std::int64_t post_mid =
                        (post_bb && post_ba) ? (*post_bb + *post_ba) / 2 : 0;
                    s.exec_quality.tick(velocity::time::realtime_ns(), post_mid);

                    if (redis) {
                        try {
                            const auto eq = s.exec_quality.snapshot();
                            nlohmann::json j{
                                {"orders_observed", eq.orders_observed},
                                {"fully_marked",    eq.fully_marked},
                                {"slippage_mean_bps",  eq.slippage_mean_bps},
                                {"slippage_p50_bps",   eq.slippage_p50_bps},
                                {"slippage_p95_bps",   eq.slippage_p95_bps},
                                {"is_mean_bps",        eq.is_mean_bps},
                                {"is_p50_bps",         eq.is_p50_bps},
                                {"is_p95_bps",         eq.is_p95_bps},
                                {"reversion_mean_bps", eq.reversion_mean_bps},
                                {"reversion_p50_bps",  eq.reversion_p50_bps},
                                {"reversion_p95_bps",  eq.reversion_p95_bps},
                            };
                            const auto k = "exec_quality:" + submission_id;
                            redis->set(k, j.dump());
                            redis->expire(k, 30 * 60);
                        } catch (...) { /* best-effort */ }
                    }
                }
                break;
            }
            case RdKafka::ERR__PARTITION_EOF:
                // Normal — no new messages.
                break;
            default:
                VLOG_ERROR("consume error: {}", msg->errstr());
                break;
        }

        // Drive producer callbacks.
        producer->poll(0);

        // Asynchronously commit the consumer position roughly every 2s
        // worth of polling iterations (consume returns every 100 ms, so
        // every 20 iterations). Async commit avoids blocking the hot
        // path; failure is logged once per minute at most.
        ++commit_tick;
        if (commit_tick >= 20) {
            commit_tick = 0;
            const auto err = consumer->commitAsync();
            if (err != RdKafka::ERR_NO_ERROR) {
                static std::atomic<int> warn{0};
                if ((warn++ % 30) == 0) {
                    VLOG_WARN("validator: commitAsync failed: {}",
                              RdKafka::err2str(err));
                }
            }
        }
    }

    VLOG_INFO("validator draining...");
    // Make sure the offsets we did handle are durably committed before
    // we tear down the consumer; otherwise the next restart will replay
    // them and double-count.
    (void)consumer->commitSync();
    producer->flush(5000);
    consumer->close();
}

}  // namespace velocity::correctness_validator
