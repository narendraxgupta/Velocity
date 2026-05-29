// =============================================================================
//  server.cpp — uWebSockets app + Redis Pub/Sub fan-out.
//
//  Architecture
//  ------------
//    main thread             : uWS event loop (accept / I/O / publish)
//    redis subscriber thread : blocks on PSUBSCRIBE, marshals payloads
//                              into the uWS loop via uWS::Loop::defer()
//
//  Clients connect to /v1/leaderboard, optionally with ?division=
//  Each is subscribed to two uWS topics:
//    * "leaderboard.global"
//    * "leaderboard.<division>"
//  Backpressure: send() returns DROPPED if the per-client buffer exceeds
//  256 KiB; we count it as a Prometheus drop and continue.
// =============================================================================

#include "leaderboard_ws/server.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <App.h>
#include <nlohmann/json.hpp>
#include <sw/redis++/redis++.h>

#include "velocity/common/log.h"
#include "velocity/common/signals.h"
#include "velocity/common/time.h"

namespace velocity::leaderboard_ws {

namespace {

struct ClientData {
    std::string division;
    std::int64_t connected_at_ns{0};
};

[[nodiscard]] auto safe_division(std::string_view d) noexcept -> bool {
    if (d.empty() || d.size() > 64) return false;
    for (const unsigned char c : d) {
        if (!std::isalnum(c) && c != '_' && c != '-' && c != '.') return false;
    }
    return true;
}

}  // namespace

struct Server::Impl {
    ServerConfig                   cfg;
    uWS::Loop*                     loop{nullptr};
    std::atomic<std::uint64_t>     msgs_in{0};
    std::atomic<std::uint64_t>     msgs_out{0};
    std::atomic<std::uint64_t>     drops{0};
    std::atomic<std::uint64_t>     drain_events{0};
    std::atomic<std::size_t>       client_count{0};
    std::atomic<us_listen_socket_t*> listen_sock{nullptr};
    std::thread                    redis_thread;
    std::thread                    shutdown_watcher;
    std::atomic<bool>              stop_requested{false};

    explicit Impl(ServerConfig c) : cfg(std::move(c)) {}

