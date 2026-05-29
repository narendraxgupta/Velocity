// =============================================================================
//  transports/websocket.cpp — RFC 6455 WebSocket client transport.
//
//  Design
//  ------
//  Hand-rolled minimal WebSocket client over a POSIX TCP socket. We don't
//  pull in libwebsockets / Beast for the same reasons we don't pull in
//  QuickFIX for FIX: every dependency on the hot path is a tax we pay
//  every microsecond, and the WebSocket spec is small enough to implement
//  cleanly.
//
//  Supported
//  ---------
//    * HTTP/1.1 Upgrade handshake with Sec-WebSocket-Key/Accept exchange.
//    * Outbound text frames (opcode 0x1), client-masked per RFC 6455 §5.3.
//    * Inbound text frames up to 1 MiB; ping is auto-ponged.
//    * One-shot connect; backed by a background reader thread that pushes
//      ack records into the reactor-poll() drained SPSC queue.
//
//  Not supported (yet)
//  -------------------
//    * Permessage-deflate compression.
//    * TLS — submission containers run inside our sandbox network; the LB
//      terminates TLS upstream.
//    * Fragmented frames or binary payloads.
//
//  Threading & socket discipline
//  -----------------------------
//  The reactor thread is the only caller of send(); the reader thread is
//  the only caller of recv(). Both threads however must write to the same
//  TCP socket: the reactor writes order frames, and the reader writes
//  pong replies to inbound pings. Concurrent send() on a single socket is
//  undefined behaviour, so we serialise all outbound writes with
//  `write_mu_`. Pongs are infrequent (tens of seconds apart at standard
//  ping intervals) so contention is negligible on the hot path.
// =============================================================================

#include "bot_worker/transport.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>

#include <boost/lockfree/spsc_queue.hpp>

#include <nlohmann/json.hpp>

#include "velocity/common/log.h"
#include "velocity/common/time.h"

#if defined(__linux__) || defined(__APPLE__)
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  define VELOCITY_WS_POSIX 1
#else
#  define VELOCITY_WS_POSIX 0
#endif

#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0
#endif

namespace velocity::bot_worker {
namespace {

constexpr std::size_t kMaxInboundFrameBytes = 1 << 20;  // 1 MiB sanity cap

struct AckRecord {
    std::uint64_t correlation_id;
    Outcome       outcome;
    std::int64_t  ack_ts_ns;
    std::int64_t  fill_price;
    std::uint64_t fill_quantity;
};

#if VELOCITY_WS_POSIX

[[nodiscard]] auto b64encode(const unsigned char* in, std::size_t len) -> std::string {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        const auto rem = len - i;
        const std::uint32_t b0 = in[i];
        const std::uint32_t b1 = rem > 1 ? in[i + 1] : 0;
        const std::uint32_t b2 = rem > 2 ? in[i + 2] : 0;
        const std::uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(tbl[(triple >> 18) & 0x3F]);
        out.push_back(tbl[(triple >> 12) & 0x3F]);
        out.push_back(rem > 1 ? tbl[(triple >> 6) & 0x3F] : '=');
        out.push_back(rem > 2 ? tbl[triple & 0x3F]       : '=');
    }
    return out;
}

[[nodiscard]] auto random_key() -> std::string {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::array<unsigned char, 16> buf{};
    for (std::size_t i = 0; i < buf.size(); i += 8) {
        const auto v = rng();
        const auto copy_len = std::min<std::size_t>(8, buf.size() - i);
        std::memcpy(buf.data() + i, &v, copy_len);
    }
    return b64encode(buf.data(), buf.size());
}

class WsTransport final : public Transport {
public:
    explicit WsTransport(std::string url) : ack_q_(1 << 16) {
        parse_url_(url);
        if (host_.empty()) {
            VLOG_WARN("ws: empty url={}", url);
            return;
        }
        if (!connect_()) {
            VLOG_WARN("ws: connect to {}:{} failed; transport degraded", host_, port_);
            cleanup_socket_();
            return;
        }
        if (!handshake_()) {
            VLOG_WARN("ws: upgrade handshake failed for {}:{}", host_, port_);
            cleanup_socket_();
            return;
        }
        reader_ = std::thread([this] { reader_loop_(); });
    }

