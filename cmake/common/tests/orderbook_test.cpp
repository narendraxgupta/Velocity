#include "velocity/orderbook.h"

#include <gtest/gtest.h>

using namespace velocity;

TEST(OrderBookTest, BuyMatchesExistingAsk) {
    InstrumentOrderBook book("XBT/USD");

    OrderPlacedEvent ask{
        .order_id = "ask-1",
        .submission_id = "sub-1",
        .instrument = "XBT/USD",
        .is_buy = false,
        .price_ticks = 10000,
        .quantity = 10,
        .intended_send_ns = 1,
    };
    auto ask_trades = book.ApplyOrder(ask);
    EXPECT_TRUE(ask_trades.empty());

    OrderPlacedEvent bid{
        .order_id = "bid-1",
        .submission_id = "sub-2",
        .instrument = "XBT/USD",
        .is_buy = true,
        .price_ticks = 10050,
        .quantity = 6,
        .intended_send_ns = 2,
    };
    auto bid_trades = book.ApplyOrder(bid);
    ASSERT_EQ(bid_trades.size(), 1);
    EXPECT_EQ(bid_trades[0].maker_order_id, "ask-1");
    EXPECT_EQ(bid_trades[0].taker_order_id, "bid-1");
    EXPECT_EQ(bid_trades[0].price_ticks, 10000);
    EXPECT_EQ(bid_trades[0].quantity, 6);
}

TEST(OrderBookTest, CancelOrderRemovesOpenOrder) {
    InstrumentOrderBook book("XBT/USD");
    OrderPlacedEvent ask{
        .order_id = "ask-2",
        .submission_id = "sub-3",
        .instrument = "XBT/USD",
        .is_buy = false,
        .price_ticks = 20000,
        .quantity = 8,
        .intended_send_ns = 3,
    };
    auto trades = book.ApplyOrder(ask);
    EXPECT_TRUE(trades.empty());

    EXPECT_TRUE(book.CancelOrder("ask-2"));

    OrderPlacedEvent bid{
        .order_id = "bid-2",
        .submission_id = "sub-4",
        .instrument = "XBT/USD",
        .is_buy = true,
        .price_ticks = 20000,
        .quantity = 8,
        .intended_send_ns = 4,
    };
    auto bid_trades = book.ApplyOrder(bid);
    EXPECT_TRUE(bid_trades.empty());
}

TEST(OrderBookTest, ModifyOrderRepricesAndRequeues) {
    InstrumentOrderBook book("XBT/USD");
    OrderPlacedEvent bid{
        .order_id = "bid-3",
        .submission_id = "sub-5",
        .instrument = "XBT/USD",
        .is_buy = true,
        .price_ticks = 9500,
        .quantity = 5,
        .intended_send_ns = 5,
    };
    book.ApplyOrder(bid);

    EXPECT_TRUE(book.ModifyOrder("bid-3", 9800, 5));

    OrderPlacedEvent ask{
        .order_id = "ask-3",
        .submission_id = "sub-6",
        .instrument = "XBT/USD",
        .is_buy = false,
        .price_ticks = 9800,
        .quantity = 5,
        .intended_send_ns = 6,
    };
    auto ask_trades = book.ApplyOrder(ask);
    ASSERT_EQ(ask_trades.size(), 1);
    EXPECT_EQ(ask_trades[0].maker_order_id, "bid-3");
    EXPECT_EQ(ask_trades[0].quantity, 5);
}
