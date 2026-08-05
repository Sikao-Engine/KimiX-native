/*
 * logging.h — spdlog-backed logging (kimix namespace + WD macros).
 *
 * Logging functions (kimix::log_*):
 *   log_verbose(fmt, args...) — fmt-style formatted logging
 *   log_info(fmt, args...)
 *   log_warning(fmt, args...)
 *   log_error(fmt, args...)
 *   log_flush()               — flush the default logger
 *
 * Level control:
 *   log_level_verbose()
 *   log_level_info()
 *   log_level_warning()
 *   log_level_error()
 *
 * Macros (include file:line automatically):
 *   KIMIX_VERBOSE(...), KIMIX_INFO(...), KIMIX_WARNING(...), KIMIX_ERROR(...)
 *   KIMIX_VERBOSE_WITH_LOCATION(...), KIMIX_INFO_WITH_LOCATION(...),
 *   KIMIX_WARNING_WITH_LOCATION(...), KIMIX_ERROR_WITH_LOCATION(...)
 *   — append " [file:line]" to the message.
 *
 * Assertion:
 *   KIMIX_ASSERT(cond, ...) — logs and calls debug_break() on failure.
 *
 * Each function accepts either a single message (convertible to
 * kimix::string) or a fmt format string + arguments.
 * Format errors abort (FMT_EXCEPTIONS=0); all functions are noexcept.
 *
 * Example:
 *   kimix::log_info("server started on port {}", 8080);
 *   KIMIX_INFO("loaded {} items in {:.2f} ms", count, elapsed);
 *   KIMIX_ASSERT(ptr != nullptr, "ptr must not be null");
 */
#pragma once

// kimix logging — thin wrappers over spdlog.
//
// The default logger lives in logging.cpp. Log messages are formatted with
// the fmt library bundled with spdlog (see core/stl/format.h). Format errors
// abort instead of throwing (spdlog is built with FMT_EXCEPTIONS=0), so all
// logging functions are noexcept.

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

#include <spdlog/spdlog.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "dll_export.h"
#include "platform.h"
#include "stl/format.h"
#include "stl/string.h"

#include <cstdlib>
#include <type_traits>
#include <utility>

#ifndef KIMIX_STRINGIFY_IMPL
    #define KIMIX_STRINGIFY_IMPL(x) #x
#endif
#ifndef KIMIX_STRINGIFY
    #define KIMIX_STRINGIFY(x) KIMIX_STRINGIFY_IMPL(x)
#endif

namespace kimix {

namespace detail {

// The process-wide default logger (defined in logging.cpp).
[[nodiscard]] KIMIX_CORE_API spdlog::logger &default_logger() noexcept;

// Low-level message dispatch into the default logger.
KIMIX_CORE_API void log_message(spdlog::level::level_enum level,
                             const char *file, int line,
                             const char *function,
                             string_view message) noexcept;

// Convert a single argument into a kimix::string without going through fmt.
template <typename T>
[[nodiscard]] inline string to_string(T &&t) noexcept {
    if constexpr (std::is_same_v<std::remove_cvref_t<T>, string>) {
        return std::forward<T>(t);
    } else {
        return string{std::forward<T>(t)};
    }
}

} // namespace detail

// ---------------------------------------------------------------------------
// Log functions
//
// Each function accepts either a single message-like argument (anything
// convertible to kimix::string) or a fmt format string followed by arguments:
//   kimix::log_info("server started");
//   kimix::log_info("loaded {} items in {:.2f} ms", count, elapsed);
// ---------------------------------------------------------------------------

template <typename... Args>
void log_verbose(Args &&...args) noexcept {
    if constexpr (sizeof...(args) == 1) {
        detail::log_message(spdlog::level::debug, nullptr, 0, "",
                            detail::to_string(std::forward<Args>(args)...));
    } else {
        detail::log_message(spdlog::level::debug, nullptr, 0, "",
                            kimix::format(std::forward<Args>(args)...));
    }
}

template <typename... Args>
void log_info(Args &&...args) noexcept {
    if constexpr (sizeof...(args) == 1) {
        detail::log_message(spdlog::level::info, nullptr, 0, "",
                            detail::to_string(std::forward<Args>(args)...));
    } else {
        detail::log_message(spdlog::level::info, nullptr, 0, "",
                            kimix::format(std::forward<Args>(args)...));
    }
}

template <typename... Args>
void log_warning(Args &&...args) noexcept {
    if constexpr (sizeof...(args) == 1) {
        detail::log_message(spdlog::level::warn, nullptr, 0, "",
                            detail::to_string(std::forward<Args>(args)...));
    } else {
        detail::log_message(spdlog::level::warn, nullptr, 0, "",
                            kimix::format(std::forward<Args>(args)...));
    }
}

template <typename... Args>
void log_error(Args &&...args) noexcept {
    if constexpr (sizeof...(args) == 1) {
        detail::log_message(spdlog::level::err, nullptr, 0, "",
                            detail::to_string(std::forward<Args>(args)...));
    } else {
        detail::log_message(spdlog::level::err, nullptr, 0, "",
                            kimix::format(std::forward<Args>(args)...));
    }
}

/// Flush the default logger.
KIMIX_CORE_API void log_flush() noexcept;

/// Set the default logger level to verbose (debug).
KIMIX_CORE_API void log_level_verbose() noexcept;
/// Set the default logger level to info.
KIMIX_CORE_API void log_level_info() noexcept;
/// Set the default logger level to warning.
KIMIX_CORE_API void log_level_warning() noexcept;
/// Set the default logger level to error.
KIMIX_CORE_API void log_level_error() noexcept;

} // namespace kimix

