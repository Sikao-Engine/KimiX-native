// Test for builtin_tools/grep_engine.h + .cpp (kimix::builtin_tools::grep).
//
// The grep engine drives the native_io branch of the Grep tool: a
// ripgrep-inspired pure-C++ search (whole-buffer zero-copy line scan, literal
// fast path, per-thread regexes over static chunks, 64 KiB NUL binary sniff,
// 4 MiB per-file cap). These suites pin its observable semantics:
//
//   literal path    pure-literal patterns ("hit") take the memchr fast path
//                   and agree with the regex paths ("h.t", "[h]it") on
//                   counts, files and rendered lines; a literal containing
//                   '\n' never matches (per-line scan, like the regex scan)
//   output modes    files_with_matches (path only, head_limit-capped lines but
//                   complete files[]), count_matches ("path:count", matching
//                   LINES not occurrences), content ("path:LN:text")
//   matching        -i case folding (literal and regex path), case-sensitive
//                   default, invalid pattern -> invalid_input status
//   context         -B/-A/-C run rendering, "--" between disjoint runs, run
//                   merging, clamping at file start/end
//   selection       include-glob over the file name, hidden file/dir skip at
//                   every depth, binary sniff (NUL in first 64 KiB skipped;
//                   a NUL past 64 KiB is searched), > 4 MiB files skipped
//   plumbing        relative roots resolve against work_dir, CRLF '\r'
//                   stripping, deterministic multi-file ordering (run twice)

#include "ut/ut.hpp"

#include "builtin_tools/grep_engine.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;
namespace ge = kimix::builtin_tools::grep;

namespace {

// Temp-dir corpus, written with std::fopen like the contract test does.
struct fixture {
    kimix::filesystem::path root;

    static void put(const kimix::filesystem::path &p, const char *text) {
        std::FILE *f = std::fopen(kimix::to_string(p).c_str(), "wb");
        expect(f != nullptr);
        if (f != nullptr) {
            std::fwrite(text, 1, std::strlen(text), f);
            std::fclose(f);
        }
    }

    static void put_bytes(const kimix::filesystem::path &p, const char *data, size_t len) {
        std::FILE *f = std::fopen(kimix::to_string(p).c_str(), "wb");
        expect(f != nullptr);
        if (f != nullptr) {
            std::fwrite(data, 1, len, f);
            std::fclose(f);
        }
    }

    static void put_repeat(const kimix::filesystem::path &p, char c, size_t count,
                           const char *tail, size_t tail_len) {
        std::FILE *f = std::fopen(kimix::to_string(p).c_str(), "wb");
        expect(f != nullptr);
        if (f == nullptr) {
            return;
        }
        char buf[8192];
        std::memset(buf, c, sizeof(buf));
        while (count > 0) {
            const size_t n = count < sizeof(buf) ? count : sizeof(buf);
            std::fwrite(buf, 1, n, f);
            count -= n;
        }
        std::fwrite(tail, 1, tail_len, f);
        std::fclose(f);
    }

    fixture() {
        namespace fs = kimix::filesystem;
        std::error_code ec;
        root = fs::temp_directory_path(ec) / "kimix_grep_engine_test";
        fs::remove_all(root, ec);
        fs::create_directories(root / "sub" / ".hid", ec);
        put(root / "a.txt", "hit\nmiss\nend\n");
        put(root / "b.txt", "foo HIT\nbar\nbaz hit\n");
        put(root / "double.txt", "hit hit\nnone\n");
        put(root / "cr.txt", "hit\r\nmiss\r\n");
        put(root / "ctx.txt", "hit\nmid1\nmid2\nhit\ntail\n");
        put(root / ".hidden.txt", "hit\n");           // hidden file: skipped
        put_bytes(root / "big.bin", "\0hit\n", 5);    // NUL in first 64 KiB: skipped
        put(root / "sub" / "c.py", "alpha\nhit\ngamma\n");
        put(root / "sub" / "c.txt", "hit\n");
        put(root / "sub" / "d.py", "hit\n");
        put(root / "sub" / ".hid" / "h.txt", "hit\n"); // hidden dir: not descended
        // NUL only AFTER the first 64 KiB: the read-buffer sniff does not
        // trigger, the file is searched (and its line contains "hit").
        put_repeat(root / "late.bin", 'x', 64 * 1024, "\0hit\n", 5);
        // Stat size above the 4 MiB cap: skipped even though it contains "hit".
        put_repeat(root / "big.txt", 'x', 4 * 1024 * 1024, "hit\n", 4);
#if defined(_WIN32) || defined(_WIN64)
        // Directory whose name contains code points the active ANSI code page
        // cannot represent (private-use U+F03A / U+F05C - the exact name that
        // crashed a live CLI run when grep's walk converted it with
        // path::string()). Created through the wide interface; the walk must
        // degrade lossily instead of terminating the process.
        const kimix::filesystem::path weird =
            root / std::wstring(L"weird_\xF03A\xF05C_dir");
        fs::create_directories(weird, ec);
        std::FILE *wf = _wfopen((weird / L"inner.txt").c_str(), L"wb");
        expect(wf != nullptr);
        if (wf != nullptr) {
            std::fwrite("hit\n", 1, 4, wf);
            std::fclose(wf);
        }
#endif
    }

