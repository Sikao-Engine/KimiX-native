// agent/dynamic_injections/budget_reminder.cpp - see budget_reminder.h.

#include "agent/dynamic_injections/budget_reminder.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace kimix::agent {

namespace {
// The reference's time.monotonic default (a monotonic clock in seconds).
double steady_monotonic_seconds() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Python f"{x:.0f}" for non-negative x: round half to even.
int64_t py_float0(double v) {
    const double fl = std::floor(v);
    const double frac = v - fl;
    int64_t base = static_cast<int64_t>(fl);
    if (frac > 0.5) {
        ++base;
    } else if (frac == 0.5 && (base % 2) != 0) {
        ++base;
    }
    return base;
}
} // namespace

BudgetReminderProvider::BudgetReminderProvider(kimix::vector<double> warn_ratios,
                                               int32_t wall_clock_seconds,
                                               kimix::function<double()> clock)
    : _warn_ratios(std::move(warn_ratios)),
      _wall_clock_seconds(wall_clock_seconds < 0 ? 0 : wall_clock_seconds),
      _clock(clock ? std::move(clock) : kimix::function<double()>(steady_monotonic_seconds)) {
    std::sort(_warn_ratios.begin(), _warn_ratios.end());
    _warned_levels.assign(_warn_ratios.size(), false);
}

void BudgetReminderProvider::sync_turn(const InjectionStepContext &ctx) {
    // budget_reminder.py:57-64 (_sync_turn), with the string turn id
    // replaced by the session turn counter.
    if (ctx.turn_seq != 0 && ctx.turn_seq != _turn_seq) {
        _turn_seq = ctx.turn_seq;
        _turn_start = _clock();
        _turn_started = true;
        _warned_levels.assign(_warn_ratios.size(), false);
    } else if (!_turn_started) {
        _turn_start = _clock();
        _turn_started = true;
    }
}

bool BudgetReminderProvider::get_injections(const InjectionStepContext &ctx,
                                            kimix::vector<DynamicInjection> &out,
                                            kimix::string &error) {
    (void)error;
    // budget_reminder.py:82-125.
    sync_turn(ctx);
    if (_warn_ratios.empty()) {
        return true;
    }
    // (usage_ratio, remaining_steps, remaining_seconds) - _usages().
    const int32_t max_steps = ctx.max_steps_per_turn > 1 ? ctx.max_steps_per_turn : 1;
    const int32_t step_no = ctx.step_no;
    const double step_usage = static_cast<double>(step_no) / max_steps;
    const int64_t remaining_steps =
        max_steps > step_no ? static_cast<int64_t>(max_steps - step_no) : 0;

    double remaining_seconds = -1.0; // -1 == the reference's float("inf")
    double wall_usage = 0.0;
    if (_wall_clock_seconds > 0 && _turn_started) {
        const double elapsed = _clock() - _turn_start;
        wall_usage = elapsed / static_cast<double>(_wall_clock_seconds);
        remaining_seconds =
            _wall_clock_seconds > elapsed ? _wall_clock_seconds - elapsed : 0.0;
    }
    const double usage = step_usage > wall_usage ? step_usage : wall_usage;

    // Fire the highest crossed level that has not fired yet this turn.
    int64_t level = -1;
    for (size_t idx = 0; idx < _warn_ratios.size(); ++idx) {
        if (usage >= _warn_ratios[idx] && !_warned_levels[idx]) {
            level = static_cast<int64_t>(idx);
        }
    }
    if (level < 0) {
        return true;
    }
    // Consume this level and all lower ones: once a stronger warning fired,
    // a weaker late warning would be confusing.
    for (int64_t i = 0; i <= level; ++i) {
        _warned_levels[static_cast<size_t>(i)] = true;
    }
    kimix::string remaining_minutes;
    if (remaining_seconds >= 0.0) {
        // f" / ~{remaining_seconds / 60:.0f} minutes" - Python .0f rounds
        // half to even; a plain llround is fine for the display minute.
        const double minutes = remaining_seconds / 60.0;
        remaining_minutes = " / ~" +
                            kimix::string(std::to_string(py_float0(minutes))) +
                            " minutes";
    }

    DynamicInjection injection;
    injection.type = "budget_reminder";
    if (level >= static_cast<int64_t>(_warn_ratios.size()) - 1) {
        // budget_reminder.py:111-117 (final ratio wording, verbatim):
        // "Budget almost exhausted (usage ≥ {ratio:.0%}; ~{steps}{minutes} left). "
        // "Wrap up immediately: run the minimal verification and summarize "
        // "the current state. Do not start new sub-tasks."
        injection.content =
            kimix::string("Budget almost exhausted (usage ≥ ") +
            py_percent0(_warn_ratios[static_cast<size_t>(level)]) + "; ~" +
            kimix::string(std::to_string(remaining_steps)) + " steps" +
            remaining_minutes +
            " left). "
            "Wrap up immediately: run the minimal verification and summarize "
            "the current state. Do not start new sub-tasks.";
    } else {
        // budget_reminder.py:118-124 (lower-ratio wording, verbatim):
        // "Budget notice: {usage:.0%} of the step/time budget used "
        // "(~{steps}{minutes} left). Plan your wrap-up: prioritize remaining "
        // "todos, and reserve enough budget for verification and a final "
        // "summary."
        injection.content =
            kimix::string("Budget notice: ") + py_percent0(usage) +
            " of the step/time budget used (~" +
            kimix::string(std::to_string(remaining_steps)) + " steps" +
            remaining_minutes +
            " left). "
            "Plan your wrap-up: prioritize remaining todos, and reserve "
            "enough budget for verification and a final summary.";
    }
    out.push_back(std::move(injection));
    return true;
}

void BudgetReminderProvider::on_context_compacted() {
    // budget_reminder.py:127-138: keep all but the highest level marked as
    // fired so the most urgent warning may fire once more post-compaction.
    size_t highest = 0;
    bool any = false;
    for (size_t i = 0; i < _warned_levels.size(); ++i) {
        if (_warned_levels[i]) {
            highest = i;
            any = true;
        }
    }
    if (!any) {
        return;
    }
    for (size_t i = highest; i < _warned_levels.size(); ++i) {
        _warned_levels[i] = false;
    }
}

} // namespace kimix::agent
