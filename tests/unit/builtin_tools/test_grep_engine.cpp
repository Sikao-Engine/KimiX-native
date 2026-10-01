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
// selection include-glob over the file name (case-sensitive: rg-style,
// -i folds content only), hidden file/dir skip at
// every depth, binary sniff (NUL in first 64 KiB skipped;
// a NUL past 64 KiB is searched), > 4 MiB files skipped
// plumbing relative roots resolve against work_dir, CRLF '\r'
// stripping, deterministic multi-file ordering (run twice),
// ambient-pool parallelism (bound caller fans chunks over a
// kimix::fiber pool; every mode byte-identical to the
// serial inline path, repeated runs identical),
//                   seeded literal-vs-regex agreement fuzz: random needles
//                   over a random corpus, the extract_literal fast path and
//                   regex_lite must agree with an independent line oracle.
//
// Function-by-function verification (every helper of grep_engine.cpp is
// file-local to the anonymous namespace ge, so it is verified through its
// only public entry point, run_grep, with inputs chosen to hit that
// function's branches):
//   ge::lower_ascii, ge::is_alpha    exercised by every case-fold and
//                  alphabetic-escape test below (no independent observable).
//   ge::extract_literal  routing table: escaped punctuation/control chars are
//                  literals; class/hex escapes and UNKNOWN escapes (letters
//                  AND digits: \0 is the NUL char, \1-\9 are back-refs) go to
//                  the regex validator; dangling backslash -> invalid_input.
//   ge::literal_in_line  memchr vs folded scan first byte, false-start
//                  retry after a mismatch (resumes at exactly off+1: an
//                  occurrence starting right after a failed candidate is
//                  found), folded tail compare, needle longer
//                  than hay, empty needle matches every line.
//   ge::collect_lines  partial last line, trailing lone '\r', exactly one
//                  '\r' stripped ("cc\r\r" keeps one), empty file,
//                  blank views ("^$" / "." / empty pattern on a blank line).
//   ge::render_content  the line_match parallel flags (1 match / 0 context /
//                  0 separator), the no-separator adjacent-run rule, and the
//                  skip of already-emitted lines when a hit falls inside the
//                  previous run's context window.
//   ge::scan_file  exactly-4-MiB file is searched (cap is strict >), the
//                  walk's directory entries are skipped, count/fwm lines
//                  carry line_match 1.
//   ge::collect_files  missing root, direct-file root (hidden name included -
//                  the hidden filter walks only, roots are taken as given),
//                  multi-root order.
//   run_grep  out-object reset across calls, head_limit <= 0 unlimited, the
//                  single-file (num_threads == 1, inline worker) path.

#include "ut/ut.hpp"
#include "ut/ut.hpp"
#include "builtin_tools/grep_engine.h"

#include <core/fiber.h> // ambient-pool determinism test binds a scheduler


#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
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

// Second, independent corpus for the per-function suites. Deliberately kept
// out of the main fixture so the corpus-wide "10 match(es) in 9 file(s)"
// pins above stay valid.
struct micro_fixture {
    kimix::filesystem::path root;

    static void put(const kimix::filesystem::path &p, const char *text) {
        std::FILE *f = std::fopen(kimix::to_string(p).c_str(), "wb");
        expect(f != nullptr);
        if (f != nullptr) {
            std::fwrite(text, 1, std::strlen(text), f);
            std::fclose(f);
        }
    }

    micro_fixture() {
        namespace fs = kimix::filesystem;
        std::error_code ec;
        root = fs::temp_directory_path(ec) / "kimix_grep_engine_micro";
        fs::remove_all(root, ec);
        fs::create_directories(root / "one" / "kid", ec); // dir entries in the walk
        put(root / "d.txt", "alpha\n");                       // 1 line
        put(root / "part.txt", "alpha\nhit");                 // no trailing '\n'
        put(root / "cr.txt", "hit\r");                        // lone trailing '\r'
        put(root / "empty.txt", "");                          // 0 bytes
        put(root / "blank.txt", "\n");                        // one blank view
        put(root / "mid.txt", "a\n\nb\n");                    // blank middle line
        put(root / "dot.txt", "3.14\nplain\n");
        put(root / "dig.txt", "9 lives\n"); // no 'd'/'w' letters: escape routing probe
        put(root / "tab.txt", "a\tb\nx\fy\np\vr\nm\rn\n");    // control chars mid-line
        put(root / "fs.txt", "Hx hit here\nh!t hit\n");       // first-byte false starts
        put(root / "case.txt", "HITline\nhitline\naHxTb\n");
        put(root / "ctx3.txt", "h\nx\ny\nh\n");               // disjoint runs
        put(root / "adj.txt", "hit\nhit\n");                  // adjacent hits
        put(root / "bs.txt", "a\\b\n");                       // line with one backslash
        put(root / "retry.txt", "qqzq\ncc\r\r\n");  // off+1 retry + double trailing \r
        put(root / "hhh.txt", "h\nh\nh\n");         // hits inside each other's -C1 window
        put(root / "zcase.txt", "Zulu\nzulu\nZEST\n"); // uppercase-first fold scan
        put(root / "lonelycr.txt", "\r\nx\r\n");        // a line that is only '\r'
        put(root / ".hid.txt", "hidden\n");                   // hidden (walk-skipped)
        put(root / "one" / "only.txt", "hit\n");              // single-file dir
        // Exactly 4 MiB: the stat cap is strict '>', so this file IS searched.
        {
            std::FILE *f = std::fopen(kimix::to_string(root / "big4.txt").c_str(), "wb");
            expect(f != nullptr);
            if (f != nullptr) {
                char buf[8192];
                std::memset(buf, 'x', sizeof(buf));
                size_t count = 4u * 1024u * 1024u - 4u;
                while (count > 0) {
                    const size_t n = count < sizeof(buf) ? count : sizeof(buf);
                    std::fwrite(buf, 1, n, f);
                    count -= n;
                }
                std::fwrite("hit\n", 1, 4, f);
                std::fclose(f);
            }
        }
        // NUL only AFTER the 64 KiB sniff window: searched, and its final
        // line is "xxx..x\0hit" - it carries a NUL byte for the regex \0 probe.
        {
            std::FILE *f = std::fopen(kimix::to_string(root / "late.bin").c_str(), "wb");
            expect(f != nullptr);
            if (f != nullptr) {
                char buf[8192];
                std::memset(buf, 'x', sizeof(buf));
                size_t count = 64u * 1024u;
                while (count > 0) {
                    const size_t n = count < sizeof(buf) ? count : sizeof(buf);
                    std::fwrite(buf, 1, n, f);
                    count -= n;
                }
                std::fwrite("\0hit\n", 1, 5, f);
                std::fclose(f);
            }
        }
    }