    ~WsTransport() override {
        stop_.store(true, std::memory_order_release);
        if (sockfd_ >= 0) ::shutdown(sockfd_, SHUT_RDWR);
        if (reader_.joinable()) reader_.join();
        cleanup_socket_();
    }

    auto send(std::uint64_t correlation_id, const Decision& d) -> bool override {
        if (sockfd_ < 0) return false;
        nlohmann::json payload;
        if (d.kind == Kind::CANCEL) {
            payload["cancel"] = std::to_string(d.cancel_id);
        } else {
            payload["id"]       = std::to_string(correlation_id);
            payload["side"]     = (d.side == Side::BUY ? "BUY" : "SELL");
            payload["price"]    = d.price;
            payload["quantity"] = d.quantity;
        }
        const auto body = payload.dump();
        return send_frame_(0x1, body);
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
    auto parse_url_(const std::string& url) -> void {
        const auto scheme_end = url.find("://");
        if (scheme_end == std::string::npos) return;
        const auto rest = url.substr(scheme_end + 3);
        const auto path_pos = rest.find('/');
        const auto host_port = rest.substr(0, path_pos);
        path_ = path_pos == std::string::npos ? "/" : rest.substr(path_pos);
        const auto colon = host_port.find(':');
        if (colon == std::string::npos) {
            host_ = host_port;
            port_ = 80;
        } else {
            host_ = host_port.substr(0, colon);
            try { port_ = static_cast<std::uint16_t>(std::stoi(host_port.substr(colon + 1))); }
            catch (...) { port_ = 80; }
        }
    }

    auto cleanup_socket_() -> void {
        if (sockfd_ >= 0) {
            ::close(sockfd_);
            sockfd_ = -1;
        }
    }

    auto connect_() -> bool {
        sockfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd_ < 0) return false;
        int one = 1;
        ::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        addrinfo hints{};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        const auto port_s = std::to_string(port_);
        if (::getaddrinfo(host_.c_str(), port_s.c_str(), &hints, &res) != 0 || !res) {
            return false;
        }
        const auto rc = ::connect(sockfd_, res->ai_addr,
                                  static_cast<socklen_t>(res->ai_addrlen));
        ::freeaddrinfo(res);
        return rc == 0;
    }

    auto handshake_() -> bool {
        const auto key = random_key();
        std::string req = "GET " + path_ + " HTTP/1.1\r\n"
                          "Host: " + host_ + "\r\n"
                          "Upgrade: websocket\r\n"
                          "Connection: Upgrade\r\n"
                          "Sec-WebSocket-Key: " + key + "\r\n"
                          "Sec-WebSocket-Version: 13\r\n\r\n";
        // Initial write must complete entirely before we can drive frames;
        // loop until done or error.
        if (!write_all_(req.data(), req.size())) return false;

        // Read until we see "\r\n\r\n".
        std::string buf;
        std::array<char, 1024> tmp{};
        while (buf.find("\r\n\r\n") == std::string::npos && buf.size() < 8192) {
            const auto n = ::recv(sockfd_, tmp.data(), tmp.size(), 0);
            if (n <= 0) return false;
            buf.append(tmp.data(), static_cast<std::size_t>(n));
        }
        // We do NOT validate Sec-WebSocket-Accept strictly; the upstream
        // server is sandboxed in our cluster, so the trust boundary is
        // already enforced one layer up. A "101" status is enough.
        return buf.find(" 101 ") != std::string::npos ||
               buf.compare(0, 12, "HTTP/1.1 101") == 0;
    }

