// agent/context_pruning.h - ContextPruner: smart context history removal
// (report.md row D1, gap-check .kimix_cache/gapcheck/05-pruning-index.md G01).
//
// Native port of kimi-cli/src/kimi_cli/soul/context_pruning.py (read in full):
//   * Tier A - drop consumed ephemeral injected messages (system-reminders,
//     notifications, task snapshots, CHECKPOINT markers; D-Mail notices are
//     excluded project-wide), keep-newest semantics for task snapshots
//   * Tier B - superseded_read / oversized_output (>=512 tok) / resolved_error
//     tool results replaced IN PLACE with a stub
//     "<system>[context-elided: {kind} — content elided. ~{n} tokens freed.
//     Retrieve full content with retrieve id=prune_{N}]</system>" (em dash,
//     byte-exact); the archived original is the caller's to index (D5)
//   * Tier C - micro-compress stale tool output in place behind the
//     annotated-marker gate (ElidedRecord kind="micro_compress")
//   * Protected set: head stable_prefix_messages (4), tail
//     recent_messages_protected user/assistant turns (6), the current turn
//     [idx, n), the cache-depth floor, and tool-call-pair extension by
//     tool_call_id with a full-history scan
//   * Gates ported exactly: target/budget sizing, max_fraction_per_pass 0.5,
//     tail-inward ordering, min_free_tokens 2000 whole-pass rollback, the
//     cache-loss payoff gate, cooldown_steps 4 + min_usage_growth 0.05
//     hysteresis, and the idempotent dry-run estimate_after_prune
//     (save/restore of hysteresis + ref state)
//
// Tier order and the 8 cache-conservative policy rules are the class contract
// documented in the reference (context_pruning.py:619-657): Tier C pre-pass on
// the pre-protected view with Tier-B candidates excluded (so stubs archive the
// UNCOMPRESSED original), then Tier A/B selection, then merge + gates.
//
// Token model: savings are always max(len(text)//4, 1) exactly like the
// reference detectors; the budget/cache-loss basis is the language-aware
// estimator the soul already uses (builtin_tools::compact::estimate_*), the
// native analogue of count_message_tokens over TextParts.
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers,
// no RTTI, exception-free.
//
// Known deviations from the reference (documented):
//   * D-Mail ephemeral detection is intentionally absent (excluded
//     project-wide per track instructions).
//   * Tier C runs only the stages ported in src/runtime/tools/compress.h
//     (reference stages 2/5/3/7: strip_control_noise, renumber_lines,
//     collapse_whitespace, intra_line_dedup); the reference stages 4/6/8/9
//     (prefix fold, banner drop, near-dup collapse, code elision) have no
//     native kernel and are skipped, so only the "[common-indent:" and
//     "chars elided]" annotated markers can ever fire the record gate.
//   * The prune_N ref id is produced by a caller-supplied allocator when the
//     soul runs the pass (turn-id authority via the history index, D5); the
//     internal counter (reference behavior) is the fallback.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "agent/loop_control.h"
#include "llm/llm.h"

