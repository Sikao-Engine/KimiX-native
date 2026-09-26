// agent/dynamic_injections/budget_reminder.h - budget-awareness provider.
//
// Byte-faithful port of kimi_cli/soul/dynamic_injections/budget_reminder.py
// (138 lines): as step (or optional wall-clock) usage crosses ascending
// warn ratios of the per-turn budget, fire one reminder per level per turn;
// the highest level may re-alert once after compaction.
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "agent/dynamic_injection.h"

namespace kimix::agent {

class BudgetReminderProvider final : public DynamicInjectionProvider {
public:
    // warn_ratios: ascending usage ratios, each triggering one reminder per
    // turn (default (0.7, 0.9) from LoopControl.budget_warn_ratios).
    // wall_clock_seconds: optional per-turn wall-clock budget; 0 disables
    // the wall-clock dimension (LoopControl.budget_wall_clock_seconds).
    // clock: monotonic-seconds source (injectable for tests; defaults to
    // std::chrono::steady_clock), the port of the reference's injectable
    // time.monotonic.
    explicit BudgetReminderProvider(
        kimix::vector<double> warn_ratios = {0.7, 0.9},
        int32_t wall_clock_seconds = 0,
        kimix::function<double()> clock = {});

    bool get_injections(const InjectionStepContext &ctx,
                        kimix::vector<DynamicInjection> &out,
                        kimix::string &error) override;

    // Allow the highest fired level to re-alert once after compaction
    // (budget_reminder.py:127-138).
    void on_context_compacted() override;

private:
    kimix::vector<double> _warn_ratios; // sorted ascending
    int32_t _wall_clock_seconds;
    kimix::function<double()> _clock;

    uint64_t _turn_seq = 0;      // last-seen turn identity
    bool _turn_started = false;  // _turn_start analogue (None until first sync)
    double _turn_start = 0.0;    // clock() at turn start
    kimix::vector<bool> _warned_levels;

    void sync_turn(const InjectionStepContext &ctx);
};

} // namespace kimix::agent
