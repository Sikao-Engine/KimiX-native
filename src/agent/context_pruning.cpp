// agent/context_pruning.cpp - ContextPruner implementation (see context_pruning.h).
//
// Faithful port of kimi_cli/soul/context_pruning.py: every detector, gate,
// sort key and user-visible literal mirrors the reference. Where the C++
// message model flattens TextParts into one content string, the notes say how
// each parts-based rule maps onto the flat form.

#include "agent/context_pruning.h"

#include <algorithm>
#include <string>

#include <core/string_scratch.h>
#include <runtime/common/text_util.h>
#include <runtime/tools/compress.h>
#include "agent/dynamic_injection.h"
#include "builtin_tools/compact_tool.h"

namespace kimix::agent {

namespace {

// ── Text helpers ─────────────────────────────────────────────────────────────

// Python str.strip()-ish view trim (the reference lstrips/strips ASCII-ish
// whitespace; full py-space tables are unnecessary for the fixed markers).
kimix::string_view lstrip_ws(kimix::string_view s) noexcept {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' ||
                            s[i] == '\r' || s[i] == '\v' || s[i] == '\f')) {
        ++i;
    }
    return s.substr(i);
}

kimix::string_view rstrip_ws(kimix::string_view s) noexcept {
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' ||
                     s[e - 1] == '\r' || s[e - 1] == '\v' || s[e - 1] == '\f')) {
        --e;
    }
    return s.substr(0, e);
}

kimix::string_view strip_ws(kimix::string_view s) noexcept {
    return rstrip_ws(lstrip_ws(s));
}

bool contains_ci(kimix::string_view haystack, kimix::string_view needle) noexcept {
    if (needle.empty() || haystack.size() < needle.size()) {
        return false;
    }
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        size_t j = 0;
        while (j < needle.size()) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') {
                a = char(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = char(b - 'A' + 'a');
            }
            if (a != b) {
                break;
            }
            ++j;
        }
        if (j == needle.size()) {
            return true;
        }
    }
    return false;
}

// max(len//4, 1) - the reference's flat savings estimate.
int64_t flat_token_savings(size_t chars) noexcept {
    return std::max<int64_t>(static_cast<int64_t>(chars / 4), 1);
}

// kimix::string from an integer (std::to_string returns a std::string, which
// never mixes into kimix::string concatenations).
kimix::string kstr(int64_t v) {
    const std::string s = std::to_string(v);
    return kimix::string(s.c_str(), s.size());
}

// count_message_tokens(history): the soul's language-aware estimator over the
// text parts (thinking parts are NOT TextParts in the reference either).
int64_t count_message_tokens(const kimix::vector<kimix::llm::Message> &history) {
    using namespace builtin_tools::compact;
    kimix::vector<message> cms;
    cms.reserve(history.size());
    for (const kimix::llm::Message &m : history) {
        message cm;
        cm.role = m.role;
        cm.tool_call_count = static_cast<int32_t>(m.tool_calls.size());
        if (!m.content.empty()) {
            content_part tp;
            tp.type = "text";
            tp.text = m.content;
            cm.content.push_back(std::move(tp));
        }
        cms.push_back(std::move(cm));
    }
    return estimate_message_tokens(kimix::span<const message>(cms.data(),
                                                              static_cast<int64_t>(cms.size())));
}

int64_t count_message_tokens(kimix::span<const kimix::llm::Message> history) {
    kimix::vector<kimix::llm::Message> copy(history.begin(), history.end());
    return count_message_tokens(copy);
}

// ── Leading <system>…</system> block handling (flat message model) ───────────
//
// The reference treats TextParts whose stripped text starts with "<system>" as
// metadata. The flattened C++ content joins parts with "\n", so a message may
// open with one or more "<system>…</system>" lines before the real output.
// Returns the byte length of the leading system-block prefix (including the
// join newlines), 0 when the content does not start with one.

size_t leading_system_prefix_len(kimix::string_view content) noexcept {
    size_t pos = 0;
    for (;;) {
        const kimix::string_view rest = content.substr(pos);
        if (rest.empty()) {
            break;
        }
        // The block must start at the current position (possibly after the
        // join newline consumed below).
        kimix::string_view line = rest;
        const size_t nl = rest.find('\n');
        if (nl != kimix::string_view::npos) {
            line = rest.substr(0, nl);
        }
        kimix::string_view t = strip_ws(line);
        if (t.substr(0, 8) != "<system>") {
            break;
        }
        // The block may span lines (DOTALL); find the closing tag from here.
        const size_t close = content.find("</system>", pos);
        if (close == kimix::string_view::npos) {
            break;
        }
        pos = close + 9;
        // Consume exactly one join newline after the block.
        if (pos < content.size() && content[pos] == '\n') {
            ++pos;
        }
    }
    return pos;
}

// Inner text of a "<system>…</system>" block (reference _extract_system_text).
kimix::optional<kimix::string> system_block_inner(kimix::string_view content) noexcept {
    const kimix::string_view t = strip_ws(content);
    if (t.substr(0, 8) != "<system>") {
        return kimix::optional<kimix::string>();
    }
    if (t.size() < 9 + 9 || t.substr(t.size() - 9) != "</system>") {
        return kimix::optional<kimix::string>();
    }
    kimix::string inner(t.substr(8, t.size() - 17));
    return kimix::optional<kimix::string>(strip_ws(inner));
}

// ── Tier C candidate detection ───────────────────────────────────────────────

