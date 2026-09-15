// Test for builtin_tools/agent_tool.h (namespace kimix::builtin_tools::agents).
//
// Covers:
// - agent_registry store semantics: put/get/close, insertion-ordered
//   list_active, MAX_SESSIONS=10 LRU eviction, the live-session map
// - the pending-message queue: 50-per-target cap, drain-once, count
// - format_pending_messages / queued_message_output / prefix_sender verbatim
// - resolve_prompt (@path indirection, base_dir -> CWD fallback, missing file)
// - prompt_saved_message / offload_long_prompt / inject_response
// - build_context_block (files, read errors, structured data)
// - format_history_markdown (role icons + labels) / format_history_summary /
//   turn_to_value
// - list_active_json (orjson OPT_INDENT_2 shape and key order)
// - resolve_send_target: sub-agent -> parent, explicit id, unregistered id,
//   most-recently-active default, no active children
// - parameter parsing for all four tools (aliases + validation)
// - Subagent / SendMessageTool / ListAgents / InterruptAgent Tool wrappers with an
//   injected runner: foreground success, failure with the saved-prompt hint,
//   awaiting-response state, close_session vs continued, background start +
//   join + interrupt, the recursion guard and the unsupported-without-runner
//   path
//
// All test logic lives in main() scope; no file-scope static registrations.
#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include "builtin_tools/agent_tool.h"

#include <string>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::builtin_tools;
using namespace kimix::builtin_tools::agents;

namespace {

kimix::string kix(std::string_view sv) {
    return kimix::string(sv.data(), sv.size());
}

std::string sv_of(const kimix::string &s) {
    return std::string(s.data(), s.size());
}

std::string json_of(const kimix::vector<char> &buf) {
    return std::string(buf.data(), buf.size());
}

bool has(const kimix::vector<char> &buf, std::string_view needle) {
    return json_of(buf).find(std::string(needle)) != std::string::npos;
}

// A deterministic clock so created_at / last_accessed are predictable.
struct fake_clock {
    kimix::shared_ptr<double> value =
        kimix::shared_ptr<double>(new double(1000.0));
    kimix::function<double()> fn() {
        kimix::shared_ptr<double> v = value;
        return [v]() { return *v; };
    }
    void advance(double seconds) { *value += seconds; }
};

// An in-memory file table standing in for the file-system hooks.
struct fake_files {
    kimix::shared_ptr<std::vector<std::pair<std::string, std::string>>> table =
        kimix::shared_ptr<std::vector<std::pair<std::string, std::string>>>(
            new std::vector<std::pair<std::string, std::string>>());
    kimix::shared_ptr<std::vector<std::string>> saved =
        kimix::shared_ptr<std::vector<std::string>>(
            new std::vector<std::string>());

    void add(std::string path, std::string content) {
        table->emplace_back(std::move(path), std::move(content));
    }
    read_file_fn reader() {
        auto t = table;
        return [t](kimix::string_view path, kimix::string &out) {
            const std::string want(path.data(), path.size());
            for (const auto &entry : *t) {
                if (entry.first == want) {
                    out = kix(entry.second);
                    return true;
                }
            }
            return false;
        };
    }
    save_prompt_fn saver() {
        auto t = table;
        auto s = saved;
        return [t, s](kimix::string_view text, kimix::string_view ext) {
            const std::string name =
                ".kimix_cache/prompt_" + std::to_string(s->size()) +
                std::string(ext.data(), ext.size());
            s->push_back(name);
            t->emplace_back(name, std::string(text.data(), text.size()));
            return kix(name);
        };
    }
};

agent_entry make_entry(std::string_view id, double created, double accessed,
                       std::string_view state = "completed") {
    agent_entry e;
    e.session_id = kix(id);
    e.created_at = created;
    e.last_accessed = accessed;
    e.state = kix(state);
    e.is_active = true;
    return e;
}

// A scripted runner that records every request it receives.
struct fake_runner {
    kimix::shared_ptr<std::vector<subagent_request>> calls =
        kimix::shared_ptr<std::vector<subagent_request>>(
            new std::vector<subagent_request>());
    kimix::shared_ptr<subagent_run_result> result =
        kimix::shared_ptr<subagent_run_result>(new subagent_run_result());

    subagent_runner fn() {
        auto c = calls;
        auto r = result;
        return [c, r](const subagent_request &req) {
            c->push_back(req);
            return *r;
        };
    }
};

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    // -----------------------------------------------------------------------
    // Registry store semantics
    // -----------------------------------------------------------------------
    "registry_put_get_close"_test = [] {
        agent_registry reg;
        expect(reg.get("missing") == nullptr);
        reg.put(make_entry("a", 1.0, 2.0));
        const agent_entry *found = reg.get("a");
        expect(found != nullptr);
        expect(found->session_id == kix("a"));
        expect(found->created_at == 1.0);
        expect(reg.size() == 1u);
        expect(reg.close("a"));
        expect(reg.get("a") == nullptr);
        expect(!reg.close("a"));
        expect(reg.size() == 0u);
    };

    "registry_put_updates_in_place"_test = [] {
        agent_registry reg;
        reg.put(make_entry("a", 1.0, 2.0));
        agent_entry updated = make_entry("a", 1.0, 9.0, "awaiting_response");
        updated.total_turns = 4;
        reg.put(updated);
        expect(reg.size() == 1u);
        const agent_entry *found = reg.get("a");
        expect(found->last_accessed == 9.0);
        expect(found->state == kix("awaiting_response"));
        expect(found->total_turns == 4_i);
    };

    "registry_list_active_keeps_insertion_order"_test = [] {
        agent_registry reg;
        reg.put(make_entry("first", 1.0, 5.0));
        reg.put(make_entry("second", 2.0, 6.0));
        reg.put(make_entry("third", 3.0, 4.0));
        const kimix::vector<agent_list_item> items = reg.list_active();
        expect(items.size() == 3u);
        expect(items[0].session_id == kix("first"));
        expect(items[1].session_id == kix("second"));
        expect(items[2].session_id == kix("third"));
    };

    "registry_list_active_skips_inactive"_test = [] {
        agent_registry reg;
        agent_entry inactive = make_entry("gone", 1.0, 1.0);
        inactive.is_active = false;
        reg.put(inactive);
        reg.put(make_entry("here", 1.0, 1.0));
        const kimix::vector<agent_list_item> items = reg.list_active();
        expect(items.size() == 1u);
        expect(items[0].session_id == kix("here"));
    };

