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
 */

#pragma once

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

} // namespace detail
} // namespace parse
} // namespace runtime
} // namespace kimix
