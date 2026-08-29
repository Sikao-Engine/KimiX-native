// write_tool.h - Built-in agent tool "write" kernels (kimix::builtin_tools::write).
//
// Plan: D:/KimiX-native/plans/write.md (\u00a73 C++ design, \u00a73.4 tool class, \u00a77 tests).
// Python source of truth (mirror byte-exact messages and decisions):
// D:/kimi-agent/kimi-cli/src/kimi_cli/tools/file/write.py (Params 82-155, __call__ 212-510,
// _conflict_guard 512-608,
// conflict resolution 610-836)
// D:/kimi-agent/kimi-cli/src/kimi_cli/tools/file/auto_generated.py (patterns 34-63, comment styles 65-131,
// extract/detect 117-227, error 230-241)
// D:/kimi-agent/kimi-cli/src/kimi_cli/tools/file/conflict_detect.py  (markers 55-63, match 127-156,
// scan 162-344, splice 523-675,
// region semantics 682-696, uri 443-488,
// bulk 844-864)
// D:/kimi-agent/kimi-cli/src/kimi_cli/tools/file/check_fmt.py (check_json_text 10-28)
// D:/kimi-agent/kimi-cli/src/kimi_cli/utils/diff.py (format_unified_diff 23-83)
//
// This pair implements the write-tool guards that are CPU-bound / correctness
// critical: strict UTF-8 validation, the auto-generated-file guard, the
// conflict-marker scan + splice (write OWNS these symbols per the
// builtin_tools ownership map), JSON format validation via vendored yyjson,
// the mkdir decision, the deterministic unified-diff emitter, and the
// post-write size/verification message kernels.  Session state (approval,
// snapshots, ConflictHistory ids, FS-cache, edit-parse guard) stays in
// Python per plan §3.6.
//
// All kernels are pure C++ (no Python includes), noexcept where they cannot
// fail, and deterministic: no file-system access inside a kernel; I/O effects
// are injected as plain data (e.g. `parent_exists`, `create_error`).
//
// ASCII gate: marker/comment scanners implement the reference `regex`
// patterns with ASCII \s = [ \t\n\r\f\v], \w = [A-Za-z0-9_],
// \b = ASCII word boundary (same strategy as security.cpp / grep_pattern).
// Callers route non-ASCII header text to the Python mirror when parity is
// required; see the report for the boundary semantics.

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include <core/kimix_core.h>

#include "builtin_tools/tool.h"
#include "builtin_tools/tool_types.h"