    "registry_evicts_lru_at_max_sessions"_test = [] {
        agent_registry reg;
        // MAX_SESSIONS is 10: eviction runs while size() >= 10, so inserting
        // the 10th entry is allowed and the 11th triggers one eviction.
        for (int i = 0; i < 10; ++i) {
            reg.put(make_entry(kimix::format("s{}", i), 1.0,
                               100.0 + static_cast<double>(i)));
        }
        expect(reg.size() == 10u);
        const kimix::vector<kimix::string> evicted = reg.evict_lru_if_needed();
        expect(evicted.size() == 1u);
        // s0 has the smallest last_accessed.
        expect(evicted[0] == kix("s0"));
        expect(reg.size() == 9u);
        expect(reg.get("s0") == nullptr);
        expect(reg.get("s9") != nullptr);
    };

    "registry_eviction_is_a_noop_below_the_cap"_test = [] {
        agent_registry reg;
        reg.put(make_entry("only", 1.0, 1.0));
        expect(reg.evict_lru_if_needed().empty());
        expect(reg.size() == 1u);
    };

    "registry_live_session_map"_test = [] {
        agent_registry reg;
        expect(!reg.has_session("x"));
        reg.register_session("x");
        expect(reg.has_session("x"));
        reg.register_session(""); // ignored
        expect(!reg.has_session(""));
        reg.unregister_session("x");
        expect(!reg.has_session("x"));
    };

    // -----------------------------------------------------------------------
    // Pending message queue
    // -----------------------------------------------------------------------
    "pending_queue_drains_once"_test = [] {
        agent_registry reg;
        reg.queue_pending_message("t1", "first");
        reg.queue_pending_message("t1", "second");
        expect(reg.pending_message_count("t1") == 2u);
        const kimix::vector<kimix::string> drained =
            reg.drain_pending_messages("t1");
        expect(drained.size() == 2u);
        expect(drained[0] == kix("first"));
        expect(drained[1] == kix("second"));
        expect(reg.pending_message_count("t1") == 0u);
        expect(reg.drain_pending_messages("t1").empty());
    };

    "pending_queue_caps_at_fifty"_test = [] {
        agent_registry reg;
        for (int i = 0; i < 60; ++i) {
            reg.queue_pending_message("t", kimix::format("m{}", i));
        }
        expect(reg.pending_message_count("t") == k_max_pending_messages);
        const kimix::vector<kimix::string> drained =
            reg.drain_pending_messages("t");
        expect(drained.size() == 50u);
        // The FIRST fifty are kept (later ones are dropped).
        if (!drained.empty()) {
            expect(drained.at(0) == kix("m0"));
            expect(drained.at(drained.size() - 1) == kix("m49"));
        }
    };

    "pending_queue_is_per_target"_test = [] {
        agent_registry reg;
        reg.queue_pending_message("a", "for-a");
        reg.queue_pending_message("b", "for-b");
        expect(reg.pending_message_count("a") == 1u);
        expect(reg.pending_message_count("b") == 1u);
        expect(reg.drain_pending_messages("a")[0] == kix("for-a"));
    };

    "format_pending_messages_block"_test = [] {
        expect(format_pending_messages(
                   kimix::span<const kimix::string>())
                   .empty());
        const kimix::vector<kimix::string> msgs = {kix("do X"), kix("then Y")};
        const std::string out = sv_of(format_pending_messages(
            kimix::span<const kimix::string>(msgs)));
        const std::string expected =
            "<pending-messages>\n"
            "You have the following queued message(s) from the parent agent "
            "(sent while you were idle or not running):\n"
            "1. do X\n"
            "2. then Y\n"
            "</pending-messages>";
        expect(out == expected) << "got:\n" << out;
    };

    "queued_message_output_wording"_test = [] {
        const std::string out = sv_of(queued_message_output("abc", "not running"));
        expect(out.find("Agent 'abc' is not running (not running).") == 0);
        expect(out.find("Message queued; it will be listed in the target's "
                        "next prompt only if you resume the session with "
                        "subagent(session_id='abc', ...).") !=
               std::string::npos);
        expect(out.find("Otherwise it stays queued (no delivery).") !=
               std::string::npos);
    };

    "prefix_sender_wording"_test = [] {
        expect(prefix_sender("", "hello") == kix("hello"));
        expect(prefix_sender("parent-1", "hello") ==
               kix("Message from agent 'parent-1':\nhello"));
    };

    // -----------------------------------------------------------------------
    // resolve_prompt
    // -----------------------------------------------------------------------
    "resolve_prompt_inline"_test = [] {
        fake_files files;
        kimix::string out;
        kimix::string error;
        expect(resolve_prompt("plain task", "/work", files.reader(), out,
                              error));
        expect(out == kix("plain task"));
        expect(error.empty());
    };

    "resolve_prompt_at_path_relative_to_base_dir"_test = [] {
        fake_files files;
        files.add("/work/task.md", "task body");
        kimix::string out;
        kimix::string error;
        expect(resolve_prompt("@task.md", "/work", files.reader(), out, error));
        expect(out == kix("task body"));
    };

    "resolve_prompt_falls_back_to_cwd_relative"_test = [] {
        fake_files files;
        // Not present under base_dir, but present relative to the process CWD.
        files.add("task.md", "cwd body");
        kimix::string out;
        kimix::string error;
        expect(resolve_prompt("@task.md", "/work", files.reader(), out, error));
        expect(out == kix("cwd body"));
    };

    "resolve_prompt_missing_file"_test = [] {
        fake_files files;
        kimix::string out;
        kimix::string error;
        expect(!resolve_prompt("@nope.md", "/work", files.reader(), out, error));
        expect(error == kix("prompt file not found: nope.md"));
    };

    "resolve_prompt_absolute_path"_test = [] {
        fake_files files;
        files.add("/abs/task.md", "abs body");
        kimix::string out;
        kimix::string error;
        expect(resolve_prompt("@/abs/task.md", "/work", files.reader(), out,
                              error));
        expect(out == kix("abs body"));
    };

    "prompt_saved_message_wording"_test = [] {
        expect(prompt_saved_message("", "p.md").empty());
        expect(prompt_saved_message("task", "").empty());
        expect(prompt_saved_message("task", ".kimix_cache/tmp_1/0.md") ==
               kix("[prompt saved to .kimix_cache/tmp_1/0.md] Retry with "
                   "subagent(prompt=@.kimix_cache/tmp_1/0.md) to reuse this "
                   "prompt."));
    };

    "offload_long_prompt_threshold"_test = [] {
        const kimix::string short_prompt(100, 'a');
        expect(offload_long_prompt(short_prompt, "p.md") == short_prompt);
        const kimix::string long_prompt(k_prompt_offload_bytes + 1, 'a');
        expect(offload_long_prompt(long_prompt, "p.md") ==
               kix("Please read the task from `p.md` and execute it."));
        // Exactly at the limit is NOT offloaded.
        const kimix::string exact(k_prompt_offload_bytes, 'a');
        expect(offload_long_prompt(exact, "p.md") == exact);
    };

