// =============================================================================
//  signals.cpp — graceful-shutdown signal handlers.
//
//  The handler is intentionally minimal: it sets a flag and returns.
//  Application code polls the flag at convenient moments (between RPC
//  servicing iterations, after a poll() wakeup, etc.). We never do real
//  work inside the signal context because doing so is undefined for most
//  C++ operations.
// =============================================================================

#include "velocity/common/signals.h"

#include <atomic>
#include <csignal>
#include <mutex>

namespace velocity::signals {
namespace {

std::atomic<bool> g_shutdown{false};
std::once_flag g_installed;

extern "C" auto handler(int /*sig*/) -> void {
    g_shutdown.store(true, std::memory_order_release);
}

}  // namespace

auto install_shutdown() -> void {
    std::call_once(g_installed, []() {
        struct sigaction sa{};
        sa.sa_handler = &handler;
        sa.sa_flags   = 0;
        ::sigemptyset(&sa.sa_mask);
        // SIGTERM is what Kubernetes sends before SIGKILL.
        ::sigaction(SIGTERM, &sa, nullptr);
        // SIGINT for ctrl-C in dev.
        ::sigaction(SIGINT,  &sa, nullptr);
        // SIGPIPE is the noise floor of network programming — ignore.
        struct sigaction ignore{};
        ignore.sa_handler = SIG_IGN;
        ::sigaction(SIGPIPE, &ignore, nullptr);
    });
}

auto shutdown_requested() noexcept -> bool {
    return g_shutdown.load(std::memory_order_acquire);
}

auto shutdown_flag() noexcept -> std::atomic<bool>& {
    return g_shutdown;
}

}  // namespace velocity::signals
