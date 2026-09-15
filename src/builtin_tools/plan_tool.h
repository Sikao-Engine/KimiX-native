// plan_tool.h - C++ port of the kimi-agent plan-file tools (WritePlan,
// ReadPlan, EditPlan).
//
// Python source of truth (C:/dev/kimi-agent/src/kimix/tools/note/__init__.py):
//   MAX_LINES / MAX_LINE_LENGTH / MAX_BYTES constants      24-26
//   _enable_plan module flag + _set_enable_plan            28-34
//   WritePlanParams (content|text alias, mode)             37-56
//   WritePlan.__call__                                     58-95
//   ReadPlanParams + _validate_line_offset                 98-140
//   ReadPlan.__call__                                      142-176
//   ReadPlan._read_forward                                 177-227
//   ReadPlan._read_tail                                    228-291
//   Edit (old|old_string, new|new_string, replace_all)     294-311
//   EditPlanParams (edit|edits alias, single or list)      313-322
//   EditPlan._normalize_line_endings                       330-332
//   EditPlan._find_similar                                 333-360
//   EditPlan._try_strip_match                              361-386
//   EditPlan._find_best_fuzzy_match                        387-419
//   EditPlan._apply_edit                                   420-461
//   EditPlan.__call__                                      462-517
// Also: kimi_cli/tools/utils.py truncate_line (ported here as
// read::truncate_line_read) and the aiofiles universal-newline line iteration
// (ported as read::split_lines).
//
// Design notes (project conventions):
// * namespace kimix::builtin_tools::plan. kimix-llm builds every
//   src/builtin_tools/*.cpp as one unity translation unit, so every symbol of
//   this tool lives inside this namespace and TU-local helpers carry the
//   `pl_` prefix.
// * kimix:: containers/strings in every public API; no RTTI; kernels never
//   throw across the tool boundary - failures are data (tool_error).
// * The line/byte budget renderers are pure: they take the already-split lines
//   of the plan file, so unit tests need no fixtures. File access happens only
//   inside the Tool wrappers and only when Session::native_io is set.
// * The fuzzy-edit chain deliberately does NOT reuse edit::apply_edit: EditPlan
//   returns no suggestion when a fuzzy match SUCCEEDS (note/__init__.py
//   452-458 returns `(new_content, 1, None)`), while the file-edit tool
//   reports "fuzzy-matched at NN%: '...'". The individual scoring kernels
//   (normalize_newlines / find_similar / try_strip_match / best_fuzzy_match)
//   ARE reused - they are byte-identical ports of the same rapidfuzz calls.
// * Session binding: the plan file is Session::plan_path (Python
//   `session.custom_data['plan_writing_path']`) and the enable gate is
//   Session::plan_enabled (Python module flag `_enable_plan`, whose False
//   state raises SkipThisTool - reported here as tool_status::unsupported).
#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "builtin_tools/tool.h"
#include "builtin_tools/tool_types.h"

