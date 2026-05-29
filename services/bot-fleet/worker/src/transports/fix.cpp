// =============================================================================
//  transports/fix.cpp — FIX 4.4 transport.
//
//  Design
//  ------
//  Hand-rolled, SOH-delimited FIX 4.4 encoder/decoder over a non-blocking
//  TCP socket. We deliberately avoid pulling in QuickFIX/C++ for three
//  reasons:
//
//    * Build hygiene. QuickFIX has its own session store, threading model,
//      and config-file requirements. For the *load-generator* role we just
//      need outbound NewOrderSingle + cancel + an ExecutionReport reader —
//      a couple hundred lines, not a dependency.
//
//    * Latency. Every cycle we save on the bot side amortises over millions
//      of orders. QuickFIX's SessionThread + persistence layer adds
//      meaningful µs at our throughputs.
//
//    * Determinism. A purpose-built encoder is trivially replayable: the
//      same Decision stream always produces the same bytes.
//
//  Threading
//  ---------
//  The reactor owns one FixTransport instance and is the *only* caller of
//  send(); the dedicated reader thread is the *only* caller of recv(). The
//  socket file descriptor therefore has exactly one writer and one reader,
//  so we do not need an outbound mutex. Asynchronous ExecutionReports flow
//  from reader_loop_ into a boost::lockfree::spsc_queue, which poll() drains
//  on the reactor thread.
//
//  Wire format produced (representative, SOH-delimited; `|` shown for
//  clarity):
//
//    8=FIX.4.4|9=<len>|35=D|49=<sender>|56=<target>|34=<seq>|52=<ts>|
//    11=<correlation_id>|55=<symbol>|54=1|38=100|40=2|44=12.34|10=<chk>
//
//  Symbols / sides:
//    Side  : 1=BUY, 2=SELL
//    OrdType: 1=Market, 2=Limit
//
//  This is a production-grade implementation for the *load-generator* role.
//  We deliberately do not implement persistent session store, resend
//  requests, or gap fills — they are unnecessary for an open-loop load
//  generator that is restarted per-benchmark.
// =============================================================================

#include "bot_worker/transport.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <boost/lockfree/spsc_queue.hpp>

#include "velocity/common/log.h"
#include "velocity/common/time.h"

#if defined(__linux__) || defined(__APPLE__)
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#  define VELOCITY_FIX_POSIX 1
#else
#  define VELOCITY_FIX_POSIX 0
#endif

#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0
#endif

