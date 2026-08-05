// Test for src/runtime/soul/prune_scanner.h PruneScanner::prune_history (Plan 017).
// This test covers:
// - empty history
// - no unprotected messages (all protected)
// - all ephemeral types (system reminder, notification, task snapshot, D-Mail,
//   checkpoint)
// - tool-pair unit protection
// - assistant with multiple tool_calls
// - superseded reads
// - oversized output boundary
// - resolved errors
// - task snapshot "keep latest"
// - negation rules (policy toggles)
// - budget cap via max_elision_tokens

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

prune_history_result run(kimix::vector<message_view>& msgs,
                         const prune_policy& policy) {
    prune_history_result out;
    PruneScanner scanner;
    scanner.prune_history(msgs, policy, out);
    return out;
}

const prune_history_action* find_action(const prune_history_result& result,
                                        uint32_t index) {
    for (const auto& a : result.actions) {
        if (a.index == index) {
            return &a;
        }
    }
    return nullptr;
}

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "prune_history_empty"_test = [] {
        kimix::vector<message_view> none;
        prune_policy policy;
        policy.stable_prefix_messages = 0;
        policy.recent_messages_protected = 0;
        policy.max_elision_tokens = 10000;
        const auto result = run(none, policy);
        expect(result.actions.empty());
        expect(eq(result.freed_tokens, uint32_t{0}));
        expect(eq(result.earliest_removed_index, uint32_t{UINT32_MAX}));
    };

    "prune_history_all_protected"_test = [] {
        message_builder b;
        for (int i = 0; i < 10; ++i) {
            b.begin_message(static_cast<uint8_t>(i % 2 == 0 ? kRoleUser : kRoleAssistant));
            b.part(part_kind::TEXT, "m");
        }
        auto msgs = b.finish();
        prune_policy policy;
        policy.max_elision_tokens = 10000;
        const auto result = run(msgs, policy);
        expect(result.actions.empty());
        expect(eq(result.freed_tokens, uint32_t{0}));
    };

    "prune_history_ephemeral_types"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "<system-reminder>\nstay on task\n</system-reminder>");
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "<notification id=\"n1\" category=\"task\">t</notification>");
        // Two snapshots so the older one is dropped; the latest is kept.
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "<active-background-tasks>old snapshot</active-background-tasks>");
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "<active-background-tasks>latest snapshot</active-background-tasks>");
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "D-Mail from your future self: remember");
        b.begin_message(kRoleSystem);
        b.part(part_kind::TEXT, "  <system>CHECKPOINT alpha</system>  ");
        auto msgs = b.finish();
        prune_policy policy;
        policy.stable_prefix_messages = 0;
        policy.recent_messages_protected = 0;
        policy.drop_checkpoints = true;
        policy.max_elision_tokens = 10000;
        const auto result = run(msgs, policy);
        expect(eq(result.actions.size(), size_t{5}));
        for (const auto& a : result.actions) {
            expect(eq(a.reason, kPruneCompact));
            expect(gt(a.savings, uint32_t{0}));
        }
        // Latest snapshot (index 3) is the only task snapshot kept.
        expect(!find_action(result, 3));
    };

    "prune_history_tool_pair_protection"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "go");
        b.begin_message(kRoleAssistant);
        b.call("call_x", "f", "{}");
        b.begin_message(kRoleTool, "call_x");
        b.part(part_kind::TEXT, "result");
        auto msgs = b.finish();
        prune_policy policy;
        policy.max_elision_tokens = 10000;
        const auto result = run(msgs, policy);
        expect(result.actions.empty()) << "tool result protected by pair unit";
    };

    "prune_history_multiple_tool_calls"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "go");
        b.begin_message(kRoleAssistant);
        b.call("c1", "f", "{}");
        b.call("c2", "f", "{}");
        b.begin_message(kRoleTool, "c1");
        b.part(part_kind::TEXT, "r1");
        b.begin_message(kRoleTool, "c2");
        b.part(part_kind::TEXT, "r2");
        auto msgs = b.finish();
        prune_policy policy;
        policy.max_elision_tokens = 10000;
        const auto result = run(msgs, policy);
        expect(result.actions.empty()) << "both tool results protected";
    };

    "prune_history_superseded_read"_test = [] {
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
        auto msgs = b.finish();
        prune_policy policy;
        policy.stable_prefix_messages = 0;
        policy.recent_messages_protected = 0;
        policy.max_elision_tokens = 10000;
        const auto result = run(msgs, policy);
        const auto* a = find_action(result, 2);
        expect(a != nullptr) << "earlier tool result selected";
        expect(eq(a->reason, kPruneSupersededRead));
        expect(!find_action(result, 4)) << "latest tool result kept";
    };

    "prune_history_superseded_by_empty_marker"_test = [] {
        message_builder b;
        b.begin_message(kRoleTool, "c1");
        b.part(part_kind::TEXT, "content one");
        b.begin_message(kRoleTool, "c2");
        b.part(part_kind::TEXT, "Tool output is empty");
        auto msgs = b.finish();
        prune_policy policy;
        policy.stable_prefix_messages = 0;
        policy.recent_messages_protected = 0;
        policy.max_elision_tokens = 10000;
        const auto result = run(msgs, policy);
        const auto* a = find_action(result, 0);
        expect(a != nullptr);
        expect(eq(a->reason, kPruneSupersededRead));
    };

    "prune_history_oversized_boundary"_test = [] {
        message_builder b;
        b.begin_message(kRoleTool, "big");
        // 2047 chars -> 2047 cp -> 511 tokens (below 512).
        b.part(part_kind::TEXT, kimix::string(2047, 'x'));
        b.begin_message(kRoleTool, "exact");
        // 2048 chars -> 512 tokens (at boundary).
        b.part(part_kind::TEXT, kimix::string(2048, 'x'));
        auto msgs = b.finish();
        prune_policy policy;
        policy.stable_prefix_messages = 0;
        policy.recent_messages_protected = 0;
        policy.tool_output_min_tokens = 512;
        policy.max_elision_tokens = 10000;
        const auto result = run(msgs, policy);
        expect(!find_action(result, 0)) << "511 tokens not oversized";
        const auto* a = find_action(result, 1);
        expect(a != nullptr) << "512 tokens is oversized";
        expect(eq(a->reason, kPruneOversizedOutput));
        expect(eq(a->savings, uint32_t{512}));
    };

    "prune_history_resolved_error"_test = [] {
        message_builder b;
        b.begin_message(kRoleTool, "e1");
        b.part(part_kind::TEXT, "<system>ERROR: boom</system>");
        b.begin_message(kRoleTool, "e2");
        b.part(part_kind::TEXT, "<system>ERROR: boom again</system>");
        b.begin_message(kRoleTool, "ok1");
        b.part(part_kind::TEXT, "worked fine and everything is good now");
        auto msgs = b.finish();
        prune_policy policy;
        policy.stable_prefix_messages = 0;
        policy.recent_messages_protected = 0;
        policy.max_elision_tokens = 10000;
        const auto result = run(msgs, policy);
        expect(find_action(result, 0) != nullptr);
        expect(find_action(result, 1) != nullptr);
        expect(eq(find_action(result, 0)->reason, kPruneResolvedError));
        expect(eq(find_action(result, 1)->reason, kPruneResolvedError));
        expect(!find_action(result, 2));
    };

    "prune_history_snapshot_keep_latest"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "old snapshot <active-background-tasks>a</active-background-tasks>");
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "latest snapshot <active-background-tasks>b</active-background-tasks>");
        auto msgs = b.finish();
        prune_policy policy;
        policy.stable_prefix_messages = 0;
        policy.recent_messages_protected = 0;
        policy.max_elision_tokens = 10000;
        const auto result = run(msgs, policy);
        const auto* old = find_action(result, 0);
        expect(old != nullptr);
        expect(eq(old->reason, kPruneCompact));
        expect(!find_action(result, 1)) << "latest snapshot kept";
    };

    "prune_history_negation_rules"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "<notification id=\"n1\">x</notification>");
        auto msgs = b.finish();
        prune_policy policy;
        policy.stable_prefix_messages = 0;
        policy.recent_messages_protected = 0;
        policy.drop_notifications = false;
        policy.max_elision_tokens = 10000;
        const auto result = run(msgs, policy);
        expect(result.actions.empty()) << "notification kept when drop_notifications=false";
    };

    "prune_history_budget_cap"_test = [] {
        message_builder b;
        for (int i = 0; i < 8; ++i) {
            b.begin_message(kRoleUser);
            b.part(part_kind::TEXT, "<system-reminder>r</system-reminder>");
        }
        auto msgs = b.finish();
        prune_policy policy;
        policy.stable_prefix_messages = 0;
        policy.recent_messages_protected = 0;
        // Each reminder saves max(len("<system-reminder>r</system-reminder>")//4,1)=9 tokens.
        policy.max_elision_tokens = 18;
        const auto result = run(msgs, policy);
        expect(eq(result.actions.size(), size_t{2})) << "budget caps selection";
        expect(eq(result.freed_tokens, uint32_t{18}));
        // Latest indices are selected first.
        expect(eq(result.actions[0].index, uint32_t{6}));
        expect(eq(result.actions[1].index, uint32_t{7}));
    };

    "prune_history_earliest_removed"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "keep");
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "<system-reminder>drop</system-reminder>");
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "keep");
        auto msgs = b.finish();
        prune_policy policy;
        policy.stable_prefix_messages = 0;
        policy.recent_messages_protected = 0;
        policy.max_elision_tokens = 10000;
        const auto result = run(msgs, policy);
        expect(eq(result.actions.size(), size_t{1}));
        expect(eq(result.actions[0].index, uint32_t{1}));
        expect(eq(result.earliest_removed_index, uint32_t{1}));
    };

    return 0;
}
