// test_step_retry.cpp - Unit tests for kimix::agent::StepRetryPolicy
// (src/agent/step_retry.*): the classify_api_error port over the structured
// ChatResult error fields, kimisoul.py _is_retryable_error's list
// (connection/timeout/empty + 429/500/502/503/504), the
// _RateLimitAwareWait schedule (exponential + jitter caps, 429 Retry-After
// honoured up to 60s), and stop_after_attempt(max_retries_per_step).
//
// Framework: Boost.UT (tests/ut/ut.hpp). CPU-only, no network.

#include "ut/ut.hpp"

#include <agent/step_retry.h>

#include <llm/common.h>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

kimix::llm::ChatResult failed(kimix::llm::ChatErrorKind kind, int32_t status,
                              kimix::string message = "boom",
                              double retry_after = 0.0) {
    kimix::llm::ChatResult r;
    r.ok = false;
    r.error_kind = kind;
    r.error_status = status;
    r.error = std::move(message);
    r.retry_after_seconds = retry_after;
    return r;
}

} // namespace

int main() {
    using kimix::agent::classify_step_error;
    using kimix::agent::is_retryable_step_error;
    using kimix::agent::StepErrorCategory;
    using kimix::agent::StepRetryPolicy;

    "classify_step_error_kinds"_test = [] {
        expect(classify_step_error(failed(kimix::llm::ChatErrorKind::connection,
                                          0))
                   .category == StepErrorCategory::network);
        expect(classify_step_error(failed(kimix::llm::ChatErrorKind::timeout, 0))
                   .category == StepErrorCategory::timeout);
        expect(classify_step_error(
                   failed(kimix::llm::ChatErrorKind::empty_response, 0))
                   .category == StepErrorCategory::empty_response);
        expect(classify_step_error(
                   failed(kimix::llm::ChatErrorKind::not_supported, 0))
                   .category == StepErrorCategory::not_supported);
    };

    "classify_step_error_status_ladder"_test = [] {
        // kimisoul.py classify_api_error: 429 -> rate_limit, 401/403 -> auth,
        // >= 500 -> 5xx_server, other 4xx -> 4xx_client.
        expect(classify_step_error(failed(kimix::llm::ChatErrorKind::http, 429))
                   .category == StepErrorCategory::rate_limit);
        expect(classify_step_error(failed(kimix::llm::ChatErrorKind::http, 401))
                   .category == StepErrorCategory::auth);
        expect(classify_step_error(failed(kimix::llm::ChatErrorKind::http, 403))
                   .category == StepErrorCategory::auth);
        expect(classify_step_error(failed(kimix::llm::ChatErrorKind::http, 500))
                   .category == StepErrorCategory::server_error);
        expect(classify_step_error(failed(kimix::llm::ChatErrorKind::http, 400))
                   .category == StepErrorCategory::client_error);
        expect(classify_step_error(failed(kimix::llm::ChatErrorKind::http, 0))
                   .category == StepErrorCategory::other);
    };

    "retryable_list_matches_the_reference"_test = [] {
        // kimisoul.py _is_retryable_error: connection/timeout/empty + 429 /
        // 500 / 502 / 503 / 504. Everything else fails fast.
        auto retryable = [](kimix::llm::ChatErrorKind kind, int32_t status) {
            return is_retryable_step_error(classify_step_error(
                failed(kind, status, "err", 0.0)));
        };
        expect(retryable(kimix::llm::ChatErrorKind::connection, 0));
        expect(retryable(kimix::llm::ChatErrorKind::timeout, 0));
        expect(retryable(kimix::llm::ChatErrorKind::empty_response, 0));
        expect(retryable(kimix::llm::ChatErrorKind::http, 429));
        expect(retryable(kimix::llm::ChatErrorKind::http, 500));
        expect(retryable(kimix::llm::ChatErrorKind::http, 502));
        expect(retryable(kimix::llm::ChatErrorKind::http, 503));
        expect(retryable(kimix::llm::ChatErrorKind::http, 504));
        // Not retryable: 501 is not in the reference list, 400/408/401 are
        // not either, and capability refusals never retry.
        expect(!retryable(kimix::llm::ChatErrorKind::http, 501));
        expect(!retryable(kimix::llm::ChatErrorKind::http, 400));
        expect(!retryable(kimix::llm::ChatErrorKind::http, 408));
        expect(!retryable(kimix::llm::ChatErrorKind::http, 401));
        expect(!retryable(kimix::llm::ChatErrorKind::not_supported, 0));
    };

    "stop_after_attempt_bounds_the_policy"_test = [] {
        StepRetryPolicy policy(StepRetryPolicy::params{.max_attempts = 5});
        expect(policy.max_attempts() == 5_i);
        expect(policy.can_retry(1));
        expect(policy.can_retry(4));
        expect(!policy.can_retry(5)); // stop_after_attempt(5)
        // max_attempts is clamped to >= 1.
        StepRetryPolicy degenerate(StepRetryPolicy::params{.max_attempts = 0});
        expect(degenerate.max_attempts() == 1_i);
    };

    "rate_limit_wait_honours_retry_after_capped_at_60"_test = [] {
        StepRetryPolicy policy(StepRetryPolicy::params{.max_attempts = 5});
        const kimix::agent::StepError limited{
            .category = StepErrorCategory::rate_limit,
            .status = 429,
            .retry_after = 10.0,
        };
        expect(policy.wait_seconds(1, limited) == 10.0_d);
        const kimix::agent::StepError huge{
            .category = StepErrorCategory::rate_limit,
            .status = 429,
            .retry_after = 120.0,
        };
        expect(policy.wait_seconds(1, huge) == 60.0_d); // max_retry_after cap
    };

    "rate_limit_wait_exponential_backoff_caps_at_30s"_test = [] {
        StepRetryPolicy policy(StepRetryPolicy::params{.max_attempts = 10});
        const kimix::agent::StepError limited{
            .category = StepErrorCategory::rate_limit, .status = 429};
        const double first = policy.wait_seconds(1, limited);
        expect(first >= 1.0_d && first < 2.0_d); // 1*2^0 + jitter(0..1)
        const double capped = policy.wait_seconds(10, limited);
        expect(capped <= 30.0_d);
        // Attempt 2 doubles: [2, 3).
        const double second = policy.wait_seconds(2, limited);
        expect(second >= 2.0_d && second < 3.0_d);
    };

    "default_wait_exponential_backoff_caps_at_5s"_test = [] {
        StepRetryPolicy policy(StepRetryPolicy::params{.max_attempts = 10});
        const kimix::agent::StepError server{
            .category = StepErrorCategory::server_error, .status = 500};
        const double first = policy.wait_seconds(1, server);
        expect(first >= 0.3_d && first < 0.8_d); // 0.3*2^0 + jitter(0..0.5)
        const double second = policy.wait_seconds(2, server);
        expect(second >= 0.6_d && second < 1.1_d);
        const double capped = policy.wait_seconds(10, server);
        expect(capped <= 5.0_d);
    };

    "wait_is_deterministic_with_a_fixed_seed"_test = [] {
        const StepRetryPolicy::params base{
            .max_attempts = 10, .jitter_seed = 42};
        const StepRetryPolicy a(base);
        const StepRetryPolicy b(base);
        const kimix::agent::StepError err{
            .category = StepErrorCategory::server_error, .status = 500};
        for (int32_t attempt = 1; attempt <= 6; ++attempt) {
            expect(a.wait_seconds(attempt, err) == b.wait_seconds(attempt, err));
        }
    };

    "before_retry_sleeps_through_the_injected_sleeper"_test = [] {
        kimix::vector<double> slept;
        StepRetryPolicy::params p;
        p.max_attempts = 5;
        p.sleep = [&slept](double seconds) { slept.push_back(seconds); };
        const StepRetryPolicy policy(std::move(p));
        const kimix::agent::StepError limited{
            .category = StepErrorCategory::rate_limit,
            .status = 429,
            .retry_after = 3.0,
        };
        policy.before_retry(1, limited);
        expect(slept.size() == 1u);
        expect(slept[0] == 3.0_d);
    };

    "parse_retry_after_seconds"_test = [] {
        using kimix::llm::parse_retry_after_seconds;
        expect(parse_retry_after_seconds("12") == 12.0_d);
        expect(parse_retry_after_seconds("0") == 0.0_d);
        expect(parse_retry_after_seconds("") == 0.0_d);
        expect(parse_retry_after_seconds("soon") == 0.0_d);
        // Trailing whitespace is tolerated; garbage is not.
        expect(parse_retry_after_seconds("7 ") == 7.0_d);
        expect(parse_retry_after_seconds("7s") == 0.0_d);
    };

    return 0;
}
