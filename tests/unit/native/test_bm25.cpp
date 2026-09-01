// Test for src/runtime/search/bm25.h (plan 005).
// This test covers:
// - bm25_idf golden values (harvested from the reference formula)
// - score: exact float64 accumulation vs a numpy-computed golden vector
//   (operation-order identical), zero for docs without postings
// - top_k: nonzero-only, (score desc, doc asc) ordering, truncation

#include "ut/ut.hpp"
#include "bench_util.h"
#include <runtime/search/bm25.h>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <random>
#include <string>
#include <utility>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::search;
using kimix::runtime::index::postings_entry;

namespace {

bool near_eq(double a, double b) {
    return std::abs(a - b) < 1e-12;
}

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "bm25_idf_golden"_test = [] {
        // math.log(1 + (N - df + 0.5) / (df + 0.5)) — golden from Python.
        expect(near_eq(bm25_idf(1000, 10, 1.2, 0.75), 4.557379522151743));
        expect(near_eq(bm25_idf(500, 50, 1.2, 0.75), 2.2946327648035507));
        expect(near_eq(bm25_idf(100, 0, 1.2, 0.75), 5.308267697401205));
        expect(near_eq(bm25_idf(5, 2, 1.2, 0.75), 0.8754687373538999));
    };

    "score_golden_numpy_accumulation"_test = [] {
        // Golden vector computed with numpy (reference op order):
        // N=5, doc_lengths=[4,3,0,2,5], avgdl=3.5, k1=1.2, b=0.75
        // term1 postings [(0,tf2),(4,tf1)], term2 [(1,tf1),(3,tf2)]
        // idf(df=2,N=5) = 0.8754687373538999
        const uint32_t doc_count = 5;
        const kimix::vector<uint32_t> doc_lengths = {4, 3, 0, 2, 5};
        const double avgdl = 3.5;
        const double idf = 0.8754687373538999;

        const postings_entry t1[] = {{0, 2}, {4, 1}};
        const postings_entry t2[] = {{1, 1}, {3, 2}};
        const kimix::span<const postings_entry> qp[] = {t1, t2};
        const double idfs[] = {idf, idf};

        Bm25Scorer scorer(1.2, 0.75);
        kimix::vector<double> scores;
        scorer.score(qp, idfs, doc_lengths, avgdl, doc_count, scores);

        expect(eq(scores.size(), 5u));
        expect(near_eq(scores[0], 1.1572719789914214));
        expect(near_eq(scores[1], 0.9298081762241421));
        expect(near_eq(scores[2], 0.0)); // doc 2 has no postings
        expect(near_eq(scores[3], 1.368753152817265));
        expect(near_eq(scores[4], 0.7448739533287326));
    };

    "score_zero_guard"_test = [] {
        // doc_count == 0 or avgdl == 0 -> all-zero scores.
        Bm25Scorer scorer;
        kimix::vector<double> scores;
        const postings_entry t1[] = {{0, 1}};
        const kimix::span<const postings_entry> qp[] = {t1};
        const double idfs[] = {1.0};
        scorer.score(qp, idfs, kimix::span<const uint32_t>{}, 3.5, 0, scores);
        expect(scores.empty());
        const uint32_t len4[] = {4};
        scorer.score(qp, idfs, len4, 0.0, 1, scores);
        expect(eq(scores.size(), 1u));
        expect(eq(scores[0], 0.0));
    };

    "top_k_ordering_and_nonzero"_test = [] {
        // scores: doc 2 is zero -> excluded; order by (score desc, doc asc).
        const double scores[] = {1.1572719789914214, 0.9298081762241421, 0.0,
                                 1.368753152817265, 0.7448739533287326};
        kimix::vector<uint32_t> out;
        top_k(scores, 10, out);
        // All nonzero docs, sorted (score desc, doc asc).
        expect(eq(out.size(), 4u));
        expect(eq(out[0], 3u));
        expect(eq(out[1], 0u));
        expect(eq(out[2], 1u));
        expect(eq(out[3], 4u));

        // Truncation to k=2.
        out.clear();
        top_k(scores, 2, out);
        expect(eq(out.size(), 2u));
        expect(eq(out[0], 3u));
        expect(eq(out[1], 0u));

        // Ties: (score desc, doc asc).
        const double tied[] = {3.0, 1.0, 3.0, 2.0};
        out.clear();
        top_k(tied, 10, out);
        expect(eq(out.size(), 4u));
        expect(eq(out[0], 0u));
        expect(eq(out[1], 2u));
        expect(eq(out[2], 3u));
        expect(eq(out[3], 1u));

        // k == 0 -> empty.
        out.clear();
        top_k(tied, 0, out);
        expect(out.empty());
    };

    // --- benchmarks (see bench_util.h contract) ---
    // No hard timing assertions; expect() guards verify the measured path.

