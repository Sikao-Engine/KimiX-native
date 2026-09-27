// test_approval.cpp - Unit tests for the approval gate (src/agent/approval.*,
// gaps G1-G4).
//
// Coverage:
// * is_auto_approve(): yolo, persisted afk, invocation-only runtime_afk, and
//   set_afk(false) clearing the runtime overlay (approval.py:107-117);
// * the gate: auto-approve short-circuits, grant-set hit, empty approver
//   refuses everything (the safe default), approve / approve_for_session /
//   reject answers, feedback capture, pending-request count;
// * approve_for_session (G2): the action joins the grant set, notify_change
//   fires, later requests short-circuit without asking;
// * G3: the exact rejection wording (feedback / sub-agent / default) + brief;
// * wire records: one ApprovalRequest on creation, one ApprovalResponse on
//   resolution, both carrying the right fields;
// * dispatch classification: approval_gated_tool (edit/write family only -
//   the shell tools are NOT gated in the reference), approval_action_for
//   (inside/outside the work dir, missing path), approval_description_for
//   ("Write file `p` — justification").
//
// Framework: Boost.UT (tests/ut/ut.hpp).

#include "ut/ut.hpp"

#include <agent/approval.h>
#include <agent/soul.h>
#include <agent/wire.h>

#include <cstdint>
#include <cstdio>
#include <system_error>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

kimix::string cli_fwd_slashes(kimix::string s) {
    for (char &c : s) {
        if (c == '\\') {
            c = '/';
        }
    }
    return s;
}

kimix::filesystem::path test_dir(const char *name) {
    std::error_code ec;
    kimix::filesystem::path dir = kimix::filesystem::temp_directory_path(ec) /
                                  "kimix_approval_test" / name;
    kimix::filesystem::remove_all(dir, ec);
    kimix::filesystem::create_directories(dir, ec);
    return dir;
}

// A capture sink recording the approval pair (and nothing else).
struct approval_capture_sink : kimix::agent::WireSink {
    kimix::vector<kimix::string> requests;
    kimix::vector<kimix::string> responses;
    kimix::string last_request_id;
    kimix::string last_tool_call_id;
    kimix::string last_sender;
    kimix::string last_action;
    kimix::string last_description;
    kimix::string last_response;
    kimix::string last_feedback;

    void wire_turn_begin(kimix::string_view) override {}
    void wire_turn_end() override {}
    void wire_step_begin(int32_t) override {}
    void wire_step_interrupted() override {}
    void wire_steer_input(kimix::string_view) override {}
    void wire_step_retry(int32_t, int32_t, int32_t, double, kimix::string_view,
                         int32_t) override {}
    void wire_status_update(double, int64_t, int64_t, int64_t, int64_t, int64_t,
                            int64_t) override {}
    void wire_compaction_begin(kimix::string_view, kimix::string_view) override {}
    void wire_compaction_end(kimix::string_view, kimix::string_view, int64_t,
                             int64_t, kimix::string_view) override {}
    void wire_llm_request(const kimix::agent::llm_request_record &) override {}
    void wire_llm_tools_snapshot(kimix::string_view,
                                 const kimix::vector<kimix::llm::Tool> &) override {}
    void wire_mcp_tools_discovered(kimix::string_view, kimix::string_view,
                                   const kimix::vector<kimix::llm::Tool> &,
                                   const kimix::vector<kimix::string> &,
                                   const kimix::vector<kimix::string> &) override {}
    void wire_approval_request(kimix::string_view id, kimix::string_view tool_call_id,
                               kimix::string_view sender, kimix::string_view action,
                               kimix::string_view description) override {
        ++request_count;
        last_request_id = kimix::string(id);
        last_tool_call_id = kimix::string(tool_call_id);
        last_sender = kimix::string(sender);
        last_action = kimix::string(action);
        last_description = kimix::string(description);
        requests.push_back(last_action + "|" + last_description);
    }
    void wire_approval_response(kimix::string_view request_id,
                                kimix::string_view response,
                                kimix::string_view feedback) override {
        ++response_count;
        last_response = kimix::string(response);
        last_feedback = kimix::string(feedback);
        responses.push_back(last_response);
    }
    void wire_btw_begin(kimix::string_view, kimix::string_view) override {}
    void wire_btw_end(kimix::string_view, kimix::string_view,
                      kimix::string_view) override {}
    int request_count = 0;
    int response_count = 0;
};

} // namespace

