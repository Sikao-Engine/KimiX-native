// test_grep_engine_matrix.cpp - Argument-combination matrix for the grep engine.
//
// Complement to test_grep_engine.cpp (which pins each mechanism in isolation):
// this suite treats kimix::builtin_tools::grep::run_grep as a black box and
// verifies the CARTESIAN PRODUCT of its arguments against an independent,
// in-file oracle that re-derives the documented semantics from scratch:
//
//   pattern   x {literal, case variant, dot/anchor/class regex, alternation
//                fast path + its refusal shapes, plus/star/counted
//                quantifiers, empty, class escapes, escaped punctuation,
//                literal-'\n', match-almost-everything shapes,
//                overlong-UTF-8 bait}
//   mode      x {files_with_matches, count_matches, content}
//   -i        x {off, on}
//   head_limit x {<= 0 unlimited, 1, 3, huge}             (fwm caps its lines)
//   -B/-A     x {(0,0), (1,0), (0,1), (2,2), (huge,huge)} (content mode,
//             plus the "context args are ignored outside content mode" corner)
//   include   x {"", "*.txt", "*", "?.md", "[bg]*", "*.TXT", "gamma.*",
//                "no*"}                                   (glob grammar corners)
//
// The oracle shares ONLY regex_lite (the matcher) and fnmatch_ascii (the glob)
// with the engine - the walk order, the hidden/binary/4 MiB pre-filters, the
// per-line splitting, the counting, the per-mode rendering, the head_limit
// cap and the message are all re-implemented here from the spec comments in
// grep_engine.h/.cpp, so a divergence is an engine bug, not a shared
// assumption.  The full matrix is then replayed three ways - serial (private
// 1-worker pool), parallel (4-worker ambient pool) and unbound (transient
// shared-pool bind) - and every replay must produce byte-identical summaries.
//
// Explicit corner pins beside the matrix:
//   * empty roots span / missing root  -> "0 match(es) in 0 file(s)"
//   * one missing + one good root: the good root still contributes
//   * a FILE root honours the include glob too
//   * duplicate roots are walked twice (results duplicated - pinned, not
//     deduplicated)
//   * relative roots resolve against work_dir (and "./" does too); with an
//     empty work_dir they resolve against the process CWD
//   * fwm head_limit at and around the matched-file count keeps files[]
//     complete and caps only the rendered lines
//   * huge -B/-A clamp at the file edges without separators
//   * a line spelling an ASCII letter overlong (\xC1\x81 == 'A') still
//     matches a pure-literal pattern through the engine fallback
//     (regression pin: the literal fast path used to answer it bytewise)
//   * invalid patterns report invalid_input with the validator's message
//   * line_match is parallel to lines in every mode, "--" flags are 0

#include "ut/ut.hpp"

#include "builtin_tools/grep_engine.h"
#include "builtin_tools/grep_tool.h" // fnmatch_ascii (shared with grep_tool.cpp)
#include "builtin_tools/regex_lite.h"

#include <core/fiber.h> // serial/parallel replay binds schedulers

#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;
namespace ge = kimix::builtin_tools::grep;
namespace bt = kimix::builtin_tools;
namespace rl = kimix::builtin_tools::regex_lite;

namespace {

// ---------------------------------------------------------------------------
// Corpus. 18 candidate files (16 searched + 1 binary-skipped + 1 > 4 MiB),
// deliberately over the >= 8 fan-out threshold so the parallel replay really
// fans out.
// ---------------------------------------------------------------------------
struct matrix_fixture {
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