// ReadFile-style line-number prefix ^\s*(\d+)\t on one line.
bool line_has_lineno_prefix(kimix::string_view line) noexcept {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    bool any = false;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
        ++i;
        any = true;
    }
    return any && i < line.size() && line[i] == '\t';
}

// The concatenated non-<system> text of a tool message (reference _output_text
// over the flattened parts).
kimix::string output_text(const kimix::llm::Message &message) {
    const size_t prefix = leading_system_prefix_len(message.content);
    kimix::string out;
    if (prefix < message.content.size()) {
        out.assign(message.content.data() + prefix, message.content.size() - prefix);
    }
    return out;
}

struct tier_c_candidate {
    int64_t index = 0;
    int64_t savings = 0;
    kimix::string kind;
};

// _tier_c_candidates: stale role=="tool" messages outside `excluded` whose
// output text compresses by at least min_saved_chars characters.
kimix::vector<tier_c_candidate>
tier_c_candidates(const kimix::vector<kimix::llm::Message> &history,
                  const kimix::set<int64_t> &excluded, int32_t min_saved_chars) {
    kimix::vector<tier_c_candidate> out;
    for (int64_t i = 0; i < static_cast<int64_t>(history.size()); ++i) {
        if (excluded.count(i) != 0) {
            continue;
        }
        const kimix::llm::Message &msg = history[static_cast<size_t>(i)];
        if (msg.role != "tool") {
            continue;
        }
        const kimix::string text = output_text(msg);
        if (strip_ws(text).empty()) {
            continue;
        }
        const kimix::string kind =
            looks_like_readfile_output(text) ? kimix::string("code") : kimix::string("log");
        const kimix::string compressed = micro_compress_text(text, kind);
        const int64_t saved = static_cast<int64_t>(text.size()) -
                              static_cast<int64_t>(compressed.size());
        if (saved >= min_saved_chars) {
            tier_c_candidate c;
            c.index = i;
            c.savings = std::max<int64_t>(saved / 4, 1);
            c.kind = kind;
            out.push_back(std::move(c));
        }
    }
    return out;
}

// ── Tier A candidate selection ───────────────────────────────────────────────

struct tier_a_candidate {
    int64_t index = 0;
    int64_t savings = 0;
};

kimix::vector<tier_a_candidate>
tier_a_candidates(const kimix::vector<kimix::llm::Message> &history,
                  const kimix::set<int64_t> &protected_indices, bool drop_notifications,
                  bool drop_task_snapshots, bool drop_checkpoints) {
    kimix::vector<tier_a_candidate> candidates;
    kimix::vector<int64_t> ephemeral_indices;
    for (int64_t i = 0; i < static_cast<int64_t>(history.size()); ++i) {
        if (protected_indices.count(i) != 0) {
            continue;
        }
        if (is_ephemeral_message(history[static_cast<size_t>(i)], drop_notifications,
                                 drop_task_snapshots, drop_checkpoints)) {
            ephemeral_indices.push_back(i);
        }
    }
    if (ephemeral_indices.empty()) {
        return candidates;
    }
    // Keep-newest for task snapshots: drop every snapshot except max(index).
    if (drop_task_snapshots) {
        kimix::vector<int64_t> snapshot_indices;
        for (const int64_t i : ephemeral_indices) {
            if (is_active_task_snapshot_message(history[static_cast<size_t>(i)])) {
                snapshot_indices.push_back(i);
            }
        }
        if (snapshot_indices.size() > 1) {
            const int64_t latest = *std::max_element(snapshot_indices.begin(),
                                                     snapshot_indices.end());
            for (const int64_t idx : snapshot_indices) {
                if (idx != latest) {
                    const kimix::llm::Message &m = history[static_cast<size_t>(idx)];
                    candidates.push_back({idx, flat_token_savings(m.content.size())});
                }
            }
        }
    }
    // All other ephemera (snapshots handled above are skipped here).
    for (const int64_t idx : ephemeral_indices) {
        if (is_active_task_snapshot_message(history[static_cast<size_t>(idx)])) {
            continue;
        }
        const kimix::llm::Message &m = history[static_cast<size_t>(idx)];
        candidates.push_back({idx, flat_token_savings(m.content.size())});
    }
    return candidates;
}

// ── Tier B detectors ─────────────────────────────────────────────────────────

struct tier_b_candidate {
    int64_t index = 0;
    int64_t savings = 0;
    kimix::string kind;
};

// (is_match, savings) helpers scanning later tool results.
bool later_tool_result_matches(const kimix::vector<kimix::llm::Message> &history,
                               int64_t index,
                               const kimix::function<bool(kimix::string_view)> &pred) {
    for (int64_t j = index + 1; j < static_cast<int64_t>(history.size()); ++j) {
        if (history[static_cast<size_t>(j)].role != "tool") {
            continue;
        }
        const kimix::string_view later = history[static_cast<size_t>(j)].content;
        if (!later.empty() && pred(later)) {
            return true;
        }
    }
    return false;
}

