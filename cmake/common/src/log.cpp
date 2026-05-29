// =============================================================================
//  log.cpp — global logger initialization.
// =============================================================================

#include "velocity/common/log.h"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace velocity::log {
namespace {

std::shared_ptr<spdlog::logger> g_logger;
std::once_flag g_once;

[[nodiscard]] auto level_from_env() noexcept -> spdlog::level::level_enum {
    const char* raw = std::getenv("VELOCITY_LOG_LEVEL");
    if (raw == nullptr) {
        return spdlog::level::info;
    }
    std::string v{raw};
    for (auto& c : v) c = static_cast<char>(std::tolower(c));
    if (v == "trace") return spdlog::level::trace;
    if (v == "debug") return spdlog::level::debug;
    if (v == "info")  return spdlog::level::info;
    if (v == "warn" || v == "warning") return spdlog::level::warn;
    if (v == "error") return spdlog::level::err;
    if (v == "fatal" || v == "critical") return spdlog::level::critical;
    return spdlog::level::info;
}

}  // namespace

auto init(std::string service) -> void {
    std::call_once(g_once, [s = std::move(service)]() mutable {
        // 8k-deep async queue, single backing thread — plenty for our log
        // volume and avoids contention on every log call.
        spdlog::init_thread_pool(8192, 1);

        auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

        // Structured-ish single line. We *will* upgrade to a JSON sink later;
        // for the dev loop the colored version is far easier to scan.
        sink->set_pattern("%Y-%m-%dT%H:%M:%S.%e%z [%^%l%$] %v");

        g_logger = std::make_shared<spdlog::async_logger>(
            std::move(s),
            sink,
            spdlog::thread_pool(),
            spdlog::async_overflow_policy::block);
        g_logger->set_level(level_from_env());
        g_logger->flush_on(spdlog::level::warn);
        spdlog::register_logger(g_logger);
        spdlog::set_default_logger(g_logger);
    });
}

auto get() -> std::shared_ptr<spdlog::logger> {
    // If a caller forgot to call init(), fall back to a stderr default. This
    // is wrong but the alternative is a crash, which is worse for diagnosis.
    if (!g_logger) {
        init("velocity-uninitialized");
    }
    return g_logger;
}

// -----------------------------------------------------------------------------
// Thread-local context — minimal implementation; full implementation arrives
// when we switch to the structured JSON sink in Phase 2.
// -----------------------------------------------------------------------------
namespace {
thread_local std::unordered_map<std::string, std::string> g_ctx;
}  // namespace

auto push_thread_context(std::string_view key, std::string_view value) -> void {
    g_ctx.emplace(std::string{key}, std::string{value});
}

auto pop_thread_context(std::string_view key) -> void {
    g_ctx.erase(std::string{key});
}

}  // namespace velocity::log
