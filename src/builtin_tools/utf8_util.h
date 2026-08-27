// utf8_util.h - Minimal self-contained UTF-8 helpers for src/builtin_tools.
//
// `src/runtime/common/utf8.h` belongs to the runtime_py target, which
// kimix-llm does not link, so builtin_tools carries its own tiny copy of the
// handful of UTF-8 primitives the tool kernels need (code-point counting and
// prefix slicing for the character-based truncation markers). Semantics match
// Python's `len(str)` / `str[:n]` on the code-point level.

#pragma once

#include <cstddef>
#include <cstdint>

#include <core/kimix_core.h>

namespace kimix::builtin_tools {

// True when every byte is < 0x80 (fast path for the ASCII-gated kernels).
bool is_ascii(kimix::string_view bytes) noexcept;

// Decode one code point starting at `it` and advance it. Invalid sequences
// decode to U+FFFD and advance by one byte (Python's errors="replace" style).
uint32_t decode_code_point(const char *&it, const char *end) noexcept;

// Number of code points in `bytes` (Python `len(str)`).
size_t utf8_code_point_count(kimix::string_view bytes) noexcept;

// Byte offset after skipping `code_points` code points from the front.
// Clamps to `bytes.size()` when the text is shorter.
size_t utf8_byte_offset_of_code_point(kimix::string_view bytes,
                                      size_t code_points) noexcept;

// Byte offset of the code point boundary at or before `byte_pos` (never splits
// a multi-byte sequence).
size_t utf8_floor_boundary(kimix::string_view bytes, size_t byte_pos) noexcept;

// True when `bytes` is valid UTF-8 (strict: no overlongs, no surrogates
// D800-DFFF, max U+10FFFF, no trailing partial sequence).
bool utf8_validate(kimix::string_view bytes) noexcept;

// If `bytes` is invalid UTF-8, fill `out` with 1-based byte offset and reason
// ("invalid start byte", "unexpected end of data", "invalid continuation
// byte", "surrogates not allowed" — the exact Python DecoderError wording),
// and return false. Returns true when valid.
bool utf8_strict_error(kimix::string_view bytes, size_t &bad_offset,
                       kimix::string &reason) noexcept;

} // namespace kimix::builtin_tools
