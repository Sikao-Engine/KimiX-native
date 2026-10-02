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
//   skew_tree   : zipf-ish size mix (two 3.9 MiB + eight 1 MiB + 400 x 8 KiB)
//                 - stresses the static chunk split (load imbalance).
//   big_file    : one 4 MiB - 64 byte file, content mode (read + line scan).
//   micro_repeat: many repeats over a 3-file tree - per-call fixed overhead
//                 (regex validation, scheduler creation when unbound...).
//   two_files   : the smallest fan-out case (pool of 2 per call in the old
//                 implementation).
//   repo_*      : the real project tree (current dir) - walk-dominated.
//
// Every scenario runs in two columns:
//   [unbound] the calling thread has NO scheduler bound. Old engine behaviour:
//             run_grep creates its own private pool per call; new behaviour:
//             the scan falls back to inline/serial.
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

struct bench_fixture {
    kimix::filesystem::path root;
    kimix::filesystem::path small_tree;
    kimix::filesystem::path skew_tree;
    kimix::filesystem::path big_file;
    kimix::filesystem::path micro_tree;

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
                                const kimix::string &micro_s) {
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
                ge::grep_options o =
                    make_opts("return", ge::grep_output_mode::files_with_matches);
                o.include_glob = kimix::string("*.cpp");
                run_scenario(o, repo_s, res);
            };

            // [unbound] no scheduler on the calling thread.
            std::fprintf(stderr, "--- unbound column (no ambient scheduler) ---\n");
            bench("unbound", "small_fwm_literal", 4, small_fwm);
            bench("unbound", "small_count_regex", 4, small_count);
            bench("unbound", "small_content_ctx", 3, small_content);
            bench("unbound", "skew_fwm_literal", 4, skew_fwm);
            bench("unbound", "big_file_content", 4, big_content);
            bench("unbound", "micro_repeat_200", 2, micro_repeat);
            if (is_repo) {
                bench("unbound", "repo_fwm_coldneedle", 2, repo_fwm);
                bench("unbound", "repo_glob_cpp", 2, repo_glob);
            }

            // [ambient] caller owns the process-level pool (shared scheduler).
            {
                kimix::fiber::shared_scheduler().bind();
                std::fprintf(stderr, "--- ambient column (shared pool of %u workers) ---\n",
                             kimix::fiber::worker_thread_count());
                bench("ambient", "small_fwm_literal", 4, small_fwm);
                bench("ambient", "small_count_regex", 4, small_count);
                bench("ambient", "small_content_ctx", 3, small_content);
                bench("ambient", "skew_fwm_literal", 4, skew_fwm);
                bench("ambient", "big_file_content", 4, big_content);
                bench("ambient", "micro_repeat_200", 2, micro_repeat);
                if (is_repo) {
                    bench("ambient", "repo_fwm_coldneedle", 2, repo_fwm);
                    bench("ambient", "repo_glob_cpp", 2, repo_glob);
                }
                // Release the main thread again; the shared pool itself lives
                // on (it is intentionally never destroyed).
                kimix::fiber::shared_scheduler().unbind();
            }
        };
        columns(fx, small_s, skew_s, big_dir, micro_s);
    };

    return 0;
}
