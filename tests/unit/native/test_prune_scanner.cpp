// Test for src/runtime/soul/prune_scanner.h (plan 015).
// This test covers context_pruning.py candidate selection:
// - protected set: stable head, recent tail turns, tool-pair units,
//   current-turn range
// - Tier A ephemeral candidates (system reminders, notifications, task
//   snapshots keeping the latest, D-Mail notices)
// - Tier B: superseded reads, oversized outputs, resolved errors
// - index-ascending stable emission; empty history

#include "ut/ut.hpp"
#include <runtime/soul/prune_scanner.h>
#include <runtime/soul/message_view.h>
#include "unit/native/soul_test_util.h"

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::soul;
using soul_test::message_builder;
using soul_test::part_kind;

namespace {

kimix::vector<prune_action> scan(const kimix::vector<message_view>& msgs,
                                 const prune_policy& policy = prune_policy()) {
    kimix::vector<prune_action> out;
    PruneScanner scanner;
    scanner.scan(msgs, policy, out);
    return out;
}

bool has_reason(const kimix::vector<prune_action>& acts, uint32_t index,
                uint8_t reason) {
    for (const auto& a : acts) {
        if (a.index == index) {
            return a.reason == reason;
        }
    }
    return false;
}

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "prune_empty_history"_test = [] {
        kimix::vector<message_view> none;
        expect(scan(none).empty());
    };

    "prune_head_and_tail_protected"_test = [] {
        message_builder b;
        // 10 messages: head (4) + tail (6) cover everything -> all protected.
        for (int i = 0; i < 10; ++i) {
            b.begin_message(static_cast<uint8_t>(i % 2 == 0 ? kRoleUser : kRoleAssistant));
            b.part(part_kind::TEXT, "m");
        }
        const auto acts = scan(b.finish());
        expect(eq(acts.size(), size_t{10}));
        for (const auto& a : acts) {
            expect(eq(a.reason, kPruneProtect));
        }
    };

