// agent/dynamic_injection.h - Dynamic-injection framework (G9/G06).
//
// Port of kimi_cli/soul/dynamic_injection.py + the apply/strip points in
// kimi_cli/soul/kimisoul.py (_collect_injections, add_injection_provider,
// _notify_injection_providers_compacted, notify_afk_changed, and the
// 2e.2 DYNAMIC INJECTION step block) + soul/message.py's
// system_reminder / is_system_reminder_message / strip_system_reminders.
//
// Per-step flow (mirrors kimisoul.py:1630-1650):
//   1. strip stale <system-reminder> messages from live history (providers
//      re-inject fresh copies; stripping must NOT notify providers - that
//      would reset throttling every step);
//   2. collect injections from every registered provider, each in error
//      isolation (a failing provider is logged and skipped, never aborts
//      the turn);
//   3. wrap each content in system_reminder() and append ONE combined user
//      message carrying "\n"-joined wrapped reminders.
//
// Exception-free note: the reference isolates providers with try/except;
// kimix-llm builds with exceptions disabled, so the provider API reports
// failure through a bool + error out-parameter instead, and the registry's
// collect/notify loops skip-and-log any provider that reports failure.
//
// normalize_history ports dynamic_injection.py:59-93: adjacent user
// messages are merged for the API request except ephemeral
// <system-reminder> messages (never merged, never stripped-from-merge).
// The reference's notification exemption is vacuous natively (notifications
// were never ported), so only the reminder predicate gates the merge.
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "llm/llm.h"

namespace kimix::agent {

// ---------------------------------------------------------------------------
// DynamicInjection
// ---------------------------------------------------------------------------

// One dynamic prompt content to inject before an LLM step
// (dynamic_injection.py:17-21). `type` is the provider identifier, e.g.
// "plan_mode"; `content` is the raw text (wrapped in <system-reminder> tags
// at apply time).
struct DynamicInjection {
    kimix::string type;    // identifier, e.g. "compact_reminder"
    kimix::string content; // text content (wrapped at apply time)
};

// ---------------------------------------------------------------------------
// Per-step provider input
// ---------------------------------------------------------------------------

// The per-step view of the soul handed to every provider. Ports the pieces
// of `KimiSoul` the reference providers read (soul.is_subagent, soul.status,
// soul.context.token_count_with_pending, soul._current_step_no,
// soul._loop_control.max_steps_per_turn, soul._current_turn_id). The C++
// soul has no string turn ids: `turn_seq` is a per-session counter bumped at
// every turn start and plays the turn-id role for per-turn provider state.
struct InjectionStepContext {
    // Live history (read-only scan input for providers like target_churn).
    const kimix::vector<kimix::llm::Message> *history = nullptr;
    bool is_subagent = false; // runtime.role == "subagent" (native soul: always root)
    // StatusSnapshot analogues (kimisoul.py:922-931).
    int64_t context_tokens = 0;             // status.context_tokens (recorded count)
    int64_t token_count_with_pending = 0;   // context.token_count_with_pending
    int64_t max_context_tokens = 0;         // status.max_context_tokens (0 == unknown)
    double context_usage = 0.0;             // status.context_usage (fallback basis)
    int32_t step_no = 0;                    // soul._current_step_no (1-based)
    int32_t max_steps_per_turn = 15000;     // loop_control.max_steps_per_turn
    uint64_t turn_seq = 0;                  // per-turn identity (turn-id analogue)
};

// ---------------------------------------------------------------------------
// Provider interface
// ---------------------------------------------------------------------------

// Base class for dynamic injection providers (dynamic_injection.py:25-56).
// Called before each LLM step; implementations handle their own throttling.
// get_injections returns false + `error` to signal failure (the exception-free
// analogue of the reference's raising provider); the registry logs and skips.
class DynamicInjectionProvider {
public:
    virtual ~DynamicInjectionProvider() = default;

    virtual bool get_injections(const InjectionStepContext &ctx,
                                kimix::vector<DynamicInjection> &out,
                                kimix::string &error) = 0;

    // Called after the context is compacted (history rebuilt). Override to
    // reset internal throttling state; default no-op
    // (dynamic_injection.py:40-47).
    virtual void on_context_compacted() {}

    // Called when afk mode is toggled at runtime. Default no-op
    // (dynamic_injection.py:49-56). The C++ CLI does not implement /afk yet
    // (Phase 3): the hook is exposed so the call site can land later.
    virtual void on_afk_changed(bool enabled) { (void)enabled; }
};

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

// The provider list (kimisoul.py self._injection_providers) plus the
// isolated collect/notify loops (_collect_injections,
// _notify_injection_providers_compacted, notify_afk_changed,
// kimisoul.py:688-713, 883-910). A provider that fails is logged to stderr
// and skipped - never propagated - so a buggy provider cannot break a turn
// or abort compaction.
class InjectionRegistry {
public:
    // Register an additional provider (kimisoul.add_injection_provider).
    void add_provider(kimix::unique_ptr<DynamicInjectionProvider> provider);

