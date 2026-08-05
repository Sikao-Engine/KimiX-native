// Test for src/runtime/search/bm25.h (plan 005).
// This test covers:
// - bm25_idf golden values (harvested from the reference formula)
// - score: exact float64 accumulation vs a numpy-computed golden vector
//   (operation-order identical), zero for docs without postings
// - top_k: nonzero-only, (score desc, doc asc) ordering, truncation

#include "ut/ut.hpp"
#include <runtime/search/bm25.h>

#include <cmath>
#include <string>

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
}
