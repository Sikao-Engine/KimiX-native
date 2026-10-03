// Benchmark for builtin_tools/grep_engine.{h,cpp} (kimix::builtin_tools::grep).
//
// Gated: only runs when the environment variable KIMIX_GREP_BENCH=1 is set, so
// `xmake test` stays fast (without it the binary registers nothing and exits).
// Run with:
//   xmake build test_builtin_grep_engine_bench
//   set KIMIX_GREP_BENCH=1 && xmake run test_builtin_grep_engine_bench
//
// Scenario families (each measures run_grep end-to-end wall time, median of
// several iterations after one warmup pass):
//   small_tree  : 2000 tiny files, literal / regex / content modes.
// skew_tree : zipf-ish size mix (two 3.9 MiB + eight 1 MiB + 400 x 8 KiB)
// - stresses the chunk ASSIGNMENT (load imbalance). Before the LPT change the
// list was cut into contiguous index ranges, so chunk 0 drew huge1 + huge2 +
// all eight 1 MiB files (walk order puts them first, 15.8 of the 19 MiB) while
// the other 31 chunks shared the 400 tiny ones: the makespan was that one
// chunk. skew_fwm_literal is the historical probe (pure read + scan, where the
// split is all that matters: 29.5 ms static vs 13.9 ms LPT on a 32-worker
// machine, the predicted ~2.1x); balanced_skew_content_ctx is the same corpus
// in content mode with -B/-A (29 088 rendered lines): 170 ms vs 156 ms there,
// only ~8%, because a content run is not makespan-bound - its wall time sits
// in the per-hit format/allocate throughput, which the balance cannot hide.
// Both print in the [unbound] and [ambient] columns, and there are no timing
// assertions.
// big_file : one 4 MiB - 64 byte file, content mode (read + line scan).
// prefilter_tree : 320 files where only ~1% carry a rare literal - the
// required-literal prefilter (and the multi-literal alternation path) decides
// such a file with memchr passes instead of running the backtracker on every
// line. Each pair is the SAME search once with and once without an extractable
// literal (a group or a character class defeats the extractor), so the two
// columns differ only by the prefilter and must report identical counts.
// micro_repeat: many repeats over a 3-file tree - per-call fixed overhead
//                 (regex validation, the transient shared-pool bind when
//                 unbound...).
//   two_files   : the smallest fan-out case (pool of 2 per call in the old
//                 implementation).
//   repo_*      : the real project tree (current dir) - walk-dominated.
//
// Every scenario runs in two columns:
//   [unbound] the calling thread has NO scheduler bound. run_grep binds the
//             process-wide shared pool for the duration of the call (an RAII
//             guard; it never creates a scheduler), so a tree of >= 8 files
//             fans out over the shared workers just like the ambient column -
//             what this column adds on top is the per-call bind/unbind cost
//             (one single-threaded marl worker). Trees below the 8-file
//             fan-out threshold still run the single inline chunk.
// [ambient] the calling thread is bound to the process-wide shared pool by
// the CALLER (the "bind once at the root main" model). Old engine: a
// nested scope is a no-op, jobs spread over the shared pool; new engine:
// uses the ambient pool directly.
// No hard timing assertions (benchmarks must never be flaky tests) but each
// scenario sanity-checks that it matched something.

#include "ut/ut.hpp"
#include "builtin_tools/grep_engine.h"

#include <core/fiber.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace ge = kimix::builtin_tools::grep;

namespace {

// ---------------------------------------------------------------------------
// timing helpers
// ---------------------------------------------------------------------------

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    if (v.empty()) {
        return 0.0;
    }
    return v[v.size() / 2];
}

template <typename F>
void bench(const char *tag, const char *name, int iters, F &&fn) {
    using clock = std::chrono::steady_clock;
    fn(); // warmup (FS cache, code paths)
    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(iters));
    for (int i = 0; i < iters; ++i) {
        const auto t0 = clock::now();
        fn();
        const auto t1 = clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::fprintf(stderr, "[bench] %-10s %-36s %12.2f ms\n", tag, name,
                 median(samples));
}

// ---------------------------------------------------------------------------
// corpus generation
// ---------------------------------------------------------------------------

