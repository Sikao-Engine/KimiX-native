// agent/dynamic_injections/context_meter.h - Retrieve-tool meter provider.
//
// Byte-faithful port of kimi_cli/soul/dynamic_injections/context_meter.py
// (149 lines): injects the "Context is volatile — use Retrieve" reminder
// when usage materially changes (>= min_usage for the first injection,
// >= min_delta since the last one, cooldown-bound), staying silent above
// the compact threshold (owned by CompactReminderProvider). The
// post-compaction fresh report is a ONE-SHOT cooldown bypass that only
// applies when usage actually dropped; the afk hook re-anchors the meter
// without clearing the throttle anchors.
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "agent/dynamic_injection.h"

namespace kimix::agent {

class ContextMeterProvider final : public DynamicInjectionProvider {
public:
    // Knobs map to LoopControl: min_delta = context_meter_min_delta
    // (default 0.15), cooldown_steps = context_meter_cooldown_steps
    // (default 30), suppress_above = compact_reminder_threshold when the
    // compact reminder is enabled, else none (kimisoul.py:544-552);
    // min_usage default 0.20 (the provider constant).
    explicit ContextMeterProvider(double min_delta = 0.15,
                                  int32_t cooldown_steps = 30,
                                  kimix::optional<double> suppress_above = 0.70,
                                  double min_usage = 0.20);

    bool get_injections(const InjectionStepContext &ctx,
                        kimix::vector<DynamicInjection> &out,
                        kimix::string &error) override;

    // Mark a real compaction: the next evaluation may bypass the cooldown
    // ONLY when usage dropped below the last injected level (one-shot flag
    // consumed on first evaluation).
    void on_context_compacted() override;
    void on_afk_changed(bool enabled) override;

private:
    double _min_delta;
    int32_t _cooldown_steps;
    kimix::optional<double> _suppress_above;
    double _min_usage;
    kimix::optional<int32_t> _last_injected_step;
    kimix::optional<double> _last_injected_usage;
    bool _compaction_pending = false;
};

} // namespace kimix::agent