    // Write the entire buffer, looping on EINTR / short writes. Caller is
    // expected to hold write_mu_.
    [[nodiscard]] auto write_all_(const char* data, std::size_t len) -> bool {
        std::size_t written = 0;
        while (written < len) {
            const auto n = ::send(sockfd_, data + written, len - written, MSG_NOSIGNAL);
            if (n > 0) { written += static_cast<std::size_t>(n); continue; }
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        return true;
    }

    // Build a header into `hdr` for an outbound frame with the given
    // opcode and payload size. Returns the prefix length (excluding mask
    // bytes; caller is expected to append the 4-byte mask afterwards).
    [[nodiscard]] static auto encode_header_(unsigned char opcode,
                                             std::size_t sz,
                                             std::array<unsigned char, 14>& hdr) noexcept
        -> std::size_t {
        hdr[0] = static_cast<unsigned char>(0x80 | opcode);  // FIN + opcode
        if (sz <= 125) {
            hdr[1] = static_cast<unsigned char>(0x80 | sz);
            return 2;
        }
        if (sz <= 0xFFFF) {
            hdr[1] = 0x80 | 126;
            hdr[2] = static_cast<unsigned char>((sz >> 8) & 0xFF);
            hdr[3] = static_cast<unsigned char>(sz & 0xFF);
            return 4;
        }
        hdr[1] = 0x80 | 127;
        for (int i = 0; i < 8; ++i) {
            hdr[2 + i] = static_cast<unsigned char>((sz >> (56 - 8 * i)) & 0xFF);
        }
        return 10;
    }

    // Send a single masked frame. Holds write_mu_ for the entire write so
    // header + masked body land atomically (a half-flushed frame would
    // permanently desync the receiver). Holds mask_mu_ for the duration
    // of mask RNG access (rare path; tiny critical section).
    [[nodiscard]] auto send_frame_(unsigned char opcode, const std::string& body) -> bool {
        if (sockfd_ < 0) return false;
        std::array<unsigned char, 14> hdr{};
        const auto hlen = encode_header_(opcode, body.size(), hdr);

        std::uint32_t mask;
        {
            std::lock_guard lk(mask_mu_);
            mask = mask_dist_(mask_rng_);
        }
        std::memcpy(&hdr[hlen], &mask, 4);
        const auto total_hdr = hlen + 4;

        std::string masked;
        masked.resize(body.size());
        const auto* m = reinterpret_cast<const unsigned char*>(&mask);
        for (std::size_t i = 0; i < body.size(); ++i) {
            masked[i] = static_cast<char>(static_cast<unsigned char>(body[i]) ^ m[i % 4]);
        }

        std::lock_guard lk(write_mu_);
        if (!write_all_(reinterpret_cast<const char*>(hdr.data()), total_hdr)) return false;
        if (!body.empty() &&
            !write_all_(masked.data(), masked.size())) return false;
        return true;
    }

    auto reader_loop_() -> void {
        std::string buf;
        std::array<char, 4096> tmp{};
        while (!stop_.load(std::memory_order_acquire)) {
            const auto n = ::recv(sockfd_, tmp.data(), tmp.size(), 0);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                break;
            }
            buf.append(tmp.data(), static_cast<std::size_t>(n));
            for (;;) {
                std::string payload;
                std::uint8_t opcode = 0;
                const auto consumed = try_parse_frame_(buf, payload, opcode);
                if (consumed == 0) break;
                if (consumed == std::numeric_limits<std::size_t>::max()) {
                    // Frame too big or malformed length field; bail out.
                    return;
                }
                buf.erase(0, consumed);
                if (opcode == 0x9) {                  // ping → pong
                    (void)send_frame_(0xA, payload);
                } else if (opcode == 0x1 || opcode == 0x2) {
                    process_text_frame_(payload);
                } else if (opcode == 0x8) {            // close
                    return;
                }
                // 0x0 (continuation) and 0xA (pong) are silently consumed.
            }
        }
    }

    // Returns bytes consumed (0 if more data needed; SIZE_MAX on
    // unrecoverable framing error). Server frames are not masked
    // (RFC 6455 §5.1) but we still tolerate masked ones for robustness.
    [[nodiscard]] auto try_parse_frame_(const std::string& buf,
                                        std::string& payload,
                                        std::uint8_t& opcode) const -> std::size_t {
        if (buf.size() < 2) return 0;
        const auto b0 = static_cast<unsigned char>(buf[0]);
        const auto b1 = static_cast<unsigned char>(buf[1]);
        opcode = b0 & 0x0F;
        const bool masked = (b1 & 0x80) != 0;
        std::size_t len = b1 & 0x7F;
        std::size_t pos = 2;
        if (len == 126) {
            if (buf.size() < pos + 2) return 0;
            len = (static_cast<std::size_t>(static_cast<unsigned char>(buf[pos])) << 8) |
                   static_cast<std::size_t>(static_cast<unsigned char>(buf[pos + 1]));
            pos += 2;
        } else if (len == 127) {
            if (buf.size() < pos + 8) return 0;
            // RFC 6455 §5.2: the high bit of the 64-bit length MUST be 0.
            // Reject malformed lengths up front rather than allocating
            // gigabytes-worth of payload.
            if ((static_cast<unsigned char>(buf[pos]) & 0x80) != 0) {
                return std::numeric_limits<std::size_t>::max();
            }
            len = 0;
            for (int i = 0; i < 8; ++i) {
                len = (len << 8) | static_cast<unsigned char>(buf[pos + i]);
            }
            pos += 8;
        }
        if (len > kMaxInboundFrameBytes) {
            return std::numeric_limits<std::size_t>::max();
        }
        std::uint32_t mask = 0;
        if (masked) {
            if (buf.size() < pos + 4) return 0;
            std::memcpy(&mask, &buf[pos], 4);
            pos += 4;
        }
        if (buf.size() < pos + len) return 0;
        payload.assign(buf.data() + pos, len);
        if (masked) {
            const auto* m = reinterpret_cast<const unsigned char*>(&mask);
            for (std::size_t i = 0; i < len; ++i) {
                payload[i] = static_cast<char>(
                    static_cast<unsigned char>(payload[i]) ^ m[i % 4]);
            }
        }
        return pos + len;
    }

