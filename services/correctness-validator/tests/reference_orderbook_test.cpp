// =============================================================================
//  reference_orderbook_test.cpp — full golden + property tests.
//
//  These tests are the contract. If something here fails, the
//  correctness-validator can no longer score submissions reliably.
// =============================================================================

#include "correctness_validator/reference_orderbook.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <random>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

using velocity::correctness_validator::Order;
using velocity::correctness_validator::OrderType;
using velocity::correctness_validator::ReferenceOrderbook;
using velocity::correctness_validator::Reject;
using velocity::correctness_validator::Side;
using velocity::correctness_validator::TimeInForce;

namespace {

[[nodiscard]] auto make_limit(std::uint64_t id, Side s, std::int64_t px,
                              std::uint64_t qty, std::int64_t ts = 0) -> Order {
    return Order{id, px, qty, ts, s, OrderType::LIMIT, TimeInForce::GTC};
}

[[nodiscard]] auto make_market(std::uint64_t id, Side s, std::uint64_t qty,
                               std::int64_t ts = 0) -> Order {
    return Order{id, 0, qty, ts, s, OrderType::MARKET, TimeInForce::IOC};
}

[[nodiscard]] auto make_ioc(std::uint64_t id, Side s, std::int64_t px,
                            std::uint64_t qty) -> Order {
    return Order{id, px, qty, 0, s, OrderType::LIMIT, TimeInForce::IOC};
}

[[nodiscard]] auto make_fok(std::uint64_t id, Side s, std::int64_t px,
                            std::uint64_t qty) -> Order {
    return Order{id, px, qty, 0, s, OrderType::LIMIT, TimeInForce::FOK};
}

// ----------------------- Basic invariants ----------------------------------

TEST(ReferenceOrderbook, BootsEmpty) {
    ReferenceOrderbook b;
    EXPECT_FALSE(b.best_bid().has_value());
    EXPECT_FALSE(b.best_ask().has_value());
    EXPECT_EQ(b.bid_depth(), 0u);
    EXPECT_EQ(b.ask_depth(), 0u);
    EXPECT_EQ(b.resting_order_count(), 0u);
}

TEST(ReferenceOrderbook, RestsLimitOrderOnEmptyBook) {
    ReferenceOrderbook b;
    auto r = b.submit(make_limit(1, Side::BUY, 1000, 10));
    EXPECT_TRUE(r.fills.empty());
    EXPECT_TRUE(r.rested);
    EXPECT_EQ(r.remaining_quantity, 10u);
    EXPECT_EQ(b.best_bid(), 1000);
    EXPECT_FALSE(b.best_ask().has_value());
    EXPECT_EQ(b.bid_depth(), 10u);
}

// ----------------------- Price priority ------------------------------------

TEST(ReferenceOrderbook, BidsSortedByDescendingPrice) {
    ReferenceOrderbook b;
    b.submit(make_limit(1, Side::BUY, 1000, 10));
    b.submit(make_limit(2, Side::BUY, 1010, 10));   // better
    b.submit(make_limit(3, Side::BUY,  990, 10));   // worse
    EXPECT_EQ(b.best_bid(), 1010);
}

TEST(ReferenceOrderbook, AsksSortedByAscendingPrice) {
    ReferenceOrderbook b;
    b.submit(make_limit(1, Side::SELL, 1000, 10));
    b.submit(make_limit(2, Side::SELL, 1010, 10));
    b.submit(make_limit(3, Side::SELL,  990, 10));  // better
    EXPECT_EQ(b.best_ask(), 990);
}

// ----------------------- Time priority -------------------------------------

TEST(ReferenceOrderbook, TimePriorityAtSamePrice) {
    ReferenceOrderbook b;
    b.submit(make_limit(1, Side::BUY, 1000, 10, /*ts=*/100));
    b.submit(make_limit(2, Side::BUY, 1000, 10, /*ts=*/200));

    // Sell aggressor matches the earliest first (id=1).
    auto r = b.submit(make_limit(3, Side::SELL, 1000, 15));
    ASSERT_EQ(r.fills.size(), 2u);
    EXPECT_EQ(r.fills[0].maker_id, 1u);
    EXPECT_EQ(r.fills[0].quantity, 10u);
    EXPECT_EQ(r.fills[1].maker_id, 2u);
    EXPECT_EQ(r.fills[1].quantity, 5u);
}

// ----------------------- Crossing & matching -------------------------------

TEST(ReferenceOrderbook, AggressorCrossesAndConsumesMultipleLevels) {
    ReferenceOrderbook b;
    b.submit(make_limit(1, Side::SELL, 1000, 5));
    b.submit(make_limit(2, Side::SELL, 1001, 5));
    b.submit(make_limit(3, Side::SELL, 1002, 5));

    auto r = b.submit(make_limit(4, Side::BUY, 1002, 12));
    ASSERT_EQ(r.fills.size(), 3u);
    EXPECT_EQ(r.fills[0].price, 1000);
    EXPECT_EQ(r.fills[0].quantity, 5u);
    EXPECT_EQ(r.fills[1].price, 1001);
    EXPECT_EQ(r.fills[1].quantity, 5u);
    EXPECT_EQ(r.fills[2].price, 1002);
    EXPECT_EQ(r.fills[2].quantity, 2u);

    // The remaining 0 of the 12 buy quantity should not rest (everything filled).
    EXPECT_FALSE(r.rested);

    // The 1002-level should still hold 3 units.
    EXPECT_EQ(b.best_ask(), 1002);
    EXPECT_EQ(b.ask_depth(), 3u);
}

TEST(ReferenceOrderbook, LimitOrderRestsAfterPartialFill) {
    ReferenceOrderbook b;
    b.submit(make_limit(1, Side::SELL, 1000, 5));
    auto r = b.submit(make_limit(2, Side::BUY, 1000, 15));
    EXPECT_EQ(r.fills.size(), 1u);
    EXPECT_EQ(r.remaining_quantity, 10u);
    EXPECT_TRUE(r.rested);
    EXPECT_EQ(b.best_bid(), 1000);
    EXPECT_EQ(b.bid_depth(), 10u);
}

// ----------------------- MARKET orders -------------------------------------

TEST(ReferenceOrderbook, MarketOrderConsumesUntilExhausted) {
    ReferenceOrderbook b;
    b.submit(make_limit(1, Side::SELL, 1000, 5));
    b.submit(make_limit(2, Side::SELL, 1005, 5));
    auto r = b.submit(make_market(3, Side::BUY, 100));
    EXPECT_EQ(r.fills.size(), 2u);
    EXPECT_EQ(r.remaining_quantity, 90u);
    EXPECT_EQ(r.reject, Reject::NO_OPPOSITE_SIDE);
    EXPECT_FALSE(r.rested);
}

TEST(ReferenceOrderbook, MarketOnEmptyBookIsRejected) {
    ReferenceOrderbook b;
    auto r = b.submit(make_market(1, Side::BUY, 10));
    EXPECT_TRUE(r.fills.empty());
    EXPECT_EQ(r.reject, Reject::NO_OPPOSITE_SIDE);
}

// ----------------------- IOC / FOK ----------------------------------------

TEST(ReferenceOrderbook, IocFillsAvailableThenKills) {
    ReferenceOrderbook b;
    b.submit(make_limit(1, Side::SELL, 1000, 5));
    auto r = b.submit(make_ioc(2, Side::BUY, 1000, 20));
    EXPECT_EQ(r.fills.size(), 1u);
    EXPECT_EQ(r.fills[0].quantity, 5u);
    EXPECT_EQ(r.remaining_quantity, 0u);
    EXPECT_FALSE(r.rested);
}

TEST(ReferenceOrderbook, FokFullyFillsOrRejectsAtomically) {
    ReferenceOrderbook b;
    b.submit(make_limit(1, Side::SELL, 1000, 5));
    auto r = b.submit(make_fok(2, Side::BUY, 1000, 20));
    EXPECT_TRUE(r.fills.empty());
    EXPECT_EQ(r.reject, Reject::FOK_UNFILLABLE);
    EXPECT_EQ(b.ask_depth(), 5u);   // book untouched
}

TEST(ReferenceOrderbook, FokSucceedsWhenLiquiditySufficient) {
    ReferenceOrderbook b;
    b.submit(make_limit(1, Side::SELL, 1000, 5));
    b.submit(make_limit(2, Side::SELL, 1001, 15));
    auto r = b.submit(make_fok(3, Side::BUY, 1001, 20));
    EXPECT_EQ(r.fills.size(), 2u);
    EXPECT_EQ(r.reject, Reject::NONE);
    EXPECT_EQ(b.ask_depth(), 0u);
}

// ----------------------- Cancellation --------------------------------------

TEST(ReferenceOrderbook, CancelRemovesOrderFromBook) {
    ReferenceOrderbook b;
    b.submit(make_limit(1, Side::BUY, 1000, 10));
    EXPECT_TRUE(b.cancel(1));
    EXPECT_FALSE(b.cancel(1));  // already gone
    EXPECT_FALSE(b.best_bid().has_value());
    EXPECT_EQ(b.bid_depth(), 0u);
}

TEST(ReferenceOrderbook, CancelMidLevelPreservesTimePriority) {
    ReferenceOrderbook b;
    b.submit(make_limit(1, Side::BUY, 1000, 10, 100));
    b.submit(make_limit(2, Side::BUY, 1000, 10, 200));
    b.submit(make_limit(3, Side::BUY, 1000, 10, 300));

    EXPECT_TRUE(b.cancel(2));

    auto r = b.submit(make_limit(4, Side::SELL, 1000, 20));
    ASSERT_EQ(r.fills.size(), 2u);
    EXPECT_EQ(r.fills[0].maker_id, 1u);
    EXPECT_EQ(r.fills[1].maker_id, 3u);
}

// ----------------------- Property tests ------------------------------------

// Property: after an arbitrary stream of submits/cancels, a faithful shadow
// model of the book agrees with it on the number of resting orders, and the
// top-of-book stays within sane bounds.
TEST(ReferenceOrderbook, BookInvariantsHoldUnderRandomOps) {
    ReferenceOrderbook b;
    std::mt19937 rng{42};
    std::uniform_int_distribution<int>  side_dist(0, 1);
    std::uniform_int_distribution<long> price_dist(990, 1010);
    std::uniform_int_distribution<long> qty_dist(1, 20);
    std::uniform_int_distribution<int>  op_dist(0, 9);   // 0..7 submit, 8..9 cancel

    // Shadow model of the book: id -> remaining resting quantity. An id is
    // present here iff that order is currently resting in `b`. We mirror every
    // mutation the book makes: an aggressor consumes resting makers (decrement,
    // and erase once fully consumed), and a limit remainder is added only when
    // the book reports it actually rested.
    std::unordered_map<std::uint64_t, std::uint64_t> resting;
    std::uint64_t next_id = 1;

    for (int i = 0; i < 5'000; ++i) {
        if (op_dist(rng) >= 8 && !resting.empty()) {
            // Cancel a random currently-resting id.
            std::uniform_int_distribution<std::size_t> idx_dist(0, resting.size() - 1);
            auto it = resting.begin();
            std::advance(it, static_cast<std::ptrdiff_t>(idx_dist(rng)));
            EXPECT_TRUE(b.cancel(it->first));
            resting.erase(it);
            continue;
        }

        const auto s   = (side_dist(rng) == 0) ? Side::BUY : Side::SELL;
        const auto px  = price_dist(rng);
        const auto qty = static_cast<std::uint64_t>(qty_dist(rng));
        const auto id  = next_id++;
        const auto r   = b.submit(make_limit(id, s, px, qty));

        // Each fill consumes quantity from a resting maker. A maker that is
        // fully consumed leaves the book, so drop it from the shadow too.
        for (const auto& f : r.fills) {
            const auto mit = resting.find(f.maker_id);
            ASSERT_NE(mit, resting.end())
                << "fill against maker " << f.maker_id
                << " that the shadow model does not have resting";
            ASSERT_GE(mit->second, f.quantity);
            mit->second -= f.quantity;
            if (mit->second == 0) resting.erase(mit);
        }

        // The aggressor's remainder rests iff the book says so.
        if (r.rested) resting[id] = r.remaining_quantity;
    }

    // Final invariants: the shadow and the book agree on resting order count.
    EXPECT_EQ(b.resting_order_count(), resting.size());
    if (b.best_bid().has_value()) {
        EXPECT_GE(*b.best_bid(), 990);
        EXPECT_LE(*b.best_bid(), 1010);
    }
    if (b.best_ask().has_value() && b.best_bid().has_value()) {
        EXPECT_LT(*b.best_bid(), *b.best_ask())
            << "bid must not be at-or-above ask in a sane book";
    }
}

// Determinism: running the same ops twice produces the same fills.
TEST(ReferenceOrderbook, IsDeterministic) {
    auto play = []() {
        ReferenceOrderbook b;
        std::vector<velocity::correctness_validator::Fill> out;
        std::mt19937 rng{123};
        std::uniform_int_distribution<int>  side_dist(0, 1);
        std::uniform_int_distribution<long> price_dist(995, 1005);
        std::uniform_int_distribution<long> qty_dist(1, 5);
        for (int i = 0; i < 500; ++i) {
            const auto s   = (side_dist(rng) == 0) ? Side::BUY : Side::SELL;
            const auto px  = price_dist(rng);
            const auto qty = static_cast<std::uint64_t>(qty_dist(rng));
            auto r = b.submit(make_limit(static_cast<std::uint64_t>(i + 1), s, px, qty));
            for (auto& f : r.fills) out.push_back(f);
        }
        return out;
    };

    const auto a = play();
    const auto bv = play();
    ASSERT_EQ(a.size(), bv.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].taker_id, bv[i].taker_id);
        EXPECT_EQ(a[i].maker_id, bv[i].maker_id);
        EXPECT_EQ(a[i].price,    bv[i].price);
        EXPECT_EQ(a[i].quantity, bv[i].quantity);
    }
}

}  // namespace
