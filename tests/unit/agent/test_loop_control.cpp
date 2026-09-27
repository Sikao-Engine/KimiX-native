// test_loop_control.cpp - Turn-level tests for the Phase-1 loop-control
// features of KimiSoul (src/agent/soul.*), driven by a scripted fake chat
// backend (no network):
// * A1 loop detectors: an identical tool call repeated across steps earns the
//   graded <system-reminder> texts inside the tool results and a force-stop
//   (stop_reason "tool_call_repeat").
// * A2 loop-recovery gate: up to 3 plain-text "[loop-recovery]" user prompts
//   restating the request; when the budget is exhausted the turn ends with the
//   reference's synthesized text (never tool-only).
// * A3 reasoning-present reset: steps carrying a non-empty thinking block
//   reset the detectors, so no force-stop ever fires.
// * A4 verification gate: unfinished todos / edits without verification turn
//   a no_tool_calls stop into a <system-reminder> nudge + extra step, bounded
//   by verification_gate_max_nudges (2).
//
// Framework: Boost.UT (tests/ut/ut.hpp).
#include "ut/ut.hpp"

#include <agent/soul.h>
#include <builtin_tools/todo_tool.h>
#include <cstdint>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

// Scripted chat backend: pops one canned result per chat() call; once the
// script is exhausted it falls back to `default_tool_call` (an identical
// tool call) or plain text.
class ScriptedBackend : public kimix::agent::IChatBackend {
public:
    kimix::vector<kimix::llm::ChatResult> scripted;
    size_t index = 0;
    bool default_tool_call = false;
    kimix::vector<kimix::llm::Message> seen_messages;

    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &tools,
         const kimix::llm::ChunkCallback &on_chunk,
         const kimix::llm::AbortCheck * /*abort*/) override {
        (void)tools;
        (void)on_chunk;
        seen_messages.push_back(messages.back());
        if (index < scripted.size()) {
            return scripted[index++];
        }
        if (default_tool_call) {
            return probe_call();
        }
        return ok_text("done");
    }
    int64_t max_context_size() const override { return 128000; }
    kimix::string model_name() const override { return "scripted"; }

    static kimix::llm::ChatResult ok_text(kimix::string_view text) {
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content.assign(text.data(), text.size());
        return r;
    }
    // One identical call to an unregistered tool: execute_tool_call reports
    // "unknown tool: probe" and the loop guard counts the call.
    static kimix::llm::ChatResult probe_call(kimix::string_view reasoning = {}) {
        kimix::llm::ChatResult r;
        r.ok = true;
        r.reasoning.assign(reasoning.data(), reasoning.size());
        kimix::llm::ToolCall tc;
        tc.id = "call-1";
        tc.name = "probe";
        tc.arguments = R"({"x":1})";
        r.tool_calls.push_back(std::move(tc));
        return r;
    }
};

kimix::agent::KimiSoul::options test_options() {
    kimix::agent::KimiSoul::options opts;
    opts.system_prompt = "test";
    opts.auto_compact = false;
    return opts;
}

// Counts history messages whose content contains `needle`.
int count_messages_with(const kimix::agent::AgentSession &session,
                        kimix::string_view needle) {
    int n = 0;
    for (const kimix::llm::Message &m : session.history()) {
        if (m.content.find(needle) != kimix::string::npos) {
            ++n;
        }
    }
    return n;
}

} // namespace

