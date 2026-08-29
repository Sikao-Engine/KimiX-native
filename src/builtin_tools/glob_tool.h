/*
 * glob_tool.h - C++ port of the Glob built-in tool kernels.
 *
 * Plan: C:/dev/kimi-agent/plans/glob.md
 *   §3.1 exported fnmatch core      -> fnmatch_match()
 *   §3.2 path-glob matcher          -> path_glob_pattern / parse_pattern() /
 *                                      match_segments() / match_path() /
 *                                      is_unsafe_recursive_pattern()
 *   §3.3 native walker              -> walk_matches() (injectable listing
 *                                      callbacks) + walk_matches_fs()
 * Plus the gitignore-ish ignore filter (pure string kernels) and the result
 * shaping used by the Glob tool's output pipeline.
 *
 * Python source of truth (behaviour mirrored exactly unless a deviation is
 * documented in src/builtin_tools/reports/glob.md):
 *   kimi-cli/src/kimi_cli/tools/file/glob.py
 *     _is_unsafe_recursive_pattern        46-67
 *     _parse_gitignore (fallback path)   137-166
 *     _gitignore_match                   169-238
 *     _is_ignored_by_gitignore           241-286
 *     _top_dirs_summary                  296-317
 *     Glob.__call__ (walk + shaping)    508-698
 *   kimi-cli/src/kimi_cli/tools/file/output_utils.py  fold_lines / truncate_line
 *     (shared byte-exact ports live in builtin_tools/tool_types.h)
 *   Python stdlib fnmatch.translate / _translate (CPython 3.14) - the
 *     authoritative single-string matcher semantics.
 *   Python stdlib pathlib._WildcardSelector / _RecursiveWildcardSelector -
 *     the authoritative per-segment / `**` walk semantics used through
 *     kaos/local.py:102-121 (`pathlib.Path.glob(pattern, case_sensitive=...)`).
 *
 * Namespace: kimix::builtin_tools::glob (MANDATORY - kimix-llm is a unity
 * build; every .cpp in src/builtin_tools is concatenated, so no tool may put
 * symbols in the shared namespace).
 *
 * Contract notes:
 *   - Every kernel is pure CPU unless it explicitly says "fs". I/O effects of
 *     the walker are injected through kimix::function callbacks so unit tests
 *     run against an in-memory tree.
 *   - Kernels never throw across the tool boundary: invalid input is returned
 *     as tool_error / tool_status (see builtin_tools/tool_types.h).
 *   - Case folding in the matchers is ASCII-only (A-Z -> a-z). Python folds
 *     full Unicode through os.path.normcase on Windows; that gap is the
 *     documented reason the shipped tool keeps Windows matching on the Python
 *     side (glob.py:43 _NATIVE_GLOB_MATCH_CASE_SENSITIVE).
 */
#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "builtin_tools/tool_types.h"
#include "builtin_tools/tool.h"