void put_file(const kimix::filesystem::path &p, const kimix::string &text) {
    std::error_code ec;
    kimix::filesystem::create_directories(p.parent_path(), ec);
    std::FILE *f = std::fopen(kimix::to_string(p).c_str(), "wb");
    if (f == nullptr) {
        return;
    }
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

// Log/code shaped text, roughly one "needle_42" line every ~37 lines.
kimix::string gen_text(size_t lines, uint64_t seed) {
    kimix::string t;
    t.reserve(lines * 64u);
    uint64_t x = seed * 0x9E3779B97F4A7C15ull + 12345u;
    static const char *frags[] = {
        "    int rc = handle_request(ctx, value);",
        "    log::debug(\"cache miss for key {}\", key);",
        "    return compute_hash(buffer.data(), buffer.size());",
        "  expect(result.status == tool_status::ok) << msg;",
        "    for (size_t i = 0; i < items.size(); ++i) {",
        "// TODO: refactor the allocator path later",
        "const char *name = lookup_symbol(table, idx);",
        "    dispatch(workflow_id, step, payload);"};
    for (size_t l = 0; l < lines; ++l) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        const size_t fi = static_cast<size_t>((x >> 33) % 8u);
        t += frags[fi];
        if ((l % 37u) == (static_cast<size_t>(x >> 17) % 13u) % 37u) {
            t += " // needle_42";
        }
        t += "\n";
    }
    return t;
}

// Code/log shaped text; `rare` files carry the needle tokens, the rest never
// do (the ~1% hit rate a required-literal prefilter lives off).
kimix::string gen_pre_text(size_t lines, uint64_t seed, bool rare) {
    kimix::string t;
    t.reserve(lines * 72u);
    uint64_t x = seed * 0x9E3779B97F4A7C15ull + 777u;
    static const char *frags[] = {
        " int rc = handle_request(ctx, value);",
        " log::debug(\"cache miss for key {}\", key);",
        " return compute_hash(buffer.data(), buffer.size());",
        " expect(result.status == tool_status::ok) << msg;",
        " for (size_t i = 0; i < items.size(); ++i) {",
        "// TODO: refactor the allocator path later",
        "const char *name = lookup_symbol(table, idx);",
        " dispatch(workflow_id, step, payload);"};
    for (size_t l = 0; l < lines; ++l) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        t += frags[static_cast<size_t>((x >> 33) % 8u)];
        if (rare && (l % 61u) == static_cast<size_t>((x >> 11) % 61u)) {
            t += " // RareLiteral found deep in the detail section";
            t += " RareWord_ALPHA with RareWord_BETA and RareWord_GAMMA here";
        }
        t += "\n";
    }
    return t;
}

struct bench_fixture {
    kimix::filesystem::path root;
    kimix::filesystem::path small_tree;
    kimix::filesystem::path skew_tree;
    kimix::filesystem::path big_file;
    kimix::filesystem::path micro_tree;
    kimix::filesystem::path pre_tree;

