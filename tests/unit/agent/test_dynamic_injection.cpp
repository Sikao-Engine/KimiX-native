// test_dynamic_injection.cpp - Unit tests for the G9 dynamic-injection
// framework (src/agent/dynamic_injection.h): the system_reminder wrap format,
// the is_system_reminder predicate, strip_system_reminders, normalize_history,
// the combined-reminder build, and per-provider error isolation in the
// registry collect/notify loops. All expected strings are the reference's
// verbatim text (soul/message.py, dynamic_injection.py).
//
// Framework: Boost.UT (tests/ut/ut.hpp).
#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include <agent/dynamic_injection.h>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

kimix::llm::Message user_msg(kimix::string_view text) {
    kimix::llm::Message m;
    m.role = "user";
    m.content.assign(text.data(), text.size());
    return m;
}

kimix::llm::Message assistant_msg(kimix::string_view text) {
    kimix::llm::Message m;
    m.role = "assistant";
    m.content.assign(text.data(), text.size());
    return m;
}

kimix::llm::Message tool_msg(kimix::string_view text) {
    kimix::llm::Message m;
    m.role = "tool";
    m.tool_call_id = "call-1";
    m.content.assign(text.data(), text.size());
    return m;
}

// A scripted provider: returns a fixed injection, or fails when `fails`.
class ScriptedProvider : public kimix::agent::DynamicInjectionProvider {
public:
    kimix::string type;
    kimix::string content;
    bool fails = false;
    int calls = 0;
    int compacted = 0;
    int afk = 0;

    bool get_injections(const kimix::agent::InjectionStepContext &,
                        kimix::vector<kimix::agent::DynamicInjection> &out,
                        kimix::string &error) override {
        ++calls;
        if (fails) {
            error = "scripted provider failure";
            return false;
        }
        kimix::agent::DynamicInjection inj;
        inj.type = type;
        inj.content = content;
        out.push_back(std::move(inj));
        return true;
    }
    void on_context_compacted() override { ++compacted; }
    void on_afk_changed(bool enabled) override { (void)enabled; ++afk; }
};

kimix::unique_ptr<ScriptedProvider> make_provider(kimix::string type,
                                                  kimix::string content) {
    kimix::unique_ptr<ScriptedProvider> p(new ScriptedProvider);
    p->type = std::move(type);
    p->content = std::move(content);
    return p;
}

} // namespace

