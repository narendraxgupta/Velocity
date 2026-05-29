// =============================================================================
//  leaderboard_ws/server.h — public surface of the WebSocket fan-out.
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace velocity::leaderboard_ws {

struct ServerConfig {
    std::string listen_host;
    std::uint16_t ws_port;
    std::uint16_t metrics_port;
    std::string redis_addr;
    std::string channel_pattern;  // default "leaderboard.*"
};

class Server {
public:
    explicit Server(ServerConfig cfg);
    ~Server();

    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    auto run() -> void;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace velocity::leaderboard_ws
