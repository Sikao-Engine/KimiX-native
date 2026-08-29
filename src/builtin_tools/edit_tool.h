// edit_tool.h - Multi-mode edit kernels (kimix::builtin_tools::edit).
//
// C++ port of the kimi-agent `edit` built-in agent tool, per
// C:/dev/kimi-agent/plans/edit.md (design sections 3.1-3.6, 7, 8). Python source
// of truth (paths relative to C:/dev/kimi-agent/kimi-cli/src/kimi_cli):
//   tools/file/edit/__init__.py            (1-107)  dispatcher helpers
//   tools/file/edit/modes/replace.py       (55-223) replace kernels
//   tools/file/edit/diff.py                (38-344) unified-diff kernels
//   tools/file/edit/modes/sloppy.py        (55-239) sloppy kernels
//   tools/file/edit/modes/hashline.py      (88-298) hashline grammar parser
//   tools/file/hash_line.py                (55-87, 250-553) line hash + apply
//   tools/file/snapshot_store.py           (62-64)  detect_line_ending
//
// Everything is a pure, stateless CPU kernel: no filesystem, no subprocess,
// no Python includes. Errors are returned as data (tool_error), never thrown.
// Non-ASCII text is handled on the code-point level (fuzz_ratio DP, strip,
// line splitting) so results stay byte-exact with the Python reference for
// UTF-8 content; the conflict-marker scan is NOT declared here (ownership map
// in src/builtin_tools/README.md assigns it to `write`).
//
// Conventions: namespace kimix::builtin_tools::edit, snake_case functions,
// _snake_case members, fixed-width integers, kimix:: containers/strings.
// Unity-build safe: every symbol lives in the nested `edit` namespace.

#pragma once

#include <cstdint>
#include <utility>

#include <core/kimix_core.h>

#include "builtin_tools/tool_types.h"
#include "builtin_tools/tool.h"

