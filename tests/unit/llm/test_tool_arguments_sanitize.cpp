// Test for kimix::llm::sanitize_tool_arguments (llm/common.h + llm/common.cpp).
//
// Regression context: some LLM gateways stream duplicated tool-call argument
// chunks which the accumulator merges into strings like "{}{}". Such
// arguments execute leniently on the client but poison the persisted chat
// history — strict backends (e.g. scnet/Qwen) reject the whole request with
// HTTP 400 (code 10013) when the invalid arguments are echoed back in
// tool_calls[].function.arguments. sanitize_tool_arguments guarantees the
// string sent to / stored in history is always strict-valid JSON:
//   1. already-valid JSON    -> unchanged
//   2. trailing garbage / duplicated chunks -> first complete JSON value
//   3. nothing parseable     -> "{}"
//   empty input              -> "{}"
//
// Every produced string is additionally verified with a strict yyjson parse.

#include "ut/ut.hpp"

#include "llm/common.h"
#include "llm/llm.h"

#include "yyjson.h"

#include "llm/yyjson_alc.h"

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::llm;

namespace {

// Strict-parse check used to validate every sanitize output.
bool is_strict_json(const kimix::string &s) {
    yyjson_doc *doc = yyjson_read_opts((char *)s.data(), s.size(), 0,
                                       &kYYJsonAlcMi, nullptr);
    if (doc) {
        yyjson_doc_free(doc);
        return true;
    }
    return false;
}

// Stub provider returning a canned result; lets the test drive LLM::chat
// (result-path sanitizing) without any network access.
class StubProvider : public ChatProvider {
public:
    ChatResult result;

    kimix::string model_name() const override { return "stub"; }

    ChatResult chat(const kimix::vector<Message> &,
                    const kimix::vector<Tool> &,
                    const ChunkCallback &) const override {
        return result;
    }
};

} // namespace

int main() {
    "valid_object_unchanged"_test = [] {
        expect(sanitize_tool_arguments(R"({"command":"ls"})")
               == R"({"command":"ls"})");
    };

    "valid_empty_object_unchanged"_test = [] {
        expect(sanitize_tool_arguments("{}") == "{}");
    };

    "valid_array_unchanged"_test = [] {
        expect(sanitize_tool_arguments("[1,2,3]") == "[1,2,3]");
    };

    "valid_scalar_unchanged"_test = [] {
        expect(sanitize_tool_arguments("42") == "42");
        expect(sanitize_tool_arguments("\"text\"") == "\"text\"");
        expect(sanitize_tool_arguments("null") == "null");
        expect(sanitize_tool_arguments("true") == "true");
    };

    "duplicated_chunk_repaired"_test = [] {
        // The exact production poison: two "{}" chunks merged into "{}{}".
        expect(sanitize_tool_arguments("{}{}") == "{}");
    };

    "duplicated_object_chunk_repaired"_test = [] {
        expect(sanitize_tool_arguments(R"({"a":1}{"b":2})") == R"({"a":1})");
    };

    "trailing_garbage_truncated"_test = [] {
        expect(sanitize_tool_arguments(R"({"a":1}garbage)") == R"({"a":1})");
        expect(sanitize_tool_arguments(R"({"a":1} trailing)") == R"({"a":1})");
    };

    "trailing_garbage_after_array_truncated"_test = [] {
        expect(sanitize_tool_arguments("[1,2]extra") == "[1,2]");
    };

    "trailing_garbage_after_scalar_truncated"_test = [] {
        expect(sanitize_tool_arguments("42xyz") == "42");
    };

    "leading_whitespace_preserved_in_prefix"_test = [] {
        const kimix::string out = sanitize_tool_arguments("  {}{}");
        expect(is_strict_json(out));
        expect(out.find("{}") != kimix::string::npos);
    };

    "truncated_object_falls_back_to_empty_object"_test = [] {
        expect(sanitize_tool_arguments(R"({"command": )") == "{}");
        expect(sanitize_tool_arguments("{") == "{}");
    };

    "garbage_falls_back_to_empty_object"_test = [] {
        expect(sanitize_tool_arguments("not json at all") == "{}");
    };

    "empty_input_becomes_empty_object"_test = [] {
        expect(sanitize_tool_arguments("") == "{}");
    };

    "all_outputs_are_strict_parseable"_test = [] {
        const char *cases[] = {
            R"({"command":"ls"})",
            "{}",
            "{}{}",
            R"({"a":1}{"b":2})",
            R"({"a":1}garbage)",
            "[1,2]extra",
            "42xyz",
            R"({"command": )",
            "{",
            "not json at all",
            "",
            "  {}{}",
        };
        for (const char *c : cases) {
            const kimix::string out = sanitize_tool_arguments(c);
            expect(is_strict_json(out)) << "input: " << c;
        }
    };

    "sanitize_is_idempotent"_test = [] {
        const char *cases[] = {
            "{}{}",
            R"({"a":1}garbage)",
            R"({"command": )",
            "garbage",
            "",
        };
        for (const char *c : cases) {
            const kimix::string once = sanitize_tool_arguments(c);
            const kimix::string twice = sanitize_tool_arguments(once);
            expect(once == twice) << "input: " << c;
        }
    };

    // The LLM::chat result path is what feeds the agent's persisted history
    // (soul.cpp stores res.tool_calls verbatim): every tool call the backend
    // returns must be strict-valid JSON before the caller ever sees it.
    "chat_results_are_sanitized"_test = [] {
        auto *raw = new StubProvider();
        raw->result.ok = true;
        raw->result.tool_calls.push_back(
            {"id_1", "function", "bash", "{}{}"});// duplicated gateway chunk
        raw->result.tool_calls.push_back(
            {"id_2", "function", "bash", R"({"command": )"});// truncated
        raw->result.tool_calls.push_back(
            {"id_3", "function", "bash", "garbage"});// unparseable
        raw->result.tool_calls.push_back(
            {"id_4", "function", "bash", R"({"command":"ls"})"});// valid
        LLM llm(kimix::unique_ptr<ChatProvider>(raw), Config{});

        const ChatResult out = llm.chat({}, {});

        expect(out.tool_calls.size() == 4u);
        expect(out.tool_calls[0].arguments == "{}");
        // Truncated objects are first leniently repaired (null-filled), then
        // kept as-is by the strict sanitize pass.
        expect(out.tool_calls[1].arguments == R"({"command":null})");
        expect(out.tool_calls[2].arguments == "{}");
        expect(out.tool_calls[3].arguments == R"({"command":"ls"})");
        for (const auto &tc : out.tool_calls) {
            expect(is_strict_json(tc.arguments)) << "id: " << tc.id.c_str();
        }
    };
}
