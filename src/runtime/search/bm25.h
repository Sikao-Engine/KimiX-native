/*
 * bm25.h — BM25 scoring kernel (kimix::runtime::search).
 *
 * Plan 005: native port of retrieval.py::BM25Scorer._token_scores +
 * _accumulate (lines 620-806 — the numpy dense path used by Searcher.search
 * for N <= 50000). Exact float semantics: Python computes with float64 in a
 * fixed operation order (denom first, then scale, then divide); the kernel
 * reproduces that order with `double` so results are bit-identical (MSVC
 * default /fp:precise does not contract into FMA):
 *
 *     denom = tf + k1 * ((1 - b) + (b / avgdl) * doc_len)
 *     score = tf * (idf * (k1 + 1) * q_weight) / denom        # q_weight == 1
 *
 * Accumulation is a single pass over the packed postings (replaces numpy's
 * np.add.at scatter + the .tolist() dict loop). out_scores is `double`
 * (documented deviation from plan 004's `vector<float>`: float32 would lose
 * the 1e-9 parity guarantee the plan requires).
 *
 * doc_count is Python's `self._N` = max_doc_id + 1 (sparse-safe); the scores
 * array is sized doc_count and indexed by doc_id. doc_lengths must be sized
 * >= doc_count and hold the per-doc token counts (indexed by doc_id; 0 for
 * docs without a length, mirroring Python's zeros-initialized
 * _doc_lengths_arr).
 *
 * bm25_idf  = log(1 + (N - df + 0.5) / (df + 0.5))          (retrieval.py:624)
 * top_k     = docs with score > 0 sorted by (score desc, doc_id asc) — the
 *             ordering of the reference's score_topk sparse path
 *             (heapq.nlargest key=(score, -doc_id)) and of its dense
 *             "top_k >= N" branch (nonzero scores, argsort by -score).
 *
 * Pure C++ kernel: no Python includes; GIL released in the binding layer.
 */

#pragma once

#include <core/kimix_core.h>

#include <runtime/index/inverted_index.h>

namespace kimix {
namespace runtime {
namespace search {

KIMIX_RUNTIME_API double bm25_idf(uint32_t doc_count, uint32_t df,
                                  double k1, double b) noexcept;

class KIMIX_RUNTIME_API Bm25Scorer {
public:
    explicit Bm25Scorer(double k1 = 1.2, double b = 0.75) noexcept;

    // Score every document that appears in any of `query_postings`.
    //   query_postings[i] : merged postings for query term i
    //   idf[i]            : bm25_idf for term i (same length as query_postings)
    //   doc_lengths       : per-doc token counts, indexed by doc_id (size >= doc_count)
    //   avg_doc_len       : collection mean doc length
    //   doc_count         : Python's N = max_doc_id + 1
    //   out_scores        : resized to doc_count and zeroed, then accumulated.
    //                       Positions with no posting stay 0 (Python zeros).
    void score(kimix::span<const kimix::span<const index::postings_entry>> query_postings,
               kimix::span<const double> idf,
               kimix::span<const uint32_t> doc_lengths,
               double avg_doc_len, uint32_t doc_count,
               kimix::vector<double>& out_scores) const noexcept;

    double k1() const noexcept { return _k1; }
    double b() const noexcept { return _b; }

private:
    double _k1;
    double _b;
};

// Top-k selection over a scores array (indexed by doc_id): returns the doc
// ids with score > 0, ordered (score desc, doc_id asc), truncated to k.
// Uses std::nth_element-style partial selection.
KIMIX_RUNTIME_API void top_k(kimix::span<const double> scores, uint32_t k,
                             kimix::vector<uint32_t>& out_docs) noexcept;

} // namespace search
} // namespace runtime
} // namespace kimix
