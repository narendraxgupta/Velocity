// =============================================================================
//  correctness_validator/reference_orderbook.h
//
//  Reference price-time priority orderbook. This is the gold standard
//  against which every submission is compared.
//
//  Algorithmic guarantees
//  ----------------------
//    submit():        O(log P + F)   P = distinct price levels touched
//                                    F = fills produced by this order
//    cancel():        O(1)           given the order id
//    best_bid/ask():  O(log P)
//    bid/ask_depth(): O(1)
//
//  Memory layout
//  -------------
//  Orders live in an `unordered_map<id, Order*>` for O(1) id lookup, and
//  are simultaneously hooked into two intrusive containers:
//
//    * A `set<PriceLevel>` keyed by price (one per side).
//    * A FIFO `list<Order>` inside each PriceLevel.
//
//  Because the hooks are intrusive (Boost.Intrusive), the price-level set
//  and the per-level FIFO share the same Order object — there is one
//  allocation per resting order, period. The book is allocator-friendly and
//  cache-coherent.
//
//  Determinism
//  -----------
//  Given the exact same sequence of submit() / cancel() calls in the same
//  order, the book produces the exact same fills in the exact same order.
//  This is the property the correctness check relies on.
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace velocity::correctness_validator {

enum class Side : std::uint8_t {
    BUY  = 0,
    SELL = 1,
};

enum class TimeInForce : std::uint8_t {
    GTC = 0,   // good-till-cancel (default for LIMIT)
    IOC = 1,   // immediate-or-cancel
    FOK = 2,   // fill-or-kill
};

enum class OrderType : std::uint8_t {
    LIMIT  = 0,
    MARKET = 1,
};

struct Order {
    std::uint64_t id;          // monotonic correlation low-bits
    std::int64_t  price;       // fixed-point; ignored for MARKET
    std::uint64_t quantity;    // remaining quantity
    std::int64_t  ts_ns;       // intended send time, for tie-breaking
    Side          side;
    OrderType     type;
    TimeInForce   tif;
};

struct Fill {
    std::uint64_t taker_id;       // the incoming order's id
    std::uint64_t maker_id;       // the resting order's id
    std::int64_t  price;          // executed at the maker's price
    std::uint64_t quantity;       // quantity traded in this fill
    Side          aggressor_side; // the taker's side
    std::int64_t  ts_ns;          // matched time (taker's ts_ns)
};

// Why we rejected an aggressive order without (full) matching. Used to
// communicate FOK / IOC outcomes.
enum class Reject : std::uint8_t {
    NONE             = 0,
    FOK_UNFILLABLE   = 1,   // FOK could not be filled in full
    NO_OPPOSITE_SIDE = 2,   // MARKET with empty book
    INVALID          = 3,   // malformed input
};

struct SubmitResult {
    std::vector<Fill> fills;
    std::uint64_t     remaining_quantity{0};  // 0 means fully filled or rejected
    Reject            reject{Reject::NONE};
    bool              rested{false};          // true if a remainder was added to the book
};

class ReferenceOrderbook {
public:
    ReferenceOrderbook();
    ~ReferenceOrderbook();

    ReferenceOrderbook(const ReferenceOrderbook&)            = delete;
    ReferenceOrderbook& operator=(const ReferenceOrderbook&) = delete;
    ReferenceOrderbook(ReferenceOrderbook&&) noexcept;
    ReferenceOrderbook& operator=(ReferenceOrderbook&&) noexcept;

    // Submit an order. Returns the matching result.
    [[nodiscard]] auto submit(const Order& order) -> SubmitResult;

    // Cancel by id. Returns true if found and cancelled.
    auto cancel(std::uint64_t order_id) -> bool;

    // Best price queries.
    [[nodiscard]] auto best_bid() const -> std::optional<std::int64_t>;
    [[nodiscard]] auto best_ask() const -> std::optional<std::int64_t>;

    // Total resting quantity on each side.
    [[nodiscard]] auto bid_depth() const noexcept -> std::uint64_t;
    [[nodiscard]] auto ask_depth() const noexcept -> std::uint64_t;

    // L2 snapshot row: one price level with the aggregated quantity at
    // that price. Used by the orderbook replay viewer.
    struct DepthRow {
        std::int64_t  price;
        std::uint64_t quantity;
    };

    // Top-N levels on each side, best-first. `n` is clamped at 50 — that
    // matches the depth-chart in the frontend, and prevents a wide L2
    // snapshot from dominating a per-second snapshot payload.
    [[nodiscard]] auto top_bids(std::size_t n) const -> std::vector<DepthRow>;
    [[nodiscard]] auto top_asks(std::size_t n) const -> std::vector<DepthRow>;

    // Number of distinct resting orders.
    [[nodiscard]] auto resting_order_count() const noexcept -> std::size_t;

    // Reset to an empty book. Releases all allocations.
    auto clear() noexcept -> void;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace velocity::correctness_validator
