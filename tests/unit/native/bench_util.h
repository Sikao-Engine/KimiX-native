// bench_util.h — shared lightweight wall-clock benchmark harness used by the
// runtime kernel benchmarks (tests/unit/native/*_bench tests).
//
// Usage inside a Boost.UT test lambda:
//
//     "bench_utf8_count_ascii"_test = [] {
//         const std::string data(1 << 20, 'a');
//         std::size_t total = 0;
//         kimix_bench::time_op("utf8/ascii_count", [&] {
//             total += kimix::runtime::common::utf8_code_point_count(
//                 kimix::string_view(data));
//         });
//         kimix_bench::sink(total); // keep the result alive
//     };
//
// Design notes:
// - All timing output goes to stderr (std::fprintf) so it is never swallowed
//   by Boost.UT's stdout capture.
// - Every case calibrates its own iteration count on a warmup pass and then
//   measures until a minimum wall duration is reached, so numbers stay
//   comparable across machines and tiny functions still get stable timings.
// - No hard timing assertions: benchmarks must never be flaky tests.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace kimix_bench {

// Discard-optimizer escape hatch: sink the produced value so the measured
// loop cannot be eliminated as dead code.
template <typename T>
inline void sink(const T&) noexcept {}

// Runs `fn` until at least `min_duration_s` seconds of *measured* time have
// elapsed (after an unmeasured warmup/calibration pass) and prints a single
// summary line to stderr:
//   [bench] <name>  <ns/op>  <ops/s>  [<MB/s>]  (<iters> iters)
//
// - ops_per_iter: number of logical operations performed per fn() call
//   (default 1). ns/op = wall_ns / (iters * ops_per_iter).
// - bytes_per_op: processed bytes per fn() call; when > 0 an MB/s figure is
//   printed as well.
template <typename Fn>
inline void run(std::string_view name, Fn&& fn, std::size_t ops_per_iter = 1,
                double bytes_per_op = 0.0, double min_duration_s = 0.25) {
    using clock = std::chrono::steady_clock;

    // Warmup + calibration: pick an iteration count that yields >= 50ms.
    fn();
    fn();
    std::size_t iters = 1;
    for (;;) {
        auto t0 = clock::now();
        for (std::size_t i = 0; i < iters; ++i) {
            fn();
        }
        auto t1 = clock::now();
        const double sec = std::chrono::duration<double>(t1 - t0).count();
        if (sec >= 0.05 || iters >= (std::size_t{1} << 26)) {
            break;
        }
        iters = iters >= (std::size_t{1} << 22)
                    ? iters + (iters >> 1)
                    : iters * 4;
    }

    // Measured run.
    std::size_t measured = 0;
    auto t0 = clock::now();
    for (;;) {
        for (std::size_t i = 0; i < iters; ++i) {
            fn();
        }
        measured += iters;
        auto t1 = clock::now();
        if (std::chrono::duration<double>(t1 - t0).count() >= min_duration_s) {
            break;
        }
    }
    auto t1 = clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    const double total_ops = static_cast<double>(measured) *
                             static_cast<double>(ops_per_iter);
    const double ns_per_op = sec * 1e9 / total_ops;
    const double ops_per_sec = total_ops / sec;

    std::fprintf(stderr, "[bench] %-44s %12.1f ns/op %14.0f ops/s", name.data(),
                 ns_per_op, ops_per_sec);
    if (bytes_per_op > 0.0) {
        const double mbps =
            static_cast<double>(measured) * bytes_per_op / (sec * 1e6);
        std::fprintf(stderr, " %12.1f MB/s", mbps);
    }
    std::fprintf(stderr, "  (%zu iters)\n", measured);
}

// Shorthand for the common case: fn() performs one logical unit of work and
// we only care about per-op latency / throughput.
template <typename Fn>
inline void time_op(std::string_view name, Fn&& fn,
                    double min_duration_s = 0.25) {
    run(name, static_cast<Fn&&>(fn), 1, 0.0, min_duration_s);
}

} // namespace kimix_bench