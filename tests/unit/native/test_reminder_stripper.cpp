// Test for src/runtime/soul/reminder_stripper.h + normalize_plan.h (plan 015).
// This test covers:
// - count_leading_reminders: leading run only, single-part TextPart rule,
//   all-reminders prefix, empty history
// - build_normalize_plan: adjacent user merge, notification boundary,
//   assistant/tool never merged, chained merges, empty history

#include "ut/ut.hpp"
#include <runtime/soul/reminder_stripper.h>
#include <runtime/soul/normalize_plan.h>
#include <runtime/soul/message_view.h>
#include "unit/native/soul_test_util.h"

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::soul;
using soul_test::message_builder;
using soul_test::part_kind;

namespace {

kimix::vector<normalize_step> plan(const kimix::vector<message_view>& msgs) {
    kimix::vector<normalize_step> out;
    build_normalize_plan(msgs, out);
    return out;
}

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "reminders_leading_run_counted"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "<system-reminder>\nreminder A\n</system-reminder>");
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "<system-reminder>\nreminder B\n</system-reminder>");
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "real user message");
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "<system-reminder>\nreminder C\n</system-reminder>");
        expect(eq(count_leading_reminders(b.finish()), 2u));
    };

    "reminders_single_part_text_rule"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "<system-reminder>\nx\n</system-reminder>");
        b.part(part_kind::TEXT, "second part"); // two parts -> not a reminder
        expect(eq(count_leading_reminders(b.finish()), 0u));

        message_builder b2;
        b2.begin_message(kRoleAssistant);
        b2.part(part_kind::TEXT, "<system-reminder>\nx\n</system-reminder>");
        expect(eq(count_leading_reminders(b2.finish()), 0u)); // wrong role
    };

    "reminders_all_prefix_and_empty"_test = [] {
        message_builder b;
        for (int i = 0; i < 5; ++i) {
            b.begin_message(kRoleUser);
            b.part(part_kind::TEXT, "<system-reminder>\nr\n</system-reminder>");
        }
        expect(eq(count_leading_reminders(b.finish()), 5u));
        kimix::vector<message_view> none;
        expect(eq(count_leading_reminders(none), 0u));
    };

    "normalize_merges_adjacent_users"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "first");
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "second");
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "third");
        b.begin_message(kRoleAssistant);
        b.part(part_kind::TEXT, "ok");
        b.begin_message(kRoleTool, "c1");
        b.part(part_kind::TEXT, "r");
        const auto steps = plan(b.finish());
        expect(eq(steps.size(), size_t{5}));
        // 1 and 2 merge into 0 (chained merge keeps target 0).
        expect(eq(steps[0].op, kNormalizeKeep));
        expect(eq(steps[0].target_index, 0u));
        expect(eq(steps[1].op, kNormalizeMergeInto));
        expect(eq(steps[1].target_index, 0u));
        expect(eq(steps[2].op, kNormalizeMergeInto));
        expect(eq(steps[2].target_index, 0u));
        expect(eq(steps[3].op, kNormalizeKeep));
        expect(eq(steps[4].op, kNormalizeKeep));
    };

    "normalize_notification_blocks_merge"_test = [] {
        message_builder b;
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "<notification id=\"n\" category=\"task\">t</notification>");
        b.begin_message(kRoleUser);
        b.part(part_kind::TEXT, "real user text");
        const auto steps = plan(b.finish());
        expect(eq(steps[0].op, kNormalizeKeep));
        expect(eq(steps[1].op, kNormalizeKeep)); // notification boundary
        expect(eq(steps[1].target_index, 1u));
    };

    "normalize_empty_history"_test = [] {
        kimix::vector<message_view> none;
        expect(plan(none).empty());
    };

    return 0;
}