    "inject_response_wording"_test = [] {
        expect(inject_response("do the task", "which mode?", "fast") ==
               kix("The parent agent responded to your question (which "
                   "mode?):\n\nfast\n\nNow, regarding your original task: do "
                   "the task"));
    };

    // -----------------------------------------------------------------------
    // build_context_block
    // -----------------------------------------------------------------------
    "context_block_files_and_data"_test = [] {
        fake_files files;
        files.add("/work/a.txt", "AAA");
        const kimix::vector<kimix::string> ctx_files = {kix("a.txt"),
                                                        kix("missing.txt")};
        const std::string out = sv_of(build_context_block(
            kimix::span<const kimix::string>(ctx_files), "{\"k\": 1}",
            files.reader(), "/work"));
        expect(out.find("<context>\n") == 0);
        expect(out.find("<file path='a.txt'>\nAAA\n</file>") !=
               std::string::npos);
        expect(out.find("<file path='missing.txt' error='") !=
               std::string::npos);
        expect(out.find("<data>\n{\"k\": 1}\n</data>") != std::string::npos);
        // "\n</context>" is eleven characters.
        expect(out.size() >= 11u && out.compare(out.size() - 11, 11,
                                                "\n</context>") == 0)
            << out;
    };

    "context_block_empty_when_no_inputs"_test = [] {
        fake_files files;
        expect(build_context_block(kimix::span<const kimix::string>(), "",
                                   files.reader(), "/work")
                   .empty());
    };

    // -----------------------------------------------------------------------
    // History formatting
    // -----------------------------------------------------------------------
    "format_history_markdown"_test = [] {
        conversation_turn user;
        user.role = "user";
        user.content = "hi";
        user.type = "text";
        conversation_turn assistant;
        assistant.role = "assistant";
        assistant.content = "hello";
        assistant.type = "text";
        conversation_turn tool;
        tool.role = "tool";
        tool.content = "result";
        tool.type = "tool_result";
        const kimix::vector<conversation_turn> turns = {user, assistant, tool};
        const std::string out = sv_of(format_history_markdown(
            kimix::span<const conversation_turn>(turns)));
        // The label is metadata["type"] when present (Python:
        // `turn.metadata.get("type", turn.role)`), so a "text" turn is
        // labelled "text", not by its role.
        expect(out.find("### Turn 1: \xF0\x9F\x91\xA4 text") !=
               std::string::npos)
            << out;
        expect(out.find("### Turn 2: \xF0\x9F\xA4\x96 text") !=
               std::string::npos)
            << out;
        expect(out.find("### Turn 3: \xF0\x9F\x94\xA7 tool_result") !=
               std::string::npos)
            << out;
        expect(out.find("\nhi\n") != std::string::npos);
        // Without metadata the label falls back to the role.
        conversation_turn bare;
        bare.role = "system";
        bare.content = "note";
        const kimix::vector<conversation_turn> bare_turns = {bare};
        const std::string bare_out = sv_of(format_history_markdown(
            kimix::span<const conversation_turn>(bare_turns)));
        expect(bare_out.find("### Turn 1: \xE2\x9A\x99\xEF\xB8\x8F system") !=
               std::string::npos)
            << bare_out;
        // An unknown role renders "?".
        conversation_turn odd;
        odd.role = "mystery";
        const kimix::vector<conversation_turn> odd_turns = {odd};
        expect(sv_of(format_history_markdown(
                   kimix::span<const conversation_turn>(odd_turns)))
                   .find("### Turn 1: ? mystery") != std::string::npos);
    };

    "format_history_summary_counts"_test = [] {
        conversation_turn call;
        call.role = "assistant";
        call.type = "tool_call";
        conversation_turn res;
        res.role = "tool";
        res.type = "tool_result";
        conversation_turn text;
        text.role = "assistant";
        text.type = "text";
        text.content = "abcde"; // 5 code points
        conversation_turn think;
        think.role = "assistant";
        think.type = "think";
        think.content = "ignored";
        const kimix::vector<conversation_turn> turns = {call, res, text, think};
        expect(format_history_summary(
                   kimix::span<const conversation_turn>(turns)) ==
               kix("Sub-agent made 1 tool call(s) with 1 result(s), and "
                   "produced 1 text response(s) (5 total characters)."));
    };

    "turn_to_value_shape"_test = [] {
        conversation_turn turn;
        turn.role = "assistant";
        turn.content = "body";
        turn.timestamp = 12.5;
        turn.type = "text";
        const ValueElement value = turn_to_value(turn);
        expect(value.is_object());
        const ToolParams *obj = value.as_object();
        expect(obj != nullptr);
        expect(obj->get("role")->as_string() == kix("assistant"));
        expect(obj->get("content")->as_string() == kix("body"));
        expect(obj->get("timestamp")->as_real() == 12.5);
        const ToolParams *meta = obj->get("metadata")->as_object();
        expect(meta != nullptr);
        expect(meta->get("type")->as_string() == kix("text"));
    };

    // -----------------------------------------------------------------------
    // list_active_json
    // -----------------------------------------------------------------------
    "list_active_json_empty"_test = [] {
        expect(list_active_json(kimix::span<const agent_list_item>()) ==
               kix("[]"));
    };

    "list_active_json_shape"_test = [] {
        const kimix::vector<agent_list_item> items = {
            {kix("id-1"), 100.5, 200.25, 3, kix("running"), true}};
        const std::string out = sv_of(list_active_json(
            kimix::span<const agent_list_item>(items)));
        // orjson OPT_INDENT_2: two-space indent, insertion key order.
        expect(out.find("[\n  {\n") == 0) << out;
        expect(out.find("\"session_id\": \"id-1\"") != std::string::npos);
        expect(out.find("\"created_at\": 100.5") != std::string::npos);
        expect(out.find("\"last_accessed\": 200.25") != std::string::npos);
        expect(out.find("\"total_turns\": 3") != std::string::npos);
        expect(out.find("\"state\": \"running\"") != std::string::npos);
        expect(out.find("\"is_active\": true") != std::string::npos);
        // The document closes with "\n  }\n]" (six characters).
        expect(out.size() >= 6u &&
               out.compare(out.size() - 6, 6, "\n  }\n]") == 0)
            << out;
        // Key order matters for byte parity with orjson.
        expect(out.find("session_id") < out.find("created_at"));
        expect(out.find("created_at") < out.find("last_accessed"));
        expect(out.find("last_accessed") < out.find("total_turns"));
        expect(out.find("total_turns") < out.find("state"));
        expect(out.find("state") < out.find("is_active"));
    };

