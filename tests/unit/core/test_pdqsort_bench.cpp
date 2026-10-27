// Benchmark for the vendored pdqsort header (src/ext/pdqsort/pdqsort.h,
// re-exported through core/stl/pdqsort.h) versus std::sort.
// This test covers:
// - correctness: ::pdqsort / ::pdqsort_branchless produce the same sorted
//   output as std::sort on every workload. (std::sort here IS MSVC's own
//   pdqsort variant, which is the point of the comparison.)
// - performance: median wall time of copy+sort iterations over several input
//   distributions (random, sorted, nearly-sorted, organ-pipe, few distinct
//   values) and sizes (1k..4M) for int32_t keys in a kimix::vector (mimalloc),
//   comparing std::sort, the raw ::pdqsort / ::pdqsort_branchless entry points
//   and kimix::sort (the dispatch wrapper the project now uses: branchless for
//   arithmetic+default-compare, plain pdqsort otherwise).
// Gated: only runs when the environment variable KIMIX_PDQSORT_BENCH=1 is set,
// so `xmake test` stays fast (without it the binary registers nothing and
// exits). Run with:
// xmake build test_stl_pdqsort_bench
// set KIMIX_PDQSORT_BENCH=1 && xmake run test_stl_pdqsort_bench
// Methodology notes:
// - A pristine source vector is built ONCE per case (fixed seed => the exact
//   same input for every algorithm and every iteration); each timed iteration
//   memcpy's the pristine source into a scratch buffer and sorts the copy, so
//   no case ever measures a pre-sorted array and all three algorithms see
//   byte-identical inputs.
// - No timing assertions: a benchmark that fails on a slow CI machine is a
//   broken benchmark. The only expects() are sortedness/checksum checks.
// - Output goes to stderr as [bench] lines (median of the per-iteration
//   samples + MiB/s + a ratio table).
#include "ut/ut.hpp"
#include "unit/native/bench_util.h" // sink() + shared bench conventions

#include <core/kimix_core.h> // kimix::vector + stl/pdqsort.h forwarding include

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace tests_unit_core_test_pdqsort_bench {

using clock = std::chrono::steady_clock;

enum class dist_kind { random, sorted, nearly_sorted, organ_pipe, few_distinct };

const char *dist_name(dist_kind d) {
    switch (d) {
    case dist_kind::random: return "random";
    case dist_kind::sorted: return "sorted";
    case dist_kind::nearly_sorted: return "nearly_sorted";
    case dist_kind::organ_pipe: return "organ_pipe";
    case dist_kind::few_distinct: return "few_distinct";
    }
    return "?";
}

kimix::vector<int32_t> make_input(dist_kind d, size_t n, uint64_t seed) {
    kimix::vector<int32_t> v(n);
    std::mt19937_64 rng(seed);
    switch (d) {
    case dist_kind::random:
        for (auto &x : v) x = static_cast<int32_t>(rng());
        break;
    case dist_kind::sorted:
        for (size_t i = 0; i < n; ++i) v[i] = static_cast<int32_t>(i);
        break;
    case dist_kind::nearly_sorted: {
        for (size_t i = 0; i < n; ++i) v[i] = static_cast<int32_t>(i);
        const size_t swaps = n / 100 + 1; // ~1 % of positions disturbed
        for (size_t s = 0; s < swaps; ++s) {
            size_t a = rng() % n, b = rng() % n;
            std::swap(v[a], v[b]);
        }
        break;
    }
    case dist_kind::organ_pipe: {
        const size_t h = n / 2;
        for (size_t i = 0; i < h; ++i) v[i] = static_cast<int32_t>(i);
        for (size_t i = h; i < n; ++i) v[i] = static_cast<int32_t>(n - i);
        break;
    }
    case dist_kind::few_distinct:
        for (auto &x : v) x = static_cast<int32_t>(rng() % 8);
        break;
    }
    return v;
}

int64_t sort_hash(const kimix::vector<int32_t> &v) { // order-sensitive checksum
    uint64_t h = 1469598103934665603ull;
    for (const auto &x : v) {
        h = (h ^ static_cast<uint64_t>(static_cast<uint32_t>(x))) * 1099511628211ull;
    }
    return static_cast<int64_t>(h);
}

struct result {
    double median_s;
    kimix::vector<int32_t> sorted; // last measured output (for correctness)
};

struct algo_times {
    double std_sort;
    double pdqsort;
    double pdqsort_branchless;
    double kimix_sort;
};

// One warmup sort + `iters` measured iterations (each copies the pristine
// source then sorts the copy); median of the per-iteration wall times.
template <typename SortFn>
result bench_sort(const kimix::vector<int32_t> &src, SortFn sort_fn, int iters) {
    kimix::vector<int32_t> work(src);
    sort_fn(work); // warmup
    std::vector<double> samples;
    samples.reserve(iters);
    for (int i = 0; i < iters; ++i) {
        std::memcpy(work.data(), src.data(), src.size() * sizeof(int32_t));
        const auto t0 = clock::now();
        sort_fn(work);
        const auto t1 = clock::now();
        samples.push_back(std::chrono::duration<double>(t1 - t0).count());
    }
    std::sort(samples.begin(), samples.end());
    return {samples[samples.size() / 2], std::move(work)};
}

void report(const char *algo, const char *dname, size_t n, const result &r,
            double bytes) {
    const double mib_per_s = (bytes / (1024.0 * 1024.0)) / r.median_s;
    std::fprintf(stderr, "[bench] %-20s %-14s n=%-9zu %10.3f ms %10.1f MiB/s\n",
                 algo, dname, n, r.median_s * 1e3, mib_per_s);
}

} // namespace tests_unit_core_test_pdqsort_bench
using namespace tests_unit_core_test_pdqsort_bench;

