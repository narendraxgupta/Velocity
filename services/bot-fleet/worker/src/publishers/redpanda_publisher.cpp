// =============================================================================
//  redpanda_publisher.cpp — drains per-reactor SPSC ring buffers into a
//  single librdkafka producer.
//
//  Threading
//  ---------
//  One drainer thread per reactor. Each drainer owns its SPSC's consumer
//  side. The librdkafka producer is shared (its produce() is thread-safe
//  and lock-free under the hood). A separate poll thread drives delivery
//  callbacks.
// =============================================================================

#include "bot_worker/publisher.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

#include <librdkafka/rdkafkacpp.h>

#include "common.pb.h"
#include "telemetry.pb.h"
#include "velocity/common/log.h"

namespace velocity::bot_worker {

namespace {

[[nodiscard]] auto map_outcome(Outcome o) noexcept -> velocity::telemetry::v1::Outcome {
    using O = velocity::telemetry::v1::Outcome;
    switch (o) {
        case Outcome::ACK:       return O::OUTCOME_ACKNOWLEDGED;
        case Outcome::REJECT:    return O::OUTCOME_REJECTED;
        case Outcome::FILLED:    return O::OUTCOME_FILLED;
        case Outcome::PARTIAL:   return O::OUTCOME_PARTIAL_FILL;
        case Outcome::CANCELLED: return O::OUTCOME_CANCELLED;
        case Outcome::TIMEOUT:   return O::OUTCOME_TIMEOUT;
        default:                 return O::OUTCOME_UNSPECIFIED;
    }
}

[[nodiscard]] auto map_kind(Kind k) noexcept -> velocity::telemetry::v1::EventKind {
    using K = velocity::telemetry::v1::EventKind;
    switch (k) {
        case Kind::NEW:    return K::EVENT_KIND_NEW;
        case Kind::CANCEL: return K::EVENT_KIND_CANCEL;
        case Kind::MODIFY: return K::EVENT_KIND_MODIFY;
        default:           return K::EVENT_KIND_UNSPECIFIED;
    }
}

// Helper that fails the constructor on any librdkafka config error rather
// than silently dropping the option.
auto set_conf(RdKafka::Conf& conf, const std::string& key, const std::string& value) -> void {
    std::string errstr;
    if (conf.set(key, value, errstr) != RdKafka::Conf::CONF_OK) {
        throw std::runtime_error("kafka conf '" + key + "' rejected: " + errstr);
    }
}

}  // namespace

// The delivery-report callback lives as a member of Publisher so that
// destruction of the Publisher cleanly tears the callback down too,
// making `dropped_` safe to reference for the lifetime of every
// in-flight delivery report.
class PublisherDrCb : public RdKafka::DeliveryReportCb {
public:
    explicit PublisherDrCb(std::atomic<std::uint64_t>* dropped) : dropped_(dropped) {}
    auto dr_cb(RdKafka::Message& msg) -> void override {
        if (msg.err() != RdKafka::ERR_NO_ERROR) {
            dropped_->fetch_add(1, std::memory_order_relaxed);
        }
    }
private:
    std::atomic<std::uint64_t>* dropped_;
};

Publisher::Publisher(std::string brokers, std::string topic,
                     std::size_t reactor_count, std::size_t queue_capacity)
    : brokers_(std::move(brokers)),
      topic_(std::move(topic)),
      dr_cb_(std::make_unique<PublisherDrCb>(&dropped_)) {

    std::unique_ptr<RdKafka::Conf> conf{RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL)};
    set_conf(*conf, "bootstrap.servers",            brokers_);
    set_conf(*conf, "linger.ms",                    "1");         // latency over throughput
    set_conf(*conf, "batch.size",                   "65536");
    set_conf(*conf, "acks",                         "1");
    set_conf(*conf, "compression.type",             "lz4");
    set_conf(*conf, "queue.buffering.max.messages", "1000000");
    set_conf(*conf, "enable.idempotence",           "false");     // at-least-once is fine

    std::string errstr;
    if (conf->set("dr_cb", dr_cb_.get(), errstr) != RdKafka::Conf::CONF_OK) {
        throw std::runtime_error("kafka dr_cb registration failed: " + errstr);
    }

    producer_.reset(RdKafka::Producer::create(conf.get(), errstr));
    if (!producer_) {
        throw std::runtime_error("failed to create Kafka producer: " + errstr);
    }

    // Boost requires capacities >= 2 to satisfy the SPSC invariant. A
    // small floor protects against misconfiguration without surprising
    // any operator who set the environment variable explicitly.
    const auto cap = std::max<std::size_t>(2, queue_capacity);
    queues_.reserve(reactor_count);
    for (std::size_t i = 0; i < reactor_count; ++i) {
        queues_.push_back(std::make_unique<SpscQueue>(cap));
        drainers_.emplace_back(&Publisher::drain_loop_, this, i);
    }
}

