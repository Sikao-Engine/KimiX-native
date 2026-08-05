/*
 * token_count.h — Heuristic UTF-8 token counting (kimix::runtime::text).
 *
 * Plan 001: native port of `kimi_cli/utils/tokens.py::_estimate_chars_tokens`,
 * `_is_cjk_text`, and the heuristic fallback of `count_tokens`. Runs on every
 * message append / prune / compaction / restore / revert, so it must be a
 * single SIMD-friendly UTF-8 pass with no per-code-point Python objects.
 *
 * Algorithms (verified against the Python source — implement exactly):
 *
 *   _CJK_RE ranges:  \u4e00-\u9fff  \u3400-\u4dbf  \U00020000-\U0002ebef
 *                    \uac00-\ud7af  \u3040-\u309f  \u30a0-\u30ff  \uff00-\uffef
 *   _is_cjk_text(text, threshold=0.15): cjk_count / len(text) > threshold
 *   _estimate_chars_tokens(text):
 *       if not text: return 0
 *       total = len(text)                     # CODE POINTS, not bytes
 *       ascii_ratio = ascii_count / total
 *       if ascii_ratio > 0.95:  return max(1, total // 4)
 *       if _is_cjk_text(text):  return max(1, total // 3)
 *       return max(1, int(total / 3.5))       # float division, truncation
 *
 * Notes:
 * - `len(text)` counts code points → the kernel decodes UTF-8 (it must never
 *   count bytes; 4-byte CJK / emoji guard this).
 * - `int(total / 3.5)` is IEEE-754 double division followed by truncation
 *   toward zero → `int64_t(double(total) / 3.5)`.
 * - Invalid UTF-8 bytes (cannot occur through the shim, which encodes with
 *   errors="surrogatepass") count as one code point each, mirroring
 *   common::decode_cp semantics — well-defined for malformed input.
 *
 * Pure C++ kernel: no Python includes; GIL is released in the binding layer
 * (src/runtime/py/py_text.cpp).
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace text {

// Result of a single UTF-8 scan: total code points and count of ASCII
// (cp < 0x80) code points. `ascii` never exceeds `code_points`.
struct count_stats {
    uint32_t code_points = 0;
    uint32_t ascii = 0;
};

// One pass over `bytes`: decodes UTF-8, counts code points and ASCII code
// points. ASCII runs are counted in bulk (SIMD-friendly later).
KIMIX_RUNTIME_API count_stats scan_utf8(kimix::string_view bytes) noexcept;

// True when `cp` falls in one of the 7 CJK ranges of `_CJK_RE`
// (CJK ideographs + Ext B, Hangul, Hiragana, Katakana, fullwidth forms).
KIMIX_RUNTIME_API bool is_cjk_cp(uint32_t cp) noexcept;

// Mirror of `_is_cjk_text`: cjk_count / code_point_total > threshold.
// Empty input is always false. Strict > (never >=).
KIMIX_RUNTIME_API bool is_cjk_text(kimix::string_view utf8,
                                   double threshold = 0.15) noexcept;

// Mirror of `_estimate_chars_tokens` (see file comment). Returns the exact
// integer the Python heuristic returns for the same text. Single pass.
KIMIX_RUNTIME_API int estimate_chars_tokens(kimix::string_view utf8) noexcept;

} // namespace text
} // namespace runtime
} // namespace kimix
