/*
 * utf8.h — UTF-8 decoding utilities (kimix::runtime::common).
 *
 * Pure C++ kernel helpers compiled into runtime.dll. This header (and its
 * .cpp) must NEVER include Python / pybind11 headers: the shared library is a
 * pure-computation runtime with no Python dependency. The GIL is released in
 * the binding layer only (see src/runtime/py/module.cpp and common/gil.h).
 *
 * Conventions:
 *   - is_ascii               : fast-path check — true when every byte < 0x80.
 *   - decode_cp              : decode ONE code point at *it and advance *it
 *                              past it. Invalid bytes yield U+FFFD and advance
 *                              exactly 1 byte (never an infinite loop).
 *   - utf8_code_point_count  : count code points (ASCII fast path +
 *                              continuation-byte skipping). Invalid bytes
 *                              count as 1 code point each.
 *   - utf8_byte_length       : 1..4 bytes needed to encode a valid code point.
 *
 * Style: kimix containers (kimix::string_view = std::string_view), noexcept,
 * no RTTI, no exceptions thrown from kernels.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace common {

// True when every byte in `bytes` is < 0x80 (pure ASCII).
KIMIX_RUNTIME_API bool is_ascii(kimix::string_view bytes) noexcept;

// Decode one UTF-8 code point starting at *it (precondition: it < end).
// Advances *it past the decoded sequence. On any invalid byte (bad lead,
// missing/incorrect continuation, overlong, out of range, truncated at end)
// advances *it by exactly 1 byte and returns U+FFFD.
KIMIX_RUNTIME_API uint32_t decode_cp(const char*& it, const char* end) noexcept;

// Number of code points in `bytes`. Exactly the number a decode_cp walk
// over the buffer would produce (ASCII runs are counted in bulk). Invalid
// bytes count as 1 code point each, so the result is well-defined even for
// malformed input.
KIMIX_RUNTIME_API size_t utf8_code_point_count(kimix::string_view bytes) noexcept;

// Number of UTF-8 bytes needed to encode `cp` (1..4; 4 for cp >= 0x10000).
KIMIX_RUNTIME_API size_t utf8_byte_length(uint32_t cp) noexcept;

} // namespace common
} // namespace runtime
} // namespace kimix
