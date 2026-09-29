// grep_engine.cpp - Implementation of the native grep search engine.
// See grep_engine.h for the design notes (ripgrep-inspired: whole-buffer
// zero-copy line scan, literal fast path, per-thread regexes over static
// chunks, 64 KiB NUL binary sniff). Semantics mirror the previous inline
// native_io branch of Grep::operator() byte-for-byte where observable.
//
// Compiled into the kimix-llm static library with a unity (jumbo) batch, so
// all file-local helpers live inside an anonymous namespace and carry
// grep-engine-specific names (no file-scope same-named globals).

#include "builtin_tools/grep_engine.h"

#include "builtin_tools/grep_tool.h" // fnmatch_ascii (shared with grep_tool.cpp)
#include "builtin_tools/regex_lite.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>

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

// Per-chunk (per-thread) search output, merged in chunk index order so the
// final result is deterministic and ordered by walk order. No mutexes: each
// chunk owns a disjoint index range of the file list.
struct chunk_output {
    kimix::vector<grep_file_result> files;
    kimix::vector<kimix::string> lines;
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
// is the literal char. Unknown alphabetic escapes are left to the regex
// engine. Unescapes into `lit` (also set on the false paths that consume a
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
                if (is_alpha(pat[i])) {
                    return false; // unknown alphabetic escape: regex engine decides
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
        for (size_t k = 1; k < needle.size(); ++k) {
            char a = hay[off + k];
            char b = needle[k];
            if (fold_case) {
                a = lower_ascii(a);
                b = lower_ascii(b);
            }
            if (a != b) {
                eq = false;
                break;
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

// Split `text` into line views at '\n', stripping one trailing '\r' per line.
// A trailing partial line (no closing '\n') is emitted; a buffer ending in
// '\n' has no extra empty line after it (same as the old substr scanner).
void collect_lines(const kimix::string &text, kimix::vector<line_view> &out) {
    out.clear();
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
                out.push_back(line_view{start, len});
            }
            break;
        }
        const size_t nl = static_cast<size_t>(nl_hit - text.data());
        size_t len = nl - start;
        if (len > 0 && text[nl - 1] == '\r') {
            --len;
        }
        out.push_back(line_view{start, len});
        start = nl + 1u;
    }
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
            last_emitted = l;
        }
        last_emitted = std::max(last_emitted, hi);
    }
}

// Search one already-collected file. Silently skipped on: not a regular
// file, stat size > 4 MiB, include-glob mismatch, open/read errors, or a NUL
// in the first 64 KiB of the buffer (rg binary convention).
void scan_file(const kimix::string &path, const grep_options &opts,
               const regex_lite::Regex *re, kimix::string_view literal, bool use_literal,
               chunk_output &out) {
    namespace fs = kimix::filesystem;
    std::error_code ec;
    const fs::path file(path);
    if (!fs::is_regular_file(file, ec)) {
        return;
    }
    if (fs::file_size(file, ec) > k_max_file_bytes) {
        return;
    }
    if (!opts.include_glob.empty()) {
        const kimix::string name = kimix::to_string(file.filename());
        if (!fnmatch_ascii(name, opts.include_glob, false)) {
            return;
        }
    }
    std::FILE *f = std::fopen(kimix::to_string(file).c_str(), "rb");
    if (f == nullptr) {
        return;
    }
    kimix::string text;
    char buf[65536];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        text.append(buf, n);
        if (text.size() > k_max_file_bytes) {
            std::fclose(f);
            return;
        }
    }
    std::fclose(f);
    // Binary sniff on the read buffer: a NUL within the first 64 KiB means
    // "binary, skip silently". A NUL past 64 KiB does NOT (the read buffer,
    // not the whole file, is what rg sniffs).
    const size_t sniff_len = std::min(text.size(), k_binary_sniff_bytes);
    if (std::memchr(text.data(), '\0', sniff_len) != nullptr) {
        return;
    }
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
    if (opts.mode == grep_output_mode::count_matches) {
        out.lines.push_back(kimix::format("{}:{}", path, file_matches));
        return;
    }
    if (opts.mode == grep_output_mode::content) {
        render_content(opts, path, text, lines, hit_lines, out);
    }
    // files_with_matches renders nothing here: the merge step derives the
    // (head_limit-capped) lines from the complete files[] array.
}

