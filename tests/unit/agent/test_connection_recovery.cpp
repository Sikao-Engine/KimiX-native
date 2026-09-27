// test_connection_recovery.cpp - A7: the connection/auth recovery seam
// (kimisoul.py _run_with_connection_recovery:2442-2523) onto the native loop:
//   * a retryable step error (connection / 429 / 5xx) that outlives the retry
//     budget invokes IChatBackend::on_retryable_error ONCE and retries the
//     step once more OUTSIDE the retry budget (the recovered attempt may
//     succeed);
//   * a 401 invokes IChatBackend::refresh_auth() ONCE and, when it returns
//     true, retries the step once more outside the budget; a false keeps the
//     failure standing; LLMBackend wires it to the Config-level callback;
//   * each recovery kind fires at most once per step; without recovery hooks
//     a persistent failure still fails the turn with the original error.
//
// Framework: Boost.UT (tests/ut/ut.hpp). CPU-only: a scripted backend.

#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include "agent/soul.h"

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::agent;

namespace {

// Scripted backend with recovery hooks.
class RecoveryBackend : public IChatBackend {
public:
    kimix::vector<kimix::llm::ChatResult> scripted;
    size_t index = 0;
    int on_retryable_calls = 0;
    int refresh_auth_calls = 0;
    bool refresh_result = false;

    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &,
         const kimix::vector<kimix::llm::Tool> &,
         const kimix::llm::ChunkCallback &) override {
        if (index < scripted.size()) {
            return scripted[index++];
        }
        kimix::llm::ChatResult ok;
        ok.ok = true;
        ok.content = "done";
        return ok;
    }
    void on_retryable_error(const kimix::llm::ChatResult &) override {
        ++on_retryable_calls;
    }
    // The test backend opts in to the recovered attempt (the reference's
    // RetryableChatProvider marker).
    bool supports_retryable_recovery() const noexcept override { return true; }
    bool refresh_auth() override {
        ++refresh_auth_calls;
        return refresh_result;
    }
    int64_t max_context_size() const override { return 128000; }
    kimix::string model_name() const override { return "scripted"; }

    static kimix::llm::ChatResult ok_text(kimix::string_view text) {
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content.assign(text.data(), text.size());
        return r;
    }
    static kimix::llm::ChatResult http_error(int status) {
        kimix::llm::ChatResult r;
        r.ok = false;
        r.error = "http " + std::to_string(status);
        r.error_kind = kimix::llm::ChatErrorKind::http;
        r.error_status = status;
        return r;
    }
    static kimix::llm::ChatResult connection_error() {
        kimix::llm::ChatResult r;
        r.ok = false;
        r.error = "connection reset";
        r.error_kind = kimix::llm::ChatErrorKind::connection;
        return r;
    }
};

KimiSoul make_soul(AgentSession &session, RecoveryBackend &backend) {
    KimiSoul::options opts;
    opts.loop_control.max_retries_per_step = 1; // one attempt, no budget retry
    opts.loop_control.verification_gate_enabled = false;
    opts.auto_compact = false;
    opts.system_prompt = "test";
    return KimiSoul(session, backend, opts);
}

} // namespace

int main() {
    "retryable_error_outliving_the_budget_recovers_once"_test = [] {
        AgentSession session;
        RecoveryBackend backend;
        // Attempt 1: 502 (max_retries_per_step == 1 -> the budget is gone).
        // The recovery fires ONCE and the recovered attempt (scripted[1])
        // succeeds - the retry was spent on the recovery, not the budget.
        backend.scripted.push_back(RecoveryBackend::http_error(502));
        backend.scripted.push_back(RecoveryBackend::ok_text("recovered"));
        KimiSoul soul = make_soul(session, backend);
        const TurnResult r = soul.turn("hello");
        expect(backend.on_retryable_calls == 1);
        expect(backend.refresh_auth_calls == 0);
        expect(r.ok);
        expect(r.content == kimix::string("recovered"));
    };

    "recovered_attempt_can_succeed"_test = [] {
        AgentSession session;
        RecoveryBackend backend;
        backend.scripted.push_back(RecoveryBackend::connection_error());
        backend.scripted.push_back(RecoveryBackend::ok_text("recovered"));
        KimiSoul soul = make_soul(session, backend);
        const TurnResult r = soul.turn("hello");
        expect(backend.on_retryable_calls == 1);
        expect(r.ok);
        expect(r.content == kimix::string("recovered"));
        expect(r.steps == 1); // the recovered attempt is still ONE step
    };

    "auth_401_refresh_true_retries_outside_the_budget"_test = [] {
        AgentSession session;
        RecoveryBackend backend;
        backend.refresh_result = true;
        backend.scripted.push_back(RecoveryBackend::http_error(401));
        backend.scripted.push_back(RecoveryBackend::ok_text("after refresh"));
        KimiSoul soul = make_soul(session, backend);
        const TurnResult r = soul.turn("hello");
        expect(backend.refresh_auth_calls == 1);
        expect(backend.on_retryable_calls == 0);
        expect(r.ok);
        expect(r.content == kimix::string("after refresh"));
        expect(r.steps == 1);
    };

    "auth_401_refresh_false_keeps_the_failure"_test = [] {
        AgentSession session;
        RecoveryBackend backend;
        backend.refresh_result = false;
        backend.scripted.push_back(RecoveryBackend::http_error(401));
        KimiSoul soul = make_soul(session, backend);
        const TurnResult r = soul.turn("hello");
        expect(backend.refresh_auth_calls == 1);
        expect(!r.ok);
        // A non-restartable 4xx: the turn reports the chat failure.
        expect(r.error_kind == TurnErrorKind::chat_failed);
    };

    "each_recovery_kind_fires_at_most_once_per_step"_test = [] {
        AgentSession session;
        RecoveryBackend backend;
        backend.refresh_result = true;
        // 401 -> refresh (1) -> 401 again -> refresh refused -> fail.
        backend.scripted.push_back(RecoveryBackend::http_error(401));
        backend.scripted.push_back(RecoveryBackend::http_error(401));
        KimiSoul soul = make_soul(session, backend);
        const TurnResult r = soul.turn("hello");
        expect(backend.refresh_auth_calls == 1);
        expect(!r.ok);
    };

    "llm_backend_refresh_auth_uses_the_config_callback"_test = [] {
        struct DummyProvider : kimix::llm::ChatProvider {
            kimix::string model_name() const override { return "fake-model"; }
            kimix::llm::ChatResult
            chat(const kimix::vector<kimix::llm::Message> &,
                 const kimix::vector<kimix::llm::Tool> &,
                 const kimix::llm::ChunkCallback &,
                 const kimix::llm::AbortCheck *) const override {
                kimix::llm::ChatResult r;
                r.ok = true;
                return r;
            }
        };
        int calls = 0;
        bool result = false;
        {
            kimix::llm::Config cfg;
            cfg.model = "fake-model";
            cfg.url = "http://localhost:9";
            cfg.type = "openai";
            cfg.auth_refresh = [&calls, &result]() {
                ++calls;
                return result;
            };
            kimix::unique_ptr<kimix::llm::LLM> llm(
                new kimix::llm::LLM(
                    kimix::unique_ptr<kimix::llm::ChatProvider>(
                        new DummyProvider()),
                    cfg));
            LLMBackend backend(std::move(llm));
            // Unset callback == nothing to refresh (a plain API-key
            // provider); a callback that answers false keeps the failure.
            expect(!backend.refresh_auth());
            result = true;
            expect(backend.refresh_auth());
            expect(calls == 2);
        }
        expect(calls == 2);
    };

    return 0;
}
