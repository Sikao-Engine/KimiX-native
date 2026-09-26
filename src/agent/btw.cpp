// agent/btw.cpp - KimiSoul::run_side_question, the /btw side question
// execution loop (see btw.h; port of btw.py execute_side_question +
// run_side_question).

#include "agent/btw.h"

#include <random>

#include "agent/dynamic_injection.h" // strip_system_reminders / normalize_history / system_reminder_text
#include "agent/soul.h"

namespace kimix::agent {

namespace {

kimix::string_view trim_ascii(kimix::string_view s) noexcept {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' ||
                            s[b] == '\r')) {
        ++b;
    }
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' ||
                     s[e - 1] == '\r')) {
        --e;
    }
    return s.substr(b, e - b);
}

} // namespace

kimix::string new_btw_id() {
    // uuid.uuid4().hex[:12] analogue: 6 random bytes as 12 lowercase hex chars.
    std::random_device rd;
    kimix::string out;
    out.reserve(12);
    static constexpr char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 6; ++i) {
        const unsigned b = static_cast<unsigned>(rd()) & 0xFFu;
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

SideQuestionResult KimiSoul::run_side_question(kimix::string_view question,
                                               const SoulEventCallback &on_text) {
    SideQuestionResult out;
    // run_side_question (btw.py:230-246): the BtwBegin/BtwEnd pair brackets
    // the whole execution; the question and answer never touch the main
    // history (the request below is built on copies).
    const kimix::string btw_id = new_btw_id();
    if (_wire != nullptr) {
        _wire->wire_btw_begin(btw_id, question);
    }
    struct btw_end_guard {
        WireSink *wire;
        kimix::string id;
        SideQuestionResult *result;
        ~btw_end_guard() {
            if (wire != nullptr) {
                wire->wire_btw_end(id, result->response, result->error);
            }
        }
    } end_guard{_wire, btw_id, &out};

    // _build_btw_context (btw.py:80-99): same system prompt, stale reminders
    // stripped and adjacent user messages merged - exactly the next main-loop
    // step's normalized prefix, so the provider keeps its cache. The reference
    // hands the system prompt to kosong.step separately; the unified native
    // backend carries it as the first system message.
    kimix::vector<kimix::llm::Message> history = _session.history();
    strip_system_reminders(history);
    kimix::vector<kimix::llm::Message> request =
        normalize_history(history);
    const kimix::string system_prompt = effective_system_prompt();
    kimix::llm::Message sys;
    sys.role = "system";
    sys.content = system_prompt;
    request.insert(request.begin(), std::move(sys));

    // The side message: the reminder wrap + the question (btw.py:94-96).
    kimix::llm::Message side;
    side.role = "user";
    side.content = system_reminder_text(kBtwSystemReminder);
    side.content += "\n\n";
    side.content.append(question.data(), question.size());
    request.push_back(std::move(side));

    // The real tool table, declared for prompt-cache stability only; every
    // call is denied like _DenyAllToolset.handle.
    const kimix::vector<kimix::llm::Tool> tools = tool_definitions();

    for (int32_t turn = 0; turn < kBtwMaxTurns; ++turn) {
        // The reference steps through kosong without the retry/escalation
        // machinery (btw.py:156); the native analogue is one bare backend call.
        kimix::string text;
        const SoulEventCallback collect = [&text, &on_text](const kimix::llm::Chunk &chunk) {
            if (!chunk.content.empty()) {
                text += chunk.content;
                if (on_text) {
                    on_text(chunk);
                }
            }
        };
        const kimix::llm::ChatResult res = _backend.chat(request, tools, collect);
        if (!res.ok) {
            out.error = res.error.empty() ? kimix::string("chat failed")
                                          : kimix::string("chat failed: ") + res.error;
            return out;
        }
        // Accept text ONLY when the model did not also call tools (mixed
        // text+tool is an incomplete preamble, btw.py:164-170).
        const kimix::string_view trimmed = trim_ascii(text);
        const kimix::string response_text(trimmed.data(), trimmed.size());
        if (!response_text.empty() && res.tool_calls.empty()) {
            out.response = response_text;
            return out;
        }
        if (res.tool_calls.empty()) {
            break; // no text, no tool calls - give up (btw.py:174-175)
        }
        if (turn + 1 >= kBtwMaxTurns) {
            // Last turn and still no text (btw.py:189-195).
            kimix::string names;
            for (size_t i = 0; i < res.tool_calls.size(); ++i) {
                if (i > 0) {
                    names += ", ";
                }
                names += res.tool_calls[i].name;
            }
            out.error = "Side question tried to call tools (" + names +
                        ") instead of answering directly. Try rephrasing or ask "
                        "in the main conversation.";
            return out;
        }
        // Feed the denial back and give the model a second chance
        // (btw.py:179-187): original history + the assistant message + one
        // denied tool result per call.
        kimix::llm::Message assistant;
        assistant.role = "assistant";
        assistant.content = res.content;
        assistant.thinking = res.reasoning;
        assistant.thinking_signature = res.signature;
        assistant.tool_calls = res.tool_calls;
        request.push_back(std::move(assistant));
        for (const kimix::llm::ToolCall &tc : res.tool_calls) {
            kimix::llm::Message denial;
            denial.role = "tool";
            denial.tool_call_id = tc.id;
            denial.content = kBtwToolDeniedMessage;
            request.push_back(std::move(denial));
        }
    }
    out.error = "No response received."; // btw.py:197-198
    return out;
}

} // namespace kimix::agent
