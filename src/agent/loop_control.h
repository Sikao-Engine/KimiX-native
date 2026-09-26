// agent/loop_control.h - Agent loop control configuration.
//
// Port of kimi_cli/config.py's LoopControl model (config.py:250-570): every
// knob with the reference's snake_case name, default value and inclusive
// range bounds, plus the validate_prune_ratios cross-check
// (prune_target_ratio <= prune_trigger_ratio < compaction_trigger_ratio).
// This header is pure data - the CLI parser fills it (src/cli/cli_config.cpp)
// and the agent options carry it; the consumers (retry loop, loop detectors,
// verification gate, dynamic injections, pruner) read it in later phases.
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

namespace kimix::agent {

// Agent loop control configuration (kimi_cli.config.LoopControl).
struct LoopControl {
    // ── Step / retry / session bounds ──────────────────────────────────────
    // Maximum number of steps in one turn (alias in the reference parser:
    // max_steps_per_run). Default 15000.
    int32_t max_steps_per_turn = 15000;
    // Maximum number of retries in one step. Default 5.
    int32_t max_retries_per_step = 5;
    // Maximum automatic session restarts when step retries are exhausted;
    // 0 disables auto-restart entirely. Default 3 (0..10).
    int32_t max_session_restarts = 3;

    // ── Compaction trigger ─────────────────────────────────────────────────
    // Reserved token count for the compaction trigger / input floor
    // (>= 1000). Default 75000.
    int64_t reserved_context_size = 75000;
    // Context usage ratio threshold for auto-compaction (0.5..0.99).
    // Default 0.8.
    double compaction_trigger_ratio = 0.8;

    // ── Context-overflow recovery (DSH port) ───────────────────────────────
    // Max force-compact-and-retry cycles after a provider-confirmed
    // context-window-exceeded error, per step; 0 disables recovery. Default 1.
    int32_t context_overflow_retries = 1;
    // Preserve depth for the forced overflow compaction (0..4). Default 1.
    int32_t context_overflow_preserve_depth = 1;
    // When true, overflow compaction bypasses should_auto_compact entirely.
    // Default true.
    bool context_overflow_force_threshold = true;

    // ── Durable compaction transaction ─────────────────────────────────────
    // Persist compaction transactions to the session ledger file. Default true.
    bool compaction_ledger_enabled = true;

    // ── System prompt / compaction preservation ────────────────────────────
    // Maximum token count for the system prompt (>= 1000). Default 4000.
    int32_t max_system_prompt_tokens = 4000;
    // Maximum recent user/assistant pairs preserved verbatim by compaction
    // (1..10). Default 2.
    int32_t max_preserved_messages = 2;
    // Minimum recent user/assistant pairs preserved verbatim by compaction
    // (1..10). Default 1.
    int32_t min_preserved_messages = 1;
    // Dynamically adjust preserve depth from session signals. Default true.
    bool adaptive_preserve_enabled = true;

    // ── Compact reminder injection ─────────────────────────────────────────
    // Inject a system-reminder suggesting compaction above the threshold.
    // Default false.
    bool compact_reminder_enabled = false;
    // Context usage ratio at which the compact reminder fires (0.5..0.95);
    // should stay below compaction_trigger_ratio. Default 0.70.
    double compact_reminder_threshold = 0.70;

    // ── Todo reminder / durability ─────────────────────────────────────────
    // Re-inject unfinished todo_write items at the recency edge. Default false.
    bool todo_reminder_enabled = false;
    // Minimum steps between repeated todo reminders (>= 1). Default 20.
    int32_t todo_reminder_interval_steps = 20;
    // Append the active todo plan to the compaction output. Default true.
    bool todo_compact_injection_enabled = true;
    // Maximum unfinished items re-injected into the compaction output
    // (1..100). Default 20.
    int32_t todo_compact_injection_max_items = 20;
    // Maximum todo_write tree/stack depth (1..8). Default 4.
    int32_t todo_max_layers = 4;

    // ── Target-churn reminder ──────────────────────────────────────────────
    // Remind when the agent repeatedly edits the same file or repeats the
    // same normalized error. Default false.
    bool target_churn_enabled = false;
    // Edits to one file target that trigger a churn reminder (>= 2). Default 8.
    int32_t target_churn_file_warn = 8;
    // Edits to one file target that trigger a strong stop-patching reminder
    // (>= 3). Default 15.
    int32_t target_churn_file_strong = 15;
    // Consecutive identical normalized tool errors that trigger a reminder
    // (>= 2). Default 5.
    int32_t target_churn_error_warn = 5;
    // Minimum steps of silence after any target-churn injection (>= 0).
    // Default 10.
    int32_t target_churn_cooldown_steps = 10;

    // ── Verification gate ──────────────────────────────────────────────────
    // Nudge a turn that ends with unfinished todos / unverified edits.
    // Default true.
    bool verification_gate_enabled = true;
    // Maximum verification-gate nudges per turn (0..10). Default 2.
    int32_t verification_gate_max_nudges = 2;
    // CLI-layer closing reminder rounds after a successful prompt (0..5);
    // 0 disables the CLI fallback. Default 1.
    int32_t cli_closing_reminder_rounds = 1;

    // ── Budget reminder ────────────────────────────────────────────────────
    // Inject budget-awareness reminders as the per-turn budget is crossed.
    // Default false.
    bool budget_reminder_enabled = false;
    // Ascending usage ratios of the per-turn budget, each triggering one
    // reminder per turn; the final ratio is the urgent wrap-up reminder.
    // Default (0.7, 0.9).
    kimix::vector<double> budget_warn_ratios{0.7, 0.9};
    // Optional per-turn wall-clock budget in seconds; 0 disables the
    // wall-clock dimension. Default 0.
    int32_t budget_wall_clock_seconds = 0;

