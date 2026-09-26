// test_btw.cpp - Unit tests for the /btw side channel (src/agent/btw.* +
// KimiSoul::run_side_question, gap G11).
//
// Coverage:
// * a direct text answer streams through on_text and returns, leaving the
//   main history untouched;
// * a tool call on the first turn is denied with the reference wording and
//   the loop retries once (kBtwMaxTurns): the second request carries the
//   assistant message + the denied tool result, and a text answer then wins;
// * two consecutive tool-call turns fail with the reference error naming the
//   tools; an empty response fails with "No response received.";
// * the wire sees a BtwBegin/BtwEnd pair with the id, question and answer;
// * the side question shares the main system prompt and the normalized
//   history prefix (cache parity), with the question wrapped in the
//   SIDE_QUESTION_SYSTEM_REMINDER <system-reminder>.
//
// Framework: Boost.UT (tests/ut/ut.hpp).

#include "ut/ut.hpp"

#include <agent/btw.h>
#include <agent/soul.h>
#include <agent/wire.h>

#include <cstdint>
#include <system_error>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

kimix::filesystem::path test_dir(const char *name) {
    std::error_code ec;
    kimix::filesystem::path dir = kimix::filesystem::temp_directory_path(ec) /
                                  "kimix_btw_test" / name;
    kimix::filesystem::remove_all(dir, ec);
    kimix::filesystem::create_directories(dir, ec);
    return dir;
}

// A scripted backend: each chat() returns the next scripted step and records
// the request it saw.
class btw_backend : public kimix::agent::IChatBackend {
public:
    struct step {
        kimix::string content;
        kimix::vector<kimix::llm::ToolCall> calls;
    };
    kimix::vector<step> steps;
    size_t calls = 0;
    kimix::vector<kimix::vector<kimix::llm::Message>> seen;

    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &,
         const kimix::llm::ChunkCallback &on_chunk) override {
        seen.push_back(messages);
        kimix::llm::ChatResult result;
        result.ok = true;
        if (calls < steps.size()) {
            const step &s = steps[calls];
            result.content = s.content;
            result.tool_calls = s.calls;
        }
        ++calls;
        if (on_chunk && !result.content.empty()) {
            kimix::llm::Chunk chunk;
            chunk.ok = true;
            chunk.content = result.content;
            on_chunk(chunk);
        }
        return result;
    }
    kimix::string model_name() const override { return "btw-test"; }
    int64_t max_context_size() const override { return 100000; }
};

kimix::llm::ToolCall mk_call(kimix::string id, kimix::string name) {
    kimix::llm::ToolCall call;
    call.id = std::move(id);
    call.type = "function";
    call.name = std::move(name);
    call.arguments = "{}";
    return call;
}

// Captures the BtwBegin/BtwEnd pair.
struct btw_sink : kimix::agent::WireSink {
    int begins = 0;
    int ends = 0;
    kimix::string begin_id;
    kimix::string question;
    kimix::string end_id;
    kimix::string response;
    kimix::string error;
    void wire_turn_begin(kimix::string_view) override {}
    void wire_turn_end() override {}
    void wire_step_begin(int32_t) override {}
    void wire_step_interrupted() override {}
    void wire_steer_input(kimix::string_view) override {}
    void wire_step_retry(int32_t, int32_t, int32_t, double, kimix::string_view,
                         int32_t) override {}
    void wire_status_update(double, int64_t, int64_t, int64_t, int64_t, int64_t,
                            int64_t) override {}
    void wire_compaction_begin(kimix::string_view, kimix::string_view) override {}
    void wire_compaction_end(kimix::string_view, kimix::string_view, int64_t,
                             int64_t, kimix::string_view) override {}
    void wire_llm_request(const kimix::agent::llm_request_record &) override {}
    void wire_llm_tools_snapshot(kimix::string_view,
                                 const kimix::vector<kimix::llm::Tool> &) override {}
    void wire_mcp_tools_discovered(kimix::string_view, kimix::string_view,
                                   const kimix::vector<kimix::llm::Tool> &,
                                   const kimix::vector<kimix::string> &,
                                   const kimix::vector<kimix::string> &) override {}
    void wire_approval_request(kimix::string_view, kimix::string_view,
                               kimix::string_view, kimix::string_view,
                               kimix::string_view) override {}
    void wire_approval_response(kimix::string_view, kimix::string_view,
                                kimix::string_view) override {}
    void wire_btw_begin(kimix::string_view id, kimix::string_view q) override {
        ++begins;
        begin_id = kimix::string(id);
        question = kimix::string(q);
    }
    void wire_btw_end(kimix::string_view id, kimix::string_view r,
                      kimix::string_view e) override {
        ++ends;
        end_id = kimix::string(id);
        response = kimix::string(r);
        error = kimix::string(e);
    }
};

} // namespace

