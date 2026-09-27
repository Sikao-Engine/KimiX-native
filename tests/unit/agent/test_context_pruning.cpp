// test_context_pruning.cpp - Unit tests for the ContextPruner engine
// (src/agent/context_pruning.*, report.md row D1 + D5 wiring).
//
// The cases mirror kimi-cli/tests/core/test_context_pruning.py and
// test_context_pruning_tier_c.py: Tier A ephemeral detectors (D-Mail excluded
// project-wide), keep-newest task snapshots, the protected set (head 4 / tail
// 6 / current turn / cache floor / tool-pair extension), the three Tier B
// detectors, the in-place stub (byte-exact wording), the gates (min payoff,
// cache-loss, cooldown + growth, trigger, max_fraction_per_pass), tail-inward
// ordering, the idempotent estimate_after_prune dry run, prune_with_policy,
// Tier C micro-compress behind the annotated-marker gate, and the prune_N
// reserve/archive/get_by_id round trip through AgentSession (D5).
//
// Like the reference detector tests, test_options() disables the head/tail
// protection so small histories are fully prunable; the protected-set tests
// exercise compute_protected_indices directly with the reference defaults.
//
// Framework: Boost.UT (tests/ut/ut.hpp).
#include "ut/ut.hpp"

#include <string>

#include <core/kimix_core.h>

#include <agent/context_pruning.h>
#include <agent/soul.h>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

kimix::llm::Message make_msg(kimix::string_view role, kimix::string_view content) {
    kimix::llm::Message m;
    m.role.assign(role.data(), role.size());
    m.content.assign(content.data(), content.size());
    return m;
}

kimix::llm::Message make_tool(kimix::string_view content, kimix::string_view call_id) {
    kimix::llm::Message m = make_msg("tool", content);
    m.tool_call_id.assign(call_id.data(), call_id.size());
    return m;
}

kimix::string filler(size_t n) { return kimix::string(n, 'x'); }

// Options with the head/tail protection and hysteresis disabled so small
// histories are fully prunable (the reference detector tests do the same).
kimix::agent::pruning_options test_options() {
    kimix::agent::pruning_options o;
    o.stable_prefix_messages = 0;
    o.recent_messages_protected = 0;
    o.min_free_tokens = 1; // bypass the min-payoff gate unless a test wants it
    o.cooldown_steps = 0;
    o.min_usage_growth = 0.0;
    return o;
}

kimix::agent::prune_call test_call() {
    kimix::agent::prune_call c;
    c.current_step = 10;
    c.context_usage = 1.0; // above the default 0.0 trigger
    c.max_context_size = 128000;
    return c;
}

kimix::llm::Message assistant_with_call(kimix::string_view call_id) {
    kimix::llm::Message call = make_msg("assistant", "calling tool");
    call.tool_calls.push_back(kimix::llm::ToolCall{});
    call.tool_calls[0].id.assign(call_id.data(), call_id.size());
    call.tool_calls[0].name = "read";
    call.tool_calls[0].arguments = "{}";
    return call;
}

} // namespace

