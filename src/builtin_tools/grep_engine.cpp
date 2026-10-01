// grep_engine.cpp - Implementation of the native grep search engine.
// See grep_engine.h for the design notes (ripgrep-inspired: whole-buffer
// zero-copy line scan, literal fast path, per-chunk regexes over static
// chunks, 64 KiB NUL binary sniff). Semantics mirror the previous inline
// native_io branch of Grep::operator() byte-for-byte where observable.
//
// Threading model: the engine NEVER creates a scheduler. It fans out over
// whatever kimix::fiber pool the CALLING thread is bound to (the host binds
// one at process/thread start - see cli_main() and the background sub-agent
// worker in agent_tool.cpp); an unbound caller scans inline through the same
// chunk logic, so results are identical, just serial.
//
// Compiled into the kimix-llm static library with a unity (jumbo) batch, so
// all file-local helpers live inside an anonymous namespace and carry
// grep-engine-specific names (no file-scope same-named globals).

#include "builtin_tools/grep_engine.h"

#include "builtin_tools/grep_tool.h" // fnmatch_ascii (shared with grep_tool.cpp)
#include "builtin_tools/regex_lite.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <utility>

#include <core/fiber.h> // the file chunks are fanned out over kimix::fiber

namespace kimix::builtin_tools::grep {

namespace {

namespace ge {

constexpr uint64_t k_max_file_bytes = 4ull * 1024 * 1024;
constexpr size_t k_binary_sniff_bytes = 64 * 1024;

// One line of the file buffer: a view (start offset + length with one
// trailing '\r' already stripped). Zero-copy: no per-line string allocs.
struct line_view {
    size_t start = 0;
    size_t len = 0;
};

// One searchable file collected by the walk: the display path (the exact
// kimix::to_string(entry) spelling the results report) plus the native
// fs::path it came from, so scan_file never re-decodes the UTF-8 string and
// never re-checks the file type (collect_files already established it is a
// regular file).
struct walk_entry {
    kimix::string display;
    kimix::filesystem::path native;
};

// Per-chunk search output, merged in chunk index order so the final result
// is deterministic and ordered by walk order. No mutexes: each chunk owns a
// disjoint index range of the file list.
struct chunk_output {
    kimix::vector<grep_file_result> files;
    kimix::vector<kimix::string> lines;
    kimix::vector<uint8_t> line_match; // parallel to lines (content mode)
    int64_t total_matches = 0;
};

char lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

bool is_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// -- literal fast path ------------------------------------------------------

// True when `pat` is a pure literal: no unescaped metacharacters among
// .^$*+?()[]{}|. Escapes: \d \D \w \W \s \S \b \B and hex/unicode escapes are
// NOT literal; \n \t \r \f \v ARE literal control chars; escaped punctuation
// is the literal char. Unknown alphabetic escapes and digit escapes (\0 is
// the NUL character, \1-\9 are back-references) are NOT literal either: only
// the regex engine assigns them meaning, so they go to the regex path and the
// validator decides - the fast path must never re-interpret them as plain
// digits (that would diverge from the inline-regex semantics run_grep
// mirrors). Unescapes into `lit` (also set on the false paths that consume a
// prefix - callers ignore it then).
bool extract_literal(kimix::string_view pat, kimix::string &lit) noexcept {
    lit.clear();
    for (size_t i = 0; i < pat.size(); ++i) {
        const char c = pat[i];
        switch (c) {
        case '.':
        case '^':
        case '$':
        case '*':
        case '+':
        case '?':
        case '(':
        case ')':
        case '[':
        case ']':
        case '{':
        case '}':
        case '|':
            return false;
        case '\\':
            if (++i >= pat.size()) {
                return false; // dangling backslash: let the regex validator reject it
            }
            switch (pat[i]) {
            case 'd':
            case 'D':
            case 'w':
            case 'W':
            case 's':
            case 'S':
            case 'b':
            case 'B': // classes / anchors: regex path
            case 'x':
            case 'u':
            case 'U': // hex / unicode escapes: regex path
                return false;
            case 'n':
                lit.push_back('\n');
                break;
            case 't':
                lit.push_back('\t');
                break;
            case 'r':
                lit.push_back('\r');
                break;
            case 'f':
                lit.push_back('\f');
                break;
            case 'v':
                lit.push_back('\v');
                break;
            default:
                if (is_alpha(pat[i]) || (pat[i] >= '0' && pat[i] <= '9')) {
                    return false; // unknown alphabetic/digit escape: regex decides
                }
                lit.push_back(pat[i]); // escaped punctuation is the literal char
                break;
            }
            break;
        default:
            lit.push_back(c);
            break;
        }
    }
    return true;
}

// Direct substring search for the literal fast path: memchr on the first
// byte + memcmp; ASCII folding byte compare under ignore_case (matches the
// regex_lite A-Za-z fold).
bool literal_in_line(kimix::string_view hay, kimix::string_view needle,
                     bool fold_case) noexcept {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > hay.size()) {
        return false;
    }
    const char first = needle[0];
    const size_t last = hay.size() - needle.size();
    // memchr on the first byte only works when the fold of the first byte is
    // the byte itself; a folded letter ('H' vs 'h') needs a folded scan.
    const bool scan_folded = fold_case && ((first >= 'a' && first <= 'z') ||
                                           (first >= 'A' && first <= 'Z'));
    size_t pos = 0;
    while (pos <= last) {
        size_t off = 0;
        if (scan_folded) {
            const char want = lower_ascii(first);
            off = pos;
            while (off <= last && lower_ascii(hay[off]) != want) {
                ++off;
            }
            if (off > last) {
                return false;
            }
        } else {
            const void *hit = std::memchr(hay.data() + pos, first,
                                          static_cast<size_t>(last - pos) + 1u);
            if (hit == nullptr) {
                return false;
            }
            off = static_cast<size_t>(static_cast<const char *>(hit) - hay.data());
        }
        bool eq = true;
        if (!fold_case) {
            // Bulk compare the tail: memcmp is SIMD-optimized, the per-byte
            // loop only matters when the fold is active.
            eq = needle.size() == 1u ||
                 std::memcmp(hay.data() + off + 1u, needle.data() + 1u,
                             needle.size() - 1u) == 0;
        } else {
            for (size_t k = 1; k < needle.size(); ++k) {
                char a = lower_ascii(hay[off + k]);
                const char b = lower_ascii(needle[k]);
                if (a != b) {
                    eq = false;
                    break;
                }
            }
        }
        if (eq) {
            return true;
        }
        pos = off + 1u;
    }
    return false;
}

// -- zero-copy line iteration ------------------------------------------------

// Stream every line of `text` through `fn(line_view)` without materializing
// the views: split at '\n', strip one trailing '\r' per line, emit a trailing
// partial line (no closing '\n'), and no extra empty line after a buffer that
// ends in '\n' - the exact enumeration ge::collect_lines produces.
template <typename Fn>
void for_each_line(const kimix::string &text, Fn &&fn) {
    const size_t size = text.size();
    size_t start = 0;
    while (start <= size) {
        const char *nl_hit = static_cast<const char *>(
            std::memchr(text.data() + start, '\n', size - start));
        if (nl_hit == nullptr) {
            if (start < size) {
                size_t len = size - start;
                if (text[size - 1] == '\r') {
                    --len;
                }
                fn(line_view{start, len});
            }
            break;
        }
        const size_t nl = static_cast<size_t>(nl_hit - text.data());
        size_t len = nl - start;
        if (len > 0 && text[nl - 1] == '\r') {
            --len;
        }
        fn(line_view{start, len});
        start = nl + 1u;
    }
}

// Split `text` into line views (same rules as for_each_line; used by the
// content mode, where context rendering needs random access to the lines).
void collect_lines(const kimix::string &text, kimix::vector<line_view> &out) {
    out.clear();
    for_each_line(text, [&](const line_view &lv) noexcept { out.push_back(lv); });
}

// -- per-file scan -----------------------------------------------------------

// Render the content-mode lines for one matched file: for each hit line the
// context run [li-B, li+A] clamped to the file bounds, overlapping runs
// merged, "--" between non-adjacent runs; match line "path:LN:text", context
// line "path-LN:text" (LN is 1-based). Same last_emitted logic as the old
// inline branch.
void render_content(const grep_options &opts, kimix::string_view path,
                    const kimix::string &text, const kimix::vector<line_view> &lines,
                    const kimix::vector<int64_t> &hit_lines, chunk_output &out) {
    const int64_t line_count = static_cast<int64_t>(lines.size());
    int64_t last_emitted = -1000;
    for (const int64_t li : hit_lines) {
        const int64_t lo = std::max<int64_t>(0, li - static_cast<int64_t>(opts.ctx_before));
        const int64_t hi = std::min<int64_t>(line_count - 1,
                                             li + static_cast<int64_t>(opts.ctx_after));
        if (lo > last_emitted + 1 && last_emitted > -999) {
            out.lines.push_back("--");
            out.line_match.push_back(0);
        }
        for (int64_t l = lo; l <= hi; ++l) {
            if (l <= last_emitted) {
                continue;
            }
            const line_view &lv = lines[static_cast<size_t>(l)];
            const kimix::string_view text_view(text.data() + lv.start, lv.len);
            const char sep = (l == li) ? ':' : '-';
            out.lines.push_back(
                kimix::format("{}{}{}{}{}", path, sep, l + 1, sep, text_view));
            out.line_match.push_back(l == li ? 1 : 0);
            last_emitted = l;
        }
        last_emitted = std::max(last_emitted, hi);
    }
}

// Search one already-collected file. Silently skipped on: stat size (or the
// growing read buffer) > 4 MiB, open/read errors, or a NUL in the first
// 64 KiB of the buffer (rg binary convention). The include-glob and the
// regular-file checks already ran during the walk (collect_files), so this
// only sees candidate regular files whose name matches.
void scan_file(const walk_entry &entry, const grep_options &opts,
               const regex_lite::Regex *re, kimix::string_view literal, bool use_literal,
               chunk_output &out) {
    const kimix::string &path = entry.display;
    const kimix::filesystem::path &file = entry.native;
    std::error_code ec;
    // A failed stat returns (uintmax)-1, which exceeds the cap: the file is
    // skipped, exactly like the previous scan-side is_regular/file_size pair.
    const uintmax_t size_hint = kimix::filesystem::file_size(file, ec);
    if (size_hint > k_max_file_bytes) {
        return;
    }
    // fopen takes the DISPLAY string directly: the old chain was
    // fopen(to_string(path_from_utf8/narrow(display))) and kimix::to_string
    // round-trips its own output (ACP-first, UTF-8 fallback - see
    // stl/filesystem.cpp), so the byte string handed to fopen is identical
    // without the per-file path re-decode + re-encode.
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return;
    }
    // Whole-buffer read with one reserve from the stat hint (the append loop
    // below no longer grows the string geometrically for multi-KiB files).
    kimix::string text;
    if (size_hint <= k_max_file_bytes) {
        text.reserve(static_cast<size_t>(size_hint));
    }
    char buf[65536];
    // Binary sniff on the FIRST 64 KiB of the read buffer (the rg convention
    // the old whole-file read implemented): collect at least 64 KiB (or EOF),
    // sniff, and only then tail-read the rest. Identical sniff set, but a
    // binary blob is skipped after one 64 KiB read instead of up to 4 MiB.
    bool sniffed = false;
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        text.append(buf, n);
        if (text.size() > k_max_file_bytes) {
            std::fclose(f);
            return;
        }
        if (!sniffed && text.size() >= k_binary_sniff_bytes) {
            if (std::memchr(text.data(), '\0', k_binary_sniff_bytes) != nullptr) {
                std::fclose(f);
                return;
            }
            sniffed = true;
        }
    }
    std::fclose(f);
    if (!sniffed) {
        // The file was shorter than 64 KiB: sniff the whole buffer. A NUL past
        // 64 KiB does NOT skip (same rule as before).
        if (std::memchr(text.data(), '\0', text.size()) != nullptr) {
            return;
        }
    }

