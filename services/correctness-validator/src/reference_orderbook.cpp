// =============================================================================
//  reference_orderbook.cpp — full implementation.
//
//  See the header for the contract. This file is intentionally
//  self-contained: the entire orderbook implementation, including the
//  intrusive nodes, lives here so it's easy to audit in one sitting.
// =============================================================================

#include "correctness_validator/reference_orderbook.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/intrusive/list.hpp>
#include <boost/intrusive/set.hpp>

namespace velocity::correctness_validator {
namespace {

namespace bi = boost::intrusive;

// -----------------------------------------------------------------------------
//  Intrusive node layouts.
//
//  An order participates in two intrusive containers simultaneously:
//    * a doubly-linked FIFO list inside its price level
//    * (transitively, via its level) a price-keyed set of levels
//
//  An order is independently identifiable by its id; we maintain a separate
//  hash map (id -> Node*) so cancel() is O(1).
// -----------------------------------------------------------------------------

struct Node : public bi::list_base_hook<bi::link_mode<bi::auto_unlink>> {
    Order   order{};
    void*   level{nullptr};  // erased pointer to the owning Level; resolved below
};

// Note: with auto_unlink hooks Boost requires constant_time_size<false>.
using NodeList = bi::list<Node, bi::constant_time_size<false>>;

// A price level: a FIFO of resting orders at a single price.
struct Level : public bi::set_base_hook<bi::link_mode<bi::auto_unlink>,
                                        bi::optimize_size<true>> {
    std::int64_t  price{0};
    std::uint64_t total_quantity{0};
    NodeList      fifo;

    explicit Level(std::int64_t p) noexcept : price(p) {}

    // Disable copy/move — Levels live inside an intrusive set; moving them
    // would invalidate the hook.
    Level(const Level&)            = delete;
    Level& operator=(const Level&) = delete;
    Level(Level&&)                 = delete;
    Level& operator=(Level&&)      = delete;
};

// Comparators. The bid side is sorted descending (best = highest), the ask
// side ascending (best = lowest).
struct LevelAsc {
    bool operator()(const Level& a, const Level& b) const noexcept {
        return a.price < b.price;
    }
    bool operator()(std::int64_t price, const Level& b) const noexcept {
        return price < b.price;
    }
    bool operator()(const Level& a, std::int64_t price) const noexcept {
        return a.price < price;
    }
};

struct LevelDesc {
    bool operator()(const Level& a, const Level& b) const noexcept {
        return a.price > b.price;
    }
    bool operator()(std::int64_t price, const Level& b) const noexcept {
        return price > b.price;
    }
    bool operator()(const Level& a, std::int64_t price) const noexcept {
        return a.price > price;
    }
};

using BidLevels = bi::set<Level, bi::compare<LevelDesc>, bi::constant_time_size<false>>;
using AskLevels = bi::set<Level, bi::compare<LevelAsc>,  bi::constant_time_size<false>>;

// A single side of the book. Named BookSide to avoid colliding with the
// public ::Side enum.
template <typename Levels, typename Cmp>
struct BookSide {
    Levels        levels;
    std::uint64_t total_qty{0};

    // Find a level by price; create if absent.
    [[nodiscard]] auto get_or_create(std::int64_t price) -> Level& {
        auto it = levels.find(price, Cmp{});
        if (it == levels.end()) {
            auto* lvl = new Level(price);
            levels.insert(*lvl);
            return *lvl;
        }
        return *it;
    }

    auto erase_level(Level& lvl) noexcept -> void {
        // auto_unlink hook unlinks on destruction
        delete &lvl;
    }
};

}  // anonymous namespace

// -----------------------------------------------------------------------------
//  Impl
// -----------------------------------------------------------------------------
struct ReferenceOrderbook::Impl {
    BookSide<BidLevels, LevelDesc> bids;
    BookSide<AskLevels, LevelAsc>  asks;
    std::unordered_map<std::uint64_t, std::unique_ptr<Node>> by_id;

    [[nodiscard]] auto best_bid_price() const -> std::optional<std::int64_t> {
        if (bids.levels.empty()) return std::nullopt;
        return bids.levels.begin()->price;
    }
    [[nodiscard]] auto best_ask_price() const -> std::optional<std::int64_t> {
        if (asks.levels.empty()) return std::nullopt;
        return asks.levels.begin()->price;
    }

    // Peek how much quantity *could* cross without mutating the book.
    // Used for FOK pre-flight: if the deliverable quantity < requested,
    // the order is rejected and the book stays untouched.
    template <typename OppositeLevels, typename Cross>
    [[nodiscard]] auto peek_crossable(const Order& taker,
                                      const OppositeLevels& levels,
                                      Cross can_cross) const noexcept
        -> std::uint64_t {
        std::uint64_t available = 0;
        for (const auto& level : levels) {
            if (!can_cross(taker.price, level.price)) break;
            available += level.total_quantity;
            if (available >= taker.quantity) {
                return taker.quantity;  // saturated — no need to keep walking
            }
        }
        return available;
    }

