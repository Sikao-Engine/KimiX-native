// grep_tool.h - C++ port of the kimi-agent grep tool's pure string kernels.
//
// Plan: C:/dev/kimi-agent/plans/grep.md ("Plan: Rewrite grep to C++").
// Source of truth (C:/dev/kimi-agent/kimi-cli/src/kimi_cli/):
//   tools/file/grep_selectors.py (320 lines)
//     LineRange/GrepPathSpec (39-59), parse_line_range_chunk (79-118),
//     parse_line_ranges (121-160), is_line_in_ranges (163-172),
//     selector_line_ranges (175-193), _is_selector_shape (206-215),
//     split_path_and_sel (218-263), _maybe_json_array (266-278),
//     expand_path_entries (281-306), merge_ranges_into (309-320)
//   tools/file/grep_archive.py (155 lines)
//     parse_archive_path_candidates (39-61), _safe_scratch_name (78-82)
//   tools/file/read_archive.py
//     ARCHIVE_EXTENSIONS (29-53), is_archive_path (68-77)
//   tools/file/grep_output.py (115 lines)
//     format_match_line (22-29), group_lines_by_file (32-60),
//     format_grouped_output (63-82), group_line_indices_by_blank (85-102),
//     should_group (105-115)
//   tools/file/grep_recorder.py (97 lines)
//     FileRecorder (28-49), _merged (52-60), record_grep_files cap (81-82)
//   tools/file/output_utils.py
//     protocol regexes (34-51), _parse_tail_hint (153-158),
//     parse_rtk_rg_output (161-239)
//   tools/file/grep_local.py
//     _pattern_has_regex_newline (106-117), _multiline_pattern (120-138),
//     _RG_CONTENT_LINE_RE / parse_content_line (284-302), _RG_LINE_RE (611),
//     _join_with_byte_limit (620-634), _strip_path_prefix (637-650),
//     _rtk_fold_note (658-695), _BARE_CONTENT_RE +
//     _reattach_single_file_prefix (836-855), _strip_key_for (980-986),
//     _remap_display (1014-1031), _normalize_slashes_content (1034-1050),
//     _range_filter_lines (1053-1078), _collect_record_files (1081-1093),
//     sensitive-filter path extraction (1660-1676), _entries_are_rich (1124-1131)
//   utils/sensitive.py
//     SENSITIVE_PATTERNS / SENSITIVE_EXEMPTIONS / is_sensitive_file /
//     sensitive_file_warning
//
// What deliberately stays in Python (plans/grep.md §3 "Reuse"/"New kernels",
// §9 "Migration steps"): rg/rtk subprocess orchestration and binary
// provisioning, workspace/VFS path resolution, archive extraction I/O, session
// persistence, the micro-compress driver (Phase C) and grep_args (Phase D).
// The PCRE2 regex line-search kernel (plans/grep.md §3 kernel 7, Phase B) is
// BLOCKED: PCRE2 is not vendored in src/ext and adding a third-party library
// is forbidden - see issue/grep.md. Phase A therefore ships the line-offset
// scanner contract (the caller supplies match offsets) instead.
//
// Unicode parity. The reference runs `regex`/`re` with Unicode semantics, so
// \d is category Nd and \s/str.strip() use the 29-code-point whitespace set.
// Two mechanisms keep the native answers byte-exact:
//   * hard gate: kernels whose decision depends on \d or [^\w.-] inside a
//     region that contains non-ASCII bytes return tool_status::unsupported, so
//     the shim falls back to the pure-Python mirror (same convention as
//     src/runtime/tools/grep_pattern.* and security.*); and
//   * bounded gate: for content-line parsing only the bytes up to the closing
//     delimiter are decisive, so a line whose first non-ASCII byte sits at or
//     after that position is parsed natively - which keeps non-ASCII *text*,
//     the overwhelmingly common case, on the fast path.
// Python whitespace/blank tests (str.strip(), `\s*`, `\S+`) are implemented
// with the exact Unicode code-point set, so they need no gating. Pure byte
// transforms (prefix strip, slash normalize, remap, recorder, join) are never
// gated either.
//
// Namespace: kimix::builtin_tools::grep. kimix-llm builds with a unity (jumbo)
// batch, so every symbol of this tool lives inside this namespace.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "builtin_tools/tool_types.h"
#include "builtin_tools/utf8_util.h"

namespace kimix::builtin_tools::grep {

// Shared vocabulary (builtin_tools/tool_types.h) - reuse, never re-declare.
using kimix::builtin_tools::line_range; // 1-based inclusive; nullopt == to EOF
using kimix::builtin_tools::tool_error;
using kimix::builtin_tools::tool_status;

// One `archive:member` split (parse_archive_path_candidates).
struct archive_candidate {
    kimix::string archive;
    kimix::string member;