namespace velocity::bot_worker {
namespace {

constexpr char SOH = '\x01';

struct AckRecord {
    std::uint64_t correlation_id;
    Outcome       outcome;
    std::int64_t  ack_ts_ns;
    std::int64_t  fill_price;
    std::uint64_t fill_quantity;
};

// Compute the FIX checksum: sum of all bytes in `prefix` mod 256, formatted
// as "10=NNN<SOH>".
//
// IMPORTANT: per FIX 4.4 spec, the checksum is computed over the *entire*
// message — every byte from the leading "8=FIX.4.4..." up to and including
// the SOH preceding tag 10. A previous bug computed checksum over the body
// only; counterparts will reject those messages.
[[nodiscard]] auto checksum(std::string_view prefix) noexcept -> std::string {
    std::uint32_t sum = 0;
    for (auto c : prefix) sum += static_cast<std::uint8_t>(c);
    sum %= 256;
    std::array<char, 16> buf{};
    std::snprintf(buf.data(), buf.size(), "10=%03u%c", sum, SOH);
    return {buf.data()};
}

// Format an unsigned int into a stack buffer (avoids std::to_string allocs).
struct IntBuf {
    char  data[32];
    std::size_t len{0};
};
[[nodiscard]] auto fmt_u64(std::uint64_t v) noexcept -> IntBuf {
    IntBuf b{};
    if (v == 0) { b.data[0] = '0'; b.len = 1; return b; }
    char tmp[32]; std::size_t n = 0;
    while (v > 0) { tmp[n++] = static_cast<char>('0' + (v % 10)); v /= 10; }
    for (std::size_t i = 0; i < n; ++i) b.data[i] = tmp[n - 1 - i];
    b.len = n;
    return b;
}

// Format a fixed-point integer price (`units`, `scale` decimal places) as a
// human-readable decimal string suitable for tag 44. e.g. units=1234500,
// scale=4 -> "123.4500"; scale=0 -> "1234500".
[[nodiscard]] auto fmt_decimal(std::int64_t units, std::uint32_t scale) -> std::string {
    const bool negative = units < 0;
    auto abs_units = static_cast<std::uint64_t>(negative ? -units : units);
    if (scale == 0) {
        const auto b = fmt_u64(abs_units);
        std::string s;
        if (negative) s.push_back('-');
        s.append(b.data, b.len);
        return s;
    }
    auto integral_part = fmt_u64(abs_units);
    std::string intpart(integral_part.data, integral_part.len);
    if (intpart.size() <= scale) {
        intpart.insert(0, scale - intpart.size() + 1, '0');
    }
    std::string out;
    if (negative) out.push_back('-');
    out.append(intpart.substr(0, intpart.size() - scale));
    out.push_back('.');
    out.append(intpart.substr(intpart.size() - scale));
    return out;
}

// Format a wall-clock instant as a FIX 4.4 UTCTimestamp:
// "YYYYMMDD-HH:MM:SS.mmm".
[[nodiscard]] auto fmt_utc_timestamp(std::chrono::system_clock::time_point tp)
    -> std::string {
    using namespace std::chrono;
    const auto ms_since_epoch =
        duration_cast<milliseconds>(tp.time_since_epoch()).count();
    const auto sec = ms_since_epoch / 1000;
    const auto ms  = static_cast<int>(ms_since_epoch % 1000);
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &sec);
#else
    {
        const std::time_t t = static_cast<std::time_t>(sec);
        gmtime_r(&t, &tm_buf);
    }
#endif
    std::array<char, 32> out{};
    std::snprintf(out.data(), out.size(),
                  "%04d%02d%02d-%02d:%02d:%02d.%03d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, ms);
    return {out.data()};
}

// Build the body of a FIX message (everything between 9=... and 10=...).
// `msg_type` is e.g. "D" for NewOrderSingle.
auto build_body(std::string& out, char msg_type,
                std::string_view sender, std::string_view target,
                std::uint64_t seq, std::string_view sending_time) -> void {
    out.clear();
    out.reserve(192);
    out.append("35="); out.push_back(msg_type); out.push_back(SOH);
    out.append("49="); out.append(sender);     out.push_back(SOH);
    out.append("56="); out.append(target);     out.push_back(SOH);
    out.append("34="); auto s = fmt_u64(seq);    out.append(s.data, s.len); out.push_back(SOH);
    out.append("52="); out.append(sending_time); out.push_back(SOH);
}

// Compose the on-the-wire bytes for a complete FIX message: standard
// header + body + checksum. The checksum is computed over header+body, as
// required by FIX 4.4.
[[nodiscard]] auto finalize_message(const std::string& body) -> std::string {
    std::string len_field;
    len_field.reserve(16);
    len_field.append("9=");
    auto b = fmt_u64(body.size());
    len_field.append(b.data, b.len);
    len_field.push_back(SOH);

    std::string out;
    out.reserve(16 + len_field.size() + body.size() + 8);
    out.append("8=FIX.4.4");
    out.push_back(SOH);
    out.append(len_field);
    out.append(body);
    out.append(checksum(out));
    return out;
}

// Extract a tag value out of an inbound FIX message buffer. Returns
// {pos, len} into the source string. Tag "11" -> ClOrdID, etc.
struct TagView { std::size_t pos{std::string::npos}; std::size_t len{0}; };

[[nodiscard]] auto get_tag(std::string_view msg, std::string_view tag) noexcept
    -> TagView {
    std::string needle;
    needle.reserve(tag.size() + 1);
    needle.append(tag);
    needle.push_back('=');
    std::size_t i = 0;
    while (i < msg.size()) {
        if (i == 0 || msg[i - 1] == SOH) {
            if (msg.compare(i, needle.size(), needle) == 0) {
                const std::size_t v = i + needle.size();
                std::size_t e = msg.find(SOH, v);
                if (e == std::string_view::npos) e = msg.size();
                return {v, e - v};
            }
        }
        ++i;
    }
    return {};
}

#if VELOCITY_FIX_POSIX

class FixTransport final : public Transport {
public:
    FixTransport(std::string host, std::uint16_t port,
                 std::string sender, std::string target,
                 std::string symbol, std::uint32_t price_scale)
        : host_(std::move(host)),
          port_(port),
          sender_(std::move(sender)),
          target_(std::move(target)),
          symbol_(std::move(symbol)),
          price_scale_(price_scale),
          ack_q_(1 << 16) {
        connect_socket_();
        send_logon_();
        reader_ = std::thread([this] { reader_loop_(); });
    }