    matrix_fixture() {
        namespace fs = kimix::filesystem;
        std::error_code ec;
        root = fs::temp_directory_path(ec) / "kimix_grep_engine_matrix";
        fs::remove_all(root, ec);
        fs::create_directories(root / "sub" / ".hidx", ec);

        put(root / "alpha.txt", "Hit one\nmiss\nhit two\nHIT three\n");
        put(root / "beta.txt", "nothing here\nfoo bar\nfoo baz\n");
        put(root / "gamma.md", "hit in md\nfoo here\n");
        put(root / "delta.c", "int hit;\nfoo();\nreturn 0;\n");
        put(root / "sub" / "eps.txt", "hit\nfoo\nmiss\nfoo\n");
        put(root / "sub" / "zeta.py", "FOO\nfoo\nfOo\nmiss\n");
        put(root / "ctx.txt", "a\nhit\nb\nc\nhit\nd\n");
        put(root / "adj.txt", "hit\nhit\nhit\n");
        put(root / "crlf.txt", "hit\r\nmiss\r\nhit\r\n");
        put(root / "notrail.txt", "miss\nhit"); // no trailing '\n'
        put(root / "blank.txt", "\n\nhit\n\n");
        put(root / "empty.txt", "");
        put(root / "uni.txt", "h\xC3\xA9llo hit\n\xC3\x84pfel FOO\n");
        // '\xC1\x81' is an OVERLONG spelling of 'A': the code-point engine
        // (regex_lite's lenient decoder) reads the line as "Apple ...", so
        // the byte-level literal fast path must agree via its engine
        // fallback instead of answering bytewise.
        put(root / "over.txt", "\xC1\x81pple hit\nplain\n");
        put(root / "special.txt", "a.b\na-b\naxb\n3.14\n");
        put_bytes(root / "binary.bin", "\0hit\nfoo\n", 9); // NUL < 64 KiB: skipped
        // NUL only AFTER the 64 KiB sniff window: searched; last line carries it.
        put_repeat(root / "late.bin", 'x', 64 * 1024, "\0hit\n", 5);
        // Stat size above the strict 4 MiB cap: skipped even though it has "hit".
        put_repeat(root / "big.txt", 'x', 4 * 1024 * 1024 + 1, "hit\n", 4);
        put(root / ".hidden.txt", "hit\n");               // hidden file: skipped
        put(root / "sub" / ".hidx" / "inner.txt", "hit\n"); // hidden dir: skipped
    }

    ~matrix_fixture() {
        std::error_code ec;
        kimix::filesystem::remove_all(root, ec);
    }
};

// ---------------------------------------------------------------------------
// Independent oracle.  Re-implements the walk, the pre-filters, the line
// splitting, the counting, the three renderings, the head_limit cap and the
// message from the grep_engine.h/.cpp spec comments.  Shares only the matcher
// (regex_lite) and the glob (fnmatch_ascii) with the engine.
// ---------------------------------------------------------------------------
struct o_file {
    kimix::string display;
    kimix::string bytes;
};

constexpr uint64_t k_oracle_max_file = 4ull * 1024 * 1024;
constexpr size_t k_oracle_sniff = 64 * 1024;

// Split [text, text+size) into line views: split at '\n', strip ONE trailing
// '\r' per line, emit a trailing partial line, no extra empty line after a
// buffer ending in '\n'.
kimix::vector<kimix::string_view> oracle_lines(kimix::string_view text) {
    kimix::vector<kimix::string_view> out;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t nl = text.find('\n', start);
        if (nl == kimix::string_view::npos) {
            if (start < text.size()) {
                size_t len = text.size() - start;
                if (text[text.size() - 1] == '\r') {
                    --len;
                }
                out.push_back(text.substr(start, len));
            }
            break;
        }
        size_t len = nl - start;
        if (len > 0 && text[nl - 1] == '\r') {
            --len;
        }
        out.push_back(text.substr(start, len));
        start = nl + 1u;
    }
    return out;
}

// Read + pre-filter one candidate file; returns false when the engine would
// silently skip it (stat failure or size above the cap, open/read error, NUL
// inside the sniff window).  `bytes` is untouched on the skip paths.
bool oracle_read(const kimix::filesystem::path &p, kimix::string &bytes) {
    namespace fs = kimix::filesystem;
    std::error_code ec;
    const uintmax_t sz = fs::file_size(p, ec);
    if (ec || sz > k_oracle_max_file) {
        return false; // a failed stat reads as uintmax(-1): over the cap
    }
    std::FILE *f = std::fopen(kimix::to_string(p).c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    bytes.clear();
    char buf[65536];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        bytes.append(buf, n);
    }
    std::fclose(f);
    const size_t sniff = bytes.size() < k_oracle_sniff ? bytes.size() : k_oracle_sniff;
    return std::memchr(bytes.data(), '\0', sniff) == nullptr;
}

