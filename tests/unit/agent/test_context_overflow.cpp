// test_context_overflow.cpp - Unit tests for kimix::agent overflow detection
// (src/agent/context_overflow.*), the port of kimi_cli/soul/context_overflow.py:
// the five CONTEXT_OVERFLOW_MARKERS, the 400 <= status < 500 band rule, the
// 401/403/429 precedence, and the OverflowRecoveryState budget.
//
// Framework: Boost.UT (tests/ut/ut.hpp). CPU-only, no network.

#include "ut/ut.hpp"

#include <agent/context_overflow.h>

#include <cstdint>

using namespace boost::ut;
using namespace boost::ut::literals;

int main() {
    using kimix::agent::is_context_overflow_error;
    using kimix::agent::OverflowRecoveryState;

    "each_marker_matches_in_the_4xx_band"_test = [] {
        expect(is_context_overflow_error(
            "This model's maximum context length is 128000 tokens", 400));
        expect(is_context_overflow_error("exceeded context_length limit", 400));
        expect(is_context_overflow_error("request exceeds max tokens", 400));
        expect(is_context_overflow_error("maximum context size exceeded", 400));
        expect(is_context_overflow_error("too many tokens in the request", 400));
    };

    "matching_is_case_insensitive"_test = [] {
        expect(is_context_overflow_error(
            "MAX TOKENS exceeded for this model", 400));
        expect(is_context_overflow_error(
            "Maximum Context Length Exceeded", 400));
    };

    "status_must_be_in_the_4xx_band"_test = [] {
        // 5xx never qualifies, even with a marker in the body.
        expect(!is_context_overflow_error(
            "maximum context length error, try later", 500));
        expect(!is_context_overflow_error(
            "maximum context length error, try later", 503));
        // No HTTP status at all (connection failure) is rejected.
        expect(!is_context_overflow_error("maximum context length", 0));
        expect(!is_context_overflow_error("maximum context length", -1));
        // 3xx / 2xx are not API errors.
        expect(!is_context_overflow_error("maximum context length", 302));
        expect(!is_context_overflow_error("maximum context length", 200));
    };

    "auth_and_rate_limit_statuses_win_over_markers"_test = [] {
        // classify_api_error precedence: 401/403 -> auth, 429 -> rate_limit;
        // a 429 whose body mentions tokens is NEVER an overflow.
        expect(!is_context_overflow_error(
            "rate limited: too many tokens per minute", 429));
        expect(!is_context_overflow_error("unauthorized: max tokens", 401));
        expect(!is_context_overflow_error("forbidden: max tokens", 403));
    };

    "plain_4xx_without_marker_is_not_overflow"_test = [] {
        expect(!is_context_overflow_error(
            "invalid request: malformed tool arguments", 400));
        expect(!is_context_overflow_error("bad request", 422));
    };

    "overflow_recovery_state_clamps_and_consumes"_test = [] {
        // Negative/zero max_retries -> never retry (max(0, max_retries)).
        OverflowRecoveryState disabled(-1);
        expect(!disabled.can_retry());
        disabled.consumed(); // must not underflow
        expect(!disabled.can_retry());

        OverflowRecoveryState state(2);
        expect(state.can_retry());
        expect(state.remaining() == 2_i);
        state.consumed();
        expect(state.can_retry());
        expect(state.remaining() == 1_i);
        state.consumed();
        expect(!state.can_retry());
        expect(state.remaining() == 0_i);
        state.consumed(); // clamped at zero
        expect(state.remaining() == 0_i);

        // reset() restores the full budget (step progress).
        state.reset();
        expect(state.can_retry());
        expect(state.remaining() == 2_i);
    };

    return 0;
}
