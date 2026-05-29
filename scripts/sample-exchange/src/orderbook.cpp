// =============================================================================
//  orderbook.cpp — sample-exchange reference matcher.
// =============================================================================

#include "sample_exchange/orderbook.h"

#include <algorithm>

namespace velocity::sample_exchange {

OrderBook::OrderBook() = default;

namespace {

// Helper that knows whether the taker crosses the maker side.
[[nodiscard]] auto crosses(Side taker_side, std::int64_t taker_px,
                           std::int64_t maker_px) noexcept -> bool {
    return taker_side == Side::BUY ? taker_px >= maker_px : taker_px <= maker_px;
}

}  // namespace

template <typename Side1, typename Side2>
auto OrderBook::match_(Order& taker, Side2& opposite, std::vector<Fill>& fills) -> void {
    while (taker.quantity > 0 && !opposite.empty()) {
        auto it = opposite.begin();
        if (!crosses(taker.side, taker.price, it->first)) break;

        auto& fifo = it->second;
        while (taker.quantity > 0 && !fifo.empty()) {
            auto& maker  = fifo.front();
            const auto q = std::min(taker.quantity, maker.quantity);

            fills.push_back(Fill{
                .taker_id       = taker.id,
                .maker_id       = maker.id,
                .price          = it->first,
                .quantity       = q,
                .aggressor_side = taker.side,
                .ts_ns          = taker.ts_ns,
            });

            taker.quantity -= q;
            maker.quantity -= q;
            if (maker.quantity == 0) {
                by_id_.erase(maker.id);
                fifo.pop_front();
            }
        }
        if (fifo.empty()) opposite.erase(it);
    }
}

auto OrderBook::submit(Order order) -> SubmitOutcome {
    SubmitOutcome out;
    if (order.quantity == 0) {
        out.accepted      = false;
        out.reject_reason = "zero quantity";
        return out;
    }
    if (order.id.empty()) {
        out.accepted      = false;
        out.reject_reason = "missing id";
        return out;
    }

    std::lock_guard lock(mu_);

    if (by_id_.contains(order.id)) {
        out.accepted      = false;
        out.reject_reason = "duplicate id";
        return out;
    }

    if (order.side == Side::BUY) {
        match_<decltype(bids_), decltype(asks_)>(order, asks_, out.fills);
    } else {
        match_<decltype(asks_), decltype(bids_)>(order, bids_, out.fills);
    }

    if (order.quantity > 0) {
        if (order.side == Side::BUY) {
            auto& fifo = bids_[order.price];
            fifo.push_back(order);
            by_id_.emplace(order.id, Locator{order.side, order.price, std::prev(fifo.end())});
        } else {
            auto& fifo = asks_[order.price];
            fifo.push_back(order);
            by_id_.emplace(order.id, Locator{order.side, order.price, std::prev(fifo.end())});
        }
        out.resting_quantity = order.quantity;
    }
    return out;
}

auto OrderBook::cancel(const std::string& id) -> bool {
    std::lock_guard lock(mu_);
    auto it = by_id_.find(id);
    if (it == by_id_.end()) return false;

    const auto loc = it->second;
    if (loc.side == Side::BUY) {
        auto level = bids_.find(loc.price);
        if (level != bids_.end()) {
            level->second.erase(loc.it);
            if (level->second.empty()) bids_.erase(level);
        }
    } else {
        auto level = asks_.find(loc.price);
        if (level != asks_.end()) {
            level->second.erase(loc.it);
            if (level->second.empty()) asks_.erase(level);
        }
    }
    by_id_.erase(it);
    return true;
}

auto OrderBook::best_bid() const -> std::optional<std::int64_t> {
    std::lock_guard lock(mu_);
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

auto OrderBook::best_ask() const -> std::optional<std::int64_t> {
    std::lock_guard lock(mu_);
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

auto OrderBook::snapshot(std::size_t levels) const
    -> std::pair<std::vector<DepthLevel>, std::vector<DepthLevel>> {
    std::lock_guard lock(mu_);
    std::vector<DepthLevel> bid_out, ask_out;
    bid_out.reserve(std::min(levels, bids_.size()));
    ask_out.reserve(std::min(levels, asks_.size()));

    auto emit = [](const auto& container, std::size_t n, auto& out) {
        for (auto it = container.begin(); it != container.end() && n > 0; ++it, --n) {
            std::uint64_t qty = 0;
            for (const auto& o : it->second) qty += o.quantity;
            out.push_back(DepthLevel{
                it->first,
                qty,
                static_cast<std::uint32_t>(it->second.size()),
            });
        }
    };
    emit(bids_, levels, bid_out);
    emit(asks_, levels, ask_out);
    return {std::move(bid_out), std::move(ask_out)};
}

}  // namespace velocity::sample_exchange