kimix::vector<tier_b_candidate>
tier_b_candidates(const kimix::vector<kimix::llm::Message> &history,
                  const kimix::set<int64_t> &protected_indices, int32_t min_output_tokens) {
    kimix::vector<tier_b_candidate> candidates;
    for (int64_t i = 0; i < static_cast<int64_t>(history.size()); ++i) {
        if (protected_indices.count(i) != 0) {
            continue;
        }
        const kimix::llm::Message &msg = history[static_cast<size_t>(i)];
        if (msg.role != "tool") {
            continue;
        }
        const kimix::string_view text = msg.content;
        if (strip_ws(text).empty()) {
            continue;
        }
        const int64_t savings = flat_token_savings(text.size());
        // superseded_read: a later tool result says "Tool output is empty" or
        // is shorter than half of this one.
        if (later_tool_result_matches(history, i, [&](kimix::string_view later) {
                return later.find("Tool output is empty") != kimix::string_view::npos ||
                       later.size() < text.size() / 2;
            })) {
            tier_b_candidate c;
            c.index = i;
            c.savings = savings;
            c.kind = "superseded_read";
            candidates.push_back(std::move(c));
            continue;
        }
        // oversized_output: len//4 >= min_tokens (default 512).
        if (savings >= min_output_tokens) {
            tier_b_candidate c;
            c.index = i;
            c.savings = savings;
            c.kind = "oversized_output";
            candidates.push_back(std::move(c));
            continue;
        }
        // resolved_error: contains "<system>ERROR:" and a later tool result
        // does not.
        if (text.find("<system>ERROR:") != kimix::string_view::npos &&
            later_tool_result_matches(history, i, [](kimix::string_view later) {
                return later.find("<system>ERROR:") == kimix::string_view::npos;
            })) {
            tier_b_candidate c;
            c.index = i;
            c.savings = savings;
            c.kind = "resolved_error";
            candidates.push_back(std::move(c));
        }
    }
    return candidates;
}

// ── Layer-1 metadata coalescing (soul/message.py coalesce_tool_metadata) ─────

// Merge adjacent identical leading <system> blocks across consecutive tool
// messages: keep the first, annotate it "[×N] inner", drop the redundant
// copies (never leaving a tool message without content). Returns the number of
// blocks removed.
size_t coalesce_tool_metadata(kimix::vector<kimix::llm::Message> &history) {
    size_t removed = 0;
    int64_t i = 0;
    const int64_t n = static_cast<int64_t>(history.size());
    while (i < n - 1) {
        kimix::llm::Message &msg = history[static_cast<size_t>(i)];
        kimix::llm::Message &next = history[static_cast<size_t>(i + 1)];
        if (msg.role != "tool" || next.role != "tool" || msg.content.empty() ||
            next.content.empty()) {
            ++i;
            continue;
        }
        const kimix::optional<kimix::string> sys_text = system_block_inner(msg.content);
        if (!sys_text.has_value()) {
            ++i;
            continue;
        }
        // Count the consecutive run sharing this exact metadata.
        int64_t run = 0;
        int64_t j = i + 1;
        while (j < n && history[static_cast<size_t>(j)].role == "tool" &&
               !history[static_cast<size_t>(j)].content.empty()) {
            const kimix::optional<kimix::string> inner =
                system_block_inner(history[static_cast<size_t>(j)].content);
            if (!inner.has_value() || *inner != *sys_text) {
                break;
            }
            ++run;
            ++j;
        }
        if (run == 0) {
            ++i;
            continue;
        }
        const int64_t total = run + 1;
        // Annotate the first occurrence: "<system>[×N] inner</system>".
        msg.content = "<system>[\xC3\x97";
        msg.content += kstr(total);
        msg.content += "] ";
        msg.content += *sys_text;
        msg.content += "</system>";
        // Drop the block from each subsequent message, keeping any output that
        // follows it (never leave the message empty).
        for (int64_t k = i + 1; k < j; ++k) {
            kimix::llm::Message &m = history[static_cast<size_t>(k)];
            const size_t prefix = leading_system_prefix_len(m.content);
            if (prefix > 0 && prefix < m.content.size()) {
                m.content.erase(0, prefix);
                ++removed;
            }
        }
        i = j;
    }
    return removed;
}

} // namespace

int64_t estimate_history_tokens(const kimix::vector<kimix::llm::Message> &history) {
    return count_message_tokens(history);
}

int64_t estimate_history_tokens(kimix::span<const kimix::llm::Message> history) {
    kimix::vector<kimix::llm::Message> copy(history.begin(), history.end());
    return count_message_tokens(copy);
}

// ---------------------------------------------------------------------------
// Tier A detectors
// ---------------------------------------------------------------------------

bool is_active_task_snapshot_message(const kimix::llm::Message &message) noexcept {
    if (message.role != "user") {
        return false;
    }
    return message.content.find("<active-background-tasks>") != kimix::string::npos ||
           contains_ci(message.content, "active background tasks");
}

bool is_checkpoint_marker_message(const kimix::llm::Message &message) noexcept {
    if (message.role != "user" && message.role != "system") {
        return false;
    }
    if (message.content.find("CHECKPOINT") == kimix::string::npos) {
        return false;
    }
    const kimix::string_view t = strip_ws(message.content);
    return t.substr(0, 18) == "<system>CHECKPOINT" ||
           t.find("<system>CHECKPOINT") != kimix::string_view::npos;
}

bool is_notification_message(const kimix::llm::Message &message) noexcept {
    if (message.role != "user") {
        return false;
    }
    return lstrip_ws(message.content).substr(0, 14) == "<notification ";
}

