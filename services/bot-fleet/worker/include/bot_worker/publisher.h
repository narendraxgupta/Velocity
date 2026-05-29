// =============================================================================
//  bot_worker/publisher.h
//
//  Drains per-reactor SPSC ring buffers and forwards events to Redpanda as
//  protobuf-encoded OrderEvent messages. One Publisher per worker process.
// =============================================================================

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <boost/lockfree/spsc_queue.hpp>

#include "bot_worker/event.h"

namespace RdKafka { class Producer; }

namespace velocity::bot_worker {
class PublisherDrCb;
}

namespace velocity::bot_worker {

// Runtime-sized SPSC queue. Capacity is fixed at construction time but
// chosen at runtime by the worker so deployments can dial it for the
// memory budget of the host.
using SpscQueue = boost::lockfree::spsc_queue<Event>;

class Publisher {
public:
    Publisher(std::string brokers,
              std::string topic,
              std::size_t reactor_count,
              std::size_t queue_capacity = 65'536);
    ~Publisher();

    Publisher(const Publisher&)            = delete;
    Publisher& operator=(const Publisher&) = delete;

    // Get the ring for the given reactor — the reactor pushes into it.
    [[nodiscard]] auto queue_for(std::size_t reactor_id) -> SpscQueue&;

    // Counters surfaced through Prometheus.
    [[nodiscard]] auto published() const noexcept -> std::uint64_t;
    [[nodiscard]] auto dropped()   const noexcept -> std::uint64_t;

    auto stop() -> void;

private:
    auto drain_loop_(std::size_t reactor_id) -> void;

    std::string                              brokers_;
    std::string                              topic_;
    std::vector<std::unique_ptr<SpscQueue>>  queues_;
    std::vector<std::thread>                 drainers_;
    std::unique_ptr<RdKafka::Producer>       producer_;
    std::unique_ptr<PublisherDrCb>           dr_cb_;
    std::atomic<bool>                        stop_{false};
    std::atomic<std::uint64_t>               published_{0};
    std::atomic<std::uint64_t>               dropped_{0};
};

}  // namespace velocity::bot_worker
