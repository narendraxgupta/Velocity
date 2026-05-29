// =============================================================================
//  transports/rest.cpp — REST transport over libcurl-multi.
//
//  Why libcurl-multi and not a hand-rolled io_uring HTTP client?
//
//    * Correctness first. libcurl gets HTTP/1.1 keep-alive, redirects, and
//      timeouts right; reproducing them in 2,000 lines of io_uring is a
//      Phase 3 optimization, not a Phase 2 deliverable.
//    * libcurl's multi handle is fully non-blocking via curl_multi_socket_action
//      and easily wired into the reactor's event loop.
//    * At the throughputs we run (~200k RPS per worker), libcurl is not the
//      bottleneck — the submission's matching engine is.
//
//  Replacement strategy: when we ship Phase 3, the io_uring variant lives
//  in `rest_uring.cpp` and gets selected at construction time by a
//  feature-detected `make_rest_transport`.
// =============================================================================

#include "bot_worker/transport.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "velocity/common/log.h"
#include "velocity/common/time.h"

namespace velocity::bot_worker {
namespace {

[[nodiscard]] auto side_str(Side s) noexcept -> const char* {
    return s == Side::BUY ? "BUY" : "SELL";
}

struct InFlight {
    CURL*         easy{nullptr};
    std::uint64_t correlation_id{0};
    std::int64_t  sent_ts_ns{0};
    std::string   buf;
    std::string   body;
    // We need to keep the URL alive for libcurl's lifetime of the easy handle.
    std::string   url;
    Kind          kind{Kind::NEW};
};

extern "C" auto write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) -> std::size_t {
    auto* f = static_cast<InFlight*>(userdata);
    const auto bytes = size * nmemb;
    f->buf.append(ptr, bytes);
    return bytes;
}

class RestTransport final : public Transport {
public:
    RestTransport(std::string host, std::uint16_t port, std::uint32_t connection_count)
        : base_url_("http://" + host + ":" + std::to_string(port)),
          connection_count_(connection_count) {
        multi_ = curl_multi_init();
        if (!multi_) throw std::runtime_error("curl_multi_init failed");
        // Cap the number of concurrent connections to a small constant —
        // submissions typically expose a single HTTP/1.1 server; more
        // connections only help until OS socket buffers saturate.
        curl_multi_setopt(multi_, CURLMOPT_MAX_TOTAL_CONNECTIONS,
                          static_cast<long>(connection_count_));
        curl_multi_setopt(multi_, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
    }

    ~RestTransport() override {
        for (auto& [_, f] : inflight_) {
            curl_multi_remove_handle(multi_, f->easy);
            curl_easy_cleanup(f->easy);
        }
        if (multi_) curl_multi_cleanup(multi_);
    }

    auto send(std::uint64_t correlation_id, const Decision& d) -> bool override {
        auto f = std::make_unique<InFlight>();
        f->correlation_id = correlation_id;
        f->sent_ts_ns     = velocity::time::monotonic_ns();
        f->kind           = d.kind;
        f->easy           = curl_easy_init();
        if (!f->easy) return false;

        // setopt failure mid-build means the handle is unusable. Track
        // any non-OK return so we tear down gracefully instead of issuing
        // half-configured requests.
        CURLcode rc = CURLE_OK;
        const auto set = [&](CURLoption opt, auto value) {
            if (rc == CURLE_OK) rc = curl_easy_setopt(f->easy, opt, value);
        };

        if (d.kind == Kind::CANCEL) {
            f->url = base_url_ + "/orders/" + std::to_string(d.cancel_id);
            set(CURLOPT_URL, f->url.c_str());
            set(CURLOPT_CUSTOMREQUEST, "DELETE");
        } else {
            f->url  = base_url_ + "/orders";
            f->body = nlohmann::json{
                {"id",       std::to_string(correlation_id)},
                {"side",     side_str(d.side)},
                {"price",    d.price},
                {"quantity", d.quantity},
            }.dump();
            set(CURLOPT_URL,        f->url.c_str());
            set(CURLOPT_POST,       1L);
            set(CURLOPT_POSTFIELDS, f->body.c_str());
            set(CURLOPT_POSTFIELDSIZE, static_cast<long>(f->body.size()));
            set(CURLOPT_HTTPHEADER, headers_());
        }
        set(CURLOPT_WRITEFUNCTION, &write_cb);
        set(CURLOPT_WRITEDATA,     f.get());
        set(CURLOPT_TIMEOUT_MS,    500L);
        set(CURLOPT_CONNECTTIMEOUT_MS, 200L);
        set(CURLOPT_TCP_NODELAY,   1L);
        set(CURLOPT_NOSIGNAL,      1L);
        // Encode the correlation id on the handle so we can recover it
        // when the completion fires.
        set(CURLOPT_PRIVATE,       f.get());

        if (rc != CURLE_OK) {
            VLOG_WARN("rest: curl_easy_setopt failed ({}) — dropping request",
                      curl_easy_strerror(rc));
            curl_easy_cleanup(f->easy);
            return false;
        }

        const auto code = curl_multi_add_handle(multi_, f->easy);
        if (code != CURLM_OK) {
            curl_easy_cleanup(f->easy);
            return false;
        }
        inflight_[correlation_id] = std::move(f);
        return true;
    }

    auto poll(int /*max_events*/) -> void override {
        int still_running = 0;
        curl_multi_perform(multi_, &still_running);

        // Drain completions.
        for (;;) {
            int msgs_in_queue = 0;
            CURLMsg* msg = curl_multi_info_read(multi_, &msgs_in_queue);
            if (!msg) break;
            if (msg->msg != CURLMSG_DONE) continue;

            CURL* easy = msg->easy_handle;
            InFlight* f = nullptr;
            curl_easy_getinfo(easy, CURLINFO_PRIVATE, &f);
            // Defensively bail if we cannot recover the InFlight*: leaving
            // the handle attached would loop forever; cleaning it up
            // unblocks the multi loop.
            if (!f) {
                curl_multi_remove_handle(multi_, easy);
                curl_easy_cleanup(easy);
                continue;
            }

            const auto now_ns = velocity::time::monotonic_ns();
            Outcome     outcome     = Outcome::UNKNOWN;
            std::int64_t fill_price = 0;
            std::uint64_t fill_qty  = 0;

            long http_code = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_code);

            if (msg->data.result != CURLE_OK) {
                outcome = (msg->data.result == CURLE_OPERATION_TIMEDOUT)
                              ? Outcome::TIMEOUT
                              : Outcome::REJECT;
            } else if (http_code == 204) {
                outcome = Outcome::CANCELLED;
            } else if (http_code >= 200 && http_code < 300) {
                outcome = parse_fills_(f->buf, fill_price, fill_qty);
            } else {
                outcome = Outcome::REJECT;
            }

            const auto cid = f->correlation_id;
            if (ack_cb_) {
                ack_cb_(cid, outcome, now_ns, fill_price, fill_qty);
            }

            curl_multi_remove_handle(multi_, easy);
            curl_easy_cleanup(easy);
            inflight_.erase(cid);
        }
    }