    void build() {
        namespace fs = kimix::filesystem;
        std::error_code ec;
        root = fs::temp_directory_path(ec) / "kimix_grep_engine_bench";
        fs::remove_all(root, ec);
        // 2000 small files, 20 dirs x 100 files, ~45 lines each (~2.5 KiB).
        small_tree = root / "small_tree";
        for (int d = 0; d < 20; ++d) {
            for (int f = 0; f < 100; ++f) {
                char name[64];
                std::snprintf(name, sizeof(name), "d%02d/f%04d.cpp", d, f);
                put_file(small_tree / name, gen_text(45, static_cast<uint64_t>(d) * 1000u +
                                                                static_cast<uint64_t>(f)));
            }
        }
        // Skew: two near-cap files + eight 1 MiB + 400 tiny.
        skew_tree = root / "skew_tree";
        put_file(skew_tree / "huge1.txt", gen_text(62000, 7701)); // ~3.9 MiB
        put_file(skew_tree / "huge2.txt", gen_text(62000, 7702));
        for (int i = 0; i < 8; ++i) {
            char name[64];
            std::snprintf(name, sizeof(name), "mid%d.txt", i);
            put_file(skew_tree / name, gen_text(16000, static_cast<uint64_t>(i) + 500));
        }
        for (int i = 0; i < 400; ++i) {
            char name[64];
            std::snprintf(name, sizeof(name), "tiny/t%03d.txt", i);
            put_file(skew_tree / name, gen_text(90, static_cast<uint64_t>(i) + 9000));
        }
        // One big file just below the 4 MiB cap.
        big_file = root / "big/big.txt";
        put_file(big_file, gen_text(65000, 424242));
        // Micro tree: 3 small files (per-call overhead scenario).
        micro_tree = root / "micro";
        put_file(micro_tree / "a.txt", gen_text(60, 1));
        put_file(micro_tree / "b.txt", gen_text(60, 2));
        put_file(micro_tree / "c.txt", gen_text(60, 3));
        // Prefilter tree: 16 dirs x 20 files (~1500 lines, ~90 KiB each), only
        // every 100th file (3 of 320) carries the rare literals. The files are
        // text-heavy on purpose: with tiny files the walk + fopen/fread cost
        // dominates and the prefilter win is invisible.
        pre_tree = root / "pre_tree";
        for (int d = 0; d < 16; ++d) {
            for (int f = 0; f < 20; ++f) {
                char name[64];
                std::snprintf(name, sizeof(name), "p%d/f%02d.cpp", d, f);
                const int idx = d * 20 + f;
                put_file(pre_tree / name,
                         gen_pre_text(1500, static_cast<uint64_t>(idx) + 31337u,
                                      (idx % 100) == 3));
            }
        }
    }
    ~bench_fixture() {
        std::error_code ec;
        kimix::filesystem::remove_all(root, ec);
    }
};

ge::grep_options make_opts(const char *pat, ge::grep_output_mode mode) {
    ge::grep_options o;
    o.pattern = kimix::string(pat);
    o.mode = mode;
    o.head_limit = 250;
    return o;
}

kimix::string p_str(const kimix::filesystem::path &p) { return kimix::to_string(p); }

void run_scenario(ge::grep_options &o, const kimix::string &root_s, ge::grep_result &out) {
    const kimix::string work_dir;
    kimix::vector<kimix::string> roots;
    roots.push_back(root_s);
    ge::run_grep(o, roots, kimix::string_view(work_dir), out);
}

// Every scenario must find something (the bench never measures an empty walk).
void sanity(const ge::grep_result &out, const char *name) {
    expect(out.status == kimix::builtin_tools::tool_status::ok) << name;
    expect(out.total_matches > 0) << name;
}

} // namespace

