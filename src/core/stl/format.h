/*
 * format.h — kimix::format and kimix::format_to (std::format-based formatting).
 *
 * kimix::format(fmt, args...) — format into kimix::string (default) or
 *   any basic_string type with kimix::format<String>(...).
 *
 * kimix::format_to(output_iter, fmt, args...) — appends to an output
 *   iterator (e.g. std::back_inserter(string)).
 *
 * Supports the full C++20 std::format syntax:
 *   {:d} {:x} {:X} {:b} {:o} {:f} {:e} {:g}
 *   {:05d} {:10.2f} {:.4f}
 *   Positional arguments: {1} {0}
 *   Escaped braces: {{ }} for literal braces.
 *
 * Format strings are compile-time checked via std::format_string (a format
 * error is a compile error when the format string is a literal); runtime
 * failures inside the noexcept entry points abort (matching the previous fmt
 * behavior with FMT_EXCEPTIONS=0). FMT_STRING("...") is a no-op passthrough
 * kept for source compatibility.
 *
 * Example:
 *   auto s = kimix::format("answer is {}", 42);
 *   kimix::format_to(std::back_inserter(buf), "{} {}", 1, 2);
 */
#pragma once

// kimix::format — formatting utilities built on C++20 std::format.
//
// The fmt library previously vendored with spdlog has been removed; the
// standard <format> header is used instead. All formatting entry points
// here are noexcept (runtime format errors abort instead of throwing).

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

#include <algorithm>
#include <format>
#include <iterator>
#include <string>

#include <core/stl/memory.h>
#include <core/stl/string.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#ifndef FMT_STRING
    #define FMT_STRING(...) __VA_ARGS__
#endif

namespace kimix {

// ---------------------------------------------------------------------------
// std::format entry points re-exported into the kimix namespace
// ---------------------------------------------------------------------------

// Format into a string of type `String` (defaults to kimix::string).
// The result is produced by std::format and then moved into the requested
// string type (which may use a custom allocator, e.g. kimix::string).
template <typename String, typename... Args>
[[nodiscard]] inline auto format(std::format_string<Args...> f, Args &&...args) noexcept {
    auto text = std::format(f, std::forward<Args>(args)...);
    return String{text.data(), text.size()};
}

template <typename... Args>
[[nodiscard]] inline auto format(std::format_string<Args...> f, Args &&...args) noexcept {
    return kimix::format<kimix::string>(f, std::forward<Args>(args)...);
}

// Append formatted output to an output iterator (e.g. std::back_inserter).
template <typename OutputIt, typename... Args>
inline void format_to(OutputIt out, std::format_string<Args...> f, Args &&...args) noexcept {
    std::format_to(std::forward<OutputIt>(out), f, std::forward<Args>(args)...);
}

} // namespace kimix
