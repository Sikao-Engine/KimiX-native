// agent/dynamic_injections/target_churn.h - anti-loop churn provider.
//
// Byte-faithful port of kimi_cli/soul/dynamic_injections/target_churn.py
// (251 lines): cross-tool per-file edit counting (edit tools via
// PATH_PARAM_KEYS, shell tools via redirect / sed -i / tee extraction) plus
// normalized-error fingerprint streaks, throttled to one alert per file per
// turn, one strong alert per file per turn, one error alert per turn, and a
// cooldown after any injection.
//
// The three shell-extraction patterns are ported regex-free (std::regex is
// never used in this codebase - see grep_tool.h / pwsh_tool.cpp):
//   _REDIRECT_RE = (?<![<>])>>?\s*([^\s;&|<>]+)
//   _SED_I_RE    = \bsed\s+(?:-\w+\s+)*-i(?:\.\S+)?(?:\s+['"][^'"]*['"])+
//   _TEE_RE      = \btee\s+(?:-\w+\s+)*([^\s;&|]+)
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "agent/dynamic_injection.h"

namespace kimix::agent {

// Extracts the shell write-targets of one command string (redirects,
// sed -i destinations, tee targets), each normalized for cross-tool
// comparison. Ports target_churn.py:_extract_tool_call_targets'
// SHELL_TOOLS branch; exported for tests.
kimix::vector<kimix::string> shell_write_targets(kimix::string_view command);

// Normalizes an error message so line numbers/paths/values don't matter
// (_normalize_error): quoted spans -> <str>, path-like spans -> <path>,
// digit runs -> <n>, whitespace collapsed to single spaces.
kimix::string normalize_error_text(kimix::string_view text);

class TargetChurnProvider final : public DynamicInjectionProvider {
public:
    // Knobs map to LoopControl: file_warn (>= 2, default 8), file_strong
    // (>= file_warn + 1, default 15), error_warn (>= 2, default 5),
    // cooldown_steps (>= 0, default 10).
    explicit TargetChurnProvider(int32_t file_warn = 8, int32_t file_strong = 15,
                                 int32_t error_warn = 5, int32_t cooldown_steps = 10);

    bool get_injections(const InjectionStepContext &ctx,
                        kimix::vector<DynamicInjection> &out,
                        kimix::string &error) override;

    // Reset all counters - compacted history no longer shows the churn.
    void on_context_compacted() override;

private:
    int32_t _file_warn;
    int32_t _file_strong;
    int32_t _error_warn;
    int32_t _cooldown_steps;

    size_t _cursor = 0; // next unprocessed history index
    kimix::vector_map<kimix::string, int32_t> _file_counts;
    uint64_t _error_last_fingerprint = 0;
    bool _has_error_fingerprint = false; // _error_last_fingerprint is None
    int32_t _error_streak = 0;

    // Per-turn alert dedup (reset when the turn identity changes).
    uint64_t _turn_seq = 0;
    kimix::unordered_set<kimix::string, kimix::string_hash> _warned_files;
    kimix::unordered_set<kimix::string, kimix::string_hash> _strong_warned_files;
    bool _error_warned_this_turn = false;

    kimix::optional<int32_t> _last_alert_step;

    void process_message(const kimix::llm::Message &message);
    void sync_turn(const InjectionStepContext &ctx);
};

} // namespace kimix::agent