int main() {
    // ── Tier A ephemeral detectors ───────────────────────────────────────────
    "system_reminder_is_ephemeral"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "real question"));
        h.push_back(make_msg("user", "<system-reminder>\nmeter\n</system-reminder>"));
        expect(kimix::agent::is_ephemeral_message(h[1], true, true, true));
        expect(!kimix::agent::is_ephemeral_message(h[0], true, true, true));
    };

    "notification_is_ephemeral"_test = [] {
        kimix::llm::Message m = make_msg("user", "<notification id=1>note</notification>");
        expect(kimix::agent::is_ephemeral_message(m, true, true, true));
        expect(!kimix::agent::is_ephemeral_message(m, false, true, true));
        kimix::llm::Message a = make_msg("assistant", "<notification id=1>");
        expect(!kimix::agent::is_ephemeral_message(a, true, true, true));
    };

    "task_snapshot_detection"_test = [] {
        kimix::llm::Message m =
            make_msg("user", "<active-background-tasks>\n<task/>\n</active-background-tasks>");
        expect(kimix::agent::is_active_task_snapshot_message(m));
        kimix::llm::Message lower = make_msg("user", "Active Background Tasks snapshot");
        expect(kimix::agent::is_active_task_snapshot_message(lower));
        expect(!kimix::agent::is_active_task_snapshot_message(
            make_msg("user", "just a task list")));
        expect(!kimix::agent::is_active_task_snapshot_message(
            make_msg("assistant", "<active-background-tasks>")));
    };

    "checkpoint_marker_detection"_test = [] {
        expect(kimix::agent::is_checkpoint_marker_message(
            make_msg("user", "<system>CHECKPOINT reached</system>")));
        expect(kimix::agent::is_checkpoint_marker_message(
            make_msg("system", "<system>CHECKPOINT</system>")));
        // Default toggle: checkpoints are NOT ephemeral.
        kimix::llm::Message plain = make_msg("user", "<system>CHECKPOINT notes</system>");
        expect(!kimix::agent::is_ephemeral_message(plain, true, true, false));
        expect(kimix::agent::is_ephemeral_message(plain, true, true, true));
    };

    "dmail_is_excluded_project_wide"_test = [] {
        // D-Mail ephemera are skipped per the project-wide exclusion: a D-Mail
        // notice only counts when another predicate matches.
        kimix::llm::Message m = make_msg("user", "D-Mail from your future self: do X");
        expect(!kimix::agent::is_ephemeral_message(m, true, true, true));
    };

    // ── Protected set (reference defaults 4 / 6) ─────────────────────────────
    "protected_head_and_tail"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        for (int i = 0; i < 4; ++i) {
            h.push_back(make_msg("user", filler(64)));
        }
        h.push_back(make_msg("user", "<system-reminder>\nr\n</system-reminder>"));
        for (int i = 0; i < 6; ++i) {
            h.push_back(make_msg(i % 2 ? "assistant" : "user", filler(64)));
        }
        const kimix::set<int64_t> p =
            kimix::agent::compute_protected_indices(h, 4, 6, std::nullopt, std::nullopt);
        expect(p.count(0) == 1u);
        expect(p.count(3) == 1u);
        expect(p.count(4) == 0u); // the reminder is prunable
        expect(p.count(5) == 1u); // last 6 user/assistant turns: 5..10
        expect(p.count(h.size() - 1) == 1u);
    };

    "protected_current_turn"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "old"));
        h.push_back(make_msg("assistant", "old answer"));
        h.push_back(make_msg("user", "<system-reminder>\nr\n</system-reminder>"));
        h.push_back(make_msg("user", "current turn question"));
        h.push_back(make_msg("assistant", "current answer"));
        const kimix::set<int64_t> p = kimix::agent::compute_protected_indices(
            h, 4, 6, kimix::optional<int64_t>(2), std::nullopt);
        expect(p.count(2) == 1u);
        expect(p.count(3) == 1u);
        expect(p.count(4) == 1u);
    };

    "protected_tool_pair_extension"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(assistant_with_call("call_1")); // 0: protected assistant
        h.push_back(make_tool("result", "call_1")); // 1: its tool result
        h.push_back(make_msg("user", "later"));
        kimix::set<int64_t> p;
        p.insert(0);
        p = kimix::agent::protect_tool_pair_indices(h, std::move(p));
        expect(p.count(1) == 1u);
        expect(p.count(2) == 0u);
        // An unmatched tool result is not protected.
        kimix::vector<kimix::llm::Message> h2;
        h2.push_back(assistant_with_call("call_9"));
        h2.push_back(make_tool("orphan", "call_x"));
        kimix::set<int64_t> p2;
        p2.insert(0);
        p2 = kimix::agent::protect_tool_pair_indices(h2, std::move(p2));
        expect(p2.count(1) == 0u);
    };

    "cache_depth_floor_protects_head"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        for (int i = 0; i < 20; ++i) {
            h.push_back(make_msg("user", filler(64)));
        }
        const kimix::set<int64_t> p = kimix::agent::compute_protected_indices(
            h, 4, 6, std::nullopt, kimix::optional<int32_t>(8));
        expect(p.count(7) == 1u);
        expect(p.count(8) == 0u); // outside both the head floor and the tail
    };

    // ── Tier B detectors ─────────────────────────────────────────────────────
    "superseded_read_detected"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "q"));
        h.push_back(make_tool(filler(400), "c1"));
        h.push_back(make_tool("Tool output is empty.", "c2"));
        kimix::agent::ContextPruner pruner(test_options());
        kimix::agent::pruning_result r = pruner.prune(h, test_call());
        expect(r.earliest_removed_index.has_value());
        expect(r.elided.size() == 1u);
        expect(r.elided[0].kind == "superseded_read");
        // In-place stub: same list length, same role + tool_call_id.
        expect(r.messages.size() == h.size());
        expect(r.messages[1].role == "tool");
        expect(r.messages[1].tool_call_id == "c1");
        expect(kimix::agent::is_pruned_stub(r.messages[1]));
        expect(r.messages[1].content.find("[context-elided: superseded_read") !=
               kimix::string::npos);
        expect(r.messages[1].content.find("Retrieve full content with retrieve id=") !=
               kimix::string::npos);
        // The record archives the ORIGINAL text.
        expect(r.elided[0].original_text == h[1].content);
        expect(r.elided[0].ref.find("prune_") == 0u);
    };

    "superseded_read_by_half_length"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "q"));
        h.push_back(make_tool(filler(400), "c1"));
        h.push_back(make_tool(filler(100), "c2"));
        kimix::agent::ContextPruner pruner(test_options());
        kimix::agent::pruning_result r = pruner.prune(h, test_call());
        expect(r.elided.size() == 1u);
        expect(r.elided[0].kind == "superseded_read");
    };

    "oversized_output_detected"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "q"));
        h.push_back(make_tool(filler(2600), "c1"));
        kimix::agent::ContextPruner pruner(test_options());
        kimix::agent::pruning_result r = pruner.prune(h, test_call());
        expect(r.elided.size() == 1u);
        expect(r.elided[0].kind == "oversized_output");
    };

    "oversized_below_threshold_kept"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "q"));
        h.push_back(make_tool(filler(1000), "c1"));
        kimix::agent::ContextPruner pruner(test_options());
        kimix::agent::pruning_result r = pruner.prune(h, test_call());
        expect(!r.earliest_removed_index.has_value());
        expect(r.elided.empty());
    };

    "resolved_error_detected"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "q"));
        h.push_back(make_tool("<system>ERROR: boom</system>", "c1"));
        // The later success must NOT be shorter than half (or the detector
        // classifies the pair as superseded_read first, like the reference).
        h.push_back(make_tool(filler(100), "c2"));
        kimix::agent::ContextPruner pruner(test_options());
        kimix::agent::pruning_result r = pruner.prune(h, test_call());
        expect(r.elided.size() == 1u);
        expect(r.elided[0].kind == "resolved_error");
    };

    "unresolved_error_kept"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "q"));
        h.push_back(make_tool("<system>ERROR: boom</system>", "c1"));
        kimix::agent::ContextPruner pruner(test_options());
        kimix::agent::pruning_result r = pruner.prune(h, test_call());
        expect(r.elided.empty());
    };

    "tier_b_refuses_user_assistant_elision"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "q"));
        h.push_back(assistant_with_call("c1"));
        h.back().content = filler(2600);
        h.push_back(make_tool(filler(2600), "c1"));
        kimix::agent::ContextPruner pruner(test_options());
        kimix::agent::pruning_result r = pruner.prune(h, test_call());
        expect(r.elided.size() == 1u);
        expect(r.elided[0].role == "tool");
    };

    "stub_wording_byte_exact"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "q"));
        h.push_back(make_tool(filler(2048), "c1"));
        kimix::agent::ContextPruner pruner(test_options());
        kimix::agent::prune_call c = test_call();
        int64_t next = 0;
        c.alloc_ref = [&next] {
            return kimix::string("prune_") +
                   std::to_string(static_cast<long long>(next++)).c_str();
        };
        kimix::agent::pruning_result r = pruner.prune(h, c);
        expect(r.messages.size() == 2u);
        // Byte-exact stub with the em dash (U+2014).
        const kimix::string expected =
            "<system>[context-elided: oversized_output \xE2\x80\x94 content elided. "
            "~512 tokens freed. Retrieve full content with retrieve id=prune_0]"
            "</system>";
        expect(r.messages[1].content == expected) << r.messages[1].content;
        expect(kimix::agent::is_pruned_stub(r.messages[1]));
        expect(!kimix::agent::is_pruned_stub(h[1]));
    };

    // ── Tier A pass behaviour ────────────────────────────────────────────────
    "tier_a_drops_ephemera_keep_newest_snapshot"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "q"));
        h.push_back(make_msg("user",
                             "<active-background-tasks>one</active-background-tasks>"));
        h.push_back(make_msg("user", filler(32)));
        h.push_back(make_msg("user",
                             "<active-background-tasks>two</active-background-tasks>"));
        kimix::agent::ContextPruner pruner(test_options());
        kimix::agent::pruning_result r = pruner.prune(h, test_call());
        expect(r.earliest_removed_index.has_value());
        expect(*r.earliest_removed_index == 1);
        expect(r.messages.size() == h.size() - 1);
        // The NEWEST snapshot (old index 3) survives at position 2.
        expect(r.messages[2].content.find("two") != kimix::string::npos);
    };

    "tier_a_respects_protected_set"_test = [] {
        // With the reference defaults the head reminder sits in the stable
        // prefix: nothing may drop.
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "<system-reminder>\nprotected\n</system-reminder>"));
        for (int i = 0; i < 10; ++i) {
            h.push_back(make_msg(i % 2 ? "assistant" : "user", filler(64)));
        }
        kimix::agent::pruning_options o; // defaults: stable 4, recent 6
        o.min_free_tokens = 1;
        o.cooldown_steps = 0;
        o.min_usage_growth = 0.0;
        kimix::agent::ContextPruner pruner(o);
        kimix::agent::pruning_result r = pruner.prune(h, test_call());
        expect(!r.earliest_removed_index.has_value());
    };

    // ── Gates ────────────────────────────────────────────────────────────────
    "min_payoff_gate_rolls_back"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "q"));
        h.push_back(make_tool(filler(2600), "c1"));
        kimix::agent::pruning_options o = test_options();
        o.min_free_tokens = 2000; // the reference default
        kimix::agent::ContextPruner pruner(o);
        kimix::agent::pruning_result r = pruner.prune(h, test_call());
        expect(!r.earliest_removed_index.has_value());
        expect(r.freed_tokens == 0);
        expect(r.messages.size() == h.size());
        expect(r.messages[1].content == h[1].content);
    };

    "cache_loss_payoff_gate_blocks_and_allows"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "q"));
        h.push_back(make_tool(filler(2600), "c1"));
        h.push_back(make_msg("user", filler(20000)));
        {
            kimix::agent::pruning_options o = test_options();
            o.cache_loss_penalty = 0.1; // freed * 1.1 < cache_loss -> blocked
            kimix::agent::ContextPruner pruner(o);
            expect(!pruner.prune(h, test_call()).earliest_removed_index.has_value());
        }
        {
            kimix::agent::pruning_options o = test_options();
            o.cache_loss_penalty = 100.0; // freed * 101 > cache_loss -> applies
            kimix::agent::ContextPruner pruner(o);
            expect(pruner.prune(h, test_call()).earliest_removed_index.has_value());
        }
        // Gate disabled (default): the pass applies regardless.
        {
            kimix::agent::ContextPruner pruner(test_options());
            expect(pruner.prune(h, test_call()).earliest_removed_index.has_value());
        }
    };

    "cooldown_and_growth_hysteresis"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "q"));
        h.push_back(make_tool(filler(2600), "c1"));
        kimix::agent::pruning_options o = test_options();
        o.cooldown_steps = 4;
        o.min_usage_growth = 0.05;
        kimix::agent::ContextPruner pruner(o);
        kimix::agent::prune_call c = test_call();
        expect(pruner.prune(h, c).earliest_removed_index.has_value());
        // Same step: step cooldown.
        expect(!pruner.prune(h, c).earliest_removed_index.has_value());
        // 4 steps later but usage grew < 5%: growth cooldown.
        c.current_step = 14;
        c.context_usage = 1.01;
        expect(!pruner.prune(h, c).earliest_removed_index.has_value());
        // Usage grew enough: prunes again.
        c.context_usage = 1.20;
        expect(pruner.prune(h, c).earliest_removed_index.has_value());
        // reset_cooldown unlocks immediately.
        pruner.reset_cooldown();
        c.current_step = 15;
        c.context_usage = 1.21;
        expect(pruner.prune(h, c).earliest_removed_index.has_value());
    };

    "trigger_ratio_no_op"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "q"));
        h.push_back(make_tool(filler(2600), "c1"));
        kimix::agent::pruning_options o = test_options();
        o.trigger_ratio = 0.9;
        kimix::agent::ContextPruner pruner(o);
        kimix::agent::prune_call c = test_call();
        c.context_usage = 0.5;
        expect(!pruner.prune(h, c).earliest_removed_index.has_value());
        c.context_usage = 0.95;
        expect(pruner.prune(h, c).earliest_removed_index.has_value());
    };

    "disabled_pruner_returns_original"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "q"));
        h.push_back(make_tool(filler(2600), "c1"));
        kimix::agent::pruning_options o = test_options();
        o.enabled = false;
        kimix::agent::ContextPruner pruner(o);
        expect(!pruner.prune(h, test_call()).earliest_removed_index.has_value());
    };

    "max_fraction_per_pass_caps_selection_tail_inward"_test = [] {
        // Two 2048-char outputs (512 tok each); current ~= 1025 tok so the
        // 50% cap allows only ONE - and tail-inward order picks the LATER.
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "q"));
        h.push_back(make_tool(filler(2048), "c1"));
        h.push_back(make_tool(filler(2048), "c2"));
        kimix::agent::ContextPruner pruner(test_options());
        kimix::agent::pruning_result r = pruner.prune(h, test_call());
        expect(r.elided.size() == 1u);
        expect(r.elided[0].index == 2);
        expect(!kimix::agent::is_pruned_stub(r.messages[1]));
        expect(kimix::agent::is_pruned_stub(r.messages[2]));
    };

    "candidate_sort_prefers_higher_index"_test = [] {
        // With a budget too small for both, the higher-index candidate wins
        // even when the lower one is Tier A (index dominates the tier break).
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "<notification id=7>n</notification>"));
        h.push_back(make_tool(filler(500), "c1"));
        kimix::agent::pruning_options o = test_options();
        o.tool_output_min_tokens = 50; // the 125-token output qualifies
        kimix::agent::ContextPruner pruner(o);
        kimix::agent::pruning_result r = pruner.prune(h, test_call());
        expect(r.elided.size() == 1u);
        expect(r.elided[0].index == 1);
        expect(!kimix::agent::is_pruned_stub(r.messages[0])); // notification kept
    };

    // ── estimate_after_prune (C14 dry run) ───────────────────────────────────
    "estimate_after_prune_is_dry_run"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "q"));
        h.push_back(make_tool(filler(2600), "c1"));
        kimix::agent::pruning_options o = test_options();
        o.cooldown_steps = 4;
        o.min_usage_growth = 0.05;
        kimix::agent::ContextPruner pruner(o);
        kimix::agent::prune_call c = test_call();
        const int64_t estimated = pruner.estimate_after_prune(h, c);
        expect(estimated < kimix::agent::estimate_history_tokens(h));
        // Hysteresis + ref state restored: the real prune at the same step
        // still applies and reuses the same ref id.
        kimix::agent::pruning_result r = pruner.prune(h, c);
        expect(r.earliest_removed_index.has_value());
        expect(r.elided[0].ref == "prune_0");
    };

    "estimate_after_prune_no_change_returns_current"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "q"));
        h.push_back(make_msg("assistant", "a"));
        kimix::agent::ContextPruner pruner(test_options());
        expect(pruner.estimate_after_prune(h, test_call()) ==
               kimix::agent::estimate_history_tokens(h));
    };

    // ── prune_with_policy (the manual path) ──────────────────────────────────
    "prune_with_policy_bypasses_cooldown_and_gate"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "q"));
        h.push_back(make_tool(filler(2600), "c1"));
        kimix::agent::pruning_options o = test_options();
        o.cooldown_steps = 4;
        kimix::agent::ContextPruner pruner(o);
        kimix::agent::prune_call c = test_call();
        expect(pruner.prune(h, c).earliest_removed_index.has_value());
        // Same step: the cooldown blocks the auto pass.
        expect(!pruner.prune(h, c).earliest_removed_index.has_value());
        // Manual invocation acts anyway (fresh pruner, gates bypassed).
        kimix::agent::prune_policy_call pc;
        pc.max_context_size = 32; // small window -> positive budget
        pc.keep_recent_turns = 1; // the 2-message history leaves the tool free
        kimix::agent::pruning_result r = pruner.prune_with_policy(h, pc);
        expect(r.earliest_removed_index.has_value());
        // The ref counter round-tripped: the next id continues the series.
        expect(r.elided[0].ref == "prune_1");
    };

    "prune_with_policy_remove_tool_results_false_disables_tier_b"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "q"));
        h.push_back(make_tool(filler(2600), "c1"));
        kimix::agent::ContextPruner pruner(test_options());
        kimix::agent::prune_policy_call pc;
        pc.max_context_size = 32;
        pc.remove_tool_results = false;
        expect(!pruner.prune_with_policy(h, pc).earliest_removed_index.has_value());
    };

    "prune_with_policy_keep_recent_turns_window"_test = [] {
        // 4 head fillers + two task snapshots (4, 5) + 14 tail fillers: the
        // keep-newest rule drops the OLDER snapshot only when it is outside
        // the keep_recent_turns tail window.
        kimix::vector<kimix::llm::Message> h;
        for (int i = 0; i < 4; ++i) {
            h.push_back(make_msg("user", filler(64)));
        }
        h.push_back(
            make_msg("user", "<active-background-tasks>one</active-background-tasks>"));
        h.push_back(
            make_msg("user", "<active-background-tasks>two</active-background-tasks>"));
        for (int i = 0; i < 14; ++i) {
            h.push_back(make_msg(i % 2 ? "assistant" : "user", filler(64)));
        }
        kimix::agent::ContextPruner pruner(test_options());
        // A 6-turn window leaves the snapshots outside the tail: the older
        // one drops (keep-newest keeps the newest).
        kimix::agent::prune_policy_call narrow;
        narrow.max_context_size = 32;
        narrow.keep_recent_turns = 6;
        kimix::agent::pruning_result r = pruner.prune_with_policy(h, narrow);
        expect(r.earliest_removed_index.has_value());
        expect(r.messages.size() == h.size() - 1);
        // A 16-turn window covers both snapshots: nothing prunes.
        kimix::agent::prune_policy_call wide;
        wide.max_context_size = 32;
        wide.keep_recent_turns = 16;
        expect(!pruner.prune_with_policy(h, wide).earliest_removed_index.has_value());
    };

    "prune_with_policy_target_token_count"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "q"));
        h.push_back(make_tool(filler(2600), "c1"));
        kimix::agent::ContextPruner pruner(test_options());
        // Target at/above the current size: budget <= 0, nothing to do.
        kimix::agent::prune_policy_call high;
        high.target_token_count = 128000; // ratio 1.0 -> target == the window
        expect(!pruner.prune_with_policy(h, high).earliest_removed_index.has_value());
        // Target below the current size: prunes toward it.
        kimix::agent::prune_policy_call low;
        low.target_token_count = 100;
        expect(pruner.prune_with_policy(h, low).earliest_removed_index.has_value());
    };

    // ── Tier C micro-compress ────────────────────────────────────────────────
    "tier_c_micro_compresses_and_annotates"_test = [] {
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "q"));
        kimix::string body;
        for (int i = 0; i < 40; ++i) {
            body += "        indented line with some payload text\n";
        }
        h.push_back(make_tool(body, "c1"));
        kimix::agent::pruning_options o = test_options();
        o.micro_compress_enabled = true;
        kimix::agent::ContextPruner pruner(o);
        kimix::agent::pruning_result r = pruner.prune(h, test_call());
        expect(r.earliest_removed_index.has_value());
        expect(r.messages.size() == h.size()); // in place, nothing dropped
        expect(r.messages[1].tool_call_id == "c1");
        expect(r.messages[1].content.size() < h[1].content.size());
        bool has_micro_record = false;
        for (const kimix::agent::elided_record &rec : r.elided) {
            if (rec.kind == "micro_compress") {
                has_micro_record = true;
                expect(rec.original_text == body);
                expect(rec.summary.find("micro-compressed") == 0u);
            }
        }
        expect(has_micro_record);
        // Idempotent: a second pass finds nothing to change.
        kimix::agent::ContextPruner pruner2(o);
        expect(!pruner2.prune(r.messages, test_call()).earliest_removed_index.has_value());
    };

    "tier_c_excludes_tier_b_candidates"_test = [] {
        // An oversized output is a Tier B candidate: Tier C must NOT rewrite
        // it, so the stub archives the true (uncompressed) original.
        kimix::vector<kimix::llm::Message> h;
        h.push_back(make_msg("user", "q"));
        const kimix::string body = filler(2600);
        h.push_back(make_tool(body, "c1"));
        kimix::agent::pruning_options o = test_options();
        o.micro_compress_enabled = true;
        kimix::agent::ContextPruner pruner(o);
        kimix::agent::pruning_result r = pruner.prune(h, test_call());
        expect(r.elided.size() == 1u);
        expect(r.elided[0].kind == "oversized_output");
        expect(r.elided[0].original_text == body);
        expect(r.messages[1].content.find("[context-elided:") != kimix::string::npos);
    };

    // ── D5: prune_N archive round trip ───────────────────────────────────────
    "prune_ref_resolves_through_session_index"_test = [] {
        kimix::agent::AgentSession session;
        session.append_history(make_msg("user", "hello there"));
        // The producer flow: reserve the id, archive the original under it -
        // get_by_id must resolve end to end.
        const uint32_t id = session.reserve_elided_turn_id();
        expect(id == 1u); // turn 0 was the user message
        session.archive_elided_original(id, "tool", "the elided original text");
        const kimix::optional<kimix::runtime::index::turn_meta> turn =
            session.history_get_by_id(id);
        expect(turn.has_value());
        expect(turn->text == "the elided original text");
        expect(turn->role == 2u); // tool
    };

    "soul_prune_allocator_refs_resolve"_test = [] {
        // Full D5 loop: the stub's ref id comes from the session's index
        // authority, so retrieve id=prune_N resolves to the archived original.
        kimix::agent::AgentSession session;
        session.append_history(make_msg("user", "q"));
        session.append_history(make_tool(filler(2600), "c1"));
        kimix::agent::ContextPruner pruner(test_options());
        kimix::agent::prune_call c = test_call();
        c.alloc_ref = [&session] {
            const uint32_t id = session.reserve_elided_turn_id();
            kimix::string ref = "prune_";
            const std::string n = std::to_string(static_cast<long long>(id));
            ref.append(n.c_str(), n.size());
            return ref;
        };
        kimix::agent::pruning_result r = pruner.prune(session.history(), c);
        expect(r.elided.size() == 1u);
        expect(r.elided[0].ref == "prune_2"); // ids 0+1 were the history turns
        session.archive_elided_original(2, r.elided[0].role, r.elided[0].original_text);
        const kimix::optional<kimix::runtime::index::turn_meta> turn =
            session.history_get_by_id(2);
        expect(turn.has_value());
        expect(turn->text == r.elided[0].original_text);
    };

    return 0;
}