int main() {
    using kimix::agent::AgentSession;
    using kimix::agent::KimiSoul;

    "identical_tool_call_loop_force_stops_and_recovers"_test = [] {
        ScriptedBackend backend;
        for (int i = 0; i < 16; ++i) {
            backend.scripted.push_back(ScriptedBackend::probe_call());
        }
        backend.scripted.push_back(ScriptedBackend::ok_text("recovered answer"));
        AgentSession session;
        KimiSoul soul(session, backend, test_options());
        kimix::string wire;
        const auto out = soul.turn(
            "do the thing",
            [&wire](const kimix::llm::Chunk &c) { wire += c.content; });
        expect(out.ok);
        expect(out.content == "recovered answer");
        expect(out.steps == 17_i); // 16 looping steps + 1 recovery answer
        // Graded reminders were appended to the tool results the model sees:
        // R1 at streaks 3-7 (5x), R2 at 8-11 (4x), R3 at 12-16 (5x).
        expect(count_messages_with(session, "Stop repeating the same tool call "
                                            "with identical parameters.") == 5_i);
        expect(count_messages_with(session, "Repeated identical call:") == 4_i);
        expect(count_messages_with(session, "Dead-end loop detected.") == 5_i);
        // The force-stop produced one recovery user prompt + the wire notice.
        expect(count_messages_with(session, "[loop-recovery] Stop repeating tool "
                                            "calls.") == 1_i);
        expect(count_messages_with(session, "Loop detector: adjacent-repeat.") ==
               1_i);
        expect(count_messages_with(session, "Repeated tool: probe.") == 1_i);
        expect(count_messages_with(session,
                                   "(recovery 1/3)") == 1_i);
        expect(wire.find("\n[Recovering from repeated tool calls...]\n") !=
               kimix::string::npos);
    };

    "exhausted_recovery_budget_synthesizes_final_text"_test = [] {
        ScriptedBackend backend;
        backend.default_tool_call = true; // loop forever
        AgentSession session;
        KimiSoul soul(session, backend, test_options());
        const auto out = soul.turn("fix the bug");
        // 16 identical steps to the first force-stop + 3 recovery rounds; the
        // 4th consecutive tool_call_repeat stop exhausts the budget and
        // synthesizes the fallback immediately.
        expect(out.steps == 19_i);
        expect(out.ok); // the turn ends on text, never tool-only
        expect(out.content ==
               "I could not complete the task without repeating tool calls.\n\n"
               "Original request: fix the bug\n\n"
               "Progress is preserved above. Rephrase, narrow the scope, or add "
               "context so I can continue.");
        // Three recovery prompts went out before the synthesis.
        expect(count_messages_with(session, "[loop-recovery] Stop repeating tool "
                                            "calls.") == 3_i);
        // The synthesized fallback is a real assistant message on the history.
        const kimix::llm::Message &last = session.history().back();
        expect(last.role == "assistant");
        expect(last.content == out.content);
    };

    "reasoning_between_tool_calls_resets_the_detectors"_test = [] {
        ScriptedBackend backend;
        for (int i = 0; i < 20; ++i) {
            // Every step thinks before its (identical) tool call.
            backend.scripted.push_back(
                ScriptedBackend::probe_call("thinking about the problem"));
        }
        backend.scripted.push_back(ScriptedBackend::ok_text("done"));
        AgentSession session;
        KimiSoul soul(session, backend, test_options());
        const auto out = soul.turn("explore");
        expect(out.ok);
        expect(out.content == "done");
        expect(out.steps == 21_i);
        // No detector ever fired: no reminder, no recovery prompt.
        expect(count_messages_with(session, "<system-reminder>") == 0_i);
        expect(count_messages_with(session, "[loop-recovery]") == 0_i);
    };

    "unfinished_todos_nudge_then_let_the_turn_finish"_test = [] {
        ScriptedBackend backend;
        backend.scripted.push_back(ScriptedBackend::ok_text("all done"));
        backend.scripted.push_back(ScriptedBackend::ok_text("all done"));
        backend.scripted.push_back(ScriptedBackend::ok_text("all done"));
        AgentSession session;
        // Seed the session todo state with one unfinished item.
        auto &todos = kimix::builtin_tools::todo::session_todos(
            session.tool_session());
        kimix::builtin_tools::todo::todo_item item;
        item.content = "Fix the bug";
        item.status = kimix::builtin_tools::todo::todo_status::pending;
        todos.todos.push_back(std::move(item));
        KimiSoul soul(session, backend, test_options());
        const auto out = soul.turn("do it");
        expect(out.ok);
        expect(out.content == "all done");
          expect(out.steps == 3_i); // answer + 2 nudged continuations
          // G9 interaction (kimisoul.py:1634): the per-step dynamic-injection
          // strip removes the previous step's <system-reminder> gate nudge
          // from live history before the next request - the nudges are
          // ephemeral exactly like the provider reminders and do NOT persist
          // on the final history. The gate wording itself is pinned by
          // test_verification_gate.cpp; here the observable effect is the
          // two forced extra steps plus no stale reminders left behind.
          expect(count_messages_with(session, "Unfinished todo_write tasks "
                                              "remain:") == 0_i);
          expect(count_messages_with(session, "- [pending] Fix the bug") == 0_i);
          expect(count_messages_with(session,
                                     "The turn cannot finish yet — verification gate "
                                     "findings:") == 0_i);
          expect(count_messages_with(session, "(nudge 1/2 this turn)") == 0_i);
          expect(count_messages_with(session, "(nudge 2/2 this turn)") == 0_i);
          // No stale <system-reminder> user message survives the turn.
          int reminders = 0;
          for (const kimix::llm::Message &m : session.history()) {
              if (m.role == "user" &&
                  m.content.find("<system-reminder>\nThe turn cannot finish yet") ==
                      0) {
                  ++reminders;
              }
          }
          expect(reminders == 0_i);
      };

    "edits_without_verification_nudge_then_finish"_test = [] {
        ScriptedBackend backend;
        // One edit-class tool call (unregistered name -> error result, but the
        // gate classifies by the call name), then plain answers.
        kimix::llm::ChatResult edit;
        edit.ok = true;
        kimix::llm::ToolCall tc;
        tc.id = "call-1";
        tc.name = "HashEdit";
        tc.arguments = R"({"path":"a.cpp"})";
        edit.tool_calls.push_back(std::move(tc));
        backend.scripted.push_back(edit);
        backend.scripted.push_back(ScriptedBackend::ok_text("all done"));
        backend.scripted.push_back(ScriptedBackend::ok_text("all done"));
        backend.scripted.push_back(ScriptedBackend::ok_text("all done"));
        AgentSession session;
        KimiSoul soul(session, backend, test_options());
        const auto out = soul.turn("patch the file");
        expect(out.ok);
        expect(out.content == "all done");
          expect(out.steps == 4_i); // edit step + 3 answers, 2 of them nudged
          // Same G9 interaction as above: the gate nudges are ephemeral
          // <system-reminder> user messages, stripped before the next step,
          // so none persist on the final history.
          expect(count_messages_with(session,
                                     "You modified code this turn but ran no "
                                     "verification (no tests/check commands).") ==
                 0_i);
          expect(count_messages_with(session, "(nudge 2/2 this turn)") == 0_i);
      };

    "verified_edits_pass_the_gate"_test = [] {
        ScriptedBackend backend;
        kimix::llm::ChatResult edit;
        edit.ok = true;
        kimix::llm::ToolCall tc;
        tc.id = "call-1";
        tc.name = "HashEdit";
        tc.arguments = R"({"path":"a.cpp"})";
        edit.tool_calls.push_back(std::move(tc));
        kimix::llm::ChatResult verify;
        verify.ok = true;
        kimix::llm::ToolCall vc;
        vc.id = "call-2";
        // "Run" is in the reference SHELL_TOOLS set but not registered here,
        // so it classifies as a verification hint without executing anything.
        vc.name = "Run";
        vc.arguments = R"({"command":"ctest"})";
        verify.tool_calls.push_back(std::move(vc));
        backend.scripted.push_back(edit);
        backend.scripted.push_back(verify);
        backend.scripted.push_back(ScriptedBackend::ok_text("all done"));
        AgentSession session;
        KimiSoul soul(session, backend, test_options());
        const auto out = soul.turn("patch and test");
        expect(out.ok);
        expect(out.content == "all done");
        expect(out.steps == 3_i); // no nudge: verification ran
        expect(count_messages_with(session, "verification gate findings:") == 0_i);
    };

    return 0;
}
