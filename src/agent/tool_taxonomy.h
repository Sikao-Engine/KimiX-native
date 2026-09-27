// agent/tool_taxonomy.h - Shared tool classification constants.
//
// Byte-faithful port of kimi_cli/soul/tool_taxonomy.py (38 lines): the single
// source of truth for "which tools edit files" and "which tools can run
// verification", shared by the loop detectors (tool_loop_guard.h), the
// verification gate (verification_gate.h) and later the target-churn provider,
// so the guardrails never drift apart.
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers.

#pragma once

#include <cstddef>

#include <core/kimix_core.h>

#include <llm/yyjson_alc.h>
#include "yyjson.h"

namespace kimix::agent {

// A span over one of the classification arrays below (std::size equivalent
// without pulling <iterator> into every includer).
template <size_t N>
kimix::span<const kimix::string_view>
taxonomy_span(const kimix::string_view (&arr)[N]) noexcept {
    return kimix::span<const kimix::string_view>(arr, N);
}

// ── Classification sets (tool_taxonomy.py:11-35, exact contents) ────────────

// Tools whose primary effect is modifying a file's content.
inline constexpr kimix::string_view kEditTools[] = {
    "write",
    "edit",
    "HashEdit",
    "WritePlan",
    "EditPlan",
    // Aliases used by other tool naming schemes.
    "Write",
    "Edit",
    "Replace",
    "StrReplace",
};

// Tools that execute arbitrary shell commands.
inline constexpr kimix::string_view kShellTools[] = {"bash", "pwsh", "Run"};

// Tools that can act as verification signals for the verification gate:
// marking a todo `done` signals completion; shell tools can run the project's
// own test/check commands. (tool_taxonomy.py: VERIFICATION_TOOL_HINTS =
// {"todo_list"} | SHELL_TOOLS)
inline constexpr kimix::string_view kVerificationOnlyHints[] = {"todo_list"};

// Argument keys that carry the target file path for edit tools.
inline constexpr kimix::string_view kPathParamKeys[] = {"path", "file_path",
                                                        "filename"};

// Argument keys that carry the command string for shell tools.
inline constexpr kimix::string_view kCommandParamKeys[] = {"command", "cmd",
                                                           "code"};

// ── Predicates ───────────────────────────────────────────────────────────────

inline bool taxonomy_contains(kimix::string_view name,
                              kimix::span<const kimix::string_view> set) noexcept {
    for (kimix::string_view entry : set) {
        if (entry == name) {
            return true;
        }
    }
    return false;
}

// True when `name` is an edit-class tool (modifies a file's content).
inline bool is_edit_tool(kimix::string_view name) noexcept {
    return taxonomy_contains(name, taxonomy_span(kEditTools));
}

// True when `name` is a shell-class tool (executes arbitrary commands).
inline bool is_shell_tool(kimix::string_view name) noexcept {
    return taxonomy_contains(name, taxonomy_span(kShellTools));
}

// True when `name` can act as a verification signal for the verification
// gate. NOTE: a todo_list call only counts as verification when it actually
// marks something done - the gate re-checks the arguments
// (verification_gate.py:78-84).
inline bool is_verification_tool_hint(kimix::string_view name) noexcept {
    if (taxonomy_contains(name, taxonomy_span(kVerificationOnlyHints))) {
        return true;
    }
    return is_shell_tool(name);
}

// First value found under one of PATH_PARAM_KEYS in a JSON object argument
// string (the target-churn port's `next((arguments[k] for k in
// PATH_PARAM_KEYS if k in arguments), None)`). Returns nullopt when the
// arguments are not a JSON object or carry none of the path keys.
inline kimix::optional<kimix::string>
path_params_of(kimix::string_view arguments_json) {
    if (arguments_json.empty()) {
        return kimix::optional<kimix::string>();
    }
    // yyjson_read_opts takes a mutable buffer; copy the view.
    kimix::string buf(arguments_json);
    yyjson_doc *doc = yyjson_read_opts(buf.data(), buf.size(), 0,
                                       &kimix::llm::kYYJsonAlcMi, nullptr);
    if (doc == nullptr) {
        return kimix::optional<kimix::string>();
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    kimix::optional<kimix::string> out;
    if (root != nullptr && yyjson_is_obj(root)) {
        for (kimix::string_view key : kPathParamKeys) {
            yyjson_val *v = yyjson_obj_getn(root, key.data(), key.size());
            if (v != nullptr && yyjson_is_str(v)) {
                out.emplace(yyjson_get_str(v), yyjson_get_len(v));
                break;
            }
        }
    }
    yyjson_doc_free(doc);
    return out;
}

} // namespace kimix::agent