    if (opts.mode == grep_output_mode::content) {
        // Content mode needs random line access for the -B/-A context runs.
        kimix::vector<line_view> lines;
        collect_lines(text, lines);
        kimix::vector<int64_t> hit_lines;
        for (size_t li = 0; li < lines.size(); ++li) {
            const line_view &lv = lines[li];
            const kimix::string_view line(text.data() + lv.start, lv.len);
            bool matched = false;
            if (use_literal) {
                matched = literal_in_line(line, literal, opts.ignore_case);
            } else {
                size_t mb = 0;
                size_t me = 0;
                matched = re->search(line, mb, me);
            }
            if (matched) {
                hit_lines.push_back(static_cast<int64_t>(li));
            }
        }
        if (hit_lines.empty()) {
            return;
        }
        const int64_t file_matches = static_cast<int64_t>(hit_lines.size());
        out.total_matches += file_matches;
        out.files.push_back(grep_file_result{path, file_matches});
        render_content(opts, path, text, lines, hit_lines, out);
        return;
    }
    // files_with_matches / count_matches: stream the lines, never materialize
    // the views (the per-line scan result is identical).
    int64_t file_matches = 0;
    for_each_line(text, [&](const line_view &lv) noexcept {
        const kimix::string_view line(text.data() + lv.start, lv.len);
        bool matched = false;
        if (use_literal) {
            matched = literal_in_line(line, literal, opts.ignore_case);
        } else {
            size_t mb = 0;
            size_t me = 0;
            matched = re->search(line, mb, me);
        }
        if (matched) {
            ++file_matches;
        }
    });
    if (file_matches == 0) {
        return;
    }
    out.total_matches += file_matches;
    out.files.push_back(grep_file_result{path, file_matches});
    if (opts.mode == grep_output_mode::count_matches) {
        out.lines.push_back(kimix::format("{}:{}", path, file_matches));
        out.line_match.push_back(1);
    }
    // files_with_matches renders nothing here: the merge step derives the
    // (head_limit-capped) lines from the complete files[] array.
}