int main(int argc, char **argv) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));
    const char *gate = std::getenv("KIMIX_GREP_BENCH");
    if (gate == nullptr || gate[0] == '0') {
        std::fprintf(stderr, "[bench] KIMIX_GREP_BENCH not set; skipping.\n");
        return 0;
    }

    bench_fixture fx;
    fx.build();

    "grep_engine_bench"_test = [&fx] {
        const kimix::string small_s = p_str(fx.small_tree);
        const kimix::string skew_s = p_str(fx.skew_tree);
        const kimix::string big_dir = p_str(fx.big_file.parent_path());
        const kimix::string micro_s = p_str(fx.micro_tree);
        const kimix::string pre_s = p_str(fx.pre_tree);

        // ---- one pass of correctness sanity for each mode (both engines) ----
        {
            ge::grep_result o1;
            ge::grep_options a = make_opts("needle_42", ge::grep_output_mode::files_with_matches);
            run_scenario(a, small_s, o1);
            sanity(o1, "small_fwm");
            ge::grep_result o2;
            ge::grep_options b = make_opts("n[e]edle_4[0-9]", ge::grep_output_mode::count_matches);
            run_scenario(b, small_s, o2);
            expect(o2.total_matches == o1.total_matches) << "literal vs regex counts";
        }

        const auto columns = [](bench_fixture &f, const kimix::string &small_s,
                                const kimix::string &skew_s, const kimix::string &big_dir,
                                const kimix::string &micro_s, const kimix::string &pre_s) {
            ge::grep_result res;

            // 1) 2000 small files, literal, files_with_matches
            auto small_fwm = [&] {
                ge::grep_options o =
                    make_opts("needle_42", ge::grep_output_mode::files_with_matches);
                run_scenario(o, small_s, res);
            };
            // 2) same tree, regex, count_matches
            auto small_count = [&] {
                ge::grep_options o =
                    make_opts("n[e]edle_4[0-9]", ge::grep_output_mode::count_matches);
                run_scenario(o, small_s, res);
            };
            // 3) same tree, content mode with context
            auto small_content = [&] {
                ge::grep_options o =
                    make_opts("needle_42", ge::grep_output_mode::content);
                o.ctx_before = 2;
                o.ctx_after = 2;
                o.head_limit = 0;
                run_scenario(o, small_s, res);
            };
            // 4) skew corpus: static-split imbalance probe
            auto skew_fwm = [&] {
                ge::grep_options o =
                    make_opts("needle_42", ge::grep_output_mode::files_with_matches);
                run_scenario(o, skew_s, res);
            };
            // 5) single big file: read + scan bandwidth
            auto big_content = [&] {
                ge::grep_options o =
                    make_opts("needle_42", ge::grep_output_mode::content);
                o.head_limit = 0;
                run_scenario(o, big_dir, res);
            };
            // 4b) the SAME skew corpus, content mode with -B/-A: the case where
            // a size-blind split hurts most (the huge chunk also renders the
            // most lines). Comparable to skew_fwm_literal on the same tree.
            auto balanced_skew = [&] {
                ge::grep_options o =
                    make_opts("needle_42", ge::grep_output_mode::content);
                o.ctx_before = 1;
                o.ctx_after = 1;
                o.head_limit = 0;
                run_scenario(o, skew_s, res);
                sanity(res, "balanced_skew_content");
            };
            // 6) micro tree repeated: per-call fixed cost
            auto micro_repeat = [&] {
                for (int i = 0; i < 200; ++i) {
                    ge::grep_options o =
                        make_opts("needle_42", ge::grep_output_mode::files_with_matches);
                    run_scenario(o, micro_s, res);
                }
            };
            // 7) real project tree (walk-heavy), found by walking up from cwd
            kimix::filesystem::path repo = kimix::filesystem::current_path();
            for (int up = 0; up < 6; ++up) {
                if (kimix::filesystem::exists(repo / "src/builtin_tools/grep_engine.cpp")) {
                    break;
                }
                repo = repo.parent_path();
            }
            const bool is_repo =
                kimix::filesystem::exists(repo / "src/builtin_tools/grep_engine.cpp");
            const kimix::string repo_s = is_repo ? p_str(repo) : kimix::string();
            auto repo_fwm = [&] {
                ge::grep_options o =
                    make_opts("needle_42_unlikely_zz", ge::grep_output_mode::files_with_matches);
                run_scenario(o, repo_s, res);
            };
            auto repo_glob = [&] {
                ge::grep_options o = make_opts("return", ge::grep_output_mode::files_with_matches);
                o.include_glob = kimix::string("*.cpp");
                run_scenario(o, repo_s, res);
            };
            // 8) prefilter_tree: 320 files where only ~1% carry the rare literals.
            // "RareLiteral.*detail" exposes "detail" as a required run, so a file without
            // it is settled by memchr and the backtracker never runs on its lines; the same
            // search written with a leading group - "(RareLiteral).*detail" - defeats the
            // extractor and pays the engine line by line. Same pair for the literal
            // alternation versus its single-character-class spelling (which bails out).
            auto pre_fwm = [&] {
                ge::grep_options o =
                    make_opts("RareLiteral.*detail", ge::grep_output_mode::files_with_matches);
                run_scenario(o, pre_s, res);
                sanity(res, "pre_fwm");
            };
            auto pre_ungated = [&] {
                ge::grep_options o =
                    make_opts("(RareLiteral).*detail", ge::grep_output_mode::files_with_matches);
                run_scenario(o, pre_s, res);
            };
            auto pre_content = [&] {
                ge::grep_options o =
                    make_opts("RareLiteral.*detail", ge::grep_output_mode::content);
                o.head_limit = 0;
                run_scenario(o, pre_s, res);
            };
            auto pre_alt = [&] {
                ge::grep_options o = make_opts(
                    "RareWord_ALPHA|RareWord_BETA|RareWord_GAMMA|RareWord_DELTA",
                    ge::grep_output_mode::files_with_matches);
                run_scenario(o, pre_s, res);
                sanity(res, "pre_alt");
            };
            auto pre_alt_ungated = [&] {
                ge::grep_options o = make_opts(
                    "[R]areWord_ALPHA|[R]areWord_BETA|[R]areWord_GAMMA|[R]areWord_DELTA",
                    ge::grep_output_mode::files_with_matches);
                run_scenario(o, pre_s, res);
            };
            // A fast path that answers differently is a bug, not a benchmark: pin that
            // the gated and the ungated spelling of each search agree.
            auto pre_total = [&](const char *pat) {
                ge::grep_options o = make_opts(pat, ge::grep_output_mode::files_with_matches);
                ge::grep_result r;
                run_scenario(o, pre_s, r);
                return r.total_matches;
            };
            expect(pre_total("RareLiteral.*detail") > 0);
            expect(pre_total("RareLiteral.*detail") == pre_total("(RareLiteral).*detail"))
                << "prefilter must not change the answer";
            expect(pre_total("RareWord_ALPHA|RareWord_BETA|RareWord_GAMMA|RareWord_DELTA") ==
                       pre_total("[R]areWord_ALPHA|[R]areWord_BETA|[R]areWord_GAMMA|"
                                 "[R]areWord_DELTA"))
                << "alternation fast path must not change the answer";
            
            // [unbound] no scheduler on the calling thread.
            std::fprintf(stderr, "--- unbound column (no ambient scheduler) ---\n");
            bench("unbound", "small_fwm_literal", 4, small_fwm);
            bench("unbound", "small_count_regex", 4, small_count);
            bench("unbound", "small_content_ctx", 3, small_content);
            bench("unbound", "skew_fwm_literal", 4, skew_fwm);
            bench("unbound", "balanced_skew_content_ctx", 4, balanced_skew);
            bench("unbound", "big_file_content", 4, big_content);
            bench("unbound", "micro_repeat_200", 2, micro_repeat);
            if (is_repo) {
                bench("unbound", "repo_fwm_coldneedle", 2, repo_fwm);
                bench("unbound", "repo_glob_cpp", 2, repo_glob);
            }
            bench("unbound", "pre_fwm_gated", 4, pre_fwm);
            bench("unbound", "pre_fwm_ungated_regex", 4, pre_ungated);
            bench("unbound", "pre_content_gated", 3, pre_content);
            bench("unbound", "pre_alt_gated", 4, pre_alt);
            bench("unbound", "pre_alt_ungated_regex", 4, pre_alt_ungated);
            
            // [ambient] caller owns the process-level pool (shared scheduler).
            {
                kimix::fiber::shared_scheduler().bind();
                std::fprintf(stderr, "--- ambient column (shared pool of %u workers) ---\n",
                              kimix::fiber::worker_thread_count());
                bench("ambient", "small_fwm_literal", 4, small_fwm);
                bench("ambient", "small_count_regex", 4, small_count);
                bench("ambient", "small_content_ctx", 3, small_content);
                bench("ambient", "skew_fwm_literal", 4, skew_fwm);
                bench("ambient", "balanced_skew_content_ctx", 4, balanced_skew);
                bench("ambient", "big_file_content", 4, big_content);
                bench("ambient", "micro_repeat_200", 2, micro_repeat);
                if (is_repo) {
                    bench("ambient", "repo_fwm_coldneedle", 2, repo_fwm);
                    bench("ambient", "repo_glob_cpp", 2, repo_glob);
                }
                bench("ambient", "pre_fwm_gated", 4, pre_fwm);
                bench("ambient", "pre_fwm_ungated_regex", 4, pre_ungated);
                bench("ambient", "pre_content_gated", 3, pre_content);
                bench("ambient", "pre_alt_gated", 4, pre_alt);
                bench("ambient", "pre_alt_ungated_regex", 4, pre_alt_ungated);
                // Release the main thread again; the shared pool itself lives
                // on (it is intentionally never destroyed).
                kimix::fiber::shared_scheduler().unbind();
            }
        };
        columns(fx, small_s, skew_s, big_dir, micro_s, pre_s);
    };

    return 0;
}