    "bench_bm25_score_50kdocs"_test = [] {
        // Corpus: 50k docs, 20 query terms, ~40k packed postings total.
        constexpr uint32_t kDocs = 50000;
        constexpr uint32_t kTerms = 20;
        kimix::vector<uint32_t> doc_lengths(kDocs);
        uint64_t total_len = 0;
        for (uint32_t d = 0; d < kDocs; ++d) {
            doc_lengths[d] = 40 + (d * 7u) % 80u; // 40..119 tokens
            total_len += doc_lengths[d];
        }
        const double avgdl = static_cast<double>(total_len) / static_cast<double>(kDocs);

        kimix::vector<kimix::vector<postings_entry>> term_postings(kTerms);
        kimix::vector<kimix::span<const postings_entry>> qp(kTerms);
        kimix::vector<double> idf(kTerms);
        for (uint32_t t = 0; t < kTerms; ++t) {
            auto& pl = term_postings[t];
            pl.reserve(kDocs / 20);
            for (uint32_t d = t; d < kDocs; d += 20) {
                pl.push_back({d, 1 + (d % 3)}); // tf 1..3, doc asc
            }
            qp[t] = pl;
            idf[t] = bm25_idf(kDocs, static_cast<uint32_t>(pl.size()), 1.2, 0.75);
        }

        Bm25Scorer scorer(1.2, 0.75);
        kimix::vector<double> scores;
        scorer.score(qp, idf, doc_lengths, avgdl, kDocs, scores);
        expect(eq(scores.size(), size_t(kDocs)));
        // Spot-recompute two docs with the documented float64 formula.
        auto score_ref = [&](uint32_t doc) {
            double acc = 0.0;
            for (uint32_t t = 0; t < kTerms; ++t) {
                const double scale = idf[t] * (1.2 + 1.0);
                for (const auto& e : term_postings[t]) {
                    if (e.doc_id == doc) {
                        const double tf = static_cast<double>(e.tf);
                        const double dl = static_cast<double>(doc_lengths[doc]);
                        const double denom =
                            tf + 1.2 * ((1.0 - 0.75) + (0.75 / avgdl) * dl);
                        acc += tf * scale / denom;
                    }
                }
            }
            return acc;
        };
        expect(near_eq(scores[0], score_ref(0)));
        expect(near_eq(scores[12345], score_ref(12345)));

        int64_t checksum = 0;
        kimix_bench::run("bm25/score_50kdocs_20terms", [&] {
            scorer.score(qp, idf, doc_lengths, avgdl, kDocs, scores);
            checksum += static_cast<int64_t>(scores[0] * 1e6);
        }, 1);
        kimix_bench::sink(checksum);
    };

    "bench_bm25_topk_100k"_test = [] {
        // 100k scores, k=10 — realistic retrieval cut with heavy ties.
        constexpr uint32_t kN = 100000;
        kimix::vector<double> scores(kN);
        std::mt19937 rng(42u);
        for (uint32_t i = 0; i < kN; ++i) {
            const uint32_t r = rng() % 100;
            if (r < 50) {
                scores[i] = 0.0; // no posting -> left at zero
            } else {
                const double x = static_cast<double>(
                    (static_cast<uint64_t>(i) * 2654435761ull) % 53u);
                scores[i] = 10.0 * std::exp(-x / 9.0);
            }
        }
        // Correctness: compare with a brute-force partial_sort reference.
        kimix::vector<uint32_t> out;
        top_k(scores, 10, out);
        kimix::vector<std::pair<double, uint32_t>> cand;
        cand.reserve(kN);
        for (uint32_t d = 0; d < kN; ++d) {
            if (scores[d] > 0.0) {
                cand.emplace_back(scores[d], d);
            }
        }
        const size_t keep = (std::min)(cand.size(), size_t{10});
        std::partial_sort(cand.begin(), cand.begin() + keep, cand.end(),
                          [](const std::pair<double, uint32_t>& a,
                             const std::pair<double, uint32_t>& b) {
                              if (a.first != b.first) {
                                  return a.first > b.first;
                              }
                              return a.second < b.second;
                          });
        expect(eq(out.size(), keep));
        for (size_t i = 0; i < keep; ++i) {
            expect(eq(out[i], cand[i].second)) << "top_k vs brute force";
        }
        // Ordering invariant on the k=10 slice: (score desc, doc asc).
        expect(out.size() == 10);
        for (size_t i = 1; i < out.size(); ++i) {
            const double pa = scores[out[i - 1]], pb = scores[out[i]];
            expect(pa >= pb) << "top_k score desc";
            if (pa == pb) {
                expect(out[i - 1] < out[i]) << "top_k doc asc on ties";
            }
        }

        int64_t checksum = 0;
        kimix::vector<uint32_t> tmp;
        kimix_bench::run("bm25/topk_100k_k10", [&] {
            top_k(scores, 10, tmp);
            checksum += tmp.empty() ? 0 : static_cast<int64_t>(tmp[0]);
        }, 1);
        kimix_bench::sink(checksum);
    };
}
