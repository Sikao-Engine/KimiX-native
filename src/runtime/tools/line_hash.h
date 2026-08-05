/*
 * line_hash.h - Per-line chained xxHash32 (kimix::runtime::tools).
 *
 * Plan 013: native port of kimi-cli hash_line.py::compute_line_hash (41-69).
 * The Python recipe (replicated EXACTLY):
 *   1. strip one trailing '\r' if present;
 *   2. collect non-whitespace chars (Python str.isspace set:
 *      U+0009-000D, U+0020, U+0085, U+00A0, U+1680, U+2000-200A, U+2028,
 *      U+2029, U+202F, U+205F, U+3000), tracking has_significant = any
 *      alphanumeric char (Python str.isalnum) seen;
 *   3. seed: prev_hash (2-char nibble string) -> seed = sum(ord(c)*256^k)
 *      masked to 32 bits; otherwise has_significant ? HASH_SEED(0) : line_num;
 *   4. hash = xxh32(filtered_utf8, seed).intdigest() & 0xFF.
 *
 * The kernel exposes the two steps the bindings need:
 *   compute_line_hash(line, seed)  - steps 1-2-4 for one line given a final
 *                                    32-bit seed;
 *   compute_line_hashes(content, seed, out) - the full chained scan: line
 *                                    splitting on '\n' with trailing '\r'
 *                                    stripped (matches splitlines() for
 *                                    LF/CRLF files), has_significant default
 *                                    seed (HASH_SEED) for the first line,
 *                                    nibble-decoded prev-hash seeds after.
 *
 * The nibble chain maps a hash value h (0..255) back to the reference's
 * 2-char string via NIBBLE_STR = "ZPMQVRWSNKTXJBYH" and re-derives the seed
 * from its ASCII codes, so the C++ chain is bit-exact with Python.
 *
 * isalnum note: the reference uses full Unicode isalnum. The kernel embeds a
 * generated table of the Unicode L* and N* categories (letters + numbers),
 * so has_significant is exact for arbitrary UTF-8 input. The whitespace set
 * is exact by construction.
 *
 * Pure C++ kernel: no Python includes; GIL released in the binding layer.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace tools {

// Steps 1-2-4 of the reference recipe for one line with a final seed.
// Returns xxh32(non_whitespace_bytes, seed) & 0xFF (0..255).
KIMIX_RUNTIME_API uint32_t compute_line_hash(kimix::string_view line,
                                             uint32_t seed) noexcept;

// Chained per-line hashes with reference seed semantics (see header).
// `seed` is HASH_SEED (0) for the first line's has_significant case; the
// first all-non-alnum line uses line_num instead. `out` is cleared first.
KIMIX_RUNTIME_API void compute_line_hashes(kimix::string_view content,
                                           uint32_t seed,
                                           kimix::vector<uint32_t>& out);

// True when `cp` is alphanumeric per Python str.isalnum (Unicode L*/N*).
KIMIX_RUNTIME_API bool is_alnum_cp(uint32_t cp) noexcept;

} // namespace tools
} // namespace runtime
} // namespace kimix
