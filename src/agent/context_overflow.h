// agent/context_overflow.h - Context-overflow detection and the per-step
// recovery budget (Phase 4 §6.1 port of kimi_cli/soul/context_overflow.py).
//
// When a provider confirms the context window was exceeded (a 4xx API status
// error whose message matches the overflow markers), the soul force-compacts
// the context and re-runs the step instead of interrupting the session - see
// KimiSoul::turn (the recovery loop itself) and kimisoul.py:1815-1882.
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

namespace kimix::agent {

// Substrings (matched case-insensitively) that identify a provider-confirmed
// context-window-exceeded error message
// (context_overflow.py:18-24 CONTEXT_OVERFLOW_MARKERS).
inline constexpr kimix::string_view k_context_overflow_markers[] = {
    "context length",
    "context_length",
    "max tokens",
    "maximum context",
    "too many tokens",
};

// Return true for a 4xx API error whose message matches an overflow marker.
//
// Mirrors the reference's is_context_overflow_error + classify_api_error
// precedence (context_overflow.py:29-55, kimisoul.py:290-312): the status must
// be in [400, 500); statuses that classify_api_error resolves BEFORE the 4xx
// branch - 401/403 (auth) and 429 (rate limit) - are rejected so a rate-limit
// or auth error is never an overflow, even when its body mentions tokens. 5xx,
// network errors and timeouts are rejected as well. `status` <= 0 means "no
// HTTP status" (connection failure) and is rejected.
bool is_context_overflow_error(kimix::string_view message,
                               int32_t status) noexcept;

// Per-step overflow retry budget (context_overflow.py:58-83
// OverflowRecoveryState). The soul creates one instance at the top of each
// top-level step and shares it across the re-entrant step attempts made by
// the overflow recovery loop, so the budget is consumed across re-entries but
// naturally resets when the step completes.
class OverflowRecoveryState {
public:
    // Negative/zero max_retries -> never retry (the reference clamps via
    // max(0, max_retries)).
    explicit OverflowRecoveryState(int32_t max_retries) noexcept
        : _max_retries(max_retries > 0 ? max_retries : 0),
          _remaining(_max_retries) {}

    // True while at least one overflow retry is still available.
    bool can_retry() const noexcept { return _remaining > 0; }

    // Record that one overflow retry was used.
    void consumed() noexcept {
        if (_remaining > 0) {
            --_remaining;
        }
    }

    // Restore the full retry budget (the reference resets on step progress).
    void reset() noexcept { _remaining = _max_retries; }

    int32_t remaining() const noexcept { return _remaining; }
    int32_t max_retries() const noexcept { return _max_retries; }

private:
    int32_t _max_retries;
    int32_t _remaining;
};

} // namespace kimix::agent
