// =============================================================================
//  microstructure_test.cpp — unit tests for the microstructure validator.
//
//  Coverage matrix:
//    * FIFO time priority on simple GTC tape
//    * POST_ONLY rejects when crossing
//    * FOK rejects when not fully fillable
//    * STP CANCEL_NEWEST aborts the aggressor
//    * GTD orders expire on tick_clock
//    * Pro-rata allocation within tolerance
// =============================================================================

#include "correctness_validator/microstructure.h"

#include <gtest/gtest.h>

namespace v = velocity::correctness_validator;

namespace {

// Helper: build a maker (resting) order event. By convention resting
// makers have empty claimed_fills and rest.
v::MicroOrderEvent make_resting(std::uint64_t id, v::MicroSide side,
                                 std::int64_t price, std::uint64_t qty,
                                 std::uint64_t account = 0,
                                 v::MicroTif tif = v::MicroTif::GTC) {
    v::MicroOrderEvent ev;
    ev.id = id;
    ev.side = side;
    ev.price = price;
    ev.quantity = qty;
    ev.account_id = account;
    ev.tif = tif;
    ev.ts_ns = static_cast<std::int64_t>(id) * 1'000;  // monotonic
    return ev;
}

// Aggressive order with N claimed fills.
v::MicroOrderEvent make_taker(std::uint64_t id, v::MicroSide side,
                              std::int64_t price, std::uint64_t qty,
                              std::vector<v::MicroFill> claimed,
                              v::MicroTif tif = v::MicroTif::GTC,
                              std::uint64_t account = 0) {
    auto ev = make_resting(id, side, price, qty, account, tif);
    ev.claimed_fills = std::move(claimed);
    return ev;
}

}  // namespace

// -----------------------------------------------------------------------------

TEST(Microstructure, FifoOrderingHappyPath) {
    v::MicrostructureValidator vd;

    // Two resting bids at the same price, both GTC.
    EXPECT_TRUE(vd.process(make_resting(1, v::MicroSide::BUY, 100, 10)).empty());
    EXPECT_TRUE(vd.process(make_resting(2, v::MicroSide::BUY, 100, 5)).empty());

    // Aggressive sell sweeps 12 lots — earliest maker (id=1) takes 10,
    // next maker (id=2) takes 2.
    auto taker = make_taker(3, v::MicroSide::SELL, 100, 12, {
        v::MicroFill{1, 100, 10, 0},
        v::MicroFill{2, 100, 2,  0},
    });
    auto out = vd.process(taker);
    EXPECT_TRUE(out.empty()) << "got " << out.size() << " violations";
}

TEST(Microstructure, FifoOutOfOrderFlagged) {
    v::MicrostructureValidator vd;
    EXPECT_TRUE(vd.process(make_resting(1, v::MicroSide::BUY, 100, 10)).empty());
    EXPECT_TRUE(vd.process(make_resting(2, v::MicroSide::BUY, 100, 5)).empty());

    // Engine matches maker 2 BEFORE maker 1 — violation.
    auto taker = make_taker(3, v::MicroSide::SELL, 100, 15, {
        v::MicroFill{2, 100, 5,  0},
        v::MicroFill{1, 100, 10, 0},
    });
    auto out = vd.process(taker);
    ASSERT_FALSE(out.empty());
    EXPECT_EQ(out.front().kind, v::Violation::FIFO_OUT_OF_ORDER);
}

TEST(Microstructure, PostOnlyCrossingFlagged) {
    v::MicrostructureValidator vd;
    // Resting ask at 100.
    EXPECT_TRUE(vd.process(make_resting(1, v::MicroSide::SELL, 100, 10)).empty());

    // POST_ONLY buy at 100 would cross — engine claimed fills, but
    // POST_ONLY MUST reject in that case.
    auto bad_post_only = make_taker(2, v::MicroSide::BUY, 100, 5, {
        v::MicroFill{1, 100, 5, 0},
    }, v::MicroTif::POST_ONLY);
    auto out = vd.process(bad_post_only);
    ASSERT_FALSE(out.empty());
    bool found_cross = false;
    for (const auto& v : out) {
        if (v.kind == v::Violation::POST_ONLY_CROSSED) found_cross = true;
    }
    EXPECT_TRUE(found_cross);
}