    auto set_ack_callback(AckCallback cb) -> void override { ack_cb_ = std::move(cb); }

private:
    static auto headers_() -> curl_slist* {
        static std::once_flag once;
        static curl_slist* h = nullptr;
        std::call_once(once, []() {
            h = curl_slist_append(nullptr, "Content-Type: application/json");
            h = curl_slist_append(h,       "Expect:");
            h = curl_slist_append(h,       "User-Agent: velocity-bot/1.0");
        });
        return h;
    }

    // Parse the sample-exchange response shape:
    //   { "fills": [...], "resting_quantity": N }
    static auto parse_fills_(const std::string& body,
                             std::int64_t& fill_price,
                             std::uint64_t& fill_qty) -> Outcome {
        if (body.empty()) return Outcome::ACK;
        try {
            const auto j = nlohmann::json::parse(body);
            const auto& fills = j.value("fills", nlohmann::json::array());
            if (fills.empty()) {
                // No fills returned — request was accepted and either rests
                // on the book (resting_quantity > 0) or was a cancel-ack.
                // We treat both as ACK; the validator decides correctness.
                return Outcome::ACK;
            }
            // Volume-weighted average price across the fills array. Use a
            // 128-bit accumulator where the toolchain provides one so we
            // don't silently overflow at high notionals (a single 1e9
            // price * 1e10 quantity already saturates uint64_t).
#if defined(__SIZEOF_INT128__)
            __int128 notional = 0;
#else
            std::uint64_t notional = 0;
#endif
            std::uint64_t qty = 0;
            for (const auto& fill : fills) {
                const auto p = fill.value("price",    std::int64_t{0});
                const auto q = fill.value("quantity", std::uint64_t{0});
                notional += static_cast<decltype(notional)>(p) *
                            static_cast<decltype(notional)>(q);
                qty      += q;
            }
            if (qty > 0) {
                fill_qty   = qty;
                fill_price = static_cast<std::int64_t>(notional / qty);
            }
            const auto resting = j.value("resting_quantity", 0ULL);
            return resting == 0 ? Outcome::FILLED : Outcome::PARTIAL;
        } catch (...) {
            return Outcome::REJECT;
        }
    }

    std::string                                                base_url_;
    std::uint32_t                                              connection_count_;
    CURLM*                                                     multi_{nullptr};
    std::unordered_map<std::uint64_t, std::unique_ptr<InFlight>> inflight_;
    AckCallback                                                ack_cb_;
};

}  // namespace

auto make_rest_transport(std::string host, std::uint16_t port,
                         std::uint32_t connection_count)
    -> std::unique_ptr<Transport> {
    return std::make_unique<RestTransport>(std::move(host), port, connection_count);
}

}  // namespace velocity::bot_worker