    "list_active_json_escapes"_test = [] {
        const kimix::vector<agent_list_item> items = {
            {kix("q\"uote"), 1.0, 2.0, 0, kix("a&b<c>"), true}};
        const std::string out = sv_of(list_active_json(
            kimix::span<const agent_list_item>(items)));
        expect(out.find("q\\\"uote") != std::string::npos);
        expect(out.find("a&b<c>") != std::string::npos);
    };

    // -----------------------------------------------------------------------
    // resolve_send_target
    // -----------------------------------------------------------------------
    "send_target_sub_agent_messages_parent"_test = [] {
        agent_registry reg;
        reg.register_session("parent-1");
        const send_target t = resolve_send_target(reg, true, "parent-1", "child");
        expect(t.target_id.has_value());
        expect(*t.target_id == kix("parent-1")); // `id` is ignored
        expect(t.has_live_session);
        expect(t.reason.empty());
    };

    "send_target_sub_agent_without_parent"_test = [] {
        agent_registry reg;
        const send_target t = resolve_send_target(reg, true, "", "child");
        expect(!t.target_id.has_value());
        expect(t.reason ==
               kix("sub-agent has no recorded parent_session_id"));
    };

    "send_target_sub_agent_parent_unregistered"_test = [] {
        agent_registry reg;
        const send_target t =
            resolve_send_target(reg, true, "gone-parent", "child");
        expect(t.target_id.has_value());
        expect(*t.target_id == kix("gone-parent"));
        expect(!t.has_live_session);
        expect(t.reason == kix("parent agent 'gone-parent' is not registered"));
    };

    "send_target_explicit_id"_test = [] {
        agent_registry reg;
        reg.register_session("child-1");
        const send_target t = resolve_send_target(reg, false, "", "child-1");
        expect(*t.target_id == kix("child-1"));
        expect(t.has_live_session);
    };

    "send_target_unregistered_id"_test = [] {
        agent_registry reg;
        const send_target t = resolve_send_target(reg, false, "", "ghost");
        expect(*t.target_id == kix("ghost"));
        expect(!t.has_live_session);
        expect(t.reason == kix("agent 'ghost' is not registered"));
    };

    "send_target_defaults_to_most_recently_active"_test = [] {
        agent_registry reg;
        reg.put(make_entry("old", 1.0, 10.0));
        reg.put(make_entry("newest", 1.0, 99.0));
        reg.put(make_entry("middle", 1.0, 50.0));
        reg.register_session("newest");
        const send_target t = resolve_send_target(reg, false, "", "");
        expect(*t.target_id == kix("newest"));
        expect(t.has_live_session);
    };

    "send_target_no_active_children"_test = [] {
        agent_registry reg;
        const send_target t = resolve_send_target(reg, false, "", "");
        expect(!t.target_id.has_value());
        expect(t.reason == kix("no active sub-agents to message"));
    };

    "is_resumable_requires_an_active_entry"_test = [] {
        agent_registry reg;
        expect(!is_resumable(reg, "a"));
        expect(!is_resumable(reg, ""));
        reg.put(make_entry("a", 1.0, 1.0));
        expect(is_resumable(reg, "a"));
        agent_entry inactive = make_entry("b", 1.0, 1.0);
        inactive.is_active = false;
        reg.put(inactive);
        expect(!is_resumable(reg, "b"));
    };

    // -----------------------------------------------------------------------
    // Parameter parsing
    // -----------------------------------------------------------------------
    "subagent_params_defaults"_test = [] {
        ToolParams params;
        params.values["prompt"] = ValueElement::make_string(kix("do it"));
        subagent_params out;
        expect(!parse_subagent_params(&params, out).failed());
        expect(out.prompt == kix("do it"));
        expect(out.run_in_background);
        expect(out.close_session);
        expect(!out.return_history);
        expect(out.history_format == kix("json"));
        expect(!out.inherit_context);
        expect(!out.session_id.has_value());
    };

    "subagent_params_task_alias"_test = [] {
        ToolParams params;
        params.values["task"] = ValueElement::make_string(kix("aliased"));
        subagent_params out;
        expect(!parse_subagent_params(&params, out).failed());
        expect(out.prompt == kix("aliased"));
    };

    "subagent_params_requires_prompt"_test = [] {
        ToolParams params;
        subagent_params out;
        const tool_error err = parse_subagent_params(&params, out);
        expect(err.failed());
        expect(err.message == kix("missing required field: prompt"));
    };

    "subagent_params_session_alias_and_flags"_test = [] {
        ToolParams params;
        params.values["prompt"] = ValueElement::make_string(kix("p"));
        params.values["session"] = ValueElement::make_string(kix("s-1"));
        params.values["run_in_background"] = ValueElement::make_bool(false);
        params.values["close_session"] = ValueElement::make_bool(false);
        params.values["return_history"] = ValueElement::make_bool(true);
        params.values["history_format"] =
            ValueElement::make_string(kix("summary"));
        params.values["inherit_context"] = ValueElement::make_bool(true);
        subagent_params out;
        expect(!parse_subagent_params(&params, out).failed());
        expect(*out.session_id == kix("s-1"));
        expect(!out.run_in_background);
        expect(!out.close_session);
        expect(out.return_history);
        expect(out.history_format == kix("summary"));
        expect(out.inherit_context);
    };

    "subagent_params_bad_history_format"_test = [] {
        ToolParams params;
        params.values["prompt"] = ValueElement::make_string(kix("p"));
        params.values["history_format"] =
            ValueElement::make_string(kix("yaml"));
        subagent_params out;
        const tool_error err = parse_subagent_params(&params, out);
        expect(err.failed());
        expect(sv_of(err.message).find("'json', 'markdown' or 'summary'") !=
               std::string::npos);
    };

    "subagent_params_context_files_and_data"_test = [] {
        ToolParams params;
        params.values["prompt"] = ValueElement::make_string(kix("p"));
        ValueElement::Array files;
        files.push_back(ValueElement::make_string(kix("a.txt")));
        files.push_back(ValueElement::make_string(kix("b.txt")));
        params.values["context_files"] =
            ValueElement::make_array(std::move(files));
        kimix::shared_ptr<ToolParams> data(new ToolParams());
        data->values["k"] = ValueElement::make_int(7);
        params.values["context_data"] =
            ValueElement::make_object(std::move(data));
        subagent_params out;
        expect(!parse_subagent_params(&params, out).failed());
        expect(out.context_files.size() == 2u);
        expect(out.context_files[1] == kix("b.txt"));
        expect(out.context_data_json.has_value());
        expect(sv_of(*out.context_data_json).find("\"k\": 7") !=
               std::string::npos);
    };

