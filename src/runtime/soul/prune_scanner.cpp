/*
 * prune_scanner.cpp - see prune_scanner.h (plan 015).
 *
 * Ports the O(t^2) pair scans of context_pruning.py to O(n) suffix
 * aggregates. Predicates verified against the reference source:
 *
 *   _is_superseded_read : role=tool, non-blank text; a later tool message
 *     with "Tool output is empty" OR with a shorter non-empty text
 *     (cp_len(later) < cp_len(text)//2) supersedes it.
 *   _is_resolved_error  : role=tool, text contains "<system>ERROR:"; a later
 *     tool message with non-blank text without that marker resolves it.
 *   _is_oversized_output: role=tool, max(cp_len(text)//4, 1) >= min_tokens.
 *   _is_ephemeral_message (Tier A): system reminders / notifications / task
 *     snapshots / D-Mail notices / checkpoints (per policy flags); task
 *     snapshots keep only the most recent one.
 *   _compute_protected_indices: stable head + last K user/assistant turns +
 *     tool-pair units (hash-map pass) + current turn.
 */

#include <runtime/soul/prune_scanner.h>

#include <limits>

#include <runtime/common/text_util.h>
#include <runtime/common/utf8.h>
#include <runtime/soul/soul_util.h>

namespace kimix {
namespace runtime {
namespace soul {
namespace {

// Concatenated TextPart text + code-point length (Python len()).
struct msg_text {
    kimix::string text;
    size_t cp_len = 0;
};

msg_text build_text(const message_view& m) noexcept {
    msg_text out;
    out.text = concat_text_parts(m);
    out.cp_len = common::utf8_code_point_count(out.text);
    return out;
}

bool is_task_snapshot(const message_view& m, const msg_text& t) noexcept {
    if (m.role != kRoleUser) {
        return false;
    }
    if (contains(t.text, "<active-background-tasks>")) {
        return true;
    }
    kimix::string lower;
    common::append_lower_ascii(lower, t.text);
    return contains(lower, "active background tasks");
}

bool is_dmail(const message_view& m, const msg_text& t) noexcept {
    return m.role == kRoleUser && contains(t.text, "D-Mail from your future self");
}

bool is_checkpoint(const message_view& m, const msg_text& t) noexcept {
    if (m.role != kRoleUser && m.role != kRoleSystem) {
        return false;
    }
    if (!contains(t.text, "CHECKPOINT")) {
        return false;
    }
    const kimix::string_view stripped = common::trim_py_ws(t.text);
    constexpr kimix::string_view kMarker = "<system>CHECKPOINT";
    return stripped.substr(0, kMarker.size()) == kMarker ||
           contains(stripped, kMarker);
}

// Tier A predicates with the policy's per-category flags.
bool is_ephemeral(const message_view& m, const msg_text& t,
                  const prune_policy& policy) noexcept {
    if (is_system_reminder_msg(m)) {
        return true;
    }
    if (policy.drop_notifications && is_notification_msg(m)) {
        return true;
    }
    if (policy.drop_task_snapshots && is_task_snapshot(m, t)) {
        return true;
    }
    if (policy.drop_dmail && is_dmail(m, t)) {
        return true;
    }
    if (policy.drop_checkpoints && is_checkpoint(m, t)) {
        return true;
    }
    return false;
}

} // namespace

void PruneScanner::scan(kimix::span<const message_view> msgs,
                        const prune_policy& policy,
                        kimix::vector<prune_action>& out) const noexcept {
    out.clear();
    const size_t n = msgs.size();
    if (n == 0) {
        return;
    }

    // ---- per-message text -------------------------------------------------
    kimix::vector<msg_text> texts;
    texts.reserve(n);
    for (const message_view& m : msgs) {
        texts.push_back(build_text(m));
    }

    // ---- protected set (_compute_protected_indices) -----------------------
    kimix::bitvector protected_flag(n, false);

    // Stable head.
    const size_t head = (std::min)(static_cast<size_t>(policy.stable_prefix_messages), n);
    for (size_t i = 0; i < head; ++i) {
        protected_flag[i] = true;
    }

    // Recent tail: last K user/assistant turns.
    size_t tail_count = 0;
    for (size_t i = n; i-- > 0;) {
        if (tail_count >= policy.recent_messages_protected) {
            break;
        }
        if (msgs[i].role == kRoleUser || msgs[i].role == kRoleAssistant) {
            protected_flag[i] = true;
            ++tail_count;
        }
    }

    // Tool-pair protection (_protect_tool_pair_indices) via one hash pass:
    // collect every tool-call id of protected assistant messages, then a
    // single scan protects matching role=tool results.
    kimix::set<kimix::string> pair_ids;
    for (size_t i = 0; i < n; ++i) {
        if (!protected_flag[i] || msgs[i].role != kRoleAssistant) {
            continue;
        }
        for (const tool_call_view& tc : msgs[i].tool_calls) {
            if (!tc.id.empty()) {
                pair_ids.insert(kimix::string(tc.id));
            }
        }
    }
    for (size_t j = 0; j < n; ++j) {
        if (protected_flag[j]) {
            continue;
        }
        if (msgs[j].role == kRoleTool && !msgs[j].tool_call_id.empty() &&
            pair_ids.find(kimix::string(msgs[j].tool_call_id)) != pair_ids.end()) {
            protected_flag[j] = true;
        }
    }

    // Current turn (everything from current_turn_index onward).
    if (policy.current_turn_index != UINT32_MAX) {
        for (size_t i = (std::min)(static_cast<size_t>(policy.current_turn_index), n);
             i < n; ++i) {
            protected_flag[i] = true;
        }
    }

    // ---- Tier A snapshot bookkeeping --------------------------------------
    // Latest non-protected ephemeral task snapshot (kept, never a candidate).
    size_t latest_snapshot = n; // none
    size_t snapshot_count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (protected_flag[i]) {
            continue;
        }
        if (policy.drop_task_snapshots && is_task_snapshot(msgs[i], texts[i])) {
            ++snapshot_count;
            latest_snapshot = i;
        }
    }