TEST(Microstructure, PostOnlyNonCrossingRests) {
    v::MicrostructureValidator vd;
    EXPECT_TRUE(vd.process(make_resting(1, v::MicroSide::SELL, 105, 10)).empty());
    // Bid at 100, post-only, doesn't cross — should rest cleanly.
    auto ok_post_only = make_taker(2, v::MicroSide::BUY, 100, 5, {},
                                   v::MicroTif::POST_ONLY);
    auto out = vd.process(ok_post_only);
    EXPECT_TRUE(out.empty());
}

TEST(Microstructure, FokRejectsWhenInsufficient) {
    v::MicrostructureValidator vd;
    EXPECT_TRUE(vd.process(make_resting(1, v::MicroSide::SELL, 100, 5)).empty());

    // FOK buy 10 — only 5 takeable, engine should reject; if it didn't
    // (here we simulate the engine claiming the partial fill anyway):
    auto bad_fok = make_taker(2, v::MicroSide::BUY, 100, 10, {
        v::MicroFill{1, 100, 5, 0},
    }, v::MicroTif::FOK);
    auto out = vd.process(bad_fok);
    ASSERT_FALSE(out.empty());
    EXPECT_EQ(out.front().kind, v::Violation::FOK_PARTIAL_FILL);
}

TEST(Microstructure, StpCancelNewest) {
    v::MicrostructureValidator vd;
    // Resting bid from account=42.
    EXPECT_TRUE(vd.process(make_resting(1, v::MicroSide::BUY, 100, 10, 42)).empty());

    // Aggressive sell, also account=42 — must be rejected (cancel newest).
    auto taker = make_resting(2, v::MicroSide::SELL, 100, 5, 42);
    taker.stp = v::StpMode::CANCEL_NEWEST;
    // Engine claims a fill anyway → STP violation surfaced.
    taker.claimed_fills = { v::MicroFill{1, 100, 5, 0} };
    auto out = vd.process(taker);
    ASSERT_FALSE(out.empty());
    bool stp_seen = false;
    for (const auto& v : out) {
        if (v.kind == v::Violation::STP_VIOLATED) stp_seen = true;
    }
    EXPECT_TRUE(stp_seen);
}

TEST(Microstructure, GtdExpiresOnTick) {
    v::MicrostructureValidator vd;
    auto ord = make_resting(1, v::MicroSide::BUY, 100, 10);
    ord.tif = v::MicroTif::GTD;
    ord.expire_at_ns = 1'000'000'000;  // 1s
    EXPECT_TRUE(vd.process(ord).empty());

    // Tick to 1.5s — beyond expiry + grace. Engine never cancelled, so
    // tick_clock should report the late expiry.
    auto out = vd.tick_clock(1'500'000'000);
    ASSERT_FALSE(out.empty());
    EXPECT_EQ(out.front().kind, v::Violation::GTD_EXPIRY_LATE);
    EXPECT_EQ(out.front().order_id, 1u);
}

TEST(Microstructure, ProRataWithinTolerance) {
    v::MicrostructureValidator vd;
    // Three resting bids at price=100 with qty 10/20/30.
    EXPECT_TRUE(vd.process(make_resting(1, v::MicroSide::BUY, 100, 10)).empty());
    EXPECT_TRUE(vd.process(make_resting(2, v::MicroSide::BUY, 100, 20)).empty());
    EXPECT_TRUE(vd.process(make_resting(3, v::MicroSide::BUY, 100, 30)).empty());

    // Aggressive sell 30, pro-rata. Total at level = 60.
    // Expected shares: 5 / 10 / 15.
    auto taker = make_taker(4, v::MicroSide::SELL, 100, 30, {
        v::MicroFill{1, 100, 5,  0},
        v::MicroFill{2, 100, 10, 0},
        v::MicroFill{3, 100, 15, 0},
    });
    taker.match_algo = v::MatchAlgo::PRO_RATA;
    auto out = vd.process(taker);
    EXPECT_TRUE(out.empty());
}
