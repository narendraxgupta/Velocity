// =============================================================================
//  env.cpp — type-safe environment variable parsing.
// =============================================================================

#include "velocity/common/env.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include <fmt/format.h>

namespace velocity::env {
namespace {

template <typename Int>
[[nodiscard]] auto to_integer(std::string_view name, std::string_view value) -> Int {
    Int result{};
    const auto first = value.data();
    const auto last  = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(first, last, result);
    if (ec != std::errc{} || ptr != last) {
        throw bad_env(fmt::format("env var '{}' is not a valid integer: '{}'", name, value));
    }
    return result;
}

}  // namespace

auto raw(std::string_view name) noexcept -> std::optional<std::string> {
    // std::getenv requires a null-terminated string; copy out to a small
    // local buffer when needed. Names are tiny (< 64 chars in practice).
    const std::string name_z{name};
    const char* val = std::getenv(name_z.c_str());
    if (val == nullptr) return std::nullopt;
    return std::string{val};
}

auto parse_bool(std::string_view value) -> bool {
    std::string lower{value};
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "1" || lower == "true" || lower == "yes" || lower == "y" || lower == "on") {
        return true;
    }
    if (lower == "0" || lower == "false" || lower == "no" || lower == "n" || lower == "off") {
        return false;
    }
    throw bad_env(fmt::format("expected boolean value, got: '{}'", value));
}

// -----------------------------------------------------------------------------
//  required<T>
// -----------------------------------------------------------------------------
template <typename T>
auto required(std::string_view name) -> T {
    auto v = raw(name);
    if (!v) {
        throw missing_env(fmt::format("required env var '{}' is unset", name));
    }
    if constexpr (std::is_same_v<T, std::string>) {
        return *v;
    } else if constexpr (std::is_same_v<T, bool>) {
        return parse_bool(*v);
    } else {
        static_assert(std::is_integral_v<T>, "Unsupported type for env::required");
        return to_integer<T>(name, *v);
    }
}

template auto required<std::string>(std::string_view) -> std::string;
template auto required<bool>(std::string_view) -> bool;
template auto required<std::int32_t>(std::string_view) -> std::int32_t;
template auto required<std::int64_t>(std::string_view) -> std::int64_t;
template auto required<std::uint16_t>(std::string_view) -> std::uint16_t;
template auto required<std::uint32_t>(std::string_view) -> std::uint32_t;
template auto required<std::uint64_t>(std::string_view) -> std::uint64_t;

// -----------------------------------------------------------------------------
//  optional<T>
// -----------------------------------------------------------------------------
template <typename T>
auto optional(std::string_view name, T default_value) -> T {
    auto v = raw(name);
    if (!v) return default_value;
    if constexpr (std::is_same_v<T, std::string>) {
        return *v;
    } else if constexpr (std::is_same_v<T, bool>) {
        return parse_bool(*v);
    } else {
        static_assert(std::is_integral_v<T>, "Unsupported type for env::optional");
        return to_integer<T>(name, *v);
    }
}

template auto optional<std::string>(std::string_view, std::string) -> std::string;
template auto optional<bool>(std::string_view, bool) -> bool;
template auto optional<std::int32_t>(std::string_view, std::int32_t) -> std::int32_t;
template auto optional<std::int64_t>(std::string_view, std::int64_t) -> std::int64_t;
template auto optional<std::uint16_t>(std::string_view, std::uint16_t) -> std::uint16_t;
template auto optional<std::uint32_t>(std::string_view, std::uint32_t) -> std::uint32_t;
template auto optional<std::uint64_t>(std::string_view, std::uint64_t) -> std::uint64_t;

}  // namespace velocity::env