namespace kimix::builtin_tools::plan {

// Shared vocabulary (builtin_tools/tool_types.h) - reuse, never re-declare.
using kimix::builtin_tools::tool_error;
using kimix::builtin_tools::tool_status;
using kimix::builtin_tools::ToolParams;
using kimix::builtin_tools::ValueElement;

// ---------------------------------------------------------------------------
// Constants (note/__init__.py 24-26)
// ---------------------------------------------------------------------------
inline constexpr int64_t k_max_lines = 1000; // MAX_LINES
inline constexpr int64_t k_max_line_length = 2000; // MAX_LINE_LENGTH (code points)
inline constexpr int64_t k_max_bytes = 100 * 1024; // MAX_BYTES = 102400
inline constexpr int64_t k_default_max_char = 65536; // ReadPlanParams.max_char

// ---------------------------------------------------------------------------
// Parameter models
// ---------------------------------------------------------------------------
// WritePlanParams (37-56): `content` accepts the `text` alias; `mode` is
// "overwrite" (default) or "append".
struct write_plan_params {
    kimix::string content;
    kimix::string mode = "overwrite";
};

// ReadPlanParams (98-140) including the _validate_line_offset rules:
// line_offset != 0 and line_offset >= -MAX_LINES; n_lines >= 1;
// max_char >= 0; char_offset >= 0.
struct read_plan_params {
    int64_t line_offset = 1;
    int64_t n_lines = k_max_lines;
    int64_t max_char = k_default_max_char;
    int64_t char_offset = 0;
};

// Edit (294-311): one literal replacement. `old` accepts the `old_string`
// alias, `new` accepts `new_string`.
struct plan_edit_item {
    kimix::string old_text;
    kimix::string new_text;
    bool replace_all = false;
};

// EditPlanParams (313-322): `edit` accepts the `edits` alias and may be a
// single object or a list of objects.
struct edit_plan_params {
    kimix::vector<plan_edit_item> edits;
};

// Parameter parsing. Each returns tool_status::ok on success; on failure the
// returned tool_error carries tool_status::invalid_input and the byte-exact
// pydantic/ValueError wording of the reference implementation.
tool_error parse_write_params(const ToolParams *params, write_plan_params &out);
tool_error parse_read_params(const ToolParams *params, read_plan_params &out);
tool_error parse_edit_params(const ToolParams *params, edit_plan_params &out);

// ---------------------------------------------------------------------------
// Render engine (ReadPlan._read_forward / _read_tail)
// ---------------------------------------------------------------------------
// One budgeted render of the plan file. `output` is "".join(lines_with_no)
// (each line rendered as f"{line_no:6d}\t{truncated}"), `message` is the
// " ".join(message_parts) status line.
struct plan_render {
    kimix::string output;
    kimix::string message;
    int64_t start_line = 1; // first rendered line (total_lines + 1 when empty)
    int64_t total_lines = -1; // -1 == unknown (forward stop before EOF)
    bool max_lines_reached = false;
    bool max_bytes_reached = false;
    kimix::vector<int64_t> truncated_line_numbers;
};

// _read_forward (177-227): positive `line_offset`. Budgets are
// min(n_lines, MAX_LINES) lines and MAX_BYTES bytes, counted over the
// truncated line bodies only (the "%6d\t" prefix is NOT counted, matching the
// reference). `lines` come from read::split_lines (trailing '\n' preserved).
plan_render render_forward(kimix::span<const kimix::string> lines,
                           int64_t line_offset, int64_t n_lines);

// _read_tail (228-291): negative `line_offset` keeps the last
// abs(line_offset) lines, then applies the n_lines/MAX_LINES cap from the head
// of that window and the byte budget by reverse-scanning for the newest lines
// that fit. Always reports "Total lines in file: {total_lines}.".
plan_render render_tail(kimix::span<const kimix::string> lines,
                        int64_t line_offset, int64_t n_lines);

// ReadPlan.__call__ post-processing (169-172): `output[char_offset:max_char]`.
// NOTE the reference quirk - max_char is the slice END index, not a length.
// Code-point slicing; out-of-range and inverted windows clamp like Python.
kimix::string apply_char_window(kimix::string_view output, int64_t char_offset,
                                int64_t max_char);

// Python list repr of the truncated line numbers, e.g. "[3, 7]".
kimix::string format_line_list(kimix::span<const int64_t> line_numbers);

// ---------------------------------------------------------------------------
// Edit kernels (EditPlan._apply_edit and friends)
// ---------------------------------------------------------------------------
struct plan_edit_result {
    kimix::string content; // possibly unchanged
    size_t replacements = 0;
    kimix::optional<kimix::string> suggestion; // "Did you mean" candidate
    tool_error error; // too_large when the fuzzy DP budget is exceeded
};

// _apply_edit (420-461), byte-exact:
// * empty old or old == new -> unchanged, 0 replacements, no suggestion
// * replace_all -> count occurrences of the normalized old text; 0 hits yields
//   the _find_similar suggestion, otherwise every hit is replaced
// * single -> first literal hit in the normalized content
// * else _try_strip_match (whitespace-insensitive single-line splice)
// * else _find_best_fuzzy_match (>= 75%): replace the normalized match once
//   and report NO suggestion (this is where EditPlan differs from the file
//   edit tool)
// * else the _find_similar suggestion with 0 replacements.
plan_edit_result apply_plan_edit(kimix::string_view content,
                                 const plan_edit_item &item);

struct edit_plan_result {
    kimix::string content;
    size_t total_replacements = 0;
    kimix::optional<kimix::string> last_suggestion;
    bool changed = false; // content != original (EditPlan.__call__ 486)
    tool_error error;
};

// EditPlan.__call__ loop (477-486): apply every edit in order, summing the
// replacement counts and keeping the LAST non-empty suggestion.
edit_plan_result apply_plan_edits(kimix::string_view content,
                                  kimix::span<const plan_edit_item> edits);

// ---------------------------------------------------------------------------
// Message composition (verbatim reference strings)
// ---------------------------------------------------------------------------
// "{Tool} tool invalid: no plan_writing_path set."
kimix::string no_plan_path_message(kimix::string_view tool_name);
// "Plan file `{path}` does not exist."
kimix::string plan_missing_message(kimix::string_view path);
// "`{path}` is not a file."
kimix::string plan_not_a_file_message(kimix::string_view path);
// "Plan {written to|appended to} {path}"
kimix::string plan_written_message(kimix::string_view mode,
                                   kimix::string_view path);
// "No replacements were made. The old string was not found in the plan file."
// plus the optional "\n\nDid you mean:\n  {suggestion}" tail.
kimix::string no_replacements_message(kimix::string_view suggestion);
// "Plan file successfully edited. Applied {n} edit(s) with {m} total
// replacement(s)."
kimix::string plan_edited_message(size_t edit_count, size_t replacements);
// "Failed to {write plan|read plan|edit plan}. Error: {what}"
kimix::string plan_failure_message(kimix::string_view action,
                                   kimix::string_view what);

// ---------------------------------------------------------------------------
// File access (isolated so the kernels above stay fixture-free)
// ---------------------------------------------------------------------------
// Read the whole plan file as UTF-8 bytes. Returns not_found when the file
// does not exist, invalid_input when the path is not a regular file.
tool_error read_plan_file(kimix::string_view path, kimix::string &out);
// Write (mode == "overwrite") or append (mode == "append") `content`, creating
// the parent directories first (WritePlan.__call__ 76-83).
tool_error write_plan_file(kimix::string_view path, kimix::string_view content,
                           kimix::string_view mode, uint64_t &size_bytes);

// ---------------------------------------------------------------------------
// Tool classes (CallableTool2-style binding entry points)
// ---------------------------------------------------------------------------
// Shared result envelope serialized by all three tools:
//   status   "ok" | "invalid_input" | "not_found" | "no_change" |
//            "unsupported" | "external_library"
//   message  reference message string
//   output   reference output string ("" where the Python returns output="")
//   brief    reference brief string
// ReadPlan additionally emits start_line / total_lines / max_lines_reached /
// max_bytes_reached / truncated_line_numbers; EditPlan emits
// total_replacements.
class WritePlan : public kimix::builtin_tools::Tool {
public:
    explicit WritePlan(kimix::builtin_tools::Session *session);
    void operator()(const ToolParams *parameters) override;
    kimix::vector<char> const &serialized_result() const { return _result; }
    void result_json(kimix::vector<char> &out) const override { out = _result; }