    bool operator==(const archive_candidate &) const = default;
};

// Scratch path -> display path entry (_remap_display / materialization map).
struct display_entry {
    kimix::string scratch;
    kimix::string display;

    bool operator==(const display_entry &) const = default;
};

// Per-path line ranges in insertion order (Python dict semantics; the vector
// keeps the C++ side deterministic and iteration order-compatible).
struct path_ranges {
    kimix::string path;
    kimix::vector<line_range> ranges;
};
using ranges_map = kimix::vector<path_ranges>;
using display_map = kimix::vector<display_entry>;

// Outcome of one selector-grammar parse. `ok` carries a value (or
// `no_value == true` when Python returned None), `invalid_input` carries
// Python's ValueError text byte for byte, `unsupported` means the input is
// outside the native subset (non-ASCII digits, or an integer past uint32_t).
struct selector_result {
    tool_status status = tool_status::ok;
    kimix::string message;
    bool no_value = false;
    line_range range;                 // single-chunk results
    kimix::vector<line_range> ranges; // multi-chunk parsers

    bool failed() const { return status != tool_status::ok; }
};

// A parsed rg content-mode line: `path:LN:text` (match) or `path-LN-text`
// (context) - grep_local.parse_content_line's 4-tuple.
struct content_line {
    kimix::string path;
    uint32_t line_no = 0;
    kimix::string text;
    bool is_match = false;
};

// One entry of a grouped body: (line_no, text, is_match). line_no == 0 is
// Python's sentinel for verbatim lines ("--" separators, gap markers).
struct grouped_entry {
    uint32_t line_no = 0;
    kimix::string text;
    bool is_match = false;
};

// One file group: display path + body, in encounter order.
struct file_group {
    kimix::string path;
    kimix::vector<grouped_entry> body;
};

// ---------------------------------------------------------------------------
// 1. Selector grammar (grep_selectors.py)
// ---------------------------------------------------------------------------

// parse_line_range_chunk (79-118). The chunk is Python-strip()ed first; the
// grammar is `^L?(\d+)(?:(\.\.|[-+])(?:L?(\d+))?)?$` matched
// case-insensitively. `..` normalizes to the `-` branch, "N+" behaves like
// "N-", and a bare "N" is open-ended. Error texts:
//   "Line selector {start} is invalid; lines are 1-indexed. Use :1."
//   "Invalid range {start}-{end}: end must be >= start."
//   "Invalid range {start}+{count}: count must be >= 1."
selector_result parse_line_range_chunk(kimix::string_view chunk);

// parse_line_ranges (121-160): comma-split, keep every chunk that parses,
// sort (stable) by start, merge overlapping/adjacent, open-ended absorbs
// everything after it. `no_value` mirrors Python's None.
selector_result parse_line_ranges(kimix::string_view sel);

// is_line_in_ranges (163-172). An empty `ranges` list means unfiltered -> true.
bool is_line_in_ranges(uint32_t line_number,
                       kimix::span<const line_range> ranges) noexcept;

// selector_line_ranges (175-193): ':'-separated chunks, skipping the `raw` /
// `conflicts` display modes; returns the first parseable range list.
selector_result selector_line_ranges(kimix::string_view sel);

// split_path_and_sel (218-263). `probe_exists` replaces os.path.lexists(raw)
// (inject it so the kernel stays pure and tests need no fixtures); when it
// answers true the raw path wins (issue #4618 parity) and nothing is peeled.
// The Windows drive-letter guard (ntpath.splitdrive) and the
// `scheme://authority` port guard are replicated exactly. At most two chunks
// peel; they are rejoined with ':' so the result feeds selector_line_ranges.
// `has_selector` mirrors `selector is not None` (a peeled empty tail is
// impossible: _is_selector_shape rejects it).
struct path_selector {
    kimix::string path;
    kimix::string selector;
    bool has_selector = false;
};
tool_status split_path_and_sel(kimix::string_view raw_path,
                               const kimix::function<bool(kimix::string_view)> &probe_exists,
                               path_selector &out);

// expand_path_entries (281-306), single-string form: a JSON array of strings
// wins (orjson strictness: no trailing comma, no lone surrogates, no trailing
// content), otherwise ';' splits. Commas never split entries. Items are
// Python-strip()ed, empties dropped, duplicates removed in first-seen order.
// Non-ASCII input is rejected (the reference decodes unicode JSON strings).
tool_status expand_path_entries(kimix::string_view raw, kimix::vector<kimix::string> &out);

// expand_path_entries (281-306), list form (Python's `isinstance(raw, list)`).
// Non-ASCII items are skipped the way Python skips non-str items.
tool_status expand_path_entries(kimix::span<const kimix::string> raw,
                                kimix::vector<kimix::string> &out);

// merge_ranges_into (309-320): append `ranges` under `abs_key`, creating the
// bucket when needed; empty ranges are a no-op.
void merge_ranges_into(ranges_map &map, kimix::string_view abs_key,
                       kimix::span<const line_range> ranges);

// ranges_map lookup (Python dict .get): nullptr when the key is absent.
const kimix::vector<line_range> *ranges_map_find(const ranges_map &map,
                                                 kimix::string_view key) noexcept;

// _entries_are_rich (1124-1131): more than one entry, or the single entry
// carries a selector or an archive member.
bool entries_are_rich(kimix::span<const kimix::string> entries,
                      const kimix::function<bool(kimix::string_view)> &probe_exists);

// ---------------------------------------------------------------------------
// 2. Archive paths (grep_archive.py + read_archive.py tables)
// ---------------------------------------------------------------------------

// ARCHIVE_EXTENSIONS (read_archive.py 29-53), longest first so `.tar.gz` wins
// over `.gz`.
kimix::span<const kimix::string_view> archive_extensions() noexcept;

// is_archive_path (read_archive.py 76-77): case-insensitive suffix test
// (Python `path.lower().endswith(ext)`).
bool is_archive_path(kimix::string_view path) noexcept;

// parse_archive_path_candidates (grep_archive.py 39-61): rightmost-first ':'
// splits into (archive, member) pairs; nested archives keep splitting and the
// first non-archive left side stops the scan.
void parse_archive_path_candidates(kimix::string_view entry,
                                   kimix::vector<archive_candidate> &out);

// _safe_scratch_name (grep_archive.py 78-82): ntpath/os.path.basename of the
// '\\'-normalized member, then `[^\w.-]+` runs collapse to a single '_', and
// "member" when the result is empty. ASCII-gated (\w is Unicode-aware).
tool_status safe_scratch_name(kimix::string_view member, kimix::string &out);

// _remap_display (grep_local.py 1014-1031): rewrite scratch-path prefixes to
// their `archive:member` display form (forward-slash comparison; the ORIGINAL
// line is kept verbatim when no scratch prefix matches). An empty map is the
// Python early return (lines copied verbatim).
void remap_display(kimix::span<const kimix::string> lines, const display_map &map,
                   kimix::vector<kimix::string> &out);

// _strip_key_for (grep_local.py 980-986): one path -> post-strip display key.
void strip_key_for(kimix::string_view path_arg, kimix::string_view prefix_base,
                   kimix::string &out);

// ---------------------------------------------------------------------------
// 3. Output rendering (grep_output.py + grep_local.py content pipeline)
// ---------------------------------------------------------------------------

// parse_content_line (287-302) with the exact `^(.*?)([:\-])(\d+)\2(.*)$`
// re.DOTALL semantics: leftmost delimiter pair, the closing delimiter must
// equal the opening one, and the path must be non-empty. `no_match` mirrors
// Python returning None ("--", separators, malformed lines, empty path).
tool_status parse_content_line(kimix::string_view line, content_line &out, bool &no_match);

// The `_RG_LINE_RE` shape test (`^(.*?)([:\-])(\d+)\2`, NO re.DOTALL) used by
// the sensitive-file step (grep_local.py 1667): `path_len` is the byte length
// of group 1, which may be 0 (the Python caller tests `if file_path`). A
// newline before the delimiter ends the scan, as in the reference.
tool_status line_path_shape(kimix::string_view line, size_t &path_len, bool &no_match) noexcept;

// format_match_line (grep_output.py 22-29): "*N|text" for matches, " N|text"
// for context lines; line numbers are never padded.
void format_match_line(uint32_t line_number, kimix::string_view text, bool is_match,
                       kimix::string &out);

// group_lines_by_file (grep_output.py 32-60) using parse_content_line. Any
// non-ASCII line is unsupported (it can never form a group natively).
tool_status group_lines_by_file(kimix::span<const kimix::string> lines,
                                kimix::vector<file_group> &out);

// format_grouped_output (grep_output.py 63-82): a blank line between groups
// (none before the first), a "# <path>" header, then the body rendered with
// format_match_line; line_no 0 entries emit verbatim.
void format_grouped_output(const kimix::vector<file_group> &groups,
                           kimix::vector<kimix::string> &out);

// group_line_indices_by_blank (grep_output.py 85-102): 0-based index groups
// delimited by blank lines; blank lines belong to no group. Python blankness
// (str.strip()) is exact, so this needs no ASCII gate.
void group_line_indices_by_blank(kimix::span<const kimix::string> raw_lines,
                                 kimix::vector<kimix::vector<uint32_t>> &out);

// should_group (grep_output.py 105-115): an explicit params.grouped wins;
// auto (None) activates grouping only for rich entries.
bool should_group(bool grouped, bool grouped_is_set, bool has_rich_entries) noexcept;

// _range_filter_lines (grep_local.py 1053-1078): drop out-of-range content
// lines, then prune leading / doubled / trailing "--". An empty map is the
// Python early return (lines copied verbatim).
tool_status range_filter_lines(kimix::span<const kimix::string> lines, const ranges_map &map,
                               kimix::vector<kimix::string> &out);

// _reattach_single_file_prefix (grep_local.py 836-855): rg omits the path for
// single-file targets ("2:text"); re-attach "prefix<sep>" in front of lines
// matching `^(\d+)([:\-])`. An empty prefix is the Python passthrough.
tool_status reattach_single_file_prefix(kimix::span<const kimix::string> lines,
                                        kimix::string_view prefix,
                                        kimix::vector<kimix::string> &out);

// _strip_path_prefix (grep_local.py 637-650): compare the forward-slash
// normalized line against the normalized base, but slice the ORIGINAL bytes -
// exactly what the Python does (so backslash bytes inside the stripped prefix
// survive, e.g. "a\b\c.py" -> "b\c.py").
void strip_path_prefix(kimix::span<const kimix::string> lines, kimix::string_view search_base,
                       kimix::vector<kimix::string> &out);

// _normalize_slashes_content (grep_local.py 1034-1050). `on_windows` is the
// caller-supplied os.sep == "\\" test (the Python is a passthrough on POSIX);
// when false the lines are copied verbatim. Note the line number is re-rendered
// from the parsed integer, so "a:007:x" becomes "a:7:x" (as in Python).
tool_status normalize_slashes_content(kimix::span<const kimix::string> lines,
                                      kimix::string_view output_mode, bool on_windows,
                                      kimix::vector<kimix::string> &out);

// _collect_record_files (grep_local.py 1081-1093): distinct paths in stream
// order. output_mode: "content" | "count_matches" | anything else (fwm).
tool_status collect_record_files(kimix::span<const kimix::string> lines,
                                 kimix::string_view output_mode, kimix::vector<kimix::string> &out);

// ---------------------------------------------------------------------------
// 4. rtk protocol (output_utils.parse_rtk_rg_output + grep_local._rtk_fold_note)
// ---------------------------------------------------------------------------

// One "  +37 more in <path> [see remaining: <hint>]" marker.
struct rtk_folded_file {
    kimix::string path;
    uint32_t count = 0;
    kimix::string log; // hint payload; empty == Python None
    bool has_log = false;
    kimix::optional<uint32_t> start_line;
};

// Metadata extracted from rtk's protocol lines.
struct rtk_meta {
    kimix::optional<uint32_t> total_matches;
    kimix::optional<uint32_t> total_files;
    kimix::vector<rtk_folded_file> folded_files;
    kimix::optional<uint32_t> skipped_files;
    kimix::string skipped_log;
    bool has_skipped_log = false;

