// =============================================================================
//  questdb_writer.cpp — ILP-over-TCP writer for QuestDB ≥ 7.x.
//
//  Why TCP and not UDP?
//  --------------------
//  QuestDB removed ILP/UDP support starting with v7.0 (Q1 2023). TCP is the
//  only supported on-wire ILP transport for current releases and is what
//  this writer targets.
//
//  Line format
//  -----------
//    order_events,submission_id=01HQ... outcome=2i,latency_ns=12345i,
//      price=10000i,qty=10i <ns_timestamp>\n
//
//  Notes
//  -----
//    * Tag values (after =) must NOT contain spaces, commas, or quotes.
//    * Field values use `i` suffix for integers, `f` for floats, "..." for
//      strings.
//    * Trailing nanosecond timestamp lets QuestDB partition by event time.
//    * The writer batches lines in a 64 KiB buffer; flush() sends the entire
//      buffer in one writev/send call to amortise syscalls.
//    * On send failure we drop the current batch and lazily reconnect on the
//      next flush — losing batched lines is preferable to blocking the
//      ingester hot path.
// =============================================================================

#include "telemetry_ingester/questdb_writer.h"

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "Ws2_32.lib")
using socklen_t = int;
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include <charconv>
#include <cstring>
#include <stdexcept>

#include "velocity/common/log.h"

namespace velocity::telemetry_ingester {

namespace {

auto close_socket(int fd) noexcept -> void {
#ifdef _WIN32
    closesocket(fd);
#else
    ::close(fd);
#endif
}

}  // namespace

QuestDbWriter::QuestDbWriter(std::string host, std::uint16_t port)
    : host_(std::move(host)), port_(port) {
    buf_.reserve(65536);
    try {
        connect_();
    } catch (const std::exception& ex) {
        // Defer the failure — flush() will keep trying to reconnect so that a
        // transient DNS / startup-race doesn't crash the ingester pod.
        VLOG_WARN("questdb-writer: initial connect failed: {}", ex.what());
    }
}

QuestDbWriter::~QuestDbWriter() {
    try {
        flush();
    } catch (...) {
    }
    if (fd_ >= 0) close_socket(fd_);
}

auto QuestDbWriter::connect_() -> void {
#ifdef _WIN32
    static bool wsa_initialised = false;
    if (!wsa_initialised) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
        wsa_initialised = true;
    }
#endif
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const auto port_str = std::to_string(port_);
    if (getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &res) != 0 || res == nullptr) {
        throw std::runtime_error("getaddrinfo failed for QuestDB host " + host_);
    }

    int fd = -1;
    for (auto* rp = res; rp != nullptr; rp = rp->ai_next) {
        fd = static_cast<int>(socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol));
        if (fd < 0) continue;
        if (::connect(fd, rp->ai_addr, static_cast<socklen_t>(rp->ai_addrlen)) == 0) {
            // Disable Nagle so batched lines hit the wire as soon as flush() runs.
            int yes = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                         reinterpret_cast<const char*>(&yes), sizeof(yes));
            break;
        }
        close_socket(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        throw std::runtime_error("TCP connect failed to QuestDB ILP " + host_ + ":" + port_str);
    }
    fd_ = fd;
}

auto QuestDbWriter::append_order_event(const std::string& submission_id,
                                       std::int64_t latency_ns,
                                       std::int64_t price_units,
                                       std::int64_t qty_units,
                                       std::int32_t outcome,
                                       std::int64_t event_ts_ns) -> void {
    buf_.append("order_events,submission_id=");
    buf_.append(submission_id);
    buf_.append(" outcome=");
    buf_.append(std::to_string(outcome));
    buf_.append("i,latency_ns=");
    buf_.append(std::to_string(latency_ns));
    buf_.append("i,price=");
    buf_.append(std::to_string(price_units));
    buf_.append("i,qty=");
    buf_.append(std::to_string(qty_units));
    buf_.append("i ");
    buf_.append(std::to_string(event_ts_ns));
    buf_.push_back('\n');

    if (buf_.size() >= 32768) flush();
}

auto QuestDbWriter::flush() -> void {
    if (buf_.empty()) return;

    // Lazy reconnect on a previously dropped socket.
    if (fd_ < 0) {
        try {
            connect_();
        } catch (const std::exception&) {
            ++packets_dropped_;
            buf_.clear();
            return;
        }
    }

    const char* data = buf_.data();
    std::size_t remaining = buf_.size();
    while (remaining > 0) {
#ifdef _WIN32
        const auto n = ::send(fd_, data, static_cast<int>(remaining), 0);
#else
        const auto n = ::send(fd_, data, remaining, MSG_NOSIGNAL);
#endif
        if (n <= 0) {
            ++packets_dropped_;
            // Drop the socket so the next flush attempts a fresh connect.
            close_socket(fd_);
            fd_ = -1;
            buf_.clear();
            return;
        }
        data += n;
        remaining -= static_cast<std::size_t>(n);
        bytes_sent_ += static_cast<std::uint64_t>(n);
    }
    buf_.clear();
}

}  // namespace velocity::telemetry_ingester
