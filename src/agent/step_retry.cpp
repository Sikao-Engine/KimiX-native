// agent/step_retry.cpp - Step retry policy implementation (see step_retry.h).

#include "agent/step_retry.h"

#include <chrono>
#include <thread>

#include "llm/common.h"

namespace kimix::agent {

StepError classify_step_error(const kimix::llm::ChatResult &result) {
    StepError e;
    e.status = result.error_status;
    e.retry_after = result.retry_after_seconds;
    e.message = result.error;
    switch (result.error_kind) {
    case kimix::llm::ChatErrorKind::none:
        e.category = StepErrorCategory::none;
        e.type_name = "none";
        return e;
    case kimix::llm::ChatErrorKind::connection:
        e.category = StepErrorCategory::network;
        e.type_name = "APIConnectionError";
        return e;
    case kimix::llm::ChatErrorKind::timeout:
        e.category = StepErrorCategory::timeout;
        e.type_name = "APITimeoutError";
        return e;
    case kimix::llm::ChatErrorKind::empty_response:
        e.category = StepErrorCategory::empty_response;
        e.type_name = "APIEmptyResponseError";
        return e;
    case kimix::llm::ChatErrorKind::not_supported:
        e.category = StepErrorCategory::not_supported;
        e.type_name = "LLMNotSupported";
        return e;
    case kimix::llm::ChatErrorKind::aborted:
        // G8: caller-cancelled mid-stream; the turn loop surfaces this as a
        // cancelled turn long before the retry policy sees it, and it must
        // never be retried or session-restarted.
        e.category = StepErrorCategory::other;
        e.type_name = "RequestAborted";
        return e;
    case kimix::llm::ChatErrorKind::http:
        break; // resolved from the status below
    }
    // classify_api_error's status ladder (kimisoul.py:290-312).
    const int32_t s = result.error_status;
    e.retry_after = result.retry_after_seconds;
    if (s == 429) {
        e.category = StepErrorCategory::rate_limit;
        e.type_name = "APIStatusError";
    } else if (s == 401 || s == 403) {
        e.category = StepErrorCategory::auth;
        e.type_name = "APIStatusError";
    } else if (s >= 500) {
        e.category = StepErrorCategory::server_error;
        e.type_name = "APIStatusError";
    } else if (s >= 400) {
        e.category = StepErrorCategory::client_error;
        e.type_name = "APIStatusError";
    } else {
        e.category = StepErrorCategory::other;
        e.type_name = "ChatProviderError";
    }
    return e;
}

bool is_retryable_step_error(const StepError &error) noexcept {
    // kimisoul.py _is_retryable_error (kimisoul.py:2387-2398): connection /
    // timeout / empty responses plus APIStatusError 429/500/502/503/504.
    switch (error.category) {
    case StepErrorCategory::network:
    case StepErrorCategory::timeout:
    case StepErrorCategory::empty_response:
        return true;
    case StepErrorCategory::rate_limit:
        return true; // 429
    case StepErrorCategory::server_error:
        return error.status == 500 || error.status == 502 ||
               error.status == 503 || error.status == 504;
    default:
        return false;
    }
}

  StepRetryPolicy::StepRetryPolicy(const params &p)
    : _max_attempts(p.max_attempts > 0 ? p.max_attempts : 1),
      _sleep(std::move(p.sleep)), _jitter_rng(p.jitter_seed) {
    if (!_sleep) {
        _sleep = [](double seconds) {
            if (seconds > 0.0) {
                std::this_thread::sleep_for(
                    std::chrono::duration<double>(seconds));
            }
        };
    }
}

double StepRetryPolicy::wait_seconds(int32_t failed_attempt,
                                     const StepError &error) const {
    // The shared _RateLimitAwareWait port lives next to the transport loops so
    // the providers sleep with the same policy (llm/common.h).
    return kimix::llm::rate_limit_aware_wait(
        failed_attempt, error.status, error.retry_after, _jitter_rng);
}

void StepRetryPolicy::before_retry(int32_t failed_attempt,
                                   const StepError &error) const {
    _sleep(wait_seconds(failed_attempt, error));
}

} // namespace kimix::agent
