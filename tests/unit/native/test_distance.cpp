// Test for src/runtime/search/distance.h (plan 005).
// This test covers:
// - damerau_levenshtein: golden pairs (incl. reference fast-path quirks),
//   transpositions, max_dist early-exit equivalence with the full DP
//   (exhaustive over small strings), code-point awareness
// - freq_lower_bound: asymmetric pattern-first semantics, golden values
// - jaro / jaro_winkler: classic golden vectors (MARTHA/MARHTA etc.)
// - sorensen_dice / ngram_overlap: reference quirks (both-empty, single char)

#include "ut/ut.hpp"
#include <runtime/search/distance.h>

#include <cmath>
#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::search;

namespace {

bool near_eq(double a, double b) {
    return std::abs(a - b) < 1e-12;
}

kimix::string utf8_of(const std::initializer_list<uint32_t>& cps) {
    kimix::string out;
    for (uint32_t cp : cps) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "dl_golden_pairs"_test = [] {
        // Golden values harvested from the reference implementation.
        expect(eq(damerau_levenshtein("cat", "car"), 1));
        expect(eq(damerau_levenshtein("ca", "abc"), 3));
        expect(eq(damerau_levenshtein("ab", "ba"), 1));        // transposition
        expect(eq(damerau_levenshtein("abc", "x"), 1));        // n==1 fast-path quirk
        expect(eq(damerau_levenshtein("kitten", "sitting"), 3));
        expect(eq(damerau_levenshtein("CA", "ABC"), 3));
        expect(eq(damerau_levenshtein("", "abc"), 3));
        expect(eq(damerau_levenshtein("abc", "abc"), 0));
        expect(eq(damerau_levenshtein("abcd", "abdc"), 1));    // adjacent swap
        expect(eq(damerau_levenshtein("a", ""), 1));
        expect(eq(damerau_levenshtein("abc", "ab"), 1));
        expect(eq(damerau_levenshtein("dixon", "dicksonx"), 4));
        // Symmetric after the internal swap.
        expect(eq(damerau_levenshtein("car", "cat"), 1));
    };

    "dl_exhaustive_matches_reference_dp"_test = [] {
        // Brute-force the reference DP inline (exact port) and compare with
        // the kernel for ALL pairs of small strings — catches swap/fast-path
        // errors.
        const char* alphabet = "abc";
        kimix::vector<kimix::string> strs;
        for (int l = 0; l <= 4; ++l) {
            const int total = static_cast<int>(std::pow(3, l));
            for (int code = 0; code < total; ++code) {
                kimix::string s;
                int c = code;
                for (int i = 0; i < l; ++i) {
                    s.push_back(alphabet[c % 3]);
                    c /= 3;
                }
                strs.push_back(s);
            }
        }
        // Reference DP (exact port of retrieval.py:842-884).
        auto ref_dl = [](const kimix::string& a, const kimix::string& b) -> int32_t {
            kimix::string s = a, t = b;
            if (s.size() < t.size()) {
                std::swap(s, t);
            }
            const size_t m = s.size(), n = t.size();
            if (n == 0) {
                return static_cast<int32_t>(m);
            }
            if (n == 1) {
                return s[0] == t[0] ? 0 : 1;
            }
            if (m == 2 && n == 2) {
                if (s == t) {
                    return 0;
                }
                if (s[0] == t[0] || s[1] == t[1]) {
                    return 1;
                }
                if (s[0] == t[1] && s[1] == t[0]) {
                    return 1;
                }
                return 2;
            }
            kimix::vector<int32_t> prev_prev(n + 1), prev(n + 1), curr(n + 1);
            for (size_t j = 0; j <= n; ++j) {
                prev_prev[j] = static_cast<int32_t>(j);
                prev[j] = static_cast<int32_t>(j);
            }
            for (size_t i = 1; i <= m; ++i) {
                curr[0] = static_cast<int32_t>(i);
                const char si_1 = s[i - 1];
                for (size_t j = 1; j <= n; ++j) {
                    const int32_t cost = si_1 == t[j - 1] ? 0 : 1;
                    int32_t v = curr[j - 1] + 1;
                    if (prev[j] + 1 < v) {
                        v = prev[j] + 1;
                    }
                    if (prev[j - 1] + cost < v) {
                        v = prev[j - 1] + cost;
                    }
                    if (i > 1 && j > 1 && si_1 == t[j - 2] && s[i - 2] == t[j - 1]) {
                        if (prev_prev[j - 2] + 1 < v) {
                            v = prev_prev[j - 2] + 1;
                        }
                    }
                    curr[j] = v;
                }
                std::swap(prev_prev, prev);
                std::swap(prev, curr);
            }
            return prev[n];
        };

        bool all_match = true;
        for (const auto& a : strs) {
            for (const auto& b : strs) {
                const int32_t expected = ref_dl(a, b);
                const int32_t got = damerau_levenshtein(a, b);
                if (got != expected) {
                    all_match = false;
                    printf("DL mismatch %s vs %s: got %d expected %d\n",
                           a.c_str(), b.c_str(), got, expected);
                }
                // max_dist semantics: exact when <= max_dist else max_dist+1.
                for (int32_t md = 0; md <= 4; ++md) {
                    const int32_t got_bounded = damerau_levenshtein(a, b, md);
                    const int32_t want = expected > md ? md + 1 : expected;
                    if (got_bounded != want) {
                        all_match = false;
                        printf("DL(bound %d) mismatch %s vs %s: got %d want %d\n",
                               md, a.c_str(), b.c_str(), got_bounded, want);
                    }
                }
            }
        }
        expect(all_match) << "exhaustive small-string DL equivalence";
    };

    "dl_cjk_code_points"_test = [] {
        // "你" (U+4F60) vs "他" (U+4ED6): 1 substitution; byte-wise distance
        // would be 3+ (multi-byte) — the kernel must operate on code points.
        const kimix::string ni = utf8_of({0x4F60});
        const kimix::string ta = utf8_of({0x4ED6});
        expect(eq(damerau_levenshtein(ni, ta), 1));
        // Mixed ASCII/CJK: "a你" vs "a他" -> 1 substitution.
        const kimix::string a_ni = kimix::string("a") + ni;
        const kimix::string a_ta = kimix::string("a") + ta;
        expect(eq(damerau_levenshtein(a_ni, a_ta), 1));
    };

    "freq_lower_bound_golden"_test = [] {
        // Pattern "kitten": counts {k:1,i:1,t:2,e:1,n:1}
        // term "sitting": {s:1,i:2,t:2,n:1,g:1}
        // total = |1-0|(k) + |1-2|(i) + |1-0|(e) + |1-1|(n) = 1+1+1+0 = 3
        // matched = t-counts of pattern chars = 0+2+2+0+1 = 5
        // total += len(term) - matched = 7 - 5 = 2 -> total 5 -> (5+1)//2 = 3
        expect(eq(freq_lower_bound("kitten", "sitting"), 3));
        // Asymmetry: freq_lower_bound(a, b) != freq_lower_bound(b, a) in
        // general (pattern-first formula). "ab" vs "b": pattern counts
        // a:1,b:1; term counts a:0,b:1 -> |1-0| + matched(0+1)=1; += len(1)-1
        // -> total 1 -> 1. Reverse: pattern "b", term "ab": b:1 vs 1 -> no
        // add; matched 1; += 2-1 -> 1 -> (1+1)//2 = 1. Both 1 here.
        expect(eq(freq_lower_bound("ab", "b"), 1));
        expect(eq(freq_lower_bound("b", "ab"), 1));
        // Empty term: pattern "abc" (a:1,b:1,c:1), term "": |1-0| x3 ->
        // total 3, matched 0, += 0-0 -> (3+1)//2 = 2.
        expect(eq(freq_lower_bound("abc", ""), 2));
        // Identical strings -> 0.
        expect(eq(freq_lower_bound("abc", "abc"), 0));
    };

    "jaro_golden"_test = [] {
        expect(near_eq(jaro_similarity("MARTHA", "MARHTA"), 0.9444444444444445));
        expect(near_eq(jaro_similarity("DIXON", "DICKSONX"), 0.7666666666666666));
        expect(near_eq(jaro_similarity("JELLYFISH", "SMELLYFISH"), 0.8962962962962964));
        expect(near_eq(jaro_similarity("MARTHA", "MARTHA"), 1.0));
        expect(near_eq(jaro_similarity("", "abc"), 0.0));
        expect(near_eq(jaro_similarity("abc", ""), 0.0));
        expect(near_eq(jaro_similarity("", ""), 1.0)); // s == t short-circuits to 1.0
    };

    "jaro_winkler_golden"_test = [] {
        expect(near_eq(jaro_winkler("MARTHA", "MARHTA"), 0.9611111111111111));
        expect(near_eq(jaro_winkler("DIXON", "DICKSONX"), 0.8133333333333332));
        expect(near_eq(jaro_winkler("JELLYFISH", "SMELLYFISH"), 0.8962962962962964));
        // Custom prefix_scale.
        expect(near_eq(jaro_winkler("MARTHA", "MARHTA", 0.2), 0.9777777777777777));
    };

    "dice_golden_and_quirks"_test = [] {
        expect(near_eq(sorensen_dice("night", "nacht"), 0.25));
        expect(near_eq(sorensen_dice("", ""), 1.0));
        expect(near_eq(sorensen_dice("a", ""), 0.0));
        expect(near_eq(sorensen_dice("", "a"), 0.0));
        // Reference quirk: two identical single-char strings -> 0.0 (no
        // bigrams, denom == 0).
        expect(near_eq(sorensen_dice("a", "a"), 0.0));
        expect(near_eq(sorensen_dice("a", "ab"), 0.0));
        expect(near_eq(sorensen_dice("ab", "ab"), 1.0));
        expect(near_eq(sorensen_dice("abc", "abd"), 0.5));
    };

    "ngram_overlap_golden"_test = [] {
        expect(near_eq(ngram_overlap("night", "nacht", 2), 0.14285714285714285));
        expect(near_eq(ngram_overlap("a", "ab", 2), 0.0));
        expect(near_eq(ngram_overlap("abc", "abd", 3), 0.0));
        expect(near_eq(ngram_overlap("", "ab", 2), 0.0));
        expect(near_eq(ngram_overlap("ab", "ab", 2), 1.0));
        expect(near_eq(ngram_overlap("abcd", "abce", 2), 0.5));
        // len < n collapses the whole string to one gram.
        expect(near_eq(ngram_overlap("ab", "abc", 2), 0.5)); // {"ab"} vs {"ab","bc"}
    };
}
