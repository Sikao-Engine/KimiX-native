/*
 * rerank.cpp — implementation of MMR / xQuAD (see header).
 */

#include <runtime/search/rerank.h>

#include <algorithm>
#include <limits>

namespace kimix {
namespace runtime {
namespace search {

kimix::vector<uint32_t> mmr_rerank(kimix::span<const double> scores,
                                   const similarity_fn& sim,
                                   double lambda_param, uint32_t k) noexcept {
    kimix::vector<uint32_t> selected;
    const size_t n = scores.size();
    if (n == 0 || k == 0) {
        return selected;
    }
    const size_t cap = (std::min)(n, static_cast<size_t>(k));

    // Order-preserving linked list of remaining positions: the reference
    // removes with list.pop(idx) which keeps the ORIGINAL relative order —
    // that order IS the tie-break ("strict >: first remaining with the max
    // score wins"). A list gives O(1) erase (vs the reference's O(n) shift)
    // while preserving the semantics.
    kimix::list<uint32_t> remaining;
    for (uint32_t i = 0; i < n; ++i) {
        remaining.push_back(i);
    }
    selected.reserve(cap);

    while (!remaining.empty() && selected.size() < cap) {
        auto best_it = remaining.begin();
        double best_score = -std::numeric_limits<double>::infinity();
        bool found = false;
        for (auto it = remaining.begin(); it != remaining.end(); ++it) {
            const uint32_t cand = *it;
            double max_sim = 0.0; // reference: max_sim starts at 0.0
            for (uint32_t sel : selected) {
                const double s = sim(cand, sel);
                if (s > max_sim) {
                    max_sim = s;
                }
            }
            const double score = lambda_param * scores[cand] - (1.0 - lambda_param) * max_sim;
            // STRICT >: the FIRST remaining with the max score wins (stable).
            if (!found || score > best_score) {
                found = true;
                best_score = score;
                best_it = it;
            }
        }
        if (!found) {
            break;
        }
        selected.push_back(*best_it);
        remaining.erase(best_it);
    }
    return selected;
}

kimix::vector<uint32_t> xquad_rerank(kimix::span<const double> scores,
                                     kimix::span<const kimix::bitvector> aspects,
                                     double lambda_param, uint32_t k) noexcept {
    kimix::vector<uint32_t> selected;
    const size_t n = scores.size();
    if (n == 0 || k == 0) {
        return selected;
    }
    const size_t cap = (std::min)(n, static_cast<size_t>(k));
    const bool have_aspects = aspects.size() == n;

    // Order-preserving removal (same stability rationale as mmr_rerank).
    kimix::list<uint32_t> remaining;
    for (uint32_t i = 0; i < n; ++i) {
        remaining.push_back(i);
    }
    selected.reserve(cap);

    // covered: bitvector over aspect labels (grows with the largest label).
    kimix::bitvector covered;
    auto ensure_covered = [&covered](size_t label) {
        if (label >= covered.size()) {
            covered.resize(label + 1, false);
        }
    };

    while (!remaining.empty() && selected.size() < cap) {
        auto best_it = remaining.begin();
        double best_score = -std::numeric_limits<double>::infinity();
        bool found = false;
        for (auto it = remaining.begin(); it != remaining.end(); ++it) {
            const uint32_t cand = *it;
            double diversity = 0.0;
            if (have_aspects && !aspects[cand].empty()) {
                size_t new_count = 0;
                const auto& doc_aspects = aspects[cand];
                for (size_t a = 0; a < doc_aspects.size(); ++a) {
                    if (doc_aspects[a]) {
                        ensure_covered(a);
                        if (!covered[a]) {
                            ++new_count;
                        }
                    }
                }
                diversity = static_cast<double>(new_count) /
                            static_cast<double>(doc_aspects.size());
            }
            // score = (1 - lambda) * relevance + lambda * diversity
            const double score = (1.0 - lambda_param) * scores[cand] + lambda_param * diversity;
            // STRICT >: first remaining with the max score wins (stable).
            if (!found || score > best_score) {
                found = true;
                best_score = score;
                best_it = it;
            }
        }
        if (!found) {
            break;
        }
        const uint32_t chosen = *best_it;
        selected.push_back(chosen);
        // covered |= aspects[chosen]
        if (have_aspects) {
            const auto& doc_aspects = aspects[chosen];
            for (size_t a = 0; a < doc_aspects.size(); ++a) {
                if (doc_aspects[a]) {
                    ensure_covered(a);
                    covered[a] = true;
                }
            }
        }
        remaining.erase(best_it);
    }
    return selected;
}

kimix::vector<uint32_t> xquad_rerank(kimix::span<const double> scores, uint32_t k) noexcept {
    // Score-only: stable relevance-descending selection (empty aspects).
    return xquad_rerank(scores, {}, 0.5, k);
}

} // namespace search
} // namespace runtime
} // namespace kimix