int main(int argc, char **argv) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));
    const char *gate = std::getenv("KIMIX_PDQSORT_BENCH");
    if (gate == nullptr || gate[0] == '0') {
        std::fprintf(stderr, "[bench] KIMIX_PDQSORT_BENCH not set; skipping.\n");
        return 0;
    }

    "pdqsort_vs_std_sort"_test = [] {
        const size_t sizes[] = {1000, 10000, 100000, 1000000, 4000000};
        const dist_kind dists[] = {dist_kind::random, dist_kind::sorted,
                                   dist_kind::nearly_sorted, dist_kind::organ_pipe,
                                   dist_kind::few_distinct};

        std::fprintf(stderr,
                     "[bench] ========== vendored pdqsort vs std::sort (int32_t, kimix::vector) ==========\n");

        for (const size_t n : sizes) {
            const int iters = n >= 1000000 ? 5 : (n >= 100000 ? 15 : 60);
            const double bytes = static_cast<double>(n) * sizeof(int32_t);
            for (const dist_kind d : dists) {
                const uint64_t seed = 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(n) << 8) ^ static_cast<uint64_t>(d);
                const auto src = make_input(d, n, seed);

                // correctness reference: std::sort on its own copy
                kimix::vector<int32_t> ref(src);
                std::sort(ref.begin(), ref.end());

                auto r_std = bench_sort(src, [](kimix::vector<int32_t> &v) {
                    std::sort(v.begin(), v.end());
                }, iters);
                auto r_pdq = bench_sort(src, [](kimix::vector<int32_t> &v) {
                    ::pdqsort(v.begin(), v.end());
                }, iters);
                auto r_bl = bench_sort(src, [](kimix::vector<int32_t> &v) {
                    ::pdqsort_branchless(v.begin(), v.end());
                }, iters);
                auto r_kimix = bench_sort(src, [](kimix::vector<int32_t> &v) {
                    kimix::sort(v.begin(), v.end());
                }, iters);

                // All three must produce the same fully-sorted sequence
                // (int32 keys: sorted order is unique, so checksums must match).
                expect(sort_hash(r_std.sorted) == sort_hash(ref)) << "std::sort checksum " << dist_name(d) << "/" << n;
                expect(sort_hash(r_pdq.sorted) == sort_hash(ref)) << "pdqsort checksum " << dist_name(d) << "/" << n;
                expect(sort_hash(r_bl.sorted) == sort_hash(ref)) << "pdqsort_branchless checksum " << dist_name(d) << "/" << n;
                expect(sort_hash(r_kimix.sorted) == sort_hash(ref)) << "kimix::sort checksum " << dist_name(d) << "/" << n;
                expect(std::is_sorted(r_pdq.sorted.begin(), r_pdq.sorted.end())) << "pdqsort sorted " << dist_name(d);

                report("std::sort", dist_name(d), n, r_std, bytes);
                report("pdqsort", dist_name(d), n, r_pdq, bytes);
                report("pdqsort_branchless", dist_name(d), n, r_bl, bytes);
                report("kimix::sort", dist_name(d), n, r_kimix, bytes);
                std::fprintf(stderr, "[bench]   -> speedup pdqsort/std %5.2fx   branchless/std %5.2fx   kimix/std %5.2fx (>1 = faster than std::sort)\n",
                             r_std.median_s / r_pdq.median_s,
                             r_std.median_s / r_bl.median_s,
                             r_std.median_s / r_kimix.median_s);

                kimix_bench::sink(r_std.median_s + r_pdq.median_s + r_bl.median_s + r_kimix.median_s);
            }
        }
    };
}
