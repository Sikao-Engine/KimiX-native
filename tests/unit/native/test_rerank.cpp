// Test for src/runtime/search/rerank.h (plan 005).
// This test covers:
// - mmr_rerank: golden sequences harvested from a reference replica
//   (lambda 0.5 / 1.0 / 0.0, k truncation, strict-> first-max-wins ties)
// - xquad_rerank: aspects-driven greedy diversification + the score-only
//   overload (stable relevance-descending selection)

#include "ut/ut.hpp"
#include "bench_util.h"
#include <runtime/search/rerank.h>

#include <cstdint>
#include <random>
#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::search;

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "mmr_golden_lambda_05"_test = [] {
        // Golden from the reference replica: [0, 2, 4, 3, 1].
        const double scores[] = {0.9, 0.8, 0.7, 0.6, 0.5};
        const double sim_data[5][5] = {
            {1.0, 0.9, 0.1, 0.1, 0.0},
            {0.9, 1.0, 0.1, 0.0, 0.1},
            {0.1, 0.1, 1.0, 0.2, 0.0},
            {0.1, 0.0, 0.2, 1.0, 0.1},
            {0.0, 0.1, 0.0, 0.1, 1.0},
        };
        similarity_fn sim = [&](uint32_t a, uint32_t b) { return sim_data[a][b]; };
        auto sel = mmr_rerank(scores, sim, 0.5, 5);
        expect(eq(sel.size(), 5u));
        expect(eq(sel[0], 0u));
        expect(eq(sel[1], 2u));
        expect(eq(sel[2], 4u));
        expect(eq(sel[3], 3u));
        expect(eq(sel[4], 1u));
    };

    "mmr_golden_lambda_1_and_0"_test = [] {
        const double scores[] = {0.9, 0.8, 0.7, 0.6, 0.5};
        const double sim_data[5][5] = {
            {1.0, 0.9, 0.1, 0.1, 0.0},
            {0.9, 1.0, 0.1, 0.0, 0.1},
            {0.1, 0.1, 1.0, 0.2, 0.0},
            {0.1, 0.0, 0.2, 1.0, 0.1},
            {0.0, 0.1, 0.0, 0.1, 1.0},
        };
        similarity_fn sim = [&](uint32_t a, uint32_t b) { return sim_data[a][b]; };
        // lambda=1.0: pure relevance -> relevance order.
        auto sel = mmr_rerank(scores, sim, 1.0, 5);
        expect(eq(sel.size(), 5u));
        for (uint32_t i = 0; i < 5; ++i) {
            expect(eq(sel[i], i));
        }
        // lambda=0.0: pure diversity -> golden [0, 4, 2, 3, 1].
        sel = mmr_rerank(scores, sim, 0.0, 5);
        printf("DBG lam0: ");
        for (auto d : sel) {
            printf("%u ", d);
        }
        printf("\n");
        expect(eq(sel[0], 0u));
        expect(eq(sel[1], 4u));
        expect(eq(sel[2], 2u));
        expect(eq(sel[3], 3u));
        expect(eq(sel[4], 1u));
    };

    "mmr_k_truncation_and_ties"_test = [] {
        const double scores[] = {0.9, 0.8, 0.7, 0.6, 0.5};
        const double sim_data[5][5] = {
            {1.0, 0.9, 0.1, 0.1, 0.0},
            {0.9, 1.0, 0.1, 0.0, 0.1},
            {0.1, 0.1, 1.0, 0.2, 0.0},
            {0.1, 0.0, 0.2, 1.0, 0.1},
            {0.0, 0.1, 0.0, 0.1, 1.0},
        };
        similarity_fn sim = [&](uint32_t a, uint32_t b) { return sim_data[a][b]; };
        auto sel = mmr_rerank(scores, sim, 0.5, 2);
        expect(eq(sel.size(), 2u));
        expect(eq(sel[0], 0u));
        expect(eq(sel[1], 2u));
        expect(mmr_rerank(scores, sim, 0.5, 0).empty());
        // Ties: strict > -> FIRST remaining wins (stable).
        const double tied[] = {1.0, 1.0, 1.0};
        const double sim2[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        similarity_fn sim2f = [&](uint32_t a, uint32_t b) { return sim2[a][b]; };
        auto sel2 = mmr_rerank(tied, sim2f, 0.5, 3);
        printf("DBG ties: ");
        for (auto d : sel2) {
            printf("%u ", d);
        }
        printf("\n");
        expect(eq(sel2[0], 0u));
        expect(eq(sel2[1], 1u));
        expect(eq(sel2[2], 2u));
        // Empty scores.
        expect(mmr_rerank({}, sim2f, 0.5, 3).empty());
    };

    "xquad_aspects_golden"_test = [] {
        // Two docs share aspect 0; doc 2 has a new aspect.
        const double scores[] = {0.9, 0.8, 0.7};
        kimix::vector<kimix::bitvector> aspects;
        aspects.emplace_back(2, false);
        aspects[0][0] = true;
        aspects.emplace_back(2, false);
        aspects[1][0] = true; // same aspect as doc 0
        aspects.emplace_back(2, false);
        aspects[2][1] = true; // fresh aspect
        // Golden (verified against a reference replica): round 1 — doc0 gets
        // diversity 1.0 (aspect 0 new) -> 0.45 + 0.5 = 0.95, doc1 0.4+0.5 =
        // 0.9, doc2 0.35+0.5 = 0.85 -> doc0. Round 2 — doc2 (aspect 1 new,
        // 0.85) beats doc1 (aspect 0 covered, 0.4). Round 3 — doc1.
        auto sel = xquad_rerank(scores, aspects, 0.5, 3);
        expect(eq(sel.size(), 3u));
        expect(eq(sel[0], 0u));
        expect(eq(sel[1], 2u));
        expect(eq(sel[2], 1u));
    };

    "xquad_score_only_overload"_test = [] {
        // Stable relevance-descending selection (empty aspects).
        const double scores[] = {3.0, 1.0, 2.0};
        auto sel = xquad_rerank(scores, 3);
        expect(eq(sel.size(), 3u));
        expect(eq(sel[0], 0u));
        expect(eq(sel[1], 2u));
        expect(eq(sel[2], 1u));
        // k truncation.
        auto sel2 = xquad_rerank(scores, 2);
        expect(eq(sel2.size(), 2u));
        expect(eq(sel2[0], 0u));
        expect(eq(sel2[1], 2u));
        expect(xquad_rerank(scores, 0).empty());
        expect(xquad_rerank({}, 3).empty());
    };

    // --- benchmarks (see bench_util.h contract) ---
    // No hard timing assertions; expect() guards verify the measured path.

    "bench_mmr_1k_candidates"_test = [] {
        // 1k retrieval candidates, k=50, lambda=0.5 (typical rerank cut).
        constexpr uint32_t kN = 1000;
        constexpr uint32_t kSel = 50;
        std::mt19937 rng(0xAB01u);
        kimix::vector<double> scores(kN);
        for (uint32_t i = 0; i < kN; ++i) {
            scores[i] = static_cast<double>(rng() % 1000) / 1000.0;
        }
        // Pairwise similarity with |i-j| decay (query-agnostic doc similarity).
        kimix::vector<double> sim_data(kN * kN);
        for (uint32_t i = 0; i < kN; ++i) {
            for (uint32_t j = 0; j < kN; ++j) {
                const double dist =
                    static_cast<double>(i >= j ? i - j : j - i) / kN;
                double sim = 0.85 - 0.8 * dist;
                if (sim < 0.05) {
                    sim = 0.05;
                }
                sim_data[static_cast<size_t>(i) * kN + j] = i == j ? 1.0 : sim;
            }
        }
        similarity_fn sim = [&](uint32_t a, uint32_t b) {
            return sim_data[static_cast<size_t>(a) * kN + b];
        };
        // Correctness: cap, uniqueness, and pure-relevance order at lambda=1.
        auto sel = mmr_rerank(scores, sim, 0.5, kSel);
        expect(eq(sel.size(), size_t(kSel)));
        kimix::vector<uint8_t> seen(kN, 0);
        bool unique = true;
        for (uint32_t d : sel) {
            if (seen[d]) {
                unique = false;
            }
            seen[d] = 1;
        }
        expect(unique) << "mmr must not repeat positions";
        auto rel = mmr_rerank(scores, sim, 1.0, kN);
        expect(eq(rel.size(), size_t(kN)));
        bool non_inc = true;
        for (size_t i = 1; i < rel.size(); ++i) {
            if (scores[rel[i]] > scores[rel[i - 1]] + 1e-15) {
                non_inc = false;
            }
        }
        expect(non_inc) << "lambda=1.0 -> relevance descending";
        uint64_t checksum = 0;
        kimix_bench::run("mmr/1k_cand_l05_k50", [&] {
            auto s = mmr_rerank(scores, sim, 0.5, kSel);
            checksum += s.empty() ? 0ull : s[0];
        }, 1);
        kimix_bench::sink(checksum);
    };

    "bench_xquad_1k_candidates"_test = [] {
        // 1k candidates with per-doc aspect-label bitsets, k=50, lambda=0.5.
        constexpr uint32_t kN = 1000;
        constexpr uint32_t kSel = 50;
        constexpr uint32_t kLabels = 12;
        std::mt19937 rng(0xAB02u);
        kimix::vector<double> scores(kN);
        for (uint32_t i = 0; i < kN; ++i) {
            scores[i] = static_cast<double>(rng() % 1000) / 1000.0;
        }
        kimix::vector<kimix::bitvector> aspects;
        aspects.resize(kN);
        for (uint32_t d = 0; d < kN; ++d) {
            aspects[d].resize(kLabels, false);
            const uint32_t nlab = 2 + rng() % 5; // 2..6 labels per doc
            for (uint32_t a = 0; a < nlab; ++a) {
                aspects[d][rng() % kLabels] = true;
            }
        }
        auto sel = xquad_rerank(scores, aspects, 0.5, kSel);
        expect(eq(sel.size(), size_t(kSel)));
        kimix::vector<uint8_t> seen(kN, 0);
        bool unique = true;
        for (uint32_t d : sel) {
            if (seen[d]) {
                unique = false;
            }
            seen[d] = 1;
        }
        expect(unique) << "xquad must not repeat positions";
        // Score-only overload = stable relevance-descending selection.
        auto so = xquad_rerank(scores, kSel);
        expect(eq(so.size(), size_t(kSel)));
        bool non_inc = true;
        for (size_t i = 1; i < so.size(); ++i) {
            if (scores[so[i]] > scores[so[i - 1]] + 1e-15) {
                non_inc = false;
            }
        }
        expect(non_inc) << "score-only xquad -> relevance descending";
        uint64_t checksum = 0;
        kimix_bench::run("xquad/1k_cand_aspects_k50", [&] {
            auto s = xquad_rerank(scores, aspects, 0.5, kSel);
            checksum += s.empty() ? 0ull : s[0];
        }, 1);
        kimix_bench::run("xquad/1k_cand_score_only_k50", [&] {
            auto s = xquad_rerank(scores, kSel);
            checksum += s.empty() ? 0ull : s[0];
        }, 1);
        kimix_bench::sink(checksum);
    };
}