int main() {
    "direct_text_answer_does_not_touch_history"_test = [] {
        const auto dir = test_dir("direct");
        kimix::agent::AgentSession session(kimix::to_string(dir));
        session.append_history([] {
            kimix::llm::Message user;
            user.role = "user";
            user.content = "the original question";
            return user;
        }());
        session.append_history([] {
            kimix::llm::Message assistant;
            assistant.role = "assistant";
            assistant.content = "the original answer";
            return assistant;
        }());
        btw_backend backend;
        backend.steps.push_back({"Because it is.", {}});
        btw_sink sink;
        kimix::agent::KimiSoul soul(session, backend);
        soul.set_wire_sink(&sink);

        kimix::string streamed;
        const kimix::agent::SideQuestionResult result = soul.run_side_question(
            "why?", [&streamed](const kimix::llm::Chunk &chunk) {
                streamed += chunk.content;
            });
        expect(result.error.empty());
        expect(result.response == "Because it is.");
        expect(streamed == "Because it is.");
        expect(backend.calls == size_t{1});
        // The main history is untouched (user + assistant only).
        expect(session.history().size() == 2u);
        // The request shared the system prompt + history prefix and carried
        // the reminder-wrapped question last.
        expect(backend.seen.size() == 1u);
        const kimix::vector<kimix::llm::Message> &request = backend.seen[0];
        expect(request.front().role == "system");
        expect(request[1].role == "user");
        expect(request[1].content == "the original question");
        const kimix::llm::Message &side = request.back();
        expect(side.role == "user");
        expect(kimix::string_view(side.content).substr(0, 20) ==
               "<system-reminder>\nTh");
        expect(side.content.find("why?") != kimix::string::npos);
        // The wire pair brackets the run with matching ids.
        expect(sink.begins == 1);
        expect(sink.ends == 1);
        expect(sink.begin_id == sink.end_id);
        expect(sink.begin_id.size() == 12u) << "uuid4().hex[:12]";
        expect(sink.question == "why?");
        expect(sink.response == "Because it is.");
    };

    "denied_tool_call_gets_one_retry"_test = [] {
        const auto dir = test_dir("retry");
        kimix::agent::AgentSession session(kimix::to_string(dir));
        btw_backend backend;
        // Turn 1: the model tries a tool. Turn 2: it answers with text.
        backend.steps.push_back({"Let me check.", {mk_call("c1", "read")}});
        backend.steps.push_back({"It is fine.", {}});
        kimix::agent::KimiSoul soul(session, backend);
        const kimix::agent::SideQuestionResult result =
            soul.run_side_question("status?");
        expect(result.error.empty());
        expect(result.response == "It is fine.");
        expect(backend.calls == size_t{2});
        // The retry request = prefix + assistant(tool call) + denied result.
        const kimix::vector<kimix::llm::Message> &retry = backend.seen[1];
        expect(retry.size() == backend.seen[0].size() + 2);
        const kimix::llm::Message &assistant = retry[retry.size() - 2];
        expect(assistant.role == "assistant");
        expect(assistant.tool_calls.size() == 1u);
        const kimix::llm::Message &denial = retry.back();
        expect(denial.role == "tool");
        expect(denial.tool_call_id == "c1");
        expect(denial.content ==
               "Tool calls are disabled for side questions. Answer with text only.");
        expect(session.history().empty()) << "still no main-history pollution";
    };

    "persistent_tool_calls_fail_with_reference_error"_test = [] {
        const auto dir = test_dir("deny");
        kimix::agent::AgentSession session(kimix::to_string(dir));
        btw_backend backend;
        backend.steps.push_back({"", {mk_call("c1", "read")}});
        backend.steps.push_back({"", {mk_call("c2", "bash")}});
        kimix::agent::KimiSoul soul(session, backend);
        const kimix::agent::SideQuestionResult result =
            soul.run_side_question("now?");
        expect(result.response.empty());
        expect(result.error ==
               "Side question tried to call tools (bash) instead of answering "
               "directly. Try rephrasing or ask in the main conversation.");
        expect(backend.calls == 2u) << "kBtwMaxTurns = 2";
    };

    "empty_response_fails_with_no_response_received"_test = [] {
        const auto dir = test_dir("empty");
        kimix::agent::AgentSession session(kimix::to_string(dir));
        btw_backend backend;
        backend.steps.push_back({"", {}});
        kimix::agent::KimiSoul soul(session, backend);
        const kimix::agent::SideQuestionResult result =
            soul.run_side_question("hello?");
        expect(result.response.empty());
        expect(result.error == "No response received.");
    };

    "mixed_text_and_tool_call_is_not_an_answer"_test = [] {
        // btw.py:164-167: text + tool calls in one response is an incomplete
        // preamble, never an answer.
        const auto dir = test_dir("mixed");
        kimix::agent::AgentSession session(kimix::to_string(dir));
        btw_backend backend;
        backend.steps.push_back({"preamble text", {mk_call("c1", "read")}});
        backend.steps.push_back({"final answer", {}});
        kimix::agent::KimiSoul soul(session, backend);
        const kimix::agent::SideQuestionResult result =
            soul.run_side_question("mixed?");
        expect(result.error.empty());
        expect(result.response == "final answer");
        expect(backend.calls == size_t{2});
    };
}
