// test_turn_resilience.cpp - Turn-level tests for the Phase-1 loop-resilience
// features of KimiSoul (src/agent/soul.*), all driven by scripted fake chat
// backends (no network):
//   * A5 per-step retry: 429/5xx failures retry with the _RateLimitAwareWait
//     schedule; a retried step counts once against max_steps.
//   * B2 context overflow: a provider-confirmed overflow force-compacts
//     (AGGRESSIVE, preserve_depth_override=1, trigger "overflow") and re-enters
//     the step; a 429 mentioning tokens is a rate limit, not an overflow.
//   * A8 reasoning-only: empty/think-only responses escalate the output budget
//     x1.5 from 8192 and then stop with the reference's user-visible text.
//   * A6 session restart: exhausted retries auto-restart bounded by
//     max_session_restarts, emitting the reference's user-visible notices.
//   * A11 typed step cap: MaxStepsReached carries the count and wording.
//   * B1 token ledger: provider usage anchors recorded tokens after a step.
//
// Framework: Boost.UT (tests/ut/ut.hpp).

#include "ut/ut.hpp"

#include <agent/soul.h>

#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

// Scripted chat backend: pops one canned result per chat() call (the last
// entry repeats). Records call count and the output-budget escalation.
class ScriptedBackend : public kimix::agent::IChatBackend {
public:
    kimix::vector<kimix::llm::ChatResult> scripted;
    size_t index = 0;
    int calls = 0;
    int64_t budget = 0;
    int64_t max_context = 128000;
    bool fail_default = false; // after the script: keep failing with 503s

    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &tools,
         const kimix::llm::ChunkCallback &on_chunk) override {
        (void)messages;
        (void)tools;
        (void)on_chunk;
        ++calls;
        if (index < scripted.size()) {
            return scripted[index++];
        }
        if (fail_default) {
            kimix::llm::ChatResult r;
            r.ok = false;
            r.error_kind = kimix::llm::ChatErrorKind::http;
            r.error_status = 503;
            r.error = "unavailable";
            return r;
        }
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content = "done";
        return r;
    }
    int64_t max_context_size() const override { return max_context; }
    kimix::string model_name() const override { return "scripted"; }
    void set_output_token_budget(int64_t tokens) override { budget = tokens; }
    int64_t output_token_budget() const override { return budget; }
};

kimix::llm::ChatResult ok_text(kimix::string text, int64_t prompt_tokens = 0) {
    kimix::llm::ChatResult r;
    r.ok = true;
    r.content = std::move(text);
    r.prompt_tokens = prompt_tokens;
    r.total_tokens = prompt_tokens;
    return r;
}

kimix::llm::ChatResult http_error(int32_t status, kimix::string body,
                                  double retry_after = 0.0) {
    kimix::llm::ChatResult r;
    r.ok = false;
    r.error_kind = kimix::llm::ChatErrorKind::http;
    r.error_status = status;
    r.retry_after_seconds = retry_after;
    r.error = std::move(body);
    return r;
}

kimix::llm::ChatResult empty_response_error() {
    kimix::llm::ChatResult r;
    r.ok = false;
    r.error_kind = kimix::llm::ChatErrorKind::empty_response;
    r.error = "backend returned an unusable response body";
    return r;
}

// Options with fast, observable retries: no real sleeping, tiny jitter-free
// bookkeeping left to the injected sleeper.
kimix::agent::KimiSoul::options resilient_options(
    kimix::vector<double> *slept = nullptr) {
    kimix::agent::KimiSoul::options opts;
    opts.system_prompt = "test";
    opts.auto_compact = false; // compaction is exercised explicitly
    if (slept != nullptr) {
        opts.step_retry_sleep = [slept](double seconds) {
            slept->push_back(seconds);
        };
    }
    return opts;
}

} // namespace

