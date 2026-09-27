// test_tool_argument_repair.cpp - F9 (audit G16/G17/G18/G24): the argument
// anti-hallucination repair pipeline - the {"arguments":...}/{"args":...}
// unwrap, stringified-JSON parsing, the schema-driven non-string coercion,
// the todo top-level shape repair, the canonical call key (F11) and the long
// malformed content param -> temp .txt flow with the reference wording.
//
// Framework: Boost.UT; every assertion lives in main()-scope lambdas.

#include "ut/ut.hpp"

#include <core/kimix_core.h>

#include "agent/soul.h"
#include "agent/tool_argument_repair.h"
#include "agent/tool_name_resolver.h"

#include <cstdio>

#include <llm/yyjson_alc.h>
#include "yyjson.h"

namespace {

using namespace boost::ut;

class FakeBackend : public kimix::agent::IChatBackend {
public:
    kimix::llm::ChatResult
    chat(const kimix::vector<kimix::llm::Message> &messages,
         const kimix::vector<kimix::llm::Tool> &tools,
         const kimix::llm::ChunkCallback &on_chunk,
         const kimix::llm::AbortCheck * /*abort*/) override {
        (void)messages;
        (void)tools;
        (void)on_chunk;
        kimix::llm::ChatResult r;
        r.ok = true;
        r.content = "done";
        return r;
    }
    int64_t max_context_size() const override { return 128000; }
    kimix::string model_name() const override { return "fake"; }
};

kimix::string tmp_workspace(const char *name) {
    std::error_code ec;
    kimix::filesystem::path base =
        kimix::filesystem::temp_directory_path(ec) / name;
    kimix::filesystem::remove_all(base, ec);
    kimix::filesystem::create_directories(base, ec);
    return kimix::to_string(base);
}

// True when `haystack` parses as a JSON object carrying `key` with a
// non-string JSON value (i.e. the string was coerced into structure).
bool has_structured_field(kimix::string_view haystack, const char *key) {
    kimix::string buffer(haystack);
    yyjson_doc *doc = yyjson_read_opts(buffer.data(), buffer.size(),
                                       YYJSON_READ_STOP_WHEN_DONE,
                                       &kimix::llm::kYYJsonAlcMi, nullptr);
    if (doc == nullptr) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    bool ok = false;
    if (root != nullptr && yyjson_is_obj(root)) {
        yyjson_val *v = yyjson_obj_get(root, key);
        ok = v != nullptr && (yyjson_is_arr(v) || yyjson_is_obj(v));
    }
    yyjson_doc_free(doc);
    return ok;
}

} // namespace