void oracle_maybe_take(const kimix::filesystem::path &p, kimix::string_view include_glob,
                       kimix::vector<o_file> &out) {
    if (!include_glob.empty()) {
        const kimix::string name = kimix::to_string(p.filename());
        if (!ge::fnmatch_ascii(name, include_glob, false)) {
            return;
        }
    }
    o_file f;
    f.display = kimix::to_string(p);
    if (oracle_read(p, f.bytes)) {
        out.push_back(std::move(f));
    }
}

// Recursive sorted walk, hidden entries skipped at every depth, directories
// descended at their sorted position - the engine's canonical walk order.
void oracle_walk(const kimix::filesystem::path &dir, kimix::string_view include_glob,
                 kimix::vector<o_file> &out) {
    namespace fs = kimix::filesystem;
    std::error_code ec;
    kimix::vector<fs::path> entries;
    fs::directory_iterator it(dir, fs::directory_options::none, ec);
    if (ec) {
        return;
    }
    const fs::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            return;
        }
        entries.push_back(it->path());
    }
    std::sort(entries.begin(), entries.end());
    for (const fs::path &p : entries) {
        const kimix::string fname = kimix::to_string(p.filename());
        if (!fname.empty() && fname[0] == '.' && fname != "." && fname != "..") {
            continue;
        }
        std::error_code t_ec;
        if (fs::is_directory(p, t_ec)) {
            oracle_walk(p, include_glob, out);
            continue;
        }
        if (!fs::is_regular_file(p, t_ec)) {
            continue;
        }
        oracle_maybe_take(p, include_glob, out);
    }
}

// One root argument (file or directory), resolved like collect_files: an
// existing file is taken directly (include glob over its name); anything else
// that exists is walked; a missing root contributes nothing.
void oracle_root(const kimix::filesystem::path &root, kimix::string_view include_glob,
                 kimix::vector<o_file> &out) {
    namespace fs = kimix::filesystem;
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        return;
    }
    if (fs::is_regular_file(root, ec)) {
        oracle_maybe_take(root, include_glob, out);
        return;
    }
    oracle_walk(root, include_glob, out);
}

struct o_result {
    int64_t total = 0;
    kimix::vector<kimix::string> files; // display paths, walk order
    kimix::vector<int64_t> counts;      // parallel to files
    kimix::vector<kimix::string> lines; // rendered, per mode
    kimix::vector<uint8_t> flags;       // parallel to lines
};

