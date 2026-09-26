// agent/dynamic_injections/compact_reminder.cpp - see compact_reminder.h.

#include "agent/dynamic_injections/compact_reminder.h"

namespace kimix::agent {

kimix::string compact_reminder_template(double usage, int64_t tokens,
                                        int64_t max_tokens) {
    // compact_reminder.py:158-163, the .format() result verbatim:
    //   "Context {usage:.0%} full ({tokens}/{max_tokens} tokens). "
    //   "Call `Compact` after completing the current atomic task, "
    //   "before auto-compaction forces it. "
    //   "Optionally pass an instruction to guide what to preserve."
    return kimix::string("Context ") + py_percent0(usage) + " full (" +
           kimix::string(std::to_string(tokens)) + "/" +
           kimix::string(std::to_string(max_tokens)) +
           " tokens). "
           "Call `Compact` after completing the current atomic task, "
           "before auto-compaction forces it. "
           "Optionally pass an instruction to guide what to preserve.";
}

CompactReminderProvider::CompactReminderProvider(double threshold,
                                                 int32_t cooldown_steps,
                                                 double min_usage)
    : _threshold(threshold), _cooldown_steps(cooldown_steps),
      _min_usage(min_usage) {}

bool CompactReminderProvider::get_injections(const InjectionStepContext &ctx,
                                             kimix::vector<DynamicInjection> &out,
                                             kimix::string &error) {
    (void)error;
    // compact_reminder.py:183-230.
    // Only inject for root sessions (skip subagents).
    if (ctx.is_subagent) {
        return true;
    }
    // The pending-inclusive count drives the trigger (tool results appended
    // since the last LLM call are counted), matching the auto-compaction
    // trigger; status.context_usage is the fallback when max_tokens == 0.
    const int64_t max_tokens = ctx.max_context_tokens;
    double context_usage = ctx.context_usage;
    if (max_tokens > 0) {
        context_usage =
            static_cast<double>(ctx.token_count_with_pending) /
            static_cast<double>(max_tokens);
    }
    if (context_usage < _min_usage) {
        // Nowhere near full: never nag below the hard floor even when the
        // configured threshold is lower than it.
        return true;
    }
    if (context_usage < _threshold) {
        return true;
    }
    // Throttle: skip when already injected and usage has not grown enough
    // (5%) AND not enough steps have passed since the last injection
    // (BOTH conditions re-arm; either one alone keeps it quiet).
    const int32_t step_no = ctx.step_no;
    if (_last_injected_step.has_value()) {
        const int32_t steps_since = step_no - *_last_injected_step;
        const double usage_growth = context_usage - _last_injected_usage;
        if (steps_since <= _cooldown_steps || usage_growth < 0.05) {
            return true;
        }
    }
    _last_injected_step = step_no;
    _last_injected_usage = context_usage;

    DynamicInjection injection;
    injection.type = "compact_reminder";
    injection.content = compact_reminder_template(context_usage, ctx.context_tokens,
                                                  ctx.max_context_tokens);
    out.push_back(std::move(injection));
    return true;
}

void CompactReminderProvider::on_context_compacted() {
    // compact_reminder.py:232-235: re-arm after compaction.
    _last_injected_step.reset();
    _last_injected_usage = 0.0;
}

void CompactReminderProvider::on_afk_changed(bool enabled) {
    // compact_reminder.py:237-241: same reset pattern.
    (void)enabled;
    _last_injected_step.reset();
    _last_injected_usage = 0.0;
}

} // namespace kimix::agent
