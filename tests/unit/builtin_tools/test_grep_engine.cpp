// Test for builtin_tools/grep_engine.h + .cpp (kimix::builtin_tools::grep).
//
// The grep engine drives the native_io branch of the Grep tool: a
// ripgrep-inspired pure-C++ search (whole-buffer zero-copy line scan, literal
// fast path, per-thread regexes over size-balanced (LPT) chunks merged back
// into walk order, 64 KiB NUL binary sniff,
// 4 MiB per-file cap). These suites pin its observable semantics:
//
// literal path pure-literal patterns ("hit") take the memchr fast path
// and agree with the regex paths ("h.t", "[h]it") on
// counts, files and rendered lines; a literal containing
// '\n' never matches (per-line scan, like the regex scan)
// prefilter (ripgrep-style) flat regex patterns are gated by the longest
// run of bytes every match must contain
// (ge::extract_required_literal): a line - or a whole
// file buffer - without it never reaches regex_lite, and
// every pinned case is compared against a per-line
// regex_lite oracle (pf_oracle), including the lines that
// DO carry the literal but do not match the pattern
// multi-literal alternation "foo|bar|baz" of pure literal
// branches is answered by an any-of substring scan
// (ge::extract_literal_alternation); 1-byte branches,
// empty branches and >16 branches fall back to the engine
// overlong UTF-8 escape hatch a line whose bytes could spell
// an ASCII code point
// overlong (C1 81, E0 81 81, F0 80 81 81) is always
// handed to the engine, so the byte-level prefilter never
// skips a line the engine would match
// (ge::may_hide_ascii_cp / buffer_may_hide_ascii_cp)
// probe byte under -i the folded scan probes the first
// non-letter needle byte with memchr ("todo_urgent")
// parallel + prefilter the shared read-only match plan gives
// byte-identical results on a serial and a
// parallel chunk split
// output modes files_with_matches (path only, head_limit-capped lines but
// complete files[]), count_matches ("path:count", matching
// LINES not occurrences), content ("path:LN:text")
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
// ambient-pool parallelism (a bound caller fans chunks over its
// pool; an UNBOUND caller transiently binds the
// process-wide shared pool, so it fans out too; every
// mode byte-identical to the single-chunk serial path,
// repeated runs identical),
// LPT chunk assignment (Longest-Processing-Time-first over the
// walk's size hints: chunks hold scattered file indices and the
// merge rebuilds walk order from the per-chunk indexed blocks, so
// a skewed corpus - two ~3.2 MiB files among ~8 KiB ones - stays
// byte-identical to the serial scan in files[], in the rendered
// lines, in line_match and in the head_limit cap, and the biggest
// file keeps its WALK position instead of leading the result;
// pinned at the chunk-count boundaries too: n_files ==
// num_chunks (one file per chunk), n_files below the fan-out
// threshold (inline single chunk) and an empty file list),
// seeded literal-vs-regex agreement fuzz: random needles
// over a random corpus, the extract_literal fast path and
// regex_lite must agree with an independent line oracle.
// seeded prefilter-vs-regex fuzz: random regex shapes with
// required literals, alternations and bail-outs over a
// corpus carrying valid / truncated / overlong multi-byte
// sequences, every shape compared against the per-line
// regex_lite oracle in all three modes and both foldings.
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
// ge::literal_in_line  memchr vs folded scan first byte, false-start
// retry after a mismatch (resumes at exactly off+1: an
// occurrence starting right after a failed candidate is
// found), folded tail compare, needle longer
// than hay, empty needle matches every line, and the
// non-letter probe byte under -i ("todo_urgent")
// ge::extract_literal_alternation  any-of literal fast path for
// "foo|bar|baz"; 1-byte / non-ASCII / '\n' / empty branches
// and >16 branches refuse it
// ge::extract_required_literal  longest run every match must contain:
// anchors skipped, '.'/class/'*'/'?' split, '+' restarts,
// groups / '|' / '{' / regex escapes bail
// ge::may_hide_ascii_cp / ge::buffer_may_hide_ascii_cp  overlong UTF-8
// lines and files are handed to the engine instead of being
// skipped by the byte-level prefilter
// ge::line_matches / ge::buffer_cannot_match  the per-line gate and the
// whole-buffer early-out (identical
// answers to the un-prefiltered scan in every mode, with and
// without context lines, serial and parallel)
// ge::collect_lines  partial last line, trailing lone '\r', exactly one
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
//                  small-tree / 1-worker (num_chunks == 1, inline worker) path
//                  and the unbound-caller fiber guarantee (shared-pool bind for
//                  the duration of the call, unbound again on return).

#include "ut/ut.hpp"
#include "ut/ut.hpp"
#include "builtin_tools/grep_engine.h"
#include "builtin_tools/regex_lite.h" // per-line oracle for the prefilter suites

#include <core/fiber.h> // ambient-pool determinism test binds a scheduler


#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <string>
#include <utility>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;
namespace ge = kimix::builtin_tools::grep;
namespace rl = kimix::builtin_tools::regex_lite;

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

// ~64-byte pseudo-log lines ("log " + 50..69 'x' + newline), every 37th line
// carrying " hit" when `with_hit`. A corpus can therefore be built with a KNOWN
// size spread (lines x ~64 bytes) whose match count is a pure function of the
// line count. The LCG padding is seeded, so the same arguments write the same
// bytes on every run - which is what lets a parallel run be compared to a
// serial one byte for byte.
void put_sized_file(const kimix::filesystem::path &p, size_t lines, uint64_t seed,
                    bool with_hit) {
    std::error_code ec;
    kimix::filesystem::create_directories(p.parent_path(), ec);
    std::FILE *f = std::fopen(kimix::to_string(p).c_str(), "wb");
    expect(f != nullptr);
    if (f == nullptr) {
        return;
    }
    kimix::string t;
    t.reserve(lines * 64u);
    uint64_t x = seed * 0x9E3779B97F4A7C15ull + 12345u;
    for (size_t l = 0; l < lines; ++l) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        const size_t pad = 50u + static_cast<size_t>((x >> 33) % 20u);
        t += "log ";
        for (size_t k = 0; k < pad; ++k) {
            t += 'x';
        }
        if (with_hit && (l % 37u) == 5u) {
            t += " hit";
        }
        t += "\n";
    }
    std::fwrite(t.data(), 1, t.size(), f);
    std::fclose(f);
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

// ---------------------------------------------------------------------------
// Prefilter agreement harness (regex-literal prefiltering, ripgrep-style).
//
// Every fast path the engine may take - pure literal, multi-literal
// alternation, required-literal prefilter, or the plain regex path - must
// reproduce what regex_lite answers line by line. This oracle is that
// per-line regex run: it mirrors ge::collect_lines, ge::render_content and the
// count / files_with_matches rendering, so an over-eager prefilter shows up as
// a diff instead of a silent slowdown.
// ---------------------------------------------------------------------------
struct pf_file {
    std::string path; // display path exactly as kimix::to_string(root / rel)
    std::string text; // raw bytes written to disk
};

struct pf_fixture {
    kimix::filesystem::path root;
    std::vector<pf_file> files;

    static void put(const kimix::filesystem::path &p, const std::string &text) {
        std::FILE *f = std::fopen(kimix::to_string(p).c_str(), "wb");
        expect(f != nullptr);
        if (f != nullptr) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        }
    }

    pf_fixture(const char *dir, const std::vector<std::pair<std::string, std::string>> &spec) {
        namespace fs = kimix::filesystem;
        std::error_code ec;
        root = fs::temp_directory_path(ec) / dir;
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
        for (const auto &sp : spec) {
            const fs::path p = root / sp.first;
            put(p, sp.second);
            files.push_back(pf_file{s_of(kimix::to_string(p)), sp.second});
        }
    }
    ~pf_fixture() {
        std::error_code ec;
        kimix::filesystem::remove_all(root, ec);
    }
    kimix::string p(const char *rel) const {
        namespace fs = kimix::filesystem;
        return kimix::to_string(fs::path(rel).is_relative() ? root / rel : fs::path(rel));
    }
};

inline ge::grep_result run_pf(const pf_fixture &fx, const char *pattern, bool ic,
                              ge::grep_output_mode mode, uint32_t before = 0,
                              uint32_t after = 0) {
    ge::grep_options o = make_opts(pattern);
    o.mode = mode;
    o.ignore_case = ic;
    o.ctx_before = before;
    o.ctx_after = after;
    o.head_limit = 0;
    return run_at(o, kimix::to_string(fx.root));
}

// Does `pattern` match `line` according to regex_lite alone (no prefilter)?
inline bool pf_line_match(rl::Regex &re, const std::string &line) {
    size_t mb = 0;
    size_t me = 0;
    return re.search(kimix::string_view(line.data(), line.size()), mb, me);
}

