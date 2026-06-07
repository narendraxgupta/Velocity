// Deterministic Instrument Order Book aggregate used by the replay engine.
// This is a compact, well-documented skeleton. The real implementation
// must avoid floating point and use integer ticks for price, and be
// fully deterministic across platforms.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace velocity {

struct OrderPlacedEvent {
    std::string order_id;
    std::string submission_id;
    std::string instrument;
    bool is_buy;
    int64_t price_ticks;
    int64_t quantity;
    int64_t intended_send_ns;
};

struct Trade {
    std::string trade_id;
    std::string maker_order_id;
    std::string taker_order_id;
    int64_t price_ticks;
    int64_t quantity;
};

// Minimal deterministic orderbook interface. The concrete implementation
// must be single-threaded for a single aggregate replay and use stable tie-
// breakers (arrival sequence then order_id) to ensure determinism.
class InstrumentOrderBook {
public:
    InstrumentOrderBook(const std::string& instrument) : instrument_(instrument) {}

    ~InstrumentOrderBook() = default;

    // Apply an OrderPlaced event and return any trades produced by matching.
    std::vector<Trade> ApplyOrder(const OrderPlacedEvent& ev);

    // Cancel or modify an order (stubs for now).
    bool CancelOrder(const std::string& order_id);
    bool ModifyOrder(const std::string& order_id, int64_t new_price_ticks, int64_t new_qty);

    // Snapshot / restore helpers (serialization left for later).
    std::string Snapshot() const;
    void RestoreFromSnapshot(const std::string& blob);

private:
    std::string instrument_;

    // Deterministic internal state:
    struct InternalOrder {
        std::string order_id;
        int64_t remaining_qty;
        int64_t price_ticks;
        uint64_t arrival_seq;
    };

    // price -> queue of orders at that price. For bids: highest price wins.
    std::map<int64_t, std::deque<InternalOrder>> bids_;  // key = price_ticks
    std::map<int64_t, std::deque<InternalOrder>> asks_;
    uint64_t next_seq_ = 1;  // deterministic incrementing arrival sequence
};

}  // namespace velocity
