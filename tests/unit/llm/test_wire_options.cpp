// Test for the provider wire options ported from the reference providers:
//   * E5 - Anthropic prompt caching (cache_control = {"type":"ephemeral"}):
//     the system text block, the LAST content block of the serialized
//     conversation (never a thinking block, never string content) and the LAST
//     tool definition; toggleable via Config.anthropic_cache_control (the
//     reference anthropic provider applies it unconditionally, so the default
//     is on).
//   * E6 - output-token budget: OpenAI Chat sends `max_completion_tokens`
//     (thinking on) / `max_tokens` (thinking off) like kimi_cli/llm.py's
//     openai_legacy branch; Responses keeps `max_output_tokens`; Anthropic
//     always sends `max_tokens`.
//   * E7 - thinking control: disabled -> Anthropic sends NO thinking object,
//     OpenAI Chat sends thinking {"type":"disabled"} + the "no_think" effort
//     strings and NO top-level reasoning_effort, Responses sends NO reasoning
//     object; enabled -> the configured effort ranks are still sent.
//   * E11 - sampling controls: temperature/top_p reach the OpenAI Chat and
//     Anthropic bodies only when configured (0 == unset; the Responses API has
//     no such parameter in the reference).
//
// No network access: only the request-body builders are exercised.

#include "ut/ut.hpp"

#include "llm/llm.h"

#include "yyjson.h"

#include "llm/yyjson_alc.h"

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using namespace kimix::llm;

namespace {

bool contains(const kimix::string &hay, const char *needle) {
    return hay.find(needle) != kimix::string::npos;
}

yyjson_val *root_of(const kimix::string &body, yyjson_doc **out_doc) {
    *out_doc = yyjson_read_opts((char *)body.data(), body.size(), 0,
                                &kYYJsonAlcMi, nullptr);
    if (*out_doc == nullptr) {
        return nullptr;
    }
    return yyjson_doc_get_root(*out_doc);
}

// Path accessors: get(v, "messages", at{0}, "content", at{1}, "id") walks
// objects by key and arrays by index (at{} avoids int/size_t ambiguity).
struct at {
    size_t i;
};
yyjson_val *get_one(yyjson_val *v, at i) {
    return yyjson_is_arr(v) ? yyjson_arr_get(v, i.i) : nullptr;
}
yyjson_val *get_one(yyjson_val *v, const char *k) {
    return yyjson_is_obj(v) ? yyjson_obj_get(v, k) : nullptr;
}
template <typename T, typename... Rest>
yyjson_val *get(yyjson_val *v, T first, Rest... rest) {
    if constexpr (sizeof...(Rest) == 0) {
        return get_one(v, first);
    } else {
        return get(get_one(v, first), rest...);
    }
}
yyjson_val *get(yyjson_val *v, size_t idx) {
    return yyjson_is_arr(v) ? yyjson_arr_get(v, idx) : nullptr;
}

kimix::string str(yyjson_val *v) {
    return (v != nullptr && yyjson_is_str(v))
               ? kimix::string(yyjson_get_str(v), yyjson_get_len(v))
               : kimix::string{};
}
int64_t num(yyjson_val *v) { return v != nullptr ? yyjson_get_int(v) : -1; }
bool has_key(yyjson_val *v, const char *k) { return get(v, k) != nullptr; }

Config make_config(const char *type) {
    Config cfg;
    cfg.type = type;
    cfg.model = "test-model";
    cfg.url = "http://127.0.0.1:9/v1";
    cfg.api_key = "k";
    return cfg;
}

kimix::vector<Message> simple_history() {
    kimix::vector<Message> messages;
    Message sys;
    sys.role = "system";
    sys.content = "be helpful";
    messages.push_back(std::move(sys));
    Message user;
    user.role = "user";
    user.content = "hello";
    messages.push_back(std::move(user));
    return messages;
}

kimix::vector<anthropic::Tool> two_tools() {
    kimix::vector<anthropic::Tool> tools;
    tools.push_back({"tool_a", "a", "{\"type\":\"object\"}"});
    tools.push_back({"tool_b", "b", "{\"type\":\"object\"}"});
    return tools;
}

} // namespace