    "subagent_params_context_files_must_be_strings"_test = [] {
        ToolParams params;
        params.values["prompt"] = ValueElement::make_string(kix("p"));
        ValueElement::Array files;
        files.push_back(ValueElement::make_int(1));
        params.values["context_files"] =
            ValueElement::make_array(std::move(files));
        subagent_params out;
        const tool_error err = parse_subagent_params(&params, out);
        expect(err.failed());
        expect(err.message == kix("context_files must be a list of strings"));
    };

    "send_message_params"_test = [] {
        ToolParams params;
        params.values["question"] = ValueElement::make_string(kix("hi"));
        params.values["id"] = ValueElement::make_string(kix("sub-1"));
        send_message_params out;
        expect(!parse_send_message_params(&params, out).failed());
        expect(out.message == kix("hi"));
        expect(*out.subagent_id == kix("sub-1"));
    };

    "send_message_params_requires_message"_test = [] {
        ToolParams params;
        send_message_params out;
        const tool_error err = parse_send_message_params(&params, out);
        expect(err.failed());
        expect(err.message == kix("missing required field: message"));
    };

    "list_agents_params_default_scope"_test = [] {
        list_agents_params out;
        expect(!parse_list_agents_params(nullptr, out).failed());
        expect(out.scope == kix("children"));
        ToolParams params;
        params.values["scope"] = ValueElement::make_string(kix("descendants"));
        expect(!parse_list_agents_params(&params, out).failed());
        expect(out.scope == kix("descendants"));
    };

    "interrupt_params_aliases"_test = [] {
        ToolParams by_session;
        by_session.values["session"] = ValueElement::make_string(kix("a"));
        interrupt_agent_params out;
        expect(!parse_interrupt_params(&by_session, out).failed());
        expect(out.agent_id == kix("a"));
        ToolParams by_session_id;
        by_session_id.values["session_id"] =
            ValueElement::make_string(kix("b"));
        expect(!parse_interrupt_params(&by_session_id, out).failed());
        expect(out.agent_id == kix("b"));
        ToolParams none;
        const tool_error err = parse_interrupt_params(&none, out);
        expect(err.failed());
    };

    // -----------------------------------------------------------------------
    // Subagent Tool wrapper
    // -----------------------------------------------------------------------
    "subagent_requires_a_runner"_test = [] {
        Session session;
        session.session_id = "parent";
        Subagent tool(&session);
        ToolParams params;
        params.values["prompt"] = ValueElement::make_string(kix("task"));
        tool(&params);
        expect(has(tool.serialized_result(), "unsupported"));
        expect(has(tool.serialized_result(),
                   "native subagent requires an injected runner"));
    };

    "subagent_recursion_guard"_test = [] {
        Session session;
        session.is_sub_agent = true;
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        fake_runner runner;
        session.agents->runner = runner.fn();
        Subagent tool(&session);
        ToolParams params;
        params.values["prompt"] = ValueElement::make_string(kix("task"));
        tool(&params);
        expect(has(tool.serialized_result(),
                   "Recursive sub-agent call detected"));
        expect(has(tool.serialized_result(), "sub-agent recursively"));
        expect(runner.calls->empty());
    };

    "subagent_foreground_success"_test = [] {
        Session session;
        session.session_id = "parent";
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        fake_clock clock;
        session.agents->now = clock.fn();
        fake_runner runner;
        runner.result->ok = true;
        runner.result->output = "the answer";
        conversation_turn turn;
        turn.role = "assistant";
        turn.content = "the answer";
        turn.type = "text";
        runner.result->turns = {turn};
        session.agents->runner = runner.fn();

        Subagent tool(&session);
        ToolParams params;
        params.values["prompt"] = ValueElement::make_string(kix("compute 6*7"));
        params.values["run_in_background"] = ValueElement::make_bool(false);
        params.values["description"] =
            ValueElement::make_string(kix("quick math"));
        tool(&params);

        expect(runner.calls->size() == 1u);
        expect(runner.calls->at(0).prompt == kix("compute 6*7"));
        expect(!runner.calls->at(0).resume);
        expect(runner.calls->at(0).close_session);
        expect(!runner.calls->at(0).background);
        expect(has(tool.serialized_result(), "Session ID: "));
        expect(has(tool.serialized_result(), "the answer"));
        expect(has(tool.serialized_result(), "Sub-agent task completed"));
        expect(has(tool.serialized_result(), "\"status\":\"closed\""));
        expect(has(tool.serialized_result(), "\"turn_count\":1"));
        // close_session defaults to true -> the entry leaves the store.
        expect(session.agents->size() == 0u);
    };

    "subagent_keeps_the_session_when_close_session_is_false"_test = [] {
        Session session;
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        fake_clock clock;
        session.agents->now = clock.fn();
        fake_runner runner;
        runner.result->ok = true;
        runner.result->output = "done";
        session.agents->runner = runner.fn();

        Subagent tool(&session);
        ToolParams params;
        params.values["prompt"] = ValueElement::make_string(kix("task"));
        params.values["run_in_background"] = ValueElement::make_bool(false);
        params.values["close_session"] = ValueElement::make_bool(false);
        tool(&params);

        expect(has(tool.serialized_result(), "\"status\":\"continued\""));
        expect(session.agents->size() == 1u);
        const kimix::vector<agent_list_item> items =
            session.agents->list_active();
        expect(items.size() == 1u);
        expect(items[0].state == kix("completed"));
    };

    "subagent_failure_saves_the_prompt"_test = [] {
        Session session;
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        fake_files files;
        session.agents->read_file = files.reader();
        session.agents->save_prompt = files.saver();
        fake_runner runner;
        runner.result->ok = false;
        runner.result->error = "provider exploded";
        session.agents->runner = runner.fn();

        Subagent tool(&session);
        ToolParams params;
        params.values["prompt"] = ValueElement::make_string(kix("a task"));
        params.values["run_in_background"] = ValueElement::make_bool(false);
        tool(&params);

        const std::string json = json_of(tool.serialized_result());
        expect(json.find("provider exploded") != std::string::npos);
        expect(json.find("[prompt saved to ") != std::string::npos);
        expect(json.find("Retry with subagent(prompt=@") != std::string::npos);
        expect(json.find("sub-agent task failed") != std::string::npos);
        expect(json.find("(no text output)") != std::string::npos);
        expect(files.saved->size() == 1u);
        // The failed session is closed and dropped.
        expect(session.agents->size() == 0u);
    };