int main() {
    using kimix::agent::Approval;
    using kimix::agent::ApprovalResponse;
    using kimix::agent::ApprovalResult;

    // ── Auto-approve modes (G4) ─────────────────────────────────────────
    "is_auto_approve_yolo_afk_runtime_afk"_test = [] {
        Approval gate;
        expect(!gate.is_auto_approve());
        gate.set_yolo(true);
        expect(gate.is_yolo());
        expect(gate.is_auto_approve());
        gate.set_yolo(false);
        gate.set_runtime_afk(true);
        expect(gate.is_runtime_afk());
        expect(gate.is_afk());
        expect(gate.is_auto_approve());
        gate.set_afk(true);
        expect(gate.is_afk_flag());
        gate.set_runtime_afk(false);
        expect(gate.is_afk()); // persisted afk still on
        expect(gate.is_auto_approve());
        // approval.py:114-117: set_afk(False) clears the invocation overlay.
        gate.set_afk(false);
        expect(!gate.is_afk());
        expect(!gate.is_auto_approve());
    };

    "auto_approve_short_circuits_without_asking"_test = [] {
        Approval gate;
        gate.set_yolo(true);
        bool asked = false;
        gate.set_approver([&asked](kimix::string_view, kimix::string_view,
                                   kimix::string_view, kimix::string &) {
            asked = true;
            return ApprovalResponse::reject;
        });
        const ApprovalResult r = gate.request("write", "edit file", "Write file `x`",
                                              false);
        expect(r.approved);
        expect(!asked);
        expect(gate.pending_requests() == 0u);
    };

    // ── The gate ─────────────────────────────────────────────────────────
    "empty_approver_refuses_everything"_test = [] {
        Approval gate; // no approver installed: the safe default
        approval_capture_sink sink;
        gate.set_wire_sink(&sink);
        const ApprovalResult r = gate.request("edit", "edit file", "Edit file `x`",
                                              false, "call_1");
        expect(!r.approved);
        expect(r.feedback.empty());
        expect(r.rejection_message(false) ==
               "Tool call rejected by user. Stop and wait for instructions.");
        expect(r.rejection_brief() == "Rejected by user");
        expect(sink.request_count == 1);
        expect(sink.response_count == 1);
        expect(sink.last_tool_call_id == "call_1");
    };

    "approve_and_reject_with_feedback"_test = [] {
        Approval gate;
        approval_capture_sink sink;
        gate.set_wire_sink(&sink);
        // First ask: approve. Second: reject with a typed reason (G3).
        int asks = 0;
        gate.set_approver([&asks](kimix::string_view, kimix::string_view,
                                  kimix::string_view, kimix::string &feedback) {
            ++asks;
            if (asks == 1) {
                return ApprovalResponse::approve;
            }
            feedback = "that file is generated, edit the template instead";
            return ApprovalResponse::reject;
        });
        const ApprovalResult ok = gate.request("edit", "edit file", "Edit file `a`",
                                               false);
        expect(ok.approved);
        expect(ok.feedback.empty());
        const ApprovalResult no = gate.request("edit", "edit file", "Edit file `b`",
                                               false);
        expect(!no.approved);
        expect(no.rejection_message(false) ==
               "The tool call is rejected by the user. User feedback: that file is "
               "generated, edit the template instead");
        expect(no.rejection_brief() ==
               "Rejected: that file is generated, edit the template instead");
        expect(asks == 2);
        expect(sink.request_count == 2);
        expect(sink.response_count == 2);
        expect(sink.last_response == "reject");
        expect(sink.last_feedback == "that file is generated, edit the template instead");
    };

    "subagent_rejection_wording_forbids_retry"_test = [] {
        ApprovalResult r;
        r.approved = false;
        expect(r.rejection_message(true) ==
               "The tool call is rejected by the user. Try a different approach to "
               "complete your task, or explain the limitation in your summary if no "
               "alternative is available. Do not retry the same tool call, and do "
               "not attempt to bypass this restriction through indirect means.");
        // The feedback variant wins even for sub-agents (approval.py:34-39).
        r.feedback = "no";
        expect(r.rejection_message(true) ==
               "The tool call is rejected by the user. User feedback: no");
    };

    // ── approve_for_session (G2) ─────────────────────────────────────────
    "approve_for_session_grants_action_and_notifies"_test = [] {
        Approval gate;
        approval_capture_sink sink;
        gate.set_wire_sink(&sink);
        int changes = 0;
        gate.state().on_change = [&changes] { ++changes; };
        int asks = 0;
        gate.set_approver([&asks](kimix::string_view, kimix::string_view action,
                                  kimix::string_view, kimix::string &) {
            ++asks;
            // The same action keeps asking until the grant lands.
            return action == "edit file"
                       ? ApprovalResponse::approve_for_session
                       : ApprovalResponse::approve;
        });
        const ApprovalResult first = gate.request("edit", "edit file", "Edit file `a`",
                                                  false);
        expect(first.approved);
        expect(changes == 1) << "notify_change fires on the grant";
        // The same action now short-circuits without asking.
        const ApprovalResult second = gate.request("edit", "edit file",
                                                   "Edit file `b`", false);
        expect(second.approved);
        expect(asks == 1) << "the grant set answers the second call";
        // A different action still asks.
        const ApprovalResult third = gate.request("edit", "edit file outside working "
                                                         "directory",
                                                  "Edit file `/etc/x`", false);
        expect(third.approved);
        expect(asks == 2);
        expect(gate.state().auto_approve_actions.count("edit file") == 1u);
    };

    "shared_state_between_gates"_test = [] {
        // approval.py share(): a child gate's grant feeds the parent's set.
        kimix::shared_ptr<kimix::agent::ApprovalState> state(
            new kimix::agent::ApprovalState());
        Approval parent(state);
        Approval child(state);
        child.state().auto_approve_actions.insert("edit file");
        expect(parent.is_auto_approve() == false);
        int asks = 0;
        parent.set_approver([&asks](kimix::string_view, kimix::string_view,
                                    kimix::string_view, kimix::string &) {
            ++asks;
            return ApprovalResponse::reject;
        });
        const ApprovalResult r = parent.request("edit", "edit file", "d", false);
        expect(r.approved);
        expect(asks == 0);
    };

    // ── Turn-level dispatch hook (G1 + G3 integration) ───────────────────
    "soul_turn_gates_gated_tools_and_feeds_back_rejection"_test = [] {
        // A scripted backend drives one step: the model calls `write`, the
        // gate asks, the user rejects with feedback. The tool must NOT run
        // (no file appears) and the history's tool message must carry the
        // reference rejection wording as the <system>ERROR: envelope.
        class one_shot_backend : public kimix::agent::IChatBackend {
        public:
            kimix::llm::ChatResult
            chat(const kimix::vector<kimix::llm::Message> &,
                 const kimix::vector<kimix::llm::Tool> &,
                 const kimix::llm::ChunkCallback &) override {
                kimix::llm::ChatResult r;
                r.ok = true;
                if (!sent) {
                    sent = true;
                    kimix::llm::ToolCall call;
                    call.id = "call_w1";
                    call.type = "function";
                    call.name = "write";
                    call.arguments = "{\"file_path\":\"never.txt\",\"content\":\"x\"}";
                    r.tool_calls.push_back(call);
                } else {
                    r.content = "understood";
                }
                return r;
            }
            kimix::string model_name() const override { return "approval-test"; }
            int64_t max_context_size() const override { return 100000; }
            bool sent = false;
        };

        const auto dir = test_dir("turn_gate");
        kimix::agent::AgentSession session(kimix::to_string(dir));
        one_shot_backend backend;
        kimix::agent::KimiSoul::options opts;
        opts.system_prompt = "test";
        opts.auto_compact = false;
        opts.enabled_tools = {"write"};
        kimix::agent::KimiSoul soul(session, backend, opts);

        kimix::agent::Approval gate;
        gate.set_yolo(false);
        kimix::string asked_action;
        kimix::string asked_description;
        gate.set_approver([&asked_action, &asked_description](
                              kimix::string_view, kimix::string_view action,
                              kimix::string_view description, kimix::string &feedback) {
            asked_action = kimix::string(action);
            asked_description = kimix::string(description);
            feedback = "do not write that file";
            return kimix::agent::ApprovalResponse::reject;
        });
        soul.set_approval(&gate);

        const kimix::agent::TurnResult result = soul.turn("please write the file");
        expect(result.ok);
        expect(backend.sent);
        expect(asked_action == "edit file") << "the action is the FileActions key";
        expect(asked_description == "Write file `never.txt`");
        // The tool never ran.
        std::error_code ec;
        expect(!kimix::filesystem::exists(dir / "never.txt", ec));
        // G3: the rejection reached the model as the error tool result.
        bool saw_rejection = false;
        for (const kimix::llm::Message &m : session.history()) {
            if (m.role == "tool" &&
                m.content.find("The tool call is rejected by the user. User feedback: "
                               "do not write that file") != kimix::string::npos &&
                m.content.find("<system>ERROR:") != kimix::string::npos) {
                saw_rejection = true;
            }
        }
        expect(saw_rejection) << "the typed reason feeds back as the tool error";
    };

    "soul_turn_auto_approves_under_yolo_and_grants"_test = [] {
        // yolo on: the gate never asks, the write executes.
        class one_shot_backend : public kimix::agent::IChatBackend {
        public:
            kimix::llm::ChatResult
            chat(const kimix::vector<kimix::llm::Message> &,
                 const kimix::vector<kimix::llm::Tool> &,
                 const kimix::llm::ChunkCallback &) override {
                kimix::llm::ChatResult r;
                r.ok = true;
                if (!sent) {
                    sent = true;
                    kimix::llm::ToolCall call;
                    call.id = "call_w2";
                    call.type = "function";
                    call.name = "write";
                    call.arguments = "{\"file_path\":\"yolo.txt\",\"content\":\"x\"}";
                    r.tool_calls.push_back(call);
                } else {
                    r.content = "done";
                }
                return r;
            }
            kimix::string model_name() const override { return "approval-test"; }
            int64_t max_context_size() const override { return 100000; }
            bool sent = false;
        };

        const auto dir = test_dir("turn_yolo");
        kimix::agent::AgentSession session(kimix::to_string(dir));
        one_shot_backend backend;
        kimix::agent::KimiSoul::options opts;
        opts.system_prompt = "test";
        opts.auto_compact = false;
        opts.enabled_tools = {"write"};
        kimix::agent::KimiSoul soul(session, backend, opts);
        kimix::agent::Approval gate;
        gate.set_yolo(true);
        int asks = 0;
        gate.set_approver([&asks](kimix::string_view, kimix::string_view,
                                  kimix::string_view, kimix::string &) {
            ++asks;
            return kimix::agent::ApprovalResponse::reject;
        });
        soul.set_approval(&gate);
        const kimix::agent::TurnResult result = soul.turn("write it");
        expect(result.ok);
        expect(asks == 0) << "yolo auto-approves without asking";
        std::error_code ec;
          expect(kimix::filesystem::exists(dir / "yolo.txt", ec))
              << "the write executed under yolo";
      };

    // ── Dispatch classification ──────────────────────────────────────────
    "gated_tools_are_the_edit_write_family"_test = [] {
        expect(kimix::agent::approval_gated_tool("write"));
        expect(kimix::agent::approval_gated_tool("edit"));
        expect(!kimix::agent::approval_gated_tool("bash"));
        expect(!kimix::agent::approval_gated_tool("pwsh"));
        expect(!kimix::agent::approval_gated_tool("read"));
        expect(!kimix::agent::approval_gated_tool("grep"));
    };

    "action_and_description_for_paths"_test = [] {
        const auto dir = test_dir("actions");
        const kimix::string work = kimix::to_string(dir);
        // JSON string values need forward slashes (a raw Windows path is an
        // invalid escape sequence).
        const kimix::string inside = cli_fwd_slashes(kimix::to_string(dir / "sub" / "f.txt"));
        // A path OUTSIDE the workspace must be absolute on the running platform:
        // "C:/other/x.txt" is only absolute on Windows.
#if defined(_WIN32)
        const kimix::string outside = "C:/other/x.txt";
#else
        const kimix::string outside = "/other/x.txt";
#endif
        // Inside the work dir (existing and not-yet-existing targets).
        expect(kimix::agent::approval_action_for(work,
                                                 "{\"path\":\"" + inside + "\"}") ==
               "edit file");
        expect(kimix::agent::approval_action_for(work,
                                                 "{\"file_path\":\"sub/new.txt\"}") ==
               "edit file");
        // Outside the work dir.
        expect(kimix::agent::approval_action_for(work,
                                                 "{\"path\":\"" + outside + "\"}") ==
               "edit file outside working directory");
        // Missing / unparseable path conservatively yields OUTSIDE.
        expect(kimix::agent::approval_action_for(work, "{}") ==
               "edit file outside working directory");
        expect(kimix::agent::approval_action_for(work, "not json") ==
               "edit file outside working directory");
        // Descriptions mirror the reference prompt_text.
        expect(kimix::agent::approval_description_for(
                   "write", "{\"file_path\":\"a.txt\"}") == "Write file `a.txt`");
        expect(kimix::agent::approval_description_for(
                   "edit", "{\"path\":\"a.txt\",\"justification\":\"fix typo\"}") ==
               "Edit file `a.txt` — fix typo");
    };
}
