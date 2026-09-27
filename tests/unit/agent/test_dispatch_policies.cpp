// test_dispatch_policies.cpp - Turn-level dispatch policies of the soul:
//   * F11 (audit G12): the same-step duplicate short-circuit - identical
//     (resolved tool name, canonical args) reuses the in-flight/last result,
//     no second tool run, no second loop-guard counting;
//   * F10 (kimisoul.py:1983-1993): a pure approval rejection (no user
//     feedback) stops a ROOT soul's turn with stop_reason "tool_rejected"
//     while a sub-agent session keeps going;
//   * G13 (toolset.py hide/unhide): a hidden tool leaves tool_definitions()
//     but stays callable.
//
// Framework: Boost.UT; every assertion lives in main()-scope lambdas.

#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include "agent/approval.h"
#include "agent/soul.h"
#include "builtin_tools/write_tool.h"

namespace {

using namespace boost::ut;

// Scripted backend: one canned result per chat() call, then plain text.
class ScriptedBackend : public kimix::agent::IChatBackend {
public:
    kimix::vector<kimix::llm::ChatResult> scripted;
    size_t index = 0;

    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &tools,
         const kimix::llm::ChunkCallback &on_chunk) override {
        (void)messages;
        (void)tools;
        (void)on_chunk;
        if (index < scripted.size()) {
            return scripted[index++];
        }
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content = "done";
        return r;
    }
    int64_t max_context_size() const override { return 128000; }
    kimix::string model_name() const override { return "scripted"; }

    static kimix::llm::ChatResult ok_text(kimix::string_view text) {
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content.assign(text.data(), text.size());
        return r;
    }
    // One tool call with the given id/name/arguments.
    static kimix::llm::ChatResult call(kimix::string_view id,
                                       kimix::string_view name,
                                       kimix::string_view arguments) {
        kimix::llm::ChatResult r;
        r.ok = true;
        kimix::llm::ToolCall tc;
        tc.id.assign(id.data(), id.size());
        tc.name.assign(name.data(), name.size());
        tc.arguments.assign(arguments.data(), arguments.size());
        r.tool_calls.push_back(std::move(tc));
        return r;
    }
};

kimix::string tmp_workspace(const char *name) {
    std::error_code ec;
    kimix::filesystem::path base =
        kimix::filesystem::temp_directory_path(ec) / name;
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

size_t count_messages_with(const kimix::agent::AgentSession &session,
                           kimix::string_view needle) {
    size_t n = 0;
    for (const kimix::llm::Message &m : session.history()) {
        if (m.content.find(needle) != kimix::string::npos) {
            ++n;
        }
    }
    return n;
}

} // namespace