o_result oracle_run(const kimix::vector<kimix::string> &roots, const ge::grep_options &opts) {
    kimix::vector<o_file> files;
    for (const kimix::string &r : roots) {
        kimix::filesystem::path p;
        if (!kimix::path_from_utf8(r, p)) {
            kimix::path_from_narrow(r, p);
        }
        oracle_root(p, opts.include_glob, files);
    }

    rl::Regex re;
    kimix::string err;
    // An empty pattern matches every line (regex_lite compiles it so).
    const bool ok = re.compile(opts.pattern, opts.ignore_case, err);
    expect(ok) << opts.pattern.c_str();

    o_result out;
    for (const o_file &f : files) {
        const kimix::vector<kimix::string_view> lines = oracle_lines(f.bytes);
        kimix::vector<int64_t> hits;
        for (size_t i = 0; i < lines.size(); ++i) {
            size_t b = 0, e = 0;
            if (re.search(lines[i], b, e)) {
                hits.push_back(static_cast<int64_t>(i));
            }
        }
        if (hits.empty()) {
            continue;
        }
        out.total += static_cast<int64_t>(hits.size());
        out.files.push_back(f.display);
        out.counts.push_back(static_cast<int64_t>(hits.size()));
        switch (opts.mode) {
        case ge::grep_output_mode::files_with_matches:
            // lines are the paths, capped at head_limit; files[] stays complete.
            if (opts.head_limit <= 0 ||
                static_cast<int64_t>(out.lines.size()) < opts.head_limit) {
                out.lines.push_back(f.display);
                out.flags.push_back(1);
            }
            break;
        case ge::grep_output_mode::count_matches:
            out.lines.push_back(kimix::format("{}:{}", f.display, hits.size()));
            out.flags.push_back(1);
            break;
        case ge::grep_output_mode::content: {
            int64_t last_emitted = -1000;
            for (const int64_t li : hits) {
                const int64_t lo =
                    std::max<int64_t>(0, li - static_cast<int64_t>(opts.ctx_before));
                const int64_t hi = std::min<int64_t>(static_cast<int64_t>(lines.size()) - 1,
                                                     li + static_cast<int64_t>(opts.ctx_after));
                if (lo > last_emitted + 1 && last_emitted > -999) {
                    out.lines.emplace_back("--");
                    out.flags.push_back(0);
                }
                for (int64_t l = lo; l <= hi; ++l) {
                    if (l <= last_emitted) {
                        continue;
                    }
                    const char sep = (l == li) ? ':' : '-';
                    out.lines.push_back(kimix::format(
                        "{}{}{}{}{}", f.display, sep, l + 1, sep,
                        lines[static_cast<size_t>(l)]));
                    out.flags.push_back(l == li ? 1 : 0);
                    last_emitted = l;
                }
                last_emitted = std::max(last_emitted, hi);
            }
            break;
        }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Summary + engine runner.  The whole result is flattened into one string so
// a matrix cell compares (and reports) wholesale.
// ---------------------------------------------------------------------------
kimix::string summarize(const char *tag, const ge::grep_result &r) {
    kimix::string s;
    s += kimix::format("{}|st={}|msg={}|tot={}|files=", tag,
                       r.status == bt::tool_status::ok ? "ok" : "?", r.message,
                       r.total_matches);
    for (const ge::grep_file_result &f : r.files) {
        s += kimix::format("[{}:{}]", f.path, f.match_count);
    }
    s += "|lines=";
    for (size_t i = 0; i < r.lines.size(); ++i) {
        s += kimix::format("{}{}`{}`", i == 0 ? "" : ",",
                           i < r.line_match.size() ? r.line_match[i] : 9, r.lines[i]);
    }
    return s;
}

// Converts an oracle answer into a grep_result-shaped pseudo record so both
// sides of the differential pass through the very same summarize().
ge::grep_result pseudo_result(const o_result &o) {
    ge::grep_result pseudo;
    pseudo.status = bt::tool_status::ok;
    pseudo.message = kimix::format("{} match(es) in {} file(s)", o.total, o.files.size());
    pseudo.total_matches = o.total;
    for (size_t i = 0; i < o.files.size(); ++i) {
        pseudo.files.push_back(ge::grep_file_result{o.files[i], o.counts[i]});
    }
    pseudo.lines = o.lines;
    pseudo.line_match = o.flags;
    return pseudo;
}

ge::grep_result engine_run(const kimix::vector<kimix::string> &roots,
                           const kimix::string_view work_dir, const ge::grep_options &opts) {
    ge::grep_result out;
    ge::run_grep(opts, roots, work_dir, out);
    return out;
}

// ---------------------------------------------------------------------------
// The matrix itself.  `run` produces the grep_result for one options combo;
// one summary line per cell is appended to `log`.  Returns the cell count.
// ---------------------------------------------------------------------------
const char *mode_name(ge::grep_output_mode m) {
    switch (m) {
    case ge::grep_output_mode::files_with_matches:
        return "fwm";
    case ge::grep_output_mode::count_matches:
        return "cnt";
    case ge::grep_output_mode::content:
        return "ctx";
    }
    return "?";
}

template <typename Run>
size_t run_matrix(const kimix::vector<kimix::string> &roots, const Run &run, kimix::string &log) {
    // Patterns across every plan the engine can pick: pure literal (incl. the
    // overlong bait and escaped punctuation), regex with prefilter shapes
    // (anchors/dot/plus/star/counted), class shapes, alternation fast path +
    // its refusal shapes, empty, literal-'\n', and match-almost-everything
    // shapes.  Invalid shapes are checked separately (the oracle cannot
    // compile them).
    const char *k_patterns[] = {
        "hit", "HIT", "h.t", "^hit$", "^hit", "hit$", "^", "$", "^$", ".*",
        "hit|foo", "HIT|FOO", "hit|foo|miss", "h|f",
        "fo+", "fo*", "f.o", "f.*o", "fo{2}",
        "", "\\d", "\\w+", "[Hh]it", "[0-9]", "[^x]",
        "App", "app", "a\\.b", "a-b", "hit\\nfoo",
    };
    const bool k_ic[] = {false, true};
    const int64_t k_heads[] = {0, 1, 3, 1000000};

    size_t cells = 0;
    for (const char *pat : k_patterns) {
        for (const bool ic : k_ic) {
            // fwm x head_limit (the only mode the engine caps).
            for (const int64_t hl : k_heads) {
                ge::grep_options o;
                o.pattern = kimix::string(pat);
                o.ignore_case = ic;
                o.mode = ge::grep_output_mode::files_with_matches;
                o.head_limit = hl;
                const kimix::string tag = kimix::format("{}|fwm|ic={}|hl={}|", pat, ic, hl);
                log += summarize(tag.c_str(), run(o));
                log += '\n';
                ++cells;
            }
            // count + content at default head_limit.
            for (const ge::grep_output_mode m :
                 {ge::grep_output_mode::count_matches, ge::grep_output_mode::content}) {
                ge::grep_options o;
                o.pattern = kimix::string(pat);
                o.ignore_case = ic;
                o.mode = m;
                const kimix::string tag = kimix::format("{}|{}|ic={}|", pat, mode_name(m), ic);
                log += summarize(tag.c_str(), run(o));
                log += '\n';
                ++cells;
            }
        }
    }
    // Context grid: content mode x -B/-A corners, plus the same ctx applied
    // to count_matches (where the engine must ignore it).
    const char *k_ctx_pats[] = {"hit", "hit|foo", "fo+", "^hit$", ""};
    struct {
        uint32_t b, a;
    } const k_ctx[] = {{1, 0}, {0, 1}, {2, 2}, {1000, 1000}};
    for (const char *pat : k_ctx_pats) {
        for (const bool ic : k_ic) {
            for (const auto c : k_ctx) {
                for (const ge::grep_output_mode m :
                     {ge::grep_output_mode::content, ge::grep_output_mode::count_matches}) {
                    ge::grep_options o;
                    o.pattern = kimix::string(pat);
                    o.ignore_case = ic;
                    o.mode = m;
                    o.ctx_before = c.b;
                    o.ctx_after = c.a;
                    const kimix::string tag = kimix::format("{}|{}|ic={}|b={}|a={}|", pat,
                                                            mode_name(m), ic, c.b, c.a);
                    log += summarize(tag.c_str(), run(o));
                    log += '\n';
                    ++cells;
                }
            }
        }
    }
    // Include-glob grid: glob grammar corners x a small pattern x mode x -i.
    const char *k_globs[] = {"", "*.txt", "*", "?.md", "[bg]*", "*.TXT", "gamma.*", "no*"};
    const char *k_glob_pats[] = {"hit", "foo", "hit|foo"};
    for (const char *glob : k_globs) {
        for (const char *pat : k_glob_pats) {
            for (const bool ic : k_ic) {
                for (const ge::grep_output_mode m : {ge::grep_output_mode::files_with_matches,
                                                     ge::grep_output_mode::count_matches,
                                                     ge::grep_output_mode::content}) {
                    ge::grep_options o;
                    o.pattern = kimix::string(pat);
                    o.ignore_case = ic;
                    o.mode = m;
                    o.include_glob = kimix::string(glob);
                    const kimix::string tag =
                        kimix::format("glob={}|{}|{}|ic={}|", glob, pat, mode_name(m), ic);
                    log += summarize(tag.c_str(), run(o));
                    log += '\n';
                    ++cells;
                }
            }
        }
    }
    return cells;
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    // ------------------------------------------------------ matrix vs oracle
    // Serial replay (private 1-worker pool) on the engine side, oracle answers
    // packed into the same shape; the two logs must be byte-identical.
    "arg_matrix_matches_oracle"_test = [] {
        matrix_fixture fx;
        const kimix::vector<kimix::string> roots{kimix::to_string(fx.root)};
        const kimix::string work_dir;

        kimix::string engine_log;
        size_t cells = 0;
        {
            kimix::fiber::scheduler one{1u};
            cells = run_matrix(roots, [&](const ge::grep_options &o) {
                return engine_run(roots, kimix::string_view(work_dir), o);
            }, engine_log);
        }
        expect(cells > 300) << cells; // guard against a silently empty matrix

        kimix::string oracle_log;
        run_matrix(roots,
                   [&](const ge::grep_options &o) {
                       return pseudo_result(oracle_run(roots, o));
                   },
                   oracle_log);
        expect(oracle_log == engine_log) << "\n--- engine ---\n"
                                         << engine_log.c_str() << "\n--- oracle ---\n"
                                         << oracle_log.c_str();
    };

    // ------------------------------------------- parallel replays vs serial
    "arg_matrix_parallel_matches_serial"_test = [] {
        matrix_fixture fx;
        const kimix::vector<kimix::string> roots{kimix::to_string(fx.root)};
        const kimix::string work_dir;

        kimix::string serial_log;
        {
            kimix::fiber::scheduler one{1u};
            run_matrix(roots, [&](const ge::grep_options &o) {
                return engine_run(roots, kimix::string_view(work_dir), o);
            }, serial_log);
        }

        kimix::string par_log;
        {
            kimix::fiber::scheduler pool{4u};
            run_matrix(roots, [&](const ge::grep_options &o) {
                return engine_run(roots, kimix::string_view(work_dir), o);
            }, par_log);
        }
        expect(par_log == serial_log) << "4-worker ambient pool diverged";

        // Unbound caller: the engine transiently binds the shared pool and
        // must land on the same bytes, then unbind again.
        expect(!kimix::fiber::is_bound());
        kimix::string shared_log;
        run_matrix(roots, [&](const ge::grep_options &o) {
            return engine_run(roots, kimix::string_view(work_dir), o);
        }, shared_log);
        expect(!kimix::fiber::is_bound());
        expect(shared_log == serial_log) << "unbound shared-pool run diverged";
    };

    // ------------------------------------------------- argument corner pins
    "empty_and_missing_roots"_test = [] {
        matrix_fixture fx;
        ge::grep_options o;
        o.pattern = kimix::string("hit");
        // Empty roots span.
        {
            const kimix::vector<kimix::string> roots;
            const ge::grep_result r = engine_run(roots, {}, o);
            expect(r.status == bt::tool_status::ok);
            expect(r.total_matches == 0);
            expect(r.files.empty());
            expect(r.lines.empty());
            expect(r.message == "0 match(es) in 0 file(s)");
        }
        // Missing root (silently walked as empty).
        {
            const kimix::vector<kimix::string> roots{
                kimix::to_string(fx.root / "does_not_exist")};
            const ge::grep_result r = engine_run(roots, {}, o);
            expect(r.status == bt::tool_status::ok);
            expect(r.total_matches == 0);
            expect(r.message == "0 match(es) in 0 file(s)");
        }
        // One missing + one good root: the good root still contributes.
        // (alpha.txt carries ONE lowercase "hit" - the other two hits are
        // case variants.)
        {
            kimix::vector<kimix::string> roots;
            roots.push_back(kimix::to_string(fx.root / "nope"));
            roots.push_back(kimix::to_string(fx.root / "alpha.txt"));
            const ge::grep_result r = engine_run(roots, {}, o);
            expect(r.total_matches == 1) << r.message.c_str();
            expect(r.files.size() == 1);
        }
    };

    "file_root_and_include_glob_corner"_test = [] {
        matrix_fixture fx;
        // A FILE root is searched... (alpha.txt: one lowercase "hit")
        {
            ge::grep_options o;
            o.pattern = kimix::string("hit");
            const kimix::vector<kimix::string> roots{
                kimix::to_string(fx.root / "alpha.txt")};
            const ge::grep_result r = engine_run(roots, {}, o);
            expect(r.total_matches == 1) << r.message.c_str();
            expect(r.files.size() == 1);
        }
        // ... and the include glob applies to it too: a mismatched glob
        // silently skips even a direct file root.
        {
            ge::grep_options o;
            o.pattern = kimix::string("hit");
            o.include_glob = kimix::string("*.md");
            const kimix::vector<kimix::string> roots{
                kimix::to_string(fx.root / "alpha.txt")};
            const ge::grep_result r = engine_run(roots, {}, o);
            expect(r.total_matches == 0);
            expect(r.message == "0 match(es) in 0 file(s)");
        }
        // A matching glob keeps it.
        {
            ge::grep_options o;
            o.pattern = kimix::string("hit");
            o.include_glob = kimix::string("a*.txt");
            const kimix::vector<kimix::string> roots{
                kimix::to_string(fx.root / "alpha.txt")};
            const ge::grep_result r = engine_run(roots, {}, o);
            expect(r.total_matches == 1) << r.message.c_str();
        }
    };

    "duplicate_roots_are_walked_twice"_test = [] {
        matrix_fixture fx;
        ge::grep_options o;
        o.pattern = kimix::string("hit");
        o.mode = ge::grep_output_mode::count_matches;
        kimix::vector<kimix::string> roots;
        roots.push_back(kimix::to_string(fx.root / "alpha.txt"));
        roots.push_back(kimix::to_string(fx.root / "alpha.txt"));
        const ge::grep_result r = engine_run(roots, {}, o);
        // No dedup at the engine level: the file is searched once per root
        // and appears once per root in the result (pinned behaviour).
        expect(r.files.size() == 2) << r.message.c_str();
        expect(r.total_matches == 2) << r.message.c_str();
        expect(r.lines.size() == 2);
        expect(r.lines[0] == r.lines[1]);
    };

    "relative_root_resolves_against_work_dir"_test = [] {
        matrix_fixture fx;
        const kimix::string work_dir = kimix::to_string(fx.root);
        // Relative file root.
        {
            ge::grep_options o;
            o.pattern = kimix::string("hit");
            const kimix::vector<kimix::string> roots{kimix::string("alpha.txt")};
            const ge::grep_result r = engine_run(roots, work_dir, o);
            expect(r.total_matches == 1) << r.message.c_str();
            // Display paths are the RESOLVED walk paths, not the raw root.
            expect(r.files[0].path.find(work_dir) == 0) << r.files[0].path.c_str();
        }
        // Relative directory root ("." == the work dir itself).
        {
            ge::grep_options o;
            o.pattern = kimix::string("hit");
            o.include_glob = kimix::string("gamma.*");
            const kimix::vector<kimix::string> roots{kimix::string("./")};
            const ge::grep_result r = engine_run(roots, work_dir, o);
            expect(r.total_matches == 1) << r.message.c_str();
            expect(r.files.size() == 1);
            expect(r.files[0].path.find("gamma.md") != kimix::string::npos)
                << r.files[0].path.c_str();
        }
        // A relative root with an EMPTY work_dir resolves against the process
        // CWD - "alpha.txt" is not there: no crash, no match.
        {
            ge::grep_options o;
            o.pattern = kimix::string("hit");
            const kimix::vector<kimix::string> roots{kimix::string("alpha.txt")};
            const ge::grep_result r = engine_run(roots, {}, o);
            expect(r.total_matches == 0) << r.message.c_str();
        }
    };

    "head_limit_exact_boundary_fwm"_test = [] {
        matrix_fixture fx;
        // Pattern "hit" (case-sensitive) matches 16 lines across 12 files in
        // the corpus (alpha/gamma/delta/eps/ctx(x2)/adj(x3)/crlf(x2)/
        // notrail/over/uni/late.bin); the fwm line cap must keep exactly
        // head_limit rendered lines while files[] always stays complete.
        for (const int64_t hl : {1, 11, 12, 13}) {
            ge::grep_options o;
            o.pattern = kimix::string("hit");
            o.head_limit = hl;
            const kimix::vector<kimix::string> roots{kimix::to_string(fx.root)};
            const ge::grep_result r = engine_run(roots, {}, o);
            expect(r.files.size() == 12) << r.message.c_str();
            const size_t want = hl < 12 ? static_cast<size_t>(hl) : size_t(12);
            expect(r.lines.size() == want) << hl;
            expect(r.line_match.size() == want) << hl;
            for (const uint8_t f : r.line_match) {
                expect(f == 1);
            }
        }
    };

    "context_clamps_at_file_edges"_test = [] {
        matrix_fixture fx;
        ge::grep_options o;
        o.pattern = kimix::string("hit");
        o.mode = ge::grep_output_mode::content;
        o.ctx_before = 1000000u; // absurd values clamp, never overflow
        o.ctx_after = 1000000u;
        const kimix::vector<kimix::string> roots{kimix::to_string(fx.root / "adj.txt")};
        const ge::grep_result r = engine_run(roots, {}, o);
        // Whole file rendered exactly once: 3 lines, the first is the hit
        // (':' delimiters, flag 1), the other two are its context ('-'
        // delimiters, flag 0), and no "--" separator appears - the later
        // hits fall inside the already-emitted run.
        expect(r.total_matches == 3) << r.message.c_str();
        expect(r.lines.size() == 3) << r.message.c_str();
        expect(r.lines[0].find(":1:hit") != kimix::string::npos) << r.lines[0].c_str();
        expect(r.lines[1].find("-2-hit") != kimix::string::npos) << r.lines[1].c_str();
        expect(r.lines[2].find("-3-hit") != kimix::string::npos) << r.lines[2].c_str();
        expect(r.line_match.size() == 3);
        expect(r.line_match[0] == 1 && r.line_match[1] == 0 && r.line_match[2] == 0);
    };

    "overlong_utf8_still_matches_literal_pattern"_test = [] {
        matrix_fixture fx;
        // Regression pin: '\xC1\x81' decodes to 'A' under regex_lite's lenient
        // decoder, so "App"/"app" MUST match the over.txt line; the pure-
        // literal fast path may only say "no" bytewise when the line cannot
        // hide an ASCII code point overlong - which this line can.
        for (const char *pat : {"App", "app", "APP"}) {
            for (const bool ic : {false, true}) {
                ge::grep_options o;
                o.pattern = kimix::string(pat);
                o.ignore_case = ic;
                o.mode = ge::grep_output_mode::content;
                const kimix::vector<kimix::string> roots{kimix::to_string(fx.root / "over.txt")};
                const ge::grep_result r = engine_run(roots, {}, o);
                const bool should = ic || std::strcmp(pat, "App") == 0;
                expect((r.total_matches == 1) == should)
                    << pat << " ic=" << ic << " msg=" << r.message.c_str();
            }
        }
        // Same line, alternation fast path (which always had the escape
        // hatch): "App|zzz" must also match.
        {
            ge::grep_options o;
            o.pattern = kimix::string("App|zzz");
            const kimix::vector<kimix::string> roots{kimix::to_string(fx.root / "over.txt")};
            const ge::grep_result r = engine_run(roots, {}, o);
            expect(r.total_matches == 1) << r.message.c_str();
        }
    };

    "invalid_patterns_report_invalid_input"_test = [] {
        matrix_fixture fx;
        const kimix::vector<kimix::string> roots{kimix::to_string(fx.root / "alpha.txt")};
        for (const char *pat : {"h(it", "\\", "a{2,1}"}) {
            ge::grep_options o;
            o.pattern = kimix::string(pat);
            ge::grep_result r;
            ge::run_grep(o, roots, {}, r);
            expect(r.status == bt::tool_status::invalid_input) << pat;
            expect(r.message.find("invalid pattern: ") == 0) << r.message.c_str();
            expect(r.files.empty());
            expect(r.total_matches == 0);
        }
    };

    "line_match_parallel_array_invariants"_test = [] {
        matrix_fixture fx;
        const kimix::vector<kimix::string> roots{kimix::to_string(fx.root)};
        struct {
            const char *pat;
            ge::grep_output_mode mode;
            uint32_t b, a;
        } const cases[] = {
            {"hit", ge::grep_output_mode::files_with_matches, 0, 0},
            {"hit", ge::grep_output_mode::count_matches, 0, 0},
            {"hit", ge::grep_output_mode::content, 0, 0},
            {"hit", ge::grep_output_mode::content, 1, 1},
            {"miss", ge::grep_output_mode::content, 2, 2},
        };
        for (const auto &c : cases) {
            ge::grep_options o;
            o.pattern = kimix::string(c.pat);
            o.mode = c.mode;
            o.ctx_before = c.b;
            o.ctx_after = c.a;
            const ge::grep_result r = engine_run(roots, {}, o);
            expect(r.line_match.size() == r.lines.size()) << c.pat;
            if (c.mode == ge::grep_output_mode::content) {
                // Every "--" separator is flagged 0; matches flagged 1.
                for (size_t i = 0; i < r.lines.size(); ++i) {
                    if (r.lines[i] == "--") {
                        expect(r.line_match[i] == 0) << i;
                    }
                }
                // Without context the flagged-1 count equals total_matches;
                // with context it is the number of rendered hit lines.
                if (c.b == 0 && c.a == 0) {
                    int64_t flagged = 0;
                    for (const uint8_t f : r.line_match) {
                        flagged += f;
                    }
                    expect(flagged == r.total_matches) << c.pat;
                }
            } else {
                for (const uint8_t f : r.line_match) {
                    expect(f == 1);
                }
            }
        }
    };
}
