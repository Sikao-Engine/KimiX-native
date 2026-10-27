// Test for core/stl/pdqsort.h — kimix::sort, the project's pdqsort entry point.
// This test covers:
// - kimix::sort(begin, end) sorts arithmetic ranges (the branchless pdqsort
//   dispatch) and matches std::sort exactly.
// - kimix::sort with std::greater reverses (also the branchless dispatch).
// - a custom comparator (non-default, struct payload) takes the plain pdqsort
//   dispatch and matches std::sort with the same comparator — pdqsort is
//   unstable, so compare by key, not by payload.
// - empty / single-element / already-sorted / reverse-sorted / organ-pipe /
//   few-distinct inputs (the pattern-detection paths of pdqsort).
// The performance comparison against std::sort lives in the env-gated
// benchmark tests/unit/core/test_pdqsort_bench.cpp (KIMIX_PDQSORT_BENCH=1).
#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace tests_unit_core_test_stl_pdqsort {

struct payload {
    int32_t key;
    uint32_t seq; // provenance; NOT part of the sort order
};

// std::sort on a copy is the oracle; pdqsort is unstable so struct checks are
// key-only.
bool same_keys(const std::vector<payload> &a, const std::vector<payload> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].key != b[i].key) return false;
    return true;
}

} // namespace tests_unit_core_test_stl_pdqsort
using namespace tests_unit_core_test_stl_pdqsort;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "sort_arithmetic_matches_std"_test = [] {
        std::mt19937_64 rng(42);
        std::vector<int32_t> v(10000);
        for (auto &x : v) x = static_cast<int32_t>(rng());
        std::vector<int32_t> ref = v;
        std::sort(ref.begin(), ref.end());
        kimix::sort(v.begin(), v.end());
        expect(v == ref) << "kimix::sort == std::sort on random int32";
        expect(std::is_sorted(v.begin(), v.end()));
    };

    "sort_dispatch_paths_patterns"_test = [] {
        // Exercises both dispatch arms and the pattern-detection paths:
        // arithmetic+std::less -> branchless; custom lambda -> branchy pdqsort.
        const std::vector<std::vector<int64_t>> cases = {
            {},
            {1},
            [] { std::vector<int64_t> v(500); for (size_t i = 0; i < v.size(); ++i) v[i] = i; return v; }(),          // sorted
            [] { std::vector<int64_t> v(500); for (size_t i = 0; i < v.size(); ++i) v[i] = v.size() - i; return v; }(), // reversed
            [] {                                                                                                       // organ pipe
                std::vector<int64_t> v(500);
                for (size_t i = 0; i < 250; ++i) v[i] = i;
                for (size_t i = 250; i < 500; ++i) v[i] = 500 - i;
                return v;
            }(),
            [] { // few distinct
                std::vector<int64_t> v(500);
                for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<int64_t>(i % 4);
                return v;
            }(),
        };
        for (const auto &c : cases) {
            std::vector<int64_t> ref = c;
            std::sort(ref.begin(), ref.end());
            auto check = [&ref](const std::vector<int64_t> &in) {
                std::vector<int64_t> v = in;
                kimix::sort(v.begin(), v.end());
                expect(v == ref) << "pattern case n=" << in.size();
                std::vector<int64_t> vc = in;
                kimix::sort(vc.begin(), vc.end(), [](int64_t a, int64_t b) { return a < b; });
                expect(vc == ref) << "pattern case (custom-less) n=" << in.size();
                std::vector<int64_t> vr = in;
                std::sort(vr.rbegin(), vr.rend()); // descending oracle
                kimix::sort(vr.begin(), vr.end(), std::greater<int64_t>());
                std::reverse(vr.begin(), vr.end());
                expect(vr == ref) << "descending dispatch n=" << in.size();
            };
            check(c);
        }
    };

    "sort_custom_comparator_stable_by_key"_test = [] {
        std::mt19937_64 rng(7);
        std::vector<payload> v(2000);
        for (uint32_t i = 0; i < v.size(); ++i) v[i] = payload{static_cast<int32_t>(rng() % 100), i};
        std::vector<payload> ref = v;
        std::sort(ref.begin(), ref.end(), [](const payload &a, const payload &b) { return a.key < b.key; });
        kimix::sort(v.begin(), v.end(), [](const payload &a, const payload &b) { return a.key < b.key; });
        expect(same_keys(v, ref)) << "struct payload + custom comparator";
        expect(std::is_sorted(v.begin(), v.end(), [](const payload &a, const payload &b) { return a.key < b.key; }));
    };

    "sort_raw_pointer_range"_test = [] {
        std::vector<uint32_t> v(333);
        for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<uint32_t>((i * 131) % 333);
        std::vector<uint32_t> ref = v;
        std::sort(ref.begin(), ref.end());
        kimix::sort(v.data(), v.data() + v.size()); // pointer iterators (wire_envelope-style call)
        expect(v == ref) << "raw pointer range";
    };

    "sort_kimix_vector"_test = [] {
        kimix::vector<int32_t> v; // the mimalloc-backed container used across the project
        for (int32_t i = 0; i < 4096; ++i) v.push_back((i * 7919) % 4096);
        kimix::sort(v.begin(), v.end());
        expect(std::is_sorted(v.begin(), v.end()));
        expect(v.size() == 4096 && v.front() == 0 && v.back() == 4095);
    };
}