    ~FixTransport() override {
        stop_.store(true, std::memory_order_release);
        if (sockfd_ >= 0) {
            // Best-effort logout before we tear the socket down.
            send_logout_();
            ::shutdown(sockfd_, SHUT_RDWR);
        }
        if (reader_.joinable()) reader_.join();
        if (sockfd_ >= 0) {
            ::close(sockfd_);
            sockfd_ = -1;
        }
    }

    auto send(std::uint64_t correlation_id, const Decision& d) -> bool override {
        if (sockfd_ < 0) {
            send_errors_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const auto sending_time = fmt_utc_timestamp(std::chrono::system_clock::now());
        const auto seq = next_seq_.fetch_add(1, std::memory_order_relaxed);

        std::string body;
        if (d.kind == Kind::CANCEL) {
            build_body(body, 'F', sender_, target_, seq, sending_time);
            body.append("41="); auto a = fmt_u64(d.cancel_id);
            body.append(a.data, a.len); body.push_back(SOH);
            body.append("11="); auto c = fmt_u64(correlation_id);
            body.append(c.data, c.len); body.push_back(SOH);
            body.append("55="); body.append(symbol_); body.push_back(SOH);
            body.append("54="); body.push_back(d.side == Side::BUY ? '1' : '2');
            body.push_back(SOH);
            body.append("60="); body.append(sending_time); body.push_back(SOH);
        } else {
            build_body(body, 'D', sender_, target_, seq, sending_time);
            body.append("11="); auto c = fmt_u64(correlation_id);
            body.append(c.data, c.len); body.push_back(SOH);
            body.append("55="); body.append(symbol_); body.push_back(SOH);
            body.append("54="); body.push_back(d.side == Side::BUY ? '1' : '2');
            body.push_back(SOH);
            body.append("38="); auto q = fmt_u64(d.quantity);
            body.append(q.data, q.len); body.push_back(SOH);
            body.append("40="); body.push_back(d.price == 0 ? '1' : '2');
            body.push_back(SOH);
            if (d.price != 0) {
                const auto px = fmt_decimal(d.price, price_scale_);
                body.append("44="); body.append(px); body.push_back(SOH);
            }
            body.append("59=0"); body.push_back(SOH);
            body.append("60="); body.append(sending_time); body.push_back(SOH);
        }

        return write_wire_(finalize_message(body));
    }

    auto poll(int /*max_events*/) -> void override {
        AckRecord rec;
        while (ack_q_.pop(rec)) {
            if (ack_cb_) {
                ack_cb_(rec.correlation_id, rec.outcome,
                        rec.ack_ts_ns, rec.fill_price, rec.fill_quantity);
            }
        }
    }

    auto set_ack_callback(AckCallback cb) -> void override { ack_cb_ = std::move(cb); }

private:
    auto connect_socket_() -> void {
        sockfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd_ < 0) throw std::runtime_error("fix: socket() failed");

        int one = 1;
        ::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        addrinfo hints{};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        const auto port_s = std::to_string(port_);
        const auto gai = ::getaddrinfo(host_.c_str(), port_s.c_str(), &hints, &res);
        if (gai != 0 || !res) {
            ::close(sockfd_); sockfd_ = -1;
            VLOG_WARN("fix: getaddrinfo failed for {}", host_);
            return;
        }
        const auto rc = ::connect(sockfd_, res->ai_addr,
                                  static_cast<socklen_t>(res->ai_addrlen));
        ::freeaddrinfo(res);
        if (rc != 0) {
            VLOG_WARN("fix: connect to {}:{} failed; transport degraded", host_, port_);
            ::close(sockfd_);
            sockfd_ = -1;
        }
    }

