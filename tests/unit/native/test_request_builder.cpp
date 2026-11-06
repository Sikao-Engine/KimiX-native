// Test for src/runtime/soul/request_builder.{h,cpp} — the provider request
// builder over the arena (report candidate B1).
//
// Covers the pipeline stages and pins the per-provider envelope contract from
// request_builder.h. The native output is also compared byte-for-byte with the
// shim's `_compat_build_request` from pytest; here the assertions are structural
// (parse the emitted JSON) plus a few exact-string checks on the stable parts.

#include "ut/ut.hpp"

#include <runtime/soul/kmb2.h>
#include <runtime/soul/message_view.h>
#include <runtime/soul/request_builder.h>

#include <yyjson.h>

#include "llm/yyjson_alc.h"

using namespace boost::ut;
using namespace boost::ut::literals;
using kimix::runtime::soul::message_view;
using kimix::runtime::soul::part_kind;
using kimix::runtime::soul::part_view;
using kimix::runtime::soul::provider_kind;
using kimix::runtime::soul::request_options;
using kimix::runtime::soul::tool_call_view;

namespace tests_unit_native_test_request_builder {

struct fixture {
    kimix::vector<kimix::vector<part_view>> parts;
    kimix::vector<kimix::vector<tool_call_view>> calls;
    kimix::vector<message_view> msgs;
    kimix::vector<kimix::string> tcid;
    kimix::vector<kimix::string> texts;

    // 0 user text, 1 user text (mergeable), 2 assistant text + tool call,
    // 3 tool result, 4 assistant text.
    fixture() {
        parts.resize(5);
        calls.resize(5);
        tcid.resize(5);
        texts.resize(5);
        msgs.resize(5);

        texts[0] = "hello";
        parts[0].push_back(part_view{part_kind::TEXT, kimix::string_view(texts[0])});
        texts[1] = "world";
        parts[1].push_back(part_view{part_kind::TEXT, kimix::string_view(texts[1])});
        texts[2] = "reading";
        parts[2].push_back(part_view{part_kind::TEXT, kimix::string_view(texts[2])});
        parts[2].push_back(part_view{part_kind::THINK, "deliberating"});
        calls[2].push_back(tool_call_view{"call_1", "read", "{\"path\":\"a\"}"});
        texts[3] = "file body";
        parts[3].push_back(part_view{part_kind::TEXT, kimix::string_view(texts[3])});
        tcid[3] = "call_1";
        texts[4] = "done";
        parts[4].push_back(part_view{part_kind::TEXT, kimix::string_view(texts[4])});

        const uint8_t roles[5] = {kimix::runtime::soul::kRoleUser,
                                  kimix::runtime::soul::kRoleUser,
                                  kimix::runtime::soul::kRoleAssistant,
                                  kimix::runtime::soul::kRoleTool,
                                  kimix::runtime::soul::kRoleAssistant};
        for (size_t i = 0; i < 5; ++i) {
            msgs[i].role = roles[i];
            msgs[i].parts = kimix::span<const part_view>(parts[i].data(), parts[i].size());
            msgs[i].tool_calls =
                kimix::span<const tool_call_view>(calls[i].data(), calls[i].size());
            if (!tcid[i].empty()) {
                msgs[i].tool_call_id = kimix::string_view(tcid[i]);
            }
        }
    }
};

// Parse a body and answer questions about it without leaving a document behind.
struct parsed {
    yyjson_doc* doc = nullptr;