namespace kimix::agent {

// ---------------------------------------------------------------------------
// Records / results
// ---------------------------------------------------------------------------

// A record of a Tier B (or Tier C annotated) elision for archival/retrieval
// indexing (context_pruning.py:27-45). `index` is the index in the ORIGINAL
// (unpruned) history; `ref` is the stable "prune_N" reference the stub names.
struct elided_record {
    int64_t index = 0;
    kimix::string role;
    kimix::string kind;
    kimix::string summary;
    kimix::string original_text;
    kimix::string ref;
};

// Result of one prune pass (context_pruning.py:48-60). `messages` is the new
// LLM-visible list (Tier A dropped, Tier B stubbed); `elided` carries the
// originals for archiving; `earliest_removed_index` is None (nullopt) when the
// pass made no change.
struct pruning_result {
    kimix::vector<kimix::llm::Message> messages;
    kimix::vector<elided_record> elided;
    int64_t freed_tokens = 0;
    kimix::optional<int64_t> earliest_removed_index;
};

// ---------------------------------------------------------------------------
// Tier A ephemeral detectors (context_pruning.py:68-125)
// ---------------------------------------------------------------------------

// Post-compaction active-task snapshot: role user whose text contains
// "<active-background-tasks>" or "active background tasks" (case-insensitive).
bool is_active_task_snapshot_message(const kimix::llm::Message &message) noexcept;

// CHECKPOINT marker: role user|system whose stripped text contains
// "CHECKPOINT" and (starts with or contains) "<system>CHECKPOINT".
bool is_checkpoint_marker_message(const kimix::llm::Message &message) noexcept;

// Notification: role user, single TextPart whose lstripped text starts with
// "<notification " (notifications/llm.py is_notification_message).
bool is_notification_message(const kimix::llm::Message &message) noexcept;

// The general ephemeral predicate (D-Mail excluded project-wide).
bool is_ephemeral_message(const kimix::llm::Message &message, bool check_notifications,
                          bool check_task_snapshots, bool check_checkpoints) noexcept;

// ---------------------------------------------------------------------------
// Protected set (context_pruning.py:133-216)
// ---------------------------------------------------------------------------

// Extend `protected` so every tool result whose tool_call_id matches a
// protected assistant's tool_calls is kept as a unit (full-history scan, robust
// to reordering/normalization).
kimix::set<int64_t>
protect_tool_pair_indices(const kimix::vector<kimix::llm::Message> &history,
                          kimix::set<int64_t> protected_indices);

// The protected set: head floor (min(n, max(stable_prefix, cache floor))), the
// last `recent_messages_protected` user/assistant turns, the tool-pair
// extension, and [current_turn_index, n). `current_turn_index` and
// `min_cache_prefix_depth` nullopt keep the legacy behavior.
kimix::set<int64_t>
compute_protected_indices(const kimix::vector<kimix::llm::Message> &history,
                          int32_t stable_prefix_messages, int32_t recent_messages_protected,
                          kimix::optional<int64_t> current_turn_index = std::nullopt,
                          kimix::optional<int32_t> min_cache_prefix_depth = std::nullopt);

// Index of the current turn's first real (non-system-reminder) user message,
// scanning backwards; nullopt when there is none (the reference's
// _current_turn_start_index).
kimix::optional<int64_t>
current_turn_start_index(const kimix::vector<kimix::llm::Message> &history) noexcept;

// ---------------------------------------------------------------------------
// Tier C micro-compress kernels (context_pruning.py:422-611)
// ---------------------------------------------------------------------------

// Mirror of MicroCompressConfig (tools/file/micro_compress.py:68-125) with the
// defaults the prune pass uses.
struct micro_compress_config {
    bool enabled = true;
    bool lossless_only = false;
    int32_t blank_line_collapse = 1;
    bool strip_trailing_ws = true;
    bool common_indent_factor = true;
    bool prefix_fold = true;         // stage 4 - NOT ported (no native kernel)
    int32_t prefix_fold_min_chars = 8;
    double prefix_fold_min_ratio = 0.80;
    int32_t prefix_fold_min_lines = 20;
    bool banner_drop = true;         // stage 6 - NOT ported
    int32_t intra_line_dedup_len = 2000;
    bool intra_line_dedup = true;
    bool near_dup_collapse = true;   // stage 8 - NOT ported
    int32_t near_dup_min_run = 4;
    int32_t near_dup_threshold = 90;
    bool read_compact_code = false;  // stage 9 - NOT ported
};

// The deterministic, idempotent micro-compress pipeline over the native
// kernels (reference `compress(text, kind, config)`). Stage order mirrors the
// reference (2 -> 5 -> 3 -> 7 with 4/6/8/9 skipped - see the header note).
kimix::string micro_compress_text(kimix::string_view text, kimix::string_view kind,
                                  const micro_compress_config &config = {});

// True when every substantial line matches ^\s*(\d+)\t (ReadFile-style).
bool looks_like_readfile_output(kimix::string_view text) noexcept;

// The annotated-marker gate (reference _ANNOTATED_MARKERS): true when the
// compressed text carries a marker of a LOSSY stage.
bool has_annotated_marker(kimix::string_view text) noexcept;

// is_pruned_stub: any content carries "[context-elided:".
bool is_pruned_stub(const kimix::llm::Message &message) noexcept;

// count_message_tokens port: the language-aware estimate over the message
// TEXT parts only (thinking excluded, like the reference's TextPart-only
// sum). This is the budget/cache-loss basis the pruner and the soul's
// prune-before-compact overhead math share.
int64_t estimate_history_tokens(const kimix::vector<kimix::llm::Message> &history);
int64_t estimate_history_tokens(kimix::span<const kimix::llm::Message> history);

// ---------------------------------------------------------------------------
// ContextPruner (context_pruning.py:619-1304)
// ---------------------------------------------------------------------------

// 1:1 with the reference constructor knobs; pruning_options_from_loop_control
// maps LoopControl's prune_* fields.
struct pruning_options {
    bool enabled = true;
    double trigger_ratio = 0.0;
    double target_ratio = 0.0;
    int32_t stable_prefix_messages = 4;
    int32_t recent_messages_protected = 6;
    int32_t min_free_tokens = 2000;
    int32_t cooldown_steps = 4;
    double min_usage_growth = 0.05;
    double max_fraction_per_pass = 0.5;
    // Tier A
    bool ephemeral_enabled = true;
    bool ephemeral_notifications = true;
    bool ephemeral_task_snapshots = true;
    bool ephemeral_checkpoint_markers = false;
    // Tier B
    bool substantive_enabled = true;
    int32_t tool_output_min_tokens = 512;
    // Tier C
    bool micro_compress_enabled = false;
    int32_t micro_compress_min_saved_chars = 64;
    // cache-03
    kimix::optional<int32_t> min_cache_prefix_depth;
    kimix::optional<double> cache_loss_penalty;
};

pruning_options pruning_options_from_loop_control(const LoopControl &lc) noexcept;

// One prune() invocation's inputs (the reference's keyword arguments).
struct prune_call {
    int64_t current_step = 0;
    double context_usage = 0.0;
    int64_t max_context_size = 128000;
    kimix::optional<int64_t> current_turn_index;
    kimix::optional<int32_t> min_cache_prefix_depth;
    // The prune_N producer. When set (the soul's pass), each allocated ref is
    // backed by a RESERVED history-index turn id so the archived original
    // resolves through get_by_id end-to-end (D5). Empty -> the pruner's
    // internal counter (reference behavior; ids are not index-authoritative).
    kimix::function<kimix::string()> alloc_ref;
};

// One prune_with_policy() invocation's inputs.
struct prune_policy_call {
    bool remove_reasoning = true;
    bool remove_tool_results = true;
    int32_t keep_recent_turns = 6;
    kimix::optional<int64_t> target_token_count;
    int64_t max_context_size = 128000;
    int64_t current_step = 0;
    kimix::optional<int64_t> current_turn_index;
    kimix::optional<int32_t> min_cache_prefix_depth;
    kimix::function<kimix::string()> alloc_ref;
};

class ContextPruner {
public:
    explicit ContextPruner(pruning_options opts = {});