struct pf_expected {
    bool compiled = false;
    std::string error;
    int64_t total = 0;
    int64_t nfiles = 0;
    std::vector<std::string> lines; // sorted (walk order is unspecified)
    std::string message;
};

// The oracle: per-line regex_lite match + the engine's own rendering rules.
inline pf_expected pf_oracle(const pf_fixture &fx, const std::string &pattern, bool ic,
                             ge::grep_output_mode mode, uint32_t before, uint32_t after) {
    pf_expected want;
    rl::Regex re;
    kimix::string err;
    if (!re.compile(kimix::string_view(pattern.data(), pattern.size()), ic, err)) {
        want.error = s_of(err);
        return want; // compiled stays false
    }
    want.compiled = true;
    for (const pf_file &f : fx.files) {
        const std::vector<std::string> ls = fuzz_lines_of_text(f.text);
        std::vector<int64_t> hits;
        for (size_t li = 0; li < ls.size(); ++li) {
            if (pf_line_match(re, ls[li])) {
                hits.push_back(static_cast<int64_t>(li));
            }
        }
        if (hits.empty()) {
            continue;
        }
        want.total += static_cast<int64_t>(hits.size());
        ++want.nfiles;
        if (mode == ge::grep_output_mode::files_with_matches) {
            want.lines.push_back(f.path);
            continue;
        }
        if (mode == ge::grep_output_mode::count_matches) {
            want.lines.push_back(f.path + ":" + std::to_string(hits.size()));
            continue;
        }
        // content: render_content's run merging, clamping and "--" separators
        int64_t last_emitted = -1000;
        for (const int64_t li : hits) {
            const int64_t lo = std::max<int64_t>(0, li - static_cast<int64_t>(before));
            const int64_t hi = std::min<int64_t>(static_cast<int64_t>(ls.size()) - 1,
                                                  li + static_cast<int64_t>(after));
            if (lo > last_emitted + 1 && last_emitted > -999) {
                want.lines.push_back("--");
            }
            for (int64_t l = lo; l <= hi; ++l) {
                if (l <= last_emitted) {
                    continue;
                }
                const char sep = (l == li) ? ':' : '-';
                want.lines.push_back(f.path + sep + std::to_string(l + 1) + sep + ls[static_cast<size_t>(l)]);
                last_emitted = l;
            }
            last_emitted = std::max(last_emitted, hi);
        }
    }
    std::sort(want.lines.begin(), want.lines.end());
    want.message = std::to_string(want.total) + " match(es) in " +
                   std::to_string(want.nfiles) + " file(s)";
    return want;
}

// One pattern, all three modes and both foldings pinned against the oracle.
inline void pf_expect_agreement(const pf_fixture &fx, const std::string &pattern,
                                bool ic, const std::string &tag) {
    const ge::grep_output_mode modes[] = {ge::grep_output_mode::content,
                                          ge::grep_output_mode::count_matches,
                                          ge::grep_output_mode::files_with_matches};
    for (const ge::grep_output_mode m : modes) {
        for (uint32_t ctx = 0; ctx <= 1; ++ctx) {
            const pf_expected want = pf_oracle(fx, pattern, ic, m, ctx, ctx);
            const ge::grep_result r = run_pf(fx, pattern.c_str(), ic, m, ctx, ctx);
            if (!want.compiled) {
                expect(r.status == kimix::builtin_tools::tool_status::invalid_input) << tag;
                expect(s_of(r.message) == "invalid pattern: " + want.error) << tag;
                continue;
            }
            expect(r.status == kimix::builtin_tools::tool_status::ok) << tag << " " << pattern;
            expect(s_of(r.message) == want.message) << tag << " ctx=" << ctx << " [" << pattern << "]";
            std::vector<std::string> got = lines_of(r);
            std::sort(got.begin(), got.end());
            expect(got == want.lines)
                << tag << " ic=" << (ic ? 1 : 0) << " mode=" << static_cast<int>(m)
                << " ctx=" << ctx << " pat=[" << pattern << "]\n" << joined(got) << "\nvs\n"
                << joined(want.lines);
            expect(r.total_matches == want.total) << tag << " [" << pattern << "]";
            expect(static_cast<int64_t>(r.files.size()) == want.nfiles) << tag << " [" << pattern << "]";
        }
    }
}

// Number of rendered lines whose trailing text is exactly `text` (content mode
// renders "path:LN:text" and context "path-LN-text"; a Windows path itself may
// contain ':', so only the tail is compared).
inline int counts_lines(const ge::grep_result &r, const std::string &text) {
    int n = 0;
    for (const kimix::string &l : r.lines) {
        const std::string s = s_of(l);
        if (s.size() >= text.size() &&
            s.compare(s.size() - text.size(), text.size(), text) == 0) {
            ++n;
        }
    }
    return n;
}

// Corpora for the prefilter suites. Every file is crafted so the required
// literal of the pinned patterns occurs in lines that do NOT match them - an
// over-eager prefilter would answer "no match" for the negatives and an
// unsound one would drop the positives, so both directions are covered.
inline std::vector<std::pair<std::string, std::string>> pf_prefilter_spec() {
    return {
        {"flat.txt",
         "TODO nothing here\n"                 // "TODO" without "urgent"
         "nothing urgent either\n"             // "urgent" without "TODO"
         "x TODO -- urgent y\n"                // TODO.*urgent: mid-line, real hit
         "urgent before TODO stays put\n"      // right bytes, wrong ORDER
         "todo URGENT now\n"                   // fold-only hit
         "color\n"
         "colour\n"
         "colr\n"                              // "colo" absent: prefilter rejects too
         "colo\n"                              // required run present, regex fails
         "prefix42x\n"
         "xprefix42x\n"                        // anchor: same bytes, must not match
         "PREFIX7x\n"                          // fold-only anchor hit
         "prefix 42x\n"                        // class fails on the space
         "hit\n"
         "h t\n"
         "ht\n"                                // "h.t" needs three characters
         "th\n"
         "foobar\n"
         "foooobar\n"                          // fo+bar: does NOT contain "fobar"
         "fbar\n"                              // no 'o' at all
         "abc\n"
         "aXc\n"
         "abcd\n"                              // a.c$ must not match
         "TODO_URGENT deadline\n"
         "end\n"},
        {"crlf.txt", "TODO\r\nTODO urgent\r\n\r\n"}, // /r stripped per line
    };
}

inline std::vector<std::pair<std::string, std::string>> pf_alternation_spec() {
    return {
        {"alt.txt",
         "foo bar baz\n"
         "xx foo xx\n"
         "xx bar xx\n"
         "xx baz xx\n"
         "none of these\n"
         "FOO lowercase? no\n"
         "a bc\n"
         "zz bc zz\n"
         "foobarbaz\n"},
        {"fold.txt", "BaR\n"},
    };
}

inline std::vector<std::pair<std::string, std::string>> pf_probe_spec() {
    return {
        {"probe.txt",
         "a_TODO_URGENT_b\n"     // probe '_' hits, folded needle matches
         "todo_urgent\n"
         "TODO_URGEN\n"          // one byte short (probe hit, needle fails)
         "TODO-URGENT\n"         // dash where the needle has an underscore
         "_todo_urgentX\n"
         "todo__urgent\n"        // double underscore: probe hit, needle fails
         "zzz_zyyzy\n"           // probe hits only
         "some_urgent\n"
         "urgent tail\n"},
    };
}

// Overlong UTF-8: regex_lite's lenient decoder turns C1 81 / E0 81 81 /
// F0 80 81 81 into the code point 'A' and E6/E4-led sequences into nothing ASCII
// - the prefilter must consult the engine for the first kind and stay fast for
// the second.
inline std::vector<std::pair<std::string, std::string>> pf_overlong_spec() {
    std::string over;
    over += "x\xC1\x81y\n";        // hidden 'A' (2-byte overlong)
    over += "\xE0\x81\x81z\n";     // hidden 'A' (3-byte overlong)
    over += "\xF0\x80\x81\x81q\n"; // hidden 'A' (4-byte overlong)
    over += "\xC1\x81\xC1\x81 end\n"; // two hidden 'A's in a row ("AA")
    over += "q\xC0\x81q\n";        // overlong lead, but not an 'A'
    over += "plain line\n";
    std::string cjk;
    cjk += "\xE6\x97\xA5\xE6\x9B\xB9\n"; // valid UTF-8, no ASCII code points
    cjk += "\xE4\xB8\xAD\xE6\x96\x87\n";
    cjk += "no capital a here\n";
    std::string nrm;
    nrm += "the needle here\n";
    nrm += "N/A bracket [A] tail\n";
    return {{"over.txt", over}, {"cjk.txt", cjk}, {"nrm.txt", nrm}};
}