    // ── Compaction decision sections ───────────────────────────────────────
    // Compaction summaries must include "## Decisions & Conclusions" and
    // "## Verification Status" sections. Default true.
    bool compaction_decision_section_enabled = true;

    // ── Best-of-N sampling ─────────────────────────────────────────────────
    // Best-of-N sampling (N isolated samples, then select + apply).
    // Default false.
    bool best_of_n_enabled = false;
    // Default number of parallel samples (1..16). Default 4.
    int32_t best_of_n = 4;
    // Candidate selection strategy: "self_eval" or "majority". Default
    // "self_eval".
    kimix::string best_of_n_selector = "self_eval";

    // ── Context-meter reminder ─────────────────────────────────────────────
    // Remind the agent to recall past history with the Retrieve tool when
    // usage materially changes. Default false.
    bool context_meter_enabled = false;
    // Minimum usage-ratio change since the last injection (0.0..0.5).
    // Default 0.15.
    double context_meter_min_delta = 0.15;
    // Minimum steps between context-meter injections (>= 0). Default 30.
    int32_t context_meter_cooldown_steps = 30;

    // ── Auto-retrieve ──────────────────────────────────────────────────────
    // Search archived conversation history before each turn. Default true.
    bool auto_retrieve_history = true;
    // Minimum BM25 score for an archived-turn injection (>= 0.0). Default 5.0.
    double auto_retrieve_history_threshold = 5.0;
    // Search the current conversation for buried relevant turns. Default true.
    bool auto_retrieve_working_memory = true;
    // Minimum BM25 score for a working-memory injection (>= 0.0). Default 5.0.
    double auto_retrieve_working_memory_threshold = 5.0;
    // Boost recent turns with a time-decay factor and inject the best match.
    // Default true.
    bool auto_retrieve_recency_memory = true;
    // Minimum boosted score for a recency-memory injection (>= 0.0).
    // Default 4.0.
    double auto_retrieve_recency_memory_threshold = 4.0;
    // Weight applied to the recency boost multiplier (>= 0.0). Default 1.0.
    double auto_retrieve_recency_weight = 1.0;
    // Maximum auto-retrieved injections per turn (1..5). Default 3.
    int32_t auto_retrieve_max_injections_per_turn = 3;
    // Total token budget for auto-retrieved injections in one turn
    // (500..100000). Default 20000.
    int32_t auto_retrieve_max_tokens_per_turn = 20000;

    // ── Context pruning ────────────────────────────────────────────────────
    // Enable the context pruner (dynamic context reclamation). Default true.
    bool context_pruning_enabled = true;
    // Context usage ratio that triggers a prune pass (0.0..0.95); 0.0 prunes
    // eagerly from the first step. Default 0.0.
    double prune_trigger_ratio = 0.0;
    // Target context usage ratio after a prune pass (0.0..0.9). Default 0.0.
    double prune_target_ratio = 0.0;
    // Initial messages always kept as a stable cached prefix (>= 1).
    // Default 4.
    int32_t prune_stable_prefix_messages = 4;
    // Cache-depth floor for the pruner's protected head. Unset (the default)
    // derives a dynamic floor per step; 0 disables the floor.
    kimix::optional<int32_t> prune_min_cache_prefix_depth;
    // Cache-invalidation cost gate: when set, a prune pass applies only if
    // freed_tokens * (1 + penalty) > cache_loss. Unset keeps the legacy
    // no-gate behavior.
    kimix::optional<double> prune_cache_loss_penalty;
    // Recent user/assistant turns (plus their tool messages) protected from
    // pruning (>= 1). Default 6.
    int32_t prune_recent_messages_protected = 6;
    // Minimum token savings required to justify a prune pass (>= 0).
    // Default 2000.
    int32_t prune_min_free_tokens = 2000;
    // Minimum steps between consecutive prune passes (>= 1). Default 4.
    int32_t prune_cooldown_steps = 4;
    // Minimum usage growth since the last prune that allows re-pruning.
    // Default 0.05.
    double prune_min_usage_growth = 0.05;
    // Maximum fraction of effective tokens pruned in one pass (0.1..0.9).
    // Default 0.5.
    double prune_max_fraction_per_pass = 0.5;

    // Tier A - ephemeral injected messages (primary, safest).
    bool prune_ephemeral_enabled = true;          // Tier A removal. Default true.
    bool prune_ephemeral_notifications = true;    // Drop consumed notifications. Default true.
    bool prune_ephemeral_task_snapshots = true;   // Keep only the newest task snapshot. Default true.
    bool prune_ephemeral_dmail_notices = true;    // Drop spent D-Mail notices. Default true.
    bool prune_ephemeral_checkpoint_markers = false; // Drop CHECKPOINT markers. Default false.

    // Tier B - substantive content elision (escalation only).
    bool prune_substantive_enabled = true; // Stale/oversized elision. Default true.
    // Minimum tokens for a tool output to be elidable (>= 64). Default 512.
    int32_t prune_tool_output_min_tokens = 512;

    // Tier C - micro-compress in place (opt-in: invalidates the KV cache).
    bool prune_micro_compress_enabled = false; // Default false.
    // Minimum chars a message must save for Tier C to rewrite it (>= 1).
    // Default 64.
    int32_t prune_micro_compress_min_saved_chars = 64;
    bool prune_elide_thinking = true;           // Elide old ThinkPart content. Default true.
    bool prune_dedupe_near_duplicates = true;   // Elide near-duplicate blobs. Default true.

    bool prune_persist = false;  // Persist prune operations to storage. Default false.
    bool prune_subagents = true; // Prune subagent sessions too. Default true.
};

} // namespace kimix::agent