    ~micro_fixture() {
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

// Run with absolute root(s); work_dir is empty so roots are taken verbatim.
ge::grep_result run_at(const ge::grep_options &o, const kimix::string &root) {
    const kimix::vector<kimix::string> roots{kimix::string(root)};
    const kimix::string work_dir;
    ge::grep_result out;
    ge::run_grep(o, roots, kimix::string_view(work_dir), out);
    return out;
}

ge::grep_result run_at_roots(const ge::grep_options &o,
                             const kimix::vector<kimix::string> &roots) {
    const kimix::string work_dir;
    ge::grep_result out;
    ge::run_grep(o, roots, kimix::string_view(work_dir), out);
    return out;
}

// One search rooted at the micro corpus, optionally scoped by include-glob.
ge::grep_result run_micro(const char *pattern, const micro_fixture &fx,
                          const char *glob = nullptr, bool ignore_case = false,
                          ge::grep_output_mode mode = ge::grep_output_mode::content) {
    ge::grep_options o = make_opts(pattern);
    o.mode = mode;
    o.ignore_case = ignore_case;
    if (glob != nullptr) {
        o.include_glob = kimix::string(glob);
    }
    return run_at(o, kimix::to_string(fx.root));
}

std::vector<int> flags_of(const ge::grep_result &r) {
    std::vector<int> out;
    out.reserve(r.line_match.size());
    for (const uint8_t f : r.line_match) {
        out.push_back(f);
    }
    return out;
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

// ---------------------------------------------------------------------------
// Deterministic literal-vs-regex agreement fuzz (f08e1611 bug class):
// random needles are searched in two generated forms that MUST agree with an
// independent substring oracle - and hence with each other:
//   literal form  plain letters, metacharacters backslash-escaped: passes
//                 ge::extract_literal, so the memchr/memcmp fast path answers;
//   regex form    every character in its own single-element class ([x]): the
//                 bare '[' fails extract_literal, so regex_lite answers with
//                 the same literal semantics.
// ---------------------------------------------------------------------------
struct fuzz_rng {
    uint64_t s;
    explicit fuzz_rng(uint64_t seed) : s(seed) {}
    uint64_t next() { // xorshift64*
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }
    char pick(const char *alphabet) {
        return alphabet[below(static_cast<uint32_t>(std::strlen(alphabet)))];
    }
};

inline const char *fuzz_alphabet() { return "abcXY^$*+?()[]{}|-. 0e"; }

// Regex metacharacters of extract_literal/regex_lite that need escaping.
inline bool fuzz_meta(char c) {
    return std::strchr("^$*+?()[]{}|.", c) != nullptr;
}

// Force the literal fast path: letters/digits/space/'-' verbatim, metachars
// escaped (escaped punctuation is the literal char in extract_literal).
inline std::string fuzz_literal_form(const std::string &needle) {
    std::string out;
    for (const char c : needle) {
        if (fuzz_meta(c)) {
            out += '\\';
        }
        out += c;
    }
    return out;
}

// Force the regex path with identical literal semantics: a single-character
// class per needle char; metachars are backslash-escaped INSIDE the class
// ("[^]" would negate, "[.]" is fine but "[\.]" keeps one rule for all).
inline std::string fuzz_regex_form(const std::string &needle) {
    std::string out;
    for (const char c : needle) {
        out += "[";
        if (fuzz_meta(c)) {
            out += '\\';
        }
        out += c;
        out += ']';
    }
    return out;
}

// Mirror of ge::collect_lines: split on '\n', strip EXACTLY one trailing '\r',
// partial last line kept, empty file yields no lines.
inline std::vector<std::string> fuzz_lines_of_text(const std::string &text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size()) {
        const size_t nl = text.find('\n', start);
        std::string v = (nl == std::string::npos) ? text.substr(start)
                                                  : text.substr(start, nl - start);
        if (!v.empty() && v.back() == '\r') {
            v.pop_back();
        }
        out.push_back(v);
        if (nl == std::string::npos) {
            break;
        }
        start = nl + 1;
    }
    return out;
}

inline char fuzz_fold(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// Oracle: does `needle` occur in `line` (ASCII-folded when ic)? The engine
// counts matching LINES, occurrence multiplicity is irrelevant here.
inline bool fuzz_line_match(const std::string &line, const std::string &needle, bool ic) {
    if (ic) {
        std::string l, n;
        for (const char c : line) l += fuzz_fold(c);
        for (const char c : needle) n += fuzz_fold(c);
        return l.find(n) != std::string::npos;
    }
    return line.find(needle) != std::string::npos;
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

    // Epoch-1 pin: the include-glob is CASE-SENSITIVE. scan_file calls
    // fnmatch_ascii(name, glob, false) unconditionally - like rg's default
    // --glob - unlike Python fnmatch's ntpath.normcase fold (grep_local.py)
    // or the sensitive-file path's on_windows fold. -i folds CONTENT
    // matching only; it never folds the file-name filter.
    "include_glob_is_case_sensitive_even_under_ignore_case"_test = [] {
        fixture fx;
        auto run_inc = [&](const char *glob, bool ic) {
            ge::grep_options o = make_opts("hit");
            o.include_glob = kimix::string(glob);
            o.ignore_case = ic;
            return run_root(o, fx);
        };
        expect(run_inc("*.PY", false).files.empty());
        expect(run_inc("*.py", false).files.size() == size_t(2));
        expect(run_inc("*.Py", false).files.empty());
        // With -i the glob still selects by exact case...
        expect(run_inc("*.TXT", true).files.empty());
        // ...while the content fold widens the count: 6 .txt files, 8
        // matching lines (b.txt gains its "foo HIT" line).
        const ge::grep_result folded = run_inc("*.txt", true);
        expect(folded.files.size() == size_t(6));
        expect(folded.total_matches == 8) << folded.total_matches;
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

      // The engine fans out over the AMBIENT fiber pool (it never creates a
      // scheduler itself): with >= 8 files and a bound caller, chunk merges
      // must produce byte-identical results to the serial inline path, for
      // every mode, the fwm head_limit cap, context runs, and the per-file
      // match lines. Also pins: repeated runs under the pool are identical
      // (chunk claim order must never leak into the output).
      "ambient_pool_parallel_matches_serial"_test = [] {
          namespace fs = kimix::filesystem;
          std::error_code ec;
          const fs::path root = fs::temp_directory_path(ec) / "kimix_grep_engine_par";
          fs::remove_all(root, ec);
          fs::create_directories(root / "sub1" / "deep", ec);
          fs::create_directories(root / "sub2", ec);
          // 26 files over 3 dirs; every 3rd contains two "hit" lines with an
          // unmatched line between them (context runs inside each file).
          for (int i = 0; i < 26; ++i) {
            char rel[64];
            std::snprintf(rel, sizeof(rel), "%s/f%02d.txt",
                          i % 3 == 0 ? "sub1/deep" : (i % 2 == 0 ? "sub2" : "."), i);
            kimix::string text;
            if (i % 3 != 2) {
                text += "alpha hit one\nbeta\ngamma hit two\n";
            } else {
                text += "alpha\nbeta\n";
            }
            std::FILE *f = std::fopen(kimix::to_string(root / rel).c_str(), "wb");
            expect(f != nullptr);
            if (f != nullptr) {
                std::fwrite(text.data(), 1, text.size(), f);
                std::fclose(f);
            }
          }
          const kimix::string root_s = kimix::to_string(root);

          auto run_serial = [](const kimix::string &root_s, const ge::grep_options &o) {
              kimix::vector<kimix::string> roots;
              roots.push_back(root_s);
              const kimix::string work_dir;
              ge::grep_result out;
              ge::run_grep(o, roots, kimix::string_view(work_dir), out);
              return out;
          };
          // Serial oracle (calling thread unbound -> single inline chunk).
          struct variant {
              const char *pat;
              ge::grep_output_mode mode;
              int32_t hl;
              uint32_t b, a;
              bool ic;
          };
          const variant variants[] = {
              {"hit", ge::grep_output_mode::files_with_matches, 3, 0, 0, false},
              {"hit", ge::grep_output_mode::files_with_matches, 0, 0, 0, false},
              {"hit", ge::grep_output_mode::count_matches, 0, 0, 0, false},
              {"hit", ge::grep_output_mode::content, 0, 1, 1, false},
              {"hit", ge::grep_output_mode::content, 0, 0, 0, false},
              {"HIT", ge::grep_output_mode::content, 0, 0, 2, true},
              {"h.t", ge::grep_output_mode::files_with_matches, 5, 0, 0, false},
              {"h[a-z]t", ge::grep_output_mode::count_matches, -1, 0, 0, true},
          };
          for (const variant &v : variants) {
              ge::grep_options o = make_opts(v.pat);
              o.mode = v.mode;
              o.head_limit = v.hl;
              o.ctx_before = v.b;
              o.ctx_after = v.a;
              o.ignore_case = v.ic;
              const ge::grep_result serial = run_serial(root_s, o);
              expect(serial.total_matches > 0) << v.pat;
              ge::grep_result par;
              {
                  kimix::fiber::scoped_scheduler pool{4u};
                  par = run_serial(root_s, o);
                  expect(par.total_matches > 0) << v.pat;
              }
              expect(par.status == serial.status) << v.pat;
              expect(par.total_matches == serial.total_matches) << v.pat;
              expect(par.message == serial.message) << v.pat;
              expect(files_of(par) == files_of(serial)) << v.pat;
              expect(par.files.size() == serial.files.size()) << v.pat;
              for (size_t i = 0; i < par.files.size(); ++i) {
                  expect(par.files[i].path == serial.files[i].path) << v.pat;
                  expect(par.files[i].match_count == serial.files[i].match_count) << v.pat;
              }
              expect(lines_of(par) == lines_of(serial)) << v.pat;
              expect(par.line_match == serial.line_match) << v.pat;
              // Running again under the pool (claims land on different
              // workers) must stay identical.
              ge::grep_result par2;
              {
                  kimix::fiber::scoped_scheduler pool{4u};
                  par2 = run_serial(root_s, o);
              }
              expect(lines_of(par2) == lines_of(serial)) << v.pat;
              expect(par2.total_matches == serial.total_matches) << v.pat;
          }
          fs::remove_all(root, ec);
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

    // =============================================================== per-function
    // Function-by-function verification of grep_engine.cpp's anonymous-namespace
    // helpers (they are file-local, so each test drives the one public entry
    // point run_grep with inputs chosen to hit that function's branches).

    // ge::extract_literal: escaped punctuation stays on the literal fast path
    // and must agree with the regex path: pattern "3\.14" (literal "3.14") vs
    // the equivalent regex "[3]\.[1][4]". "\." alone matches a dot, unlike the
    // regex metacharacter "." which matches any character. An escaped
    // backslash (pattern "a\\b" = a,/,/,b) is the 3-char literal a/b.
    "extract_literal_escaped_punctuation_is_literal"_test = [] {
        micro_fixture fx;
        const std::string dot = s_of(fx.p("dot.txt"));
        const ge::grep_result esc = run_micro("3\\.14", fx, "dot.txt");
        expect(lines_of(esc) == std::vector<std::string>{dot + ":1:3.14"}) << joined(lines_of(esc));
        const ge::grep_result cls = run_micro("[3]\\.[1][4]", fx, "dot.txt");
        expect(lines_of(cls) == lines_of(esc));
        const ge::grep_result dot_lit = run_micro("\\.", fx, "dot.txt");
        expect(dot_lit.total_matches == 1); // only "3.14" has a dot
        const ge::grep_result dot_re = run_micro(".", fx, "dot.txt");
        expect(dot_re.total_matches == 2); // "." matches every non-blank line
        const std::string bs = s_of(fx.p("bs.txt"));
        const ge::grep_result r = run_micro("a\\\\b", fx, "bs.txt");
        expect(lines_of(r) == std::vector<std::string>{bs + ":1:a\\b"}) << joined(lines_of(r));
        const ge::grep_result r2 = run_micro("[a]\\\\[b]", fx, "bs.txt");
        expect(lines_of(r2) == lines_of(r));
    };

    // ge::extract_literal: /n /t /r /f /v are literal control characters.
    // Each probe matches exactly the tab.txt line carrying that byte (the
    // control byte survives into the rendered content text). A TRAILING '\r'
    // is stripped by ge::collect_lines, so cr.txt ("hit\r") has no '\r' left.
    "extract_literal_control_escapes_are_literal"_test = [] {
        micro_fixture fx;
        const std::string tab = s_of(fx.p("tab.txt"));
        const ge::grep_result rt = run_micro("\\t", fx, "tab.txt");
        expect(lines_of(rt) == std::vector<std::string>{tab + ":1:a\tb"}) << joined(lines_of(rt));
        const ge::grep_result rf = run_micro("\\f", fx, "tab.txt");
        expect(lines_of(rf) == std::vector<std::string>{tab + ":2:x\fy"});
        const ge::grep_result rv = run_micro("\\v", fx, "tab.txt");
        expect(lines_of(rv) == std::vector<std::string>{tab + ":3:p\vr"});
        const ge::grep_result rr = run_micro("\\r", fx, "tab.txt");
        expect(lines_of(rr) == std::vector<std::string>{tab + ":4:m\rn"});
        expect(run_micro("\\r", fx, "cr.txt").total_matches == 0);
    };

    // ge::extract_literal + ge::is_alpha: class escapes (/d /w) and unknown
    // ALPHABETIC escapes (/p /b /A) are not literal - they go to the regex
    // engine, which decides. dig.txt ("9 lives") deliberately contains no 'd'
    // and no 'w' letters: a literal mis-route would match nothing, the regex
    // classes match the '9'. /p / /b / /A are rejected by regex_lite ->
    // invalid_input (not a silent literal "p"/"b"/"A" search).
    "extract_literal_class_and_alpha_escapes_route_to_regex"_test = [] {
        micro_fixture fx;
        const ge::grep_result rd =
            run_micro("\\d", fx, "dig.txt", false, ge::grep_output_mode::count_matches);
        expect(rd.status == kimix::builtin_tools::tool_status::ok) << s_of(rd.message);
        expect(rd.total_matches == 1);
        expect(rd.line_match.size() == size_t(1));
        const ge::grep_result rw = run_micro("\\w", fx, "dig.txt");
        expect(rw.total_matches == 1);
        for (const char *pat : {"\\p", "\\b", "\\A", "\\Z", "\\z"}) {
            const ge::grep_result r = run_micro(pat, fx, "dig.txt");
            expect(r.status == kimix::builtin_tools::tool_status::invalid_input) << pat;
            expect(r.message.find("invalid pattern: unsupported escape: \\") == 0)
                << pat << s_of(r.message);
        }
    };

    // ge::extract_literal: DIGIT escapes are NOT "escaped punctuation": /0 is
    // the NUL character for the regex engine and /1-/9 are back-refs, so both
    // must go through the validator like every other unknown escape - the
    // literal fast path may not silently re-interpret them as digit literals,
    // that would diverge from the inline-regex semantics the engine mirrors.
    // late.bin has one NUL (past the 64 KiB sniff) and no '0' character;
    // big4.txt is all 'x' plus "hit" (no '0', no NUL).
    "extract_literal_digit_escapes_route_to_regex"_test = [] {
        micro_fixture fx;
        const ge::grep_result r9 = run_micro("\\9", fx, "dig.txt");
        expect(r9.status == kimix::builtin_tools::tool_status::invalid_input);
        expect(r9.message.find("back-references") != kimix::string::npos)
            << s_of(r9.message);
        const ge::grep_result rnul =
            run_micro("\\0", fx, "late.bin", false, ge::grep_output_mode::files_with_matches);
        expect(rnul.status == kimix::builtin_tools::tool_status::ok) << s_of(rnul.message);
        expect(rnul.files.size() == size_t(1)) << joined(files_of(rnul));
        expect(contains(files_of(rnul), "late.bin"));
        expect(run_micro("\\0", fx, "big4.txt").total_matches == 0);
    };

    // ge::extract_literal: a dangling backslash returns false ("let the regex
    // validator reject it") -> parameter error, not a search result.
    "extract_literal_trailing_backslash_is_invalid"_test = [] {
        micro_fixture fx;
        const ge::grep_result r = run_micro("hit\\", fx, "part.txt");
        expect(r.status == kimix::builtin_tools::tool_status::invalid_input);
        expect(r.message == kimix::string("invalid pattern: trailing backslash"))
            << s_of(r.message);
    };

    // ge::literal_in_line: memchr false-start retry (first byte hits, tail
    // mismatches, scan continues from off+1): fs.txt "Hx hit here" / "h!t hit"
    // each start a candidate at a position that fails the tail compare.
    // ignore_case folds BOTH directions (needle "hxt" vs haystack "HxT") and
    // the folded-first-byte scan only kicks in when the needle starts with a
    // letter - "3.1" (first byte '3') keeps the plain memchr path with a
    // folded tail compare. Empty needle matches every line; a needle longer
    // than the line matches none.
    "literal_in_line_memchr_and_folded_paths"_test = [] {
        micro_fixture fx;
        const std::string fs_p = s_of(fx.p("fs.txt"));
        const ge::grep_result cs =
            run_micro("hit", fx, "fs.txt", false, ge::grep_output_mode::count_matches);
        expect(cs.total_matches == 2); // both lines, two false starts survived
        expect(lines_of(cs) == std::vector<std::string>{fs_p + ":2"});
        const std::string case_p = s_of(fx.p("case.txt"));
        expect(lines_of(run_micro("hxt", fx, "case.txt", true)) ==
               std::vector<std::string>{case_p + ":3:aHxTb"});
        expect(lines_of(run_micro("HxTb", fx, "case.txt")) ==
               std::vector<std::string>{case_p + ":3:aHxTb"});
        expect(run_micro("HIT", fx, "case.txt").total_matches == 1); // exact case
        expect(run_micro("3\\.1", fx, "dot.txt", true).total_matches == 1);
        expect(run_micro("alphabet", fx, "d.txt").total_matches == 0);
        const ge::grep_result empty = run_micro("", fx, "mid.txt");
        expect(empty.total_matches == 3); // blank views match the empty needle
        expect(flags_of(empty) == std::vector<int>({1, 1, 1}));
    };

    // ge::literal_in_line: the false-start retry resumes EXACTLY at off+1
    // (memchr from the next byte), so an occurrence starting right after a
    // failed candidate is still found. "qzq" in "qqzq": the candidate at 0
    // fails the tail compare ('q' != 'z'), the real occurrence starts at 1 -
    // an off+2 skip-ahead would miss it. The regex path must agree.
    "literal_in_line_overlap_retry"_test = [] {
        micro_fixture fx;
        const std::string rp = s_of(fx.p("retry.txt"));
        const ge::grep_result lit = run_micro("qzq", fx, "retry.txt");
        expect(lines_of(lit) == std::vector<std::string>{rp + ":1:qqzq"})
            << joined(lines_of(lit));
        const ge::grep_result re = run_micro("[q]z[q]", fx, "retry.txt");
        expect(lines_of(re) == lines_of(lit));
    };

    // ge::literal_in_line: an ignore_case needle whose first byte is uppercase
    // must NOT take the raw-memchr fast path (the raw byte would never hit a
    // lowercase occurrence): every A-Z first byte uses the folded scan, so
    // "ZULU" and "zulu" match the same two lines of zcase.txt.
    "folded_scan_covers_uppercase_first_byte"_test = [] {
        micro_fixture fx;
        const std::string pfx = s_of(fx.p("zcase.txt"));
        const ge::grep_result low = run_micro("zulu", fx, "zcase.txt", true);
        expect(lines_of(low) == std::vector<std::string>{pfx + ":1:Zulu", pfx + ":2:zulu"})
            << joined(lines_of(low));
        const ge::grep_result up = run_micro("ZULU", fx, "zcase.txt", true);
        expect(lines_of(up) == lines_of(low)) << joined(lines_of(up));
    };

    // ge::collect_lines: a line whose entire content is one '\r' strips to
    // the EMPTY view (not a one-byte "\r" view): the pattern "\r" therefore
    // matches nothing in "\r\nx\r\n", while "x" still matches line 2.
    "collect_lines_cr_only_line_becomes_empty_view"_test = [] {
        micro_fixture fx;
        expect(run_micro("\\r", fx, "lonelycr.txt").total_matches == 0);
        expect(lines_of(run_micro("x", fx, "lonelycr.txt")) ==
               std::vector<std::string>{s_of(fx.p("lonelycr.txt")) + ":2:x"});
    };

    // ge::collect_lines: partial last line (no trailing '\n'), lone trailing '\r' stripped,
    // lone trailing '\r' stripped, and the blank/empty-file corners:
    // "\n" is one blank line, the 0-byte file has no lines at all, "^$"
    // matches a blank view (rendered "path:2:"), "." does not.
    "collect_lines_views_and_edges"_test = [] {
        micro_fixture fx;
        expect(lines_of(run_micro("hit", fx, "part.txt")) ==
               std::vector<std::string>{s_of(fx.p("part.txt")) + ":2:hit"});
        expect(lines_of(run_micro("hit", fx, "cr.txt")) ==
               std::vector<std::string>{s_of(fx.p("cr.txt")) + ":1:hit"});
        const std::string mid = s_of(fx.p("mid.txt"));
        const ge::grep_result anchored = run_micro("^$", fx, "mid.txt");
        expect(lines_of(anchored) == std::vector<std::string>{mid + ":2:"});
        expect(flags_of(anchored) == std::vector<int>({1}));
        expect(run_micro(".", fx, "mid.txt").total_matches == 2);
        expect(run_micro("", fx, "blank.txt").total_matches == 1);
        expect(run_micro(".", fx, "blank.txt").total_matches == 0);
        const ge::grep_result nofile = run_micro("", fx, "empty.txt");
        expect(nofile.status == kimix::builtin_tools::tool_status::ok);
        expect(nofile.files.empty());
        expect(nofile.total_matches == 0);
    };

    // ge::collect_lines strips exactly ONE trailing '\r': "cc\r\r\n" keeps
    // the second carriage return as part of the line (view "cc\r"), so the
    // literal "cc\r" matches it and the rendered text carries the '\r',
    // while "cc\r\r" (two) never matches.
    "collect_lines_strips_exactly_one_trailing_cr"_test = [] {
        micro_fixture fx;
        const std::string rp = s_of(fx.p("retry.txt"));
        const ge::grep_result one = run_micro("cc\\r", fx, "retry.txt");
        expect(lines_of(one) == std::vector<std::string>{rp + ":2:cc\r"})
            << joined(lines_of(one));
        expect(run_micro("cc\\r\\r", fx, "retry.txt").total_matches == 0);
    };

    // ge::render_content + the line_match parallel vector (grep_result.h:
    // 1 = match, 0 = context line or "--" separator; sized to lines):
    // disjoint runs get "--" (flags 1,0,0,1), touching runs merge without it,
    // and adjacent hits (lo == last_emitted+1) emit back-to-back with no
    // separator.
    "render_content_line_match_flags"_test = [] {
        micro_fixture fx;
        const std::string pfx = s_of(fx.p("ctx3.txt")); // "h\nx\ny\nh\n"
        ge::grep_options o = make_opts("h");
        o.mode = ge::grep_output_mode::content;
        o.include_glob = kimix::string("ctx3.txt");
        o.ctx_before = 1;
        const ge::grep_result b1 = run_at(o, kimix::to_string(fx.root));
        expect(lines_of(b1) == std::vector<std::string>{pfx + ":1:h", "--", pfx + "-3-y",
                                                        pfx + ":4:h"}) << joined(lines_of(b1));
        expect(b1.line_match.size() == b1.lines.size());
        expect(flags_of(b1) == std::vector<int>({1, 0, 0, 1}));
        o.ctx_after = 1; // runs [0..1] and [2..3] touch -> merged
        const ge::grep_result c1 = run_at(o, kimix::to_string(fx.root));
        expect(lines_of(c1) == std::vector<std::string>{pfx + ":1:h", pfx + "-2-x",
                                                        pfx + "-3-y", pfx + ":4:h"});
        expect(flags_of(c1) == std::vector<int>({1, 0, 0, 1}));
        const ge::grep_result adj = run_micro("hit", fx, "adj.txt");
        const std::string apfx = s_of(fx.p("adj.txt"));
        expect(lines_of(adj) == std::vector<std::string>{apfx + ":1:hit", apfx + ":2:hit"});
        expect(flags_of(adj) == std::vector<int>({1, 1}));
    };

    // ge::render_content: a hit INSIDE the previous run's context window
    // (adjacent hits with -C1) re-enters the emit loop at l <= last_emitted:
    // those lines are skipped (never rendered twice, no separator), and only
    // the new tail is emitted. Line 2 IS itself a match line, but it was
    // already rendered as context of hit 1, so its flag stays 0 - the count
    // (matching LINES) is unaffected.
    "render_content_overlapping_hit_windows"_test = [] {
        micro_fixture fx;
        const std::string pfx = s_of(fx.p("hhh.txt")); // "h\nh\nh\n"
        ge::grep_options o = make_opts("h");
        o.mode = ge::grep_output_mode::content;
        o.include_glob = kimix::string("hhh.txt");
        o.ctx_before = 1;
        o.ctx_after = 1;
        const ge::grep_result r = run_at(o, kimix::to_string(fx.root));
        expect(r.total_matches == 3); // three matching LINES regardless of render
        expect(lines_of(r) == std::vector<std::string>{pfx + ":1:h", pfx + "-2-h",
                                                      pfx + "-3-h"})
            << joined(lines_of(r));
        expect(flags_of(r) == std::vector<int>({1, 0, 0}));
    };

    // ge::scan_file: every count_matches / files_with_matches line carries
    // line_match flag 1; the walk's directory entries ("one", "one/kid") are
    // silently skipped by the is_regular_file check; and a file of EXACTLY
    // 4 MiB is searched - the k_max_file_bytes cap is strict '>'.
    "scan_file_mode_flags_and_caps"_test = [] {
        micro_fixture fx;
        const ge::grep_result cnt =
            run_micro("hit", fx, "adj.txt", false, ge::grep_output_mode::count_matches);
        expect(lines_of(cnt) == std::vector<std::string>{s_of(fx.p("adj.txt")) + ":2"});
        expect(flags_of(cnt) == std::vector<int>({1}));
        const ge::grep_result fwm =
            run_micro(".", fx, "d.txt", false, ge::grep_output_mode::files_with_matches);
        expect(flags_of(fwm) == std::vector<int>({1}));
        ge::grep_options o = make_opts(".");
        const ge::grep_result one = run_at(o, fx.p("one"));
        expect(one.files.size() == size_t(1)); // only.txt; the dirs are skipped
        expect(contains(files_of(one), "only.txt"));
        const ge::grep_result big =
            run_micro("hit", fx, "big4.txt", false, ge::grep_output_mode::count_matches);
        expect(big.files.size() == size_t(1)) << joined(files_of(big));
        if (big.files.size() == 1) {
            expect(big.files[0].match_count == 1);
        }
    };

    // ge::collect_files: a missing root yields nothing (status ok), a direct
    // file root is searched even when its NAME is hidden (the hidden filter
    // applies to walked entries; roots are taken as given), and several roots
    // concatenate in root order. The single-file root also pins the
    // n_files==1 clamp -> the num_threads==1 inline-worker branch of run_grep.
    "collect_files_roots_semantics"_test = [] {
        micro_fixture fx;
        const ge::grep_options dot = make_opts(".");
        const ge::grep_result missing = run_at(dot, fx.p("no_such_dir"));
        expect(missing.status == kimix::builtin_tools::tool_status::ok);
        expect(missing.files.empty());
        expect(missing.message == kimix::string("0 match(es) in 0 file(s)"));
        const ge::grep_result direct = run_at(dot, fx.p("d.txt"));
        expect(files_of(direct) == std::vector<std::string>{s_of(fx.p("d.txt"))});
        const ge::grep_result hid = run_at(dot, fx.p(".hid.txt"));
        expect(hid.files.size() == size_t(1)); // hidden NAME, direct root: searched
        kimix::vector<kimix::string> roots;
        roots.push_back(fx.p("one"));
        roots.push_back(fx.p("d.txt"));
        const ge::grep_result multi = run_at_roots(dot, roots);
        expect(multi.files.size() == size_t(2));
        if (multi.files.size() == 2) {
            expect(multi.files[0].path == kimix::to_string(fx.root / "one" / "only.txt"));
            expect(multi.files[1].path == fx.p("d.txt"));
        }
        expect(multi.total_matches == 2);
    };

    // run_grep plumbing: the out parameter is reset at entry (a reused
    // grep_result must not carry over files/lines/line_match/total/message),
    // and head_limit <= 0 means unlimited for the fwm line cap (files[]
    // stays complete; positive limits cap lines only). Micro corpus "hit":
    // part, cr, fs(2), case, adj(2), one/only, big4, late.bin = 10 lines /
    // 8 files.
    "run_grep_out_reset_and_head_limit"_test = [] {
        micro_fixture fx;
        ge::grep_options o = make_opts("hit");
        o.include_glob = kimix::string("part.txt");
        kimix::vector<kimix::string> roots;
        roots.push_back(kimix::to_string(fx.root));
        const kimix::string work_dir;
        ge::grep_result out;
        ge::run_grep(o, roots, kimix::string_view(work_dir), out);
        expect(out.files.size() == size_t(1));
        o.pattern = kimix::string("zzz_no_such_pattern_zzz");
        ge::run_grep(o, roots, kimix::string_view(work_dir), out);
        expect(out.status == kimix::builtin_tools::tool_status::ok);
        expect(out.files.empty());
        expect(out.lines.empty());
        expect(out.line_match.empty());
        expect(out.total_matches == 0);
        expect(out.message == kimix::string("0 match(es) in 0 file(s)"));
        o.pattern = kimix::string("hit");
        o.mode = ge::grep_output_mode::files_with_matches;
        o.include_glob = kimix::string();
        o.head_limit = 1;
        ge::grep_result capped;
        ge::run_grep(o, roots, kimix::string_view(work_dir), capped);
        expect(capped.lines.size() == size_t(1));
        expect(capped.files.size() == size_t(8));
        o.head_limit = -5;
        ge::grep_result unlimited;
        ge::run_grep(o, roots, kimix::string_view(work_dir), unlimited);
        expect(unlimited.lines.size() == unlimited.files.size());
        expect(unlimited.total_matches == 10) << unlimited.total_matches;
        // head_limit == 0 is the OTHER unlimited spelling (<= 0): without the
        // '<= 0' half of the guard the size comparison caps output at zero
        // lines - a mutation of exactly that '0' survived once, so pin it.
        o.head_limit = 0;
        ge::grep_result zeroed;
        ge::run_grep(o, roots, kimix::string_view(work_dir), zeroed);
        expect(zeroed.lines.size() == size_t(8));
        expect(zeroed.files.size() == size_t(8));
    };

    // Deterministic fuzz of the epoch-0 bug CLASS (literal fast path vs the
    // regex path must never diverge): a seeded random corpus of 6 files,
    // 60 random needles (biased to occur), each searched through the
    // literal-form pattern (forces ge::extract_literal + literal_in_line),
    // and through the single-char-class form (forces the regex_lite path,
    // identical literal semantics). Both must equal an independent substring
    // oracle that mirrors collect_lines (exactly one trailing '\r' stripped,
    // partial last line kept), in count and content mode, case-sensitive and
    // -insensitive. Walk order is unspecified, so line sets are compared
    // sorted; total_matches and the all-1 content flags are pinned too.
    "fuzz_literal_regex_agreement"_test = [] {
        namespace fs = kimix::filesystem;
        std::error_code ec;
        const fs::path root = fs::temp_directory_path(ec) / "kimix_grep_engine_fuzz";
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);

        fuzz_rng rng(0x9E3779B97F4A7C15ull);
        const char *words[] = {"ab", "cX", "^.", "*?", "()", "|{}", "-a", "[b]", "0e",
                               "Y", "^$", "?", " ", "a", "b.", "c", "Xc", "[", "]", "|"};
        std::vector<std::pair<std::string, std::string>> files; // path, raw text
        for (int f = 0; f < 6; ++f) {
            std::string text;
            const int nlines = 2 + static_cast<int>(rng.below(5));
            for (int l = 0; l < nlines; ++l) {
                if (rng.below(8) == 0) {
                    text += "\n"; // blank line
                    continue;
                }
                std::string line;
                const int nw = 1 + static_cast<int>(rng.below(6));
                for (int w = 0; w < nw; ++w) {
                    line += words[rng.below(20)];
                }
                if (line.size() > 40) {
                    line.resize(40);
                }
                text += line;
                text += (rng.below(4) == 0) ? "\r\n" : "\n";
            }
            if (f == 3) {
                text.pop_back(); // partial last line (maybe with dangling '\r')
            }
            const fs::path p = root / ("f" + std::to_string(f) + ".txt");
            std::FILE *fh = std::fopen(kimix::to_string(p).c_str(), "wb");
            expect(fh != nullptr);
            if (fh != nullptr) {
                std::fwrite(text.data(), 1, text.size(), fh);
                std::fclose(fh);
            }
            files.emplace_back(s_of(kimix::to_string(p)), text);
        }

        const std::string root_s = s_of(kimix::to_string(root));
        for (int iter = 0; iter < 60; ++iter) {
            // Needle: 80% a (mutated) substring of a real line so hits exist,
            // else fully random 1..4 alphabet chars (mostly-miss probe).
            std::string needle;
            const std::string &srct = files[rng.below(static_cast<uint32_t>(files.size()))].second;
            const std::vector<std::string> sls = fuzz_lines_of_text(srct);
            if (!sls.empty() && rng.below(5) != 0) {
                const std::string &ln = sls[rng.below(static_cast<uint32_t>(sls.size()))];
                if (!ln.empty()) {
                    const size_t st = rng.below(static_cast<uint32_t>(ln.size()));
                    size_t len = 1 + static_cast<size_t>(rng.below(3));
                    if (st + len > ln.size()) {
                        len = ln.size() - st;
                    }
                    needle = ln.substr(st, len);
                    if (rng.below(3) == 0 && needle.size() < 4) {
                        needle += rng.pick(fuzz_alphabet()); // may reach across a boundary
                    }
                }
            }
            if (needle.empty()) {
                const int n = 1 + static_cast<int>(rng.below(4));
                for (int i = 0; i < n; ++i) {
                    needle += rng.pick(fuzz_alphabet());
                }
            }
            const bool ic = rng.below(4) == 0;
            const bool content = rng.below(2) == 0;

            std::vector<std::string> want;
            int64_t want_total = 0;
            int64_t want_files = 0;
            for (const auto &fp : files) {
                const std::vector<std::string> ls = fuzz_lines_of_text(fp.second);
                int64_t cnt = 0;
                int64_t prev_ln = -1000;
                for (size_t li = 0; li < ls.size(); ++li) {
                    if (!fuzz_line_match(ls[li], needle, ic)) {
                        continue;
                    }
                    ++cnt;
                    ++want_total;
                    if (content) {
                        // render_content emits "--" between non-adjacent runs
                        // even with ctx=0 (each match is its own run); the
                        // separator is not a matching line (flag 0, not
                        // counted in total_matches).
                        if (prev_ln > -1000 && static_cast<int64_t>(li) > prev_ln + 1) {
                            want.push_back("--");
                        }
                        want.push_back(fp.first + ":" + std::to_string(li + 1) + ":" + ls[li]);
                        prev_ln = static_cast<int64_t>(li);
                    }
                }
                if (cnt > 0) {
                    ++want_files;
                }
                if (!content && cnt > 0) {
                    want.push_back(fp.first + ":" + std::to_string(cnt));
                }
            }
            std::sort(want.begin(), want.end());

            const std::string lit = fuzz_literal_form(needle);
            const std::string rex = fuzz_regex_form(needle);
            for (const std::string &pat : {lit, rex}) {
                ge::grep_options o = make_opts(pat.c_str());
                o.mode = content ? ge::grep_output_mode::content
                                 : ge::grep_output_mode::count_matches;
                o.ignore_case = ic;
                o.head_limit = 0;
                const ge::grep_result r = run_at(o, kimix::string(root_s));
                expect(r.status == kimix::builtin_tools::tool_status::ok) << pat;
                expect(s_of(r.message) ==
                       std::to_string(want_total) + " match(es) in " +
                           std::to_string(want_files) + " file(s)")
                    << "iter " << iter << " pat=[" << pat << "]";
                std::vector<std::string> got = lines_of(r);
                std::sort(got.begin(), got.end());
                expect(got == want) << "iter " << iter << " ic=" << (ic ? 1 : 0)
                                    << " content=" << (content ? 1 : 0) << " needle=[" << needle
                                    << "] pat=[" << pat << "]\n" << joined(got) << "\nvs\n"
                                    << joined(want);
                expect(r.total_matches == want_total) << "iter " << iter << " pat=[" << pat << "]";
                if (content) {
                    expect(r.line_match.size() == r.lines.size()) << pat;
                    int64_t ones = 0;
                    for (const uint8_t fl : r.line_match) {
                        ones += fl;
                    }
                    // ctx=0: every rendered line is a match (1) except the
                    // "--" separators (0), so the ones must equal the match count.
                    expect(ones == want_total) << pat;
                }
            }
        }
        fs::remove_all(root, ec);
    };
}