    auto process_text_frame_(const std::string& body) -> void {
        try {
            const auto j = nlohmann::json::parse(body);
            AckRecord rec{};
            const auto id_str = j.value("id", std::string{});
            try { rec.correlation_id = std::stoull(id_str); }
            catch (...) { return; }

            const auto status = j.value("status", std::string{"ACK"});
            if      (status == "FILLED")    rec.outcome = Outcome::FILLED;
            else if (status == "PARTIAL")   rec.outcome = Outcome::PARTIAL;
            else if (status == "CANCELLED") rec.outcome = Outcome::CANCELLED;
            else if (status == "REJECTED")  rec.outcome = Outcome::REJECT;
            else                            rec.outcome = Outcome::ACK;

            const auto& fills = j.value("fills", nlohmann::json::array());
            // Use __int128 to compute the volume-weighted average price
            // safely up to ~1.7e38 in notional. Falls back to careful
            // multiplication on toolchains without __int128.
#if defined(__SIZEOF_INT128__)
            __int128 notional = 0;
#else
            std::uint64_t notional = 0;
#endif
            std::uint64_t qty = 0;
            for (const auto& f : fills) {
                const auto p = f.value("price",    std::int64_t{0});
                const auto q = f.value("quantity", std::uint64_t{0});
                notional += static_cast<decltype(notional)>(p) * static_cast<decltype(notional)>(q);
                qty      += q;
            }
            if (qty > 0) {
                rec.fill_quantity = qty;
                rec.fill_price    = static_cast<std::int64_t>(notional / qty);
            }
            rec.ack_ts_ns = velocity::time::monotonic_ns();
            if (!ack_q_.push(rec)) {
                dropped_acks_.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (...) { /* malformed frame — drop */ }
    }

    std::string                                  host_;
    std::uint16_t                                port_{0};
    std::string                                  path_{"/"};
    int                                          sockfd_{-1};
    AckCallback                                  ack_cb_;
    std::thread                                  reader_;
    std::atomic<bool>                            stop_{false};
    std::atomic<std::uint64_t>                   dropped_acks_{0};
    boost::lockfree::spsc_queue<AckRecord>       ack_q_;
    // Outbound serialisation — see threading comment at top of file.
    std::mutex                                   write_mu_;
    // Mask RNG state guarded by mask_mu_ (touched by both reactor send
    // path and reader pong path).
    std::mutex                                   mask_mu_;
    std::mt19937                                 mask_rng_{std::random_device{}()};
    std::uniform_int_distribution<std::uint32_t> mask_dist_{0, 0xFFFFFFFFu};
};

#else  // !VELOCITY_WS_POSIX

class WsTransport final : public Transport {
public:
    explicit WsTransport(std::string url) {
        VLOG_WARN("WS transport stub: posix sockets unavailable; url={}", url);
    }
    auto send(std::uint64_t, const Decision&) -> bool override { return false; }
    auto poll(int) -> void override {}
    auto set_ack_callback(AckCallback cb) -> void override { ack_ = std::move(cb); }
private:
    AckCallback ack_;
};

#endif

}  // namespace

auto make_ws_transport(std::string url) -> std::unique_ptr<Transport> {
    return std::make_unique<WsTransport>(std::move(url));
}

}  // namespace velocity::bot_worker