    "subagent_awaiting_response"_test = [] {
        Session session;
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        fake_clock clock;
        session.agents->now = clock.fn();
        fake_runner runner;
        runner.result->ok = true;
        runner.result->output = "which mode should I use?";
        runner.result->pending_question = kimix::string("which mode?");
        session.agents->runner = runner.fn();

        Subagent tool(&session);
        ToolParams params;
        params.values["prompt"] = ValueElement::make_string(kix("task"));
        params.values["run_in_background"] = ValueElement::make_bool(false);
        params.values["return_history"] = ValueElement::make_bool(true);
        params.values["history_format"] =
            ValueElement::make_string(kix("summary"));
        tool(&params);

        expect(has(tool.serialized_result(),
                   "Sub-agent is awaiting a response"));
        expect(has(tool.serialized_result(),
                   "\"status\":\"awaiting_response\""));
        expect(has(tool.serialized_result(), "\"question\":\"which mode?\""));
        expect(session.agents->size() == 1u);
        const kimix::vector<agent_list_item> items =
            session.agents->list_active();
        expect(items[0].state == kix("awaiting_response"));
    };

    "subagent_resolves_at_path_prompts"_test = [] {
        Session session;
        session.work_dir = "/work";
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        fake_files files;
        files.add("/work/task.md", "the real task");
        session.agents->read_file = files.reader();
        session.agents->save_prompt = files.saver();
        fake_runner runner;
        runner.result->ok = true;
        runner.result->output = "ok";
        session.agents->runner = runner.fn();

        Subagent tool(&session);
        ToolParams params;
        params.values["prompt"] = ValueElement::make_string(kix("@task.md"));
        params.values["run_in_background"] = ValueElement::make_bool(false);
        tool(&params);
        expect(runner.calls->at(0).prompt == kix("the real task"));
    };

    "subagent_missing_prompt_file"_test = [] {
        Session session;
        session.work_dir = "/work";
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        fake_files files;
        session.agents->read_file = files.reader();
        session.agents->save_prompt = files.saver();
        fake_runner runner;
        session.agents->runner = runner.fn();

        Subagent tool(&session);
        ToolParams params;
        params.values["prompt"] = ValueElement::make_string(kix("@gone.md"));
        params.values["run_in_background"] = ValueElement::make_bool(false);
        tool(&params);
        expect(has(tool.serialized_result(),
                   "prompt file not found: gone.md"));
        expect(has(tool.serialized_result(), "\"prompt_file\""));
        expect(runner.calls->empty());
    };

    "subagent_builds_the_context_block"_test = [] {
        Session session;
        session.work_dir = "/work";
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        fake_files files;
        files.add("/work/ctx.txt", "CONTEXT");
        session.agents->read_file = files.reader();
        session.agents->save_prompt = files.saver();
        fake_runner runner;
        runner.result->ok = true;
        runner.result->output = "ok";
        session.agents->runner = runner.fn();

        Subagent tool(&session);
        ToolParams params;
        params.values["prompt"] = ValueElement::make_string(kix("the task"));
        params.values["run_in_background"] = ValueElement::make_bool(false);
        ValueElement::Array ctx;
        ctx.push_back(ValueElement::make_string(kix("ctx.txt")));
        params.values["context_files"] =
            ValueElement::make_array(std::move(ctx));
        tool(&params);
        const std::string prompt = sv_of(runner.calls->at(0).prompt);
        expect(prompt.find("<context>") == 0) << prompt;
        expect(prompt.find("<file path='ctx.txt'>") != std::string::npos);
        expect(prompt.find("CONTEXT") != std::string::npos);
        expect(prompt.size() > 12u &&
               prompt.compare(prompt.size() - 8, 8, "the task") == 0)
            << prompt;
    };

    "subagent_drains_queued_messages_into_the_prompt"_test = [] {
        Session session;
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        fake_runner runner;
        runner.result->ok = true;
        runner.result->output = "ok";
        session.agents->runner = runner.fn();
        // A message queued for a session id we are about to resume.
        session.agents->put(make_entry("resume-me", 1.0, 1.0));
        session.agents->register_session("resume-me");
        session.agents->queue_pending_message("resume-me", "queued note");

        Subagent tool(&session);
        ToolParams params;
        params.values["prompt"] = ValueElement::make_string(kix("next task"));
        params.values["run_in_background"] = ValueElement::make_bool(false);
        params.values["session"] =
            ValueElement::make_string(kix("resume-me"));
        tool(&params);
        expect(runner.calls->at(0).resume);
        expect(runner.calls->at(0).session_id == kix("resume-me"));
        const std::string prompt = sv_of(runner.calls->at(0).prompt);
        expect(prompt.find("next task") == 0);
        expect(prompt.find("<pending-messages>") != std::string::npos);
        expect(prompt.find("1. queued note") != std::string::npos);
        expect(session.agents->pending_message_count("resume-me") == 0u);
    };

    "subagent_injects_the_deprecated_response"_test = [] {
        Session session;
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        fake_runner runner;
        runner.result->ok = true;
        runner.result->output = "ok";
        session.agents->runner = runner.fn();
        agent_entry entry = make_entry("q-1", 1.0, 1.0, "awaiting_response");
        entry.pending_question = kimix::string("which mode?");
        session.agents->put(entry);
        session.agents->register_session("q-1");

        Subagent tool(&session);
        ToolParams params;
        params.values["prompt"] = ValueElement::make_string(kix("the task"));
        params.values["run_in_background"] = ValueElement::make_bool(false);
        params.values["session"] = ValueElement::make_string(kix("q-1"));
        params.values["response"] = ValueElement::make_string(kix("fast"));
        tool(&params);
        const std::string prompt = sv_of(runner.calls->at(0).prompt);
        expect(prompt.find(
                   "The parent agent responded to your question (which "
                   "mode?):") == 0)
            << prompt;
        expect(prompt.find("Now, regarding your original task: the task") !=
               std::string::npos);
    };

    "subagent_offloads_a_huge_prompt"_test = [] {
        Session session;
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        fake_files files;
        session.agents->read_file = files.reader();
        session.agents->save_prompt = files.saver();
        fake_runner runner;
        runner.result->ok = true;
        runner.result->output = "ok";
        session.agents->runner = runner.fn();

        Subagent tool(&session);
        ToolParams params;
        params.values["prompt"] = ValueElement::make_string(
            kimix::string(k_prompt_offload_bytes + 10, 'z'));
        params.values["run_in_background"] = ValueElement::make_bool(false);
        tool(&params);
        const std::string prompt = sv_of(runner.calls->at(0).prompt);
        expect(prompt.find("Please read the task from `") == 0) << prompt;
        expect(prompt.find("` and execute it.") != std::string::npos);
        expect(files.saved->size() == 1u);
    };

