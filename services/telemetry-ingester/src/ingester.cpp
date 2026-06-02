// =============================================================================
//  ingester.cpp — the consume / aggregate / fan-out loop.
//
//  Flow per message
//  ----------------
//  The bot-worker emits TWO OrderEvents per order that share a correlation
//  id: an *intent* (carries intended_ts_ns + sent_ts_ns, ack==0) pushed at
//  send time, and a *completion* (carries ack_received_ts_ns + fills,
//  intended==0) pushed from the transport ack callback. Honest, coordinated-
//  omission-corrected latency is `ack_received_ts_ns - intended_ts_ns`, so it
//  can only be computed by pairing the two. The publisher partitions by
//  submission_id, so both halves are guaranteed to reach THIS instance on the
//  same partition, intent before completion.
//
//    1. Parse OrderEvent.
//    2. Intent  → stash {intended,sent,price,qty} keyed by correlation id.
//       Completion → join with the stashed intent, latency = ack - intended.
//       Rejected intents (no completion will follow) are recorded inline.
//    3. Record latency into the per-submission HdrHistogram.
//    4. Append a row to the QuestDB ILP buffer.
//    5. Every `flush_interval_ms`: flush QuestDB; serialize each histogram
//       into a LatencyBucket, publish to `metrics.latency.1s`; evict pending
//       intents older than the join TTL as timeouts so memory stays bounded.
// =============================================================================

#include "telemetry_ingester/ingester.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>

#include <hdr_histogram.h>
#include <hdr_histogram_log.h>
#include <librdkafka/rdkafkacpp.h>

#include "common.pb.h"
#include "telemetry.pb.h"

#include "telemetry_ingester/histograms.h"
#include "telemetry_ingester/questdb_writer.h"

#include "velocity/common/log.h"
#include "velocity/common/signals.h"
#include "velocity/common/time.h"

namespace velocity::telemetry_ingester {

namespace {

[[nodiscard]] auto encode_hg(hdr_histogram* h, std::string& out) -> bool {
    char* encoded = nullptr;
    const auto rc = hdr_log_encode(h, &encoded);
    if (rc != 0 || encoded == nullptr) return false;
    out.assign(encoded);
    ::free(encoded);
    return true;
}

[[nodiscard]] auto value_at(hdr_histogram* h, double pct) noexcept -> std::uint64_t {
    return static_cast<std::uint64_t>(hdr_value_at_percentile(h, pct));
}

}  // namespace

// An intent awaiting its completion event. Captured from the first
// (intent) OrderEvent and joined when the matching completion arrives.
struct PendingIntent {
    std::string  submission_id;
    std::int64_t intended_ts_ns;
    std::int64_t sent_ts_ns;
    std::int64_t price_units;
    std::int64_t qty_units;
    std::int64_t arrived_at_ns;   // ingester realtime clock — for TTL eviction
};

// Orders whose completion never arrives (drops, submission hangs) are
// evicted after this long so the pending map can't grow without bound.
constexpr std::int64_t kJoinTtlNs = 30LL * 1'000'000'000;   // 30s
// Hard ceiling as a second line of defence against a pathological run.
constexpr std::size_t  kMaxPending = 5'000'000;

struct Ingester::Impl {
    IngesterConfig                          cfg;
    std::unique_ptr<RdKafka::KafkaConsumer> consumer;
    std::unique_ptr<RdKafka::Producer>      producer;
    std::unique_ptr<QuestDbWriter>          questdb;
    HistogramStore                          store;
    std::int64_t                            window_start_ns{0};
    // Correlation-id (low 64 bits) → intent awaiting its completion.
    std::unordered_map<std::uint64_t, PendingIntent> pending;

    explicit Impl(IngesterConfig c) : cfg(std::move(c)) {
        std::string errstr;

        std::unique_ptr<RdKafka::Conf> cc{RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL)};
        cc->set("bootstrap.servers",       cfg.brokers,  errstr);
        cc->set("group.id",                cfg.group_id, errstr);
        cc->set("enable.auto.commit",      "true",       errstr);
        cc->set("auto.commit.interval.ms", "1000",       errstr);
        cc->set("fetch.min.bytes",         "131072",     errstr);
        cc->set("fetch.wait.max.ms",       "50",         errstr);
        cc->set("auto.offset.reset",       "latest",     errstr);
        consumer.reset(RdKafka::KafkaConsumer::create(cc.get(), errstr));
        if (!consumer) throw std::runtime_error("consumer failed: " + errstr);
        consumer->subscribe({cfg.telemetry_topic});

        std::unique_ptr<RdKafka::Conf> pc{RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL)};
        pc->set("bootstrap.servers", cfg.brokers,  errstr);
        pc->set("linger.ms",         "5",          errstr);
        pc->set("acks",              "1",          errstr);
        pc->set("compression.type",  "lz4",        errstr);
        producer.reset(RdKafka::Producer::create(pc.get(), errstr));
        if (!producer) throw std::runtime_error("producer failed: " + errstr);

