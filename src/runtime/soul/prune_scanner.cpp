/*
 * prune_scanner.cpp - see prune_scanner.h (plans 015/017).
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

// Concatenated TextPart text + code-point length (Python len()), plus the
// first content part's text/length (Tier A savings uses content[0].text).
struct msg_text {
    kimix::string text;
    size_t cp_len = 0;
    kimix::string first_text;
    size_t first_cp_len = 0;
};

msg_text build_text(const message_view& m) noexcept {
    msg_text out;
    out.text = concat_text_parts(m);
    out.cp_len = common::utf8_code_point_count(out.text);
    if (!m.parts.empty()) {
        out.first_text = kimix::string(m.parts[0].text);
        out.first_cp_len = common::utf8_code_point_count(out.first_text);
    }
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

// Compute protected set exactly like _compute_protected_indices +
// _protect_tool_pair_indices.
void compute_protected_set(kimix::span<const message_view> msgs,
                           const prune_policy& policy,
                           kimix::bitvector& protected_flag) noexcept {
    const size_t n = msgs.size();
    protected_flag.assign(n, false);

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

    // Tool-pair protection via one hash pass. Membership-only lookups, so
    // the dense unordered_set is semantically identical to the tree set
    // (kimix::string has no kimix::hash specialization; string_hash is the
    // codebase convention).
    kimix::unordered_set<kimix::string, kimix::string_hash> pair_ids;
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

    // ---- protected set ----------------------------------------------------
    kimix::bitvector protected_flag;
    compute_protected_set(msgs, policy, protected_flag);

    // ---- Tier A snapshot bookkeeping --------------------------------------
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
    // uint8_t flags (std::vector<bool> would be bit-packed and slower).
    kimix::vector<uint8_t> later_empty_marker(n, 0);
    kimix::vector<size_t> later_min_len(n, std::numeric_limits<size_t>::max());
    kimix::vector<uint8_t> later_has_success(n, 0);
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

void PruneScanner::prune_history(kimix::span<const message_view> msgs,
                                 const prune_policy& policy,
                                 prune_history_result& out) const noexcept {
    out = {};
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

    // ---- protected set ----------------------------------------------------
    kimix::bitvector protected_flag;
    compute_protected_set(msgs, policy, protected_flag);

    // ---- Tier A snapshot bookkeeping --------------------------------------
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
    // uint8_t flags (std::vector<bool> would be bit-packed and slower).
    kimix::vector<uint8_t> later_empty_marker(n, 0);
    kimix::vector<size_t> later_min_len(n, std::numeric_limits<size_t>::max());
    kimix::vector<uint8_t> later_has_success(n, 0);
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

    // ---- collect Tier A and Tier B candidates -----------------------------
    struct candidate {
        uint32_t index;
        uint32_t savings;
        uint8_t reason;
        bool tier_a;
    };
    kimix::vector<candidate> candidates;

    // Tier A: ephemeral drops.
    for (size_t i = 0; i < n; ++i) {
        if (protected_flag[i]) {
            continue;
        }
        if (!is_ephemeral(msgs[i], texts[i], policy)) {
            continue;
        }
        const uint32_t savings = static_cast<uint32_t>(
            (std::max)(texts[i].first_cp_len / 4, size_t{1}));
        if (is_task_snapshot(msgs[i], texts[i])) {
            if (policy.drop_task_snapshots && snapshot_count >= 2 &&
                i != latest_snapshot) {
                candidates.push_back(
                    {static_cast<uint32_t>(i), savings, kPruneCompact, true});
            }
        } else {
            candidates.push_back(
                {static_cast<uint32_t>(i), savings, kPruneCompact, true});
        }
    }

    // Tier B: substantive elision (superseded -> oversized -> resolved).
    for (size_t i = 0; i < n; ++i) {
        if (protected_flag[i]) {
            continue;
        }
        if (msgs[i].role != kRoleTool) {
            continue;
        }
        const bool has_text =
            !texts[i].text.empty() && !common::empty_after_trim(texts[i].text);
        const uint32_t token_count = static_cast<uint32_t>(
            (std::max)(texts[i].cp_len / 4, size_t{1}));

        if (policy.superseded_read_enabled && has_text &&
            (later_empty_marker[i] || later_min_len[i] < texts[i].cp_len / 2)) {
            candidates.push_back({static_cast<uint32_t>(i), token_count,
                                  kPruneSupersededRead, false});
            continue;
        }
        if (policy.oversized_output_enabled &&
            token_count >= policy.tool_output_min_tokens) {
            candidates.push_back({static_cast<uint32_t>(i), token_count,
                                  kPruneOversizedOutput, false});
            continue;
        }
        if (policy.stale_tool_result_enabled && has_text &&
            contains(texts[i].text, "<system>ERROR:") && later_has_success[i]) {
            candidates.push_back({static_cast<uint32_t>(i), token_count,
                                  kPruneResolvedError, false});
        }
    }

    if (candidates.empty()) {
        return;
    }

    // ---- tail-inward greedy selection (latest first, Tier A before Tier B,
    //      highest savings first) -------------------------------------------
    std::sort(candidates.begin(), candidates.end(),
              [](const candidate& a, const candidate& b) {
                  if (a.index != b.index) {
                      return a.index > b.index;
                  }
                  if (a.tier_a != b.tier_a) {
                      return a.tier_a > b.tier_a;
                  }
                  return a.savings > b.savings;
              });

    kimix::vector<candidate> selected;
    uint32_t total_freed = 0;
    for (const candidate& c : candidates) {
        if (total_freed >= policy.max_elision_tokens) {
            break;
        }
        selected.push_back(c);
        total_freed += c.savings;
    }

    if (selected.empty()) {
        return;
    }

    // ---- emit actions in index-ascending order ----------------------------
    std::sort(selected.begin(), selected.end(),
              [](const candidate& a, const candidate& b) {
                  return a.index < b.index;
              });

    out.actions.reserve(selected.size());
    for (const candidate& c : selected) {
        prune_history_action a;
        a.index = c.index;
        a.reason = c.reason;
        a.savings = c.savings;
        out.actions.push_back(a);
    }
    out.freed_tokens = total_freed;
    out.earliest_removed_index = selected[0].index;
}

} // namespace soul
} // namespace runtime
} // namespace kimix
