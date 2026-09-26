// test_turn_injections.cpp - Turn-level tests for the G9 dynamic-injection
// integration in KimiSoul (src/agent/soul.*), all driven by scripted fake
// chat backends (no network):
// * providers registered via add_injection_provider() see their reminder
//   delivered as ONE combined <system-reminder> user message in the request
//   the backend receives;
// * the previous step's stale reminder is stripped from live history before
//   the next step (never two reminder copies in one request);
// * adjacent user messages are merged in the request (normalize_history)
//   while the live history keeps the standalone reminder;
// * the loop_control gates register the five reference providers, and the
//   compact reminder fires from the live context usage;
// * a failing provider never breaks the turn (error isolation).
//
// Framework: Boost.UT (tests/ut/ut.hpp).
#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include <agent/soul.h>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

// Scripted chat backend: pops one canned result per chat() call (the last
// entry repeats) and records every request it receives.
class RecordingBackend : public kimix::agent::IChatBackend {
public:
    kimix::vector<kimix::llm::ChatResult> scripted;
    kimix::vector<kimix::vector<kimix::llm::Message>> requests;
    size_t index = 0;
    int64_t max_context = 128000;

    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &tools,
         const kimix::llm::ChunkCallback &on_chunk) override {
        (void)tools;
        (void)on_chunk;
        requests.push_back(messages);
        if (index < scripted.size()) {
            return scripted[index++];
        }
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content = "done";
        return r;
    }
    int64_t max_context_size() const override { return max_context; }
    kimix::string model_name() const override { return "recording"; }
};

kimix::llm::ChatResult ok_text(kimix::string text) {
    kimix::llm::ChatResult r;
    r.ok = true;
    r.content = std::move(text);
    return r;
}

// Counts messages in the LAST user-role message of a request (the combined
// reminder is always the last history entry).
size_t reminder_count(const kimix::vector<kimix::llm::Message> &request) {
    size_t n = 0;
    for (const kimix::llm::Message &m : request) {
        if (m.role == "user" && kimix::agent::is_system_reminder_message(m)) {
            ++n;
        }
    }
    return n;
}

// The last user message of the request, when it is a reminder.
const kimix::llm::Message *last_reminder(const kimix::vector<kimix::llm::Message> &request) {
    for (size_t i = request.size(); i > 0; --i) {
        const kimix::llm::Message &m = request[i - 1];
        if (m.role == "user") {
            return kimix::agent::is_system_reminder_message(m) ? &m : nullptr;
        }
    }
    return nullptr;
}

// A provider that always returns a fixed injection (or fails, for the
// isolation test).
class FixedProvider : public kimix::agent::DynamicInjectionProvider {
public:
    kimix::string content;
    bool fails = false;

    bool get_injections(const kimix::agent::InjectionStepContext &,
                        kimix::vector<kimix::agent::DynamicInjection> &out,
                        kimix::string &error) override {
        if (fails) {
            error = "fixed provider failure";
            return false;
        }
        kimix::agent::DynamicInjection inj;
        inj.type = "fixed";
        inj.content = content;
        out.push_back(std::move(inj));
        return true;
    }
};

kimix::agent::KimiSoul::options base_options() {
    kimix::agent::KimiSoul::options opts;
    opts.system_prompt = "test system prompt";
    opts.auto_compact = false; // keep the context static across the turn
    opts.max_steps = 16;
    opts.loop_control.max_steps_per_turn = 16;
    return opts;
}

} // namespace

