// =============================================================================
//  correctness_validator/microstructure.h
//
//  Microstructure validator — checks that a submission's fills obey the
//  matching engine rules that real exchanges enforce, beyond plain
//  price-time priority:
//
//    * FIFO time priority (the existing reference orderbook enforces this
//      for LIMIT_GTC orders; here we extend coverage to mixed TIF tapes).
//    * Pro-rata allocation when the engine claims `MATCH_ALGO_PRO_RATA`
//      (e.g. CME's STIR contracts): fill quantity must be proportional to
//      each maker's resting quantity, with the "spillover" leftover going
//      to the largest resting order. Tolerates the ±1-share rounding that
//      real engines emit.
//    * Iceberg orders — only `display_quantity` is shown at the top of
//      book, but the full `quantity` must be matched when an aggressor
//      sweeps through.
//    * STP — Self-Trade Prevention: two orders from the same
//      `account_id` that would cross MUST result in one of the
//      configured outcomes:
//          - CANCEL_NEWEST  (default) — incoming order cancelled.
//          - CANCEL_OLDEST  — resting order cancelled.
//          - DECREMENT_BOTH — fill is suppressed and both orders shrink
//                              by the matched quantity.
//    * TIF — Time-in-force:
//          - IOC: any unfilled remainder must NOT rest.
//          - FOK: the engine must reject if total takeable size < ord qty.
//          - GTD: order must expire at exactly `expire_at_ns` (engines
//                  are allowed a 100ms grace window for clock skew).
//          - POST_ONLY: order must be REJECTED (no fills) if it would
//                       have crossed the spread.
//
//  Design
//  ------
//  The validator consumes a stream of `MicroOrderEvent` records — these
//  are the order-acks the submission engine emits, augmented with the
//  engine's *claimed* fills. The validator maintains its own gold book,
//  applies the order as the engine *should have*, and reports a
//  `Violation` whenever the engine's fills disagree.
//
//  This module is independent of `reference_orderbook` — it's a thinner,
//  microstructure-focused checker tuned for tape diffing. Both can run
//  in parallel; mismatches from each go into different Redis lists so
//  judges can drill in without one masking the other.
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace velocity::correctness_validator {

enum class MicroSide : std::uint8_t {
    BUY = 0, SELL = 1,
};

enum class MatchAlgo : std::uint8_t {
    FIFO     = 0,    // pure time priority (the default)
    PRO_RATA = 1,    // size-weighted within a level
};

enum class MicroTif : std::uint8_t {
    GTC       = 0,
    IOC       = 1,
    FOK       = 2,
    GTD       = 3,
    POST_ONLY = 4,
};

enum class StpMode : std::uint8_t {
    NONE            = 0,
    CANCEL_NEWEST   = 1,
    CANCEL_OLDEST   = 2,
    DECREMENT_BOTH  = 3,
};

// The aggressive / passive distinction is established by the order's
// final disposition, not its intent: an order REST_ed without any fills
// is a maker; one that produced fills (and possibly rested its
// remainder) is a taker.
struct MicroFill {
    std::uint64_t maker_id{0};
    std::int64_t  price{0};
    std::uint64_t quantity{0};
    std::int64_t  ts_ns{0};
};

// One order as seen by the engine — id, parameters, AND the fills the
// engine emitted (which the validator scrutinises against its own
// matching).
struct MicroOrderEvent {
    std::uint64_t id{0};
    std::uint64_t account_id{0};
    MicroSide     side{MicroSide::BUY};
    MicroTif      tif{MicroTif::GTC};
    MatchAlgo     match_algo{MatchAlgo::FIFO};
    std::int64_t  price{0};
    std::uint64_t quantity{0};
    std::uint64_t display_quantity{0};   // 0 ⇒ same as quantity (no iceberg)
    std::int64_t  expire_at_ns{0};       // GTD only
    std::int64_t  ts_ns{0};
    StpMode       stp{StpMode::CANCEL_NEWEST};
    std::vector<MicroFill> claimed_fills;
    bool          claimed_rejected{false};
};

enum class Violation : std::uint8_t {
    OK                          = 0,
    FIFO_OUT_OF_ORDER           = 1,   // maker_id is not the earliest at price
    PRO_RATA_SHARE_MISMATCH     = 2,   // allocated qty disagrees with rule
    ICEBERG_VISIBLE_TOO_LARGE   = 3,   // engine showed more than display_qty
    ICEBERG_HIDDEN_NOT_MATCHED  = 4,   // sweep stopped at visible only
    STP_VIOLATED                = 5,   // crossed against own account
    IOC_REMAINDER_RESTED        = 6,
    FOK_PARTIAL_FILL            = 7,   // FOK fired any fill at all without full
    POST_ONLY_CROSSED           = 8,   // post-only matched the spread
    GTD_EXPIRY_LATE             = 9,   // not cancelled by expire_at_ns + grace
    UNKNOWN                     = 255,
};

struct ViolationRecord {
    std::uint64_t   order_id{0};
    std::uint64_t   maker_id{0};       // set when violation is per-fill
    Violation       kind{Violation::OK};
    std::string     detail;
    std::int64_t    ts_ns{0};
};

[[nodiscard]] auto violation_name(Violation v) noexcept -> std::string_view;

class MicrostructureValidator {
public:
    MicrostructureValidator();
    ~MicrostructureValidator();

    MicrostructureValidator(const MicrostructureValidator&)            = delete;
    MicrostructureValidator& operator=(const MicrostructureValidator&) = delete;

    // Feed the validator a single order event. Returns the list of
    // violations triggered by *this* order (zero is the happy path).
    // Internally, the validator advances its gold book to reflect what
    // the engine *should* have done with the order.
    [[nodiscard]] auto process(const MicroOrderEvent& ev) -> std::vector<ViolationRecord>;

    // Advance the validator clock so GTD orders expire. Idempotent.
    // Returns any GTD expiry violations the engine missed.
    [[nodiscard]] auto tick_clock(std::int64_t now_ns) -> std::vector<ViolationRecord>;

    // Counters — surfaced to Prometheus.
    [[nodiscard]] auto orders_seen()      const noexcept -> std::uint64_t;
    [[nodiscard]] auto violations_count() const noexcept -> std::uint64_t;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace velocity::correctness_validator