// -- single-threaded walk ----------------------------------------------------

// Append every non-hidden REGULAR file entry under `root_str` to `out`, in
// walk order, already filtered through the include-glob when one is set.
// Relative roots resolve against `work_dir`. Hidden entries (name starting
// with '.') are skipped at every depth and hidden directories are not
// descended. Directories and other non-regular entries never enter the list
// (the old scan-side is_regular_file check skipped them after a full path
// re-decode and two stats; the directory_iterator's cached entry type answers
// the same question without the extra syscalls, following symlinks like
// std::filesystem::is_regular_file did). Only error_code overloads are used;
// any iterator error ends the walk for that root.
void collect_files(const kimix::string &root_str, kimix::string_view work_dir,
                   const kimix::string &include_glob, kimix::vector<walk_entry> &out) {
    namespace fs = kimix::filesystem;
    // No narrow path constructor: it converts through the ANSI code page and
    // THROWS std::system_error on bytes it cannot represent (fatal with C++
    // exceptions disabled). Tool arguments arrive UTF-8, work_dir follows the
    // CLI's ANSI/lossy convention; a conversion failure reports "missing".
    fs::path rp;
    if (!kimix::path_from_utf8(root_str, rp)) {
        if (!kimix::path_from_narrow(root_str, rp)) {
            return;
        }
    }
    if (rp.is_relative() && !work_dir.empty()) {
        fs::path wd;
        if (kimix::path_from_narrow(work_dir, wd) ||
            kimix::path_from_utf8(work_dir, wd)) {
            rp = wd / rp;
        }
    }
    std::error_code ec;
    if (!fs::exists(rp, ec)) {
        return;
    }
    if (fs::is_regular_file(rp, ec)) {
        // The include-glob selects by file name for file roots too (the old
        // scan-side check silently skipped a mismatched direct root).
        if (!include_glob.empty()) {
            const kimix::string name = kimix::to_string(rp.filename());
            if (!fnmatch_ascii(name, include_glob, false)) {
                return;
            }
        }
        walk_entry entry;
        entry.display = kimix::to_string(rp);
        entry.native = std::move(rp);
        out.push_back(std::move(entry));
        return;
    }
    fs::recursive_directory_iterator it(rp, fs::directory_options::none, ec);
    if (ec) {
        return;
    }
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        const fs::path &entry_path = it->path();
        // Skip hidden dirs (.git etc.) at every depth of the walk.
        const kimix::string fname = kimix::to_string(entry_path.filename());
        if (!fname.empty() && fname[0] == '.' && fname != "." && fname != "..") {
            if (it->is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        // Only regular files are searchable; the cached entry status answers
        // this for free on Windows (directory enumeration already returned
        // the attributes).
        if (!it->is_regular_file(ec)) {
            continue;
        }
        if (!include_glob.empty() && !fnmatch_ascii(fname, include_glob, false)) {
            continue;
        }
        walk_entry entry;
        entry.display = kimix::to_string(entry_path);
        entry.native = entry_path;
        out.push_back(std::move(entry));
    }
}

} // namespace ge

} // namespace

