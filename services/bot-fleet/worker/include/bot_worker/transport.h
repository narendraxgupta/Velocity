// =============================================================================
//  bot_worker/transport.h
//
//  The transport interface — REST and (Phase 2) WebSocket implementations.
//  send() returns immediately; the ack arrives later via on_ack().
//
//  We keep this synchronous-looking but actually async by burying the
//  io_uring submission into send() and resolving completions on the
//  reactor's poll() iteration. Reactor code therefore stays linear.
// =============================================================================

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "bot_worker/event.h"

namespace velocity::bot_worker {

using AckCallback = std::function<void(std::uint64_t correlation_id,
                                       Outcome outcome,
                                       std::int64_t ack_ts_ns,
                                       std::int64_t fill_price,
                                       std::uint64_t fill_quantity)>;

class Transport {
public:
    virtual ~Transport() = default;

    // Submit one decision against the target. Returns true if the send was
    // accepted by the transport; false on transient failure (back-pressure,
    // not-yet-connected, etc.).
    virtual auto send(std::uint64_t correlation_id, const struct Decision& d) -> bool = 0;

    // Drive any pending I/O. Reactor must call this at least every 100µs.
    virtual auto poll(int max_events) -> void = 0;

    // Register the callback invoked on each ack. Set once at startup.
    virtual auto set_ack_callback(AckCallback cb) -> void = 0;
};

// Factory: REST transport over a host:port. Internally uses io_uring on Linux
// and a portable fallback on macOS/Windows for dev convenience.
[[nodiscard]] auto make_rest_transport(std::string host, std::uint16_t port,
                                       std::uint32_t connection_count)
    -> std::unique_ptr<Transport>;

[[nodiscard]] auto make_ws_transport(std::string url) -> std::unique_ptr<Transport>;

// FIX 4.4 transport. Opens a TCP connection, performs Logon (35=A), and
// then translates `Decision`s into NewOrderSingle (35=D) / OrderCancelRequest
// (35=F). ExecutionReports (35=8) drive the ack callback.
//
// `sender_comp_id` and `target_comp_id` identify the parties; defaults are
// "VELOCITY-BOT" / "SUBMISSION".
//
// `symbol` is the FIX `55` tag (instrument). `price_scale` is the number of
// implied decimal places in the integer prices supplied via `Decision::price`
// (i.e. how many places must be inserted before serialising to wire). When
// `price_scale == 0` we emit prices as plain integers (FIX accepts those).
[[nodiscard]] auto make_fix_transport(std::string host,
                                      std::uint16_t port,
                                      std::string sender_comp_id = "VELOCITY-BOT",
                                      std::string target_comp_id = "SUBMISSION",
                                      std::string symbol         = "SPOT/USDT",
                                      std::uint32_t price_scale  = 0)
    -> std::unique_ptr<Transport>;

}  // namespace velocity::bot_worker