int main() {
    using namespace boost::ut;
    using namespace kimix::agent;

    // ── 1. Format repairs (toolset.py:657-699) ──────────────────────────────

    "unwrap_double_wrapped_arguments"_test = [] {
        kimix::string out;
        expect(repair_argument_format(
            R"({"arguments":{"file_path":"a.txt"}})", out));
        expect(eq(out, kimix::string(R"({"file_path":"a.txt"})")));
        // The {"args": ...} spelling unwraps too.
        expect(repair_argument_format(R"({"args":{"path":"b"}})", out));
        expect(eq(out, kimix::string(R"({"path":"b"})")));
        // A well-formed call passes through byte-stable.
        expect(repair_argument_format(R"({"path":"c"})", out));
        expect(eq(out, kimix::string(R"({"path":"c"})")));
    };

    "parse_stringified_arguments_then_unwrap_again"_test = [] {
        kimix::string out;
        // A fully stringified argument object is parsed...
        expect(repair_argument_format(
            R"("{\"path\":\"a.txt\",\"flags\":[1,2]}")", out));
        expect(has_structured_field(out, "flags"));
        // ...and a stringified DOUBLE-wrapped object is parsed and unwrapped.
        expect(repair_argument_format(
            R"("{\"arguments\":{\"path\":\"a.txt\"}}")", out));
        expect(eq(out, kimix::string(R"({"path":"a.txt"})")));
        // Non-JSON strings are left alone.
        expect(repair_argument_format(R"("just text")", out));
        expect(eq(out, kimix::string(R"("just text")")));
    };

    // ── 2. Schema-driven JSON-string repair (utils.py repair_tool_arguments) ──

    "schema_driven_json_string_coercion"_test = [] {
        const char *schema_text =
            R"JSON({"type":"object","properties":{"todos":{"type":"array","items":{"type":"object"}},"title":{"type":"string"},"count":{"type":"integer"}},"required":[]})JSON";
        tool_param_schema schema;
        expect(parse_tool_param_schema(schema_text, schema));
        kimix::string out;
        expect(repair_tool_arguments(
            R"({"todos":"[{\"title\":\"a\"}]","title":"[1,2]","count":"3"})",
            schema, out));
        // The array-typed field was parsed back into a list...
        expect(has_structured_field(out, "todos"));
        // ...while the plain-string field kept its raw string value.
        expect(!has_structured_field(out, "title"));
        // Without a schema nothing is rewritten.
        kimix::string untouched;
        expect(repair_tool_arguments(
            R"({"todos":"[1]"})", tool_param_schema{}, untouched));
        expect(eq(untouched,
                  kimix::string(R"({"todos":"[1]"})")));
    };

    // ── 3. Todo top-level shape repair (toolset.py:739-802) ────────────────

    "todo_write_repairs_the_top_level_shape"_test = [] {
        kimix::string out;
        // A retired batch key folds onto the canonical one.
        expect(repair_todo_arguments("todo_write",
                                     R"({"items":[{"title":"a"}]})", out));
        expect(has_structured_field(out, "todos"));
        // A bare string list becomes one wrapped item.
        expect(repair_todo_arguments("todo_write", R"({"todos":"write the doc"})",
                                     out));
        expect(out.find("\"title\":\"write the doc\"") != kimix::string::npos)
            << out;
        // A single object becomes a one-item list.
        expect(repair_todo_arguments("todo_write",
                                     R"({"todos":{"title":"a","status":"done"}})",
                                     out));
        expect(out.find("[{") != kimix::string::npos) << out;
        // A singular key is promoted; top-level extras fold into the item.
        expect(repair_todo_arguments("todo_write",
                                     R"({"task":"ship it","status":"done"})",
                                     out));
        expect(out.find("\"title\":\"ship it\"") != kimix::string::npos) << out;
        expect(out.find("\"status\":\"done\"") != kimix::string::npos) << out;
        // Well-formed calls are untouched.
        kimix::string same;
        expect(repair_todo_arguments("todo_write",
                                     R"({"todos":[{"title":"a"}]})", same));
        expect(eq(same, kimix::string(R"({"todos":[{"title":"a"}]})")));
        // The repair is scoped to the todo tools.
        kimix::string other;
        expect(repair_todo_arguments("read", R"({"task":"x"})", other));
        expect(eq(other, kimix::string(R"({"task":"x"})")));
    };

    "todo_update_folds_onto_updates"_test = [] {
        kimix::string out;
        expect(eq(todo_batch_key_for("todo_update"),
                  kimix::string_view("updates")));
        expect(eq(todo_batch_key_for("todo_write"), kimix::string_view("todos")));
        expect(repair_todo_arguments("todo_update",
                                     R"({"edits":[{"title":"a","status":"done"}]})",
                                     out));
        expect(has_structured_field(out, "updates"));
    };

    // ── 4. Canonical call key (F11) ────────────────────────────────────────

    "canonical_tool_arguments_sorts_keys_recursively"_test = [] {
        const kimix::string a = canonical_tool_arguments(
            R"({"b":1,"a":{"d":2,"c":3}})");
        const kimix::string b = canonical_tool_arguments(
            R"({"a":{"c":3,"d":2},"b":1})");
        expect(!a.empty());
        expect(eq(a, b)); // key order is irrelevant for the call key
        expect(a.find("\"a\":{\"c\":3,\"d\":2}") != kimix::string::npos) << a;
        // Non-JSON input is returned unchanged (the reference's fallback).
        expect(eq(canonical_tool_arguments("not json"),
                  kimix::string("not json")));
    };

    // ── 5. Long malformed content params (common.py:50-250) ────────────────

    "long_param_detector_and_extractor"_test = [] {
        expect(!looks_like_malformed_json_param("short"));
        // Case 3: escaped newlines only.
          kimix::string escaped;
          for (int i = 0; i < 40; ++i) {
              escaped += "line\\n";
          }
          expect(escaped.size() >= kLongParamMinLength);
          expect(looks_like_malformed_json_param(escaped));
        auto content = extract_content_from_malformed(escaped);
        expect(content.has_value());
        if (content.has_value()) {
            expect((*content).find("line\nline") != kimix::string::npos);
        }
        // Case 2: a JSON array of lines.
        const kimix::string list_param =
            R"(["first line","second line","third line"])";
        content = extract_content_from_malformed(list_param);
        expect(content.has_value());
        if (content.has_value()) {
            expect(eq(*content, kimix::string("first line\nsecond line\nthird line")));
        }
        // Case 1: a JSON-encoded string.
        content = extract_content_from_malformed(
            "\"just a long enough payload to be plausible here\"");
        expect(content.has_value());
        // A plain string extracts nothing.
        expect(!extract_content_from_malformed("plain text").has_value());
        // The known long-content parameter names.
        expect(long_content_params_of("bash").size() == 2u);
        expect(long_content_params_of("python").size() == 3u);
        expect(long_content_params_of("write").size() == 2u);
        expect(long_content_params_of("edit").size() == 4u);
        expect(long_content_params_of("read").empty());
    };

    "extract_and_save_long_param_writes_the_temp_file"_test = [] {
        const kimix::string ws = tmp_workspace("kimix_test_arg_repair_ws");
        kimix::string big;
        for (int i = 0; i < 40; ++i) {
            // \\\\n at the JSON level decodes to a literal backslash-n inside
            // the string value: the escaped-newline shape (common.py case 4).
            big += "some very long command line segment that keeps going\\\\n";
        }
        expect(big.size() > kLongParamMinLength);
        kimix::string arguments =
            R"({"command":")" + big + R"("})";
        kimix::vector<long_param_save> saved;
        kimix::string error;
        expect(extract_and_save_long_param(arguments, "bash", ws, saved, error));
        expect(saved.size() == 1u);
        if (saved.size() == 1u) {
            expect(eq(saved[0].param, kimix::string("command")));
            // The file exists and holds the recovered content.
            std::error_code ec;
            expect(kimix::filesystem::file_size(
                       kimix::filesystem::path(saved[0].path), ec) > 0);
            std::FILE *f = std::fopen(saved[0].path.c_str(), "rb");
            expect(f != nullptr);
            if (f != nullptr) {
                char buffer[64] = {};
                const size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, f);
                std::fclose(f);
                expect(n > 0);
                expect(kimix::string(buffer, n).find("line") !=
                       kimix::string::npos);
            }
        }
        // The retry message carries the reference wording + the file list.
        const kimix::string msg = build_long_param_retry_msg(
            saved,
            "Parameters appear to be in the wrong format. The raw content has "
            "been saved to temp files.",
            ws);
        expect(msg.find("[Long content extracted to temp files]") !=
               kimix::string::npos)
            << msg;
        expect(msg.find("Please use `read` to inspect the files and retry with "
                        "the correct format.") != kimix::string::npos)
            << msg;
        expect(msg.find("  - `command`: saved to `.kimix_cache/") !=
               kimix::string::npos)
            << msg;
        // Short values are never extracted.
        expect(!extract_and_save_long_param(R"({"command":"ls"})", "bash", ws,
                                            saved, error));
    };

    // ── 6. Dispatch-level: the pipeline runs before the tool ───────────────

    "soul_dispatch_repairs_a_double_wrapped_write"_test = [] {
        const kimix::string ws = tmp_workspace("kimix_test_arg_repair_ws2");
        kimix::agent::AgentSession session(ws);
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        kimix::string err;
        // The arguments arrive double-wrapped AND the content stringified.
        const kimix::string out = soul.execute_tool_call(
            "write",
            R"({"arguments":{"file_path":"repaired.txt","content":"saved body"}})",
            err);
        expect(err.empty()) << err;
        std::error_code ec;
        expect(kimix::filesystem::exists(
            kimix::filesystem::path(ws) / "repaired.txt", ec));
    };

    "soul_dispatch_saves_a_long_malformed_command"_test = [] {
        kimix::agent::AgentSession session(
            tmp_workspace("kimix_test_arg_repair_ws3"));
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        kimix::string big;
        for (int i = 0; i < 40; ++i) {
            big += "a rather long script line that the model mangled\\n";
        }
        expect(big.size() > kLongParamMinLength);
        kimix::string err;
        // The long command arrives as a JSON array (list of lines): the bash
        // call is refused with the temp-file flow.
        kimix::string arguments = R"({"command":["line one","line two"])";
        for (int i = 0; i < 30; ++i) {
            arguments += ",\"filler line to push the length past the gate\"";
        }
        arguments += "]}";
        const kimix::string out = soul.execute_tool_call("bash", arguments, err);
        expect(out.find("Parameters appear to be in the wrong format") !=
               kimix::string::npos)
            << out;
        expect(out.find("Malformed parameter") == kimix::string::npos)
            << "brief is UI-only, never in the model message: " << out;
                  // The error out-param carries the message (the brief stays in the
          // typed-brief channel the host consumes).
          expect(err.find("Parameters appear to be in the wrong format") !=
                 kimix::string::npos)
              << err;
    };

    "soul_dispatch_coerces_a_stringified_todos_list"_test = [] {
        kimix::agent::AgentSession session(
            tmp_workspace("kimix_test_arg_repair_ws4"));
        FakeBackend backend;
        kimix::agent::KimiSoul soul(session, backend);
        kimix::string err;
        // "todos" arrives as a JSON-encoded STRING: the schema-driven repair
        // parses it before the todo tool validates it.
        const kimix::string out = soul.execute_tool_call(
            "todo_write",
            R"({"todos":"[{\"title\":\"first\",\"status\":\"pending\"}]"})",
            err);
        expect(out.find("first") != kimix::string::npos) << out;
        expect(out.find("Invalid arguments") == kimix::string::npos) << out;
    };

    return 0;
}
