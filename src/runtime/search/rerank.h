/*
 * rerank.h — MMR / xQuAD re-ranking kernels (kimix::runtime::search).
 *
 * Plan 005: native ports of retrieval.py::mmr_rerank (lines 1358-1402) and
 * xquad_rerank (lines 2482-2519).
 *
 * mmr_rerank(scores, sim, lambda, k) — greedy Maximal Marginal Relevance.
 * Positions are indices into `scores` (the caller maps them to doc ids).
 * Exact reference semantics:
 *   - max_sim starts at 0.0 (no selected docs yet -> sim contribution 0).
 *   - score = lambda * relevance - (1 - lambda) * max_sim
 *   - STRICT > comparison: among ties the FIRST remaining candidate wins
 *     (stable, reference iterates remaining in order).
 *   - `sim(a, b)` is called with original positions. The binding wraps a
 *     caller-supplied similarity matrix in this function.
 *
 * xquad_rerank(scores, aspects, lambda, k) — greedy xQuAD diversification.
 * `aspects[d]` is a bitvector over aspect labels for the doc at position d
 * (bitset ops replace the reference's set arithmetic). Reference semantics:
 *   - diversity = |new_aspects| / max(|doc_aspects|, 1)  (0 when none)
 *   - score = (1 - lambda) * relevance + lambda * diversity
 *   - strict >, first-max-wins; covered labels accumulate.
 * The score-only overload xquad_rerank(scores, k) supplies empty aspect sets
 * (documented: the plan's binding API is score-only; with no aspects the
 * reference reduces to a stable relevance-descending selection).
 *
 * Pure C++ kernel: no Python includes; GIL released in the binding layer.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace search {

// Similarity lookup by position index (a, b in [0, scores.size())).
using similarity_fn = kimix::function<double(uint32_t a, uint32_t b)>;

// Greedy MMR over `scores` (relevance per position). Returns selected
// positions in selection order, capped at k (min(k, scores.size())).
KIMIX_RUNTIME_API kimix::vector<uint32_t> mmr_rerank(
    kimix::span<const double> scores, const similarity_fn& sim,
    double lambda_param, uint32_t k) noexcept;

// Greedy xQuAD with per-position aspect-label bitsets. `aspects` must have
// scores.size() entries when non-empty. Returns positions in selection order.
KIMIX_RUNTIME_API kimix::vector<uint32_t> xquad_rerank(
    kimix::span<const double> scores,
    kimix::span<const kimix::bitvector> aspects,
    double lambda_param, uint32_t k) noexcept;

// Score-only xQuAD (empty aspects): stable relevance-descending selection —
// the plan's binding API. Equivalent to xquad_rerank with lambda = 0.5 and
// all-empty aspect sets (see file comment).
KIMIX_RUNTIME_API kimix::vector<uint32_t> xquad_rerank(
    kimix::span<const double> scores, uint32_t k) noexcept;

} // namespace search
} // namespace runtime
} // namespace kimix