int main() {
    // -- E5: anthropic cache_control placement --------------------------------
    "cache_control_on_system_last_block_and_last_tool"_test = [] {
        Config cfg = make_config("anthropic");
        kimix::vector<Message> messages = simple_history();
        Message assistant;
        assistant.role = "assistant";
        assistant.content = "the answer";
        ToolCall tc;
        tc.id = "call_1";
        tc.name = "tool_a";
        tc.arguments = "{}";
        assistant.tool_calls.push_back(std::move(tc));
        messages.push_back(std::move(assistant));
        Message tool;
        tool.role = "tool";
        tool.content = "result";
        tool.tool_call_id = "call_1";
        messages.push_back(std::move(tool));

        const kimix::string body =
            anthropic::build_messages_body(cfg, "sys", anthropic_wire_request(messages).messages,
                                           two_tools());
        // system block + last content block (tool_result) + last tool only.
        expect(contains(body,
                        "\"system\":[{\"type\":\"text\",\"text\":\"sys\","
                        "\"cache_control\":{\"type\":\"ephemeral\"}}]"));
        expect(contains(body,
                        "\"type\":\"tool_result\",\"tool_use_id\":\"call_1\","
                        "\"content\":\"result\",\"cache_control\":"
                        "{\"type\":\"ephemeral\"}}"));
        // The FIRST tool must NOT carry cache_control; the LAST one must.
        yyjson_doc *doc = nullptr;
        yyjson_val *root = root_of(body, &doc);
        expect(root != nullptr);
        expect(!has_key(get(root, "tools", at{0}), "cache_control"));
        expect(has_key(get(root, "tools", at{1}), "cache_control"));
        expect(str(get(root, "tools", at{1}, "cache_control", "type")) == "ephemeral");
        yyjson_doc_free(doc);
    };

    "cache_control_skips_trailing_thinking_block"_test = [] {
        Config cfg = make_config("anthropic");
        // Thinking block as the LAST block of the last message: the reference
        // leaves thinking blocks untagged.
        anthropic::ChatMessage m;
        m.role = "assistant";
        m.thinking = "hmm";
        m.thinking_signature = "sig";
        kimix::vector<anthropic::ChatMessage> messages;
        messages.push_back(std::move(m));
        const kimix::string body = anthropic::build_messages_body(cfg, "sys", messages, {});
        expect(contains(body, "\"system\":[{\"type\":\"text\""));
        // The thinking block has no cache_control.
        yyjson_doc *doc = nullptr;
        yyjson_val *root = root_of(body, &doc);
        expect(root != nullptr);
        expect(!has_key(get(root, "messages", at{0}, "content", at{0}), "cache_control"));
        yyjson_doc_free(doc);
    };

    "cache_control_skipped_for_string_content"_test = [] {
        Config cfg = make_config("anthropic");
        // A conversation ending in plain user text (a STRING content, not a
        // block list) gets no message-level cache_control - the reference's
        // isinstance(last_content, list) guard.
        kimix::vector<anthropic::ChatMessage> messages;
        anthropic::ChatMessage user;
        user.role = "user";
        user.text = "hello";
        messages.push_back(std::move(user));
        const kimix::string body = anthropic::build_messages_body(cfg, "", messages, {});
        expect(contains(body, "\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]"));
    };

    "cache_control_off_removes_all_markers"_test = [] {
        Config cfg = make_config("anthropic");
        cfg.anthropic_cache_control = false;
        kimix::vector<anthropic::ChatMessage> messages;
        anthropic::ChatMessage user;
        user.role = "user";
        user.text = "hello";
        messages.push_back(std::move(user));
        const kimix::string body =
            anthropic::build_messages_body(cfg, "sys", messages, two_tools());
        expect(!contains(body, "cache_control")) << "no cache_control anywhere";
        // The system prompt falls back to the plain-string form.
        expect(contains(body, "\"system\":\"sys\""));
    };

    // -- E6: output-token budget ----------------------------------------------
    "openai_sends_max_completion_tokens_when_thinking_on"_test = [] {
        Config cfg = make_config("openai_legacy");
        cfg.max_tokens = 8192;
        cfg.thinking_effort = "high";
        const kimix::string body =
            openai::build_chat_body(cfg, kimix::vector<openai::ChatMessage>{}, {});
        yyjson_doc *doc = nullptr;
        yyjson_val *root = root_of(body, &doc);
        expect(root != nullptr);
        expect(num(get(root, "max_completion_tokens")) == 8192);
        // The legacy key must NOT be sent alongside it.
        expect(!has_key(root, "max_tokens"));
        yyjson_doc_free(doc);
    };

    "openai_sends_max_tokens_when_thinking_off"_test = [] {
        Config cfg = make_config("openai_legacy");
        cfg.max_tokens = 8192;
        cfg.enable_thinking = false;
        const kimix::string body =
            openai::build_chat_body(cfg, kimix::vector<openai::ChatMessage>{}, {});
        yyjson_doc *doc = nullptr;
        yyjson_val *root = root_of(body, &doc);
        expect(root != nullptr);
        expect(num(get(root, "max_tokens")) == 8192);
        expect(!has_key(root, "max_completion_tokens"));
        yyjson_doc_free(doc);
    };

    "openai_omits_token_budget_when_zero"_test = [] {
        Config cfg = make_config("openai_legacy");
        cfg.max_tokens = 0; // "unset"
        const kimix::string body =
            openai::build_chat_body(cfg, kimix::vector<openai::ChatMessage>{}, {});
        yyjson_doc *doc = nullptr;
        yyjson_val *root = root_of(body, &doc);
        expect(root != nullptr);
        expect(!has_key(root, "max_tokens"));
        expect(!has_key(root, "max_completion_tokens"));
        yyjson_doc_free(doc);
    };

    "responses_keeps_max_output_tokens"_test = [] {
        Config cfg = make_config("openai_responses");
        cfg.max_tokens = 4096;
        const kimix::string body =
            openai_responses::build_responses_body(cfg, {}, {});
        expect(contains(body, "\"max_output_tokens\":4096"));
        expect(!contains(body, "\"max_tokens\""));
    };

    "anthropic_always_sends_max_tokens"_test = [] {
        Config cfg = make_config("anthropic");
        cfg.max_tokens = 1234;
        const kimix::string body =
            anthropic::build_messages_body(cfg, "", {}, {});
        expect(contains(body, "\"max_tokens\":1234"));
    };

    // -- E7: thinking control --------------------------------------------------
    "anthropic_thinking_off_omits_thinking_object"_test = [] {
        Config cfg = make_config("anthropic");
        cfg.thinking_effort = "high";
        kimix::vector<anthropic::ChatMessage> on_messages;
        const kimix::string on = anthropic::build_messages_body(cfg, "", on_messages, {});
        expect(contains(on, "\"thinking\":{\"type\":\"enabled\""));
        expect(contains(on, "\"budget_tokens\":32000")); // high -> 32000

        cfg.enable_thinking = false;
        const kimix::string off = anthropic::build_messages_body(cfg, "", on_messages, {});
        expect(!contains(off, "\"thinking\"")) << "no thinking parameters at all";
        expect(!contains(off, "budget_tokens"));
    };

    "anthropic_thinking_off_via_effort_off"_test = [] {
        Config cfg = make_config("anthropic");
        cfg.thinking_effort = "off"; // LEGAL_THINKING_EFFORT includes "off"
        const kimix::string body = anthropic::build_messages_body(cfg, "", {}, {});
        expect(!contains(body, "\"thinking\""));
    };

    "anthropic_thinking_effort_ranks_map_to_budgets"_test = [] {
        Config cfg = make_config("anthropic");
        const char *ranks[] = {"low", "medium", "high", "xhigh", "max"};
        const int64_t budgets[] = {1024, 4096, 32000, 64000, 128000};
        for (size_t i = 0; i < 5; ++i) {
            cfg.thinking_effort = ranks[i];
            cfg.enable_thinking = true;
            const kimix::string body = anthropic::build_messages_body(cfg, "", {}, {});
            const kimix::string expect_key =
                kimix::format("\"budget_tokens\":{}", budgets[i]);
            expect(contains(body, expect_key.c_str()))
                << "budget for rank " << ranks[i];
        }
    };

    "openai_thinking_off_sends_disabled_and_no_reasoning_effort"_test = [] {
        Config cfg = make_config("openai_legacy");
        cfg.thinking_effort = "high";
        kimix::vector<openai::ChatMessage> empty;
        const kimix::string on = openai::build_chat_body(cfg, empty, {});
        expect(contains(on, "\"thinking\":{\"type\":\"enabled\"}"));
        expect(contains(on, "\"reasoning\":{\"effort\":\"high\"}"));
        expect(contains(on, "\"reasoning_effort\":\"high\""));

        cfg.enable_thinking = false;
        const kimix::string off = openai::build_chat_body(cfg, empty, {});
        // The reference's disable shape: thinking {"type":"disabled"} plus the
        // "no_think" soft switch in the effort strings; the top-level
        // reasoning_effort request parameter disappears.
        expect(contains(off, "\"thinking\":{\"type\":\"disabled\"}"));
        expect(contains(off, "\"reasoning\":{\"effort\":\"no_think\"}"));
        expect(contains(off, "\"chat_template_kwargs\":{\"reasoning_effort\":\"no_think\"}"));
        expect(!contains(off, "\"reasoning_effort\":\"high\""));
        expect(!contains(off, "\"type\":\"enabled\""));
    };

    "responses_thinking_off_omits_reasoning_object"_test = [] {
        Config cfg = make_config("openai_responses");
        cfg.thinking_effort = "high";
        const kimix::string on = openai_responses::build_responses_body(cfg, {}, {});
        expect(contains(on, "\"reasoning\":{\"effort\":\"high\",\"summary\":\"auto\"}"));

        cfg.enable_thinking = false;
        const kimix::string off = openai_responses::build_responses_body(cfg, {}, {});
        expect(!contains(off, "\"reasoning\"")) << "no reasoning requested";
    };

    // -- E11: sampling controls ------------------------------------------------
    "openai_sampling_controls_only_when_set"_test = [] {
        Config cfg = make_config("openai_legacy");
        kimix::vector<openai::ChatMessage> empty;
        const kimix::string unset = openai::build_chat_body(cfg, empty, {});
        expect(!contains(unset, "\"temperature\""));
        expect(!contains(unset, "\"top_p\""));

        cfg.temperature = 0.2;
        cfg.top_p = 0.8;
        const kimix::string set = openai::build_chat_body(cfg, empty, {});
        expect(contains(set, "\"temperature\":0.2"));
        expect(contains(set, "\"top_p\":0.8"));
    };

    "anthropic_sampling_controls_only_when_set"_test = [] {
        Config cfg = make_config("anthropic");
        const kimix::string unset = anthropic::build_messages_body(cfg, "", {}, {});
        expect(!contains(unset, "\"temperature\""));
        expect(!contains(unset, "\"top_p\""));

        cfg.temperature = 0.5;
        const kimix::string set = anthropic::build_messages_body(cfg, "", {}, {});
        expect(contains(set, "\"temperature\":0.5"));
        expect(!contains(set, "\"top_p\""));
    };

    "responses_has_no_sampling_controls"_test = [] {
        // The reference openai_responses branch sends neither key.
        Config cfg = make_config("openai_responses");
        cfg.temperature = 0.5;
        cfg.top_p = 0.9;
        const kimix::string body = openai_responses::build_responses_body(cfg, {}, {});
        expect(!contains(body, "\"temperature\""));
        expect(!contains(body, "\"top_p\""));
    };

    return 0;
}
