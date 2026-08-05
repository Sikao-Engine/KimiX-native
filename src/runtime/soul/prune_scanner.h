/*
 * prune_scanner.h - native context-pruning candidate scan
 * (kimix::runtime::soul).
 *
 * Plan 015: one linear pass over message views producing the same prune
 * candidate set as kimi_cli/soul/context_pruning.py (verified against
 * source):
 *   - protected set = _compute_protected_indices (stable head + recent tail
 *     user/assistant turns + tool-pair units + current turn) with
 *     _protect_tool_pair_indices via a single hash-map pass (O(n+|pairs|)),
 *   - Tier A ephemeral candidates (system reminders / notifications / task
 *     snapshots / D-Mail notices / checkpoints, keeping only the latest task
 *     snapshot),
 *   - Tier B candidates: _is_superseded_read / _is_oversized_output /
 *     _is_resolved_error -- the O(t^2) pair scans become O(n) suffix
 *     aggregates (later-empty-marker OR / later-min-length / later-success).
 *
 * Emits one prune_action per index, index-ascending, so Python applies the
 * plan deterministically. The pruning POLICY (cooldown, trigger ratio,
 * budget, greedy selection, stub building) stays in Python -- this scanner
 * only classifies.
 */

#pragma once

#include <core/kimix_core.h>
#include <runtime/soul/message_view.h>

namespace kimix {
namespace runtime {
namespace soul {

// Action reasons (mirrors the plan; reason 4 is an extension for the
// oversized-output Tier B elision class):
enum : uint8_t {
    kPruneSupersededRead = 0, // Tier B elide: stale read result
    kPruneResolvedError = 1,  // Tier B elide: error later resolved
    kPruneCompact = 2,        // Tier A drop: accumulated ephemera
    kPruneProtect = 3,        // must never be pruned
    kPruneOversizedOutput = 4 // Tier B elide: oversized tool output
};

struct prune_action {
    uint32_t index;
    uint8_t reason;
};

// Thresholds from context_pruning.py ContextPruner defaults.
struct prune_policy {
    uint32_t stable_prefix_messages = 4;
    uint32_t recent_messages_protected = 6;
    uint32_t tool_output_min_tokens = 512;
    uint32_t current_turn_index = UINT32_MAX; // sentinel = None
    bool drop_notifications = true;
    bool drop_task_snapshots = true;
    bool drop_dmail = true;
    bool drop_checkpoints = false;
    // Plan 017 additions: full prune_history budget and per-class toggles.
    uint32_t max_elision_tokens = 0;
    bool superseded_read_enabled = true;
    bool oversized_output_enabled = true;
    bool stale_tool_result_enabled = true;
};

// Result of the full PruneScanner::prune_history pass (Plan 017).  Actions are
// emitted in index-ascending order; the binding assigns prune_0, prune_1, ...
// refs to Tier B actions in that order.
struct prune_history_action {
    uint32_t index;
    uint8_t reason; // kPruneCompact, kPruneSupersededRead, kPruneOversizedOutput, kPruneResolvedError
    uint32_t savings;
};

struct prune_history_result {
    kimix::vector<prune_history_action> actions; // index-ascending
    uint32_t freed_tokens = 0;
    uint32_t earliest_removed_index = UINT32_MAX; // sentinel = None
};

class KIMIX_RUNTIME_API PruneScanner {
public:
    // One pass; `out` receives one action per index in ascending index
    // order (protected indices are emitted with reason kPruneProtect).
    void scan(kimix::span<const message_view> msgs,
              const prune_policy& policy,
              kimix::vector<prune_action>& out) const noexcept;

    // Full prune_history (Plan 017): Tier A drops + Tier B elision candidates,
    // greedily selected tail-inward within max_elision_tokens.
    void prune_history(kimix::span<const message_view> msgs,
                       const prune_policy& policy,
                       prune_history_result& out) const noexcept;
};

} // namespace soul
} // namespace runtime
} // namespace kimix
