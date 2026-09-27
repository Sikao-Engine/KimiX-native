// agent/step_retry.h - Per-step retry policy for the agent loop.
//
// Port of the reference's step-retry machinery (kimisoul.py:1749-1789,
// 2386-2398; kosong error taxonomy):
//   * tenacity stop_after_attempt(LoopControl.max_retries_per_step) with
//     retryable = connection error / timeout / empty(think-only) response +
//     HTTP 429 / 500 / 502 / 503 / 504 (kimisoul.py _is_retryable_error);
//   * _RateLimitAwareWait (kimisoul.py:247-287): exponential backoff + jitter
//     (0.3s initial, 5s cap), 429 -> 1s initial / 30s cap, honouring the
//     Retry-After response header capped at 60s. The raw wait formula is
//     shared with the provider transport loops (llm/common.h) so the fixed
//     "3 x 300ms" sleeps defer to the same policy.
//
// The classification consumes the structured error fields the providers now
// set on kimix::llm::ChatResult (error_kind / error_status /
// retry_after_seconds) instead of a bare string.
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "agent/loop_control.h"
#include "llm/llm.h"

namespace kimix::agent {

// kimisoul.py classify_api_error's error_type values, ported to an enum.
enum class StepErrorCategory : uint8_t {
    none = 0,        // no error (successful result)
    network,         // APIConnectionError (no HTTP status)
    timeout,         // APITimeoutError / TimeoutError (no HTTP status)
    empty_response,  // APIEmptyResponseError: empty or think-only response
    rate_limit,      // HTTP 429
    auth,            // HTTP 401/403
    server_error,    // HTTP 5xx
    client_error,    // other HTTP 4xx (incl. context_overflow, which the
                     // recovery loop inspects before this classification
                     // matters for retryability)
    not_supported,   // capability pre-flight refusal (LLMNotSupported)
    other,           // anything else
};

// Structured classification of one failed chat attempt.
struct StepError {
    StepErrorCategory category = StepErrorCategory::other;
    int32_t status = 0;          // HTTP status when there is one, else 0
    double retry_after = 0.0;    // Retry-After hint (seconds) when present
    kimix::string type_name;     // reference's type(e).__name__ for messages
    kimix::string message;       // the provider's error text
};

// Classify a failed ChatResult into the retry-policy taxonomy (port of
// classify_api_error, kimisoul.py:290-312).
StepError classify_step_error(const kimix::llm::ChatResult &result);

// kimisoul.py _is_retryable_error: connection/timeout errors, empty
// (think-only) responses, and APIStatusError 429/500/502/503/504. Auth
// failures, other 4xx and capability refusals are NOT retryable.
bool is_retryable_step_error(const StepError &error) noexcept;

// The _RateLimitAwareWait port (kimisoul.py:247-287) as a policy object:
// tenacity's stop_after_attempt + wait + before_sleep folded into one
// injectable, synchronous loop helper.
//
// attempt numbering matches tenacity: `attempt` passed to wait_seconds() is
// the 1-based number of the attempt that just FAILED, so the first retry of
// the first attempt computes 0.3 * 2^0 + jitter.
class StepRetryPolicy {
public:
    struct params {
        int32_t max_attempts = 5; // stop_after_attempt(LoopControl.max_retries_per_step)
        // Sleep injection: tests record the requested delays and skip real
        // waiting. Defaults to a real sleep.
        kimix::function<void(double seconds)> sleep;
        // Jitter RNG seed for deterministic waits in tests
        // (random.uniform in the reference).
        uint64_t jitter_seed = 0x9E3779B97F4A7C15ull;
    };

    // Default-constructible: a delegating default constructor instead of a
    // `params p = {}` default argument - GCC rejects the brace-init default
    // argument for the aggregate (MSVC accepts it; keep the form portable).
    StepRetryPolicy() : StepRetryPolicy(params{}) {}
    explicit StepRetryPolicy(const params &p);

    // stop_after_attempt(n): total tries allowed for one step.
    int32_t max_attempts() const noexcept { return _max_attempts; }

    // True while another attempt may be made after `failed_attempts` tries.
    bool can_retry(int32_t failed_attempts) const noexcept {
        return failed_attempts < _max_attempts;
    }

    // The _RateLimitAwareWait wait for the attempt that just failed (no
    // sleeping): 429 -> Retry-After (capped at 60s) or 1*2^(n-1)+jitter
    // capped at 30s; anything else -> 0.3*2^(n-1)+jitter capped at 5s.
    double wait_seconds(int32_t failed_attempt, const StepError &error) const;

    // before_sleep: compute the wait and sleep through the injected sleeper.
    void before_retry(int32_t failed_attempt, const StepError &error) const;

private:
    int32_t _max_attempts;
    kimix::function<void(double seconds)> _sleep;
    mutable uint64_t _jitter_rng; // xorshift state (mutable: waits are const)
};

} // namespace kimix::agent