    ~fixture() {
        std::error_code ec;
        kimix::filesystem::remove_all(root, ec);
    }

    kimix::string p(const char *rel) const {
        namespace fs = kimix::filesystem;
        return kimix::to_string(fs::path(rel).is_relative() ? root / rel
                                                            : fs::path(rel));
    }
};

ge::grep_options make_opts(const char *pattern) {
    ge::grep_options o;
    o.pattern = kimix::string(pattern);
    return o;
}

ge::grep_result run_opts(const ge::grep_options &o, const fixture &fx,
                         kimix::vector<kimix::string> roots) {
    const kimix::string work_dir = kimix::to_string(fx.root);
    ge::grep_result out;
    ge::run_grep(o, roots, kimix::string_view(work_dir), out);
    return out;
}

// Single-root convenience overload (absolute root path).
ge::grep_result run_root(const ge::grep_options &o, const fixture &fx) {
    kimix::vector<kimix::string> roots;
    roots.push_back(kimix::to_string(fx.root));
    return run_opts(o, fx, std::move(roots));
}

// Roots resolved against work_dir (relative entries).
ge::grep_result run_rel(const ge::grep_options &o, const fixture &fx, const char *rel) {
    kimix::vector<kimix::string> roots;
    roots.emplace_back(rel);
    return run_opts(o, fx, std::move(roots));
}

std::string s_of(const kimix::string &s) { return std::string(s.data(), s.size()); }

std::vector<std::string> lines_of(const ge::grep_result &r) {
    std::vector<std::string> out;
    out.reserve(r.lines.size());
    for (const kimix::string &l : r.lines) {
        out.push_back(s_of(l));
    }
    return out;
}

std::vector<std::string> files_of(const ge::grep_result &r) {
    std::vector<std::string> out;
    out.reserve(r.files.size());
    for (const ge::grep_file_result &f : r.files) {
        out.push_back(s_of(f.path));
    }
    return out;
}

std::string joined(const std::vector<std::string> &v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i != 0) {
            out += '\n';
        }
        out += v[i];
    }
    return out;
}

