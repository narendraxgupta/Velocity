// =============================================================================
//  api_gateway/server.h — HTTP/WebSocket facade.
//
//  Owns the Drogon app object's lifecycle and exposes the small surface our
//  main() needs. Keeping Drogon types out of main.cpp lets us swap the HTTP
//  layer later without touching the entrypoint.
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace velocity::api_gateway {

struct ServerConfig {
    std::string listen_host;
    std::uint16_t http_port;
    std::uint16_t metrics_port;
    std::string submission_engine_grpc;  // host:port
    std::string bot_controller_grpc;     // host:port
    std::string redis_addr;
    std::string minio_endpoint;
    std::string minio_access_key;
    std::string minio_secret_key;
};

class Server {
public:
    explicit Server(ServerConfig cfg);
    ~Server();

    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&)                 = delete;
    Server& operator=(Server&&)      = delete;

    // Blocks until shutdown is requested.
    auto run() -> void;

    // Signals the server to stop from any thread.
    auto stop() noexcept -> void;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace velocity::api_gateway
