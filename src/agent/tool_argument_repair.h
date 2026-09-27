// agent/tool_argument_repair.h - Argument anti-hallucination repairs
// (F9 / audit G16+G17+G18+G24) and the canonical call key (F11 / audit G12).
//
// Port of the reference's dispatch repair pipeline, in the order toolset.py's
// handle() applies it:
//   1. _repair_argument_format (toolset.py:657-699): unwrap a lone
//      {"arguments": ...} / {"args": ...} wrapper, parse a stringified JSON
//      object/array, unwrap again; a non-dict root degrades to {}
//      (toolset.py:1205-1206);
//   2. repair_tool_arguments (kimi_cli/tools/utils.py:51-86): for every field
//      whose schema annotation is NOT a plain str, a JSON-looking string value
//      is parsed back into the structure (schema-driven, generic);
//   3. _repair_todo_arguments (toolset.py:739-802): the todo tools'
//      top-level shape repair - retired batch keys folded onto the canonical
//      one, bare strings wrapped as items, singular task/todo/item/name
//      promoted to a one-item list;
//   4. _extract_and_save_long_param (src/kimix/tools/common.py:50-250): a
//      long (>200 chars) content param passed in the wrong shape is recovered
//      into a temp .txt file and reported with the "Parameters appear to be in
//      the wrong format..." flow.
//   Plus _canonical_tool_arguments (toolset.py:958-980): the recursively
//   key-sorted JSON text that keys the same-step duplicate short-circuit.
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers,
// no exceptions, no RTTI, MSVC + GCC portable.

#pragma once

#include <cstdint>
#include <utility>

#include <core/kimix_core.h>

#include "agent/tool_name_resolver.h" // tool_param_schema

namespace kimix::agent {

// ── 1. Format repairs (toolset.py:657-699) ───────────────────────────────────

// _repair_argument_format: unwrap double-wrapped arguments, parse stringified
// JSON, unwrap again. `text` must already be valid JSON text (the caller
// parses/repairs it first); the returned text is the compact serialization of
// the repaired value. False + `out` untouched when `text` is not parseable.
bool repair_argument_format(kimix::string_view text, kimix::string &out);

// _unwrap_nested_arguments on an already-parsed document, serialized back to
// compact JSON (single-key {"arguments": ...} / {"args": ...} objects are
// replaced by their inner value).
bool unwrap_nested_arguments(kimix::string_view text, kimix::string &out);

// ── 2. Schema-driven JSON-string repair (utils.py repair_tool_arguments) ────

// repair_tool_arguments: for every argument whose value is a string and whose
// schema field is not a plain string, parse the string as JSON (strict first,
// then the json-repair kernel) and substitute the structure. The schema comes
// from the tool registry entry's parameters_json (empty schema == no repair).
bool repair_tool_arguments(kimix::string_view text,
                           const tool_param_schema &schema, kimix::string &out);

// ── 3. Todo top-level shape repair (toolset.py:739-802) ─────────────────────

// The canonical item-list key of the two native todo tools (the reference's
// single todo_list tool): "todos" for todo_write, "updates" for todo_update.
kimix::string_view todo_batch_key_for(kimix::string_view tool_name);

// _repair_todo_arguments scoped to the native todo tools ("todo_write" /
// "todo_update" - the reference scopes it to todo_list + its retired names).
// Folds the retired batch keys onto the canonical one, wraps bare strings and
// single objects as items, promotes a singular task/todo/item/name key to a
// one-item list (folding the top-level status/notes/rename_to/complete/parent
// extras into the item), and only rewrites when the canonical key is absent.
bool repair_todo_arguments(kimix::string_view tool_name,
                           kimix::string_view text, kimix::string &out);

// ── 4. Canonical call key (toolset.py:958-980, F11) ─────────────────────────

// _canonical_tool_arguments(_text): recursively key-sorted, minified JSON.
// The input text is returned unchanged when it is not valid JSON (the
// reference's `except: return str(arguments)` fallback).
kimix::string canonical_tool_arguments(kimix::string_view arguments_json);

// ── 5. Long malformed content params (common.py:50-250) ─────────────────────

// Minimum length of a param value before extraction is worth it
// (common.py _LONG_PARAM_MIN_LENGTH).
inline constexpr size_t kLongParamMinLength = 200;

// One saved param: the argument key and the temp file that now holds the
// recovered raw content.
struct long_param_save {
    kimix::string param;
    kimix::string path;
};

// _extract_and_save_long_param: detect a long content param passed in the
// wrong shape (JSON-encoded string, list of lines, object, \n-escaped text)
// and save the recovered content to <work_dir>/.kimix_cache/tmp_<pid>/<n>.txt.
// Returns true when at least one param was saved. `error` carries an fs
// failure message.
bool extract_and_save_long_param(kimix::string_view arguments_json,
                                 kimix::string_view tool_name,
                                 kimix::string_view work_dir,
                                 kimix::vector<long_param_save> &saved,
                                 kimix::string &error);

// The long-content parameter names per tool (common.py
// _LONG_CONTENT_PARAMS:31-39, verbatim).
kimix::vector<kimix::string> long_content_params_of(kimix::string_view tool_name);

// _looks_like_malformed_json_param (common.py:57-104): the malformed-shape
// detector for a string value (exposed for tests).
bool looks_like_malformed_json_param(kimix::string_view value);

// _extract_content_from_malformed (common.py:107-153): recover the raw
// content from a malformed string value; nullopt when nothing can be
// extracted.
kimix::optional<kimix::string>
extract_content_from_malformed(kimix::string_view value);

// _build_long_param_retry_msg (common.py:219-250), verbatim text:
//   "{original_error}\n\n[Long content extracted to temp files]\n..."
// The file references use the short forward-slashed display form
// (_display_temp_path): relative to `work_dir` when the file lives under it.
kimix::string build_long_param_retry_msg(
    const kimix::vector<long_param_save> &saved,
    kimix::string_view original_error, kimix::string_view work_dir = {});

} // namespace kimix::agent