        questdb = std::make_unique<QuestDbWriter>(cfg.questdb_host, cfg.questdb_ilp_port);
        window_start_ns = velocity::time::realtime_ns();
    }

    auto run() -> void {
        const auto flush_iv = std::chrono::milliseconds(cfg.flush_interval_ms);
        auto next_flush_at  = std::chrono::steady_clock::now() + flush_iv;

        VLOG_INFO("ingester consuming '{}' -> QuestDB udp://{}:{} & '{}'",
                  cfg.telemetry_topic, cfg.questdb_host, cfg.questdb_ilp_port,
                  cfg.latency_topic);

        while (!velocity::signals::shutdown_requested()) {
            std::unique_ptr<RdKafka::Message> msg{consumer->consume(50)};
            if (msg && msg->err() == RdKafka::ERR_NO_ERROR) {
                handle_event_(*msg);
            } else if (msg && msg->err() != RdKafka::ERR__TIMED_OUT &&
                       msg->err() != RdKafka::ERR__PARTITION_EOF) {
                VLOG_WARN("consume error: {}", msg->errstr());
            }

            if (std::chrono::steady_clock::now() >= next_flush_at) {
                flush_();
                next_flush_at = std::chrono::steady_clock::now() + flush_iv;
            }
        }

        VLOG_INFO("ingester draining");
        flush_();
        producer->flush(5000);
        consumer->close();
    }

    auto handle_event_(RdKafka::Message& msg) -> void {
        velocity::telemetry::v1::OrderEvent ev;
        if (!ev.ParseFromArray(msg.payload(), static_cast<int>(msg.len()))) return;

        const auto& sid      = ev.submission_id().value();
        const auto  key      = ev.correlation_id().low();
        const auto  intended = static_cast<std::int64_t>(ev.intended_ts_ns());
        const auto  ack      = static_cast<std::int64_t>(ev.ack_received_ts_ns());
        const auto  outcome  = static_cast<std::int32_t>(ev.outcome());

        // --- Completion event: ack_received_ts_ns set, intended absent. ---
        if (ack > 0) {
            auto it = pending.find(key);
            if (it != pending.end()) {
                const auto& p = it->second;
                std::int64_t latency = 0;
                // Include same-nanosecond completions (ack == intended): a
                // strict `>` dropped those samples entirely, biasing very
                // fast paths' percentiles. Floor the recorded value at 1ns.
                if (p.intended_ts_ns > 0 && ack >= p.intended_ts_ns) {
                    latency = ack - p.intended_ts_ns;
                    if (latency < 1) latency = 1;
                    store.record(p.submission_id, latency);
                }
                // The completion carries only the final outcome/fills; the
                // request fields (price/qty/sent) come from the stashed intent.
                questdb->append_order_event(
                    p.submission_id, latency,
                    p.price_units, p.qty_units, outcome, p.sent_ts_ns);
                pending.erase(it);
            } else {
                // Completion with no matching intent (intent dropped or evicted).
                // Record what we can; latency is unknowable without the intent.
                questdb->append_order_event(
                    sid, 0, ev.price().units(), ev.quantity().units(),
                    outcome, static_cast<std::int64_t>(ev.sent_ts_ns()));
            }
            return;
        }

        // --- Intent event: intended set, ack still 0. ---
        if (intended > 0) {
            // A rejected send never produces a completion — record inline so
            // it still lands as an errored order, and don't stash it.
            if (outcome == static_cast<std::int32_t>(
                    velocity::telemetry::v1::Outcome::OUTCOME_REJECTED)) {
                questdb->append_order_event(
                    sid, 0, ev.price().units(), ev.quantity().units(),
                    outcome, static_cast<std::int64_t>(ev.sent_ts_ns()));
                return;
            }
            if (pending.size() < kMaxPending) {
                pending.emplace(key, PendingIntent{
                    sid, intended,
                    static_cast<std::int64_t>(ev.sent_ts_ns()),
                    ev.price().units(), ev.quantity().units(),
                    velocity::time::realtime_ns()});
            }
            return;
        }

        // Neither intent nor completion (malformed/legacy) — record raw, no latency.
        questdb->append_order_event(
            sid, 0, ev.price().units(), ev.quantity().units(),
            outcome, static_cast<std::int64_t>(ev.sent_ts_ns()));
    }

    // Evict pending intents whose completion never arrived. They are written
    // as timeout rows (no latency) so they still count as errored orders.
    auto evict_stale_pending_(std::int64_t now_ns) -> void {
        for (auto it = pending.begin(); it != pending.end();) {
            if (now_ns - it->second.arrived_at_ns > kJoinTtlNs) {
                questdb->append_order_event(
                    it->second.submission_id, 0,
                    it->second.price_units, it->second.qty_units,
                    static_cast<std::int32_t>(
                        velocity::telemetry::v1::Outcome::OUTCOME_TIMEOUT),
                    it->second.sent_ts_ns);
                it = pending.erase(it);
            } else {
                ++it;
            }
        }
    }

    auto flush_() -> void {
        const auto end_ns = velocity::time::realtime_ns();
        evict_stale_pending_(end_ns);
        questdb->flush();

        store.drain_for_each([&](const std::string& sid, hdr_histogram* h) {
            velocity::telemetry::v1::LatencyBucket bucket;
            bucket.mutable_submission_id()->set_value(sid);
            bucket.set_window_start_ns(window_start_ns);
            bucket.set_window_end_ns(end_ns);
            bucket.set_count(static_cast<std::uint64_t>(h->total_count));
            bucket.set_p50_ns (value_at(h, 50.0));
            bucket.set_p90_ns (value_at(h, 90.0));
            bucket.set_p99_ns (value_at(h, 99.0));
            bucket.set_p999_ns(value_at(h, 99.9));
            bucket.set_max_ns (static_cast<std::uint64_t>(hdr_max(h)));

            std::string encoded;
            if (encode_hg(h, encoded)) bucket.set_hdr_histogram(encoded);

            std::string payload;
            bucket.SerializeToString(&payload);
            producer->produce(
                cfg.latency_topic, RdKafka::Topic::PARTITION_UA,
                RdKafka::Producer::RK_MSG_COPY,
                payload.data(), payload.size(),
                sid.data(), sid.size(),
                0, nullptr, nullptr);
        });
        producer->poll(0);
        window_start_ns = end_ns;
    }
};

Ingester::Ingester(IngesterConfig cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}
Ingester::~Ingester() = default;
auto Ingester::run() -> void { impl_->run(); }

}  // namespace velocity::telemetry_ingester
