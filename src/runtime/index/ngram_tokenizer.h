/*
 * ngram_tokenizer.h — Overlapping n-gram tokenizer (kimix::runtime::index).
 *
 * Plan 004: native port of `kimix/retrieval.py::NgramTokenizer` (lines 23-92,
 * verified against the source). Pure C++ kernel compiled into runtime.dll —
 * never includes Python headers; the GIL is released in the binding layer
 * (src/runtime/py/py_index.cpp).
 *
 * Algorithms (implemented exactly):
 * - normalize : lowercase. The kernel implements the ASCII fast path only
 *   (A-Z -> a-z); non-ASCII bytes are returned unchanged. Python's
 *   `text.lower()` lowercases ALL code points and then applies NFKC when
 *   non-ASCII survives. That split is composed in the Python shim
 *   (python/kimix_native/index.py): native normalize -> (if non-ASCII)
 *   unicodedata .lower() + NFKC — which is byte-exact with the reference for
 *   every input. The kernel contract is deliberately ASCII-only so the
 *   runtime DLL stays free of Unicode case tables.
 * - detect_n : `_detect_n` from the source — empty -> default n; pure ASCII
 *   -> 3 if default n < 3 else default n; otherwise count CJK code points and
 *   return 2 as soon as cjk_count > len(text) * 3 // 10 (len in CODE POINTS,
 *   mirroring Python str length); else 3 if default n < 3 else default n.
 * - tokenize : `_tokenize_impl` — when len(text) < n a single token equal to
 *   the whole text is produced; otherwise overlapping n-grams over CODE
 *   POINTS (never bytes), written as string_view slices into the input
 *   buffer (zero per-token allocation). ASCII input uses a byte fast path.
 *   The kernel does NOT strip: Python's tokenize() strips after normalize —
 *   the shim applies strip() so the composition stays exact.
 * - is_cjk_cp : the 16 ranges of NgramTokenizer._is_cjk (retrieval.py
 *   lines 43-60). NOTE this is a DIFFERENT predicate than
 *   kimix::runtime::text::is_cjk_cp (plan 001, 7 ranges incl. fullwidth
 *   forms); the tokenizer mirrors the retrieval.py ranges exactly.
 *
 * Style: kimix containers, noexcept, no RTTI, no exceptions from kernels.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace index {

// True when `cp` falls in one of the 16 CJK ranges of retrieval.py::_is_cjk.
KIMIX_RUNTIME_API bool is_cjk_cp(uint32_t cp) noexcept;

class KIMIX_RUNTIME_API NgramTokenizer {
public:
    explicit NgramTokenizer(uint32_t default_n = 2) noexcept;

    // ASCII-lowercase fast path; non-ASCII bytes unchanged (see file comment
    // for the shim composition that makes this exact vs the reference).
    kimix::string normalize(kimix::string_view text) const;

    // Auto-detect n-gram size: 2 for CJK-dense text, else max(default_n, 3)
    // (returns default_n when default_n >= 3). Mirrors `_detect_n`.
    uint32_t detect_n(kimix::string_view normalized) const noexcept;

    // Overlapping n-grams over code points of `normalized_text`. Appends
    // string_view slices into `normalized_text` to `out` (the caller must
    // keep the input buffer alive while the views are in use). Empty input
    // appends nothing; len(text) < n appends the whole text as one token.
    // Exactly mirrors `_tokenize_impl`.
    void tokenize(kimix::string_view normalized_text, uint32_t n,
                                    kimix::vector<kimix::string_view>& out) const;

    uint32_t default_n() const noexcept { return _default_n; }

private:
    uint32_t _default_n;
};

} // namespace index
} // namespace runtime
} // namespace kimix
