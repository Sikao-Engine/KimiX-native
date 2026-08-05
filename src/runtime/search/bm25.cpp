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
            // Python _build_denom_base: k1 * ((1.0 - b) + (b / avg_doc_len) * doc_len)
            const double denom_base = k1 * ((1.0 - b) + (b / avg_doc_len) * doc_len);
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
    // Collect (doc, score) for docs with score > 0 (Python keeps only
    // nonzero scores), then partial-select the top k by (score desc, doc asc).
    kimix::vector<std::pair<double, uint32_t>> cand;
    cand.reserve(scores.size());
    for (uint32_t doc = 0; doc < static_cast<uint32_t>(scores.size()); ++doc) {
        const double s = scores[doc];
        if (s > 0.0) {
            cand.emplace_back(s, doc);
        }
    }
    if (cand.empty()) {
        return;
    }
    const auto by_score_desc_doc_asc = [](const std::pair<double, uint32_t>& a,
                                          const std::pair<double, uint32_t>& b) {
        if (a.first != b.first) {
            return a.first > b.first;
        }
        return a.second < b.second;
    };
    const size_t keep = (std::min)(cand.size(), static_cast<size_t>(k));
    std::partial_sort(cand.begin(), cand.begin() + keep, cand.end(), by_score_desc_doc_asc);
    out_docs.reserve(keep);
    for (size_t i = 0; i < keep; ++i) {
        out_docs.push_back(cand[i].second);
    }
}

} // namespace search
} // namespace runtime
} // namespace kimix
