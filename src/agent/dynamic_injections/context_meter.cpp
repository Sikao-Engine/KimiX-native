// agent/dynamic_injections/context_meter.cpp - see context_meter.h.

#include "agent/dynamic_injections/context_meter.h"

namespace kimix::agent {

ContextMeterProvider::ContextMeterProvider(double min_delta,
                                           int32_t cooldown_steps,
                                           kimix::optional<double> suppress_above,
                                           double min_usage)
    : _min_delta(min_delta), _cooldown_steps(cooldown_steps < 0 ? 0 : cooldown_steps),
      _suppress_above(suppress_above), _min_usage(min_usage) {}

bool ContextMeterProvider::get_injections(const InjectionStepContext &ctx,
                                          kimix::vector<DynamicInjection> &out,
                                          kimix::string &error) {
    (void)error;
    // context_meter.py:300-368.
    // Only meter root sessions (skip subagents).
    if (ctx.is_subagent) {
        return true;
    }
    const int64_t max_tokens = ctx.max_context_tokens;
    if (max_tokens <= 0) {
        return true;
    }
    const double usage =
        static_cast<double>(ctx.token_count_with_pending) /
        static_cast<double>(max_tokens);

    // High-usage region belongs to the compact reminder.
    if (_suppress_above.has_value() && usage >= *_suppress_above) {
        return true;
    }
    const int32_t step_no = ctx.step_no;

    // The post-compaction "fresh report" is a one-shot: consume the flag on
    // the first evaluation regardless of outcome, so a harness that
    // re-notifies providers on every step can never keep the bypass alive.
    const bool compaction_pending = _compaction_pending;
    _compaction_pending = false;

    // ── Frequency guards ──
    // The cooldown and delta guards are the ONLY thing between a periodic
    // reminder and a per-step nag; they hold even after
    // on_context_compacted()/on_afk_changed() ran, because those hooks never
    // clear the throttle anchors here.
    if (_last_injected_step.has_value()) {
        const int32_t steps_since = step_no - *_last_injected_step;
        const bool cooldown_ok = steps_since >= _cooldown_steps;
        // A real compaction drops usage below the last injected level; only
        // then may the one-shot bypass the cooldown. A spurious reset does
        // not drop usage, so the cooldown keeps applying.
        const bool usage_dropped =
            _last_injected_usage.has_value() && usage < *_last_injected_usage;
        if (!cooldown_ok && !(compaction_pending && usage_dropped)) {
            return true;
        }
        if (_last_injected_usage.has_value()) {
            const double usage_delta =
                usage > *_last_injected_usage ? usage - *_last_injected_usage
                                              : *_last_injected_usage - usage;
            if (usage_delta < _min_delta) {
                return true;
            }
        }
    } else if (!compaction_pending && usage < _min_usage) {
        // First injection (or right after an AFK resume): require a minimum
        // usage so we don't nag during a nearly-empty session.
        return true;
    }

    _last_injected_step = step_no;
    _last_injected_usage = usage;

    // context_meter.py:361-367 (verbatim):
    // "Context is volatile — when unsure about history, recall past
    // decisions, file paths, or errors with the `Retrieve` tool. There is no
    // need to call it frequently — this reminder only fires when context
    // usage materially changes, so retrieve only when there is something
    // genuinely important."
    DynamicInjection injection;
    injection.type = "context_meter";
    injection.content =
        "Context is volatile — when unsure about history, recall past decisions, "
        "file paths, or errors with the `Retrieve` tool. "
        "There is no need to call it frequently — this reminder only fires when "
        "context usage materially changes, so retrieve only when there is "
        "something genuinely important.";
    out.push_back(std::move(injection));
    return true;
}

void ContextMeterProvider::on_context_compacted() {
    // context_meter.py:370-383: one-shot flag only; the throttle anchors
    // (last step / last usage) are intentionally NOT cleared.
    _compaction_pending = true;
}

void ContextMeterProvider::on_afk_changed(bool enabled) {
    // context_meter.py:385-389: re-anchor after AFK so the meter fires once
    // when the agent resumes (still gated by min_usage via the
    // first-injection path).
    (void)enabled;
    _last_injected_step.reset();
    _last_injected_usage.reset();
}

} // namespace kimix::agent
