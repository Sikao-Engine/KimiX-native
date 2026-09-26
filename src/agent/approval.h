// agent/approval.h - The approval gate (gaps G1-G4).
//
// Port of kimi_cli/soul/approval.py (228 lines, read in full) +
// kimi_cli/approval_runtime/{models,runtime}.py onto the native soul's
// synchronous turn:
//   * ApprovalState - yolo / afk (persisted session flag) / runtime_afk
//     (invocation-only, e.g. --print; never persisted) + the
//     approve-for-session grant set keyed on the ACTION name
//     (approval.py:55-79, session_state.py ApprovalStateData);
//   * Approval::request() - the gate (approval.py:151-228): short-circuits on
//     is_auto_approve() and on the grant set, otherwise asks the caller's
//     approver callback and blocks until an answer arrives (the reference's
//     `await runtime.wait_for_response` becomes a synchronous callback wait -
//     the C++ turn is a plain loop on one thread, so there is exactly one
//     pending request at a time; the runtime's asyncio.Future waiters collapse
//     into the callback). `approve_for_session` adds the action to the grant
//     set, persists through on_change and resolves every already-pending
//     request with the same action (approval.py:218-224);
//   * ApprovalResult + the rejection wording (approval.py:33-52 incl. the
//     sub-agent variant that forbids retry/bypass, and ToolRejectedError's
//     default "Tool call rejected by user. Stop and wait for instructions.");
//   * wire records: ApprovalRequest on creation and ApprovalResponse on
//     resolution (approval_runtime/runtime.py _publish_wire_request/_publish_
//     wire_response), so UI/web clients see the same pair the reference sends.
//
// Deliberately NOT ported (documented): the asyncio waiter bookkeeping
// (waiter_counts/timeout cancel - vacuous with one synchronous waiter),
// cancel_by_source (no background-agent approval lifecycle port), the afk
// slash command and afk persistence modes (per the phase brief), /yolo.
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers,
// no exceptions, no RTTI.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "agent/wire.h"

namespace kimix::agent {

// ---------------------------------------------------------------------------
// Response + result
// ---------------------------------------------------------------------------

// The approver's answer (approval.py Response Literal).
enum class ApprovalResponse : uint8_t {
    approve,
    approve_for_session,
    reject,
};

// ApprovalResult (approval.py:21-52). Behaves as bool: `if (!result)` is a
// rejection. `feedback` is the user's typed rejection reason (empty when the
// approver gave none).
struct ApprovalResult {
    bool approved = false;
    kimix::string feedback;

    explicit operator bool() const noexcept { return approved; }

    // G3: the ToolRejectedError message the model sees as the tool result
    // (approval.py:33-52, tools/utils.py ToolRejectedError default wording).
    kimix::string rejection_message(bool is_subagent) const;
    // The reference's `brief`: "Rejected: {feedback}" / "Rejected by user".
    kimix::string rejection_brief() const;
};

// The decision channel the host installs. Called with (sender, action,
// description); fills `feedback` on a rejection-with-reason and returns the
// response. The gate blocks inside this callback - the interactive CLI
// prompts y/n here, scripted tests inject answers, a non-interactive host
// refuses. An empty callback refuses everything (safe default).
using ApprovalApprover =
    kimix::function<ApprovalResponse(kimix::string_view sender,
                                     kimix::string_view action,
                                     kimix::string_view description,
                                     kimix::string &feedback)>;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

// Port of ApprovalState (approval.py:55-79). `afk` is the persisted session
// flag; `runtime_afk` is invocation-only (--print style) and is never written
// to state.json. `on_change` fires after every mutation: the CLI installs the
// state.json persistence there (agent.py:_on_approval_change).
struct ApprovalState {
    bool yolo = false;
    bool afk = false;         // persisted; "no user present" -> auto-approve
    bool runtime_afk = false; // invocation-only; NOT persisted
    // The approve-for-session grant set: action names auto-approved for the
    // rest of the session (approval.py:72).
    kimix::unordered_set<kimix::string, kimix::string_hash> auto_approve_actions;
    kimix::function<void()> on_change;
};

// ---------------------------------------------------------------------------
// Approval
// ---------------------------------------------------------------------------

// The gate (approval.py class Approval). Owns a SHARED state so sub-agents
// can share the parent's decisions (approval.py share(), agent.py:225
// `approval=self.approval.share()`): a grant - or a yolo toggle - on the
// parent applies to every child, and a child's approve_for_session feeds the
// same persisted set.
class Approval {
public:
    Approval();
    explicit Approval(kimix::shared_ptr<ApprovalState> state);