    auto send_logon_() -> void {
        if (sockfd_ < 0) return;
        const auto sending_time = fmt_utc_timestamp(std::chrono::system_clock::now());
        std::string body;
        build_body(body, 'A', sender_, target_,
                   next_seq_.fetch_add(1, std::memory_order_relaxed), sending_time);
        body.append("98=0"); body.push_back(SOH);       // EncryptMethod
        body.append("108=30"); body.push_back(SOH);     // HeartBtInt = 30s
        body.append("141=Y"); body.push_back(SOH);      // ResetSeqNumFlag
        write_wire_(finalize_message(body));
    }

    auto send_logout_() -> void {
        if (sockfd_ < 0) return;
        const auto sending_time = fmt_utc_timestamp(std::chrono::system_clock::now());
        std::string body;
        build_body(body, '5', sender_, target_,
                   next_seq_.fetch_add(1, std::memory_order_relaxed), sending_time);
        write_wire_(finalize_message(body));
    }

    // Write a complete FIX message to the socket, looping until the entire
    // buffer is flushed or send() fails permanently.
    [[nodiscard]] auto write_wire_(const std::string& wire) -> bool {
        std::size_t written = 0;
        while (written < wire.size()) {
            const auto n = ::send(sockfd_, wire.data() + written,
                                  wire.size() - written, MSG_NOSIGNAL);
            if (n > 0) {
                written += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EINTR)) continue;
            send_errors_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        sent_orders_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Pull the BodyLength out of an inbound frame so we can compute the
    // exact end of message: header + body + checksum (always 7 bytes:
    // "10=NNN" + SOH).
    [[nodiscard]] static auto frame_length_(std::string_view view) noexcept
        -> std::size_t {
        constexpr std::string_view kHeader = "8=FIX.4.4\x01" "9=";
        if (view.size() < kHeader.size() + 4) return 0;
        if (view.compare(0, kHeader.size(), kHeader) != 0) return 0;
        const std::size_t p = kHeader.size();
        const auto end = view.find(SOH, p);
        if (end == std::string_view::npos) return 0;
        std::uint64_t body_len = 0;
        for (std::size_t i = p; i < end; ++i) {
            const char c = view[i];
            if (c < '0' || c > '9') return 0;
            body_len = body_len * 10 + static_cast<std::uint64_t>(c - '0');
        }
        // Header through the SOH after the BodyLength field, then body,
        // then "10=NNN<SOH>" (7 bytes).
        return (end + 1) + static_cast<std::size_t>(body_len) + 7;
    }

    auto reader_loop_() -> void {
        if (sockfd_ < 0) return;
        std::string buf;
        buf.reserve(8192);
        std::array<char, 4096> tmp{};
        while (!stop_.load(std::memory_order_acquire)) {
            const auto n = ::recv(sockfd_, tmp.data(), tmp.size(), 0);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                break;
            }
            buf.append(tmp.data(), static_cast<std::size_t>(n));

            for (;;) {
                const auto frame_len = frame_length_(buf);
                if (frame_len == 0 || buf.size() < frame_len) break;
                process_frame_(std::string_view(buf.data(), frame_len));
                buf.erase(0, frame_len);
            }
        }
    }