    explicit parsed(kimix::string_view body) {
        doc = yyjson_read_opts(const_cast<char*>(body.data()), body.size(), 0,
                               &kimix::llm::kYYJsonAlcMi, nullptr);
    }
    ~parsed() {
        if (doc != nullptr) {
            yyjson_doc_free(doc);
        }
    }
    yyjson_val* root() const { return doc == nullptr ? nullptr : yyjson_doc_get_root(doc); }
    yyjson_val* get(const char* key) const {
        return root() == nullptr ? nullptr : yyjson_obj_get(root(), key);
    }
    size_t array_size(const char* key) const {
        yyjson_val* v = get(key);
        return v != nullptr && yyjson_is_arr(v) ? yyjson_arr_size(v) : 0u;
    }
    kimix::string str(const char* key) const {
        yyjson_val* v = get(key);
        if (v == nullptr || !yyjson_is_str(v)) {
            return kimix::string();
        }
        return kimix::string(yyjson_get_str(v), yyjson_get_len(v));
    }
};

kimix::string make_body(const fixture& f, provider_kind kind) {
    request_options opts;
    opts.provider = kind;
    opts.model = "test-model";
    opts.max_tokens = 128;
    opts.temperature = 0.25;
    opts.has_temperature = true;
    opts.system_prompt = "be helpful";
    opts.reasoning_key = "reasoning_content";
    kimix::string body;
    expect(kimix::runtime::soul::build_request(f.msgs, opts, body));
    return body;
}

} // namespace tests_unit_native_test_request_builder
using namespace tests_unit_native_test_request_builder;

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "request_provider_names"_test = [] {
        provider_kind kind;
        expect(kimix::runtime::soul::parse_provider("openai_legacy", kind));
        expect(kind == provider_kind::openai_legacy);
        expect(kimix::runtime::soul::parse_provider("openai", kind));
        expect(kind == provider_kind::openai_legacy);
        expect(kimix::runtime::soul::parse_provider("openai_responses", kind));
        expect(kind == provider_kind::openai_responses);
        expect(kimix::runtime::soul::parse_provider("responses", kind));
        expect(kind == provider_kind::openai_responses);
        expect(kimix::runtime::soul::parse_provider("anthropic", kind));
        expect(kind == provider_kind::anthropic);
        expect(kimix::runtime::soul::parse_provider("kimi", kind));
        expect(kind == provider_kind::kimi);
        expect(!kimix::runtime::soul::parse_provider("gemini", kind));
        expect(kimix::runtime::soul::provider_name(provider_kind::kimi) == "kimi");
    };

    "request_normalize_merges_users"_test = [] {
        fixture f;
        kimix::string structure, history;
        kimix::runtime::soul::normalize_history(f.msgs, structure, history);
        kimix::runtime::soul::kmb2_view view;
        expect(kimix::runtime::soul::kmb2_parse(structure, history, view));
        // 5 messages, two adjacent users -> 4.
        expect(view.msg_count == 4u);
        kimix::runtime::soul::kmb2_message_record first;
        expect(view.message(0, first));
        expect(first.part_count == 2u); // "hello" + "world"
    };

    "request_normalize_keeps_reminders"_test = [] {
        kimix::vector<part_view> reminder_parts;
        kimix::vector<part_view> user_parts;
        kimix::vector<kimix::string> owned;
        owned.push_back("<system-reminder>stay</system-reminder>");
        owned.push_back("real question");
        reminder_parts.push_back(part_view{part_kind::TEXT, kimix::string_view(owned[0])});
        user_parts.push_back(part_view{part_kind::TEXT, kimix::string_view(owned[1])});
        message_view a, b;
        a.role = kimix::runtime::soul::kRoleUser;
        a.parts = kimix::span<const part_view>(reminder_parts.data(), reminder_parts.size());
        b.role = kimix::runtime::soul::kRoleUser;
        b.parts = kimix::span<const part_view>(user_parts.data(), user_parts.size());
        message_view input[2] = {a, b};
        kimix::string structure, history;
        kimix::runtime::soul::normalize_history(kimix::span<const message_view>(input, 2),
                                               structure, history);
        kimix::runtime::soul::kmb2_view view;
        expect(kimix::runtime::soul::kmb2_parse(structure, history, view));
        expect(view.msg_count == 2u); // never merged
    };

    "request_repair_inserts_placeholder"_test = [] {
        fixture f;
        // Drop the tool result: the assistant tool call has no pairing.
        kimix::vector<message_view> input = f.msgs;
        input.erase(input.begin() + 3);
        kimix::string structure, history;
        kimix::runtime::soul::repair_tool_pairing(input, structure, history);
        kimix::runtime::soul::kmb2_view view;
        expect(kimix::runtime::soul::kmb2_parse(structure, history, view));
        expect(view.msg_count == 5u); // 4 + the synthetic result
        kimix::runtime::soul::kmb2_message_record rec;
        expect(view.message(3, rec));
        expect(rec.role == kimix::runtime::soul::kRoleTool);
        expect((rec.flags & kimix::runtime::soul::kKmb2MsgHasToolCallId) != 0);
        kimix::runtime::soul::kmb2_part_record part;
        expect(view.part(rec.part_first, part));
        const kimix::string_view text = view.span(part.start, part.end);
        expect(text.find("Tool result missing") != kimix::string_view::npos);
        expect(text.find("read") != kimix::string_view::npos);
    };

    "request_repair_drops_orphans"_test = [] {
        fixture f;
        // Two tool results for one call: the duplicate is dropped.
        kimix::vector<message_view> input = f.msgs;
        input.push_back(f.msgs[3]);
        kimix::string structure, history;
        kimix::runtime::soul::repair_tool_pairing(input, structure, history);
        kimix::runtime::soul::kmb2_view view;
        expect(kimix::runtime::soul::kmb2_parse(structure, history, view));
        expect(view.msg_count == 5u);

        // An orphan-only history is left untouched (nothing to reclaim).
        kimix::vector<message_view> orphan = {f.msgs[3]};
        kimix::runtime::soul::repair_tool_pairing(orphan, structure, history);
        expect(kimix::runtime::soul::kmb2_parse(structure, history, view));
        expect(view.msg_count == 0u); // the orphan has no claiming call
    };

    "request_openai_envelope"_test = [] {
        fixture f;
        const kimix::string body = make_body(f, provider_kind::openai_legacy);
        parsed p(body);
        expect(p.root() != nullptr);
        expect(p.str("model") == "test-model");
        expect(p.get("stream") != nullptr && yyjson_get_bool(p.get("stream")));
        expect(p.get("stream_options") != nullptr);
        expect(p.get("max_tokens") != nullptr);
        expect(yyjson_get_int(p.get("max_tokens")) == 128);
        expect(p.get("temperature") != nullptr);
        // The two leading user messages merged: 3 + assistant = 4.
        expect(p.array_size("messages") == 4u);

        yyjson_val* messages = p.get("messages");
        yyjson_val* first = yyjson_arr_get_first(messages);
        expect(kimix::string_view(yyjson_get_str(yyjson_obj_get(first, "role"))) == "user");
        yyjson_val* content = yyjson_obj_get(first, "content");
        expect(yyjson_is_arr(content));
        expect(yyjson_arr_size(content) == 2u);

        // The assistant tool call keeps the OpenAI shape (messages[1]: the two
        // leading user messages merged, so the assistant is the second entry).
        yyjson_val* assistant = yyjson_arr_get(messages, 1);
        yyjson_val* calls = yyjson_obj_get(assistant, "tool_calls");
        expect(calls != nullptr && yyjson_is_arr(calls));
        yyjson_val* call = yyjson_arr_get_first(calls);
        expect(kimix::string_view(yyjson_get_str(yyjson_obj_get(call, "id"))) == "call_1");
        expect(kimix::string_view(yyjson_get_str(yyjson_obj_get(call, "type"))) ==
               "function");

        // The tool result carries content + tool_call_id.
        yyjson_val* tool = yyjson_arr_get(messages, 2);
        expect(kimix::string_view(yyjson_get_str(yyjson_obj_get(tool, "role"))) == "tool");
        expect(kimix::string_view(
                   yyjson_get_str(yyjson_obj_get(tool, "tool_call_id"))) == "call_1");
    };

    "request_kimi_uses_completion_tokens"_test = [] {
        fixture f;
        const kimix::string body = make_body(f, provider_kind::kimi);
        parsed p(body);
        expect(p.get("max_completion_tokens") != nullptr);
        expect(p.get("max_tokens") == nullptr);
    };

    "request_anthropic_envelope"_test = [] {
        fixture f;
        const kimix::string body = make_body(f, provider_kind::anthropic);
        parsed p(body);
        expect(p.root() != nullptr);
        expect(p.str("system") == "be helpful");
        expect(p.get("max_tokens") != nullptr);
        expect(p.array_size("messages") == 4u);

        yyjson_val* messages = p.get("messages");
        yyjson_val* assistant = yyjson_arr_get(messages, 1);
        yyjson_val* content = yyjson_obj_get(assistant, "content");
        expect(yyjson_is_arr(content));
        // text + tool_use
        expect(yyjson_arr_size(content) == 2u);
        yyjson_val* tool_use = yyjson_arr_get(content, 1);
        expect(kimix::string_view(yyjson_get_str(yyjson_obj_get(tool_use, "type"))) ==
               "tool_use");
        expect(kimix::string_view(yyjson_get_str(yyjson_obj_get(tool_use, "name"))) ==
               "read");
        // input is an OBJECT (parsed arguments), never a string.
        expect(yyjson_is_obj(yyjson_obj_get(tool_use, "input")));

        // Tool result -> tool_result block.
        yyjson_val* tool = yyjson_arr_get(messages, 2);
        yyjson_val* tool_content = yyjson_obj_get(tool, "content");
        yyjson_val* block = yyjson_arr_get_first(tool_content);
        expect(kimix::string_view(yyjson_get_str(yyjson_obj_get(block, "type"))) ==
               "tool_result");
        expect(kimix::string_view(
                   yyjson_get_str(yyjson_obj_get(block, "tool_use_id"))) == "call_1");

        // Think parts are dropped unless preserve_thinking is set.
        request_options opts;
        opts.provider = provider_kind::anthropic;
        opts.model = "m";
        opts.max_tokens = 8;
        kimix::string mini;
        expect(kimix::runtime::soul::build_request(f.msgs, opts, mini));
        expect(mini.find("thinking") == kimix::string::npos);
        opts.preserve_thinking = true;
        expect(kimix::runtime::soul::build_request(f.msgs, opts, mini));
        expect(mini.find("\"type\":\"thinking\"") != kimix::string::npos);
    };

    "request_responses_envelope"_test = [] {
        fixture f;
        const kimix::string body = make_body(f, provider_kind::openai_responses);
        parsed p(body);
        expect(p.root() != nullptr);
        expect(p.str("instructions") == "be helpful");
        expect(p.get("max_output_tokens") != nullptr);
        expect(p.array_size("input") >= 4u);
        expect(body.find("\"function_call\"") != kimix::string::npos);
        expect(body.find("\"function_call_output\"") != kimix::string::npos);
        expect(body.find("\"input_text\"") != kimix::string::npos);
        expect(body.find("\"output_text\"") != kimix::string::npos);
    };

    "request_tools_fragment"_test = [] {
        fixture f;
        request_options opts;
        opts.provider = provider_kind::openai_legacy;
        opts.model = "m";
        opts.tools_json = "[{\"type\":\"function\",\"function\":{\"name\":\"read\"}}]";
        opts.tool_choice = "\"auto\"";
        opts.reasoning_effort = "high";
        kimix::string body;
        expect(kimix::runtime::soul::build_request(f.msgs, opts, body));
        parsed p(body);
        expect(p.array_size("tools") == 1u);
        yyjson_val* choice = p.get("tool_choice");
        expect(choice != nullptr && yyjson_is_str(choice));
        expect(p.str("reasoning_effort") == "high");

        // An unparseable tools fragment degrades to null instead of a broken body.
        opts.tools_json = "not json";
        expect(kimix::runtime::soul::build_request(f.msgs, opts, body));
        parsed q(body);
        expect(q.get("tools") != nullptr && yyjson_is_null(q.get("tools")));
    };

    "request_empty_history"_test = [] {
        request_options opts;
        opts.provider = provider_kind::openai_legacy;
        opts.model = "m";
        kimix::string body;
        expect(kimix::runtime::soul::build_request(
            kimix::span<const message_view>(), opts, body));
        parsed p(body);
        expect(p.root() != nullptr);
        expect(p.array_size("messages") == 0u);
    };

    "bench_request_750"_test = [] {
        fixture f;
        kimix::vector<message_view> many;
        many.reserve(750);
        for (int i = 0; i < 150; ++i) {
            for (const message_view& mv : f.msgs) {
                many.push_back(mv);
            }
        }
        expect(many.size() == 750u);
        kimix::string body;
        request_options opts;
        opts.provider = provider_kind::openai_legacy;
        opts.model = "m";
        opts.max_tokens = 4096;
        for (int i = 0; i < 10; ++i) {
            expect(kimix::runtime::soul::build_request(many, opts, body));
        }
        expect(body.size() > 1000u);
    };

    return 0;
}