    // Injected plan-file content for pure (non-native_io) testing. When
    // `has_injected_content` is true the tool edits `injected_content` instead
    // of touching the file system and reports the result through
    // `injected_content`.
    bool has_injected_content = false;
    kimix::string injected_content;
    // Override for Session::plan_path (empty == use the session value).
    kimix::string plan_path_override;

private:
    kimix::vector<char> _result;
};

class ReadPlan : public kimix::builtin_tools::Tool {
public:
    explicit ReadPlan(kimix::builtin_tools::Session *session);
    void operator()(const ToolParams *parameters) override;
    kimix::vector<char> const &serialized_result() const { return _result; }
    void result_json(kimix::vector<char> &out) const override { out = _result; }

    bool has_injected_content = false;
    kimix::string injected_content;
    kimix::string plan_path_override;

private:
    kimix::vector<char> _result;
};

class EditPlan : public kimix::builtin_tools::Tool {
public:
    explicit EditPlan(kimix::builtin_tools::Session *session);
    void operator()(const ToolParams *parameters) override;
    kimix::vector<char> const &serialized_result() const { return _result; }
    void result_json(kimix::vector<char> &out) const override { out = _result; }

    bool has_injected_content = false;
    kimix::string injected_content;
    kimix::string plan_path_override;

private:
    kimix::vector<char> _result;
};

} // namespace kimix::builtin_tools::plan