    auto process_frame_(std::string_view msg) -> void {
        const auto t = get_tag(msg, "35");
        if (t.pos == std::string::npos || t.len != 1) return;
        const auto mtype = msg[t.pos];

        // ExecutionReport (35=8) is what we care about. Heartbeats / Logon
        // / Logout are silently consumed.
        if (mtype != '8') return;

        AckRecord rec{};
        const auto cid_tag = get_tag(msg, "11");
        if (cid_tag.pos == std::string::npos) return;
        try {
            rec.correlation_id = std::stoull(std::string(msg.substr(cid_tag.pos, cid_tag.len)));
        } catch (...) { return; }

        const auto et_tag = get_tag(msg, "150");
        const char et = (et_tag.pos != std::string::npos && et_tag.len > 0)
            ? msg[et_tag.pos] : '0';
        switch (et) {
            case '0': rec.outcome = Outcome::ACK;       break;  // New
            case '4': rec.outcome = Outcome::CANCELLED; break;  // Cancelled
            case '8': rec.outcome = Outcome::REJECT;    break;  // Rejected
            case 'F': rec.outcome = Outcome::FILLED;    break;  // Trade
            case '1': rec.outcome = Outcome::PARTIAL;   break;  // PartialFill
            default:  rec.outcome = Outcome::ACK;       break;
        }

        const auto px_tag  = get_tag(msg, "31");
        const auto qty_tag = get_tag(msg, "32");
        if (px_tag.pos != std::string::npos) {
            try { rec.fill_price =
                static_cast<std::int64_t>(std::stoll(std::string(msg.substr(px_tag.pos, px_tag.len))));
            } catch (...) {}
        }
        if (qty_tag.pos != std::string::npos) {
            try { rec.fill_quantity =
                static_cast<std::uint64_t>(std::stoull(std::string(msg.substr(qty_tag.pos, qty_tag.len))));
            } catch (...) {}
        }

        rec.ack_ts_ns = velocity::time::monotonic_ns();
        if (!ack_q_.push(rec)) {
            dropped_acks_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    std::string                                  host_;
    std::uint16_t                                port_;
    std::string                                  sender_;
    std::string                                  target_;
    std::string                                  symbol_;
    std::uint32_t                                price_scale_;
    int                                          sockfd_{-1};
    std::atomic<std::uint64_t>                   next_seq_{1};
    std::atomic<std::uint64_t>                   sent_orders_{0};
    std::atomic<std::uint64_t>                   send_errors_{0};
    std::atomic<std::uint64_t>                   dropped_acks_{0};
    AckCallback                                  ack_cb_;
    std::atomic<bool>                            stop_{false};
    std::thread                                  reader_;
    boost::lockfree::spsc_queue<AckRecord>       ack_q_;
};

#else   // !VELOCITY_FIX_POSIX  — stub for non-POSIX dev builds.

class FixTransport final : public Transport {
public:
    FixTransport(std::string host, std::uint16_t port,
                 std::string /*sender*/, std::string /*target*/,
                 std::string /*symbol*/, std::uint32_t /*price_scale*/) {
        VLOG_WARN("FIX transport stub: posix sockets unavailable; host={}:{}", host, port);
    }
    auto send(std::uint64_t, const Decision&) -> bool override { return false; }
    auto poll(int) -> void override {}
    auto set_ack_callback(AckCallback cb) -> void override { ack_ = std::move(cb); }
private:
    AckCallback ack_;
};

#endif

}  // namespace

auto make_fix_transport(std::string host, std::uint16_t port,
                        std::string sender_comp_id,
                        std::string target_comp_id,
                        std::string symbol,
                        std::uint32_t price_scale)
    -> std::unique_ptr<Transport> {
    return std::make_unique<FixTransport>(std::move(host), port,
                                          std::move(sender_comp_id),
                                          std::move(target_comp_id),
                                          std::move(symbol),
                                          price_scale);
}

}  // namespace velocity::bot_worker
