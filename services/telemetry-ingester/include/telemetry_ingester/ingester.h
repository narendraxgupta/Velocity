// =============================================================================
//  telemetry_ingester/ingester.h — public API.
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace velocity::telemetry_ingester {

struct IngesterConfig {
    std::string   brokers;
    std::string   group_id;
    std::string   telemetry_topic;   // "telemetry.raw"
    std::string   latency_topic;     // "metrics.latency.1s"
    std::string   questdb_host;
    std::uint16_t questdb_ilp_port;
    std::string   redis_addr;
    std::uint16_t metrics_port;
    std::uint32_t flush_interval_ms;
};

class Ingester {
public:
    explicit Ingester(IngesterConfig cfg);
    ~Ingester();

    Ingester(const Ingester&)            = delete;
    Ingester& operator=(const Ingester&) = delete;

    // Blocks until shutdown_requested().
    auto run() -> void;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace velocity::telemetry_ingester