namespace kimix {
namespace builtin_tools {
namespace glob {

// ===========================================================================
// §3.1 fnmatch core
// ===========================================================================

// True on Windows, false elsewhere - mirrors pathlib/fnmatch platform defaults
// (same rule as runtime/glob/gitignore.h::default_case_insensitive, restated
// here because that header belongs to the runtime_py target).
constexpr bool default_case_insensitive() noexcept {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

// Full-string fnmatch match, mirroring CPython `fnmatch.fnmatchcase(text,
// pattern)` (and therefore `fnmatch.fnmatch` after normcase when
// `case_insensitive` is true). Supported syntax:
//   *        any run of characters, INCLUDING '/' (the path layer prevents
//            cross-segment matching by calling this per segment)
//   ?        exactly one character
//   [seq]    character class; leading ']' belongs to the class; ranges 'a-z'
//   [!seq]   negated class
//   '['      an unterminated '[' is a literal '[' (CPython _translate)
// Backslashes are literal characters (CPython 3.14's _translate() escapes the
// '\' itself and never treats it as an escape character), and consecutive
// '*' characters behave as a single '*'.
bool fnmatch_match(kimix::string_view pattern, kimix::string_view text,
                   bool case_insensitive) noexcept;

// Convenience: fnmatch_match with the platform default case rule.
bool fnmatch_match_default_case(kimix::string_view pattern,
                                kimix::string_view text) noexcept;

// ===========================================================================
// §3.2 path-glob matcher (pathlib.Path.glob semantics)
// ===========================================================================

// Classification of a parsed pattern segment.
enum class segment_kind : uint8_t {
    literal = 0, // no wildcard metacharacters at all (fast path)
    wildcard,    // contains *, ? or [...] - matched with fnmatch_match()
    double_star, // the segment is exactly "**"
};

// A parsed, reusable path-glob pattern: parse once, match many paths.
struct path_glob_pattern {
    kimix::vector<kimix::string> segments; // '/'-split, empty/'.' segments dropped
    kimix::vector<segment_kind> kinds;     // parallel to `segments`
    bool recursive = false;                // contains a "**" segment
    bool case_insensitive = false;         // ASCII folding in the matcher
    bool dir_only = false;                 // pattern ended with '/'
    bool anchored = true;                  // pattern contained a '/' separator

    bool empty() const { return segments.empty(); }
    size_t size() const { return segments.size(); }
};

// Normalize a path/pattern for matching: '\\' -> '/'.
kimix::string normalize_slashes(kimix::string_view path);

// Split a normalized path into segments, dropping empty ones ("a//b" ==
// "a/b", "./a" == "a" - pathlib._parse_pattern drops them).
void split_segments(kimix::string_view path,
                    kimix::vector<kimix::string_view> &out);

// Parse `pattern` (port of pathlib._parse_pattern + the per-segment
// classification). Errors (returned as data, never thrown):
//   invalid_input  empty pattern / pattern made only of empty or '.' segments
//                  (pathlib: "Unacceptable pattern")
//   invalid_input  absolute pattern or drive-qualified pattern
//                  (pathlib: "Non-relative patterns are unsupported")
tool_error parse_pattern(kimix::string_view pattern, bool case_insensitive,
                         path_glob_pattern &out);

// Convenience one-shot: default case rule.
tool_error parse_pattern_default_case(kimix::string_view pattern,
                                      path_glob_pattern &out);

// Match an already-split relative path (segment list) against `pat`.
bool match_segments(const path_glob_pattern &pat,
                    kimix::span<const kimix::string_view> rel_segments) noexcept;

// Match a relative path string against `pat` ('\\' is normalized to '/').
// An empty `rel_path` (the search root itself) matches only when the pattern
// is fully nullable ("**"), mirroring pathlib yielding `.` - the Glob tool
// never reports the root, so the walker filters it out separately.
bool match_path(const path_glob_pattern &pat, kimix::string_view rel_path) noexcept;

// Single-shot helper: parse + match (used by parity harnesses).
bool match_path_pattern(kimix::string_view pattern, kimix::string_view rel_path,
                        bool case_insensitive) noexcept;

// Relaxed, tool-description-level matching: a pattern without '/' matches the
// basename at any depth (`*.ts` finds `src/a.ts`). The shipped Python tool
// does NOT do this - it calls pathlib.Path.glob(), which anchors a '/'-free
// pattern at the search root. Exposed because the tool description promises
// it (glob.py:425-428); the walker only uses pathlib-faithful matching.
bool match_basename_at_any_depth(const path_glob_pattern &pat,
                                 kimix::string_view rel_path) noexcept;

// True when the pattern has no '/' separator after backslash normalization
// (i.e. it addresses a single basename segment).
bool is_basename_pattern(kimix::string_view pattern) noexcept;

// Port of glob.py:46-67. Blocks "**", "**/*", "**/**", "**\\*" and any
// pattern whose every segment is a bare wildcard while containing at least
// one "**" segment: such patterns recursively match everything under the
// root, which is meaningless and extremely slow.
bool is_unsafe_recursive_pattern(kimix::string_view pattern) noexcept;

// Error envelope + brief for the rejected pattern, byte-identical to the
// ToolError text in glob.py:513-522 (status = invalid_input). `brief` is the
// ToolError brief field ("Unsafe pattern: <pattern>").
tool_error make_unsafe_pattern_error(kimix::string_view pattern,
                                     kimix::string &brief);

// ===========================================================================
// gitignore-ish ignore filter (pure string kernels) — declared before the
// walker because walk_options references it.
// ===========================================================================

// One parsed ignore rule (mirrors glob.py:_GitignoreRule / the native
// runtime::glob::gitignore_rule, restated for this namespace).
struct ignore_rule {
    kimix::string pattern;
    bool negated = false;    // leading '!'
    bool anchored = false;   // contains '/' (not just a trailing one)
    bool dir_only = false;   // trailing '/'
    kimix::string source_dir; // directory owning the rule, relative to the
                              // walk root ('' == the root itself); rules never
                              // apply to paths outside their source dir.

    bool operator==(const ignore_rule &) const = default;
};

// ===========================================================================
// §3.3 walker inputs (injectable so unit tests need no fixtures)
// ===========================================================================

// One directory entry as reported by a listing callback.
struct dirent_info {
    kimix::string name;   // basename only, no separators
    bool is_dir = false;  // directory (or junction/symlink-to-dir)
    bool is_symlink = false; // reparse point / symlink (walk never descends)

    bool operator==(const dirent_info &) const = default;
};

// Stat information for one entry (mtime is caller-supplied in tests).
struct entry_stat {
    bool is_dir = false;
    int64_t size_bytes = -1; // -1 when unknown
    double mtime = 0.0;      // seconds since epoch (st_mtime float parity)
};

// A collected match.
struct walk_entry {
    kimix::string rel_path; // '/'-separated, relative to the walk root
    bool is_dir = false;
    int64_t size = -1;   // -1 unless walk_options::collect_stats
    double mtime = 0.0;  // 0.0 unless walk_options::collect_stats

    bool operator==(const walk_entry &) const = default;
};

// Lists `dir_rel_path` ('' == the root itself; otherwise '/'-separated, no
// trailing slash). On success return a non-failed tool_error and append the
// entries. A failed tool_error means "cannot be listed": the walker skips the
// directory silently (pathlib behaviour) and counts it in
// walk_result::skipped_dirs.
using list_dir_fn = kimix::function<tool_error(
    kimix::string_view dir_rel_path, kimix::vector<dirent_info> &out)>;

// Optional stat probe (size/mtime). Returns false when the entry vanished; the
// walker then keeps size = -1 / mtime = 0.0 like Python's _safe_getmtime.
using stat_fn = kimix::function<bool(kimix::string_view rel_path,
                                     entry_stat &out)>;

struct walk_options {
    bool include_dirs = false;   // keep directory matches (glob.py:570)
    // Null = no ignore filtering. Otherwise rules are evaluated per entry with
    // source_dir-relative paths, mirroring _is_ignored_by_gitignore.
    const kimix::vector<ignore_rule> *ignore_rules = nullptr;
    bool prune_ignored_dirs = false; // skip descending into ignored dirs
    size_t max_matches = 0;          // 0 = unlimited (glob.py MAX_MATCHES)
    bool collect_stats = false;      // fill size/mtime via the stat probe
    uint64_t deadline_ms = 0;        // 0 = no deadline (cooperative abort)
};

struct walk_result {
    kimix::vector<walk_entry> entries; // sorted by rel_path (byte order)
    bool truncated = false;            // a match arrived after max_matches
    bool timed_out = false;            // deadline_ms expired
    size_t ignored_count = 0;          // matches suppressed by ignore rules
    size_t skipped_dirs = 0;           // listings that failed (permission, ...)
    size_t visited_dirs = 0;           // successful listings (diagnostics)
    size_t listed_entries = 0;         // entries seen (diagnostics)
};

// Walk an injected tree. `pattern` must come from parse_pattern(). Results are
// relative paths normalized to '/', sorted byte-wise, deduplicated, capped at
// max_matches (with `truncated` set exactly like glob.py:597-600: the flag is
// only raised by an overflow candidate, so exactly max_matches matches is not
// "capped"), and aborted at deadline_ms (partial results + timed_out).
walk_result walk_matches(const list_dir_fn &lister, const stat_fn &stat,
                         const path_glob_pattern &pattern,
                         const walk_options &options);

// Real-filesystem convenience wrapper. Directory listing uses Win32
// FindFirstFile/FindNextFile on Windows and opendir/readdir elsewhere; stats
// use the matching stat() call. Non-existent / unreadable directories are
// skipped silently. `root` is never included in the results.
walk_result walk_matches_fs(const kimix::filesystem::path &root,
                            const path_glob_pattern &pattern,
                            const walk_options &options);

// Same, with a one-shot pattern string. `out_error` receives the parse failure.
walk_result walk_matches_fs(kimix::string_view root, kimix::string_view pattern,
                            const walk_options &options, tool_error &out_error);

// Parse ignore-file content into rules (port of the pure-Python
// _parse_gitignore, glob.py:137-166): rstrip whitespace, skip blank and
// '#'-comment lines, '!' negation, trailing '/' dir-only, anchoring on any
// interior '/', leading '/' stripped (and forces anchored).
kimix::vector<ignore_rule> parse_ignore_rules(
    kimix::string_view content, kimix::string_view source_dir = {});

// Match one relative path against one rule (port of _gitignore_match,
// glob.py:169-238) including the dir-only "matches every descendant" rule and
// the '**' / '**/' / '/**' / '/**/' special cases.
bool ignore_rule_match(kimix::string_view rel_path, bool is_dir,
                       const ignore_rule &rule, bool case_insensitive) noexcept;

// True when `rel_path` is ignored by the rule list (port of
// _is_ignored_by_gitignore, glob.py:241-286): rules are evaluated in order,
// each against the path relative to its own source_dir, and a later match
// overrides the earlier verdict (negation un-ignores).
bool is_ignored(kimix::string_view rel_path, bool is_dir,
                const kimix::vector<ignore_rule> &rules,
                bool case_insensitive) noexcept;

// ===========================================================================
// result shaping (Glob tool output pipeline)
// ===========================================================================

inline constexpr size_t k_max_matches = 1000u;  // glob.py:36 MAX_MATCHES
inline constexpr size_t k_default_top_dirs = 3u;

// Byte-wise sort (kimix::string order), matching the tool's determinism
// requirement (glob.py:605 matches.sort()).
void sort_entries(kimix::vector<walk_entry> &entries) noexcept;

// Drop duplicate rel_path rows, keeping the first occurrence. Returns the
// number of removed entries.
size_t dedup_entries(kimix::vector<walk_entry> &entries) noexcept;

// Strip `prefix` (a '/'-separated relative path, e.g. the search root as seen
// from the workspace) from every entry, leaving "" for exact matches. Returns
// the number of rewritten entries.
size_t strip_prefix(kimix::vector<walk_entry> &entries,
                    kimix::string_view prefix) noexcept;

// Stable ordering by descending mtime, keeping the first `k` rows (k == 0
// means "keep all"). Ties keep the incoming (path-sorted) order. This is the
// "modification-time order, up to N paths" shape advertised by the tool
// description (glob.md); the shipped Python code sorts by path instead.
void order_by_mtime_top_k(kimix::vector<walk_entry> &entries, size_t k) noexcept;

// head_limit / offset pagination over already-sorted entries. `out` receives
// the window, `total` the input size and `omitted_after` the number of rows
// dropped from the tail. head_limit == 0 means unlimited.
void paginate_entries(kimix::span<const walk_entry> entries, size_t head_limit,
                      size_t offset, kimix::vector<walk_entry> &out,
                      size_t &total, size_t &omitted_after);

// Display-line builder: one line per entry (optionally "  (N bytes, file|dir,
// <mtime text>)" appended, mirroring glob.py:615-629 - the mtime text itself is
// produced by the caller because pendulum formatting stays in Python), each
// line capped with truncate_line, then the 100 KiB byte budget and finally the
// head+tail fold to `max_results` (0 = unlimited).
struct shape_options {
    bool verbose = false;
    size_t max_line_len = 500u;        // truncate_line budget (output_utils)
    size_t max_bytes = k_max_output_bytes;
    size_t max_results = 500u;         // fold budget (Params.max_results)
    // verbose mtime renderer (rel_path, entry) -> text. Empty = size/kind only.
    kimix::function<kimix::string(const walk_entry &)> format_mtime;
};

struct shaped_output {
    kimix::vector<kimix::string> lines;
    size_t omitted_by_fold = 0; // marker line NOT counted in `lines`
    bool truncated_by_bytes = false;
    size_t shown_count = 0; // real (non-marker) lines
    size_t total_bytes = 0;
};

void shape_output(kimix::span<const walk_entry> entries,
                  const shape_options &options, shaped_output &out);

// "top dirs: a (5), b (2)" summary over the collected matches (port of
// _top_dirs_summary, glob.py:296-317). Files directly in the search root are
// not counted. Returns "" when there is nothing to report.
kimix::string top_dirs_summary(kimix::span<const walk_entry> entries,
                               size_t top = k_default_top_dirs);

// Everything the message builder needs; field names mirror the Python locals.
struct message_input {
    kimix::string_view pattern;
    size_t total = 0;             // collected match count
    size_t ignored_count = 0;     // matches suppressed by .gitignore
    size_t shown_count = 0;       // real lines in the output
    size_t omitted_by_fold = 0;   // lines hidden by the fold marker
    bool respect_gitignore = true;
    bool truncated = false;       // collection cap hit (MAX_MATCHES)
    bool timed_out = false;       // timeout fired
    uint64_t timeout_seconds = 10;
    bool truncated_by_bytes = false;
    size_t max_matches = k_max_matches;
    size_t max_bytes = k_max_output_bytes;
    bool with_top_dirs = false;   // Python only appends it inside the fold branch
    kimix::string top_dirs;       // precomputed top_dirs_summary() text, may be ""
};

// Exact assembly of the Glob `message` field (glob.py:660-690).
kimix::string build_result_message(const message_input &input);

// ---------------------------------------------------------------------------
// §4 Tool class and standard integration
// ---------------------------------------------------------------------------

// Concrete Glob tool subclass. The Python binding layer invokes this through
// the shared ToolParams JSON-object contract; the C++ side owns the pattern
// guard, filesystem walk, result shaping and message assembly. Path
// resolution, .gitignore rule discovery, pendulum formatting and the final
// ToolOk/ToolError envelope stay in Python (see reports/glob.md).
class Glob : public kimix::builtin_tools::Tool {
public:
    explicit Glob(kimix::builtin_tools::Session *session);
    void operator()(kimix::builtin_tools::ToolParams const *parameters) override;

    // Access the serialized JSON produced by the last operator() invocation.
    const kimix::vector<char> &last_result() const { return _last_result; }

private:
    kimix::vector<char> _last_result;
};

} // namespace glob
} // namespace builtin_tools
} // namespace kimix