tool_status run_grep(const grep_options &opts, kimix::span<const kimix::string> roots,
                     kimix::string_view work_dir, grep_result &out) {
    out = grep_result{};
    // Literal fast path: a pure literal skips the regex engine. A literal
    // containing '\n' must NOT take it - per-line matching can never span
    // lines, exactly like the regex scan.
    kimix::string literal;
    bool use_literal = ge::extract_literal(opts.pattern, literal);
    if (use_literal && literal.find('\n') != kimix::string::npos) {
        use_literal = false;
    }
    // Compile validation BEFORE the walk (single-threaded), like the old
    // inline branch: an invalid pattern is a parameter error, not a search
    // result. Literal patterns compile by construction and skip this.
    if (!use_literal) {
        regex_lite::Regex re;
        kimix::string re_error;
        if (!re.compile(opts.pattern, opts.ignore_case, re_error)) {
            out.status = tool_status::invalid_input;
            out.message = "invalid pattern: " + re_error;
            return out.status;
        }
    }
    kimix::vector<ge::walk_entry> files;
    for (const kimix::string &root : roots) {
        ge::collect_files(root, work_dir, opts.include_glob, files);
    }
    const size_t n_files = files.size();
    // Threading: the engine never creates a scheduler. If the calling thread
    // is bound to one (the host binds at process/thread start), the chunks
    // fan out over the AMBIENT pool and its worker count sizes the split;
    // an unbound thread scans inline through the same single-chunk path
    // (fiber::parallel would run it inline anyway - going straight to
    // worker(0) skips the dispatch entirely).
    uint32_t pool_workers = 1u;
    if (kimix::fiber::is_bound()) {
        pool_workers = kimix::fiber::worker_thread_count();
    }
    size_t num_chunks = 1;
    // Fan-out threshold: dispatching the chunk jobs costs more than it saves
    // on very small file lists (a micro-benchmark of a 3-file tree runs
    // ~2x faster through the single inline chunk than through fiber::parallel
    // over the ambient pool). Only spread from >= k_min_fanout files.
    constexpr size_t k_min_fanout = 8u;
    if (pool_workers > 1u && n_files >= k_min_fanout) {
        num_chunks = std::min<size_t>(pool_workers, n_files);
    }
    kimix::vector<ge::chunk_output> chunks(num_chunks);
    const size_t base = n_files / num_chunks;
    const size_t rem = n_files % num_chunks;
    // One worker per static index chunk; each compiles its OWN regex_lite
    // engine (Regex is not thread-safe) and renders into its own vectors.
    auto worker = [&](size_t ci) noexcept {
        regex_lite::Regex re;
        if (!use_literal) {
            kimix::string err;
            if (!re.compile(opts.pattern, opts.ignore_case, err)) {
                return; // validated upfront; cannot fail
            }
        }
        ge::chunk_output &chunk = chunks[ci];
        const size_t begin = ci * base + std::min(ci, rem);
        const size_t count = base + (ci < rem ? 1u : 0u);
        for (size_t i = 0; i < count; ++i) {
            ge::scan_file(files[begin + i], opts, &re, literal, use_literal, chunk);
        }
    };
    if (num_chunks <= 1) {
        worker(0);
    } else {
        // One job per chunk, one claim per job, so a claim compiles exactly
        // one regex and fills exactly one chunk. Chunks are disjoint and
        // merged in index order afterwards, so the result stays
        // deterministic and ordered by walk order. The pool the jobs run on
        // is the caller's (num_chunks <= worker_thread_count() of it).
        kimix::fiber::parallel(
            static_cast<uint32_t>(num_chunks),
            [&](uint32_t ci) noexcept { worker(ci); },
            /*internal_jobs=*/1u);
    }
    // Merge chunks in index order: deterministic output ordered by walk order.
    // Chunk storage is dead after its turn, so the rendered lines and file
    // records MOVE into the result (no second round of string allocations).
    for (size_t ci = 0; ci < num_chunks; ++ci) {
        ge::chunk_output &chunk = chunks[ci];
        out.total_matches += chunk.total_matches;
        for (grep_file_result &f : chunk.files) {
            if (opts.mode == grep_output_mode::files_with_matches) {
                // fwm lines are the paths, capped DURING collection (the old
                // branch's behaviour); the files[] array stays complete.
                if (opts.head_limit <= 0 ||
                    static_cast<int64_t>(out.lines.size()) < opts.head_limit) {
                    out.lines.push_back(f.path);
                    out.line_match.push_back(1);
                }
            }
            out.files.push_back(std::move(f));
        }
        if (opts.mode != grep_output_mode::files_with_matches) {
            out.lines.insert(out.lines.end(),
                             std::make_move_iterator(chunk.lines.begin()),
                             std::make_move_iterator(chunk.lines.end()));
            out.line_match.insert(out.line_match.end(), chunk.line_match.begin(),
                                  chunk.line_match.end());
        }
    }
    out.message = kimix::format("{} match(es) in {} file(s)", out.total_matches,
                                out.files.size());
    return out.status;
}

} // namespace kimix::builtin_tools::grep
