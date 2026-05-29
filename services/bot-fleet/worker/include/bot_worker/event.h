// =============================================================================
//  bot_worker/event.h
//
//  The internal POD that reactor threads push into the SPSC ring buffer
//  and the publisher drains from it. Smaller than the wire protobuf —
//  serialization to OrderEvent happens on the publisher thread.
// =============================================================================

#pragma once

#include <array>
#include <cstdint>

namespace velocity::bot_worker {

enum class Side    : std::uint8_t { BUY = 0, SELL = 1 };
enum class Kind    : std::uint8_t { NEW = 0, CANCEL = 1, MODIFY = 2 };
enum class Outcome : std::uint8_t {
    UNKNOWN   = 0,
    ACK       = 1,
    REJECT    = 2,
    FILLED    = 3,
    PARTIAL   = 4,
    CANCELLED = 5,
    TIMEOUT   = 6,
};

// 128 bytes — fits in two cache lines. Aligned for the ring buffer.
struct alignas(64) Event {
    std::array<char, 26> submission_id;   // ULID
    std::uint64_t correlation_id;
    std::int64_t  intended_ts_ns;          // when we *meant* to send
    std::int64_t  sent_ts_ns;
    std::int64_t  ack_ts_ns;               // 0 = pending
    std::int64_t  price;                   // fixed-point
    std::uint64_t quantity;
    std::uint64_t fill_quantity;
    std::int64_t  fill_price;
    Kind          kind;
    Side          side;
    Outcome       outcome;
    std::uint8_t  reactor_id;
    std::uint32_t bot_id;
};

}  // namespace velocity::bot_worker