// Deterministic random corpus for the prefilter fuzz: short lines, tokens that
// include multi-byte and overlong sequences, no NUL (the engine would treat the
// file as binary, the oracle would not).
inline std::vector<std::pair<std::string, std::string>> pf_fuzz_spec() {
    static const char *toks[] = {"ab",  "cX",  "_",    ".",    "-",   "A",   "x1",  "\xC3\xA9",
                                 "\xC1\x81", "bar", "foo", "todo", "URGENT", "[",  "]",   "0",
                                 "\xE6\x97\xA5", "\xE0\x81\x81", " ", "q"};
    const size_t ntok = sizeof(toks) / sizeof(toks[0]);
    fuzz_rng r(0x11235813213476ULL);
    std::vector<std::pair<std::string, std::string>> spec;
    for (int f = 0; f < 5; ++f) {
        std::string text;
        const int nlines = 3 + static_cast<int>(r.below(6));
        for (int l = 0; l < nlines; ++l) {
            std::string line;
            const int nw = 1 + static_cast<int>(r.below(7));
            for (int w = 0; w < nw; ++w) {
                line += toks[r.below(static_cast<uint32_t>(ntok))];
            }
            if (line.size() > 40) {
                line.resize(40); // may cut a multi-byte sequence: still no NUL
            }
            text += line;
            text += (r.below(5) == 0) ? "\r\n" : "\n";
        }
        if (f == 2 && !text.empty()) {
            text.resize(text.size() - 1); // partial last line
        }
        spec.emplace_back("p" + std::to_string(f) + ".txt", text);
    }
    return spec;
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

      // The engine fans out over the fiber pool of the CALLING thread (it
      // never creates one itself): a bound caller uses its ambient pool, an
      // unbound caller transiently binds the process-wide shared pool for the
      // duration of the run_grep() call. With >= 8 files both spread the work
      // over several workers, and the chunk merge must produce byte-identical
      // results to a genuine single-chunk serial scan, for every mode, the fwm
      // head_limit cap, context runs and the per-file match lines. Also pins:
      // repeated runs are identical (chunk claim order must never leak into
      // the output).
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

          auto run_engine = [](const kimix::string &root_s, const ge::grep_options &o) {
              kimix::vector<kimix::string> roots;
              roots.push_back(root_s);
              const kimix::string work_dir;
              ge::grep_result out;
              ge::run_grep(o, roots, kimix::string_view(work_dir), out);
              return out;
          };
          // Serial oracle: a private ONE-worker pool, so the calling thread is
          // bound (run_grep keeps it, no shared-pool binding happens) and the
          // split collapses to a single chunk that runs inline on this thread
          // - a genuine serial scan of the whole file list. (An unbound
          // caller used to give this away for free; since run_grep binds the
          // shared pool instead of degrading, an unbound call is now a
          // parallel run over that pool, i.e. one of the subjects below.)
          auto run_serial = [&](const kimix::string &root_s, const ge::grep_options &o) {
              ge::grep_result out;
              {
                  kimix::fiber::scheduler one{1u};
                  out = run_engine(root_s, o);
              }
              return out;
          };

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
                  kimix::fiber::scheduler pool{4u};
                  par = run_engine(root_s, o);
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
                  kimix::fiber::scheduler pool{4u};
                  par2 = run_engine(root_s, o);
              }
              expect(lines_of(par2) == lines_of(serial)) << v.pat;
              expect(par2.total_matches == serial.total_matches) << v.pat;
              // And the UNBOUND caller - which now spreads the same chunks
              // over the shared pool the engine bound for the call - must land
              // on the very same bytes as both private-pool runs.
              expect(!kimix::fiber::is_bound());
              const ge::grep_result shared_par = run_engine(root_s, o);
              expect(!kimix::fiber::is_bound()); // the bind was transient
              expect(files_of(shared_par) == files_of(serial)) << v.pat;
              expect(lines_of(shared_par) == lines_of(serial)) << v.pat;
              expect(shared_par.line_match == serial.line_match) << v.pat;
              expect(shared_par.total_matches == serial.total_matches) << v.pat;
              expect(shared_par.message == serial.message) << v.pat;
          }
          fs::remove_all(root, ec);
      };

    // -----------------------------------------------------------------
    // Size-balanced (LPT) chunk assignment + walk-order merge. The chunk a
    // file lands in is now chosen by SIZE (longest job into the chunk with
    // the least work so far), so a chunk's files are scattered over the
    // walk-order list and the merge has to rebuild walk order from the
    // per-chunk indexed output blocks. Two properties must hold at once:
    // (a) the output stays byte-identical to a genuine single-chunk serial
    // scan - same files, same rendered lines, same line_match, same totals
    // and message - however the sizes scatter; (b) it is deterministic
    // across repeated runs and across every way of reaching a pool (the
    // size sort tiebreaks on the walk index, the chunk choice on
    // (running total, files so far, chunk index)).
    // Corpus shapes below: the bench's skew_tree (two ~3.2 MiB under the
    // 4 MiB cap + six ~1 MiB + a dozen ~8 KiB + three non-matching, spread
    // so the WALK order interleaves the sizes) and explicit single-file
    // root lists, whose walk order equals the argument order on every
    // filesystem - that is what lets the order be pinned absolutely rather
    // than relatively.
    // -----------------------------------------------------------------
    "lpt_skew_tree_matches_serial_in_every_mode"_test = [] {
        namespace fs = kimix::filesystem;
        std::error_code ec;
        const fs::path root = fs::temp_directory_path(ec) / "kimix_grep_engine_lpt_skew";
        fs::remove_all(root, ec);
        fs::create_directories(root / "m" / "deep", ec);
        const fs::path huge1 = root / "a_huge1.txt";
        const fs::path huge2 = root / "m" / "deep" / "z_huge2.txt";
        put_sized_file(huge1, 50000u, 7701u, true);
        put_sized_file(huge2, 50000u, 7702u, true);
        // The skew has to be REAL: big enough to dominate the corpus, small
        // enough to stay under the 4 MiB scan cap (a capped file would be
        // skipped and the balance claim would be vacuous).
        const uintmax_t h1 = fs::file_size(huge1, ec);
        expect(h1 > 3ull * 1024 * 1024) << h1;
        expect(h1 <= 4ull * 1024 * 1024) << h1;
        for (int i = 0; i < 6; ++i) {
            char rel[32];
            std::snprintf(rel, sizeof(rel), "mid%d.txt", i);
            put_sized_file(root / rel, 15000u, static_cast<uint64_t>(i) + 500u, true);
        }
        for (int i = 0; i < 12; ++i) {
            char rel[32];
            std::snprintf(rel, sizeof(rel), "s%02d.txt", i);
            put_sized_file(root / rel, 120u, static_cast<uint64_t>(i) + 9000u, true);
        }
        for (int i = 0; i < 3; ++i) {
            char rel[32];
            std::snprintf(rel, sizeof(rel), "z_none%d.txt", i);
            put_sized_file(root / rel, 5000u, static_cast<uint64_t>(i) + 400u, false);
        }
        const kimix::string root_s = kimix::to_string(root);
        const std::string huge1_s = s_of(kimix::to_string(huge1));
        const std::string huge2_s = s_of(kimix::to_string(huge2));

        auto run_engine = [](const kimix::string &r, const ge::grep_options &o) {
            kimix::vector<kimix::string> roots;
            roots.push_back(r);
            const kimix::string work_dir;
            ge::grep_result out;
            ge::run_grep(o, roots, kimix::string_view(work_dir), out);
            return out;
        };
        // Serial oracle: a private ONE-worker pool, so the split collapses to a
        // single inline chunk that scans the whole list on this thread (same
        // pattern as ambient_pool_parallel_matches_serial).
        auto run_serial = [&](const ge::grep_options &o) {
            ge::grep_result out;
            {
                kimix::fiber::scheduler one{1u};
                out = run_engine(root_s, o);
            }
            return out;
        };

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
            {"hit", ge::grep_output_mode::count_matches, -1, 0, 0, false},
            {"hit", ge::grep_output_mode::content, 0, 1, 1, false},
            {"h.t", ge::grep_output_mode::content, 0, 2, 0, false},
            {"hit|miss", ge::grep_output_mode::content, 0, 0, 1, true},
        };
        for (const variant &v : variants) {
            ge::grep_options o = make_opts(v.pat);
            o.mode = v.mode;
            o.head_limit = v.hl;
            o.ctx_before = v.b;
            o.ctx_after = v.a;
            o.ignore_case = v.ic;
            const ge::grep_result serial = run_serial(o);
            expect(serial.total_matches > 0) << v.pat;
            // Both huge files must really be in the result: the merge is only
            // tested against a corpus whose big jobs were scanned, not capped.
            expect(contains(files_of(serial), huge1_s)) << v.pat;
            expect(contains(files_of(serial), huge2_s)) << v.pat;
            ge::grep_result par;
            {
                kimix::fiber::scheduler pool{4u};
                par = run_engine(root_s, o);
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
            // The head_limit cap moved from the chunk-contiguous merge to the
            // walk-ordered one: it must still cap the RENDERED lines to the first
            // head_limit in output order while files[] stays complete.
            if (v.mode == ge::grep_output_mode::files_with_matches && v.hl > 0) {
                expect(par.lines.size() == size_t(v.hl)) << v.pat;
                expect(par.files.size() > par.lines.size()) << v.pat;
                const std::vector<std::string> capped = lines_of(par);
                const std::vector<std::string> full = lines_of(run_serial(o));
                for (size_t i = 0; i < capped.size(); ++i) {
                    expect(capped[i] == full[i]) << v.pat; // a PREFIX, not a subset
                }
            }
            // A second parallel run (the claims land differently) and an UNBOUND
            // run (the transient shared-pool bind) must be the same bytes again.
            ge::grep_result par2;
            {
                kimix::fiber::scheduler pool{4u};
                par2 = run_engine(root_s, o);
            }
            expect(files_of(par2) == files_of(serial)) << v.pat;
            expect(lines_of(par2) == lines_of(serial)) << v.pat;
            expect(par2.line_match == serial.line_match) << v.pat;
            expect(par2.total_matches == serial.total_matches) << v.pat;
            expect(!kimix::fiber::is_bound());
            const ge::grep_result shared = run_engine(root_s, o);
            expect(!kimix::fiber::is_bound());
            expect(files_of(shared) == files_of(serial)) << v.pat;
            expect(lines_of(shared) == lines_of(serial)) << v.pat;
            expect(shared.line_match == serial.line_match) << v.pat;
            expect(shared.total_matches == serial.total_matches) << v.pat;
            expect(shared.message == serial.message) << v.pat;
        }
        fs::remove_all(root, ec);
    };

    // THE regression pin for the change: LPT hands the BIGGEST file to the chunk
    // with the least work, which at assignment time is chunk 0 - the chunk the
    // old contiguous split associated with the FIRST walk indices. A merge that
    // still emitted chunk by chunk would therefore print the big file first.
    // Roots are explicit single-file roots, so the walk order is exactly the
    // argument order (collect_files appends per root) on every filesystem: the
    // sizes below are in the reverse order of the indices, and two files that
    // match nothing at all sit in the middle of the list (no block for them, the
    // neighbours must still keep their order).
    "lpt_assignment_keeps_walk_order_not_size_order"_test = [] {
        namespace fs = kimix::filesystem;
        std::error_code ec;
        const fs::path root = fs::temp_directory_path(ec) / "kimix_grep_engine_lpt_order";
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
        kimix::vector<kimix::string> paths;    // the intended WALK order
        std::vector<std::string> want_files;   // matched subset, same order
        for (int i = 0; i < 4; ++i) {
            char rel[32];
            std::snprintf(rel, sizeof(rel), "s%d.txt", i);
            const fs::path p = root / rel;
            put_sized_file(p, 40u, static_cast<uint64_t>(i) + 3u, true);
            paths.push_back(kimix::to_string(p));
            want_files.push_back(s_of(kimix::to_string(p)));
        }
        const fs::path none_a = root / "none_a.txt"; // in the MIDDLE of the list
        put_sized_file(none_a, 900u, 21u, false);
        paths.push_back(kimix::to_string(none_a));
        for (int i = 4; i < 8; ++i) {
            char rel[32];
            std::snprintf(rel, sizeof(rel), "s%d.txt", i);
            const fs::path p = root / rel;
            put_sized_file(p, 160u - static_cast<uint64_t>(i) * 20u,
                           static_cast<uint64_t>(i) + 3u, true);
            paths.push_back(kimix::to_string(p));
            want_files.push_back(s_of(kimix::to_string(p)));
        }
        const fs::path none_b = root / "none_b.txt";
        put_sized_file(none_b, 900u, 22u, false);
        paths.push_back(kimix::to_string(none_b));
        const fs::path big = root / "big.txt"; // LAST in walk order, BIGGEST by size
        put_sized_file(big, 25000u, 99u, true);
        expect(fs::file_size(big, ec) > 1500ull * 1000) << fs::file_size(big, ec);
        paths.push_back(kimix::to_string(big));
        const std::string big_s = s_of(kimix::to_string(big));
        want_files.push_back(big_s);
        expect(paths.size() == size_t(11)); // >= the 8-file fan-out threshold

        auto run = [&](const kimix::vector<kimix::string> &p, const ge::grep_options &o) {
            const kimix::string work_dir;
            ge::grep_result out;
            ge::run_grep(o, p, kimix::string_view(work_dir), out);
            return out;
        };
        ge::grep_options o = make_opts("hit");
        o.mode = ge::grep_output_mode::count_matches;
        o.ctx_before = 1;
        o.ctx_after = 1;
        o.head_limit = 0;

        ge::grep_result serial;
        {
            kimix::fiber::scheduler one{1u};
            serial = run(paths, o);
        }
        ge::grep_result par;
        {
            kimix::fiber::scheduler pool{4u};
            par = run(paths, o);
        }
        // Absolute order, not just "equal to the oracle": small files first, the
        // big one LAST, the two non-matching files nowhere.
        expect(files_of(par) == want_files) << joined(files_of(par));
        expect(files_of(par).size() == size_t(9));
        expect(files_of(par)[0] == want_files[0]);
        expect(files_of(par).back() == big_s) << joined(files_of(par));
        expect(lines_of(par).back().rfind(big_s, 0) == 0) << lines_of(par).back();
        expect(lines_of(par).front() ==
               want_files[0] + ":" + std::to_string(serial.files[0].match_count))
            << joined(lines_of(par));
        // (the two non-matching files are absent from the exact list above: the
        // comparison is against want_files, which never contains them)
        expect(par.total_matches == serial.total_matches) << par.total_matches;
        expect(lines_of(par) == lines_of(serial)) << joined(lines_of(par));
        expect(files_of(serial) == want_files) << joined(files_of(serial));
        expect(par.line_match == serial.line_match);
        expect(par.message == serial.message);

        // Content mode with context: the same order, and the per-file line spans
        // must not be cut or interleaved by the scattered assignment.
        ge::grep_options oc = make_opts("hit");
        oc.mode = ge::grep_output_mode::content;
        oc.ctx_before = 1;
        oc.ctx_after = 1;
        oc.head_limit = 0;
        ge::grep_result c_serial;
        {
            kimix::fiber::scheduler one{1u};
            c_serial = run(paths, oc);
        }
        ge::grep_result c_par;
        {
            kimix::fiber::scheduler pool{4u};
            c_par = run(paths, oc);
        }
        expect(lines_of(c_par) == lines_of(c_serial)) << joined(lines_of(c_par));
        expect(c_par.line_match == c_serial.line_match);
        expect(files_of(c_par) == want_files) << joined(files_of(c_par));
        expect(c_par.files.back().path == kimix::to_string(big));
        expect(c_par.total_matches == c_serial.total_matches);
        // Every rendered line of the big file sits at the END of lines[] (the
        // per-file blocks are appended whole, in walk order).
        // Sentinel SIZE_MAX, not 0: index 0 is a legal answer here (the first
        // rendered line IS the first file's), so 0 cannot mean "not found".
        size_t last_big = 0;
        size_t first_small = static_cast<size_t>(-1);
        for (size_t i = 0; i < c_par.lines.size(); ++i) {
            const kimix::string &l = c_par.lines[i];
            if (l.rfind(big_s, 0) == 0) {
                last_big = i;
            }
            if (l.rfind(want_files[0], 0) == 0 && first_small == static_cast<size_t>(-1)) {
                first_small = i;
            }
        }
        expect(last_big == c_par.lines.size() - 1u) << last_big;
        expect(first_small == 0u) << first_small;

        // Empty result set over the very same scattered list: nothing matches, so
        // every chunk ends up with zero blocks and the merge emits nothing.
        ge::grep_options none_o = make_opts("zzz_no_such_pattern_zzz");
        none_o.mode = ge::grep_output_mode::content;
        ge::grep_result empty_serial;
        {
            kimix::fiber::scheduler one{1u};
            empty_serial = run(paths, none_o);
        }
        ge::grep_result empty_par;
        {
            kimix::fiber::scheduler pool{4u};
            empty_par = run(paths, none_o);
        }
        expect(empty_par.status == kimix::builtin_tools::tool_status::ok);
        expect(empty_par.files.empty());
        expect(empty_par.lines.empty());
        expect(empty_par.line_match.empty());
        expect(empty_par.total_matches == 0);
        expect(empty_par.message == kimix::string("0 match(es) in 0 file(s)"));
        expect(empty_par.message == empty_serial.message);
        fs::remove_all(root, ec);
    };

    // The chunk-count boundaries around the new assignment, pinned with pools of
    // an exact width so num_chunks is known rather than machine-dependent:
    //   - n_files == num_chunks (8 files, 8 workers): every chunk holds exactly
    //     one file, so the merge is pure walk-order reconstruction;
    //   - n_files < k_min_fanout (5 files): num_chunks collapses to 1 and the
    //     inline worker(0) runs the whole list through the SAME merge;
    //   - an empty file list: the merge loop never runs, the result is the zero
    //     message (unsigned arithmetic in lpt_assign must not underflow).
    "lpt_merge_boundaries_and_inline_path"_test = [] {
        namespace fs = kimix::filesystem;
        std::error_code ec;
        const fs::path root = fs::temp_directory_path(ec) / "kimix_grep_engine_lpt_edge";
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
        kimix::vector<kimix::string> eight;
        std::vector<std::string> want_eight;
        for (int i = 0; i < 8; ++i) {
            char rel[32];
            std::snprintf(rel, sizeof(rel), "e%d.txt", i);
            const fs::path p = root / rel;
            // Descending sizes against ascending indices again (e7 is the big one).
            put_sized_file(p, i == 7 ? 4000u : static_cast<uint64_t>(7 - i) * 40u,
                           static_cast<uint64_t>(i) + 60u, true);
            eight.push_back(kimix::to_string(p));
            want_eight.push_back(s_of(kimix::to_string(p)));
        }
        auto run = [&](const kimix::vector<kimix::string> &p, const ge::grep_options &o) {
            const kimix::string work_dir;
            ge::grep_result out;
            ge::run_grep(o, p, kimix::string_view(work_dir), out);
            return out;
        };
        ge::grep_options o = make_opts("hit");
        o.mode = ge::grep_output_mode::count_matches;
        o.head_limit = 0;
        ge::grep_result serial;
        {
            kimix::fiber::scheduler one{1u};
            serial = run(eight, o);
        }
        ge::grep_result each; // n_files == num_chunks == 8
        {
            kimix::fiber::scheduler pool{8u};
            each = run(eight, o);
        }
        expect(files_of(each) == want_eight) << joined(files_of(each));
        expect(files_of(each) == files_of(serial)) << joined(files_of(each));
        expect(lines_of(each) == lines_of(serial)) << joined(lines_of(each));
        expect(each.total_matches == serial.total_matches);
        expect(each.message == serial.message);
        expect(each.files.back().path == kimix::to_string(root / "e7.txt"));

        ge::grep_result fwm; // the fwm cap with one block per chunk
        o.mode = ge::grep_output_mode::files_with_matches;
        o.head_limit = 2;
        {
            kimix::fiber::scheduler pool{8u};
            fwm = run(eight, o);
        }
        expect(fwm.lines.size() == size_t(2));
        expect(fwm.files.size() == size_t(8));
        expect(lines_of(fwm) == std::vector<std::string>{want_eight[0], want_eight[1]});
        expect(fwm.total_matches == serial.total_matches);
        o.head_limit = 250;

        // Below the fan-out threshold: single inline chunk, same merge code.
        kimix::vector<kimix::string> five;
        std::vector<std::string> want_five;
        for (int i = 0; i < 5; ++i) {
            five.push_back(eight[3u + static_cast<size_t>(i)]); // last five roots
            want_five.push_back(s_of(five[i]));
        }
        ge::grep_result inline_small;
        {
            kimix::fiber::scheduler pool{8u};
            inline_small = run(five, o);
        }
        expect(files_of(inline_small) == want_five) << joined(files_of(inline_small));
        expect(inline_small.files.size() == size_t(5));
        expect(inline_small.lines.size() == size_t(5));
        ge::grep_result inline_serial;
        {
            kimix::fiber::scheduler one{1u};
            inline_serial = run(five, o);
        }
        expect(files_of(inline_serial) == want_five);
        expect(inline_small.total_matches == inline_serial.total_matches);

        // Empty walk (a directory with no files at all) plus the zero-file reset.
        const fs::path empty_dir = root / "nothing_here";
        fs::create_directories(empty_dir, ec);
        kimix::vector<kimix::string> no_files;
        no_files.push_back(kimix::to_string(empty_dir));
        ge::grep_result zero;
        {
            kimix::fiber::scheduler pool{4u};
            zero = run(no_files, o);
        }
        expect(zero.status == kimix::builtin_tools::tool_status::ok);
        expect(zero.files.empty());
        expect(zero.lines.empty());
        expect(zero.line_match.empty());
        expect(zero.total_matches == 0);
        expect(zero.message == kimix::string("0 match(es) in 0 file(s)"));
        fs::remove_all(root, ec);
    };

      // The engine's fiber guarantee: a calling thread WITHOUT a scheduler
      // bound still gets the fan-out. run_grep binds the process-wide shared
      // pool for the duration of the call - transient binding, never creation:
      // the pool is the host's - and the guard unbinds it again before
      // returning, so no binding leaks into the caller (a leaked binding would
      // make a later kimix::fiber::scheduler construction on that thread abort
      // in marl). Being unbound is not a licence to scan serially.
      // 12 files >= the engine's 8-file fan-out threshold with a shared pool
      // of > 1 worker, so the split really submits tasks instead of collapsing
      // to the inline chunk. Pinned: correct counts and walk order for the
      // unbound caller, no binding left behind, repeated runs identical, and
      // byte-identical results however the pool is reached - the transient
      // bind, the caller holding that same shared pool bound (the "bind once
      // at the root main" model), a private pool, or a FOREIGN unbound
      // std::thread (the Python-kernel / host callback case).
      "unbound_caller_still_gets_fiber_fanout"_test = [] {
          namespace fs = kimix::filesystem;
          std::error_code ec;
          const fs::path root =
              fs::temp_directory_path(ec) / "kimix_grep_engine_unbound";
          fs::remove_all(root, ec);
          fs::create_directories(root / "d1", ec);
          fs::create_directories(root / "d2", ec);
          // 12 files over 3 dirs; the 6 even-indexed ones carry two "hit"
          // lines each -> 12 matching lines in 6 files, the odd ones no hit.
          int want_matches = 0;
          int want_files = 0;
          for (int i = 0; i < 12; ++i) {
              char rel[64];
              std::snprintf(rel, sizeof(rel), "%s/f%02d.txt",
                            i % 3 == 0 ? "d1" : (i % 2 == 0 ? "d2" : "."), i);
              kimix::string text;
              if (i % 2 == 0) {
                  text = "alpha hit one\nbeta\ngamma hit two\n";
                  want_matches += 2;
                  ++want_files;
              } else {
                  text = "alpha\nbeta\n";
              }
              fixture::put(root / rel, text.c_str());
          }
          kimix::vector<kimix::string> roots;
          roots.push_back(kimix::to_string(root));
          const kimix::string work_dir;
          auto run_with = [&](const ge::grep_options &o) {
              ge::grep_result out;
              ge::run_grep(o, roots, kimix::string_view(work_dir), out);
              return out;
          };
          // Precondition of a real fan-out: the pool the guard binds is wider
          // than one worker (else num_chunks collapses to 1 and the unbound
          // run would be serial - the equality checks below stay valid, only
          // the "fan-out" claim would go unproven). KIMIX_FIBER_WORKER_THREADS
          // is the documented knob that sizes the shared pool, so the claim is
          // checked unless the environment deliberately narrowed it to 1.
          kimix::fiber::shared_scheduler().bind();
          const uint32_t shared_workers = kimix::fiber::worker_thread_count();
          kimix::fiber::shared_scheduler().unbind();
          expect(!kimix::fiber::is_bound());
          const char *pool_env = std::getenv("KIMIX_FIBER_WORKER_THREADS");
          const bool pool_narrowed = pool_env != nullptr && std::atoi(pool_env) <= 1;
          if (!pool_narrowed) {
              expect(shared_workers > 1u) << "shared fiber pool needs > 1 worker";
          }

          ge::grep_options fwm = make_opts("hit");
          fwm.mode = ge::grep_output_mode::files_with_matches;
          expect(!kimix::fiber::is_bound());
          const ge::grep_result unbound = run_with(fwm);
          expect(!kimix::fiber::is_bound()); // the bind was transient
          expect(unbound.status == kimix::builtin_tools::tool_status::ok);
          expect(unbound.total_matches == want_matches);
          expect(static_cast<int>(unbound.files.size()) == want_files);
          expect(unbound.message == kimix::string("12 match(es) in 6 file(s)"));
          // Deterministic: a second unbound run (claims land on different
          // workers) reports the very same files in the very same order.
          const ge::grep_result again = run_with(fwm);
          expect(!kimix::fiber::is_bound());
          expect(files_of(again) == files_of(unbound));
          expect(again.message == unbound.message);

          // Content mode (context runs + per-line flags) across the three
          // ways of reaching a pool: unbound (guard binds the shared pool),
          // ambient (caller bound to that same shared pool), private pool.
          ge::grep_options ctx = make_opts("hit");
          ctx.mode = ge::grep_output_mode::content;
          ctx.ctx_before = 1;
          ctx.ctx_after = 1;
          ctx.head_limit = 0;
          const ge::grep_result u_ctx = run_with(ctx);
          expect(!kimix::fiber::is_bound());
          expect(u_ctx.total_matches == want_matches);
          // Every rendered match line is flagged 1, context/separator 0, and
          // each hit line is emitted exactly once: the ones must add up.
          int64_t ones = 0;
          for (const uint8_t fl : u_ctx.line_match) {
              ones += fl ? 1 : 0;
          }
          expect(ones == want_matches) << joined(lines_of(u_ctx));
          ge::grep_result a_ctx;
          {
              kimix::fiber::shared_scheduler().bind();
              a_ctx = run_with(ctx);
              kimix::fiber::shared_scheduler().unbind();
          }
          ge::grep_result p_ctx;
          {
              kimix::fiber::scheduler pool{4u};
              p_ctx = run_with(ctx);
          }
          expect(lines_of(a_ctx) == lines_of(u_ctx)) << joined(lines_of(a_ctx));
          expect(lines_of(p_ctx) == lines_of(u_ctx)) << joined(lines_of(p_ctx));
          expect(a_ctx.line_match == u_ctx.line_match);
          expect(p_ctx.line_match == u_ctx.line_match);
          expect(a_ctx.files.size() == u_ctx.files.size());
          expect(p_ctx.total_matches == u_ctx.total_matches);
          for (size_t i = 0; i < p_ctx.files.size(); ++i) {
              expect(p_ctx.files[i].path == u_ctx.files[i].path);
              expect(p_ctx.files[i].match_count == u_ctx.files[i].match_count);
          }
          // The real target of the guarantee: a FOREIGN thread (a Python
          // kernel call, a host callback) that never bound anything. The
          // engine binds the shared pool on that thread for the call and
          // unbinds it before the thread exits. Assertions stay on the main
          // thread (ut's reporter is not thread-safe); the worker only fills
          // the results it is joined for.
          ge::grep_result f_foreign;
          ge::grep_result c_foreign;
          std::thread([&] {
              f_foreign = run_with(fwm);
              c_foreign = run_with(ctx);
          }).join();
          expect(!kimix::fiber::is_bound());
          expect(f_foreign.status == kimix::builtin_tools::tool_status::ok);
          expect(f_foreign.total_matches == want_matches);
          expect(files_of(f_foreign) == files_of(unbound));
          expect(f_foreign.message == unbound.message);
          expect(lines_of(c_foreign) == lines_of(u_ctx)) << joined(lines_of(c_foreign));
          expect(c_foreign.line_match == u_ctx.line_match);
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
    // num_chunks == 1 clamp -> the inline worker(0) branch of run_grep (a
    // file list below the fan-out threshold never dispatches, whatever pool
    // the calling thread has).
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

    // ==================================================== prefilter (ripgrep)
    // ge::extract_required_literal / ge::extract_literal_alternation /
    // ge::line_matches / ge::buffer_cannot_match / ge::may_hide_ascii_cp:
    // a flat regex pattern gets the longest run of bytes EVERY match must
    // contain, and lines (or whole file buffers) without it never reach the
    // engine. The contract is "same answers, less work", so every case below
    // is pinned against the per-line regex_lite oracle (pf_oracle) in all
    // three output modes, with -B/-A = 0 and 1, case-sensitive and folded.

    // The negative cases are the point: a line that CONTAINS the required
    // literal but does not match the pattern must still be rejected, and a
    // line that matches must never be dropped by the gate.
    "prefilter_flat_patterns_agree_with_regex"_test = [] {
        pf_fixture fx("kimix_grep_engine_prefilter", pf_prefilter_spec());
        const char *pats[] = {
            "TODO.*urgent", "colou?r", "^prefix[0-9]+x", "h.t", "fo+bar",
            "a.c$",         "prefix",  "bar+",           "x?TODO",
            "TODO[^x]*urgent", "\\.dot", "end$",          "o+r",
        };
        for (const char *p : pats) {
            pf_expect_agreement(fx, p, false, "flat");
            pf_expect_agreement(fx, p, true, "flat-i");
        }
        // Explicit pins of the shape a wrong prefilter would break: "fo+bar"
        // matches "foooobar", which does NOT contain "fobar" - so the run may
        // not merge across a '+' repeat - and "^prefix[0-9]+x" must reject the
        // unanchored "xprefix42x" even though it carries the "prefix" run.
        const ge::grep_result plus = run_pf(fx, "fo+bar", false, ge::grep_output_mode::content);
        expect(plus.total_matches == 2) << plus.total_matches;
        expect(counts_lines(plus, "foobar") == 1 && counts_lines(plus, "foooobar") == 1);
        expect(counts_lines(plus, "fbar") == 0);
        const ge::grep_result anc = run_pf(fx, "^prefix[0-9]+x", false, ge::grep_output_mode::content);
        expect(anc.total_matches == 1) << anc.total_matches;
        expect(counts_lines(anc, "xprefix42x") == 0);
        expect(counts_lines(anc, "prefix 42x") == 0);
        // ... while the folded variant of the same pattern does hit the
        // uppercase line: the required bytes may appear ASCII-folded in the
        // file, which is what the fold argument of literal_in_line is for.
        const ge::grep_result anci = run_pf(fx, "^prefix[0-9]+x", true, ge::grep_output_mode::content);
        expect(anci.total_matches == 2) << anci.total_matches;
        expect(counts_lines(anci, "PREFIX7x") == 1);
    };

    // ge::extract_required_literal bail-outs: groups, counted quantifiers and
    // mixed alternations keep the plain regex path - the results must be
    // exactly what the engine produced before the prefilter existed.
    "prefilter_bailout_patterns_smoke"_test = [] {
        pf_fixture fx("kimix_grep_engine_prefilter", pf_prefilter_spec());
        const char *bails[] = {
            "(a)b", "a{2}b", "x|y+", "(TODO).*urgent", "a\\d*b", "[a-z]+",
            "(foo|bar)baz", "a{2,3}", "()", "^(TODO)$", "TODO\\bx", "\\x41todo",
        };
        for (const char *p : bails) {
            pf_expect_agreement(fx, p, false, "bail");
            pf_expect_agreement(fx, p, true, "bail-i");
        }
    };

    // ge::extract_literal_alternation: a top-level alternation of pure
    // literals IS an any-of substring scan, the regex engine never runs. One
    // byte branches and empty branches refuse the fast path (and must still be
    // answered correctly by the engine: an empty alternative matches every
    // line), and a branch count over the cap falls back to the regex path.
    "prefilter_multi_literal_alternation"_test = [] {
        pf_fixture fx("kimix_grep_engine_alts", pf_alternation_spec());
        const char *pats[] = {
            "foo|bar|baz", "FOO|bar", "a|bc", "baz|quux|xx", "foobar|zz bc zz",
            "none of these|foo bar baz", "BAZ|Foo",
            // An alt that is a PREFIX of another ("foo" of "foobar") and
            // alts sharing a common prefix ("fooba*") - the single-pass
            // any-of fingerprint scan must not stop at the shorter alt
            // and must not confuse the shared-prefix candidates.
            "foo|foobar", "foobar|foo", "foobaz|foobar", "fo|foo|foob",
        };
        for (const char *p : pats) {
            pf_expect_agreement(fx, p, false, "alts");
            pf_expect_agreement(fx, p, true, "alts-i");
        }
        // Exactly k_max_alt_literals (16) branches: the single-pass
        // any-of scan's full table, with and without folding.
        {
            std::string a16;
            static const char *tok[] = {"aa", "bc", "cd", "de", "ef", "fg",
                                        "gh", "hi", "ij", "jk", "kl", "lm",
                                        "mn", "no", "op", "qr"};
            for (int i = 0; i < 16; ++i) {
                if (i > 0) {
                    a16 += '|';
                }
                a16 += tok[i];
            }
            pf_expect_agreement(fx, a16, false, "alts-16");
            pf_expect_agreement(fx, a16, true, "alts-16-i");
            // One branch too many: the extractor refuses and the regex
            // path answers instead (same counts).
            a16 += "|xs";
            pf_expect_agreement(fx, a16, false, "alts-17");
        }
        // Refusals: empty branch, 1-byte branch, and > k_max_alt_literals
        // branches all take the regex path with the same answers.
        pf_expect_agreement(fx, "foo|", false, "alts-empty");
        pf_expect_agreement(fx, "|foo", false, "alts-empty2");
        pf_expect_agreement(fx, "a|bc", false, "alts-short");
        std::string many;
        for (char c = 'b'; c <= 'r'; ++c) { // 17 two-byte branches
            if (!many.empty()) {
                many += '|';
            }
            many += 'x';
            many += c;
        }
        pf_expect_agreement(fx, many, false, "alts-many");
        pf_expect_agreement(fx, many, true, "alts-many-i");
        // Pinned hits of the fast path itself (1 line, several needles).
        // Pinned hits of the fast path itself: five lines carry foo, bar or
        // baz (case-sensitive, so "FOO lowercase? no" and fold.txt stay out).
        const ge::grep_result r = run_pf(fx, "foo|bar|baz", false, ge::grep_output_mode::count_matches);
        expect(r.total_matches == 5) << r.total_matches;
        expect(r.files.size() == size_t(1)) << r.files.size();
        const ge::grep_result ri = run_pf(fx, "foo|bar|baz", true, ge::grep_output_mode::count_matches);
        expect(ri.total_matches == 7) << ri.total_matches;
        expect(ri.files.size() == size_t(2)) << ri.files.size();
    };

    // ge::literal_in_line probe byte: under -i the scan probes the first
    // NON-letter needle byte (such a byte never needs folding, so memchr stays
    // SIMD-fast) and verifies the whole folded needle at hit minus the probe
    // offset. Behavior must be byte-identical, so the corpus is built to
    // produce probe hits that the needle does not follow.
    "prefilter_probe_byte_non_letter"_test = [] {
        pf_fixture fx("kimix_grep_engine_probe", pf_probe_spec());
        const char *pats[] = {"todo_urgent", "todo_urgent.*x", "TOD_URGENT", "_TODO|urgent",
                              "x_todo", "urgent$"};
        for (const char *p : pats) {
            pf_expect_agreement(fx, p, false, "probe");
            pf_expect_agreement(fx, p, true, "probe-i");
        }
        const ge::grep_result r = run_pf(fx, "todo_urgent", true, ge::grep_output_mode::content);
        expect(r.total_matches == 3) << r.total_matches << joined(lines_of(r));
        expect(counts_lines(r, "TODO-URGENT") == 0); // dash, not underscore
        expect(counts_lines(r, "todo_URGEN") == 0);  // one byte short
    };

    // ge::buffer_cannot_match across the 64 KiB sniff window: the required
    // literal sits past the first 64 KiB of the file, so a whole-buffer check
    // that only looked at the sniffed head would wrongly skip the file. The
    // match must still be found, and a file that carries the literal nowhere
    // must still produce nothing.
    "prefilter_buffer_gate_across_the_64kib_sniff_boundary"_test = [] {
        std::string big;
        while (big.size() < 70u * 1024u) {
            big += " filler line that cannot match anything\n";
        }
        // The needle sits at a line past 64 KiB, twice: "RARE" then "detail".
        big += "RARE token and detail follows\n";
        big += "trailing filler RARE alone\n";
        std::string quiet = big;
        const std::string tail = big.substr(70u * 1024u);
        expect(tail.find("RARE token and detail follows") != std::string::npos);
        pf_fixture fx("kimix_grep_engine_big",
                      {{"big.txt", big}, {"quiet.txt", "RARE never pairs with nothing\n"}});
        const pf_expected want = pf_oracle(fx, "RARE.*detail", false, ge::grep_output_mode::content, 0, 0);
        const ge::grep_result r = run_pf(fx, "RARE.*detail", false, ge::grep_output_mode::content);
        expect(want.compiled);
        expect(want.total == 1) << want.total;
        expect(r.total_matches == 1) << r.total_matches;
        expect(s_of(r.message) == want.message) << s_of(r.message);
        std::vector<std::string> got = lines_of(r);
        std::sort(got.begin(), got.end());
        expect(got == want.lines) << joined(got) << "\nvs\n" << joined(want.lines);
        expect(counts_lines(r, "RARE token and detail follows") == 1);
        expect(counts_lines(r, "RARE never pairs with nothing") == 0);
        // Same file, count mode + context: the gate must not disturb the run
        // rendering either.
        pf_expect_agreement(fx, "RARE.*detail", false, "big");
        pf_expect_agreement(fx, "RARE.*detail", true, "big-i");
        pf_expect_agreement(fx, "FILLER.*x", false, "big-none");
    };

    // ge::may_hide_ascii_cp: regex_lite's decoder folds OVERLONG UTF-8
    // sequences back into ASCII code points (bytes C1 81 decode to 'A'), so a
    // line can match an ASCII literal without ever containing its bytes. The
    // per-line gate must fall back to the engine for such a line, and the
    // whole-buffer early-out must not skip such a file. A valid multi-byte
    // sequence (lead C2-DF / E1-EF / F1-F7) never decodes to an ASCII code
    // point, so those files keep the full prefilter and must agree too.
    "prefilter_overlong_utf8_is_not_skipped"_test = [] {
        pf_fixture fx("kimix_grep_engine_overlong", pf_overlong_spec());
        const char *pats[] = {"A.", "needle", "^A", "A|x", "BA..D", "\\[A\\]",
                              // Two-byte branches, so the alternation fast path
                              // is taken and must fall back on the overlong line
                              // too (a byte any-of would miss "AA" spelled twice
                              // overlong).
                              "AA|zz", "AA|needle", "en|ds"};
        for (const char *p : pats) {
            pf_expect_agreement(fx, p, false, "overlong");
            pf_expect_agreement(fx, p, true, "overlong-i");
        }
        // The positive control: all three overlong 'A' lines of over.txt really
        // do match "A." (plus the plain "[A]" in nrm.txt), while the CJK file
        // (valid UTF-8: leads E6/E4, no overlong-capable lead) matches nothing
        // - so the guard is not simply disabled by any high byte.
        // 4 overlong-'A' lines of over.txt (C1 81, E0 81 81, F0 80 81 81 and the
        // double C1 81 line) plus the literal "[A]" in nrm.txt.
        const ge::grep_result r = run_pf(fx, "A.", false, ge::grep_output_mode::count_matches);
        expect(r.total_matches == 5) << r.total_matches << joined(lines_of(r));
        expect(r.files.size() == size_t(2)) << r.files.size();
        bool over_hit = false;
        bool cjk_out = true;
        for (const ge::grep_file_result &f : r.files) {
            if (f.path.find("over.txt") != std::string::npos) {
                over_hit = (f.match_count == 4);
            }
            if (f.path.find("cjk.txt") != std::string::npos) {
                cjk_out = false;
            }
        }
        expect(over_hit);
        expect(cjk_out);
        // The alternation fast path on the same file: "AA" occurs only as the
        // double overlong sequence, so any-of alone would answer "no match".
        const ge::grep_result aa = run_pf(fx, "AA|zz", false, ge::grep_output_mode::count_matches);
        expect(aa.total_matches == 1) << aa.total_matches << joined(lines_of(aa));
        const ge::grep_result n = run_pf(fx, "Q.", false, ge::grep_output_mode::content);
        expect(n.total_matches == 0) << n.total_matches;
    };

    // ------------------------------------------------------------------
    // Seeded prefilter-vs-regex fuzz: random regex shapes built around literal
    // cores taken from the corpus (so hits exist) plus a mandatory
    // metacharacter piece (so the pattern never is a pure literal - the
    // existing use_literal path answers with byte semantics by design and is
    // covered by fuzz_literal_regex_agreement above). Every shape is compared
    // against the per-line regex_lite oracle in all three modes, with -B/-A 0
    // and 1, in both foldings. The corpus carries valid multi-byte, TRUNCATED
    // and overlong sequences for the byte-vs-code-point guard (may_hide_ascii_cp)
    // and never a NUL, which would make the engine skip the file as binary.
    // ------------------------------------------------------------------
    // The match plan is computed ONCE on the calling thread and then read by
    // every chunk worker, so the prefilter paths must not make a parallel run
    // differ from a serial one in any mode (mirrors
    // ambient_pool_parallel_matches_serial for the new fast paths).
    "prefilter_parallel_matches_serial"_test = [] {
        std::vector<std::pair<std::string, std::string>> spec;
        for (int i = 0; i < 24; ++i) {
            std::string text = "TODO nothing here\n";
            if (i % 4 == 0) {
                text += "x TODO -- urgent y\n";
            }
            if (i % 5 == 0) {
                text += "colour PREFIX9x foobar\n";
            }
            if (i % 3 == 0) {
                text += "foooobar and a foo line\n";
            }
            text += "zz bc zz\nlast line\n";
            spec.emplace_back("pf" + std::to_string(i) + ".txt", text);
        }
        pf_fixture fx("kimix_grep_engine_parfilter", spec);
        struct variant {
            const char *pat;
            ge::grep_output_mode mode;
            uint32_t b, a;
            bool ic;
        };
        const variant variants[] = {
            {"TODO.*urgent", ge::grep_output_mode::content, 0, 0, false},
            {"TODO.*urgent", ge::grep_output_mode::content, 1, 1, true},
            {"TODO.*urgent", ge::grep_output_mode::count_matches, 0, 0, false},
            {"colou?r", ge::grep_output_mode::files_with_matches, 0, 0, false},
            {"^PREFIX[0-9]+x", ge::grep_output_mode::content, 0, 1, true},
            {"fo+bar", ge::grep_output_mode::content, 0, 0, false},
            {"foo|bar|baz", ge::grep_output_mode::content, 1, 0, false},
            {"foo|bar|baz", ge::grep_output_mode::count_matches, 0, 0, true},
            {"a|bc", ge::grep_output_mode::content, 0, 0, false},
            {"(TODO).*urgent", ge::grep_output_mode::content, 0, 0, false},
            {"h[a-z]t", ge::grep_output_mode::files_with_matches, 0, 0, true},
        };
        for (const variant &v : variants) {
            ge::grep_options o = make_opts(v.pat);
            o.mode = v.mode;
            o.ctx_before = v.b;
            o.ctx_after = v.a;
            o.ignore_case = v.ic;
            o.head_limit = 0;
            // Serial: a private ONE-worker pool makes the split collapse to a
            // single inline chunk on this thread.
            ge::grep_result serial;
            {
                kimix::fiber::scheduler one{1u};
                const kimix::vector<kimix::string> roots{kimix::to_string(fx.root)};
                const kimix::string work_dir;
                ge::run_grep(o, roots, kimix::string_view(work_dir), serial);
            }
            // Parallel: unbound caller -> run_grep binds the shared pool and
            // spreads the 24 files over >= 2 chunks.
            ge::grep_result par = run_pf(fx, v.pat, v.ic, v.mode, v.b, v.a);
            expect(par.status == kimix::builtin_tools::tool_status::ok) << v.pat;
            expect(par.total_matches == serial.total_matches) << v.pat;
            expect(s_of(par.message) == s_of(serial.message)) << v.pat;
            expect(lines_of(par) == lines_of(serial))
                << v.pat << "\n" << joined(lines_of(par)) << "\nvs\n" << joined(lines_of(serial));
            expect(files_of(par) == files_of(serial)) << v.pat;
            expect(par.line_match == serial.line_match) << v.pat;
        }
    };

    // Differential test for the bounded backtracker: regex_lite::search()
    // (backtracker + step budget + NFA fallback) and regex_lite::search_nfa()
    // (pure pike VM) must agree on boolean AND span for every accepted pattern
    // over small texts. The pattern set deliberately includes the classic
    // exponential shapes - nested quantifiers, alternation under repetition,
    // chained .* runs, lazy variants, counted reps - so the budget trips on
    // several of them and the NFA path answers; a mismatch here is a
    // correctness bug in the fallback, not a perf issue.
    "regex_bounded_search_agrees_with_nfa"_test = [] {
        struct pat_text {
            const char *pat;
            const char *text;
        };
        const char *pats[] = {
            "a",       "ab",      "a|b",     "a*",      "a+",
            "a?",      "(a+)+",   "(a+)+$",  "(a|b)*",  "(a|b)+c",
            "(x+x+)+y", "q.*q.*q", ".*.*.*$", "a*b*c*",  "(a*)*",
            "(a*)+",   "a{2,4}",  "a{2,}",   "(ab|a)b", "(a|ab)*",
            "^(a+)+$", "a.*b",    "(a|b|c)*c$", "[ab]+", "[^a]b",
            "(a+b)+",  "(a?a)*a", "h.t",     "(hit|miss)+", "(a|)",
            "()",      "a*?b",    "(a+?)*b", "(a|b)*?c", "z{3}",
        };
        const char *texts[] = {"",    "a",     "b",      "ab",     "ba",
            "aa",   "aaa",   "aab",    "abab",   "abc",
            "aaaa", "aaab",  "baba",   "ababab", "aaaaab",
            "abcabc", "abababc", "xyz", "qqqq", "aabbcc"};
        int checked = 0;
        for (const char *pat : pats) {
            for (bool ic : {false, true}) {
                rl::Regex re;
                kimix::string err;
                if (!re.compile(kimix::string_view(pat, std::strlen(pat)), ic,
                                 err)) {
                    continue; // validator rejects it: nothing to compare
                }
                for (const char *tx : texts) {
                    const kimix::string_view tv(tx, std::strlen(tx));
                    size_t b1 = 9999, e1 = 9999, b2 = 8888, e2 = 8888;
                    const bool r1 = re.search(tv, b1, e1);
                    const bool r2 = re.search_nfa(tv, b2, e2);
                    ++checked;
                    expect(r1 == r2)
                        << "search vs nfa bool [" << pat << "] ic=" << (ic ? 1 : 0)
                        << " [" << tx << "]";
                    if (r1 && r2) {
                        expect(b1 == b2 && e1 == e2)
                            << "search vs nfa span [" << pat << "] ic="
                            << (ic ? 1 : 0) << " [" << tx << "] (" << b1 << ","
                            << e1 << ") vs (" << b2 << "," << e2 << ")";
                    }
                }
            }
        }
        expect(checked > 1000) << checked;
    };
    // The blow-up guard itself: adversarial patterns over long NON-matching
    // lines must answer (with zero matches) instead of burning exponential
    // time. Before the step budget these took minutes at KiB line lengths;
    // the budget trips and the NFA settles the line in microseconds. A wrong
    // answer here would be worse than a slow one: zero matches is pinned
    // against the plain-shape twin of each search.
    "regex_adversarial_lines_are_bounded_and_correct"_test = [] {
        micro_fixture fx;
        kimix::string line_a(3000, 'a');
        line_a += '!'; // "(a+)+$" cannot match: catastrophic shape, easy line
        fixture::put_bytes(fx.root / "adv_a.txt", line_a.data(), line_a.size());
        kimix::string line_x(2000, 'x');
        fixture::put_bytes(fx.root / "adv_x.txt", line_x.data(), line_x.size());
        kimix::string line_q(4000, 'z');
        fixture::put_bytes(fx.root / "adv_q.txt", line_q.data(), line_q.size());
        // Every adversarial pattern must report ZERO matching lines...
        expect(run_micro("(a+)+$", fx, "adv_a.txt").total_matches == 0);
        expect(run_micro("(x+x+)+y", fx, "adv_x.txt").total_matches == 0);
        expect(run_micro("q.*q.*q.*q", fx, "adv_q.txt").total_matches == 0);
        // ...and must still FIND the match when the line does contain one
        // (the NFA fallback is a decision procedure, not a negative filter).
        fixture::put_bytes(fx.root / "adv_hit.txt", line_a.data(), 2900);
        expect(run_micro("(a+)+$", fx, "adv_hit.txt").total_matches == 1);
    };
    "fuzz_prefilter_regex_agreement"_test = [] {
        pf_fixture fx("kimix_grep_engine_prefilter_fuzz", pf_fuzz_spec());
        fuzz_rng rng(0xA17C31B5ull ^ 0x9E3779B97F4A7C15ull);
        // Every suffix contains a metacharacter, so use_literal never applies.
        static const char *sufs[] = {".*", "?", "+", "*", "$", "^", ".", "[a-z]", "{2}",
                                     "|x", "(x)", "x?", "\\d", "x$", "[^q]"};
        static const char *prefs[] = {"", "^", "\\.", "[a-c]", "x", "|", "$", "\\d", "(?:"};
        const size_t nsuf = sizeof(sufs) / sizeof(sufs[0]);
        const size_t npref = sizeof(prefs) / sizeof(prefs[0]);
        int shapes = 0;
        for (int iter = 0; iter < 80; ++iter) {
            // Core: usually a real substring of a corpus line (hits exist),
            // sometimes a few random characters (mostly-miss probe).
            std::string core;
            const pf_file &f = fx.files[rng.below(static_cast<uint32_t>(fx.files.size()))];
            const std::vector<std::string> ls = fuzz_lines_of_text(f.text);
            if (!ls.empty() && rng.below(5) != 0) {
                const std::string &ln = ls[rng.below(static_cast<uint32_t>(ls.size()))];
                if (!ln.empty()) {
                    const size_t st = rng.below(static_cast<uint32_t>(ln.size()));
                    size_t len = 1 + static_cast<size_t>(rng.below(4));
                    if (st + len > ln.size()) {
                        len = ln.size() - st;
                    }
                    core = ln.substr(st, len);
                }
            }
            if (core.empty()) {
                const int n = 1 + static_cast<int>(rng.below(3));
                for (int i = 0; i < n; ++i) {
                    core += rng.pick("ab_cXY.-0 1");
                }
            }
            std::string pat = std::string(prefs[rng.below(static_cast<uint32_t>(npref))]) +
                              fuzz_literal_form(core);
            pat += sufs[rng.below(static_cast<uint32_t>(nsuf))];
            if (rng.below(3) == 0) { // a second core inside the shape
                pat += fuzz_literal_form(core.substr(0, 1));
            }
            ++shapes;
            const bool ic = rng.below(3) == 0;
            pf_expect_agreement(fx, pat, ic, "fuzz");
            pf_expect_agreement(fx, pat, !ic, "fuzz-anti");
        }
        expect(shapes == 80) << shapes;
    };

}
