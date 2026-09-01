/*
 * ascii_util.h - Shared ASCII character-class helpers for the runtime parse
 * scanners (plans 011/012).
 *
 * comment_scanner.cpp and shell_scanner.cpp used to each define
 * ascii_alnum/ascii_alpha/ascii_digit (and comment_scanner also ascii_space)
 * inside their own anonymous namespace. The runtime target builds with unity
 * (jumbo) compilation (batch_size = 8): when both TUs land in the same unity
 * batch, the two anonymous namespaces merge into one and the duplicate
 * definitions break the build (MSVC C2084). These helpers now live in this
 * single internal header (namespace kimix::runtime::parse::detail) so that no
 * two runtime .cpp files can collide regardless of unity batch assignment.
 *
 * The runtime target only compiles .cpp files (headers are not compiled), so
 * a header-only helper is safe. All functions are `inline` and share identical
 * semantics with the definitions they replace.
 *
 * The scan_* helpers below are shared low-level "find the next interesting
 * byte" primitives used by both scanners' hot loops. They let long runs of
 * uninteresting bytes (comment bodies, string contents, plain code) be
 * skipped with memchr instead of a branchy per-byte state-machine chain.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstring>

namespace kimix {
namespace runtime {
namespace parse {
namespace detail {

// ASCII whitespace: space, tab, LF, CR, VT, FF (Python str.isspace on ASCII).
inline bool ascii_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// ASCII letters and digits (Python str.isalnum on ASCII).
inline bool ascii_alnum(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// ASCII letters only (Python str.isalpha on ASCII).
inline bool ascii_alpha(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// ASCII digits only (Python str.isdigit on ASCII).
inline bool ascii_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// Constexpr 256-entry set table for table-driven scanning. Only ASCII bytes
// can be marked (the scanners never treat bytes >= 0x80 as interesting).
constexpr std::array<bool, 256> make_set_table(std::initializer_list<char> chars) noexcept {
    std::array<bool, 256> t{};
    for (char c : chars) {
        t[static_cast<unsigned char>(c)] = true;
    }
    return t;
}

// Find the first occurrence of `c` in data[from, limit); returns `limit` when
// absent. Probes the first few bytes inline (tiny comments/strings never pay
// memchr's call overhead), then memchr for the long tail (SIMD on MSVC).
inline size_t scan_find_char(const char* data, size_t limit, size_t from,
                             char c) noexcept {
    if (from >= limit) {
        return limit;
    }
    const size_t window = (from + 8 < limit) ? from + 8 : limit;
    for (size_t i = from; i < window; ++i) {
        if (data[i] == c) {
            return i;
        }
    }
    if (window >= limit) {
        return limit;
    }
    const void* p = std::memchr(data + window, c, limit - window);
    return p ? static_cast<size_t>(static_cast<const char*>(p) - data) : limit;
}

// Find the earliest byte in data[from, limit) that matches any of the up to
// `count` needles; returns `limit` when none matches.
//
// Single-pass table scan, deliberately NOT a per-needle memchr chase: memchr
// would scan the whole remaining input for an absent needle (e.g. no backslash
// in a short string literal), which turns many short states into O(n^2) over
// the whole file. One byte-class test per byte is linear and fast enough for
// the short runs these states actually see.
inline size_t scan_find_any(const char* data, size_t limit, size_t from,
                            const char* needles, size_t count) noexcept {
    if (from >= limit || count == 0) {
        return limit;
    }
    unsigned char mark[256] = {};
    for (size_t k = 0; k < count; ++k) {
        mark[static_cast<unsigned char>(needles[k])] = 1;
    }
    for (size_t i = from; i < limit; ++i) {
        if (mark[static_cast<unsigned char>(data[i])]) {
            return i;
        }
    }
    return limit;
}

// Single-pass table-driven scan for the first byte whose bit is set in
// `table` (data[from, limit)); returns `limit` when none matches. Used where
// the interesting set is large or the skipped runs are short (state-machine
// code states), where the per-call overhead of several memchr passes would
// dominate.
inline size_t scan_find_table(const char* data, size_t limit, size_t from,
                              const std::array<bool, 256>& table) noexcept {
    for (size_t i = from; i < limit; ++i) {
        if (table[static_cast<unsigned char>(data[i])]) {
            return i;
        }
    }
    return limit;
}

// Find the first occurrence of the `plen`-byte pattern in data[from, limit);
// returns `limit` when absent. memchr on the first pattern byte, then memcmp.
inline size_t scan_find_sub(const char* data, size_t limit, size_t from,
                            const char* pattern, size_t plen) noexcept {
    if (plen == 0) {
        return from < limit ? from : limit;
    }
    size_t i = from;
    for (;;) {
        const size_t pos = scan_find_char(data, limit, i, pattern[0]);
        if (pos >= limit || pos + plen > limit) {
            return limit;
        }
        if (std::memcmp(data + pos, pattern, plen) == 0) {
            return pos;
        }
        i = pos + 1;
    }
}

} // namespace detail
} // namespace parse
} // namespace runtime
} // namespace kimix