/*
 * format.h — kimix::format and kimix::format_to (fmt-based formatting).
 *
 * kimix::format(fmt, args...) — format into kimix::string (default) or
 *   any basic_string type with kimix::format<String>(...).
 *
 * kimix::format_to(output_iter, fmt, args...) — appends to an output
 *   iterator (e.g. std::back_inserter(string)).
 *
 * Supports all fmt format specs:
 *   {:d} {:x} {:X} {:b} {:o} {:f} {:e} {:g}
 *   {:05d} {:10.2f} {:.4f}
 *   Positional arguments: {1} {0}
 *   Escaped braces: {{ }} for literal braces.
 *
 * Format errors abort (FMT_EXCEPTIONS=0), all functions are noexcept.
 * FMT_STRING("...") macro is available for compile-time check.
 *
 * Example:
 *   auto s = kimix::format("answer is {}", 42);
 *   kimix::format_to(std::back_inserter(buf), "{} {}", 1, 2);
 */
#pragma once

// kimix::format — formatting utilities built on the fmt library bundled with spdlog.
//
// fmt is vendored as part of spdlog (src/ext/spdlog). It is compiled with
// FMT_EXCEPTIONS=0 (format errors abort instead of throwing) and
// FMT_USE_CONSTEVAL=0 (format strings are checked at run time unless wrapped
// with FMT_STRING / FMT_COMPILE), so all formatting entry points here are
// noexcept.

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

#include <algorithm>
#include <iterator>

#ifdef SPDLOG_FMT_EXTERNAL
    #include <fmt/xchar.h>
#else
    #include <spdlog/fmt/bundled/xchar.h>
#endif

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
// fmt entry points re-exported into the kimix namespace
// ---------------------------------------------------------------------------

using fmt::format_to;

// Format into a string of type `String` (defaults to kimix::string).
// The formatting buffer allocates through kimix's allocator.
template <typename String, typename Format, typename... Args>
[[nodiscard]] inline auto format(Format &&f, Args &&...args) noexcept {
    using char_type = typename String::value_type;
    using memory_buffer = fmt::basic_memory_buffer<char_type, fmt::inline_buffer_size, allocator<char_type>>;
    memory_buffer buffer;
    kimix::format_to(std::back_inserter(buffer), std::forward<Format>(f), std::forward<Args>(args)...);
    return String{buffer.data(), buffer.size()};
}

template <typename Format, typename... Args>
[[nodiscard]] inline auto format(Format &&f, Args &&...args) noexcept {
    return kimix::format<kimix::string>(std::forward<Format>(f), std::forward<Args>(args)...);
}

} // namespace kimix
