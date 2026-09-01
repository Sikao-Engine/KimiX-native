/*
 * bm25.cpp — implementation of the BM25 scoring kernel (see header).
 */

#include <runtime/search/bm25.h>

#include <algorithm>
#include <cmath>

namespace kimix {
namespace runtime {
namespace search {

double bm25_idf(uint32_t doc_count, uint32_t df, double /*k1*/, double /*b*/) noexcept {
    // retrieval.py::BM25Scorer._idf: log(1 + (N - df + 0.5) / (df + 0.5))
    const double N = static_cast<double>(doc_count);
    const double d = static_cast<double>(df);
    return std::log(1.0 + (N - d + 0.5) / (d + 0.5));
}

Bm25Scorer::Bm25Scorer(double k1, double b) noexcept : _k1(k1), _b(b) {}

void Bm25Scorer::score(
    kimix::span<const kimix::span<const index::postings_entry>> query_postings,
    kimix::span<const double> idf,
    kimix::span<const uint32_t> doc_lengths,
    double avg_doc_len, uint32_t doc_count,
    kimix::vector<double>& out_scores) const noexcept {
    out_scores.assign(doc_count, 0.0);
    if (doc_count == 0 || avg_doc_len == 0.0) {
        return; // Python: `if N == 0 or self._denom_base is None: return zeros`
    }

    const double k1 = _k1;
    const double b = _b;
    // Python _build_denom_base: k1 * ((1.0 - b) + (b / avg_doc_len) * doc_len).
    // `b / avg_doc_len` is constant for the whole call — hoisting it saves one
    // division per posting while keeping the double expression bit-identical.
    const double b_over_avg = b / avg_doc_len;
    const size_t n_terms = query_postings.size();
    for (size_t ti = 0; ti < n_terms; ++ti) {
        const double idf_t = ti < idf.size() ? idf[ti] : 0.0;
        // Python: tfs_f *= idf * (k1 + 1.0) * q_weight  (q_weight == 1 here)
        const double scale = idf_t * (k1 + 1.0);
        const auto& postings = query_postings[ti];
        for (const auto& e : postings) {
            const uint32_t doc = e.doc_id;
            if (doc >= doc_count) {
                continue; // defensive: caller must size doc_count = N
            }
            const double tf = static_cast<double>(e.tf);
            const double doc_len = doc < doc_lengths.size()
                                       ? static_cast<double>(doc_lengths[doc])
                                       : 0.0;
            const double denom_base = k1 * ((1.0 - b) + b_over_avg * doc_len);
            // Python _token_scores: np.add(tfs_f, denom_base[docs], out=denom)
            const double denom = tf + denom_base;
            out_scores[doc] += tf * scale / denom;
        }
    }
}

void top_k(kimix::span<const double> scores, uint32_t k,
           kimix::vector<uint32_t>& out_docs) noexcept {
    out_docs.clear();
    if (k == 0 || scores.empty()) {
        return;
    }
    const size_t n = scores.size();

    // Collect docs with score > 0 and keep the best k with a k-limited
    // min-heap (score desc, doc asc). std::push_heap makes the root the
    // comp-LARGEST element, so the comparator ranks "better" first — the root
    // is then the WORST of the kept entries: smaller score, or equal score
    // with a larger doc id. This preserves the documented (score desc, doc
    // asc) ordering and the smallest-doc tie selection, but touches O(k)
    // memory instead of building a vector of every nonzero score (O(n))
    // before partial_sort.
    struct score_doc {
        double score;
        uint32_t doc;
    };
    const auto better = [](const score_doc& a, const score_doc& b) noexcept {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.doc < b.doc;
    };
    kimix::vector<score_doc> heap;
    const size_t cap = (std::min)(n, static_cast<size_t>(k));
    heap.reserve(cap);
    for (uint32_t doc = 0; doc < n; ++doc) {
        const double s = scores[doc];
        if (!(s > 0.0)) {
            continue; // Python keeps only nonzero scores (NaN excluded too)
        }
        if (heap.size() < cap) {
            heap.push_back({s, doc});
            std::push_heap(heap.begin(), heap.end(), better);
        } else if (better(score_doc{s, doc}, heap.front())) {
            // The candidate is strictly better than the current worst
            // (comp-largest root): evict the worst and re-insert.
            std::pop_heap(heap.begin(), heap.end(), better);
            heap.back() = {s, doc};
            std::push_heap(heap.begin(), heap.end(), better);
        }
    }
    // heap holds the k best in heap order — emit them sorted
    // (score desc, doc asc) like the previous partial_sort result.
    std::sort(heap.begin(), heap.end(), [](const score_doc& a, const score_doc& b) noexcept {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.doc < b.doc;
    });
    out_docs.reserve(heap.size());
    for (const auto& e : heap) {
        out_docs.push_back(e.doc);
    }
}

} // namespace search
} // namespace runtime
} // namespace kimix