    // Collect from all registered providers with per-provider error
    // isolation. Order is registration order, matching the reference's
    // list iteration.
    kimix::vector<DynamicInjection> collect(const InjectionStepContext &ctx) const;

    // Notify hooks; failures are isolated per provider exactly like the
    // reference's try/except loops.
    void notify_context_compacted() const;
    void notify_afk_changed(bool enabled) const;

    size_t size() const noexcept { return _providers.size(); }

private:
    kimix::vector<kimix::unique_ptr<DynamicInjectionProvider>> _providers;
};

// ---------------------------------------------------------------------------
// Model-visible wrapper helpers (soul/message.py:20-33)
// ---------------------------------------------------------------------------

// system(): the exact wrap format "<system>{message}</system>". The
// model-visible envelope of tool metadata / checkpoint markers; text wrapped
// here is what the E10 coalesce pass recognizes and merges.
kimix::string system_block_text(kimix::string_view content);

// system_reminder(): the exact wrap format
// "<system-reminder>\n{message}\n</system-reminder>".
kimix::string system_reminder_text(kimix::string_view content);

// is_system_reminder_message(): a user message whose (Python-lstripped)
// content starts with "<system-reminder>". The native Message carries one
// content string, which is the reference's single-TextPart case.
bool is_system_reminder_message(const kimix::llm::Message &message) noexcept;

// strip_system_reminders(): remove all standalone system-reminder user
// messages from `history` in place; returns the count removed
// (message.py:36-44).
size_t strip_system_reminders(kimix::vector<kimix::llm::Message> &history) noexcept;

// normalize_history(): merge adjacent user messages into one, EXCEPT
// <system-reminder> messages (never merged; they are re-injected fresh every
// step and merging them would churn the provider prefix cache). Assistant
// and tool messages are never merged (their tool_calls / tool_call_id form
// linked pairs). Ports dynamic_injection.py:59-93.
//
// E10: after the merge the result runs the reference's two coalesce passes
// (soul/message.py coalesce_tool_metadata / coalesce_content_parts, adapted
// to the flat-string message model), so duplicate adjacent <system> metadata
// costs tokens once per request:
//   * identical <system> blocks that start runs of consecutive tool messages
//     are kept only on the first message (annotated "[×N] "), the redundant
//     copies are stripped from the followers;
//   * <system> blocks that are adjacent INSIDE one message are merged into
//     one block (". "-joined inner text).
// A history without adjacent <system> blocks comes out unchanged, so the
// pass is invisible to every plain-text flow.
kimix::vector<kimix::llm::Message>
normalize_history(const kimix::vector<kimix::llm::Message> &history);

// ── E10 - Layer 1 coalescing (soul/message.py:82-191) ──────────────────────

// True when `content` (stripped) is exactly one "<system>…</system>" block;
// the inner text is returned through `inner` in that case
// (_extract_system_text).
bool extract_system_block(kimix::string_view content,
                          kimix::string &inner) noexcept;

// coalesce_tool_metadata (message.py:100-161), flat-string adaptation:
// merge identical "<system>…</system>" metadata across runs of consecutive
// tool messages (modified in place; returns the number of blocks removed).
// The first message of a run keeps its block, annotated "[×N] " once N>1
// messages shared it, and every follower loses its copy - unless the copy is
// the follower's whole content (a provider invariant requires non-empty tool
// results), which is kept.
//
// NOTE: the context pruner carries its own Layer-1 twin
// (context_pruning.cpp, anonymous namespace, behind
// loop_control.prune_micro_compress_enabled). This is the request-level
// twin: it runs on EVERY normalized request, so the metadata costs tokens
// once per step regardless of the pruning configuration.
size_t coalesce_adjacent_tool_metadata(
    kimix::vector<kimix::llm::Message> &history);

// coalesce_content_parts (flat-string adaptation): merge "<system>…</system>"
// blocks that are ADJACENT inside one content string (separated only by
// whitespace) into a single block whose inner text is the ". "-joined
// sequence of the merged inner texts. Returns the coalesced content; the
// input is unchanged.
kimix::string coalesce_adjacent_system_blocks(kimix::string_view content);

// The 2e.2 apply step: "\n"-join the system_reminder() wrap of every
// injection into the single combined reminder text
// (kimisoul.py:1642-1648). Returns "" when `injections` is empty.
kimix::string
build_combined_reminder(kimix::span<const DynamicInjection> injections);

// Python f-string "{x:.0%}" (percent, 0 decimals, round-half-to-even),
// used by the compact and budget reminder wordings.
kimix::string py_percent0(double ratio);

} // namespace kimix::agent