    // share() (approval.py:92-94): point this gate at an existing state.
    void share_state(kimix::shared_ptr<ApprovalState> state) {
        _state = std::move(state);
    }
    kimix::shared_ptr<ApprovalState> shared_state() const { return _state; }
    ApprovalState &state() { return *_state; }
    const ApprovalState &state() const { return *_state; }

    void set_approver(ApprovalApprover approver) { _approver = std::move(approver); }
    void set_wire_sink(WireSink *sink) noexcept { _wire = sink; }

    // Mode setters; every mutation fires state.on_change (notify_change,
    // approval.py:76-79) so the grant set and flags persist immediately.
    void set_yolo(bool yolo);
    // set_afk(False) also clears the invocation-only overlay so an
    // interactive session started with a runtime flag returns to interactive
    // behavior (approval.py:107-117).
    void set_afk(bool afk);
    void set_runtime_afk(bool afk);

    bool is_yolo() const noexcept { return _state->yolo; }
    // True when no user is present: persisted afk OR the invocation overlay
    // (approval.py:139-141).
    bool is_afk() const noexcept { return _state->afk || _state->runtime_afk; }
    bool is_afk_flag() const noexcept { return _state->afk; }
    bool is_runtime_afk() const noexcept { return _state->runtime_afk; }
    // True when tool calls should be auto-approved: yolo or afk
    // (approval.py:123-129).
    bool is_auto_approve() const noexcept { return _state->yolo || is_afk(); }

    // The gate (approval.py request(), :151-228). `sender` is the tool name,
    // `action` the grant key ("edit file", "edit file outside working
    // directory", "mcp:<name>"), `description` the human-readable one-liner,
    // `tool_call_id` the wire id of the in-flight call (the ApprovalRequest
    // record's tool_call_id). Returns approved=true without asking when yolo/
    // afk is on or the action was granted for the session; otherwise publishes
    // the ApprovalRequest wire record, blocks on the approver callback and
    // publishes the ApprovalResponse. `is_subagent` selects the rejection
    // wording (G3).
    ApprovalResult request(kimix::string_view sender, kimix::string_view action,
                           kimix::string_view description, bool is_subagent,
                           kimix::string_view tool_call_id = {});

    // Number of requests currently awaiting an answer (the runtime's
    // list_pending().size(); 0 or 1 in the synchronous port).
    size_t pending_requests() const noexcept { return _pending.size(); }

private:
    struct pending_request {
        kimix::string id;
        kimix::string action;
    };

    kimix::shared_ptr<ApprovalState> _state;
    WireSink *_wire = nullptr;
    ApprovalApprover _approver;
    kimix::vector<pending_request> _pending;
};

// A fresh approval request id (uuid.uuid4().hex analogue: 32 lowercase hex
// chars), like new_compaction_id() in wire.cpp.
kimix::string new_approval_request_id();

// ---------------------------------------------------------------------------
// Dispatch gating (soul.cpp's execute_tool_call hook)
// ---------------------------------------------------------------------------

// True when `name` (a registry-canonical tool name) is approval-gated: the
// reference gates the file tools (tools/file/edit/base.py:148, write.py:355)
// and MCP tools (toolset.py:1998); the native registry resolves both edit and
// write through the kEditTools taxonomy, so that is the gated set.
bool approval_gated_tool(kimix::string_view canonical_tool_name) noexcept;

// The FileActions action for a target path (tools/file/__init__.py:10-13):
// "edit file" inside the session work dir, else "edit file outside working
// directory". `arguments_json` is the (repaired) tool argument JSON; a
// missing/unparseable path conservatively yields the OUTSIDE action.
kimix::string approval_action_for(kimix::string_view work_dir,
                                  kimix::string_view arguments_json);

// The one-line description the interactive prompt shows, mirroring the
// reference's prompt_text: "Write file `{path}`" / "Edit file `{path}`",
// plus " — {justification}" when the arguments carry one.
kimix::string approval_description_for(kimix::string_view canonical_tool_name,
                                       kimix::string_view arguments_json);

} // namespace kimix::agent
