// agent/dynamic_injections/compact_reminder.h - compaction nudge provider.
//
// Byte-faithful port of kimi_cli/soul/dynamic_injections/compact_reminder.py
// (103 lines): fires at/above a context-usage threshold with a hard
// below-this floor (never nag below 30%), a step cooldown AND a 5% usage
// growth requirement (both must clear to re-fire), plus throttle resets on
// compaction and on afk change.
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "agent/dynamic_injection.h"

namespace kimix::agent {

// Hard floor: never inject below this ratio regardless of the configured
// threshold (MIN_CONTEXT_USAGE, compact_reminder.py:156).
inline constexpr double kCompactReminderMinUsage = 0.30;

// The reminder body (compact_reminder.py:158-163, exact wording).
kimix::string compact_reminder_template(double usage, int64_t tokens,
                                        int64_t max_tokens);

class CompactReminderProvider final : public DynamicInjectionProvider {
public:
    // threshold defaults to LoopControl.compact_reminder_threshold (0.70);
    // cooldown_steps to the reference provider default (5); min_usage to
    // MIN_CONTEXT_USAGE (0.30).
    explicit CompactReminderProvider(double threshold = 0.70,
                                     int32_t cooldown_steps = 5,
                                     double min_usage = kCompactReminderMinUsage);

    bool get_injections(const InjectionStepContext &ctx,
                        kimix::vector<DynamicInjection> &out,
                        kimix::string &error) override;

    void on_context_compacted() override;
    void on_afk_changed(bool enabled) override;

private:
    double _threshold;
    int32_t _cooldown_steps;
    double _min_usage;
    kimix::optional<int32_t> _last_injected_step; // None until first fire
    double _last_injected_usage = 0.0;
};

} // namespace kimix::agent
