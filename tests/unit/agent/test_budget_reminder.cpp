// test_budget_reminder.cpp - Unit tests for the budget-reminder provider
// (src/agent/dynamic_injections/budget_reminder.h, a port of
// kimi_cli/soul/dynamic_injections/budget_reminder.py): the 0.7/0.9 warn
// ratios, one fire per level per turn, consumption of lower levels by a
// higher fire, the wall-clock dimension with an injectable clock, the
// post-compaction highest-level re-alert, and the exact reminder strings.
//
// Framework: Boost.UT (tests/ut/ut.hpp).
#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include <agent/dynamic_injections/budget_reminder.h>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

kimix::agent::InjectionStepContext ctx(int32_t step, int32_t max_steps,
                                       uint64_t turn_seq = 1) {
    kimix::agent::InjectionStepContext c;
    c.step_no = step;
    c.max_steps_per_turn = max_steps;
    c.turn_seq = turn_seq;
    return c;
}

kimix::vector<kimix::agent::DynamicInjection>
collect(kimix::agent::BudgetReminderProvider &p,
        const kimix::agent::InjectionStepContext &c) {
    kimix::vector<kimix::agent::DynamicInjection> out;
    kimix::string err;
    expect(p.get_injections(c, out, err)) << err;
    return out;
}

// A manually advanced fake monotonic clock.
struct FakeClock {
    double now = 1000.0;
    double operator()() { return now; }
};

} // namespace

int main() {
    "silent_below_first_ratio"_test = [] {
        kimix::agent::BudgetReminderProvider p;
        expect(collect(p, ctx(6, 10)).empty()); // 60% < 70%
        expect(collect(p, ctx(7, 10)).size() == 1u); // 70% >= 0.7
    };

    "first_ratio_fires_once_with_notice_wording"_test = [] {
        kimix::agent::BudgetReminderProvider p;
        const auto got = collect(p, ctx(7, 10));
        expect(eq(got.size(), 1u));
        expect(eq(got[0].type, kimix::string("budget_reminder")));
        // budget_reminder.py:118-124 verbatim (usage 70%, ~3 steps left).
        expect(eq(got[0].content,
                  kimix::string("Budget notice: 70% of the step/time budget used "
                                "(~3 steps left). "
                                "Plan your wrap-up: prioritize remaining todos, and reserve "
                                "enough budget for verification and a final summary.")));
        // Same level never re-fires within the turn.
        expect(collect(p, ctx(8, 10)).empty());
    };

    "final_ratio_fires_with_urgent_wording_and_consumes_lower"_test = [] {
        kimix::agent::BudgetReminderProvider p;
        // Jump straight past both ratios: only the strongest fires.
        const auto got = collect(p, ctx(9, 10));
        expect(eq(got.size(), 1u));
        // budget_reminder.py:111-117 verbatim (usage ≥ 90%, ~1 step left).
        expect(eq(got[0].content,
                  kimix::string("Budget almost exhausted (usage ≥ 90%; ~1 steps left). "
                                "Wrap up immediately: run the minimal verification and "
                                "summarize the current state. Do not start new sub-tasks.")));
        // Both levels consumed: nothing more this turn.
        expect(collect(p, ctx(10, 10)).empty());
    };

    "new_turn_resets_fired_levels"_test = [] {
        kimix::agent::BudgetReminderProvider p;
        expect(collect(p, ctx(7, 10, /*turn=*/1)).size() == 1u);
        expect(collect(p, ctx(8, 10, 1)).empty());
        expect(collect(p, ctx(7, 10, /*turn=*/2)).size() == 1u);
    };

    "wall_clock_dimension_with_fake_clock"_test = [] {
        auto clock = kimix::shared_ptr<FakeClock>(new FakeClock);
        FakeClock *raw = clock.get();
        kimix::function<double()> fn = [clock] { return (*clock)(); };
        kimix::agent::BudgetReminderProvider p({0.5, 0.9}, /*wall_clock_seconds=*/100,
                                               std::move(fn));
        // Turn starts at t=1000 (sync_turn reads the clock on first call).
        expect(collect(p, ctx(1, 100)).empty());
        raw->now = 1050.0; // 50% of the wall-clock budget elapsed.
        const auto got = collect(p, ctx(2, 100));
        expect(eq(got.size(), 1u));
        expect(eq(got[0].content,
                  kimix::string("Budget notice: 50% of the step/time budget used "
                                "(~98 steps / ~1 minutes left). "
                                "Plan your wrap-up: prioritize remaining todos, and reserve "
                                "enough budget for verification and a final summary.")));
    };

    "wall_clock_urgent_level_appends_minutes"_test = [] {
        auto clock = kimix::shared_ptr<FakeClock>(new FakeClock);
        FakeClock *raw = clock.get();
        kimix::function<double()> fn = [clock] { return (*clock)(); };
        kimix::agent::BudgetReminderProvider p({0.9}, /*wall_clock_seconds=*/120,
                                               std::move(fn));
        expect(collect(p, ctx(1, 100)).empty());
        raw->now = 1114.0; // 114/120 = 95% wall usage; ~0.1 min -> "0 minutes"
        const auto got = collect(p, ctx(2, 100));
        expect(eq(got.size(), 1u));
        expect(got[0].content.find("Budget almost exhausted (usage ≥ 90%;") == 0u);
        expect(got[0].content.find("minutes left).") != kimix::string::npos);
    };

    "compaction_lets_highest_level_realert_once"_test = [] {
        kimix::agent::BudgetReminderProvider p;
        expect(collect(p, ctx(9, 10)).size() == 1u); // fires 90% level
        expect(collect(p, ctx(10, 10)).empty());
        p.on_context_compacted();
        // The highest level may fire ONE more time...
        expect(collect(p, ctx(10, 10)).size() == 1u);
        // ...but only once.
        expect(collect(p, ctx(10, 10)).empty());
    };

    "compaction_rearms_the_highest_fired_level_even_when_it_is_the_only_one"_test =
        [] {
            kimix::agent::BudgetReminderProvider p;
            expect(collect(p, ctx(7, 10)).size() == 1u); // 70% level only
            p.on_context_compacted();
            // The only fired level WAS the highest, so the hook clears it
            // and the notice may re-alert once (budget_reminder.py:134-138);
            // the unfired 90% level still fires when crossed.
            expect(collect(p, ctx(8, 10)).size() == 1u);
            expect(collect(p, ctx(9, 10)).size() == 1u);
            expect(collect(p, ctx(9, 10)).empty()); // consumed again
        };

    "empty_warn_ratios_never_fires"_test = [] {
        kimix::agent::BudgetReminderProvider p({}, 0);
        expect(collect(p, ctx(10, 10)).empty());
    };
}