int main() {
    "system_reminder_wrap_is_verbatim"_test = [] {
        // message.py:24-25: f"<system-reminder>\n{message}\n</system-reminder>"
        expect(eq(kimix::agent::system_reminder_text("hello"),
                  kimix::string("<system-reminder>\nhello\n</system-reminder>")));
    };

    "is_system_reminder_predicate"_test = [] {
        using kimix::agent::is_system_reminder_message;
        expect(is_system_reminder_message(user_msg("<system-reminder>\nx\n</system-reminder>")));
        // Python lstrip semantics: leading whitespace still matches.
        expect(is_system_reminder_message(user_msg("  \n<system-reminder>x")));
        // A reminder embedded later in the text is NOT a standalone reminder.
        expect(!is_system_reminder_message(user_msg("note <system-reminder>x")));
        expect(!is_system_reminder_message(assistant_msg("<system-reminder>x")));
        expect(!is_system_reminder_message(user_msg("")));
    };

    "strip_system_reminders_removes_only_standalone"_test = [] {
        kimix::vector<kimix::llm::Message> history;
        history.push_back(user_msg("real request"));
        history.push_back(user_msg("<system-reminder>\nstale\n</system-reminder>"));
        history.push_back(assistant_msg("answer"));
        history.push_back(user_msg("  <system-reminder>\nanother\n</system-reminder>"));
        history.push_back(tool_msg("result"));
        const size_t removed = kimix::agent::strip_system_reminders(history);
        expect(eq(removed, 2u));
        expect(eq(history.size(), 3u));
        expect(eq(history[0].content, kimix::string("real request")));
        expect(eq(history[1].role, kimix::string("assistant")));
        expect(eq(history[2].role, kimix::string("tool")));
    };

    "normalize_history_merges_adjacent_users"_test = [] {
        // dynamic_injection.py:59-93: adjacent user messages merge;
        // assistant/tool boundaries split runs.
        kimix::vector<kimix::llm::Message> history;
        history.push_back(user_msg("first"));
        history.push_back(user_msg("second"));
        history.push_back(assistant_msg("thinking out loud"));
        history.push_back(user_msg("third"));
        const kimix::vector<kimix::llm::Message> out =
            kimix::agent::normalize_history(history);
        expect(eq(out.size(), 3u));
        expect(eq(out[0].role, kimix::string("user")));
        expect(eq(out[0].content, kimix::string("firstsecond")));
        expect(eq(out[1].role, kimix::string("assistant")));
        expect(eq(out[2].content, kimix::string("third")));
    };

    "normalize_history_never_merges_reminders"_test = [] {
        // The exemption: ephemeral reminders must stay standalone so the
        // request-time boundary matches storage (cache stability).
        kimix::vector<kimix::llm::Message> history;
        history.push_back(user_msg("real request"));
        history.push_back(user_msg("<system-reminder>\nfresh\n</system-reminder>"));
        history.push_back(user_msg("trailing"));
        const kimix::vector<kimix::llm::Message> out =
            kimix::agent::normalize_history(history);
        expect(eq(out.size(), 3u));
        expect(eq(out[1].content,
                  kimix::string("<system-reminder>\nfresh\n</system-reminder>")));
    };

    "combined_reminder_joins_wrapped_blocks"_test = [] {
        // kimisoul.py:1642: "\n".join(system_reminder(inj.content).text ...)
        kimix::vector<kimix::agent::DynamicInjection> injections(2);
        injections[0].type = "a";
        injections[0].content = "one";
        injections[1].type = "b";
        injections[1].content = "two";
        const kimix::string combined = kimix::agent::build_combined_reminder(injections);
        expect(eq(combined, kimix::string("<system-reminder>\none\n</system-reminder>\n"
                                          "<system-reminder>\ntwo\n</system-reminder>")));
        expect(kimix::agent::build_combined_reminder({}).empty());
    };

    "registry_collect_isolates_failing_provider"_test = [] {
        // kimisoul.py:692-705: a raising provider is logged and skipped;
        // the others still contribute. (Exception-free port: failure via
        // the bool + error out-parameter.)
        kimix::agent::InjectionRegistry registry;
        ScriptedProvider *failing = nullptr;
        {
            kimix::unique_ptr<ScriptedProvider> p = make_provider("bad", "nope");
            p->fails = true;
            failing = p.get();
            registry.add_provider(kimix::unique_ptr<kimix::agent::DynamicInjectionProvider>(
                static_cast<kimix::agent::DynamicInjectionProvider *>(p.release())));
        }
        ScriptedProvider *good1 = nullptr;
        {
            kimix::unique_ptr<ScriptedProvider> p = make_provider("g1", "first");
            good1 = p.get();
            registry.add_provider(kimix::unique_ptr<kimix::agent::DynamicInjectionProvider>(
                static_cast<kimix::agent::DynamicInjectionProvider *>(p.release())));
        }
        ScriptedProvider *good2 = nullptr;
        {
            kimix::unique_ptr<ScriptedProvider> p = make_provider("g2", "second");
            good2 = p.get();
            registry.add_provider(kimix::unique_ptr<kimix::agent::DynamicInjectionProvider>(
                static_cast<kimix::agent::DynamicInjectionProvider *>(p.release())));
        }
        const kimix::vector<kimix::agent::DynamicInjection> got =
            registry.collect({});
        expect(eq(got.size(), 2u));
        expect(eq(got[0].content, kimix::string("first")));
        expect(eq(got[1].content, kimix::string("second")));
        expect(eq(failing->calls, 1));
        expect(eq(good1->calls, 1));
        expect(eq(good2->calls, 1));
    };

    "registry_notify_hooks_reach_all_providers"_test = [] {
        kimix::agent::InjectionRegistry registry;
        kimix::unique_ptr<ScriptedProvider> p = make_provider("x", "y");
        ScriptedProvider *raw = p.get();
        registry.add_provider(kimix::unique_ptr<kimix::agent::DynamicInjectionProvider>(
            static_cast<kimix::agent::DynamicInjectionProvider *>(p.release())));
        registry.notify_context_compacted();
        registry.notify_afk_changed(true);
        expect(eq(raw->compacted, 1));
        expect(eq(raw->afk, 1));
    };

    "py_percent0_matches_python_format"_test = [] {
        expect(eq(kimix::agent::py_percent0(0.70), kimix::string("70%")));
        expect(eq(kimix::agent::py_percent0(0.705), kimix::string("70%"))); // half-even
        expect(eq(kimix::agent::py_percent0(0.695), kimix::string("70%")));
        expect(eq(kimix::agent::py_percent0(0.75), kimix::string("75%"))); // odd base rounds up
        expect(eq(kimix::agent::py_percent0(0.749), kimix::string("75%")));
        expect(eq(kimix::agent::py_percent0(1.0), kimix::string("100%")));
        expect(eq(kimix::agent::py_percent0(0.25), kimix::string("25%")));
    };
}