namespace kimix::builtin_tools::write {

// ---------------------------------------------------------------------------
// 1. UTF-8 strict validation wrapper
// ---------------------------------------------------------------------------

// Strict UTF-8 check of `bytes` reusing utf8_util::utf8_strict_error (CPython
// DecoderError wording + 0-based byte offset).  Returns nullopt when valid,
// otherwise the write-specific diagnostic:
//     "utf-8 decoding error: <reason>"
// where <reason> is one of "invalid start byte", "invalid continuation byte",
// "unexpected end of data" (the exact CPython utf-8 codec wording).
kimix::optional<kimix::string> utf8_decode_error(kimix::string_view bytes) noexcept;

// Expected post-write size in bytes, mirroring
// len(old_text.encode("utf-8","strict")) + len(content.encode(...)) (append)
// or len(new_text.encode("utf-8","strict")) (overwrite).  Returns nullopt when
// either relevant text is invalid UTF-8 (the Python path would raise
// UnicodeDecodeError before writing).
kimix::optional<uint64_t> expected_write_size(bool append,
                                              kimix::string_view old_text,
                                              kimix::string_view content,
                                              kimix::string_view new_text) noexcept;

// ---------------------------------------------------------------------------
// 2. Auto-generated-file guard (auto_generated.py)
// ---------------------------------------------------------------------------

inline constexpr size_t k_check_byte_count = 1024; // CHECK_BYTE_COUNT
inline constexpr size_t k_header_line_limit = 40;  // HEADER_LINE_LIMIT

// Comment style ids (auto_generated.py _EXTENSION_COMMENT_STYLES /
// _BASENAME_COMMENT_STYLES): "slash" (// and /* */), "hash" (#), "sql" (--),
// "html" (<!-- -->).
enum class comment_style : uint8_t {
    slash = 0,
    hash = 1,
    sql = 2,
    html = 3,
};

// True when the basename matches one of the 10 AUTO_GENERATED_FILENAME_PATTERNS
// (zz_generated.*, *.pb.go|cc|h|c|js|ts, *_pb2.py, *_pb2_grpc.py,
// *.gen.go|ts|js|py, generated.go|ts|js|py, *.swagger.json,
// *.openapi.json, *.mock.go|ts, *.mocks?/mock.js variants).
bool is_auto_generated_file_name(kimix::string_view file_path) noexcept;

// Per-path comment styles: basename tables (dockerfile/makefile/justfile ->
// hash) win, then the extension table, else all four styles
// (auto_generated.get_comment_styles_for_path).
void get_comment_styles_for_path(kimix::string_view file_path,
                                 kimix::vector<comment_style>& out) noexcept;

// Detect the strong header marker in the first k_check_byte_count bytes
// (auto_generated.detect_auto_generated_marker).  Filename patterns win and
// return the basename.  Otherwise the leading header comment is extracted
// (BOM strip, shebang skip, per-style comment blocks, 40-line cap) and matched
// against the four strong markers; returns the matched text
// (regex match.group(0).strip()) or nullopt.
kimix::optional<kimix::string> detect_auto_generated_marker(
    kimix::string_view content, kimix::string_view file_path) noexcept;

// Exact refusal message from auto_generated.build_auto_generated_error:
//     Cannot modify auto-generated file: {display_path}
//
//     This file appears to be automatically generated (detected marker: "{detected}"). ...
kimix::string build_auto_generated_error(kimix::string_view display_path,
                                         kimix::string_view detected) noexcept;

// ---------------------------------------------------------------------------
// 3. Conflict-marker scan + splice (conflict_detect.py) — write OWNS these
// ---------------------------------------------------------------------------

inline constexpr kimix::string_view k_ours_prefix = "<<<<<<<";
inline constexpr kimix::string_view k_base_prefix = "|||||||";
inline constexpr kimix::string_view k_separator = "=======";
inline constexpr kimix::string_view k_theirs_prefix = ">>>>>>>";
inline constexpr int32_t k_echo_trim_limit = 12;    // ECHO_TRIM_LIMIT
inline constexpr int32_t k_preview_side_lines = 6;  // PREVIEW_SIDE_LINES

// One fully-closed conflict marker block (conflict_detect.ConflictBlock).
// Lines are LF-normalized (trailing \r stripped).  `base_line` is -1 for a
// 2-way conflict; `base_lines` is empty for 2-way too.
struct conflict_block {
    int32_t start_line = 1;      // 1-indexed <<<<<<<
    int32_t separator_line = 0;  // 1-indexed =======
    int32_t end_line = 0;        // 1-indexed >>>>>>>
    int32_t base_line = -1;      // 1-indexed |||||||, -1 when 2-way
    kimix::optional<kimix::string> ours_label;
    kimix::optional<kimix::string> base_label;
    kimix::optional<kimix::string> theirs_label;
    kimix::vector<kimix::string> ours_lines;
    kimix::vector<kimix::string> base_lines;
    kimix::vector<kimix::string> theirs_lines;
};

// A registered block with a session-scoped id and display path
// (conflict_detect.ConflictEntry).  Ids are assigned by the Python-side
// ConflictHistory; the kernel only carries them for messages.
struct conflict_entry : conflict_block {
    int32_t id = 0;
    kimix::string absolute_path;
    kimix::string display_path;
};

// Result of splicing a resolved region back into file text
// (conflict_detect.ConflictSplice).
struct conflict_splice_result {
    kimix::string text;
    int32_t trimmed_leading = 0;
    int32_t trimmed_trailing = 0;
};

// A dangling opener line (unclosed <<<<<<< before EOF).
struct dangling_opener {
    int32_t line = 0;
    kimix::string marker_line;
};

// Parsed conflict:// URI (conflict_detect.ParsedConflictUri).
struct parsed_conflict_uri {
    int64_t id = 0;                       // valid when !is_star
    bool is_star = false;                 // id == "*"
    kimix::string scope;                  // "ours" | "theirs" | "base" | "" (none)
    kimix::optional<kimix::string> recovered_prefix; // "<file>:" prefix cleanup note
};

// Strict column-0 marker match (conflict_detect.match_marker): returns nullopt
// for non-markers, an empty string for the bare prefix, and the label string
// for "prefix + single space + label" (label must not itself start with a
// space).  A trailing \r is stripped first (CRLF checkouts).
kimix::optional<kimix::string> match_marker(kimix::string_view line,
                                            kimix::string_view prefix) noexcept;

// The ======= separator matches exactly (no label variant).
bool is_separator(kimix::string_view line) noexcept;

// State-machine scan (idle -> ours -> base -> theirs) of whole-file `content`.
// Input contract = scan_file_for_conflicts: lines are split on '\n' (no
// CRLF normalization; each line's trailing \r is stripped while matching).
// Only fully-closed blocks are returned; malformed sequences reset the
// partial block to idle; nested openers restart; unclosed tail is dropped.
void scan_conflict_blocks(kimix::string_view content,
                          kimix::vector<conflict_block>& out) noexcept;

// Lines holding an unclosed opener (file ends inside a conflict block).
// Call-site contract (write.py _conflict_guard): `content` is first
// CRLF-normalized then split with Python splitlines() semantics, matching
// find_dangling_openers(old_lines) where old_lines come from splitlines().
void find_dangling_openers(kimix::string_view content,
                           kimix::vector<dangling_opener>& out) noexcept;

// Replace the recorded marker region with `replacement`
// (conflict_detect.splice_conflict).  Locates the region by content anchored
// at entry.start_line-1 with nearest-occurrence fallback; trims boundary echo
// (_trim_echo: multi-line echoes always trimmed, single-line only when the
// delimiter balance of ( ) [ ] is 0); re-applies CRLF when the original text
// contained \r\n.  Returns true and fills `out` on success.  Returns false
// and fills `error` with the exact ConflictError text
// ("Conflict #{id} no longer matches the recorded block at {display_path}:{start_line}. ...")
// when the recorded block is gone or altered.
bool splice_conflict(kimix::string_view original_text,
                     const conflict_entry &entry,
                     kimix::string_view replacement,
                     conflict_splice_result &out,
                     kimix::string &error) noexcept;

// Expand @ours / @theirs / @base / @both line tokens
// (conflict_detect.expand_content_tokens).  Returns nullopt on success and
// fills `out`; otherwise returns the exact ConflictError message (@base on a
// 2-way conflict).
kimix::optional<kimix::string> expand_content_tokens(
    kimix::string_view content, const conflict_entry &entry,
    kimix::string &out) noexcept;

// Region equality (conflict_detect.conflict_regions_equal): same start/end
// line and identical rendered region text.
bool conflict_regions_equal(const conflict_block &a,
                            const conflict_block &b) noexcept;

// True when the rendered region of `entry` appears verbatim in `content`
// (CRLF-normalized substring check, conflict_detect.conflict_region_present).
bool conflict_region_present(kimix::string_view content,
                             const conflict_block &entry) noexcept;

// Render one side (or the whole region when `scope` is empty) of a block
// (conflict_detect.render_conflict_region).  Fills `out_lines` and the
// 1-based `start_line`; returns nullopt on success or the exact ConflictError
// message (2-way @base / unknown scope).
kimix::optional<kimix::string> render_conflict_region(
    const conflict_entry &entry, kimix::string_view scope,
    kimix::vector<kimix::string> &out_lines, int32_t &start_line) noexcept;

// One-line-per-block index for the `:conflicts` selector
// (conflict_detect.format_conflict_summary).  Exact message text incl. the
// ⚠ header, label lines, byte-cap note and NOTICE footer.
kimix::string format_conflict_summary(
    const kimix::vector<conflict_entry> &entries,
    kimix::string_view display_path, bool scan_truncated) noexcept;

// Parse a conflict:// URI (conflict_detect.parse_conflict_uri).  Returns
// nullopt for non-conflict paths; fills `out` on success; returns the exact
// ConflictError message for a well-formed scheme with an invalid id/scope.
kimix::optional<kimix::string> parse_conflict_uri(
    kimix::string_view raw, parsed_conflict_uri &out) noexcept;

// Parse per-id "<id>: @side" bulk directives
// (conflict_detect.parse_bulk_directives).  Returns true and fills `out`
// (id -> side) when every non-empty line is a directive and at least one
// directive exists; false otherwise (content applies to every registered
// entry instead).
bool parse_bulk_directives(
    kimix::string_view content,
    kimix::vector<std::pair<int32_t, kimix::string>> &out) noexcept;

// Mode-aware conflict-marker write guard (write.py _conflict_guard).
// Refusal error (tool_status::blocked message text) when markers must not be
// written; otherwise an optional warning note and whether the old text had
// blocks.  Old/new text line splitting replicates the write.py call site:
// replace("\r\n","\n").splitlines().
struct conflict_guard_result {
    kimix::optional<kimix::string> error; // refusal message when set
    kimix::string note;                   // warning note ("" when none)
    bool old_had_blocks = false;
};
conflict_guard_result run_conflict_guard(kimix::string_view display_path,
                                         kimix::string_view old_text,
                                         kimix::string_view new_content,
                                         bool append, bool file_existed,
                                         bool allow_conflicts) noexcept;

// Exact refusal text of write.py _conflict_markers_error.
kimix::string build_conflict_markers_error(
    kimix::string_view display_path,
    const kimix::vector<conflict_block> &blocks) noexcept;

// Exact dangling-opener refusal text (write.py _conflict_guard append path).
kimix::string build_dangling_opener_error(
    kimix::string_view display_path,
    const kimix::vector<dangling_opener> &dangling) noexcept;

// ---------------------------------------------------------------------------
// 4. JSON format validation (check_fmt.py check_json_text, vendored yyjson)
// ---------------------------------------------------------------------------

// Parse `text` with yyjson (stop-on-error).  Returns nullopt when valid;
// otherwise an orjson-style message:
//     JSON decode error at line <N>, column <M>: <msg>
// where N/M are 1-based, M counts code points (orjson colno semantics) and
// <msg> is yyjson's error wording (decision parity with orjson is exact;
// message wording is best-effort aligned — see plan §8 risk row).  The empty
// document case is special-cased to orjson's exact wording.
kimix::optional<kimix::string> check_json_format(kimix::string_view text) noexcept;

// Format validation dispatch by extension (write.py __call__ 290-300).
//   .json             -> check_json_format; fmt_error set when invalid
//   .yaml/.yml/.toml/.xml -> tool_status::unsupported (no vendored parsers;
//                        Python-side validation stays, plan §3.4/§3.6)
//   anything else     -> tool_status::ok (no format gate)
// Returns ok (with fmt_error possibly set) when the write may proceed.
tool_status validate_format_by_path(kimix::string_view file_path,
                                    kimix::string_view text,
                                    kimix::string &fmt_error) noexcept;

// ---------------------------------------------------------------------------
// 5. mkdir / diff / post-write verification kernels
// ---------------------------------------------------------------------------

// Parent-directory decision (write.py __call__ 246-261).  ok = proceed
// (parent exists, or mkdir requested and creation succeeded — `create_error`
// is nullopt).  not_found = mkdir=False and parent missing (exact refusal
// "Parent directory does not exist: ...").  invalid_input = mkdir failed
// (exact refusal "Failed to create parent directory for ...: {e}").
struct parent_dir_decision {
    tool_status status = tool_status::ok;
    kimix::string message;
};
parent_dir_decision decide_parent_dir(
    bool parent_exists, bool mkdir, kimix::string_view display_path,
    kimix::string_view parent_path,
    kimix::optional<kimix::string> create_error) noexcept;

// Deterministic unified diff (difflib-compatible; port of the
// runtime/diff/diff_engine semantics, which mirrors utils/diff.py
// format_unified_diff): splitlines(keepends=true), last-line trailing-newline
// fix, SequenceMatcher(autojunk=False) opcodes, 3 context lines, "--- a/…" /
// "+++ b/…" headers and "@@ -l,c +l,c @@" ranges.  Returns "" when identical.
kimix::string build_unified_diff(kimix::string_view old_text,
                                 kimix::string_view new_text,
                                 kimix::string_view path,
                                 bool include_file_header) noexcept;

// Exact post-write verification failure messages (write.py 404-420).
kimix::string verification_failed_error(kimix::string_view display_path,
                                        kimix::string_view reason,
                                        bool outside) noexcept;
kimix::string size_mismatch_error(kimix::string_view display_path,
                                  uint64_t expected, uint64_t actual,
                                  bool outside) noexcept;

// Success message composition (write.py 476-488): "File successfully
// {overwritten|appended to}. Current size: {N} bytes. Path: {display_path}"
// + " Verified: size matches." + optional conflict note + optional drift note
// (" Note: ..." with the leading space).
kimix::string success_message(kimix::string_view display_path, uint64_t size,
                              kimix::string_view action_desc,
                              kimix::string_view conflict_note,
                              kimix::string_view drift_note) noexcept;

// Conflict-resolution success message (write.py 689-698) incl. the optional
// boundary-echo note (" Note: dropped N content line(s) ...").
kimix::string conflict_resolved_message(int32_t id, int32_t start_line,
                                        int32_t end_line,
                                        kimix::string_view display_path,
                                        int32_t trimmed_total) noexcept;

// ---------------------------------------------------------------------------
// 6. Tool class and standard integration
// ---------------------------------------------------------------------------

// Concrete Write tool subclass.  The Python binding layer invokes this through
// the shared ToolParams JSON-object contract.  The C++ side owns parameter
// validation and dispatch to the pure kernels (UTF-8 check, auto-generated
// guard, conflict guard, format validation, diff, success-message assembly).
// File I/O, approval, snapshots, FS-cache invalidation, session-conflict
// history, json_repair, and conflict:// orchestration stay in Python per
// plan \u00a73.4/\u00a73.6.
class Write : public kimix::builtin_tools::Tool {
public:
    explicit Write(kimix::builtin_tools::Session *session);

    // Validate parameters, dispatch to the native kernels, and populate the
    // internal result object.  Never throws across the tool boundary.
    void operator()(kimix::builtin_tools::ToolParams const *parameters) override;

    // Access the result object produced by the last operator() invocation.
    kimix::builtin_tools::ToolParams const &last_result() const noexcept;

private:
    kimix::builtin_tools::ToolParams _result;
};

} // namespace kimix::builtin_tools::write