    bool empty() const {
        return !total_matches && !total_files && folded_files.empty() && !skipped_files &&
               !has_skipped_log;
    }
};

// parse_rtk_rg_output (output_utils.py 161-239): protocol lines are removed
// (including the blank line that follows the header) and collected into `meta`;
// real lines keep their order and content verbatim. Unknown lines pass through
// - the parser is deliberately tolerant of rtk version skew (plans/grep.md §8).
tool_status parse_rtk_rg_output(kimix::span<const kimix::string> lines,
                                kimix::vector<kimix::string> &cleaned, rtk_meta &meta);

// _rtk_fold_note (grep_local.py 658-695): the fold summary message fragment.
// An empty `out` mirrors Python's None (no fold markers). `original_path`
// (rg's raw output file, when kept) is appended with '\\' -> '/'.
void rtk_fold_note(const rtk_meta &meta, kimix::string_view original_path, kimix::string &out);

// ---------------------------------------------------------------------------
// 5. Recorder (grep_recorder.py)
// ---------------------------------------------------------------------------

// RECORDER_CAP (grep_recorder.py 25) - session persistence itself stays in
// Python (session.custom_data["grep"]["files"]).
inline constexpr size_t k_recorder_cap = 500;

// FileRecorder.record (37-42) over an existing list: skip empty/duplicate.
void recorder_record(kimix::vector<kimix::string> &existing, kimix::string_view path);

// _merged (52-60) + the record_grep_files overflow cap (81-82): insertion
// order preserved (existing first), empties dropped, then keep the LAST `cap`
// entries when over budget.
void recorder_merge(kimix::span<const kimix::string> existing, kimix::span<const kimix::string> fresh,
                    size_t cap, kimix::vector<kimix::string> &out);

// ---------------------------------------------------------------------------
// 6. Sensitive files (utils/sensitive.py)
// ---------------------------------------------------------------------------

// PurePosixPath(path).name: split on '/', drop empty and "." components, keep
// the last one ("" when nothing is left).
void posix_basename(kimix::string_view path, kimix::string &out);

// PureWindowsPath(path).name: '/' and '\\' both separate; a path whose drive
// is also its root ("C:", "C:\", "\", "\\server\share") yields "". ASCII-gated
// in the caller (the Python flavour is unicode-cased).
void windows_basename(kimix::string_view path, kimix::string &out);

// is_sensitive_file(path): SENSITIVE_EXEMPTIONS first (case-sensitive
// membership test), then the SENSITIVE_PATTERNS table - '/'-bearing patterns
// are path tests (path.endswith(pattern) or ("/" + pattern) in path), the rest
// are fnmatch against the basename. `on_windows` selects both the pathlib
// flavour and fnmatch's normcase (ntpath lower-cases and flips '/' to '\\';
// posixpath is the identity), so the native answer matches the reference on
// either host. Non-ASCII -> unsupported with out == false.
tool_status is_sensitive_path(kimix::string_view path, bool on_windows, bool &out) noexcept;

// sensitive_file_warning: "Skipped N sensitive file(s) (a, b) to protect
// secrets. These files may contain credentials or private keys." with distinct
// sorted basenames capped at 5 plus ", ... (M files total)". Non-ASCII ->
// unsupported and `out` is left untouched.
tool_status sensitive_file_warning(kimix::span<const kimix::string> paths, bool on_windows,
                                   kimix::string &out);

// ---------------------------------------------------------------------------
// 7. Pattern kernels (grep_local.py 106-138)
// ---------------------------------------------------------------------------
// Already ported in src/runtime/tools/grep_pattern.{h,cpp}, which belongs to
// the runtime_py target that kimix-llm does not link - re-declared here inside
// this namespace (the .cpp reproduces the odd-backslash scanner). ASCII-gated:
// non-ASCII patterns must be routed to the Python mirror by the shim.

tool_status pattern_has_regex_newline(kimix::string_view pattern, bool &out) noexcept;
tool_status multiline_pattern(kimix::string_view pattern, kimix::string &out);

// ---------------------------------------------------------------------------
// 8. Byte-budget join
// ---------------------------------------------------------------------------

// _join_with_byte_limit (grep_local.py 620-634): join with '\n', stopping as
// soon as the accumulated UTF-8 size reaches `max_bytes` (the line that
// crossed the budget IS kept, like Python). `truncated` / `omitted` mirror the
// Python return tuple. Returns false - leaving `out` untouched - when any line
// is not valid UTF-8: Python's len(line.encode("utf-8")) raises there, so the
// shim must take over.
bool join_with_byte_limit(kimix::span<const kimix::string> lines, size_t max_bytes, kimix::string &out,
                          bool &truncated, size_t &omitted);

// ===========================================================================
// 9. Regex line search (BLOCKED — plans/grep.md §3 kernel 7, Phase B)
// ===========================================================================

// One regex line hit (the scan_lines-style contract): 0-based line_index plus
// the byte_offset/line_len of the covered line. Multiline spans would expand
// to every covered line in the PCRE2 implementation.
struct grep_hit {
    uint32_t line_index = 0;
    uint32_t byte_offset = 0;
    uint32_t line_len = 0;
};

// grep_search_lines — the native regex matcher entry point.
// **BLOCKED**: byte-exact Python `regex` semantics need PCRE2 (lookaround,
// backreferences, atomic groups, `\p{...}`), which is not vendored in src/ext
// and must not be added (see issue/grep.md). std::regex (ECMAScript-only) and
// RE2 (no lookaround/backrefs) were rejected by the plan for parity reasons.
// This stub therefore ALWAYS returns tool_status::unsupported so the Python
// shim keeps the exact Python matcher — never a silent substitute.
tool_status grep_search_lines(kimix::string_view content, kimix::string_view pattern,
                              bool ignore_case, bool dotall, kimix::vector<grep_hit> &out);

} // namespace kimix::builtin_tools::grep