int main() {
    using kimix::agent::KimiSoul;
    using kimix::agent::TurnErrorKind;

    "retried_step_counts_once_and_succeeds"_test = [] {
        ScriptedBackend backend;
        backend.scripted.push_back(
            http_error(429, "rate limited", /*retry_after=*/2.0));
        backend.scripted.push_back(ok_text("hello"));
        kimix::vector<double> slept;
        kimix::agent::AgentSession session;
        KimiSoul soul(session, backend, resilient_options(&slept));
        const kimix::agent::TurnResult out = soul.turn("hi");
        expect(out.ok);
        expect(out.content == "hello");
        expect(backend.calls == 2);
        expect(out.steps == 1_i); // the retried step counts once
        expect(slept.size() == 1u); // one retry -> one wait
        expect(slept[0] == 2.0_d);  // Retry-After honoured verbatim
    };

    "non_retryable_4xx_fails_without_retrying"_test = [] {
        ScriptedBackend backend;
        backend.scripted.push_back(
            http_error(400, "invalid request: bad tool schema"));
        kimix::agent::AgentSession session;
        KimiSoul soul(session, backend, resilient_options());
        const kimix::agent::TurnResult out = soul.turn("hi");
        expect(!out.ok);
        expect(backend.calls == 1); // deterministic 4xx fails fast
        expect(out.error_kind == TurnErrorKind::chat_failed);
        expect(out.error.find("chat failed: ") == 0u);
    };

    "exhausted_retries_auto_restart_bounded"_test = [] {
        ScriptedBackend backend;
        for (int i = 0; i < 8; ++i) {
            backend.scripted.push_back(http_error(503, "unavailable"));
        }
        kimix::vector<double> slept;
        kimix::agent::AgentSession session;
        KimiSoul::options opts = resilient_options(&slept);
        opts.loop_control.max_retries_per_step = 2;
        opts.loop_control.max_session_restarts = 1;
        KimiSoul soul(session, backend, std::move(opts));
        kimix::string user_text;
        const kimix::agent::TurnResult out =
            soul.turn("hi", [&user_text](const kimix::llm::Chunk &chunk) {
                user_text += chunk.content;
            });
        expect(!out.ok);
        expect(out.error_kind == TurnErrorKind::session_restart_exhausted);
        expect(out.session_restarts == 1_i);
        // Original attempt (2 tries) + one restart (2 tries).
        expect(backend.calls == 4);
        // The reference's user-visible restart notice went out on the wire.
        expect(user_text.find("Restarting session (1/1)") !=
               kimix::string::npos);
    };

    "restart_budget_zero_fails_immediately"_test = [] {
        ScriptedBackend backend;
        backend.scripted.push_back(http_error(500, "boom"));
        kimix::agent::AgentSession session;
        KimiSoul::options opts = resilient_options();
        opts.loop_control.max_retries_per_step = 1;
        opts.loop_control.max_session_restarts = 0; // auto-restart disabled
        KimiSoul soul(session, backend, std::move(opts));
        const kimix::agent::TurnResult out = soul.turn("hi");
        expect(!out.ok);
        expect(out.error_kind == TurnErrorKind::session_restart_exhausted);
        expect(out.session_restarts == 0_i);
        expect(backend.calls == 1);
    };

    "think_only_response_escalates_budget_then_stops"_test = [] {
        ScriptedBackend backend;
        for (int i = 0; i < 6; ++i) {
            backend.scripted.push_back(empty_response_error());
        }
        kimix::agent::AgentSession session;
        KimiSoul::options opts = resilient_options();
        opts.loop_control.max_retries_per_step = 3;
        opts.loop_control.max_session_restarts = 0;
        KimiSoul soul(session, backend, std::move(opts));
        kimix::string user_text;
        const kimix::agent::TurnResult out =
            soul.turn("hi", [&user_text](const kimix::llm::Chunk &chunk) {
                user_text += chunk.content;
            });
        expect(!out.ok);
        expect(out.error_kind == TurnErrorKind::empty_response_exhausted);
        // 8192 -> 12288 -> 18432 (x1.5 per retry, from the 8192 base).
        expect(backend.budget == 18432_i);
        expect(backend.calls == 3_i);
        // The reference's user-visible stop explanation (verbatim wording).
        expect(user_text.find(
                   "(The model produced only thinking content without a "
                   "response. Stopping this turn.)") != kimix::string::npos);
        expect(out.error.find("produced only thinking content") !=
               kimix::string::npos);
    };

    "context_overflow_force_compacts_and_reenters"_test = [] {
        ScriptedBackend backend;
        // Turn 1 ("seed") succeeds. Turn 2: overflow error -> forced AGGRESSIVE
        // compaction (a chat call producing the summary) -> re-entered step.
        backend.scripted.push_back(ok_text("seeded"));
        backend.scripted.push_back(http_error(
            400, "This model's maximum context length is 128000 tokens"));
        backend.scripted.push_back(ok_text("ok summary"));
        backend.scripted.push_back(ok_text("recovered"));
        kimix::agent::AgentSession session;
        // Long enough history that the forced compaction has a region to cut
        // (the compacted head must exceed the short summary for the shrink
        // check to accept it).
        for (int i = 0; i < 2; ++i) {
            kimix::llm::Message u;
            u.role = "user";
            u.content = "earlier user message with some content to compact "
                        "away completely 0123456789";
            session.append_history(u);
            kimix::llm::Message a;
            a.role = "assistant";
            a.content = "earlier assistant reply with some content to compact "
                        "away completely 0123456789";
            session.append_history(a);
        }
        KimiSoul::options opts = resilient_options();
        opts.loop_control.max_session_restarts = 0;
        KimiSoul soul(session, backend, std::move(opts));
        const kimix::agent::TurnResult first = soul.turn("seed");
        expect(first.ok);
        const kimix::agent::TurnResult second = soul.turn("go");
        expect(second.ok);
        expect(second.content == "recovered");
        expect(second.compacted);
        expect(soul.compaction_count() == 1_i);
        // seed + overflow + summary + recovery = 4 calls.
        expect(backend.calls == 4);
        // The compaction must have shrunk the history (summary + preserved
        // tail) instead of growing it unboundedly.
        expect(session.history().size() < 6u);
    };

    "failed_overflow_compaction_restarts_the_session"_test = [] {
        ScriptedBackend backend;
        // History too short to compact (nothing to cut) so the forced
        // recovery compaction fails: the reference restarts the session for
        // this case even though a bare 400 is not restartable.
        backend.scripted.push_back(http_error(
            400, "This model's maximum context length is 128000 tokens"));
        backend.scripted.push_back(http_error(
            400, "This model's maximum context length is 128000 tokens"));
        backend.fail_default = true; // later rounds keep failing too
        kimix::vector<double> slept;
        kimix::agent::AgentSession session;
        KimiSoul::options opts = resilient_options(&slept);
        opts.loop_control.max_retries_per_step = 1;
        opts.loop_control.max_session_restarts = 2;
        KimiSoul soul(session, backend, std::move(opts));
        kimix::string user_text;
        const kimix::agent::TurnResult out =
            soul.turn("hi", [&user_text](const kimix::llm::Chunk &chunk) {
                user_text += chunk.content;
            });
        expect(!out.ok);
        // Every attempt overflowed and the recovery compaction could not cut
        // anything, so all restarts went through the restart machinery.
        expect(out.session_restarts == 2_i);
        expect(out.error_kind == TurnErrorKind::session_restart_exhausted);
        // No retry of the non-retryable 400, and the recovery compaction
        // bails out before its summary call ("nothing to compact"): exactly
        // one chat call per round (initial + 2 restarts).
        expect(backend.calls == 3_i);
        expect(user_text.find("Restarting session (2/2)") !=
               kimix::string::npos);
    };

    "rate_limit_mentioning_tokens_is_not_overflow"_test = [] {
        ScriptedBackend backend;
        // A 429 whose body mentions tokens is a rate limit (retryable), never
        // an overflow: no compaction happens.
        backend.scripted.push_back(http_error(
            429, "rate limited: too many tokens per minute", 1.0));
        backend.scripted.push_back(ok_text("hello"));
        kimix::vector<double> slept;
        kimix::agent::AgentSession session;
        KimiSoul soul(session, backend, resilient_options(&slept));
        const kimix::agent::TurnResult out = soul.turn("hi");
        expect(out.ok);
        expect(!out.compacted);
        expect(soul.compaction_count() == 0_i);
        expect(backend.calls == 2);
    };

    "provider_usage_anchors_the_token_ledger"_test = [] {
        ScriptedBackend backend;
        backend.scripted.push_back(ok_text("one", /*prompt_tokens=*/100));
        backend.scripted.push_back(ok_text("two", /*prompt_tokens=*/500));
        kimix::agent::AgentSession session;
        KimiSoul soul(session, backend, resilient_options());
        const kimix::agent::TurnResult first = soul.turn("first");
        expect(first.ok);
        expect(soul.token_ledger().has_recorded_usage());
        expect(soul.token_ledger().token_count() == 100_i);
        // The second turn appends a user message: recorded usage stays put and
        // the pending estimate grows on top of it.
        const kimix::agent::TurnResult second = soul.turn("second");
        expect(second.ok);
        expect(soul.token_ledger().token_count() == 500_i);
        expect(soul.token_ledger().token_count_with_pending() >= 500_i);
    };

    "max_steps_failure_is_typed_and_carries_the_count"_test = [] {
        ScriptedBackend backend;
        // Every step returns a tool call so the loop never ends on its own.
        kimix::llm::ChatResult keep_going = ok_text("");
        kimix::llm::ToolCall call;
        call.name = "no_such_tool";
        call.arguments = "{}";
        keep_going.tool_calls.push_back(call);
        for (int i = 0; i < 10; ++i) {
            backend.scripted.push_back(keep_going);
        }
        kimix::agent::AgentSession session;
        KimiSoul::options opts = resilient_options();
        opts.max_steps = 3; // legacy field reconciled into loop_control
        KimiSoul soul(session, backend, std::move(opts));
        const kimix::agent::TurnResult out = soul.turn("hi");
        expect(!out.ok);
        expect(out.error_kind == TurnErrorKind::max_steps_reached);
        expect(out.max_steps == 3_i);
        expect(out.steps == 3_i);
        // The reference's MaxStepsReached wording, not "max steps reached".
        expect(out.error == "Max number of steps reached: 3");
    };

    return 0;
}
