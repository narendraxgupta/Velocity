// =============================================================================
//  velocity/common/log.h
//
//  Thin, opinionated wrapper around spdlog. Every service initializes the
//  global logger exactly once at startup with `init_logging(...)` and then
//  uses the `VLOG_*` macros. Logs are emitted as one-line JSON, which the
//  Docker / Kubernetes log driver ships into Loki / Cloud Logging without
//  further parsing.
//
//  Why a wrapper instead of spdlog directly?
//    * Locks in a single line format across services.
//    * Centralizes log-level resolution from the VELOCITY_LOG_LEVEL env var.
//    * Adds a "service" tag automatically so multi-service log streams stay
//      filterable.
// =============================================================================

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

namespace velocity::log {

// Initialize the global logger.
//
// `service` becomes a structured field on every log line. Call once during
// service startup; subsequent calls are no-ops. Reading VELOCITY_LOG_LEVEL
// is done here so callers don't need to.
auto init(std::string service) -> void;

// Default global logger. After `init()` this is the only one you need.
[[nodiscard]] auto get() -> std::shared_ptr<spdlog::logger>;

// Set a key=value pair that will be attached to every subsequent log line
// from this thread until cleared.
auto push_thread_context(std::string_view key, std::string_view value) -> void;
auto pop_thread_context(std::string_view key) -> void;

}  // namespace velocity::log

// -----------------------------------------------------------------------------
//  Convenience macros — never call ::get() directly in user code.
// -----------------------------------------------------------------------------
#define VLOG_TRACE(...) SPDLOG_LOGGER_TRACE(::velocity::log::get(), __VA_ARGS__)
#define VLOG_DEBUG(...) SPDLOG_LOGGER_DEBUG(::velocity::log::get(), __VA_ARGS__)
#define VLOG_INFO(...)  SPDLOG_LOGGER_INFO(::velocity::log::get(),  __VA_ARGS__)
#define VLOG_WARN(...)  SPDLOG_LOGGER_WARN(::velocity::log::get(),  __VA_ARGS__)
#define VLOG_ERROR(...) SPDLOG_LOGGER_ERROR(::velocity::log::get(), __VA_ARGS__)
#define VLOG_FATAL(...) SPDLOG_LOGGER_CRITICAL(::velocity::log::get(), __VA_ARGS__)