    "subagent_background_returns_immediately_and_joins"_test = [] {
        Session session;
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        fake_clock clock;
        session.agents->now = clock.fn();
        fake_runner runner;
        runner.result->ok = true;
        runner.result->output = "background result";
        session.agents->runner = runner.fn();

        Subagent tool(&session);
        ToolParams params;
        params.values["prompt"] = ValueElement::make_string(kix("bg task"));
        // run_in_background defaults to true.
        tool(&params);

        expect(has(tool.serialized_result(), "Session ID: "));
        expect(has(tool.serialized_result(), "Sub-agent task started"));
        expect(has(tool.serialized_result(), "\"status\":\"running\""));
        expect(session.agents->size() == 1u);
        const kimix::vector<agent_list_item> items =
            session.agents->list_active();
        expect(items.size() == 1u);
        expect(items[0].state == kix("running"));
        // The worker settles and the result becomes available.
        subagent_run_result settled;
        expect(session.agents->join_run(items[0].session_id, settled));
        expect(settled.ok);
        expect(settled.output == kix("background result"));
        expect(runner.calls->size() == 1u);
        expect(runner.calls->at(0).background);
    };

    "subagent_background_can_be_interrupted"_test = [] {
        Session session;
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        // A runner that blocks until it is cancelled, proving the cancel flag
        // reaches the worker.
        auto gate = kimix::shared_ptr<std::atomic<int>>(new std::atomic<int>(0));
        session.agents->runner =
            [gate](const subagent_request &req) {
                subagent_run_result out;
                while (req.cancel != nullptr && !req.cancel->load() &&
                       gate->load() == 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(2));
                }
                out.ok = false;
                out.cancelled = (req.cancel != nullptr && req.cancel->load());
                out.error = out.cancelled ? "cancelled" : "gate opened";
                return out;
            };
        Subagent tool(&session);
        ToolParams params;
        params.values["prompt"] = ValueElement::make_string(kix("slow task"));
        tool(&params);
        const kimix::vector<agent_list_item> items =
            session.agents->list_active();
          expect(items.size() == 1u);
          expect(session.agents->is_running(items[0].session_id));
          expect(session.agents->request_cancel(items[0].session_id));
          subagent_run_result settled;
          expect(session.agents->join_run(items[0].session_id, settled));
          expect(settled.cancelled);
          expect(settled.error == kix("cancelled"));
      };
      "interrupt_agent_close_does_not_deadlock_steer_draining_runner"_test = [] {
          // Regression (found by new_tools_e2e turn D): a real sub-agent
          // runner polls the steer queue - a REGISTRY call - between steps.
          // The old close() joined the worker WHILE HOLDING the registry
          // lock, so interrupt_agent deadlocked against the worker's
          // drain_steer poll (and reset the agent_run before joining,
          // dangling req.cancel/finished).
          Session session;
          kimix::shared_ptr<agent_registry> reg(new agent_registry());
          session.agents = reg;
          reg->runner =
              [reg](const subagent_request &req) {
                  subagent_run_result out;
                  int64_t spins = 0;
                  while (req.cancel != nullptr && !req.cancel->load() &&
                         spins < 10000000) {
                      (void)reg->drain_steer(req.session_id);
                      std::this_thread::yield();
                      ++spins;
                  }
                  out.ok = false;
                  out.cancelled =
                      (req.cancel != nullptr && req.cancel->load());
                  out.error = out.cancelled ? kix("cancelled")
                                            : kix("spin bound reached");
                  return out;
              };
          Subagent start(&session);
          ToolParams params;
          params.values["prompt"] = ValueElement::make_string(kix("slow task"));
          start(&params); // background by default
          const kimix::vector<agent_list_item> items = reg->list_active();
          expect(items.size() == 1u);
          const kimix::string id = items[0].session_id;
          // Interrupt on a separate thread so a regression (deadlock) fails
          // the test instead of hanging the whole suite.
          std::atomic<bool> closed{false};
          std::thread closer([&] {
              InterruptAgent tool(&session);
              ToolParams ip;
              ip.values["agent_id"] = ValueElement::make_string(id);
              tool(&ip);
              closed.store(true);
          });
          const auto deadline =
              std::chrono::steady_clock::now() + std::chrono::seconds(10);
          while (!closed.load() &&
                 std::chrono::steady_clock::now() < deadline) {
              std::this_thread::sleep_for(std::chrono::milliseconds(5));
          }
          const bool finished_in_time = closed.load();
          if (!finished_in_time) {
              closer.detach(); // leave the deadlocked thread behind; fail
          }
          expect(finished_in_time);
          if (finished_in_time) {
              closer.join();
              // close() parks the settled result: the cancelled run stays
              // observable through run_finished/join_run after the session
              // bookkeeping is dropped.
              expect(reg->run_finished(id));
              subagent_run_result settled;
              expect(reg->join_run(id, settled));
              expect(settled.cancelled);
          }
      };

    "subagent_history_formats"_test = [] {
        Session session;
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        fake_runner runner;
        runner.result->ok = true;
        runner.result->output = "ok";
        conversation_turn call;
        call.role = "assistant";
        call.type = "tool_call";
        conversation_turn text;
        text.role = "assistant";
        text.type = "text";
        text.content = "final";
        runner.result->turns = {call, text};
        session.agents->runner = runner.fn();

        // markdown
        Subagent md(&session);
        ToolParams params;
        params.values["prompt"] = ValueElement::make_string(kix("t"));
        params.values["run_in_background"] = ValueElement::make_bool(false);
        params.values["return_history"] = ValueElement::make_bool(true);
        params.values["history_format"] =
            ValueElement::make_string(kix("markdown"));
        md(&params);
        expect(has(md.serialized_result(), "### Turn 1:"));

        // summary
        Subagent sum(&session);
        params.values["history_format"] =
            ValueElement::make_string(kix("summary"));
        sum(&params);
        expect(has(sum.serialized_result(),
                   "Sub-agent made 1 tool call(s) with 0 result(s), and "
                   "produced 1 text response(s) (5 total characters)."));

        // json (default): a real array of turn objects
        Subagent js(&session);
        params.values.erase("history_format");
        js(&params);
        expect(has(js.serialized_result(), "\"conversation_history\":["));
        expect(has(js.serialized_result(), "\"role\":\"assistant\""));
    };

    // -----------------------------------------------------------------------
    // SendMessage Tool wrapper
    // -----------------------------------------------------------------------
    "send_message_delivers_to_a_running_target"_test = [] {
        Session session;
        session.session_id = "parent";
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        auto gate = kimix::shared_ptr<std::atomic<int>>(new std::atomic<int>(0));
        session.agents->runner = [gate](const subagent_request &req) {
            subagent_run_result out;
            while (req.cancel != nullptr && !req.cancel->load() &&
                   gate->load() == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            out.ok = true;
            out.output = "done";
            return out;
        };
        // Start a background sub-agent so there IS a running target.
        Subagent starter(&session);
        ToolParams start;
        start.values["prompt"] = ValueElement::make_string(kix("long task"));
        starter(&start);
        const kimix::vector<agent_list_item> items =
            session.agents->list_active();
        expect(items.size() == 1u);
        const kimix::string child_id = items[0].session_id;
        expect(session.agents->is_running(child_id));

        SendMessageTool tool(&session);
        ToolParams params;
        params.values["message"] = ValueElement::make_string(kix("more work"));
        params.values["subagent_id"] = ValueElement::make_string(child_id);
        tool(&params);
        expect(has(tool.serialized_result(),
                   "Message delivered to agent '"));
        expect(has(tool.serialized_result(), "Message sent"));
        const kimix::vector<kimix::string> steered =
            session.agents->drain_steer(child_id);
        expect(steered.size() == 1u);
        expect(steered[0] == kix("Message from agent 'parent':\nmore work"));
        session.agents->request_cancel(child_id);
        subagent_run_result settled;
        session.agents->join_run(child_id, settled);
    };

    "send_message_queues_for_an_idle_target"_test = [] {
        Session session;
        session.session_id = "parent";
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        session.agents->put(make_entry("idle-1", 1.0, 1.0));
        session.agents->register_session("idle-1");

        SendMessageTool tool(&session);
        ToolParams params;
        params.values["message"] = ValueElement::make_string(kix("ping"));
        params.values["subagent_id"] =
            ValueElement::make_string(kix("idle-1"));
        tool(&params);
        expect(has(tool.serialized_result(), "Message queued"));
        expect(has(tool.serialized_result(), "Agent 'idle-1' is not running"));
        expect(session.agents->pending_message_count("idle-1") == 1u);
        const kimix::vector<kimix::string> queued =
            session.agents->drain_pending_messages("idle-1");
        expect(queued[0] == kix("Message from agent 'parent':\nping"));
    };

    "send_message_queues_for_a_closed_session"_test = [] {
        Session session;
        session.session_id = "parent";
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        SendMessageTool tool(&session);
        ToolParams params;
        params.values["message"] = ValueElement::make_string(kix("ping"));
        params.values["subagent_id"] =
            ValueElement::make_string(kix("closed-1"));
        tool(&params);
        expect(has(tool.serialized_result(), "Message queued"));
        expect(has(tool.serialized_result(), "session closed or idle"));
        expect(session.agents->pending_message_count("closed-1") == 1u);
    };

    "send_message_rejects_self_messaging"_test = [] {
        Session session;
        session.session_id = "me";
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        session.agents->register_session("me");
        SendMessageTool tool(&session);
        ToolParams params;
        params.values["message"] = ValueElement::make_string(kix("hi"));
        params.values["subagent_id"] = ValueElement::make_string(kix("me"));
        tool(&params);
        expect(has(tool.serialized_result(), "Cannot message yourself."));
        expect(has(tool.serialized_result(), "Self message rejected"));
    };

    "send_message_unresolvable_target"_test = [] {
        Session session;
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        SendMessageTool tool(&session);
        ToolParams params;
        params.values["message"] = ValueElement::make_string(kix("hi"));
        tool(&params);
        expect(has(tool.serialized_result(),
                   "Cannot resolve target agent: no active sub-agents to "
                   "message"));
        expect(has(tool.serialized_result(), "Target agent not found"));
    };

    "send_message_sub_agent_targets_parent"_test = [] {
        Session session;
        session.session_id = "child";
        session.is_sub_agent = true;
        session.parent_session_id = "parent";
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        session.agents->register_session("parent");
        session.agents->put(make_entry("parent", 1.0, 1.0));
        SendMessageTool tool(&session);
        ToolParams params;
        params.values["message"] = ValueElement::make_string(kix("need input"));
        // No subagent_id: a sub-agent always targets its parent.
        tool(&params);
        // The parent is registered but not running -> queued.
        expect(has(tool.serialized_result(), "Message queued"));
        expect(session.agents->pending_message_count("parent") == 1u);
        const kimix::vector<kimix::string> queued =
            session.agents->drain_pending_messages("parent");
        expect(queued[0] == kix("Message from agent 'child':\nneed input"));
    };

    // -----------------------------------------------------------------------
    // ListAgents Tool wrapper
    // -----------------------------------------------------------------------
    "list_agents_tool_output"_test = [] {
        Session session;
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        fake_clock clock;
        session.agents->now = clock.fn();
        agent_entry entry = make_entry("sub-1", 10.0, 20.0, "running");
        entry.total_turns = 3;
        session.agents->put(entry);

        ListAgents tool(&session);
        ToolParams params;
        tool(&params);
        const std::string json = json_of(tool.serialized_result());
        expect(json.find("Listed active subagents") != std::string::npos);
        expect(json.find("\\\"session_id\\\": \\\"sub-1\\\"") !=
               std::string::npos)
            << json;
        expect(json.find("\\\"total_turns\\\": 3") != std::string::npos);
        expect(json.find("\\\"state\\\": \\\"running\\\"") !=
               std::string::npos);
        expect(json.find("\"scope\":\"children\"") != std::string::npos);
    };

    "list_agents_empty_store"_test = [] {
        Session session;
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        ListAgents tool(&session);
        tool(nullptr);
        expect(has(tool.serialized_result(), "[]"));
    };

    // -----------------------------------------------------------------------
    // InterruptAgent Tool wrapper
    // -----------------------------------------------------------------------
    "interrupt_agent_unknown_session"_test = [] {
        Session session;
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        InterruptAgent tool(&session);
        ToolParams params;
        params.values["agent_id"] = ValueElement::make_string(kix("ghost"));
        tool(&params);
        expect(has(tool.serialized_result(), "Session not found"));
    };

    "interrupt_agent_closes_and_removes"_test = [] {
        Session session;
        session.agents = kimix::shared_ptr<agent_registry>(new agent_registry());
        session.agents->put(make_entry("sub-9", 1.0, 1.0));
        session.agents->register_session("sub-9");
        session.agents->queue_pending_message("sub-9", "kept for later");

        InterruptAgent tool(&session);
        ToolParams params;
        params.values["agent_id"] = ValueElement::make_string(kix("sub-9"));
        tool(&params);
        expect(has(tool.serialized_result(), "Session sub-9 closed."));
        expect(has(tool.serialized_result(), "Session closed"));
        expect(session.agents->get("sub-9") == nullptr);
        expect(!session.agents->has_session("sub-9"));
        // Queued messages survive the close (they are listed on resume).
        expect(session.agents->pending_message_count("sub-9") == 1u);
    };

    // -----------------------------------------------------------------------
    // Session-scoped registry attachment
    // -----------------------------------------------------------------------
    "session_registry_is_lazily_created_and_shared"_test = [] {
        Session session;
        session.session_id = "owner";
        expect(session.agents == nullptr);
        agent_registry &first = session_registry(&session);
        expect(session.agents != nullptr);
        agent_registry &second = session_registry(&session);
        expect(&first == &second);
        expect(first.owner_session_id == kix("owner"));
    };
}
