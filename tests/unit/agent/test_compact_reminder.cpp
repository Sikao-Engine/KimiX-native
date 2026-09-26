// test_compact_reminder.cpp - Unit tests for the compact-reminder provider
// (src/agent/dynamic_injections/compact_reminder.h, a port of
// kimi_cli/soul/dynamic_injections/compact_reminder.py): the 0.30 hard floor,
// the 0.70 trigger, the 5-step cooldown AND the 5% growth double-throttle,
// the exact reminder string, and the compaction/afk throttle resets.
//
// Framework: Boost.UT (tests/ut/ut.hpp).
#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include <agent/dynamic_injections/compact_reminder.h>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

kimix::agent::InjectionStepContext ctx_at(double usage_ratio, int32_t step,
                                          int64_t max_tokens = 1000,
                                          int64_t tokens = 0) {
    kimix::agent::InjectionStepContext ctx;
    ctx.max_context_tokens = max_tokens;
    ctx.token_count_with_pending =
        static_cast<int64_t>(usage_ratio * static_cast<double>(max_tokens));
    ctx.context_tokens = tokens > 0 ? tokens : ctx.token_count_with_pending;
    ctx.context_usage = usage_ratio;
    ctx.step_no = step;
    return ctx;
}

kimix::vector<kimix::agent::DynamicInjection>
collect(kimix::agent::CompactReminderProvider &p,
        const kimix::agent::InjectionStepContext &ctx) {
    kimix::vector<kimix::agent::DynamicInjection> out;
    kimix::string err;
    expect(p.get_injections(ctx, out, err)) << err;
    return out;
}

} // namespace

int main() {
    "silent_below_hard_floor_even_with_low_threshold"_test = [] {
        // threshold configured below the floor still never fires below 30%.
        kimix::agent::CompactReminderProvider p(/*threshold=*/0.10);
        expect(collect(p, ctx_at(0.29, 1)).empty());
        expect(collect(p, ctx_at(0.30, 1)).size() == 1u);
    };

    "fires_at_threshold_with_verbatim_string"_test = [] {
        kimix::agent::CompactReminderProvider p; // default 0.70
        const auto got = collect(p, ctx_at(0.70, 3, 1000, 700));
        expect(eq(got.size(), 1u));
        expect(eq(got[0].type, kimix::string("compact_reminder")));
        // compact_reminder.py:158-163 template with {usage:.0%} etc.
        expect(eq(got[0].content,
                  kimix::string("Context 70% full (700/1000 tokens). "
                                "Call `Compact` after completing the current atomic task, "
                                "before auto-compaction forces it. "
                                "Optionally pass an instruction to guide what to preserve.")));
    };

    "silent_between_floor_and_threshold"_test = [] {
        kimix::agent::CompactReminderProvider p;
        expect(collect(p, ctx_at(0.50, 1)).empty());
    };

    "subagent_never_fires"_test = [] {
        kimix::agent::CompactReminderProvider p;
        kimix::agent::InjectionStepContext ctx = ctx_at(0.95, 1);
        ctx.is_subagent = true;
        expect(collect(p, ctx).empty());
    };

    "cooldown_blocks_refire_within_five_steps"_test = [] {
        kimix::agent::CompactReminderProvider p;
        expect(collect(p, ctx_at(0.70, 1)).size() == 1u);
        // Growth alone does not re-fire inside the cooldown window
        // (steps_since <= 5 blocks even with >5% growth).
        expect(collect(p, ctx_at(0.90, 5)).empty());
        // Clears only after MORE than 5 steps with >= 5% growth.
        expect(collect(p, ctx_at(0.90, 7)).size() == 1u);
    };

    "growth_requirement_blocks_refire_after_cooldown"_test = [] {
        kimix::agent::CompactReminderProvider p;
        expect(collect(p, ctx_at(0.70, 1)).size() == 1u);
        // 6 steps passed (cooldown clear) but usage grew only 3%: stays quiet.
        expect(collect(p, ctx_at(0.73, 8)).empty());
        // 5% growth exactly re-arms.
        expect(collect(p, ctx_at(0.75, 9)).size() == 1u);
    };

    "context_usage_fallback_when_max_tokens_unknown"_test = [] {
        kimix::agent::CompactReminderProvider p;
        kimix::agent::InjectionStepContext ctx = ctx_at(0.0, 1);
        ctx.max_context_tokens = 0; // unknown window -> status.context_usage
        ctx.context_usage = 0.80;
        ctx.token_count_with_pending = 0;
        expect(collect(p, ctx).size() == 1u);
    };

    "on_context_compacted_rearms"_test = [] {
        kimix::agent::CompactReminderProvider p;
        expect(collect(p, ctx_at(0.70, 1)).size() == 1u);
        expect(collect(p, ctx_at(0.71, 2)).empty()); // throttled
        p.on_context_compacted();
        expect(collect(p, ctx_at(0.71, 2)).size() == 1u); // re-armed
    };

    "on_afk_changed_rearms"_test = [] {
        kimix::agent::CompactReminderProvider p;
        expect(collect(p, ctx_at(0.70, 1)).size() == 1u);
        p.on_afk_changed(true);
        expect(collect(p, ctx_at(0.70, 1)).size() == 1u);
    };
}
