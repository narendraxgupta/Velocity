// =============================================================================
//  bot_controller/controller_service.h
//
//  gRPC service implementation for `velocity.bot.v1.BotControl`. Manages
//  the lifecycle of all worker streams and the orchestration of a benchmark
//  across them.
// =============================================================================

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// Forward declarations to keep the heavy gRPC headers out of the .h
namespace velocity::bot::v1 {
class BotControl;
}

namespace velocity::bot_controller {

struct ControllerConfig {
    std::string listen_host;
    std::uint16_t grpc_port;
    std::uint16_t metrics_port;
};

class ControllerService {
public:
    explicit ControllerService(ControllerConfig cfg);
    ~ControllerService();

    ControllerService(const ControllerService&)            = delete;
    ControllerService& operator=(const ControllerService&) = delete;

    // Block until shutdown.
    auto run() -> void;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace velocity::bot_controller
