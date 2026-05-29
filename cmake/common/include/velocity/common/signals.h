// =============================================================================
//  velocity/common/signals.h
//
//  Tiny graceful-shutdown helper. Every service follows the same pattern:
//  install a handler for SIGTERM + SIGINT, run until a token flips, then
//  drain. This is the token + installer behind that pattern.
//
//  Usage:
//
//      int main() {
//          velocity::signals::install_shutdown();
//          while (!velocity::signals::shutdown_requested()) {
//              // do work
//          }
//          // graceful cleanup
//      }
// =============================================================================

#pragma once

#include <atomic>

namespace velocity::signals {

// Installs SIGTERM + SIGINT handlers that flip the shutdown token. Safe to
// call multiple times; subsequent calls are no-ops. Returns immediately.
auto install_shutdown() -> void;

// True once a shutdown signal has been received. Reads are lock-free and
// safe from any thread.
[[nodiscard]] auto shutdown_requested() noexcept -> bool;

// Returns the underlying atomic flag for callers that want to use it as a
// stop-token directly (e.g., to wake up condition variables on shutdown).
[[nodiscard]] auto shutdown_flag() noexcept -> std::atomic<bool>&;

}  // namespace velocity::signals
