// agent/verification_gate.cpp - VerificationGate implementation
// (see verification_gate.h; a 1:1 port of soul/verification_gate.py).

#include "agent/verification_gate.h"

#include "agent/dynamic_injection.h" // G9 framework's is_system_reminder_message
#include "agent/tool_taxonomy.h"
#include <llm/yyjson_alc.h>
#include "yyjson.h"
namespace kimix::agent {
namespace {
using builtin_tools::todo::todo_item;
using builtin_tools::todo::todo_status;

// DFS pre-order over the todo tree (the reference iterates the flat loaded
// list; the native tree is flattened in the same order).
void collect_unfinished(kimix::span<const todo_item> items,
                        kimix::vector<const todo_item *> &out) {
    for (const todo_item &item : items) {
        if (item.status != todo_status::done) {
            out.push_back(&item);
        }
        collect_unfinished(item.children, out);
    }
}

// VerificationGate._classify_turn_tool_calls (verification_gate.py:65-85):
// (has_edits, has_verification) over the turn-history suffix.
std::pair<bool, bool>
classify_turn_tool_calls(const kimix::vector<kimix::llm::Message> &turn_history) {
    bool has_edits = false;
    bool has_verification = false;
    for (const kimix::llm::Message &msg : turn_history) {
        if (msg.role != "assistant" || msg.tool_calls.empty()) {
            continue;
        }
        for (const kimix::llm::ToolCall &tc : msg.tool_calls) {
            if (is_edit_tool(tc.name)) {
                has_edits = true;
            }
            if (is_verification_tool_hint(tc.name)) {
                // A todo_list call only counts as verification when it
                // actually marks something done.
                if (tc.name == "todo_list") {
                    if (VerificationGate::todolist_marks_done(tc.arguments)) {
                        has_verification = true;
                    }
                } else {
                    has_verification = true;
                }
            }
        }
    }
    return {has_edits, has_verification};
}

} // namespace

kimix::string verification_gate_unfinished_todos_reason(
    kimix::span<const todo_item> todos) {
    kimix::vector<const todo_item *> unfinished;
    collect_unfinished(todos, unfinished);
    kimix::string out = "Unfinished todo_list tasks remain:";
    for (size_t i = 0; i < unfinished.size(); ++i) {
        if (i == static_cast<size_t>(kVerificationGateMaxUnfinishedListed)) {
            out += "\n- … and ";
            out += std::to_string(unfinished.size() -
                                  kVerificationGateMaxUnfinishedListed);
            out += " more";
            break;
        }
        out += "\n- [";
        out.append(builtin_tools::todo::status_name(unfinished[i]->status));
        out += "] ";
        out += unfinished[i]->content;
    }
    return out;
}

kimix::string_view verification_gate_unverified_edits_reason() noexcept {
    return "You modified code this turn but ran no verification "
           "(no tests/check commands). "
           "Run the project's tests or a verification command before finishing.";
}

kimix::string verification_gate_nudge_text(kimix::span<const kimix::string> reasons,
                                           int32_t nudge, int32_t max_nudges) {
    kimix::string out =
        "The turn cannot finish yet — verification gate findings:\n\n";
    for (size_t i = 0; i < reasons.size(); ++i) {
        if (i != 0) {
            out += "\n\n";
        }
        out += reasons[i];
    }
    out += "\n\nAddress the findings above, then finish. (nudge ";
    out += std::to_string(nudge);
    out += "/";
    out += std::to_string(max_nudges);
    out += " this turn)";
    return out;
}

bool VerificationGate::todolist_marks_done(kimix::string_view arguments_json) {
    // verification_gate.py:88-105.
    if (arguments_json.empty()) {
        return false;
    }
    kimix::string buf(arguments_json);
    yyjson_doc *doc = yyjson_read_opts(buf.data(), buf.size(), 0,
                                       &kimix::llm::kYYJsonAlcMi, nullptr);
    if (doc == nullptr) {
        return false;
    }
    bool marks_done = false;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root != nullptr && yyjson_is_obj(root)) {
        yyjson_val *todos = yyjson_obj_get(root, "todos");
        if (todos == nullptr) {
            todos = yyjson_obj_get(root, "items");
        }
        if (todos != nullptr) {
            // A single dict is treated as a one-element list.
            auto marks = [](yyjson_val *t) {
                if (!yyjson_is_obj(t)) {
                    return false;
                }
                yyjson_val *status = yyjson_obj_get(t, "status");
                if (status == nullptr || !yyjson_is_str(status)) {
                    return false;
                }
                const kimix::string_view v(yyjson_get_str(status),
                                           yyjson_get_len(status));
                return v == "done" || v == "completed";
            };
            if (yyjson_is_arr(todos)) {
                size_t idx = 0;
                size_t max = 0;
                yyjson_val *t = nullptr;
                yyjson_arr_foreach(todos, idx, max, t) {
                    if (marks(t)) {
                        marks_done = true;
                        break;
                    }
                }
            } else if (yyjson_is_obj(todos)) {
                marks_done = marks(todos);
            }
        }
    }
    yyjson_doc_free(doc);
    return marks_done;
}

kimix::optional<kimix::string>
VerificationGate::check(const kimix::vector<kimix::llm::Message> &history,
                        kimix::span<const todo_item> todos) {
    // verification_gate.py:111-152.
    if (_nudges >= _max_nudges) {
        return kimix::optional<kimix::string>();
    }

    kimix::vector<kimix::string> reasons;

    // Condition 1: unfinished todos.
    kimix::vector<const todo_item *> unfinished;
    collect_unfinished(todos, unfinished);
    if (!unfinished.empty()) {
        reasons.push_back(
            verification_gate_unfinished_todos_reason(todos));
    }

    // Condition 2: edits this turn but no verification-class call.
    // Turn scope: the suffix after the most recent REAL user message
    // (verification_gate.py:48-63).
    size_t start = 0;
    for (size_t idx = history.size(); idx > 0; --idx) {
        const kimix::llm::Message &msg = history[idx - 1];
        if (msg.role == "user" && !is_system_reminder_message(msg)) {
            start = idx;
            break;
        }
    }
    kimix::vector<kimix::llm::Message> turn_history(history.begin() + start,
                                                    history.end());
    const auto [has_edits, has_verification] = classify_turn_tool_calls(turn_history);
    if (has_edits && !has_verification) {
        reasons.emplace_back(verification_gate_unverified_edits_reason());
    }

    if (reasons.empty()) {
        return kimix::optional<kimix::string>();
    }

    ++_nudges;
    return verification_gate_nudge_text(reasons, _nudges, _max_nudges);
}

} // namespace kimix::agent