    const pruning_options &options() const noexcept { return _opts; }

    // One Layer-1 pass over `history` (never mutated; the result carries the
    // new LLM-visible list). Tier C pre-pass -> Tier A/B selection -> merge +
    // min-payoff gate + cache-loss gate + hysteresis update.
    pruning_result prune(const kimix::vector<kimix::llm::Message> &history,
                         const prune_call &call);

    // Pure dry run: the pass runs, but the cooldown step/usage and the ref
    // state are restored afterwards, so a real prune() at the same step still
    // applies (kimisoul.py's prune-before-compact arbitration, C14). The
    // estimated token count of the pruned history is returned.
    int64_t estimate_after_prune(const kimix::vector<kimix::llm::Message> &history,
                                 const prune_call &call);

    // Policy-driven variant for manual invocation (the context_prune tool and
    // /prune-style previews): fresh hysteresis (trigger 0, min_free 1,
    // cooldown 0, growth 0), target_ratio from target_token_count with the
    // reference's 0.5 fallback, keep_recent_turns as the tail window.
    pruning_result prune_with_policy(const kimix::vector<kimix::llm::Message> &history,
                                     const prune_policy_call &call);

    // reset_cooldown (kimisoul.py:2187 - called after a compaction).
    void reset_cooldown() noexcept;

private:
    pruning_result python_prune(const kimix::vector<kimix::llm::Message> &history,
                                int64_t max_context_size,
                                kimix::optional<int64_t> current_turn_index,
                                kimix::optional<int32_t> min_cache_prefix_depth,
                                const kimix::function<kimix::string()> &alloc_ref);
    pruning_result finalize_prune_result(const kimix::vector<kimix::llm::Message> &original,
                                         pruning_result base,
                                         kimix::vector<elided_record> tier_c_records,
                                         int64_t tier_c_freed, kimix::set<int64_t> tier_c_changes,
                                         int64_t current_step, double context_usage);
    bool in_cooldown(int64_t current_step, double current_usage) const noexcept;
    kimix::string next_ref(kimix::function<kimix::string()> alloc_ref);

    pruning_options _opts;
    int64_t _last_prune_step = -1;
    double _last_prune_usage = 0.0;
    uint32_t _ref_counter = 0;
};

} // namespace kimix::agent
