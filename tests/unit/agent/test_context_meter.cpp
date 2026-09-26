// test_context_meter.cpp - Unit tests for the context-meter provider
// (src/agent/dynamic_injections/context_meter.h, a port of
// kimi_cli/soul/dynamic_injections/context_meter.py): the 20% first-fire
// floor, the delta + cooldown throttles, the compact-region suppression, the
// one-shot post-compaction cooldown bypass (only when usage dropped), the
// afk re-anchor, and the exact reminder string.
//
// Framework: Boost.UT (tests/ut/ut.hpp).
#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include <agent/dynamic_injections/context_meter.h>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

kimix::agent::InjectionStepContext ctx_at(double usage, int32_t step,
                                          int64_t max_tokens = 1000) {
    kimix::agent::InjectionStepContext c;
    c.max_context_tokens = max_tokens;
    c.token_count_with_pending =
        static_cast<int64_t>(usage * static_cast<double>(max_tokens));
    c.context_usage = usage;
    c.step_no = step;
    return c;
}

kimix::vector<kimix::agent::DynamicInjection>
collect(kimix::agent::ContextMeterProvider &p,
        const kimix::agent::InjectionStepContext &c) {
    kimix::vector<kimix::agent::DynamicInjection> out;
    kimix::string err;
    expect(p.get_injections(c, out, err)) << err;
    return out;
}

constexpr kimix::string_view k_meter_body =
    "Context is volatile — when unsure about history, recall past decisions, "
    "file paths, or errors with the `Retrieve` tool. "
    "There is no need to call it frequently — this reminder only fires when "
    "context usage materially changes, so retrieve only when there is "
    "something genuinely important.";

} // namespace

int main() {
    "silent_below_min_usage"_test = [] {
        kimix::agent::ContextMeterProvider p;
        expect(collect(p, ctx_at(0.19, 1)).empty());
        expect(collect(p, ctx_at(0.20, 1)).size() == 1u);
    };

    "first_fire_uses_verbatim_string"_test = [] {
        kimix::agent::ContextMeterProvider p;
        const auto got = collect(p, ctx_at(0.21, 1));
        expect(eq(got.size(), 1u));
        expect(eq(got[0].type, kimix::string("context_meter")));
        expect(eq(got[0].content, kimix::string(k_meter_body)));
    };

    "suppressed_in_compact_region"_test = [] {
        // suppress_above default 0.70: the compact reminder owns that band.
        kimix::agent::ContextMeterProvider p;
        expect(collect(p, ctx_at(0.70, 1)).empty());
        expect(collect(p, ctx_at(0.85, 1)).empty());
        // No suppression when disabled.
        kimix::agent::ContextMeterProvider p2(0.05, 5, kimix::optional<double>());
        expect(collect(p2, ctx_at(0.85, 1)).size() == 1u);
    };

    "delta_and_cooldown_throttle_refires"_test = [] {
        kimix::agent::ContextMeterProvider p(0.05, 5);
        expect(collect(p, ctx_at(0.30, 1)).size() == 1u);
        // 3% delta < 5%: no fire even though 5 steps have passed.
        expect(collect(p, ctx_at(0.33, 8)).empty());
        // 6% delta (> min_delta): fires. (An exact 5.0% delta is below the
        // guard in floating point - 0.35 - 0.30 == 0.04999... - which the
        // reference shares; do not test the exact boundary.)
        expect(collect(p, ctx_at(0.36, 8)).size() == 1u);
        // Inside the cooldown with a big delta: still quiet.
        expect(collect(p, ctx_at(0.55, 10)).empty());
        // Cooldown clear (5 steps since step 8) and delta ok: fires.
        expect(collect(p, ctx_at(0.56, 13)).size() == 1u);
    };

    "subagent_never_fires"_test = [] {
        kimix::agent::ContextMeterProvider p;
        kimix::agent::InjectionStepContext c = ctx_at(0.4, 1);
        c.is_subagent = true;
        expect(collect(p, c).empty());
    };

    "zero_max_tokens_never_fires"_test = [] {
        kimix::agent::ContextMeterProvider p;
        expect(collect(p, ctx_at(0.5, 1, 0)).empty());
    };

    "compaction_bypass_needs_a_usage_drop"_test = [] {
        kimix::agent::ContextMeterProvider p(0.05, 5);
        expect(collect(p, ctx_at(0.30, 1)).size() == 1u);
        // The flag is consumed on the FIRST evaluation after the hook
        // (context_meter.py:327-328), even when nothing fires: a
        // not-dropped usage spends the one-shot without bypassing.
        p.on_context_compacted();
        expect(collect(p, ctx_at(0.31, 2)).empty());
        // A real compaction drop on the next evaluation: the bypass applies
        // (cooldown skipped) and the delta guard still passes (0.31 -> 0.10).
        p.on_context_compacted();
        expect(collect(p, ctx_at(0.10, 3)).size() == 1u);
        // The flag is one-shot: a later evaluation no longer bypasses.
        expect(collect(p, ctx_at(0.05, 4)).empty());
    };

    "compaction_flag_consumed_even_when_it_fires_nothing"_test = [] {
        kimix::agent::ContextMeterProvider p(0.05, 5);
        // A compaction with NO prior injection: the first evaluation is the
        // fresh report and fires even below min_usage (the compaction flag
        // replaces the first-injection floor).
        p.on_context_compacted();
        expect(collect(p, ctx_at(0.10, 1)).size() == 1u);
        // Below min_delta since the report: quiet even past min_usage.
        expect(collect(p, ctx_at(0.12, 2)).empty());
        // Cooldown (5 steps) + delta both clear: fires again.
        expect(collect(p, ctx_at(0.27, 7)).size() == 1u);
    };

    "afk_changed_reanchors_without_clearing_throttle"_test = [] {
        kimix::agent::ContextMeterProvider p(0.05, 5);
        expect(collect(p, ctx_at(0.30, 10)).size() == 1u);
        p.on_afk_changed(true);
        // Re-anchored: the meter fires once more (still gated by min_usage
        // via the first-injection path).
        expect(collect(p, ctx_at(0.31, 11)).size() == 1u);
        expect(collect(p, ctx_at(0.32, 12)).empty());
    };
}