// -- single-threaded walk ----------------------------------------------------

// Append every non-hidden file system entry under `root_str` to `out`, in
// walk order. Relative roots resolve against `work_dir`. Hidden entries
// (name starting with '.') are skipped at every depth and hidden directories
// are not descended. Only error_code overloads are used; any iterator error
// ends the walk for that root.
void collect_files(const kimix::string &root_str, kimix::string_view work_dir,
                   kimix::vector<kimix::string> &out) {
    namespace fs = kimix::filesystem;
    fs::path rp(root_str);
    if (rp.is_relative() && !work_dir.empty()) {
        rp = fs::path(kimix::string(work_dir)) / rp;
    }
    std::error_code ec;
    if (!fs::exists(rp, ec)) {
        return;
    }
    if (fs::is_regular_file(rp, ec)) {
        out.push_back(kimix::to_string(rp));
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
        const fs::path entry = it->path();
        // Skip hidden dirs (.git etc.) at every depth of the walk.
        const kimix::string fname = kimix::to_string(entry.filename());
        if (!fname.empty() && fname[0] == '.' && fname != "." && fname != "..") {
            if (it->is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        out.push_back(kimix::to_string(entry));
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
    kimix::vector<kimix::string> files;
    for (const kimix::string &root : roots) {
        ge::collect_files(root, work_dir, files);
    }
    const size_t n_files = files.size();
    size_t num_threads =
        std::min<size_t>(std::max(1u, std::thread::hardware_concurrency()), 8u);
    if (n_files == 0) {
        num_threads = 1;
    } else if (num_threads > n_files) {
        num_threads = n_files;
    }
    kimix::vector<ge::chunk_output> chunks(num_threads);
    const size_t base = n_files / num_threads;
    const size_t rem = n_files % num_threads;
    // One worker per static index chunk; each compiles its OWN regex_lite
    // engine (Regex is not thread-safe) and renders into its own vectors.
    auto worker = [&](size_t ti) {
        regex_lite::Regex re;
        if (!use_literal) {
            kimix::string err;
            if (!re.compile(opts.pattern, opts.ignore_case, err)) {
                return; // validated upfront; cannot fail
            }
        }
        ge::chunk_output &chunk = chunks[ti];
        const size_t begin = ti * base + std::min(ti, rem);
        const size_t count = base + (ti < rem ? 1u : 0u);
        for (size_t i = 0; i < count; ++i) {
            ge::scan_file(files[begin + i], opts, &re, literal, use_literal, chunk);
        }
    };
    if (num_threads <= 1) {
        worker(0);
    } else {
        kimix::vector<std::thread> pool;
        pool.reserve(num_threads);
        for (size_t ti = 0; ti < num_threads; ++ti) {
            pool.emplace_back(worker, ti);
        }
        for (std::thread &t : pool) {
            if (t.joinable()) {
                t.join();
            }
        }
    }
    // Merge chunks in index order: deterministic output ordered by walk order.
    for (size_t ti = 0; ti < num_threads; ++ti) {
        const ge::chunk_output &chunk = chunks[ti];
        out.total_matches += chunk.total_matches;
        for (const grep_file_result &f : chunk.files) {
            out.files.push_back(f);
            if (opts.mode == grep_output_mode::files_with_matches) {
                // fwm lines are the paths, capped DURING collection (the old
                // branch's behaviour); the files[] array stays complete.
                if (opts.head_limit <= 0 ||
                    static_cast<int64_t>(out.lines.size()) < opts.head_limit) {
                    out.lines.push_back(f.path);
                }
            }
        }
        if (opts.mode != grep_output_mode::files_with_matches) {
            out.lines.insert(out.lines.end(), chunk.lines.begin(), chunk.lines.end());
        }
    }
    out.message = kimix::format("{} match(es) in {} file(s)", out.total_matches,
                                out.files.size());
    return out.status;
}

} // namespace kimix::builtin_tools::grep
