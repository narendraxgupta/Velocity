// =============================================================================
//  velocity-sample-exchange — a minimal reference matching engine.
//
//  Submit this as a sample submission to verify the platform end-to-end.
//  It implements just enough of an exchange to be benchmarked:
//
//    POST /orders         place a limit/market order      → returns ack + id
//    DELETE /orders/{id}  cancel
//    GET /book            full snapshot (for debugging)
//    GET /healthz         health probe
//
//  The implementation is intentionally simple and readable, not fast.
// =============================================================================

#include <cstdlib>
#include <exception>

#include "velocity/common/env.h"
#include "velocity/common/log.h"
#include "velocity/common/signals.h"
#include "velocity/common/time.h"

namespace velocity::sample_exchange {
auto run(std::string listen_host, std::uint16_t listen_port) -> void;
}

auto main(int /*argc*/, char* /*argv*/[]) -> int {
    velocity::log::init("sample-exchange");
    velocity::time::init();
    velocity::signals::install_shutdown();

    try {
        const auto host = velocity::env::optional<std::string>("EXCHANGE_HOST", "0.0.0.0");
        const auto port = velocity::env::optional<std::uint16_t>("EXCHANGE_PORT", 7000);
        VLOG_INFO("velocity-sample-exchange starting on {}:{}", host, port);
        velocity::sample_exchange::run(host, port);
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        VLOG_FATAL("fatal: {}", e.what());
        return EXIT_FAILURE;
    }
}