namespace kimix::builtin_tools::edit {

// ===========================================================================
// 1. Common newline / line helpers
// ===========================================================================

// replace.py::_normalize_line_endings (55-57): "\r\n" -> "\n" only.
kimix::string normalize_newlines(kimix::string_view text);

// diff.py:40 / sloppy.py:57 / hashline.py:257: "\r\n" -> "\n", then "\r" -> "\n".
kimix::string normalize_breaks(kimix::string_view text);

// Python str.split("\n") semantics: a trailing '\n' yields a trailing empty
// element, and "".split("\n") == [""]. Used by the diff apply path.
void split_lf(kimix::string_view text, kimix::vector<kimix::string> &out);

// Python str.splitlines() semantics: splits on \n, \r\n, \r, \v, \f,
// \x1c, \x1d, \x1e, U+0085, U+2028, U+2029; no trailing empty element for a
// final line break. Used by replace/hashline/sloppy kernels.
void split_lines(kimix::string_view text, kimix::vector<kimix::string> &out);

// Python str.splitlines(keepends=True): like split_lines but each element
// keeps its line terminator (including the "\r\n" pair).
void split_lines_keepends(kimix::string_view text,
                          kimix::vector<kimix::string> &out);

// "\n".join(lines).
kimix::string join_lf(kimix::span<const kimix::string> lines);

// snapshot_store.detect_line_ending (62-64): "\r\n" if any "\r\n" present,
// else "\n".
kimix::string detect_line_ending(kimix::string_view content);

// diff.py:342-343: if `ended_with_newline` and the result does not end with
// '\n', append one.
void restore_trailing_newline(kimix::string &result, bool ended_with_newline);

// hash_line.py:551-552: same but only when the result is non-empty.
void restore_trailing_newline_nonempty(kimix::string &result,
                                       bool ended_with_newline);

// Python str.strip() (Unicode whitespace set; exact for the code points the
// line-hash kernel treats as whitespace). Returns a copy.
kimix::string py_strip(kimix::string_view text);

// Python repr() for error-message interpolation ({...!r}). Exact for ASCII
// text (escapes \\, \n, \r, \t, \xNN control bytes; quote choice follows
// Python); printable non-ASCII bytes are kept verbatim.
kimix::string py_repr(kimix::string_view text);

// ===========================================================================
// 2. fuzz_ratio (rapidfuzz fuzz.ratio / Indel normalized similarity)
// ===========================================================================

// Length gate for the Levenshtein/Indel DP. fuzz_ratio and every kernel that
// scores candidates return tool_status::too_large when a string exceeds the
// per-string code-point cap or the DP cell budget; the caller (Python shim)
// falls back to the pure-Python mirror for such inputs.
inline constexpr size_t k_fuzz_max_len = 10000;        // code points per string
inline constexpr size_t k_fuzz_max_cells = 25'000'000; // len_a * len_b budget

struct fuzz_ratio_result {
    tool_status status = tool_status::ok;
    double score = 0.0; // 0..100, rapidfuzz float scale
};

// rapidfuzz.fuzz.ratio(a, b) = 100 * (1 - indel_dist / (len_a + len_b)) where
// indel_dist is the Levenshtein distance with substitution cost 2 (equivalently
// (len_a + len_b) - 2*LCS). Common prefix/suffix are trimmed before the DP
// (the distance is invariant); the denominator always uses the ORIGINAL
// lengths. Non-ASCII inputs are decoded to code points first. Empty strings:
// ratio("","")=100, ratio("", x)=0.
fuzz_ratio_result fuzz_ratio(kimix::string_view a, kimix::string_view b);

// ===========================================================================
// 3. Replace kernels (modes/replace.py)
// ===========================================================================

// ReplaceEditItem (params.py:13-41). match_mode is "exact" or "fuzzy"
// (Python default "fuzzy").
struct replace_edit_item {
    kimix::string old_text;
    kimix::string new_text;
    bool replace_all = false;
    kimix::optional<size_t> max_replacements;
    kimix::string match_mode = "fuzzy";
};

// _apply_edit (197-223) / _apply_replace_all (146-171) /
// _apply_fuzzy_fallback (173-195) result: (content, replacements, suggestion).
struct replace_result {
    kimix::string content;
    size_t replacements = 0;
    kimix::optional<kimix::string> suggestion; // "fuzzy-matched at NN%: '...'"
    tool_error error;
};

// Optional-text result for _find_similar / _try_strip_match.
struct optional_text_result {
    kimix::optional<kimix::string> text;
    tool_error error;
};

// Best fuzzy match (matched original text + score) for _find_best_fuzzy_match.
struct fuzzy_match_result {
    kimix::optional<kimix::string> matched_original;
    double score = 0.0;
    tool_error error;
};

// replace.py::_apply_edit (197-223): full single-edit chain including the
// replace-all / strip / fuzzy fallbacks. Mirror of
// (content, replacements, suggestion) with error propagation.
replace_result apply_edit(kimix::string_view content,
                          const replace_edit_item &edit);

// replace.py::_find_similar (59-86): extract-one over lines, then over
// line-windows (first-best tie-break), returning the matched text when its
// score >= cutoff.
optional_text_result find_similar(kimix::string_view target,
                                  kimix::string_view content,
                                  double cutoff = 75.0);

// replace.py::_try_strip_match (88-109): find old.strip() inside any line
// (keeping the line's original terminator) and splice once.
optional_text_result try_strip_match(kimix::string_view content,
                                     kimix::string_view old_text,
                                     kimix::string_view new_text);

// replace.py::_find_best_fuzzy_match (111-144): score each line (single-line
// target) or each line-window (multi-line target), strict > update, and
// return the best original-text variant with score >= cutoff.
fuzzy_match_result best_fuzzy_match(kimix::string_view target,
                                    kimix::string_view content,
                                    double cutoff = 75.0);

// ===========================================================================
// 4. Unified-diff kernels (edit/diff.py)
// ===========================================================================

enum class hunk_line_kind : uint8_t { context = 0,
                                      add = 1,
                                      deleted = 2 };

struct hunk_line {
    hunk_line_kind kind = hunk_line_kind::context;
    kimix::string text;
};

struct diff_hunk {
    kimix::optional<int32_t> start_line; // 1-based; nullopt = unknown
    kimix::optional<kimix::string> change_context;
    kimix::vector<hunk_line> lines;
};

// diff.py::normalize_diff (38-57).
kimix::string normalize_diff(kimix::string_view diff);

// diff.py::normalize_create_content (60-74).
kimix::string normalize_create_content(kimix::string_view diff);

struct hunks_result {
    kimix::vector<diff_hunk> hunks;
    tool_error error;
};

// diff.py::parse_diff_hunks (77-168). Errors carry the exact ApplyPatchError
// message ("Unexpected diff content outside a hunk: ...").
hunks_result parse_diff_hunks(kimix::string_view diff);

struct diff_apply_result {
    kimix::string content;
    kimix::optional<int32_t> first_changed_line; // 1-based, nullopt if unchanged
    tool_error error;
};

// diff.py::apply_diff_hunks (276-344): bottom-up (start_line or 0) desc stable
// order, exact match first, fuzzy line-window fallback (threshold, dominance
// gap 0.05), inferred indentation adjustment for added lines, trailing-newline
// restore. Errors carry the exact ApplyPatchError messages.
diff_apply_result apply_diff_hunks(kimix::span<const diff_hunk> hunks,
                                   kimix::string_view content,
                                   bool allow_fuzzy = true,
                                   double threshold = 0.75);

// Focused kernels (also used by apply_diff_hunks; exposed for tests).
// diff.py::_hunk_pattern (171-173): lines with kind != add.
void hunk_pattern(const diff_hunk &hunk, kimix::vector<kimix::string> &out);

// diff.py::_find_exact_matches (243-251): empty pattern -> [0].
void find_exact_matches(kimix::span<const kimix::string> original_lines,
                        kimix::span<const kimix::string> pattern,
                        kimix::vector<int32_t> &out);

// diff.py::_find_fuzzy_match (254-273): unique fuzzy window >= threshold with
// a >= 0.05 dominance gap, else nullopt.
struct fuzzy_location_result {
    kimix::optional<int32_t> index;
    tool_error error;
};
fuzzy_location_result find_fuzzy_match(
    kimix::span<const kimix::string> original_lines,
    kimix::span<const kimix::string> pattern, double threshold);

// diff.py::_count_leading_whitespace (176-186).
void count_leading_whitespace(kimix::string_view line, int32_t &spaces,
                              int32_t &tabs);

// diff.py::_infer_indent_adjustment (189-216): most-common (spaces) delta and
// majority indent char; returns (delta, indent_char).
void infer_indent_adjustment(kimix::span<const kimix::string> pattern_lines,
                             kimix::span<const kimix::string> actual_lines,
                             int32_t &delta, kimix::string &indent_char);

// diff.py::_apply_indent (219-226).
kimix::string apply_indent(kimix::string_view line, int32_t delta,
                           kimix::string_view indent_char);

// ===========================================================================
// 5. Hashline kernels (modes/hashline.py + hash_line.py)
// ===========================================================================

// hash_line.py::compute_line_hash (55-69): 2-char xxHash32 nibble hash with
// cumulative chaining. prev_hash is the previous line's 2-char hash (or
// nullopt for line 1).
kimix::string compute_line_hash(int32_t line_num, kimix::string_view line,
                                kimix::optional<kimix::string_view> prev_hash);

// Chained hashes for `lines` (1-based line numbers); out is cleared first.
void compute_line_hashes(kimix::span<const kimix::string> lines,
                         kimix::vector<kimix::string> &out);

// hashline.py grammar (128-298).
enum class hashline_op_kind : uint8_t { put = 0,
                                        cut = 1,
                                        rem = 2,
                                        mv = 3 };

enum class insert_where : uint8_t { replace = 0,
                                    before = 1,
                                    after = 2 };

struct hashline_op {
    hashline_op_kind kind = hashline_op_kind::put;
    kimix::string line_text; // raw op line (stripped)
    kimix::optional<int32_t> start;
    kimix::optional<int32_t> end;
    insert_where insert_where_ = insert_where::replace;
    kimix::optional<kimix::string> register_; // "@name" reference
    kimix::vector<kimix::string> body;        // "+"-prefixed body rows
    kimix::optional<kimix::string> dest;      // MV destination
};

struct hashline_section {
    kimix::string path;
    kimix::string tag;
    kimix::vector<hashline_op> ops;
};

struct parse_hashline_result {
    kimix::vector<hashline_section> sections;
    tool_error error;
};

// hashline.py::parse_hashline_input (255-298) incl. _parse_section_body
// (128-252): [path#tag] sections, PUT/CUT/REM/MV ops, "+" body rows,
// register refs, *** preamble/postamble trimming.
parse_hashline_result parse_hashline_input(kimix::string_view input);

// hash_line.py models: a fully-resolved edit with hash anchors. op is
// "replace" | "append" | "prepend" | "delete" (delete is normalized to a
// replace with an empty body internally, exactly like _normalize_edit).
struct anchor_ref {
    int32_t line = 0;
    kimix::string hash;
};

struct hashline_edit {
    kimix::string op = "replace";
    kimix::optional<anchor_ref> pos;
    kimix::optional<anchor_ref> end;
    kimix::vector<kimix::string> lines;
};

struct hash_mismatch {
    int32_t line = 0;
    kimix::string expected;
    kimix::string actual;
};

struct apply_hashline_result {
    kimix::string content;
    kimix::optional<int32_t> first_changed_line;
    // When hash validation fails, the structured mismatch payload is filled
    // and error.status == invalid_input with the HashlineMismatchError
    // __str__ text in error.message (the shim can distinguish the two error
    // classes by checking mismatches.empty()).
    kimix::vector<hash_mismatch> mismatches;
    kimix::vector<kimix::string> file_lines;
    tool_error error;
};

// hash_line.py::apply_hashline_edits (349-553): normalize CRLF, validate
// anchors (exact + CR-stripped fuzzy fallback), dedupe identical edits,
// overlap detection, stable bottom-up apply, first-changed tracking,
// trailing-newline restore.
apply_hashline_result apply_hashline_edits(
    kimix::string_view content, kimix::span<const hashline_edit> edits);

// hash_line.py::HashlineMismatchError.__str__ (212-247).
kimix::string hashline_mismatch_message(
    kimix::span<const hash_mismatch> mismatches,
    kimix::span<const kimix::string> file_lines);

// ===========================================================================
// 6. Sloppy kernels (modes/sloppy.py)
// ===========================================================================

struct sloppy_inline {
    kimix::string old_text;
    kimix::string new_text;
};

struct sloppy_inline_line {
    kimix::string line;
    kimix::vector<sloppy_inline> selections;
};

struct sloppy_op {
    kimix::string path;
    bool all_match = false;
    kimix::vector<kimix::string> match_lines;
    // Engaged (has value) == block rewrite (MATCH >> REWRITE); nullopt ==
    // inline mode.
    kimix::optional<kimix::vector<kimix::string>> rewrite_lines;
    kimix::vector<sloppy_inline_line> inline_lines;
};

struct parse_sloppy_result {
    kimix::vector<sloppy_op> ops;
    tool_error error;
};

// sloppy.py::parse_sloppy_input (123-137) + _split_sections (55-70) +
// _parse_op (73-120): section-sign sections, section-star all-match flag, >>-separated block
// rewrites, <<old|new>> inline selections with remaining-string rescan.
parse_sloppy_result parse_sloppy_input(kimix::string_view input);

struct sloppy_apply_result {
    kimix::string content;
    tool_error error;
};

// sloppy.py::_apply_block_op (176-213): exact then fuzzy block locate,
// pure-deletion trailing-newline swallow, all_match non-overlapping loop.
sloppy_apply_result apply_block_op(kimix::string_view content,
                                   const sloppy_op &op);

// sloppy.py::_apply_inline_op (216-239): per-selection replace (all or first).
sloppy_apply_result apply_inline_op(kimix::string_view content,
                                    const sloppy_op &op);

// Dispatch: block mode when rewrite_lines is engaged, else inline mode.
sloppy_apply_result apply_sloppy_op(kimix::string_view content,
                                    const sloppy_op &op);

// Focused kernels (exposed for tests).
// sloppy.py::_find_exact_block (140-148): byte range of the joined block.
kimix::optional<byte_range> find_exact_block(
    kimix::string_view content, kimix::span<const kimix::string> block_lines);

// sloppy.py::_find_fuzzy_block (151-173): line-window fuzzy locate with
// character-range conversion (incl. +1 per line terminator).
struct fuzzy_block_result {
    kimix::optional<byte_range> range;
    tool_error error;
};
fuzzy_block_result find_fuzzy_block(
    kimix::string_view content, kimix::span<const kimix::string> block_lines,
    double threshold = 0.75);

// ===========================================================================
// 7. Tool class and standard integration (plan §3.9)
// ===========================================================================
//
// Thin Tool subclass so the Python orchestration layer can drive the edit
// kernels through the same binding path as other built-in tools. The C++ side
// owns only the deterministic editing kernels; path resolution, VFS I/O,
// approval, snapshots, and display generation stay in Python. The caller must
// pass the content to edit as the "content" parameter; the result is
// serialized into a JSON object and exposed through last_result().
//
// Supported parameter schema (all keys live in the ToolParams values map):
//   mode              : "replace" | "patch" | "hashline" | "sloppy" (required)
//   content           : string to edit (required)
// replace mode:
//   edits / edit      : array of replace-edit objects, OR top-level
//   old_string / old  : shorthand old text
//   new_string / new  : shorthand new text
//   replace_all       : bool (default false)
//   max_replacements  : positive int (optional)
//   match_mode        : "exact" | "fuzzy" (default "fuzzy")
// patch mode:
//   diff              : unified-diff hunk text
// hashline mode:
//   edits / edit      : array of fully-resolved hashline_edit objects
// sloppy mode:
//   input             : sloppy-mode source text
//
// Result object (always present, even on error):
//   status            : "ok" or the tool_status name
//   message           : diagnostic (empty on success)
//   content           : edited text (may be original on error)
// replace extras:
//   replacements      : int
//   suggestion        : string or null
// patch / hashline extras:
//   first_changed_line: int or null

class Edit : public kimix::builtin_tools::Tool {
public:
    explicit Edit(kimix::builtin_tools::Session *session);
    void operator()(kimix::builtin_tools::ToolParams const *parameters) override;

    // Serialized result of the last operator() call. Empty if operator() has
    // never been called. The returned object is stable until the next call.
    kimix::builtin_tools::ToolParams const &last_result() const noexcept;

private:
    kimix::builtin_tools::ToolParams _result;
};

} // namespace kimix::builtin_tools::edit