int main() {
    using namespace boost::ut;
    using namespace kimix::agent;

    "same_step_duplicate_short_circuits_to_one_run"_test = [] {
        const kimix::string ws = tmp_workspace("kimix_test_dedup_ws");
        ScriptedBackend backend;
        // TWO calls in ONE step (one assistant message carrying both tool
        // calls): the same write emitted twice - the second time with the
        // argument keys in a different ORDER (the canonicalized key must
        // collapse them into one run).
        kimix::llm::ChatResult step;
        step.ok = true;
        kimix::llm::ToolCall first;
        first.id = "call-1";
        first.name = "write";
        first.arguments = R"({"file_path":"dedup.txt","content":"one shot"})";
        kimix::llm::ToolCall second;
        second.id = "call-2";
        second.name = "write";
        second.arguments = R"({"content":"one shot","file_path":"dedup.txt"})";
        step.tool_calls.push_back(std::move(first));
        step.tool_calls.push_back(std::move(second));
        backend.scripted.push_back(std::move(step));
        backend.scripted.push_back(ScriptedBackend::ok_text("finished"));
        AgentSession session(ws);
        KimiSoul::options opts;
        opts.loop_control.verification_gate_enabled = false; // not under test
        opts.auto_compact = false;
        KimiSoul soul(session, backend, opts);
        const auto out = soul.turn("write the file");
        expect(out.ok);
        expect(out.content == "finished");
        expect(out.steps == 2_i); // the write step + the closing text step
        // Both tool_call ids got a result message naming the written file...
        expect(count_messages_with(session, "dedup.txt") >= 1_i);
        size_t tool_results = 0;
        for (const kimix::llm::Message &m : session.history()) {
            if (m.role == "tool") {
                ++tool_results;
            }
        }
        expect(tool_results == 2_i);
        // ...and the copy carries the ORIGINAL result verbatim under the new
        // id (toolset.py _await_dup: same return_value, new tool_call_id) -
        // not a second execution's payload.
        kimix::string first_result;
        kimix::string second_result;
        for (const kimix::llm::Message &m : session.history()) {
            if (m.role == "tool" && m.tool_call_id == "call-1") {
                first_result = m.content;
            }
            if (m.role == "tool" && m.tool_call_id == "call-2") {
                second_result = m.content;
            }
        }
        expect(!first_result.empty());
        expect(eq(first_result, second_result));
    };

    "a_refused_call_is_never_treated_as_a_duplicate"_test = [] {
        ScriptedBackend backend;
        // Two identical calls to a tool that does not exist: each fails with
        // the typed not-found error, and the loop guard still counts both
        // (landed A1 behaviour).
        backend.scripted.push_back(
            ScriptedBackend::call("call-1", "zzqq_absent", "{}"));
        backend.scripted.push_back(
            ScriptedBackend::call("call-2", "zzqq_absent", "{}"));
        backend.scripted.push_back(ScriptedBackend::ok_text("gave up"));
        AgentSession session(tmp_workspace("kimix_test_dedup_ws2"));
        KimiSoul::options opts;
        opts.loop_control.verification_gate_enabled = false; // not under test
        opts.auto_compact = false;
        KimiSoul soul(session, backend, opts);
        const auto out = soul.turn("try the missing tool twice");
        expect(out.ok);
        expect(count_messages_with(session, "Tool `zzqq_absent` not found") ==
               2_i);
    };

    "pure_approval_rejection_stops_a_root_turn"_test = [] {
        ScriptedBackend backend;
        backend.scripted.push_back(ScriptedBackend::call(
            "call-1", "write",
            R"({"file_path":"rejected.txt","content":"x"})"));
        // The reference STOPS the turn on the rejection; no further step runs.
        AgentSession session(tmp_workspace("kimix_test_reject_root_ws"));
        KimiSoul::options opts;
        opts.loop_control.verification_gate_enabled = false; // not under test
        opts.auto_compact = false;
        KimiSoul soul(session, backend, opts);
        Approval approval; // no approver installed: everything is refused
        soul.set_approval(&approval);
        const auto out = soul.turn("write the file");
        // kimisoul.py:1983-1993: StepOutcome(stop_reason="tool_rejected").
        // The turn ends without a further LLM step (the scripted backend had
        // no more entries; any extra step would have returned "done").
        expect(out.ok);
        expect(out.content.empty());
        expect(out.steps == 1_i);
        // The rejection reached the model as the tool result.
        expect(count_messages_with(
                   session, "Tool call rejected by user. Stop and wait for "
                            "instructions.") == 1_i);
    };

    "a_rejection_with_feedback_lets_a_root_turn_continue"_test = [] {
        ScriptedBackend backend;
        backend.scripted.push_back(ScriptedBackend::call(
            "call-1", "write",
            R"({"file_path":"rejected.txt","content":"x"})"));
        backend.scripted.push_back(ScriptedBackend::ok_text("understood"));
        AgentSession session(tmp_workspace("kimix_test_reject_root_ws2"));
        KimiSoul::options opts;
        opts.loop_control.verification_gate_enabled = false; // not under test
        opts.auto_compact = false;
        KimiSoul soul(session, backend, opts);
        Approval approval;
        // A rejection WITH feedback (the user typed a reason): the reference
        // keeps the turn alive so the model can react to the feedback.
        approval.set_approver(
            [](kimix::string_view, kimix::string_view, kimix::string_view,
               kimix::string &feedback) {
                feedback = "use another file";
                return ApprovalResponse::reject;
            });
        soul.set_approval(&approval);
        const auto out = soul.turn("write the file");
        expect(out.ok);
        expect(out.content == "understood");
        expect(out.steps == 2_i);
        // The typed feedback reached the model.
        expect(count_messages_with(session, "use another file") == 1_i);
    };

    "hide_keeps_a_tool_callable_but_unlisted"_test = [] {
        ScriptedBackend backend;
        backend.scripted.push_back(ScriptedBackend::call(
            "call-1", "read", R"({"file_path":"hidden.txt"})"));
        backend.scripted.push_back(ScriptedBackend::ok_text("finished"));
        AgentSession session(tmp_workspace("kimix_test_hide_ws"));
        KimiSoul::options opts;
        opts.loop_control.verification_gate_enabled = false; // not under test
        opts.auto_compact = false;
        KimiSoul soul(session, backend, opts);
        expect(soul.hide_tool("read"));
        expect(!soul.hide_tool("zzqq_absent")); // the reference returns bool
        bool listed = false;
        for (const kimix::llm::Tool &d : soul.tool_definitions()) {
            if (d.name == "read") {
                listed = true;
            }
        }
        expect(!listed) << "a hidden tool never reaches the LLM tool list";
        // ...but dispatch still runs it (toolset.py keeps _tool_dict intact).
        kimix::string err;
        const kimix::string out =
            soul.execute_tool_call("read", R"({"file_path":"hidden.txt"})", err);
        expect(out.find("<system>") == 0) << out;
        expect(err.empty());
        // unhide restores the listing.
        soul.unhide_tool("read");
        listed = false;
        for (const kimix::llm::Tool &d : soul.tool_definitions()) {
            if (d.name == "read") {
                listed = true;
            }
        }
        expect(listed);
        // A full turn with the tool hidden still executes it.
        soul.hide_tool("read");
        const auto hidden_turn = soul.turn("read the file");
        expect(hidden_turn.ok);
        expect(hidden_turn.content == "finished");
    };

    return 0;
}