    "prune_superseded_read_pair"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "read file please");
        b.begin_message(kRoleAssistant);
        b.call("call_1", "read_file", R"({"path":"a.txt"})");
        b.begin_message(kRoleTool, "call_1");
        b.part(part_kind::TEXT, "BIG OLD RESULT CONTENT THAT IS LONG");
        b.begin_message(kRoleAssistant);
        b.call("call_2", "read_file", R"({"path":"a.txt"})");
        b.begin_message(kRoleTool, "call_2");
        b.part(part_kind::TEXT, "short");
        prune_policy policy;
        policy.stable_prefix_messages = 0;
        policy.recent_messages_protected = 0;
        const auto acts = scan(b.finish(), policy);
        // index 2 is superseded (later tool result "short" < len/2); index 4
        // is the newest tool result (nothing later) so it is not.
        expect(has_reason(acts, 2, kPruneSupersededRead)) << "superseded read at 2";
        expect(!has_reason(acts, 4, kPruneSupersededRead));
    };

    "prune_superseded_via_empty_marker"_test = [] {
        message_builder b;
        b.begin_message(kRoleTool, "c1");
        b.part(part_kind::TEXT, "content one");
        b.begin_message(kRoleTool, "c2");
        b.part(part_kind::TEXT, "Tool output is empty");
        prune_policy policy;
        policy.stable_prefix_messages = 0;
        policy.recent_messages_protected = 0;
        // index 0 superseded because a later result has the empty marker.
        const auto acts = scan(b.finish(), policy);
        expect(has_reason(acts, 0, kPruneSupersededRead));
    };

    "prune_resolved_error_chain"_test = [] {
        message_builder b;
        b.begin_message(kRoleTool, "e1");
        b.part(part_kind::TEXT, "<system>ERROR: boom</system>");
        b.begin_message(kRoleTool, "e2");
        b.part(part_kind::TEXT, "<system>ERROR: boom again</system>");
        b.begin_message(kRoleTool, "ok1");
        b.part(part_kind::TEXT, "worked fine and everything is good now");
        prune_policy policy;
        policy.stable_prefix_messages = 0;
        policy.recent_messages_protected = 0;
        const auto acts = scan(b.finish(), policy);
        // Both errors resolved by the later success.
        expect(has_reason(acts, 0, kPruneResolvedError));
        expect(has_reason(acts, 1, kPruneResolvedError));
    };

    "prune_oversized_output"_test = [] {
        message_builder b;
        b.begin_message(kRoleTool, "big");
        b.part(part_kind::TEXT, kimix::string(4096, 'x'));
        prune_policy policy;
        policy.stable_prefix_messages = 0;
        policy.recent_messages_protected = 0;
        policy.tool_output_min_tokens = 512;
        const auto acts = scan(b.finish(), policy);
        expect(has_reason(acts, 0, kPruneOversizedOutput));
    };

    "prune_tool_pair_protection"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "go");
        b.begin_message(kRoleAssistant);
        b.call("call_x", "f", "{}");
        b.begin_message(kRoleTool, "call_x");
        b.part(part_kind::TEXT, "result");
        // tail protection covers the last 6 user/assistant turns: user(0),
        // assistant(1) protected; tool(2) must be protected via pair unit.
        const auto acts = scan(b.finish());
        expect(has_reason(acts, 2, kPruneProtect)) << "tool result protected";
    };

    "prune_ephemeral_drops"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "<system-reminder>\nstay on task\n</system-reminder>");
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "<notification id=\"n1\" category=\"task\">t</notification>");
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "real user message");
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "<active-background-tasks>snapshot one</active-background-tasks>");
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "D-Mail from your future self: remember");
        prune_policy policy;
        policy.stable_prefix_messages = 2; // head protects indices 0-1
        policy.recent_messages_protected = 0;
        const auto acts = scan(b.finish(), policy);
        expect(has_reason(acts, 0, kPruneProtect)) << "reminder in head";
        expect(has_reason(acts, 1, kPruneProtect)) << "notification in head";
        // index 3 is the single task snapshot (kept, no action); index 4 is
        // a D-Mail notice -> Tier A compact drop.
        expect(!has_reason(acts, 3, kPruneCompact));
        expect(has_reason(acts, 4, kPruneCompact)) << "dmail dropped";
    };

    "prune_snapshot_keeps_latest"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "old snapshot <active-background-tasks>a</active-background-tasks>");
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "latest snapshot <active-background-tasks>b</active-background-tasks>");
        prune_policy policy;
        policy.stable_prefix_messages = 0;
        policy.recent_messages_protected = 0;
        const auto acts = scan(b.finish(), policy);
        // Older snapshot dropped; latest kept (no action).
        expect(has_reason(acts, 0, kPruneCompact));
        expect(!has_reason(acts, 1, kPruneCompact));
    };

    "prune_current_turn_protection"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "old");
        b.begin_message(kRoleTool, "t1");
        b.part(part_kind::TEXT, "<system>ERROR: x</system>");
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "current");
        prune_policy policy;
        policy.stable_prefix_messages = 0;
        policy.recent_messages_protected = 0;
        policy.current_turn_index = 2;
        const auto acts = scan(b.finish(), policy);
        // index 0 unprotected; index 1 (error) would resolve but is NOT after
        // a success; index 2+ protected by current turn.
        expect(has_reason(acts, 2, kPruneProtect));
        expect(!has_reason(acts, 0, kPruneProtect));
    };

    "prune_index_ascending_stable"_test = [] {
        message_builder b;
        for (int i = 0; i < 20; ++i) {
            b.begin_message(kRoleUser);
            b.part(part_kind::TEXT, "<system-reminder>\nr\n</system-reminder>");
        }
        prune_policy policy;
        policy.stable_prefix_messages = 0;
        policy.recent_messages_protected = 0;
        const auto acts = scan(b.finish(), policy);
        expect(eq(acts.size(), size_t{20}));
        for (size_t i = 1; i < acts.size(); ++i) {
            expect(acts[i - 1].index < acts[i].index);
        }
    };

    return 0;
}
