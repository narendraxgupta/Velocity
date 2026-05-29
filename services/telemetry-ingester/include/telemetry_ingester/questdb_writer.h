// =============================================================================
//  telemetry_ingester/questdb_writer.h
//
//  ILP-over-TCP writer for QuestDB ≥ 7.x.
//
//  We batch ILP lines in an internal buffer and flush them with a single send
//  call to amortise syscalls. If the socket dies mid-flush we drop the
//  in-flight batch and lazily reconnect on the next flush — losing telemetry
//  is preferable to blocking the ingester hot path.
// =============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace velocity::telemetry_ingester {

class QuestDbWriter {
public:
    QuestDbWriter(std::string host, std::uint16_t port);
    ~QuestDbWriter();

    QuestDbWriter(const QuestDbWriter&)            = delete;
    QuestDbWriter& operator=(const QuestDbWriter&) = delete;

    // Append one row to the internal buffer. Caller is responsible for
    // calling flush() periodically to keep the buffer bounded.
    auto append_order_event(const std::string& submission_id,
                            std::int64_t latency_ns,
                            std::int64_t price_units,
                            std::int64_t qty_units,
                            std::int32_t outcome,
                            std::int64_t event_ts_ns) -> void;

    // Send the accumulated buffer over the TCP connection and reset it.
    auto flush() -> void;

    [[nodiscard]] auto bytes_sent() const noexcept -> std::uint64_t { return bytes_sent_; }
    [[nodiscard]] auto packets_dropped() const noexcept -> std::uint64_t { return packets_dropped_; }

private:
    std::string   host_;
    std::uint16_t port_;
    int           fd_{-1};
    std::string   buf_;
    std::uint64_t bytes_sent_{0};
    std::uint64_t packets_dropped_{0};

    // Resolve `host_` and connect a TCP socket to the QuestDB ILP port.
    auto connect_() -> void;
};

}  // namespace velocity::telemetry_ingester