// ---------------------------------------------------------------------------
// Logging macros
//
// Ex. KIMIX_INFO("loaded {} items", count);
// The *_WITH_LOCATION variants append " [file:line]" to the message.
// ---------------------------------------------------------------------------

#define KIMIX_VERBOSE(...) \
    ::kimix::log_verbose(__VA_ARGS__)

#define KIMIX_INFO(...) \
    ::kimix::log_info(__VA_ARGS__)

#define KIMIX_WARNING(...) \
    ::kimix::log_warning(__VA_ARGS__)

#define KIMIX_ERROR(...) \
    ::kimix::log_error(__VA_ARGS__)

#define KIMIX_VERBOSE_WITH_LOCATION(fmt, ...) \
    KIMIX_VERBOSE(fmt " [" __FILE__ ":" KIMIX_STRINGIFY(__LINE__) "]" __VA_OPT__(, ) __VA_ARGS__)

#define KIMIX_INFO_WITH_LOCATION(fmt, ...) \
    KIMIX_INFO(fmt " [" __FILE__ ":" KIMIX_STRINGIFY(__LINE__) "]" __VA_OPT__(, ) __VA_ARGS__)

#define KIMIX_WARNING_WITH_LOCATION(fmt, ...) \
    KIMIX_WARNING(fmt " [" __FILE__ ":" KIMIX_STRINGIFY(__LINE__) "]" __VA_OPT__(, ) __VA_ARGS__)

#define KIMIX_ERROR_WITH_LOCATION(fmt, ...) \
    KIMIX_ERROR(fmt " [" __FILE__ ":" KIMIX_STRINGIFY(__LINE__) "]" __VA_OPT__(, ) __VA_ARGS__)

// ---------------------------------------------------------------------------
// KIMIX_ASSERT
// ---------------------------------------------------------------------------

#define KIMIX_ASSERT_FAILED_IMPL(x) \
    KIMIX_ERROR_WITH_LOCATION("Assertion '" #x "' failed.")

#define KIMIX_ASSERT_FAILED_IMPL_WITH_MESSAGE_IMPL_WITH_FMT(x, fmt, ...) \
    KIMIX_ERROR_WITH_LOCATION("Assertion '" #x "' failed: " fmt, __VA_ARGS__)

#define KIMIX_ASSERT_FAILED_IMPL_WITH_MESSAGE_IMPL(x, msg) \
    KIMIX_ERROR_WITH_LOCATION("Assertion '" #x "' failed: " msg)

#define KIMIX_ASSERT_FAILED_IMPL_WITH_MESSAGE(x, fmt, ...) \
    KIMIX_ASSERT_FAILED_IMPL_WITH_MESSAGE_IMPL##__VA_OPT__(_WITH_FMT)(x, fmt __VA_OPT__(, ) __VA_ARGS__)

#define KIMIX_ASSERT(x, ...)                                                                   \
    do {                                                                                    \
        if (!(x)) {                                                                         \
            KIMIX_ASSERT_FAILED_IMPL##__VA_OPT__(_WITH_MESSAGE)(x __VA_OPT__(, ) __VA_ARGS__); \
            ::kimix::debug_break();                                                            \
        }                                                                                   \
    } while (0)
