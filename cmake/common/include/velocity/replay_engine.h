// Replay engine skeleton: replays events for a benchmark and drives the
// correctness validator. This header defines the high-level interface.
#pragma once

#include "velocity/orderbook.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace velocity {

class ReplayEngine {
public:
    // Handler is called for validation violations; it can record or abort.
    using ViolationHandler = std::function<void(const std::string& violation_json)>;

    ReplayEngine();
    virtual ~ReplayEngine();

    // Replay all events for a benchmark_id and return true if validation passed.
    bool ReplayBenchmark(const std::string& benchmark_id, ViolationHandler handler);

protected:
    // LoadBenchmarkEvents returns the ordered event stream for a benchmark.
    // Override in tests or production implementations that use an external event store.
    virtual std::vector<OrderPlacedEvent> LoadBenchmarkEvents(const std::string& benchmark_id);
};

}  // namespace velocity