Publisher::~Publisher() {
    stop();
    for (auto& t : drainers_) if (t.joinable()) t.join();
    if (producer_) producer_->flush(2000);
}

auto Publisher::queue_for(std::size_t reactor_id) -> SpscQueue& {
    return *queues_.at(reactor_id);
}

auto Publisher::published() const noexcept -> std::uint64_t { return published_.load(std::memory_order_relaxed); }
auto Publisher::dropped()   const noexcept -> std::uint64_t { return dropped_.load(std::memory_order_relaxed); }

auto Publisher::stop() -> void { stop_.store(true, std::memory_order_release); }

auto Publisher::drain_loop_(std::size_t reactor_id) -> void {
    auto& q = *queues_[reactor_id];
    velocity::telemetry::v1::OrderEvent wire;
    std::string buf;
    buf.reserve(256);

    constexpr std::size_t kBatch = 64;
    Event batch[kBatch];

    while (!stop_.load(std::memory_order_acquire)) {
        const auto n = q.pop(batch, kBatch);
        if (n == 0) {
            producer_->poll(1);
            continue;
        }
        for (std::size_t i = 0; i < n; ++i) {
            const auto& ev = batch[i];

            wire.Clear();
            wire.mutable_submission_id()->set_value(
                std::string{ev.submission_id.data(), ev.submission_id.size()});
            // Correlation id: pack {sent_ts_ns, reactor-scoped id} into
            // the 128-bit field. Note we cast through uint64_t so a
            // negative monotonic-relative timestamp survives bit-for-bit.
            auto* cid = wire.mutable_correlation_id();
            cid->set_high(static_cast<std::uint64_t>(ev.sent_ts_ns));
            cid->set_low(ev.correlation_id);
            wire.set_bot_id(ev.bot_id);
            wire.set_worker_id(static_cast<std::uint32_t>(ev.reactor_id));
            wire.set_intended_ts_ns(ev.intended_ts_ns);
            wire.set_sent_ts_ns(ev.sent_ts_ns);
            wire.set_ack_received_ts_ns(ev.ack_ts_ns);
            wire.set_side(ev.side == Side::BUY
                              ? velocity::common::v1::Side::SIDE_BUY
                              : velocity::common::v1::Side::SIDE_SELL);
            wire.set_order_type(velocity::common::v1::OrderType::ORDER_TYPE_LIMIT);
            wire.set_event_kind(map_kind(ev.kind));
            wire.set_outcome(map_outcome(ev.outcome));

            auto* px = wire.mutable_price();
            px->set_units(ev.price);
            px->set_scale(2);
            auto* qty = wire.mutable_quantity();
            qty->set_units(static_cast<std::int64_t>(ev.quantity));
            qty->set_scale(0);
            auto* fp = wire.mutable_fill_price();
            fp->set_units(ev.fill_price);
            fp->set_scale(2);
            auto* fq = wire.mutable_fill_quantity();
            fq->set_units(static_cast<std::int64_t>(ev.fill_quantity));
            fq->set_scale(0);

            buf.clear();
            wire.SerializeToString(&buf);

            // Partition by submission_id so per-submission ordering is preserved.
            const std::string key{ev.submission_id.data(), ev.submission_id.size()};
            const auto err = producer_->produce(
                topic_,
                RdKafka::Topic::PARTITION_UA,
                RdKafka::Producer::RK_MSG_COPY,
                buf.data(), buf.size(),
                key.data(), key.size(),
                // timestamp=0 → librdkafka stamps current wall-clock ms.
                // ev.sent_ts_ns is CLOCK_MONOTONIC_RAW (not Unix epoch), so it
                // must NOT be used as the broker message timestamp.
                0,
                nullptr, nullptr);
            if (err == RdKafka::ERR_NO_ERROR) {
                published_.fetch_add(1, std::memory_order_relaxed);
            } else if (err == RdKafka::ERR__QUEUE_FULL) {
                // Back-pressure: caller queue is full, give the producer a
                // chance to flush, then try once more.
                producer_->poll(5);
                const auto retry = producer_->produce(
                    topic_, RdKafka::Topic::PARTITION_UA,
                    RdKafka::Producer::RK_MSG_COPY,
                    buf.data(), buf.size(),
                    key.data(), key.size(),
                    0, nullptr, nullptr);
                if (retry != RdKafka::ERR_NO_ERROR) {
                    dropped_.fetch_add(1, std::memory_order_relaxed);
                } else {
                    published_.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                dropped_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        producer_->poll(0);
    }
}

}  // namespace velocity::bot_worker