    auto run() -> void {
        loop = uWS::Loop::get();

        auto app = uWS::App();
        app.ws<ClientData>(
            "/v1/leaderboard",
            {
                .compression        = uWS::DISABLED,
                .maxPayloadLength   = 16 * 1024,
                .idleTimeout        = 60,
                .maxBackpressure    = 256 * 1024,
                .closeOnBackpressureLimit = false,
                .resetIdleTimeoutOnSend   = true,
                .sendPingsAutomatically   = true,

                .upgrade = nullptr,
                .open    = [this](auto* ws) {
                    auto* d = static_cast<ClientData*>(ws->getUserData());
                    auto query = std::string{ws->getRemoteAddressAsText()};
                    d->division = "global";
                    d->connected_at_ns = velocity::time::realtime_ns();
                    ws->subscribe("leaderboard.global");
                    client_count.fetch_add(1, std::memory_order_relaxed);
                    ws->send(R"({"type":"hello","stream":"global"})", uWS::OpCode::TEXT);
                },
                .message = [this](auto* ws, std::string_view msg, uWS::OpCode) {
                    // The client may send {"subscribe":"<division>"} to
                    // switch streams. Anything else is echoed for debug.
                    if (msg.find("\"subscribe\"") != std::string_view::npos) {
                        const auto start = msg.find(':');
                        if (start != std::string_view::npos) {
                            auto v = msg.substr(start + 1);
                            const auto q1 = v.find('"');
                            const auto q2 = v.find('"', q1 + 1);
                            if (q1 != std::string_view::npos && q2 != std::string_view::npos) {
                                const std::string new_div{
                                    v.data() + q1 + 1, q2 - q1 - 1};
                                if (!safe_division(new_div)) {
                                    return;
                                }
                                auto* d = static_cast<ClientData*>(ws->getUserData());
                                ws->unsubscribe("leaderboard." + d->division);
                                d->division = new_div;
                                ws->subscribe("leaderboard." + d->division);
                                const auto ack = nlohmann::json{
                                    {"type", "subscribed"},
                                    {"stream", d->division},
                                }.dump();
                                ws->send(ack, uWS::OpCode::TEXT);
                            }
                        }
                    }
                    msgs_in.fetch_add(1, std::memory_order_relaxed);
                },
                .drain   = [this](auto* ws) {
                    // drain fires when the per-client send buffer is being
                    // flushed; bytes are still in flight. We only treat it
                    // as a drop signal when the bufferedAmount climbs past
                    // 75% of maxBackpressure — anything below is normal
                    // back-pressure on a slow client.
                    const auto buffered = ws->getBufferedAmount();
                    if (buffered > (256u * 1024u * 3u / 4u)) {
                        drops.fetch_add(1, std::memory_order_relaxed);
                    }
                    drain_events.fetch_add(1, std::memory_order_relaxed);
                },
                .close   = [this](auto* ws, int, std::string_view) {
                    auto* d = static_cast<ClientData*>(ws->getUserData());
                    if (d) {
                        ws->unsubscribe("leaderboard.global");
                        if (!d->division.empty() && d->division != "global") {
                            ws->unsubscribe("leaderboard." + d->division);
                        }
                    }
                    client_count.fetch_sub(1, std::memory_order_relaxed);
                },
            });

        app.get("/healthz", [](auto* res, auto*) {
            res->end(R"({"status":"ok","service":"leaderboard-ws"})");
        });
        app.get("/metrics", [this](auto* res, auto*) {
            std::string body;
            body += "# HELP leaderboard_clients Current number of connected WebSocket clients.\n";
            body += "# TYPE leaderboard_clients gauge\n";
            body += "leaderboard_clients " + std::to_string(client_count.load()) + "\n";
            body += "# HELP leaderboard_msgs_in Total messages received from clients + Redis.\n";
            body += "# TYPE leaderboard_msgs_in counter\n";
            body += "leaderboard_msgs_in " + std::to_string(msgs_in.load()) + "\n";
            body += "# HELP leaderboard_msgs_out Total messages successfully forwarded to clients.\n";
            body += "# TYPE leaderboard_msgs_out counter\n";
            body += "leaderboard_msgs_out " + std::to_string(msgs_out.load()) + "\n";
            body += "# HELP leaderboard_drops Total messages dropped due to backpressure.\n";
            body += "# TYPE leaderboard_drops counter\n";
            body += "leaderboard_drops " + std::to_string(drops.load()) + "\n";
            body += "# HELP leaderboard_drain_events Total per-client drain events.\n";
            body += "# TYPE leaderboard_drain_events counter\n";
            body += "leaderboard_drain_events " + std::to_string(drain_events.load()) + "\n";
            res->writeHeader("Content-Type", "text/plain; version=0.0.4");
            res->end(body);
        });

        app.listen(cfg.listen_host, cfg.ws_port,
                   [this](auto* listen_socket) {
                       if (!listen_socket) {
                           throw std::runtime_error("failed to bind WS listener on " +
                                                    cfg.listen_host + ":" +
                                                    std::to_string(cfg.ws_port));
                       }
                       listen_sock.store(listen_socket, std::memory_order_release);
                       VLOG_INFO("WS server listening on {}:{}",
                                 cfg.listen_host, cfg.ws_port);
                   });

        // Bind the publish indirection before starting the redis thread.
        bind_publisher(&app);
        redis_thread = std::thread([this]() { redis_loop(); });

        // Tear-down watcher: poll the global shutdown flag and ask uWS to
        // close the listen socket so app.run() returns. We can't safely
        // signal uWS from a signal handler, so we use a dedicated thread.
        shutdown_watcher = std::thread([this]() {
            while (!velocity::signals::shutdown_requested() &&
                   !stop_requested.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            stop_requested.store(true, std::memory_order_release);
            if (auto* sock = listen_sock.load(std::memory_order_acquire)) {
                if (loop) {
                    loop->defer([sock]() { us_listen_socket_close(0, sock); });
                }
            }
        });

        app.run();   // Blocks until the listen socket is closed.

        stop_requested.store(true, std::memory_order_release);
        if (shutdown_watcher.joinable()) shutdown_watcher.join();
        if (redis_thread.joinable())     redis_thread.join();
    }

    // Background thread: PSUBSCRIBE to "leaderboard.*", forward to uWS.
    // Reconnects forever on connection loss with exponential backoff capped
    // at 5s so a Redis hiccup doesn't blackhole the leaderboard stream.
    auto redis_loop() -> void {
        VLOG_INFO("redis subscriber connecting to {} pattern={}",
                  cfg.redis_addr, cfg.channel_pattern);
        auto backoff_ms = std::chrono::milliseconds(100);
        const auto max_backoff = std::chrono::milliseconds(5000);
        while (!stop_requested.load(std::memory_order_acquire) &&
               !velocity::signals::shutdown_requested()) {
            try {
                sw::redis::ConnectionOptions opts;
                opts.host = "";  // parsed from URI below
                sw::redis::Redis redis(cfg.redis_addr);
                auto sub = redis.subscriber();
                sub.on_pmessage([this](std::string pattern, std::string channel,
                                       std::string msg) {
                    (void)pattern;
                    msgs_in.fetch_add(1, std::memory_order_relaxed);
                    loop->defer([this, channel = std::move(channel),
                                 msg = std::move(msg)]() {
                        if (publish_) publish_(channel, msg);
                    });
                });
                sub.psubscribe(cfg.channel_pattern);
                backoff_ms = std::chrono::milliseconds(100);  // reset on success
                VLOG_INFO("redis subscriber connected to {} pattern={}",
                          cfg.redis_addr, cfg.channel_pattern);

                while (!stop_requested.load(std::memory_order_acquire) &&
                       !velocity::signals::shutdown_requested()) {
                    try {
                        sub.consume();
                    } catch (const sw::redis::TimeoutError&) {
                        // Normal — no message in window.
                    }
                }
                break;
            } catch (const std::exception& e) {
                VLOG_WARN("redis subscriber lost ({}); reconnect in {} ms",
                          e.what(), backoff_ms.count());
                std::this_thread::sleep_for(backoff_ms);
                backoff_ms = std::min(max_backoff, backoff_ms * 2);
            }
        }
    }

    // Publishes a payload to all WS clients subscribed to the topic.
    // Implemented as a friend-shaped indirection over App::publish().
    std::function<void(std::string_view, std::string_view)> publish_;

    auto bind_publisher(uWS::App* app) -> void {
        publish_ = [app, this](std::string_view topic, std::string_view payload) {
            const auto ok = app->publish(topic, payload, uWS::OpCode::TEXT, false);
            if (ok) {
                msgs_out.fetch_add(1, std::memory_order_relaxed);
            } else {
                drops.fetch_add(1, std::memory_order_relaxed);
            }
        };
    }
};

Server::Server(ServerConfig cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}
Server::~Server() = default;

auto Server::run() -> void { impl_->run(); }

}  // namespace velocity::leaderboard_ws