    // Match the incoming order against the opposite side. Returns the
    // remaining (unfilled) quantity.
    template <typename OppositeLevels, typename OppositeCmp, typename Cross>
    [[nodiscard]] auto match(const Order& taker,
                             BookSide<OppositeLevels, OppositeCmp>& opposite,
                             Cross can_cross,
                             SubmitResult& out) -> std::uint64_t {
        std::uint64_t remaining = taker.quantity;

        while (remaining > 0 && !opposite.levels.empty()) {
            auto& best = *opposite.levels.begin();
            if (!can_cross(taker.price, best.price)) break;

            // Walk the FIFO at this price level.
            while (remaining > 0 && !best.fifo.empty()) {
                auto& maker_node = best.fifo.front();
                const auto traded = std::min(remaining, maker_node.order.quantity);

                out.fills.push_back(Fill{
                    .taker_id       = taker.id,
                    .maker_id       = maker_node.order.id,
                    .price          = best.price,
                    .quantity       = traded,
                    .aggressor_side = taker.side,
                    .ts_ns          = taker.ts_ns,
                });

                remaining               -= traded;
                maker_node.order.quantity -= traded;
                best.total_quantity     -= traded;
                opposite.total_qty      -= traded;

                if (maker_node.order.quantity == 0) {
                    // Maker fully consumed. Drop it from the FIFO and the
                    // id map (which destroys the Node).
                    const auto id = maker_node.order.id;
                    best.fifo.pop_front();
                    by_id.erase(id);
                }
            }

            if (best.fifo.empty()) {
                // Level emptied; drop it.
                opposite.erase_level(best);
            }
        }

        return remaining;
    }
};

// -----------------------------------------------------------------------------
//  Lifetime
// -----------------------------------------------------------------------------
ReferenceOrderbook::ReferenceOrderbook() : impl_(std::make_unique<Impl>()) {}
ReferenceOrderbook::~ReferenceOrderbook() { clear(); }
ReferenceOrderbook::ReferenceOrderbook(ReferenceOrderbook&&) noexcept            = default;
ReferenceOrderbook& ReferenceOrderbook::operator=(ReferenceOrderbook&&) noexcept = default;

// -----------------------------------------------------------------------------
//  submit
// -----------------------------------------------------------------------------
auto ReferenceOrderbook::submit(const Order& order) -> SubmitResult {
    SubmitResult result;

    if (order.quantity == 0) {
        result.reject = Reject::INVALID;
        return result;
    }

    // Match phase. MARKET orders cross unconditionally; LIMIT orders cross
    // only if the price is on the right side of the opposite best.
    std::uint64_t remaining = order.quantity;

    auto bid_cross = [](std::int64_t taker_px, std::int64_t maker_px) noexcept {
        return taker_px >= maker_px;
    };
    auto ask_cross = [](std::int64_t taker_px, std::int64_t maker_px) noexcept {
        return taker_px <= maker_px;
    };
    auto always = [](std::int64_t, std::int64_t) noexcept { return true; };

    // FOK pre-flight: real exchanges either fully fill a FOK atomically or
    // reject without touching the book. Previously we matched first and
    // then `clear()`ed the fills, leaving the book in a partially-consumed
    // state — that's a correctness bug. Walk the opposite side without
    // mutating it; only proceed if we can fully satisfy the order.
    if (order.tif == TimeInForce::FOK) {
        const auto crossable = (order.side == Side::BUY)
            ? (order.type == OrderType::MARKET
                   ? impl_->peek_crossable(order, impl_->asks.levels, always)
                   : impl_->peek_crossable(order, impl_->asks.levels, bid_cross))
            : (order.type == OrderType::MARKET
                   ? impl_->peek_crossable(order, impl_->bids.levels, always)
                   : impl_->peek_crossable(order, impl_->bids.levels, ask_cross));
        if (crossable < order.quantity) {
            result.reject            = Reject::FOK_UNFILLABLE;
            result.remaining_quantity = 0;
            return result;
        }
    }

    if (order.side == Side::BUY) {
        if (order.type == OrderType::MARKET) {
            remaining = impl_->match(order, impl_->asks, always, result);
        } else {
            remaining = impl_->match(order, impl_->asks, bid_cross, result);
        }
    } else {
        if (order.type == OrderType::MARKET) {
            remaining = impl_->match(order, impl_->bids, always, result);
        } else {
            remaining = impl_->match(order, impl_->bids, ask_cross, result);
        }
    }

    // MARKET that exhausted the book — report and exit.
    if (order.type == OrderType::MARKET && remaining > 0) {
        result.remaining_quantity = remaining;
        result.reject             = Reject::NO_OPPOSITE_SIDE;
        return result;
    }

    // IOC: any unfilled remainder is killed.
    if (order.tif == TimeInForce::IOC) {
        result.remaining_quantity = 0;
        return result;
    }

    // LIMIT GTC with remaining quantity rests on its own side.
    if (remaining > 0 && order.type == OrderType::LIMIT) {
        auto node       = std::make_unique<Node>();
        node->order     = order;
        node->order.quantity = remaining;

        if (order.side == Side::BUY) {
            auto& lvl = impl_->bids.get_or_create(order.price);
            node->level = &lvl;
            lvl.fifo.push_back(*node);
            lvl.total_quantity   += remaining;
            impl_->bids.total_qty += remaining;
        } else {
            auto& lvl = impl_->asks.get_or_create(order.price);
            node->level = &lvl;
            lvl.fifo.push_back(*node);
            lvl.total_quantity   += remaining;
            impl_->asks.total_qty += remaining;
        }

        const auto id = node->order.id;
        impl_->by_id.emplace(id, std::move(node));
        result.rested            = true;
        result.remaining_quantity = remaining;
    } else {
        result.remaining_quantity = 0;
    }

    return result;
}

// -----------------------------------------------------------------------------
//  cancel
// -----------------------------------------------------------------------------
auto ReferenceOrderbook::cancel(std::uint64_t order_id) -> bool {
    auto it = impl_->by_id.find(order_id);
    if (it == impl_->by_id.end()) return false;

    auto& node = *it->second;
    auto* level = static_cast<Level*>(node.level);

    // Update level + side totals.
    level->total_quantity -= node.order.quantity;
    if (node.order.side == Side::BUY) {
        impl_->bids.total_qty -= node.order.quantity;
    } else {
        impl_->asks.total_qty -= node.order.quantity;
    }

    // auto_unlink hook removes the FIFO node when the Node is destroyed;
    // do it explicitly first to keep the level's count accurate above.
    node.unlink();

    if (level->fifo.empty()) {
        if (node.order.side == Side::BUY) {
            impl_->bids.erase_level(*level);
        } else {
            impl_->asks.erase_level(*level);
        }
    }

    impl_->by_id.erase(it);
    return true;
}

// -----------------------------------------------------------------------------
//  Queries
// -----------------------------------------------------------------------------
auto ReferenceOrderbook::best_bid() const -> std::optional<std::int64_t> {
    return impl_->best_bid_price();
}
auto ReferenceOrderbook::best_ask() const -> std::optional<std::int64_t> {
    return impl_->best_ask_price();
}
auto ReferenceOrderbook::bid_depth() const noexcept -> std::uint64_t { return impl_->bids.total_qty; }
auto ReferenceOrderbook::ask_depth() const noexcept -> std::uint64_t { return impl_->asks.total_qty; }
auto ReferenceOrderbook::resting_order_count() const noexcept -> std::size_t {
    return impl_->by_id.size();
}

auto ReferenceOrderbook::top_bids(std::size_t n) const -> std::vector<DepthRow> {
    std::vector<DepthRow> out;
    if (!impl_) return out;
    n = std::min<std::size_t>(n, 50);
    out.reserve(n);
    for (const auto& lvl : impl_->bids.levels) {
        if (out.size() >= n) break;
        out.push_back({lvl.price, lvl.total_quantity});
    }
    return out;
}

auto ReferenceOrderbook::top_asks(std::size_t n) const -> std::vector<DepthRow> {
    std::vector<DepthRow> out;
    if (!impl_) return out;
    n = std::min<std::size_t>(n, 50);
    out.reserve(n);
    for (const auto& lvl : impl_->asks.levels) {
        if (out.size() >= n) break;
        out.push_back({lvl.price, lvl.total_quantity});
    }
    return out;
}

auto ReferenceOrderbook::clear() noexcept -> void {
    if (!impl_) return;
    // Destroy Nodes first; their auto_unlink hooks remove them from the
    // levels' FIFOs.
    impl_->by_id.clear();
    // Now destroy any (empty) leftover levels.
    while (!impl_->bids.levels.empty()) {
        auto& lvl = *impl_->bids.levels.begin();
        impl_->bids.erase_level(lvl);
    }
    while (!impl_->asks.levels.empty()) {
        auto& lvl = *impl_->asks.levels.begin();
        impl_->asks.erase_level(lvl);
    }
    impl_->bids.total_qty = 0;
    impl_->asks.total_qty = 0;
}

}  // namespace velocity::correctness_validator