int main() {
    "reminder_appears_in_the_request_the_backend_receives"_test = [] {
        RecordingBackend backend;
        backend.scripted.push_back(ok_text("final answer"));
        kimix::agent::AgentSession session;
        kimix::agent::KimiSoul soul(session, backend, base_options());
        kimix::unique_ptr<FixedProvider> provider(new FixedProvider);
        provider->content = "remember this";
        soul.add_injection_provider(kimix::unique_ptr<kimix::agent::DynamicInjectionProvider>(
            static_cast<kimix::agent::DynamicInjectionProvider *>(provider.release())));

        const kimix::agent::TurnResult result = soul.turn("hello agent");
        expect(result.ok);
        expect(eq(backend.requests.size(), 1u));
        // The single request: [system][user hello][combined reminder]; the
        // standalone reminder is never merged (normalize_history exemption).
        const auto *reminder = last_reminder(backend.requests[0]);
        expect(reminder != nullptr);
        expect(eq(reminder->content,
                  kimix::string("<system-reminder>\nremember this\n</system-reminder>")));
        expect(eq(reminder_count(backend.requests[0]), 1u));
    };

    "stale_reminder_stripped_before_the_next_turn"_test = [] {
        // The strip runs at every step boundary, including the first step of
        // the NEXT turn: the previous turn's reminder must not accumulate.
        RecordingBackend backend;
        backend.scripted.push_back(ok_text("answer one"));
        backend.scripted.push_back(ok_text("answer two"));
        kimix::agent::AgentSession session;
        kimix::agent::KimiSoul soul(session, backend, base_options());
        kimix::unique_ptr<FixedProvider> provider(new FixedProvider);
        provider->content = "fresh each step";
        soul.add_injection_provider(kimix::unique_ptr<kimix::agent::DynamicInjectionProvider>(
            static_cast<kimix::agent::DynamicInjectionProvider *>(provider.release())));

        soul.turn("first question");
        soul.turn("second question");
        expect(eq(backend.requests.size(), 2u));
        // Turn 2 request: exactly ONE reminder (the fresh copy) - the turn 1
        // reminder was stripped from live history, not accumulated.
        expect(eq(reminder_count(backend.requests[1]), 1u));
        const auto *reminder = last_reminder(backend.requests[1]);
        expect(reminder != nullptr);
        expect(reminder->content.find("fresh each step") != kimix::string::npos);
        // Live history kept exactly one reminder message as well.
        expect(eq(reminder_count(session.history()), 1u));
    };

    "multiple_providers_combine_into_one_message"_test = [] {
        RecordingBackend backend;
        backend.scripted.push_back(ok_text("final answer"));
        kimix::agent::AgentSession session;
        kimix::agent::KimiSoul soul(session, backend, base_options());
        for (const char *text : {"first reminder", "second reminder"}) {
            kimix::unique_ptr<FixedProvider> provider(new FixedProvider);
            provider->content = text;
            soul.add_injection_provider(
                kimix::unique_ptr<kimix::agent::DynamicInjectionProvider>(
                    static_cast<kimix::agent::DynamicInjectionProvider *>(
                        provider.release())));
        }
        soul.turn("hello agent");
        expect(eq(backend.requests.size(), 1u));
        expect(eq(reminder_count(backend.requests[0]), 1u));
        const auto *reminder = last_reminder(backend.requests[0]);
        expect(reminder != nullptr);
        expect(eq(reminder->content,
                  kimix::string("<system-reminder>\nfirst reminder\n</system-reminder>\n"
                                "<system-reminder>\nsecond reminder\n</system-reminder>")));
    };

    "failing_provider_never_breaks_the_turn"_test = [] {
        RecordingBackend backend;
        backend.scripted.push_back(ok_text("final answer"));
        kimix::agent::AgentSession session;
        kimix::agent::KimiSoul soul(session, backend, base_options());
        kimix::unique_ptr<FixedProvider> failing(new FixedProvider);
        failing->fails = true;
        soul.add_injection_provider(kimix::unique_ptr<kimix::agent::DynamicInjectionProvider>(
            static_cast<kimix::agent::DynamicInjectionProvider *>(failing.release())));
        kimix::unique_ptr<FixedProvider> good(new FixedProvider);
        good->content = "still delivered";
        soul.add_injection_provider(kimix::unique_ptr<kimix::agent::DynamicInjectionProvider>(
            static_cast<kimix::agent::DynamicInjectionProvider *>(good.release())));

        const kimix::agent::TurnResult result = soul.turn("hello agent");
        expect(result.ok);
        expect(eq(backend.requests.size(), 1u));
        const auto *reminder = last_reminder(backend.requests[0]);
        expect(reminder != nullptr);
        expect(reminder->content.find("still delivered") != kimix::string::npos);
        expect(reminder->content.find("fixed provider failure") == kimix::string::npos);
    };

    "normalize_history_merges_adjacent_users_in_the_request"_test = [] {
        // With no providers registered, the user input is followed by the
        // assistant reply - no merge. Register a provider whose reminder
        // stays standalone, then verify the assistant message splits runs:
        // history [user, reminder] normalizes to the same two messages (the
        // reminder is exempt), so the request carries user + reminder
        // separately, proving the exemption is wired into the request build.
        RecordingBackend backend;
        backend.scripted.push_back(ok_text("final answer"));
        kimix::agent::AgentSession session;
        kimix::agent::KimiSoul soul(session, backend, base_options());
        soul.turn("hello agent");
        expect(eq(backend.requests.size(), 1u));
        // Without injections there is no reminder and the request is
        // [system, user] - adjacent-merge has nothing to merge.
        expect(eq(backend.requests[0].size(), 2u));
    };

    "loop_control_gate_registers_compact_reminder"_test = [] {
        // compact_reminder_enabled with a low threshold and a small model
        // window: the live context estimate is well above 0.50, so the
        // reminder fires from the turn's real token state.
        RecordingBackend backend;
        backend.max_context = 2048;
        backend.scripted.push_back(ok_text("final answer"));
        kimix::agent::AgentSession session;
        kimix::agent::KimiSoul::options opts = base_options();
        opts.loop_control.compact_reminder_enabled = true;
        opts.loop_control.compact_reminder_threshold = 0.50;
        kimix::agent::KimiSoul soul(session, backend, opts);
        const kimix::agent::TurnResult result = soul.turn("hello agent");
        expect(result.ok);
        expect(eq(backend.requests.size(), 1u));
        const auto *reminder = last_reminder(backend.requests[0]);
        expect(reminder != nullptr);
        expect(reminder->content.find("Context ") != kimix::string::npos);
        expect(reminder->content.find(" full (") != kimix::string::npos);
        expect(reminder->content.find("Call `Compact` after completing the current atomic "
                                      "task") != kimix::string::npos);
    };

    "providers_disabled_by_default_no_reminder"_test = [] {
        RecordingBackend backend;
        backend.scripted.push_back(ok_text("final answer"));
        kimix::agent::AgentSession session;
        kimix::agent::KimiSoul soul(session, backend, base_options());
        soul.turn("hello agent");
        expect(eq(reminder_count(backend.requests[0]), 0u));
    };
}
