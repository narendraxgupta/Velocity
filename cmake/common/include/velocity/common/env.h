// =============================================================================
//  velocity/common/env.h
//
//  Lightweight, type-safe environment variable parsing. Every Velocity
//  service is configured via environment variables (12-factor / K8s-native).
//  Replaces the usual ad-hoc `std::getenv` + atoi pattern with something
//  that fails fast on misconfiguration.
//
//  Example:
//      const auto port  = velocity::env::required<std::uint16_t>("VELOCITY_HTTP_PORT");
//      const auto level = velocity::env::optional<std::string>("VELOCITY_LOG_LEVEL", "info");
// =============================================================================

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace velocity::env {

class missing_env : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class bad_env : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Read an env var as a raw string. Returns nullopt if unset.
[[nodiscard]] auto raw(std::string_view name) noexcept -> std::optional<std::string>;

// Read an env var as the requested type, throwing `missing_env` if unset.
template <typename T>
[[nodiscard]] auto required(std::string_view name) -> T;

// Read an env var with a fallback default.
template <typename T>
[[nodiscard]] auto optional(std::string_view name, T default_value) -> T;

// Convenience: typical bool parsing — "1", "true", "yes", "y", "on" → true.
[[nodiscard]] auto parse_bool(std::string_view value) -> bool;

// Explicit instantiations are in env.cpp.
extern template auto required<std::string>(std::string_view) -> std::string;
extern template auto required<std::int32_t>(std::string_view) -> std::int32_t;
extern template auto required<std::int64_t>(std::string_view) -> std::int64_t;
extern template auto required<std::uint16_t>(std::string_view) -> std::uint16_t;
extern template auto required<std::uint32_t>(std::string_view) -> std::uint32_t;
extern template auto required<std::uint64_t>(std::string_view) -> std::uint64_t;
extern template auto required<bool>(std::string_view) -> bool;

extern template auto optional<std::string>(std::string_view, std::string) -> std::string;
extern template auto optional<std::int32_t>(std::string_view, std::int32_t) -> std::int32_t;
extern template auto optional<std::int64_t>(std::string_view, std::int64_t) -> std::int64_t;
extern template auto optional<std::uint16_t>(std::string_view, std::uint16_t) -> std::uint16_t;
extern template auto optional<std::uint32_t>(std::string_view, std::uint32_t) -> std::uint32_t;
extern template auto optional<std::uint64_t>(std::string_view, std::uint64_t) -> std::uint64_t;
extern template auto optional<bool>(std::string_view, bool) -> bool;

}  // namespace velocity::env
