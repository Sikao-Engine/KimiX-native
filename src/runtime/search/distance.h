/*
 * distance.h — String distance / similarity kernels (kimix::runtime::search).
 *
 * Plan 005: native ports of `kimix/retrieval.py`:
 *   - damerau_levenshtein      (LevenshteinAutomaton._damerau_levenshtein,
 *                               lines 842-884 — EXACT port, incl. the short-
 *                               string fast paths and the OSAbL restricted-
 *                               transposition DP)
 *   - freq_lower_bound         (_freq_lower_bound, lines 886-905 — the
 *                               character-multiset edit-distance lower bound;
 *                               ASYMMETRIC: pattern is the first argument)
 *   - jaro_similarity          (lines 1167-1205)
 *   - jaro_winkler             (jaro_winkler_similarity, lines 1208-1219,
 *                               max_prefix=4, p=prefix_scale)
 *   - sorensen_dice            (sorensen_dice_coefficient, lines 1222-1233)
 *   - ngram_overlap            (lines 1236-1246)
 *
 * All functions operate on CODE POINTS (Python str semantics): ASCII inputs
 * use a raw-byte fast path; non-ASCII inputs are decoded to a code-point
 * vector first (UTF-8 via runtime::common::decode_cp).
 *
 * damerau_levenshtein(a, b, max_dist): replicates the reference EXACTLY,
 * including its quirks (e.g. the n==1 fast path returns 0/1 regardless of
 * the longer string's length — a reference behavior we preserve for parity).
 * When max_dist >= 0 the kernel may early-exit with max_dist + 1 as soon as
 * the current DP row minimum proves the final distance exceeds max_dist
 * (a valid lower bound for any edit model with non-negative op costs, incl.
 * OSAbL transpositions). Semantics: returns the exact distance when it is
 * <= max_dist (or when max_dist < 0); otherwise max_dist + 1.
 *
 * Pure C++ kernel: no Python includes; GIL released in the binding layer.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace search {

// Exact port of retrieval.py::LevenshteinAutomaton._damerau_levenshtein.
// Code-point aware. See header comment for the max_dist contract.
KIMIX_RUNTIME_API int32_t damerau_levenshtein(kimix::string_view a, kimix::string_view b,
                                              int32_t max_dist = -1) noexcept;

// Char-multiset edit-distance lower bound (retrieval.py::_freq_lower_bound).
// ASYMMETRIC: `pattern` is the automaton pattern (first arg), `term` is the
// candidate. Returns (total + 1) // 2 of the reference formula.
KIMIX_RUNTIME_API int32_t freq_lower_bound(kimix::string_view pattern,
                                           kimix::string_view term) noexcept;

// Jaro similarity (0.0-1.0). Exact port incl. transposition half-counting.
KIMIX_RUNTIME_API double jaro_similarity(kimix::string_view a, kimix::string_view b) noexcept;

// Jaro-Winkler: jaro + prefix * prefix_scale * (1 - jaro), prefix capped at 4
// (the reference's max_prefix=4). prefix_scale default 0.1 (reference p).
KIMIX_RUNTIME_API double jaro_winkler(kimix::string_view a, kimix::string_view b,
                                      double prefix_scale = 0.1) noexcept;

// Sorensen-Dice over bigram sets (2.0 * |A∩B| / (|A| + |B|)). Replicates the
// reference quirks: both-empty -> 1.0; one-empty -> 0.0; denom==0 -> 0.0
// (e.g. two identical single-char strings -> 0.0).
KIMIX_RUNTIME_API double sorensen_dice(kimix::string_view a, kimix::string_view b) noexcept;

// N-gram overlap: |A∩B| / |A∪B| over n-gram sets (default n=2). len < n
// collapses the whole string into a single gram, like the reference.
KIMIX_RUNTIME_API double ngram_overlap(kimix::string_view a, kimix::string_view b,
                                       uint32_t n = 2) noexcept;

} // namespace search
} // namespace runtime
} // namespace kimix
