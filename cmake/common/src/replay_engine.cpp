#include "velocity/replay_engine.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>

namespace velocity {

ReplayEngine::ReplayEngine() {}

ReplayEngine::~ReplayEngine() {}

static std::string FormatViolation(
    const std::string& benchmark_id, const std::string& order_id, const std::string& message
) {
    nlohmann::json violation;
    violation["benchmark_id"] = benchmark_id;
    violation["order_id"] = order_id;
    violation["violation"] = message;
    return violation.dump();
}

bool ReplayEngine::ReplayBenchmark(const std::string& benchmark_id, ViolationHandler handler) {
    std::vector<OrderPlacedEvent> events = LoadBenchmarkEvents(benchmark_id);
    if (events.empty()) {
        handler(FormatViolation(benchmark_id, "", "no replay events loaded"));
        return false;
    }

    std::map<std::string, InstrumentOrderBook> books;
    bool validation_passed = true;

    for (const auto& ev : events) {
        if (ev.order_id.empty() || ev.submission_id.empty() || ev.instrument.empty()) {
            handler(FormatViolation(benchmark_id, ev.order_id, "invalid event metadata"));
            validation_passed = false;
            continue;
        }
        if (ev.price_ticks <= 0 || ev.quantity <= 0) {
            handler(FormatViolation(benchmark_id, ev.order_id, "invalid price or quantity"));
            validation_passed = false;
            continue;
        }

        auto it = books.find(ev.instrument);
        if (it == books.end()) {
            auto [new_it, inserted] =
                books.emplace(ev.instrument, InstrumentOrderBook(ev.instrument));
            it = new_it;
        }

        const auto trades = it->second.ApplyOrder(ev);
        for (const auto& trade : trades) {
            if (trade.quantity <= 0) {
                handler(
                    FormatViolation(benchmark_id, ev.order_id, "trade quantity must be positive")
                );
                validation_passed = false;
            }
            if (trade.price_ticks <= 0) {
                handler(FormatViolation(benchmark_id, ev.order_id, "trade price must be positive"));
                validation_passed = false;
            }
            if (trade.maker_order_id.empty() || trade.taker_order_id.empty()) {
                handler(
                    FormatViolation(benchmark_id, ev.order_id, "trade participant missing order id")
                );
                validation_passed = false;
            }
        }
    }

    return validation_passed;
}

std::vector<OrderPlacedEvent> ReplayEngine::LoadBenchmarkEvents(const std::string& benchmark_id) {
    const char* env_dir = std::getenv("VELOCITY_REPLAY_EVENTS_DIR");
    std::string events_dir = env_dir ? env_dir : "benchmarks";
    std::string path = events_dir + "/" + benchmark_id + ".json";

    std::ifstream file(path);
    if (!file.is_open()) {
        return {};
    }

    nlohmann::json root;
    try {
        file >> root;
    } catch (const std::exception&) {
        return {};
    }

    if (!root.is_array()) {
        return {};
    }

    std::vector<OrderPlacedEvent> events;
    for (const auto& item : root) {
        OrderPlacedEvent ev;
        ev.order_id = item.value("order_id", "");
        ev.submission_id = item.value("submission_id", "");
        ev.instrument = item.value("instrument", "");
        ev.is_buy = item.value("is_buy", false);
        ev.price_ticks = item.value("price_ticks", int64_t(0));
        ev.quantity = item.value("quantity", int64_t(0));
        ev.intended_send_ns = item.value("intended_send_ns", int64_t(0));
        events.push_back(std::move(ev));
    }
    return events;
}

}  // namespace velocity