    // ---- O(n) suffix aggregates over tool messages ------------------------
    // state for j > i: any later non-blank tool text with the empty-output
    // marker; min later non-blank tool text length; any later non-blank tool
    // text without the ERROR marker.
    kimix::vector<bool> later_empty_marker(n, false);
    kimix::vector<size_t> later_min_len(n, std::numeric_limits<size_t>::max());
    kimix::vector<bool> later_has_success(n, false);
    {
        bool empty_marker_run = false;
        size_t min_len_run = std::numeric_limits<size_t>::max();
        bool success_run = false;
        for (size_t i = n; i-- > 0;) {
            later_empty_marker[i] = empty_marker_run;
            later_min_len[i] = min_len_run;
            later_has_success[i] = success_run;
            if (msgs[i].role == kRoleTool && !texts[i].text.empty()) {
                if (contains(texts[i].text, "Tool output is empty")) {
                    empty_marker_run = true;
                }
                min_len_run = (std::min)(min_len_run, texts[i].cp_len);
                if (!contains(texts[i].text, "<system>ERROR:")) {
                    success_run = true;
                }
            }
        }
    }

    // ---- classify every index (index-ascending) ---------------------------
    for (size_t i = 0; i < n; ++i) {
        prune_action action;
        action.index = static_cast<uint32_t>(i);
        if (protected_flag[i]) {
            action.reason = kPruneProtect;
            out.push_back(action);
            continue;
        }

        const message_view& msg = msgs[i];
        const msg_text& t = texts[i];

        // Tier A: ephemeral (drop). Task snapshots keep the most recent one.
        if (is_ephemeral(msg, t, policy)) {
            if (is_task_snapshot(msg, t)) {
                if (policy.drop_task_snapshots && snapshot_count >= 2 &&
                    i != latest_snapshot) {
                    action.reason = kPruneCompact;
                    out.push_back(action);
                }
                // else: kept (single / latest snapshot)
            } else {
                action.reason = kPruneCompact;
                out.push_back(action);
            }
            continue;
        }

        if (msg.role != kRoleTool) {
            continue; // Tier B only ever elides tool results
        }
        const bool has_text = !t.text.empty() && !common::empty_after_trim(t.text);

        // Tier B classification order (Python: superseded -> oversized ->
        // resolved; first match wins).
        if (has_text &&
            (later_empty_marker[i] || later_min_len[i] < t.cp_len / 2)) {
            action.reason = kPruneSupersededRead;
            out.push_back(action);
            continue;
        }
        const size_t token_count = (std::max)(t.cp_len / 4, size_t{1});
        if (token_count >= policy.tool_output_min_tokens) {
            action.reason = kPruneOversizedOutput;
            out.push_back(action);
            continue;
        }
        if (has_text && contains(t.text, "<system>ERROR:") &&
            later_has_success[i]) {
            action.reason = kPruneResolvedError;
            out.push_back(action);
        }
    }
}

} // namespace soul
} // namespace runtime
} // namespace kimix