bool is_ephemeral_message(const kimix::llm::Message &message, bool check_notifications,
                          bool check_task_snapshots, bool check_checkpoints) noexcept {
    if (is_system_reminder_message(message)) {
        return true;
    }
    if (check_notifications && is_notification_message(message)) {
        return true;
    }
    if (check_task_snapshots && is_active_task_snapshot_message(message)) {
        return true;
    }
    if (check_checkpoints && is_checkpoint_marker_message(message)) {
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Protected set
// ---------------------------------------------------------------------------

kimix::set<int64_t>
protect_tool_pair_indices(const kimix::vector<kimix::llm::Message> &history,
                          kimix::set<int64_t> protected_indices) {
    kimix::vector<int64_t> assistant_indices;
    for (const int64_t idx : protected_indices) {
        if (idx < 0 || idx >= static_cast<int64_t>(history.size())) {
            continue;
        }
        const kimix::llm::Message &msg = history[static_cast<size_t>(idx)];
        if (msg.role == "assistant" && !msg.tool_calls.empty()) {
            assistant_indices.push_back(idx);
        }
    }
    for (const int64_t idx : assistant_indices) {
        const kimix::llm::Message &msg = history[static_cast<size_t>(idx)];
        for (int64_t j = 0; j < static_cast<int64_t>(history.size()); ++j) {
            if (protected_indices.count(j) != 0) {
                continue;
            }
            const kimix::llm::Message &candidate = history[static_cast<size_t>(j)];
            if (candidate.role != "tool" || candidate.tool_call_id.empty()) {
                continue;
            }
            for (const kimix::llm::ToolCall &tc : msg.tool_calls) {
                if (tc.id == candidate.tool_call_id) {
                    protected_indices.insert(j);
                    break;
                }
            }
        }
    }
    return protected_indices;
}

kimix::set<int64_t>
compute_protected_indices(const kimix::vector<kimix::llm::Message> &history,
                          int32_t stable_prefix_messages, int32_t recent_messages_protected,
                          kimix::optional<int64_t> current_turn_index,
                          kimix::optional<int32_t> min_cache_prefix_depth) {
    kimix::set<int64_t> protected_indices;
    const int64_t n = static_cast<int64_t>(history.size());
    // Head protection (cache-03): the floor never shrinks below the stable
    // prefix and never exceeds the history length.
    const int64_t floor = std::min<int64_t>(
        n, std::max<int64_t>(stable_prefix_messages,
                             min_cache_prefix_depth.has_value()
                                 ? *min_cache_prefix_depth
                                 : 0));
    for (int64_t i = 0; i < floor; ++i) {
        protected_indices.insert(i);
    }
    // Tail protection: last K user/assistant turns.
    kimix::vector<int64_t> tail_turn_indices;
    for (int64_t i = n - 1; i >= 0; --i) {
        if (static_cast<int64_t>(tail_turn_indices.size()) >= recent_messages_protected) {
            break;
        }
        const kimix::string &role = history[static_cast<size_t>(i)].role;
        if (role == "user" || role == "assistant") {
            tail_turn_indices.push_back(i);
        }
    }
    for (const int64_t idx : tail_turn_indices) {
        protected_indices.insert(idx);
    }
    // Tool-call pair extension.
    protected_indices = protect_tool_pair_indices(history, std::move(protected_indices));
    // Current turn protection.
    if (current_turn_index.has_value()) {
        for (int64_t i = *current_turn_index; i < n; ++i) {
            protected_indices.insert(i);
        }
    }
    return protected_indices;
}

kimix::optional<int64_t>
current_turn_start_index(const kimix::vector<kimix::llm::Message> &history) noexcept {
    for (int64_t i = static_cast<int64_t>(history.size()) - 1; i >= 0; --i) {
        const kimix::llm::Message &msg = history[static_cast<size_t>(i)];
        if (msg.role == "user" && !is_system_reminder_message(msg)) {
            return kimix::optional<int64_t>(i);
        }
    }
    return kimix::optional<int64_t>();
}

// ---------------------------------------------------------------------------
// Tier C kernels
// ---------------------------------------------------------------------------

bool looks_like_readfile_output(kimix::string_view text) noexcept {
    int64_t substantial = 0;
    int64_t numbered = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        const kimix::string_view line =
            text.substr(pos, nl == kimix::string_view::npos ? kimix::string_view::npos
                                                            : nl - pos);
        if (!strip_ws(line).empty()) {
            ++substantial;
            if (line_has_lineno_prefix(line)) {
                ++numbered;
            }
        }
        if (nl == kimix::string_view::npos) {
            break;
        }
        pos = nl + 1;
    }
    return substantial > 0 && numbered == substantial;
}

kimix::string micro_compress_text(kimix::string_view text, kimix::string_view kind,
                                  const micro_compress_config &config) {
    if (text.empty() || !config.enabled) {
        return kimix::string(text);
    }
    using namespace kimix::runtime::tools;
    // Stage 2 - lossless control-noise removal.
    kimix::string out = compress_strip_control_noise(text);
    // Stage 5 - compact line numbers (before collapse so tabs survive).
    out = compress_renumber_lines(out);
    // Stage 3 - whitespace collapse (lossless + annotated common-indent).
    out = compress_collapse_whitespace(out, kind, config.lossless_only,
                                       config.strip_trailing_ws, config.blank_line_collapse,
                                       config.common_indent_factor,
                                       false /* prefix_fold: stage 4 not ported */);
    // Stage 4 (prefix fold), 6 (banner drop), 8 (near-dup), 9 (code elision)
    // have no native kernel - intentionally skipped (see context_pruning.h).
    // Stage 7 - intra-line repetition dedup (annotated).
    if (config.intra_line_dedup && !config.lossless_only && kind != "code") {
        out = compress_intra_line_dedup(out, config.intra_line_dedup_len, 64);
    }
    return out;
}

bool has_annotated_marker(kimix::string_view text) noexcept {
    static constexpr kimix::string_view k_markers[] = {
        "[common-indent:",       "[prefix:",           "[ts-prefix folded",
        "banner lines dropped",  "near-dup",          "chars elided]",
        "license lines elided",  "comment lines elided",
        "lines of generated content",
    };
    for (const kimix::string_view marker : k_markers) {
        if (text.find(marker) != kimix::string_view::npos) {
            return true;
        }
    }
    return false;
}

bool is_pruned_stub(const kimix::llm::Message &message) noexcept {
    return message.content.find("[context-elided:") != kimix::string::npos;
}

// ---------------------------------------------------------------------------
// ContextPruner
// ---------------------------------------------------------------------------

pruning_options pruning_options_from_loop_control(const LoopControl &lc) noexcept {
    pruning_options o;
    o.enabled = lc.context_pruning_enabled;
    o.trigger_ratio = lc.prune_trigger_ratio;
    o.target_ratio = lc.prune_target_ratio;
    o.stable_prefix_messages = lc.prune_stable_prefix_messages;
    o.recent_messages_protected = lc.prune_recent_messages_protected;
    o.min_free_tokens = lc.prune_min_free_tokens;
    o.cooldown_steps = lc.prune_cooldown_steps;
    o.min_usage_growth = lc.prune_min_usage_growth;
    o.max_fraction_per_pass = lc.prune_max_fraction_per_pass;
    o.ephemeral_enabled = lc.prune_ephemeral_enabled;
    o.ephemeral_notifications = lc.prune_ephemeral_notifications;
    o.ephemeral_task_snapshots = lc.prune_ephemeral_task_snapshots;
    o.ephemeral_checkpoint_markers = lc.prune_ephemeral_checkpoint_markers;
    o.substantive_enabled = lc.prune_substantive_enabled;
    o.tool_output_min_tokens = lc.prune_tool_output_min_tokens;
    o.micro_compress_enabled = lc.prune_micro_compress_enabled;
    o.micro_compress_min_saved_chars = lc.prune_micro_compress_min_saved_chars;
    o.min_cache_prefix_depth = lc.prune_min_cache_prefix_depth;
    o.cache_loss_penalty = lc.prune_cache_loss_penalty;
    return o;
}

ContextPruner::ContextPruner(pruning_options opts) : _opts(opts) {}

kimix::string ContextPruner::next_ref(kimix::function<kimix::string()> alloc_ref) {
    if (alloc_ref) {
        return alloc_ref();
    }
    return kimix::string("prune_") + kstr(static_cast<int64_t>(_ref_counter++));
}

bool ContextPruner::in_cooldown(int64_t current_step, double current_usage) const noexcept {
    if (_last_prune_step < 0) {
        return false;
    }
    if (current_step - _last_prune_step < _opts.cooldown_steps) {
        return true;
    }
    return current_usage - _last_prune_usage < _opts.min_usage_growth;
}

pruning_result ContextPruner::prune(const kimix::vector<kimix::llm::Message> &history,
                                    const prune_call &call) {
    kimix::optional<int32_t> cache_floor = call.min_cache_prefix_depth;
    if (!cache_floor.has_value()) {
        cache_floor = _opts.min_cache_prefix_depth;
    }
    auto no_change = [&history]() {
        pruning_result r;
        r.messages = history;
        return r;
    };
    if (!_opts.enabled) {
        return no_change();
    }
    // Policy #4: cooldown.
    if (in_cooldown(call.current_step, call.context_usage)) {
        return no_change();
    }
    // Trigger check.
    if (call.context_usage < _opts.trigger_ratio) {
        return no_change();
    }
    const int64_t target_tokens =
        static_cast<int64_t>(static_cast<double>(call.max_context_size) * _opts.target_ratio);
    const int64_t current_tokens = count_message_tokens(history);
    if (current_tokens - target_tokens <= 0) {
        return no_change();
    }

    // ── Tier C pre-pass (plan §8.3) ──────────────────────────────────────────
    // Runs on the pre-protected view with Tier-B candidates excluded so their
    // stubs archive the UNCOMPRESSED original text.
    kimix::vector<kimix::llm::Message> work_history = history;
    kimix::vector<elided_record> tier_c_records;
    int64_t tier_c_freed = 0;
    kimix::set<int64_t> tier_c_changes;
    if (_opts.micro_compress_enabled) {
        const kimix::set<int64_t> protected_indices = compute_protected_indices(
            history, _opts.stable_prefix_messages, _opts.recent_messages_protected,
            call.current_turn_index, cache_floor);
        kimix::set<int64_t> excluded = protected_indices;
        if (_opts.substantive_enabled) {
            for (const tier_b_candidate &c :
                 tier_b_candidates(history, protected_indices, _opts.tool_output_min_tokens)) {
                excluded.insert(c.index);
            }
        }
        const kimix::vector<tier_c_candidate> candidates =
            tier_c_candidates(history, excluded, _opts.micro_compress_min_saved_chars);
        if (!candidates.empty()) {
            kimix::unordered_map<int64_t, kimix::string> by_index;
            for (const tier_c_candidate &c : candidates) {
                by_index.emplace(c.index, c.kind);
            }
            kimix::vector<kimix::llm::Message> work;
            work.reserve(history.size());
            for (int64_t i = 0; i < static_cast<int64_t>(history.size()); ++i) {
                const kimix::llm::Message &msg = history[static_cast<size_t>(i)];
                const auto it = by_index.find(i);
                if (it == by_index.end()) {
                    work.push_back(msg);
                    continue;
                }
                const size_t sys_prefix = leading_system_prefix_len(msg.content);
                const kimix::string text =
                    sys_prefix < msg.content.size()
                        ? kimix::string(msg.content.substr(sys_prefix))
                        : kimix::string();
                const kimix::string compressed = micro_compress_text(text, it->second);
                const int64_t saved =
                    static_cast<int64_t>(text.size()) - static_cast<int64_t>(compressed.size());
                if (saved <= 0 || compressed == text) {
                    work.push_back(msg);
                    continue;
                }
                kimix::llm::Message copy = msg;
                copy.content.clear();
                if (sys_prefix > 0) {
                    copy.content.assign(msg.content.data(), sys_prefix);
                }
                if (!compressed.empty()) {
                    if (!copy.content.empty()) {
                        copy.content += '\n';
                    }
                    copy.content += compressed;
                }
                work.push_back(std::move(copy));
                tier_c_freed += std::max<int64_t>(saved / 4, 1);
                tier_c_changes.insert(i);
                if (has_annotated_marker(compressed)) {
                    elided_record rec;
                    rec.index = i;
                    rec.role = msg.role;
                    rec.kind = "micro_compress";
                    rec.summary = "micro-compressed " + kstr(saved) + " chars at index " +
                                  kstr(i);
                    rec.original_text = text;
                    rec.ref = next_ref(call.alloc_ref);
                    tier_c_records.push_back(std::move(rec));
                }
            }
            work_history = std::move(work);
        }
    }

    pruning_result base = python_prune(work_history, call.max_context_size,
                                       call.current_turn_index, cache_floor, call.alloc_ref);
    return finalize_prune_result(history, std::move(base), std::move(tier_c_records),
                                 tier_c_freed, std::move(tier_c_changes), call.current_step,
                                 call.context_usage);
}

pruning_result ContextPruner::python_prune(
    const kimix::vector<kimix::llm::Message> &history, int64_t max_context_size,
    kimix::optional<int64_t> current_turn_index,
    kimix::optional<int32_t> min_cache_prefix_depth,
    const kimix::function<kimix::string()> &alloc_ref) {
    auto no_change = [&history]() {
        pruning_result r;
        r.messages = history;
        return r;
    };
    const int64_t target_tokens =
        static_cast<int64_t>(static_cast<double>(max_context_size) * _opts.target_ratio);
    const int64_t current_tokens = count_message_tokens(history);
    int64_t budget = current_tokens - target_tokens;
    if (budget <= 0) {
        return no_change();
    }
    // Cap the budget by max_fraction_per_pass.
    const int64_t max_prune =
        static_cast<int64_t>(static_cast<double>(current_tokens) * _opts.max_fraction_per_pass);
    budget = std::min(budget, max_prune);

    const kimix::set<int64_t> protected_indices = compute_protected_indices(
        history, _opts.stable_prefix_messages, _opts.recent_messages_protected,
        current_turn_index, min_cache_prefix_depth);

    // (index, savings, tier, kind)
    struct candidate_entry {
        int64_t index = 0;
        int64_t savings = 0;
        char tier = 'A';
        kimix::string kind;
    };
    kimix::vector<candidate_entry> candidates;

    if (_opts.ephemeral_enabled) {
        for (const tier_a_candidate &c :
             tier_a_candidates(history, protected_indices, _opts.ephemeral_notifications,
                               _opts.ephemeral_task_snapshots,
                               _opts.ephemeral_checkpoint_markers)) {
            candidate_entry e;
            e.index = c.index;
            e.savings = c.savings;
            e.tier = 'A';
            e.kind = "ephemeral";
            candidates.push_back(std::move(e));
        }
    }
    // Tier B only when Tier A alone is insufficient.
    int64_t tier_a_savings = 0;
    for (const candidate_entry &c : candidates) {
        if (c.tier == 'A') {
            tier_a_savings += c.savings;
        }
    }
    if (budget - tier_a_savings > 0 && _opts.substantive_enabled) {
        for (const tier_b_candidate &c :
             tier_b_candidates(history, protected_indices, _opts.tool_output_min_tokens)) {
            bool duplicate = false;
            for (const candidate_entry &e : candidates) {
                if (e.index == c.index) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            candidate_entry e;
            e.index = c.index;
            e.savings = c.savings;
            e.tier = 'B';
            e.kind = c.kind;
            candidates.push_back(std::move(e));
        }
    }
    if (candidates.empty()) {
        return no_change();
    }

    // Policy #3/#7/#8 ordering: tail-band first, then index-desc, Tier A
    // before B, then savings-desc.
    const int64_t tail_band =
        std::max<int64_t>(0, static_cast<int64_t>(history.size()) -
                                 _opts.recent_messages_protected - 2);
    std::stable_sort(candidates.begin(), candidates.end(),
                     [&](const candidate_entry &a, const candidate_entry &b) {
                         const int64_t a_band = a.index >= tail_band ? 1 : 0;
                         const int64_t b_band = b.index >= tail_band ? 1 : 0;
                         if (a_band != b_band) {
                             return a_band > b_band;
                         }
                         if (a.index != b.index) {
                             return a.index > b.index;
                         }
                         const int64_t a_tier = a.tier == 'A' ? 0 : 1;
                         const int64_t b_tier = b.tier == 'A' ? 0 : 1;
                         if (a_tier != b_tier) {
                             return a_tier < b_tier;
                         }
                         return a.savings > b.savings;
                     });

    // Greedy fill until the budget is met.
    kimix::set<int64_t> selected_indices;
    int64_t total_freed = 0;
    for (const candidate_entry &c : candidates) {
        if (total_freed >= budget) {
            break;
        }
        if (selected_indices.count(c.index) != 0) {
            continue;
        }
        selected_indices.insert(c.index);
        total_freed += c.savings;
    }

    // Build the result.
    kimix::vector<kimix::llm::Message> result_messages;
    result_messages.reserve(history.size());
    kimix::vector<elided_record> elided_records;
    kimix::set<int64_t> changes;
    for (int64_t i = 0; i < static_cast<int64_t>(history.size()); ++i) {
        const kimix::llm::Message &msg = history[static_cast<size_t>(i)];
        if (selected_indices.count(i) == 0) {
            result_messages.push_back(msg);
            continue;
        }
        if (is_ephemeral_message(msg, _opts.ephemeral_notifications,
                                 _opts.ephemeral_task_snapshots,
                                 _opts.ephemeral_checkpoint_markers)) {
            // Tier A: drop the message entirely.
            changes.insert(i);
            continue;
        }
        if (msg.role != "tool") {
            // Tier B rewrites a message in place; that is only acceptable for
            // role=="tool" (index alignment preserved). Refuse user/assistant
            // (cache-02/03 band policy) - they stay verbatim.
            result_messages.push_back(msg);
            continue;
        }
        changes.insert(i);
        kimix::string kind = "elided";
        int64_t savings = 0;
        for (const candidate_entry &c : candidates) {
            if (c.index == i) {
                kind = c.kind;
                savings = c.savings;
                break;
            }
        }
        const kimix::string ref = next_ref(alloc_ref);
        // Byte-exact stub (em dash U+2014), reference context_pruning.py:998-1002.
        kimix::llm::Message stub;
        stub.role = msg.role;
        stub.tool_call_id = msg.tool_call_id;
        stub.content = "<system>[context-elided: ";
        stub.content += kind;
        stub.content += " \xE2\x80\x94 content elided. ~";
        stub.content += kstr(savings);
        stub.content += " tokens freed. Retrieve full content with retrieve id=";
        stub.content += ref;
        stub.content += "]</system>";
        elided_record rec;
        rec.index = i;
        rec.role = msg.role;
        rec.kind = kind;
        rec.summary = kind + " at index " + kstr(i);
        rec.original_text = msg.content;
        rec.ref = ref;
        elided_records.push_back(std::move(rec));
        result_messages.push_back(std::move(stub));
    }

    pruning_result r;
    r.messages = std::move(result_messages);
    r.elided = std::move(elided_records);
    r.freed_tokens = total_freed;
    if (!changes.empty()) {
        r.earliest_removed_index = *changes.begin();
    }
    return r;
}

pruning_result ContextPruner::finalize_prune_result(
    const kimix::vector<kimix::llm::Message> &original, pruning_result base,
    kimix::vector<elided_record> tier_c_records, int64_t tier_c_freed,
    kimix::set<int64_t> tier_c_changes, int64_t current_step, double context_usage) {
    auto rollback = [&original]() {
        pruning_result r;
        r.messages = original;
        return r;
    };
    kimix::vector<kimix::llm::Message> messages = std::move(base.messages);
    kimix::vector<elided_record> elided = std::move(tier_c_records);
    elided.reserve(elided.size() + base.elided.size());
    for (elided_record &rec : base.elided) {
        elided.push_back(std::move(rec));
    }
    int64_t freed = tier_c_freed + base.freed_tokens;
    kimix::set<int64_t> changes = std::move(tier_c_changes);
    if (base.earliest_removed_index.has_value()) {
        changes.insert(*base.earliest_removed_index);
    }

    if (_opts.micro_compress_enabled) {
        // Layer 1 (plan §8.2) - coalesce adjacent identical <system> metadata.
        kimix::vector<kimix::llm::Message> coalesce_work = messages;
        kimix::vector<int64_t> before;
        before.reserve(coalesce_work.size());
        for (const kimix::llm::Message &m : coalesce_work) {
            before.push_back(static_cast<int64_t>(m.content.size()));
        }
        const size_t removed_parts = coalesce_tool_metadata(coalesce_work);
        int64_t coalesced_chars = 0;
        kimix::optional<int64_t> coalesced_first;
        for (size_t i = 0; i < coalesce_work.size(); ++i) {
            const int64_t after = static_cast<int64_t>(coalesce_work[i].content.size());
            if (before[i] > after) {
                coalesced_chars += before[i] - after;
                if (!coalesced_first.has_value()) {
                    coalesced_first = static_cast<int64_t>(i);
                }
            }
        }
        if (removed_parts > 0 && coalesced_chars > 0) {
            messages = std::move(coalesce_work);
            freed += std::max<int64_t>(coalesced_chars / 4, 1);
            if (coalesced_first.has_value()) {
                changes.insert(*coalesced_first);
            }
        }
    }

    // Policy #5: min-payoff gate (whole pass, Tier C included).
    if (changes.empty() || freed < _opts.min_free_tokens) {
        return rollback();
    }
    // Policy #8 (cache-03): cache-invalidation cost gate.
    if (_opts.cache_loss_penalty.has_value()) {
        const int64_t earliest = *changes.begin();
        const kimix::span<const kimix::llm::Message> tail(
            original.data() + earliest, static_cast<int64_t>(original.size()) -
                                            static_cast<size_t>(earliest));
        const int64_t cache_loss = count_message_tokens(tail);
        if (static_cast<double>(freed) * (1.0 + *_opts.cache_loss_penalty) <
            static_cast<double>(cache_loss)) {
            return rollback();
        }
    }

    // Hysteresis update.
    _last_prune_step = current_step;
    _last_prune_usage = context_usage;

    pruning_result r;
    r.messages = std::move(messages);
    r.elided = std::move(elided);
    r.freed_tokens = freed;
    r.earliest_removed_index = *changes.begin();
    return r;
}

int64_t ContextPruner::estimate_after_prune(
    const kimix::vector<kimix::llm::Message> &history, const prune_call &call) {
    // Pure dry run: snapshot and restore the hysteresis + ref state so a real
    // prune() at the same step still applies afterwards.
    const int64_t saved_step = _last_prune_step;
    const double saved_usage = _last_prune_usage;
    const uint32_t saved_ref = _ref_counter;
    pruning_result result;
    result = prune(history, call);
    _last_prune_step = saved_step;
    _last_prune_usage = saved_usage;
    _ref_counter = saved_ref;
    if (!result.earliest_removed_index.has_value()) {
        return count_message_tokens(history);
    }
    return count_message_tokens(result.messages);
}

pruning_result
ContextPruner::prune_with_policy(const kimix::vector<kimix::llm::Message> &history,
                                 const prune_policy_call &call) {
    pruning_options opts = _opts;
    if (call.target_token_count.has_value()) {
        double ratio = static_cast<double>(*call.target_token_count) /
                       static_cast<double>(call.max_context_size);
        ratio = std::max(0.0, std::min(1.0, ratio));
        opts.target_ratio = ratio;
    } else {
        opts.target_ratio = _opts.target_ratio > 0.0 ? _opts.target_ratio : 0.5;
    }
    opts.enabled = true;
    opts.trigger_ratio = 0.0;   // manual invocation always runs
    opts.min_free_tokens = 1;   // bypasses the min-payoff gate
    opts.cooldown_steps = 0;    // bypasses the cooldown
    opts.min_usage_growth = 0.0;
    opts.recent_messages_protected = call.keep_recent_turns;
    opts.substantive_enabled = _opts.substantive_enabled && call.remove_tool_results;

    ContextPruner fresh(std::move(opts));
    fresh._ref_counter = _ref_counter;
    prune_call pc;
    pc.current_step = call.current_step;
    pc.context_usage = fresh._opts.target_ratio > 0.0 ? 1.0 : 0.0;
    pc.max_context_size = call.max_context_size;
    pc.current_turn_index = call.current_turn_index;
    pc.min_cache_prefix_depth = call.min_cache_prefix_depth;
    pc.alloc_ref = call.alloc_ref;
    pruning_result result = fresh.prune(history, pc);
    _ref_counter = fresh._ref_counter;

    // Guard: even though the Tier-B detector only elides tool messages, when
    // remove_reasoning is false restore any assistant message that might have
    // been elided (e.g. future detectors).
    if (!call.remove_reasoning && !result.elided.empty()) {
        kimix::set<int64_t> elided_assistant;
        for (const elided_record &rec : result.elided) {
            if (rec.role == "assistant") {
                elided_assistant.insert(rec.index);
            }
        }
        if (!elided_assistant.empty()) {
            kimix::unordered_map<int64_t, const elided_record *> by_index;
            for (const elided_record &rec : result.elided) {
                by_index.emplace(rec.index, &rec);
            }
            kimix::vector<kimix::llm::Message> restored;
            restored.reserve(history.size());
            for (int64_t i = 0; i < static_cast<int64_t>(history.size()); ++i) {
                const auto it = by_index.find(i);
                if (it != by_index.end() && it->second->role == "assistant") {
                    kimix::llm::Message m;
                    m.role = it->second->role;
                    m.content = it->second->original_text;
                    m.tool_call_id = history[static_cast<size_t>(i)].tool_call_id;
                    restored.push_back(std::move(m));
                } else {
                    restored.push_back(history[static_cast<size_t>(i)]);
                }
            }
            kimix::vector<elided_record> kept;
            int64_t freed = 0;
            kimix::optional<int64_t> earliest;
            for (const elided_record &rec : result.elided) {
                if (rec.role == "assistant") {
                    continue;
                }
                freed += flat_token_savings(rec.original_text.size());
                if (!earliest.has_value() || rec.index < *earliest) {
                    earliest = rec.index;
                }
                kept.push_back(rec);
            }
            result.messages = std::move(restored);
            result.elided = std::move(kept);
            result.freed_tokens = freed;
            result.earliest_removed_index = earliest;
        }
    }
    return result;
}

void ContextPruner::reset_cooldown() noexcept {
    _last_prune_step = -1;
    _last_prune_usage = 0.0;
}

} // namespace kimix::agent