bool contains(const std::vector<std::string> &v, const std::string &s) {
    for (const std::string &x : v) {
        if (x.find(s) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    // ---------------------------------------------------------- literal fast path
    // "hit" is a pure literal (memchr fast path), "h.t" and "[h]it" take the
    // regex path - all three must agree on counts, files and rendered lines.
    "literal_path_agrees_with_regex_path"_test = [] {
        fixture fx;
        const std::vector<std::string> patterns = {"hit", "h.t", "[h]it"};
        ge::grep_result first;
        bool have_first = false;
        for (const std::string &pat : patterns) {
            ge::grep_options o = make_opts(pat.c_str());
            ge::grep_result r = run_root(o, fx);
            expect(r.status == kimix::builtin_tools::tool_status::ok);
            expect(r.total_matches == 10) << pat;
            expect(r.files.size() == size_t(9)) << pat;
            expect(r.message == kimix::string("10 match(es) in 9 file(s)")) << pat;
            if (!have_first) {
                first = std::move(r);
                have_first = true;
            } else {
                expect(lines_of(r) == lines_of(first)) << pat;
                expect(files_of(r) == files_of(first)) << pat;
            }
        }
        // The corpus-wide file list (hidden/binary/oversized excluded).
        const std::vector<std::string> files = files_of(first);
        expect(contains(files, "a.txt"));
        expect(contains(files, "late.bin"));
        expect(!contains(files, ".hidden.txt"));
        expect(!contains(files, ".hid"));
        expect(!contains(files, "big.bin"));
        expect(!contains(files, "big.txt"));
    };

    // Regression for the live-run 0xC0000409 crash: an entry whose name the
  // ANSI code page cannot represent must not terminate the process; the walk
  // degrades lossily and the searchable corpus is unaffected.
#if defined(_WIN32) || defined(_WIN64)
  "survives_unconvertible_entry_names"_test = [] {
      fixture fx;
      const ge::grep_result r = run_root(make_opts("hit"), fx);
      expect(r.status == kimix::builtin_tools::tool_status::ok);
      expect(r.total_matches == 10);
      expect(contains(files_of(r), s_of(fx.p("a.txt"))));
  };
#endif

  // A literal containing '\n' must NOT take the fast path: per-line matching
    // can never span lines, so "miss\nend" matches nothing.
    "literal_with_newline_never_matches"_test = [] {
        fixture fx;
        ge::grep_options o = make_opts("miss\\nend");
        ge::grep_result r = run_root(o, fx);
        expect(r.status == kimix::builtin_tools::tool_status::ok);
        expect(r.total_matches == 0);
        expect(r.files.empty());
    };

    // ------------------------------------------------------------- output modes
    "output_mode_content_line_format"_test = [] {
        fixture fx;
        ge::grep_options o = make_opts("hit");
        o.mode = ge::grep_output_mode::content;
        o.include_glob = kimix::string("a.txt");
        const ge::grep_result r = run_root(o, fx);
        expect(lines_of(r) == std::vector<std::string>{s_of(fx.p("a.txt")) + ":1:hit"});
    };

    "output_mode_files_with_matches_lists_paths"_test = [] {
        fixture fx;
        ge::grep_options o = make_opts("hit");
        o.include_glob = kimix::string("a.txt");
        const ge::grep_result r = run_root(o, fx);
        expect(lines_of(r) == std::vector<std::string>{s_of(fx.p("a.txt"))});
        expect(files_of(r) == std::vector<std::string>{s_of(fx.p("a.txt"))});
    };

    "output_mode_count_matches_line_format"_test = [] {
        fixture fx;
        ge::grep_options o = make_opts("hit");
        o.mode = ge::grep_output_mode::count_matches;
        o.include_glob = kimix::string("a.txt");
        const ge::grep_result r = run_root(o, fx);
        expect(lines_of(r) == std::vector<std::string>{s_of(fx.p("a.txt")) + ":1"});
    };

    // count_matches counts matching LINES, not occurrences: "hit hit" is 1.
    "count_matches_counts_lines_not_occurrences"_test = [] {
        fixture fx;
        ge::grep_options o = make_opts("hit");
        o.mode = ge::grep_output_mode::count_matches;
        o.include_glob = kimix::string("double.txt");
        const ge::grep_result r = run_root(o, fx);
        expect(r.total_matches == 1);
        expect(r.files.size() == size_t(1));
        expect(r.files[0].match_count == 1);
        expect(lines_of(r) == std::vector<std::string>{s_of(fx.p("double.txt")) + ":1"});
    };

    // ----------------------------------------------------------------- matching
    "ignore_case_literal_and_regex_paths"_test = [] {
        fixture fx;
        for (const char *pat : {"HIT", "h.t"}) {
            ge::grep_options o = make_opts(pat);
            o.ignore_case = true;
            o.mode = ge::grep_output_mode::content;
            o.include_glob = kimix::string("b.txt");
            const ge::grep_result r = run_root(o, fx);
            expect(r.total_matches == 2) << pat;
            // Line 2 ("bar") is not a hit and has no context -> the runs are
            // disjoint, so a "--" separator sits between them.
            expect(lines_of(r) == std::vector<std::string>{s_of(fx.p("b.txt")) + ":1:foo HIT",
                                                          "--",
                                                          s_of(fx.p("b.txt")) + ":3:baz hit"})
                << pat;
        }
    };

    "case_sensitive_by_default"_test = [] {
        fixture fx;
        // Wrong case: no match anywhere in the corpus.
        {
            ge::grep_options o = make_opts("Hit");
            const ge::grep_result r = run_root(o, fx);
            expect(r.total_matches == 0);
            expect(r.files.empty());
            expect(r.message == kimix::string("0 match(es) in 0 file(s)"));
        }
        // Exact case: only the "HIT" line of b.txt matches.
        {
            ge::grep_options o = make_opts("HIT");
            o.include_glob = kimix::string("b.txt");
            const ge::grep_result r = run_root(o, fx);
            expect(r.total_matches == 1);
            expect(r.files.size() == size_t(1));
        }
    };

    "invalid_pattern_reports_invalid_input"_test = [] {
        fixture fx;
        ge::grep_options o = make_opts("[");
        const ge::grep_result r = run_root(o, fx);
        expect(r.status == kimix::builtin_tools::tool_status::invalid_input);
        expect(r.message.find("invalid pattern: ") == 0) << s_of(r.message);
    };

    // ------------------------------------------------------------------ context
    "content_context_runs_and_separators"_test = [] {
        fixture fx;
        const std::string pfx = s_of(fx.p("ctx.txt"));
        auto run_ctx = [&](uint32_t b, uint32_t a) {
            ge::grep_options o = make_opts("hit");
            o.mode = ge::grep_output_mode::content;
            o.include_glob = kimix::string("ctx.txt");
            o.ctx_before = b;
            o.ctx_after = a;
            return lines_of(run_root(o, fx));
        };
        // No context: the hits are non-adjacent, so the old branch's
        // last_emitted logic still separates them with "--".
        expect(run_ctx(0, 0) == std::vector<std::string>{pfx + ":1:hit", "--", pfx + ":4:hit"});
        // -A1: runs [0..1] and [3..4] are disjoint -> "--" between them.
        // Context lines render as "path-LN-text" ('-' on both sides, like rg).
        expect(run_ctx(0, 1) == std::vector<std::string>{pfx + ":1:hit", pfx + "-2-mid1", "--",
                                                        pfx + ":4:hit", pfx + "-5-tail"});
        // -B1: hit at line 4 pulls in line 3 with the '-' delimiter.
        expect(run_ctx(1, 0) == std::vector<std::string>{pfx + ":1:hit", "--", pfx + "-3-mid2",
                                                        pfx + ":4:hit"});
        // -C1: the runs touch ([0..1] and [2..4]) -> merged, no "--".
        expect(run_ctx(1, 1) == std::vector<std::string>{pfx + ":1:hit", pfx + "-2-mid1",
                                                        pfx + "-3-mid2", pfx + ":4:hit",
                                                        pfx + "-5-tail"});
        // -B5 reaches back to the file start (clamped): lines 0 and 1 only.
        expect(run_ctx(5, 0) == std::vector<std::string>{pfx + ":1:hit", pfx + "-2-mid1",
                                                        pfx + "-3-mid2", pfx + ":4:hit"});
    };

    "content_context_clamps_at_file_start"_test = [] {
        fixture fx;
        ge::grep_options o = make_opts("hit");
        o.mode = ge::grep_output_mode::content;
        o.include_glob = kimix::string("c.py");
        o.ctx_before = 5;
        const ge::grep_result r = run_root(o, fx);
        // Portable join: a hardcoded "sub\\c.py" is one literal filename on
        // Linux (backslash is not a separator there), and a forward-slash
        // "sub/c.py" stays mixed-separator on Windows - join real path
        // components so kimix::to_string yields native separators on both.
        const std::string sub_c = s_of(kimix::to_string(fx.root / "sub" / "c.py"));
        expect(lines_of(r) == std::vector<std::string>{sub_c + "-1-alpha",
                                                      sub_c + ":2:hit"});
    };

    "content_context_clamps_at_file_end"_test = [] {
        fixture fx;
        ge::grep_options o = make_opts("hit");
        o.mode = ge::grep_output_mode::content;
        o.include_glob = kimix::string("c.py");
        o.ctx_before = 1;
        o.ctx_after = 5;
        const ge::grep_result r = run_root(o, fx);
        const std::string sub_c = s_of(kimix::to_string(fx.root / "sub" / "c.py"));
        expect(lines_of(r) == std::vector<std::string>{sub_c + "-1-alpha",
                                                      sub_c + ":2:hit",
                                                      sub_c + "-3-gamma"});
    };

    // ------------------------------------------------------------------ include
    "include_glob_selects_by_file_name"_test = [] {
        fixture fx;
        auto run_inc = [&](const char *glob) {
            ge::grep_options o = make_opts("hit");
            o.include_glob = kimix::string(glob);
            return run_root(o, fx);
        };
        const ge::grep_result py = run_inc("*.py");
        expect(py.files.size() == size_t(2));
        expect(!contains(files_of(py), "c.txt"));
        expect(contains(files_of(py), "c.py"));
        expect(contains(files_of(py), "d.py"));
        const ge::grep_result single = run_inc("?.py"); // '?' wildcard
        expect(single.files.size() == size_t(2));
        const ge::grep_result txt = run_inc("*.txt");
        expect(txt.files.size() == size_t(6)); // a, b, double, cr, ctx, sub/c.txt
    };

    // ---------------------------------------------------- hidden / binary / size
    "hidden_files_and_dirs_skipped_at_every_depth"_test = [] {
        fixture fx;
        const ge::grep_result r = run_root(make_opts("hit"), fx);
        const std::vector<std::string> files = files_of(r);
        expect(!contains(files, ".hidden.txt"));
        expect(!contains(files, ".hid"));
    };

    "binary_sniff_first_64kib_only"_test = [] {
        fixture fx;
        const ge::grep_result r = run_root(make_opts("hit"), fx);
        const std::vector<std::string> files = files_of(r);
        expect(!contains(files, "big.bin"));  // NUL within the first 64 KiB: skipped
        expect(contains(files, "late.bin"));  // NUL only after 64 KiB: still searched
        // And it actually matched its "hit" line.
        expect(r.total_matches == 10);
    };

    "file_above_4_mib_skipped"_test = [] {
        fixture fx;
        const ge::grep_result r = run_root(make_opts("hit"), fx);
        expect(!contains(files_of(r), "big.txt"));
    };

    // --------------------------------------------------------------- head_limit
    "head_limit_caps_fwm_lines_but_not_files"_test = [] {
        fixture fx;
        ge::grep_options o = make_opts("hit");
        o.head_limit = 1;
        const ge::grep_result r = run_root(o, fx);
        expect(r.lines.size() == size_t(1));   // rendered lines capped ...
        expect(r.files.size() == size_t(9));   // ... the files[] array stays complete
        expect(r.total_matches == 10);
    };

    "head_limit_does_not_truncate_content_lines"_test = [] {
        fixture fx;
        ge::grep_options o = make_opts("hit");
        o.mode = ge::grep_output_mode::content;
        o.include_glob = kimix::string("*.txt");
        o.head_limit = 1; // the caller (Grep::operator()) truncates at join time
        const ge::grep_result r = run_root(o, fx);
        // a:1, b:1, double:1, cr:1, ctx:3 (two hits + "--"), c.txt:1
        expect(r.lines.size() == size_t(8)) << r.lines.size();
    };

    // ------------------------------------------------------------------ plumbing
    "relative_root_resolves_against_work_dir"_test = [] {
        fixture fx;
        ge::grep_options o = make_opts("hit");
        const ge::grep_result r = run_rel(o, fx, "sub");
        expect(r.files.size() == size_t(3)); // c.py, c.txt, d.py (.hid skipped)
        const std::vector<std::string> files = files_of(r);
        for (const std::string &f : files) {
            expect(f.find(s_of(fx.p("sub"))) == 0) << f;
        }
        expect(!contains(files, ".hid"));
    };

    "crlf_trailing_carriage_return_stripped"_test = [] {
        fixture fx;
        ge::grep_options o = make_opts("hit");
        o.mode = ge::grep_output_mode::content;
        o.include_glob = kimix::string("cr.txt");
        const ge::grep_result r = run_root(o, fx);
        expect(lines_of(r) == std::vector<std::string>{s_of(fx.p("cr.txt")) + ":1:hit"});
    };

    "multi_file_results_are_deterministic"_test = [] {
        fixture fx;
        ge::grep_options o = make_opts("hit");
        o.mode = ge::grep_output_mode::content;
        const ge::grep_result r1 = run_root(o, fx);
        const ge::grep_result r2 = run_root(o, fx);
        expect(lines_of(r1) == lines_of(r2));
        expect(files_of(r1) == files_of(r2));
        // Walk-order merge: parent dir files come before sub/ files.
        bool seen_sub = false;
        bool ordered = true;
        for (const kimix::string &l : r1.lines) {
            if (l.find("sub/") != kimix::string::npos || l.find("sub\\") != kimix::string::npos) {
                seen_sub = true;
            } else if (seen_sub && l.find("ctx.txt") != kimix::string::npos) {
                ordered = false; // a parent-dir file after sub/ files
            }
        }
        expect(ordered);
    };

    "message_and_files_summary"_test = [] {
        fixture fx;
        ge::grep_options o = make_opts("hit");
        o.include_glob = kimix::string("a.txt");
        const ge::grep_result r = run_root(o, fx);
        expect(r.message == kimix::string("1 match(es) in 1 file(s)"));
        expect(r.files.size() == size_t(1));
        expect(r.files[0].match_count == 1);
        expect(r.files[0].path == fx.p("a.txt"));
    };
}
