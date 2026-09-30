// agent/approval.cpp - The approval gate implementation (see approval.h).
//
// Port of kimi_cli/soul/approval.py (read in full) onto the native soul's
// synchronous turn: the reference's `await runtime.wait_for_response` becomes
// a blocking call into the host-installed approver callback (there is exactly
// one pending request at a time - the C++ turn loop is single-threaded), and
// the asyncio waiter bookkeeping / cancel_by_source lifecycle is deliberately
// not ported (documented in approval.h).
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers,
// no exceptions, no RTTI.

#include "agent/approval.h"

#include <random>

#include "agent/tool_taxonomy.h"
#include "llm/common.h"
#include "llm/yyjson_alc.h"
#include "yyjson.h"

namespace kimix::agent {

namespace {

// uuid.uuid4().hex analogue: 16 random bytes as 32 lowercase hex chars
// (the same shape new_compaction_id() produces in wire.cpp).
kimix::string random_hex_id() {
    std::random_device rd;
    kimix::string out;
    out.reserve(32);
    static constexpr char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        const unsigned b = static_cast<unsigned>(rd()) & 0xFFu;
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

const char *response_name(ApprovalResponse response) noexcept {
    switch (response) {
    case ApprovalResponse::approve:
        return "approve";
    case ApprovalResponse::approve_for_session:
        return "approve_for_session";
    case ApprovalResponse::reject:
        return "reject";
    }
    return "reject";
}

// True when `path` lies inside `work_dir` (the reference's is_within_workspace
// reduced to the session work dir; additional_dirs is a Python-only concept
// the native CLI does not model). Relative paths resolve against `work_dir`.
bool path_inside_work_dir(kimix::string_view work_dir, kimix::string_view path) {
    if (work_dir.empty()) {
        return false; // no workspace to be inside of: conservatively outside
    }
    std::error_code ec;
    // Narrow path construction goes through the ANSI code page and throws on
    // unrepresentable bytes (the 0xC0000409 crash class in this
    // exception-free binary). path_from_narrow() treats them as "does not
    // exist", which for an approval check is the conservative outside
    // direction.
    kimix::filesystem::path root;
    kimix::filesystem::path target;
    if (!kimix::path_from_narrow(work_dir, root) || !kimix::path_from_narrow(path, target)) {
        return false;
    }
    if (target.is_relative()) {
        target = root / target;
    }
    root = kimix::filesystem::weakly_canonical(root, ec);
    if (ec) {
        return false;
    }
    const kimix::filesystem::path abs = kimix::filesystem::weakly_canonical(target, ec);
    if (ec) {
        // A not-yet-existing file: canonicalize the parent and re-append the
        // leaf (write targets usually do not exist yet).
        const kimix::filesystem::path leaf = target.filename();
        const kimix::filesystem::path parent =
            kimix::filesystem::weakly_canonical(target.parent_path(), ec);
        if (ec) {
            return false;
        }
        const kimix::filesystem::path rel = parent.lexically_relative(root);
        return !rel.empty() && *rel.begin() != kimix::filesystem::path("..") &&
               !rel.has_root_path();
    }
    const kimix::filesystem::path rel = abs.lexically_relative(root);
    return !rel.empty() && (rel.begin() == rel.end() || *rel.begin() != kimix::filesystem::path("..")) &&
           !rel.has_root_path();
}

// First string value under one of the justification-ish keys the file tools
// accept ("justification"; a missing key yields "").
kimix::string justification_of(kimix::string_view arguments_json) {
    if (arguments_json.empty()) {
        return {};
    }
    kimix::string buf(arguments_json);
    yyjson_doc *doc = yyjson_read_opts(buf.data(), buf.size(), 0,
                                       &kimix::llm::kYYJsonAlcMi, nullptr);
    if (doc == nullptr) {
        return {};
    }
    kimix::string out;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *v = root != nullptr && yyjson_is_obj(root)
                        ? yyjson_obj_get(root, "justification")
                        : nullptr;
    if (v != nullptr && yyjson_is_str(v)) {
        out.assign(yyjson_get_str(v), yyjson_get_len(v));
    }
    yyjson_doc_free(doc);
    return out;
}

} // namespace

kimix::string new_approval_request_id() { return random_hex_id(); }

// ApprovalResult::rejection_message (approval.py:33-52, verbatim strings,
// incl. tools/utils.py ToolRejectedError's default wording).
kimix::string ApprovalResult::rejection_message(bool is_subagent) const {
    if (!feedback.empty()) {
        kimix::string msg = "The tool call is rejected by the user. User feedback: ";
        msg += feedback;
        return msg;
    }
    if (is_subagent) {
        return "The tool call is rejected by the user. Try a different approach "
               "to complete your task, or explain the limitation in your summary "
               "if no alternative is available. Do not retry the same tool call, "
               "and do not attempt to bypass this restriction through indirect "
               "means.";
    }
    return "Tool call rejected by user. Stop and wait for instructions.";
}

kimix::string ApprovalResult::rejection_brief() const {
    if (!feedback.empty()) {
        kimix::string brief = "Rejected: ";
        brief += feedback;
        return brief;
    }
    return "Rejected by user";
}

Approval::Approval() : _state(kimix::shared_ptr<ApprovalState>(new ApprovalState())) {}

Approval::Approval(kimix::shared_ptr<ApprovalState> state) : _state(std::move(state)) {
    if (_state == nullptr) {
        _state = kimix::shared_ptr<ApprovalState>(new ApprovalState());
    }
}

void Approval::set_yolo(bool yolo) {
    _state->yolo = yolo;
    if (_state->on_change) {
        _state->on_change(); // notify_change (approval.py:76-79)
    }
}

void Approval::set_afk(bool afk) {
    _state->afk = afk;
    if (!afk) {
        // Turning persisted afk off also clears the invocation-only overlay so
        // an interactive session started with a runtime flag returns to
        // interactive behavior (approval.py:107-117).
        _state->runtime_afk = false;
    }
    if (_state->on_change) {
        _state->on_change();
    }
}

void Approval::set_runtime_afk(bool afk) {
    _state->runtime_afk = afk; // never persisted, no notify_change
}

ApprovalResult Approval::request(kimix::string_view sender, kimix::string_view action,
                                 kimix::string_view description, bool is_subagent,
                                 kimix::string_view tool_call_id) {
    (void)is_subagent; // selects the rejection wording at the call site
    ApprovalResult out;
    // approval.py:185-187: afk (persisted or invocation-only) and yolo both
    // imply auto-approve.
    if (is_auto_approve()) {
        out.approved = true;
        return out;
    }
    // approval.py:189-191: an approve-for-session grant short-circuits the ask.
    if (_state->auto_approve_actions.find(kimix::string(action)) !=
        _state->auto_approve_actions.end()) {
        out.approved = true;
        return out;
    }
    const kimix::string id = new_approval_request_id();
    if (_wire != nullptr) {
        _wire->wire_approval_request(id, tool_call_id, sender, action, description);
    }
    _pending.push_back({id, kimix::string(action)});
    // The blocking wait for an answer (runtime.wait_for_response): the host's
    // callback prompts the user / consults the non-interactive policy. An
    // empty callback refuses everything - the safe default.
    const ApprovalResponse response =
        _approver ? _approver(sender, action, description, out.feedback)
                  : ApprovalResponse::reject;
    for (size_t i = 0; i < _pending.size(); ++i) {
        if (_pending[i].id == id) {
            _pending.erase(_pending.begin() + static_cast<ptrdiff_t>(i));
            break;
        }
    }
    if (_wire != nullptr) {
        _wire->wire_approval_response(id, response_name(response), out.feedback);
    }
    switch (response) {
    case ApprovalResponse::approve:
        out.approved = true;
        break;
    case ApprovalResponse::approve_for_session:
        // approval.py:218-224: remember the ACTION name, persist through
        // notify_change, and auto-resolve every already-pending request with
        // the same action (in the synchronous port only this request can be
        // pending, so the loop is kept for parity).
        _state->auto_approve_actions.insert(kimix::string(action));
        if (_state->on_change) {
            _state->on_change();
        }
        for (const pending_request &pending : _pending) {
            if (pending.action == action && _wire != nullptr) {
                _wire->wire_approval_response(pending.id, "approve", "");
            }
        }
        out.approved = true;
        break;
    case ApprovalResponse::reject:
        out.approved = false; // out.feedback carries the typed reason (G3)
        break;
    }
    return out;
}

bool approval_gated_tool(kimix::string_view canonical_tool_name) noexcept {
    // The reference gates the file tools (edit/base.py:148, write.py:355) and
    // the MCP/plugin tool wrappers (toolset.py:1998, plugin/tool.py:83); the
    // native registry's edit/write family (kEditTools) is the gated set. The
    // shell tools are NOT gated in the reference (bash_tool.py never calls
    // approval.request; their guard is the ported hardline list, G37).
    return is_edit_tool(canonical_tool_name);
}

kimix::string approval_action_for(kimix::string_view work_dir,
                                  kimix::string_view arguments_json) {
    // FileActions (tools/file/__init__.py:10-13): "edit file" inside the
    // session work dir, else "edit file outside working directory". A missing
    // / unparseable path conservatively yields the OUTSIDE action.
    const kimix::optional<kimix::string> path = path_params_of(arguments_json);
    if (!path.has_value() || path->empty()) {
        return "edit file outside working directory";
    }
    return path_inside_work_dir(work_dir, kimix::string_view(*path))
               ? kimix::string("edit file")
               : kimix::string("edit file outside working directory");
}

kimix::string approval_description_for(kimix::string_view canonical_tool_name,
                                       kimix::string_view arguments_json) {
    // The reference's prompt_text: "Write file `{path}`" / "Edit file
    // `{path}`", plus " — {justification}" when the arguments carry one
    // (write.py:352-354, edit/base.py:144-146). The raw argument path is shown
    // (not a resolved one), matching the reference's display_logical_path.
    const kimix::optional<kimix::string> path = path_params_of(arguments_json);
    kimix::string desc =
        canonical_tool_name == "write" ? kimix::string("Write file `")
                                       : kimix::string("Edit file `");
    if (path.has_value()) {
        desc += *path;
    }
    desc += "`";
    const kimix::string justification = justification_of(arguments_json);
    if (!justification.empty()) {
        desc += " — ";
        desc += justification;
    }
    return desc;
}

} // namespace kimix::agent
