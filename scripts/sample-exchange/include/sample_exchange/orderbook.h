// =============================================================================
//  sample_exchange/orderbook.h
//
//  A minimal, readable matching engine. Mirrors the ReferenceOrderbook in
//  the validator but uses std::map<>+std::list<> instead of intrusive
//  Boost containers so the source is comprehensible in 10 minutes.
//
//  This is the *baseline* — any production-targeted submission should be
//  able to beat this comfortably.
// =============================================================================

#pragma once

#include <cstdint>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace velocity::sample_exchange {

enum class Side : std::uint8_t { BUY = 0, SELL = 1 };

struct Order {
    std::string   id;          // 26-char ULID
    Side          side;
    std::int64_t  price;       // fixed-point integer (e.g. cents)
    std::uint64_t quantity;
    std::int64_t  ts_ns;       // arrival timestamp (server-side)
};

struct Fill {
    std::string   taker_id;
    std::string   maker_id;
    std::int64_t  price;
    std::uint64_t quantity;
    Side          aggressor_side;
    std::int64_t  ts_ns;
};

struct SubmitOutcome {
    std::vector<Fill> fills;
    std::uint64_t     resting_quantity{0};
    bool              accepted{true};
    std::string       reject_reason;
};

// Thread-safe matching engine. Locking is coarse (one mutex for the whole
// book). Trade-off chosen intentionally — this is a clarity-first reference.
class OrderBook {
public:
    OrderBook();

    [[nodiscard]] auto submit(Order order)                -> SubmitOutcome;
    [[nodiscard]] auto cancel(const std::string& id)      -> bool;

    [[nodiscard]] auto best_bid() const -> std::optional<std::int64_t>;
    [[nodiscard]] auto best_ask() const -> std::optional<std::int64_t>;

    struct DepthLevel {
        std::int64_t  price;
        std::uint64_t quantity;
        std::uint32_t order_count;
    };

    [[nodiscard]] auto snapshot(std::size_t levels = 10) const
        -> std::pair<std::vector<DepthLevel>, std::vector<DepthLevel>>;

private:
    using FifoQueue = std::list<Order>;

    // Bids descending, asks ascending.
    std::map<std::int64_t, FifoQueue, std::greater<>> bids_;
    std::map<std::int64_t, FifoQueue, std::less<>>    asks_;

    // id -> (price, side, iterator into the FIFO for O(1) cancel)
    struct Locator {
        Side               side;
        std::int64_t       price;
        FifoQueue::iterator it;
    };
    std::unordered_map<std::string, Locator> by_id_;

    mutable std::mutex mu_;

    template <typename Side1, typename Side2>
    auto match_(Order& taker, Side2& opposite, std::vector<Fill>& fills) -> void;
};

}  // namespace velocity::sample_exchange
