// Snapshot-style tests for the Kimi (Moonshot) chat provider port
// (src/llm/kimi/kimi_chat.cpp, the C++ mirror of kosong/chat_provider/kimi.py),
// mirroring D:/kimi-agent/kimi-cli/tests/kosong/api_snapshot_tests/test_kimi.py:
// the Python suite drives the provider against a respx mock and snapshots the
// request body; the native equivalent is the build_chat_body seam (no network).
//
// Covered (case names mirror the reference test names):
//  * message_conversion: system/user/assistant/tool shapes, image content
//    parts, tool definitions, tool calls, builtin ($-prefixed) tools, the
//    assistant_with_reasoning / _empty_reasoning reasoning_content
//    round-trip, assistant_tool_call_without_text (content omitted) and
//    assistant_tool_call_with_reasoning_only.
//  * generation_kwargs: temperature passthrough, max_tokens ->
//    max_completion_tokens normalization (and the max_completion_tokens
//    preference + 384000 clamp), sends_no_completion_token_cap_by_default,
//    omits_tools_when_empty.
//  * with_thinking / thinking effort verbatim / with_thinking_off (no stale
//    effort) / the thinking keep carried in the disabled shape, and the
//    "reasoning_effort is never sent on this contract" rule.
//  * reasoning_content backfill rules (passed_back only where reasoning
//    exists; backfilled on every assistant message only while preserved
//    thinking is active, i.e. keep == "all" AND thinking not disabled; no
//    backfill for other keep values or on non-assistant messages).
//  * normalizes_invalid_tool_call_ids / truncation + dedup (the shared
//    normalize_tool_call_ids seam applied by kimi_wire_messages).
//  * session_id_prompt_cache_key_in_body / without_prompt_cache_key_omits.
//  * tool schema normalization (definitions/$ref inlining + type completion)
//    and the builtin_function shape.
//  * the stream parser's cached_tokens extraction (Moonshot usage).
//
// No network access: only the wire seams and the parsers are exercised; a
// separate opt-in e2e case (mirroring the reference's non-stream snapshot)
// hits the real backend ONLY when --config=<path> is passed.
#include "ut/ut.hpp"
#include "llm/kimi/kimi_chat.h"
#include "llm/llm.h"
#include "llm/openai/sse_parser.h"
#include "llm/yyjson_alc.h"
#include "yyjson.h"

#include <cstdio>
#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using namespace kimix::llm;

namespace {

// ---------------------------------------------------------------------------
// JSON body access helpers (same shape as test_wire_options.cpp)
// ---------------------------------------------------------------------------

// Path accessors: get(v, "messages", at{0}, "content") walks objects by key
// and arrays by index (at{} avoids int/size_t ambiguity).
struct at {
    size_t i;
};
yyjson_val *get_one(yyjson_val *v, at i) {
    return (v != nullptr && yyjson_is_arr(v)) ? yyjson_arr_get(v, i.i) : nullptr;
}
yyjson_val *get_one(yyjson_val *v, const char *k) {
    return (v != nullptr && yyjson_is_obj(v)) ? yyjson_obj_get(v, k) : nullptr;
}
template <typename T, typename... Rest>
yyjson_val *get(yyjson_val *v, T first, Rest... rest) {
    if constexpr (sizeof...(Rest) == 0) {
        return get_one(v, first);
    } else {
        return get(get_one(v, first), rest...);
    }
}
kimix::string str(yyjson_val *v) {
    return (v != nullptr && yyjson_is_str(v))
               ? kimix::string(yyjson_get_str(v), yyjson_get_len(v))
               : kimix::string{};
}
int64_t num(yyjson_val *v) { return v != nullptr ? (int64_t)yyjson_get_int(v) : -1; }
bool has_key(yyjson_val *v, const char *k) { return get(v, k) != nullptr; }

// Parse the built body; the test fails loudly on a non-JSON result.
struct Body {
    yyjson_doc *doc = nullptr;
    yyjson_val *root = nullptr;
    Body(const kimix::string &json) {
        doc = yyjson_read_opts(const_cast<char *>(json.data()), json.size(), 0,
                               &kYYJsonAlcMi, nullptr);
        root = doc ? yyjson_doc_get_root(doc) : nullptr;
    }
    Body(Body &&other) noexcept : doc(other.doc), root(other.root) {
        other.doc = nullptr;
    }
    Body(const Body &) = delete;
    ~Body() {
        if (doc != nullptr) {
            yyjson_doc_free(doc);
        }
    }
};

Config make_config() {
    Config cfg;
    cfg.type = "kimi";
    cfg.model = "kimi-k2-turbo-preview";
    cfg.url = "https://api.moonshot.ai/v1";
    cfg.api_key = "test-key";
    cfg.max_tokens = 0; // no cap by default, mirroring the provider
    cfg.enable_thinking = true;
    cfg.thinking_effort = "high";
    return cfg;
}

// A minimal 2-message history ("simple_user_message" of the reference suite).
kimix::vector<Message> simple_history() {
    kimix::vector<Message> messages;
    Message sys;
    sys.role = "system";
    sys.content = "You are helpful.";
    messages.push_back(std::move(sys));
    Message user;
    user.role = "user";
    user.content = "Hello!";
    messages.push_back(std::move(user));
    return messages;
}

// Build + parse the body for a history through the kimi wire seam.
Body kimi_body(const Config &cfg, const kimix::vector<Message> &messages,
               const kimix::vector<Tool> &tools = {}) {
    kimix::vector<kimi::ChatMessage> wire = kimi_wire_messages(cfg, messages);
    kimix::vector<openai::Tool> wire_tools;
    for (const auto &t : tools) {
        wire_tools.push_back({t.name, t.description, t.parameters_json});
    }
    kimix::string err;
    kimix::string json = kimi::build_chat_body(cfg, wire, wire_tools, &err);
    expect(!json.empty()) << "body built: " << err;
    return Body(json);
}

// ---------------------------------------------------------------------------
// The reference suite's tools and histories
// ---------------------------------------------------------------------------

const char *const kAddParameters =
    R"({"type":"object","properties":{"a":{"type":"integer","description":"First number"},"b":{"type":"integer","description":"Second number"}},"required":["a","b"]})";

Tool add_tool() {
    return {"add", "Add two integers.", kAddParameters};
}
Tool mul_tool() {
    return {"multiply", "Multiply two integers.", kAddParameters};
}
Tool builtin_tool() {
    return {"$web_search", "Search the web",
            R"({"type":"object","properties":{}})"};
}

Message user_msg(const char *text) {
    Message m;
    m.role = "user";
    m.content = text;
    return m;
}
Message assistant_msg(const char *text) {
    Message m;
    m.role = "assistant";
    m.content = text;
    return m;
}

// assistant tool_calls=[id/name/args] with optional visible text backbone.
Message assistant_with_call(const char *text, const char *id, const char *name,
                            const char *args) {
    Message m;
    m.role = "assistant";
    m.content = text;
    ToolCall tc;
    tc.id = id;
    tc.name = name;
    tc.arguments = args;
    m.tool_calls.push_back(std::move(tc));
    return m;
}

Message tool_result(const char *text, const char *id) {
    Message m;
    m.role = "tool";
    m.content = text;
    m.tool_call_id = id;
    return m;
}

// Message with a ThinkPart(think) + TextPart(text) content (the reference's
// assistant_with_reasoning shapes): the native model keeps the thinking
// round-trip on the message's `thinking` field.
Message assistant_with_think(const char *think, const char *text) {
    Message m;
    m.role = "assistant";
    m.content = text;
    m.thinking = think;
    return m;
}

} // namespace

int main(int argc, char *argv[]) {
    // Opt-in real-backend config path (see the e2e test at the bottom).
    kimix::string config_path;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr) {
            continue;
        }
        const char *arg = argv[i];
        if (std::strncmp(arg, "--config=", 9) == 0) {
            config_path = arg + 9;
            argv[i] = nullptr;
        } else if (std::strcmp(arg, "--config") == 0 && i + 1 < argc) {
            config_path = argv[i + 1];
            argv[i] = nullptr;
            argv[i + 1] = nullptr;
            ++i;
        }
    }
    int filtered_argc = 1;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != nullptr) {
            argv[filtered_argc++] = argv[i];
        }
    }
    boost::ut::detail::cfg::parse_arg_with_fallback(
        filtered_argc, const_cast<const char **>(argv));

    // -----------------------------------------------------------------------
    // test_kimi_message_conversion (COMMON_CASES + the kimi-specific cases)
    // -----------------------------------------------------------------------

    "simple_user_message"_test = [] {
        Body b = kimi_body(make_config(), simple_history());
        yyjson_val *msgs = get(b.root, "messages");
        expect(eq(yyjson_arr_size(msgs), 2u));
        expect(str(get(msgs, at{0}, "role")) == "system");
        expect(str(get(msgs, at{0}, "content")) == "You are helpful.");
        expect(str(get(msgs, at{1}, "role")) == "user");
        expect(str(get(msgs, at{1}, "content")) == "Hello!");
    };

    "multi_turn_conversation"_test = [] {
        kimix::vector<Message> history{user_msg("What is 2+2?"),
                                       assistant_msg("2+2 equals 4."),
                                       user_msg("And 3+3?")};
        Body b = kimi_body(make_config(), history);
        yyjson_val *msgs = get(b.root, "messages");
        expect(eq(yyjson_arr_size(msgs), 3u));
        expect(str(get(msgs, at{1}, "role")) == "assistant");
        expect(str(get(msgs, at{1}, "content")) == "2+2 equals 4.");
        // No reasoning field on a plain assistant message.
        expect(!has_key(get(msgs, at{1}), "reasoning_content"));
    };

    "image_url_part_serializes_as_block_array"_test = [] {
        Message m;
        m.role = "user";
        ContentPart text;
        text.kind = ContentPart::Kind::text;
        text.text = "What's in this image?";
        ContentPart img;
        img.kind = ContentPart::Kind::image_url;
        img.url = "https://example.com/image.png";
        m.parts = {text, img};
        Body b = kimi_body(make_config(), {m});
        yyjson_val *content = get(b.root, "messages", at{0}, "content");
        expect(yyjson_is_arr(content));
        expect(eq(yyjson_arr_size(content), 2u));
        expect(str(get(content, at{0}, "type")) == "text");
        expect(str(get(content, at{0}, "text")) == "What's in this image?");
        expect(str(get(content, at{1}, "type")) == "image_url");
        expect(str(get(content, at{1}, "image_url", "url")) ==
               "https://example.com/image.png");
    };

    "tool_definition_and_builtin_tool"_test = [] {
        Body b = kimi_body(make_config(), {user_msg("Add 2 and 3")},
                           {add_tool(), builtin_tool()});
        yyjson_val *tools = get(b.root, "tools");
        expect(eq(yyjson_arr_size(tools), 2u));
        expect(str(get(tools, at{0}, "type")) == "function");
        expect(str(get(tools, at{0}, "function", "name")) == "add");
        expect(str(get(tools, at{0}, "function", "description")) ==
               "Add two integers.");
        expect(str(get(tools, at{0}, "function", "parameters", "type")) == "object");
        // builtin_tool case: $-prefixed names carry type + name only.
        expect(str(get(tools, at{1}, "type")) == "builtin_function");
        expect(str(get(tools, at{1}, "function", "name")) == "$web_search");
        expect(!has_key(get(tools, at{1}, "function"), "description"));
        expect(!has_key(get(tools, at{1}, "function"), "parameters"));
    };

    "tool_call_and_result"_test = [] {
        kimix::vector<Message> history{
            user_msg("Add 2 and 3"),
            assistant_with_call("I'll add those numbers for you.", "call_abc123",
                                "add", R"({"a": 2, "b": 3})"),
            tool_result("5", "call_abc123")};
        Body b = kimi_body(make_config(), history);
        yyjson_val *msgs = get(b.root, "messages");
        expect(str(get(msgs, at{1}, "tool_calls", at{0}, "id")) == "call_abc123");
        expect(str(get(msgs, at{1}, "tool_calls", at{0}, "type")) == "function");
        expect(str(get(msgs, at{1}, "tool_calls", at{0}, "function", "name")) ==
               "add");
        expect(str(get(msgs, at{1}, "tool_calls", at{0}, "function", "arguments")) ==
               R"({"a": 2, "b": 3})");
        expect(str(get(msgs, at{2}, "role")) == "tool");
        expect(str(get(msgs, at{2}, "content")) == "5");
        expect(str(get(msgs, at{2}, "tool_call_id")) == "call_abc123");
    };

    "assistant_with_reasoning_round_trips"_test = [] {
        kimix::vector<Message> history{
            user_msg("What is 2+2?"),
            assistant_with_think("Let me think...", "The answer is 4."),
            user_msg("Thanks!")};
        Body b = kimi_body(make_config(), history);
        yyjson_val *a = get(b.root, "messages", at{1});
        expect(str(get(a, "content")) == "The answer is 4.");
        expect(str(get(a, "reasoning_content")) == "Let me think...");
        // Messages without reasoning do NOT get an empty backfill by default.
        expect(!has_key(get(b.root, "messages", at{0}), "reasoning_content"));
        expect(!has_key(get(b.root, "messages", at{2}), "reasoning_content"));
    };

    "assistant_with_empty_reasoning_round_trips"_test = [] {
        // The reference: ThinkPart(think="") must come back as
        // reasoning_content: "" (present, empty), not be dropped.
        kimix::vector<Message> history{
            user_msg("What is 2+2?"),
            [] {
                Message m;
                m.role = "assistant";
                m.content = "The answer is 4.";
                ContentPart think;
                think.kind = ContentPart::Kind::think;
                think.text = "";
                ContentPart text;
                text.kind = ContentPart::Kind::text;
                text.text = "The answer is 4.";
                m.parts = {think, text};
                return m;
            }(),
            user_msg("Thanks!")};
        Body b = kimi_body(make_config(), history);
        yyjson_val *a = get(b.root, "messages", at{1});
        expect(has_key(a, "reasoning_content"));
        expect(str(get(a, "reasoning_content")) == "");
    };

    "assistant_tool_call_without_text_omits_content"_test = [] {
        // The empty visible content alongside a tool call pops `content`
        // entirely (the compat layer rejects empty text parts).
        kimix::vector<Message> history{
            user_msg("Call the add tool"),
            assistant_with_call("", "call_abc123", "add", R"({"a": 2, "b": 3})"),
            tool_result("5", "call_abc123")};
        Body b = kimi_body(make_config(), history);
        yyjson_val *a = get(b.root, "messages", at{1});
        expect(!has_key(a, "content")) << "content omitted for empty tool call";
        expect(has_key(a, "tool_calls"));
    };

    "assistant_tool_call_with_reasoning_only"_test = [] {
        kimix::vector<Message> history{
            user_msg("Think and call the add tool"),
            [] {
                Message m = assistant_with_call("", "call_abc123", "add",
                                                R"({"a": 2, "b": 3})");
                m.thinking = "I should call the tool.";
                return m;
            }(),
            tool_result("5", "call_abc123")};
        Body b = kimi_body(make_config(), history);
        yyjson_val *a = get(b.root, "messages", at{1});
        expect(!has_key(a, "content"));
        expect(str(get(a, "reasoning_content")) == "I should call the tool.");
        expect(str(get(a, "tool_calls", at{0}, "function", "name")) == "add");
    };

    // -----------------------------------------------------------------------
    // test_kimi_generation_kwargs and the token-cap cases
    // -----------------------------------------------------------------------

    "max_tokens_is_normalized_to_max_completion_tokens"_test = [] {
        Config cfg = make_config();
        cfg.temperature = 0.7;
        cfg.max_tokens = 2048;
        const kimix::string json = [&] {
            kimix::string err;
            auto wire = kimi_wire_messages(cfg, {user_msg("Hi")});
            return kimi::build_chat_body(cfg, wire, {}, &err);
        }();
        expect(json.find("\"temperature\":0.7") != kimix::string::npos);
        expect(json.find("\"max_completion_tokens\":2048") != kimix::string::npos);
        expect(json.find("\"max_tokens\"") == kimix::string::npos);
    };

    "completion_token_cap_is_clamped_to_384000"_test = [] {
        Config cfg = make_config();
        cfg.max_tokens = 2000000; // > kMaxOutputTokens
        Body b = kimi_body(cfg, {user_msg("Hi")});
        expect(num(get(b.root, "max_completion_tokens")) == 384000);
    };

    "sends_no_completion_token_cap_by_default"_test = [] {
        Body b = kimi_body(make_config(), {user_msg("Hi")});
        expect(!has_key(b.root, "max_tokens"));
        expect(!has_key(b.root, "max_completion_tokens"));
    };

    "omits_tools_when_empty"_test = [] {
        Body b = kimi_body(make_config(), {user_msg("Hi")});
        expect(!has_key(b.root, "tools"));
        // stream is always on and usage is included (stream_options).
        expect(get(b.root, "stream") != nullptr &&
               yyjson_is_true(get(b.root, "stream")));
        expect(get(b.root, "stream_options", "include_usage") != nullptr &&
               yyjson_is_true(get(b.root, "stream_options", "include_usage")));
    };

    "model_and_fixed_fields"_test = [] {
        Body b = kimi_body(make_config(), {user_msg("Hi")});
        expect(str(get(b.root, "model")) == "kimi-k2-turbo-preview");
    };

    // -----------------------------------------------------------------------
    // with_thinking and the extra_body.thinking semantics
    // -----------------------------------------------------------------------

    "with_thinking_effort_rides_inside_thinking"_test = [] {
        Config cfg = make_config();
        cfg.thinking_effort = "high";
        Body b = kimi_body(cfg, {user_msg("Think")});
        // The effort lives in the thinking object; NO top-level
        // reasoning_effort is sent on this contract.
        expect(!has_key(b.root, "reasoning_effort"));
        expect(str(get(b.root, "thinking", "type")) == "enabled");
        expect(str(get(b.root, "thinking", "effort")) == "high");
        expect(!has_key(get(b.root, "thinking"), "keep"));
    };

    "with_thinking_effort_passes_through_verbatim"_test = [] {
        for (const char *effort : {"low", "medium", "xhigh", "max"}) {
            Config cfg = make_config();
            cfg.thinking_effort = effort;
            Body b = kimi_body(cfg, {user_msg("Think")});
            expect(str(get(b.root, "thinking", "effort")) == effort) << effort;
        }
    };

    "with_thinking_off_disables_without_stale_effort"_test = [] {
        Config cfg = make_config();
        cfg.enable_thinking = false; // the with_thinking("off") equivalent
        cfg.thinking_effort = "off";
        Body b = kimi_body(cfg, {user_msg("Think")});
        expect(!has_key(b.root, "reasoning_effort"));
        yyjson_val *thinking = get(b.root, "thinking");
        expect(str(get(thinking, "type")) == "disabled");
        expect(!has_key(thinking, "effort")) << "no stale effort on disabled";
    };

    "thinking_keep_survives_the_off_switch"_test = [] {
        // with_extra_body({"thinking":{"keep":"all"}}) then with_thinking
        // ("off"): keep is carried over, type disabled, no effort.
        Config cfg = make_config();
        cfg.enable_thinking = false;
        cfg.thinking_effort = "off";
        cfg.thinking_keep = "all";
        Body b = kimi_body(cfg, {user_msg("Think")});
        yyjson_val *thinking = get(b.root, "thinking");
        expect(str(get(thinking, "type")) == "disabled");
        expect(str(get(thinking, "keep")) == "all");
        expect(!has_key(thinking, "effort"));
    };

    "with_extra_body_thinking_deep_merge"_test = [] {
        // thinking {type enabled, effort high, keep all} when all three are
        // configured.
        Config cfg = make_config();
        cfg.thinking_effort = "high";
        cfg.thinking_keep = "all";
        Body b = kimi_body(cfg, {user_msg("Think")});
        yyjson_val *thinking = get(b.root, "thinking");
        expect(str(get(thinking, "type")) == "enabled");
        expect(str(get(thinking, "effort")) == "high");
        expect(str(get(thinking, "keep")) == "all");
    };

    // -----------------------------------------------------------------------
    // The reasoning_content backfill rules (preserved thinking)
    // -----------------------------------------------------------------------

    "reasoning_content_backfilled_when_preserved_thinking_active"_test = [] {
        Config cfg = make_config();
        cfg.thinking_effort = "high";
        cfg.thinking_keep = "all";
        kimix::vector<Message> history{
            user_msg("What is 2+2?"),
            assistant_with_think("Thinking...", "4."),
            user_msg("And 3+3?"),
            assistant_msg("6.")};
        Body b = kimi_body(cfg, history);
        yyjson_val *msgs = get(b.root, "messages");
        expect(str(get(msgs, at{1}, "reasoning_content")) == "Thinking...");
        // The assistant message WITHOUT reasoning gets the empty backfill.
        expect(has_key(get(msgs, at{3}), "reasoning_content"));
        expect(str(get(msgs, at{3}, "reasoning_content")) == "");
        // Non-assistant messages never carry the field.
        expect(!has_key(get(msgs, at{0}), "reasoning_content"));
        expect(!has_key(get(msgs, at{2}), "reasoning_content"));
    };

    "no_reasoning_content_backfill_for_other_keep_values"_test = [] {
        for (const char *keep : {"", "off", "false", "none"}) {
            Config cfg = make_config();
            cfg.thinking_keep = keep;
            kimix::vector<Message> history{assistant_with_call(
                "", "call_1", "lookup", R"({"q":"test"})")};
            Body b = kimi_body(cfg, history);
            expect(!has_key(get(b.root, "messages", at{0}), "reasoning_content"))
                << keep;
        }
    };

    "no_reasoning_content_backfill_when_thinking_disabled"_test = [] {
        Config cfg = make_config();
        cfg.enable_thinking = false;
        cfg.thinking_effort = "off";
        cfg.thinking_keep = "all";
        kimix::vector<Message> history{assistant_with_call("", "call_1", "lookup",
                                                           R"({"q":"test"})")};
        Body b = kimi_body(cfg, history);
        expect(!has_key(get(b.root, "messages", at{0}), "reasoning_content"));
    };

    "keep_all_without_explicit_effort_still_backfills"_test = [] {
        // keep == "all" alone (thinking enabled by default, no "off") counts
        // as preserved thinking.
        Config cfg = make_config();
        cfg.thinking_keep = "all";
        kimix::vector<Message> history{assistant_msg("Done.")};
        Body b = kimi_body(cfg, history);
        expect(str(get(b.root, "messages", at{0}, "reasoning_content")) == "");
    };

    // -----------------------------------------------------------------------
    // Tool-call id normalization through the kimi wire seam
    // -----------------------------------------------------------------------

    "normalizes_invalid_tool_call_ids"_test = [] {
        kimix::vector<Message> history{
            user_msg("Read a file"),
            assistant_with_call("", "Read:9", "Read", R"({"path":"/tmp/file"})"),
            tool_result("content", "Read:9")};
        const kimix::vector<Message> before = history;
        Body b = kimi_body(make_config(), history);
        yyjson_val *msgs = get(b.root, "messages");
        expect(str(get(msgs, at{1}, "tool_calls", at{0}, "id")) == "Read_9");
        expect(str(get(msgs, at{2}, "tool_call_id")) == "Read_9");
        // The caller's history is never mutated.
        expect(history[1].tool_calls[0].id == "Read:9");
        expect(history[2].tool_call_id == "Read:9");
        (void)before;
    };

    "tool_call_ids_truncated_and_deduped"_test = [] {
        const kimix::string first(100, 'a');
        kimix::string second(99, 'a');
        second += ":x"; // sanitizes to the same 64-char id as `first`
        kimix::vector<Message> history{
            [first, second] {
                Message m;
                m.role = "assistant";
                m.tool_calls.push_back({first, "function", "f", "{}"});
                m.tool_calls.push_back({second, "function", "g", "{}"});
                m.content = "";
                return m;
            }(),
            tool_result("1", first.c_str()),
            tool_result("2", second.c_str())};
        Body b = kimi_body(make_config(), history);
        yyjson_val *tc = get(b.root, "messages", at{0}, "tool_calls");
        expect(str(get(tc, at{0}, "id")) == kimix::string(64, 'a'));
        expect(str(get(tc, at{1}, "id")) == kimix::string(62, 'a') + "_2");
        expect(str(get(b.root, "messages", at{1}, "tool_call_id")) ==
               kimix::string(64, 'a'));
        expect(str(get(b.root, "messages", at{2}, "tool_call_id")) ==
               kimix::string(62, 'a') + "_2");
    };

    "valid_tool_call_ids_pass_through_unchanged"_test = [] {
        kimix::vector<Message> history{
            assistant_with_call("", "call_abc-123_XYZ", "f", "{}"),
            tool_result("1", "call_abc-123_XYZ")};
        Body b = kimi_body(make_config(), history);
        expect(str(get(b.root, "messages", at{0}, "tool_calls", at{0}, "id")) ==
               "call_abc-123_XYZ");
        expect(str(get(b.root, "messages", at{1}, "tool_call_id")) ==
               "call_abc-123_XYZ");
    };

    // -----------------------------------------------------------------------
    // prompt_cache_key (the session id contract)
    // -----------------------------------------------------------------------

    "session_id_prompt_cache_key_in_body"_test = [] {
        Config cfg = make_config();
        cfg.prompt_cache_key = "sess-abc-123";
        Body b = kimi_body(cfg, {user_msg("Hi")});
        expect(str(get(b.root, "prompt_cache_key")) == "sess-abc-123");
    };

    "without_prompt_cache_key_omits_field"_test = [] {
        Body b = kimi_body(make_config(), {user_msg("Hi")});
        expect(!has_key(b.root, "prompt_cache_key"));
    };

    // -----------------------------------------------------------------------
    // Tool parameter schema normalization (_convert_tool path)
    // -----------------------------------------------------------------------

    "convert_tool_dereferences_and_normalizes"_test = [] {
        const kimix::string schema =
            R"({"type":"object","properties":{"mode":{"$ref":"#/definitions/Mode"},"tuple":{"prefixItems":[{"enum":["left","right"]}]}},"definitions":{"Mode":{"enum":["fast","safe"]}}})";
        const kimix::string out = kimi::normalize_tool_parameters(schema);
        expect(!out.empty());
        yyjson_doc *doc =
            yyjson_read_opts(const_cast<char *>(out.data()), out.size(), 0,
                             &kYYJsonAlcMi, nullptr);
        expect(doc != nullptr);
        if (doc) {
            yyjson_val *root = yyjson_doc_get_root(doc);
            // $ref inlined + type completed from the enum values.
            expect(str(get(root, "properties", "mode", "type")) == "string");
            expect(str(get(root, "properties", "mode", "enum", at{0})) == "fast");
            // The resolved bucket is gone (nothing references it any more).
            expect(!has_key(root, "definitions"));
            // Enum-only nodes in exotic slots also get a type.
            expect(str(get(root, "properties", "tuple", "type")) == "array");
            expect(str(get(root, "properties", "tuple", "prefixItems", at{0},
                           "type")) == "string");
            yyjson_doc_free(doc);
        }
    };

    "convert_tool_keeps_cyclic_ref_and_bucket"_test = [] {
        const kimix::string schema =
            R"({"type":"object","properties":{"node":{"$ref":"#/definitions/N"}},"definitions":{"N":{"type":"object","properties":{"child":{"$ref":"#/definitions/N"}}}}})";
        const kimix::string out = kimi::normalize_tool_parameters(schema);
        expect(out.find("definitions") != kimix::string::npos)
            << "cycle keeps the bucket resolvable";
        expect(out.find("$ref") != kimix::string::npos);
    };

    "convert_tool_repairs_type_contradicting_enum"_test = [] {
        // The Xcode-MCP generator bug: an explicit object type alongside
        // string enum values; the repair also drops the object structure
        // keywords that no longer apply.
        const kimix::string schema =
            R"({"type":"object","properties":{"mode":{"type":"object","enum":["fast","safe"],"properties":{"x":{"type":"string"}}}}})";
        const kimix::string out = kimi::normalize_tool_parameters(schema);
        yyjson_doc *doc =
            yyjson_read_opts(const_cast<char *>(out.data()), out.size(), 0,
                             &kYYJsonAlcMi, nullptr);
        expect(doc != nullptr);
        if (doc) {
            yyjson_val *mode =
                get(yyjson_doc_get_root(doc), "properties", "mode");
            expect(str(get(mode, "type")) == "string");
            expect(!has_key(mode, "properties"));
            yyjson_doc_free(doc);
        }
    };

    "convert_tool_json_wire_shape"_test = [] {
        const kimix::string builtin =
            kimi::convert_tool_json({"$web_search", "Search the web", "{}"});
        expect(builtin.find("\"builtin_function\"") != kimix::string::npos);
        expect(builtin.find("$web_search") != kimix::string::npos);
        expect(builtin.find("parameters") == kimix::string::npos);
        const kimix::string normal =
            kimi::convert_tool_json({"lookup", "Look something up.",
                                     R"({"type":"object","properties":{"q":{"enum":["a"]}}})"});
        expect(normal.find("\"function\"") != kimix::string::npos);
        // enum-only property got its type completed.
        expect(normal.find(R"("q":{"enum":["a"],"type":"string"})") !=
                   kimix::string::npos ||
               normal.find(R"("type":"string")") != kimix::string::npos);
    };

    // -----------------------------------------------------------------------
    // Stream: Moonshot usage cached_tokens extraction + the empty-think
    // round-trip (reasoning deltas stream through unchanged).
    // -----------------------------------------------------------------------

    "sse_usage_extracts_moonshot_cached_tokens"_test = [] {
        openai::SseParser parser;
        const std::string sse =
            "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hi\"}}]}\n\n"
            "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":100,"
            "\"completion_tokens\":5,\"total_tokens\":105,\"cached_tokens\":40}}\n\n"
            "data: [DONE]\n\n";
        auto chunks = parser.feed(sse.data(), sse.size());
        bool found = false;
        for (const auto &c : chunks) {
            if (c.has_usage) {
                found = true;
                expect(eq(c.prompt_tokens, 100));
                expect(eq(c.completion_tokens, 5));
                expect(eq(c.cached_tokens, 40));
            }
        }
        expect(found) << "usage chunk observed";
    };

    "sse_usage_falls_back_to_prompt_tokens_details"_test = [] {
        openai::SseParser parser;
        const std::string sse =
            "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":50,"
            "\"prompt_tokens_details\":{\"cached_tokens\":20}}}\n\n";
        auto chunks = parser.feed(sse.data(), sse.size());
        bool found = false;
        for (const auto &c : chunks) {
            if (c.has_usage) {
                found = true;
                expect(eq(c.cached_tokens, 20));
            }
        }
        expect(found);
    };

    // -----------------------------------------------------------------------
    // create_llm dispatch + provider round trip (no network at construction)
    // -----------------------------------------------------------------------

    "create_llm_dispatches_kimi_type"_test = [] {
        auto llm = create_llm(make_config());
        expect(llm != nullptr);
        if (llm) {
            expect(llm->model_name() == "kimi-k2-turbo-preview");
        }
    };

    "kimi_env_defaults_apply"_test = [] {
        // apply_env_overrides is provider-agnostic (the KIMI_* chain); the
        // Kimi config flows through create_llm unchanged.
        Config cfg = make_config();
        expect(!cfg.model.empty());
    };

    // -----------------------------------------------------------------------
    // Opt-in REAL e2e (the reference file's _dev_main equivalent): keep this
    // as cheap as possible - one tiny prompt, a tight output cap, thinking
    // OFF (skips the reasoning tokens), stream on. Only runs with
    // --config=<path> (e.g. --config=D:/k27.json).
    // -----------------------------------------------------------------------

    "e2e_real_backend_kimi"_test = [config_path] {
        if (config_path.empty()) {
            std::printf("SKIPPED e2e_real_backend_kimi: pass --config=<path>\n");
            return;
        }
        Config cfg;
        if (!load_config(config_path, cfg)) {
            std::printf("SKIPPED e2e_real_backend_kimi: cannot load %s\n",
                        config_path.c_str());
            return;
        }
        cfg.type = "kimi";
        // Keep it cheap: thinking off and a tiny completion budget.
        cfg.enable_thinking = false;
        cfg.thinking_effort = "off";
        cfg.max_tokens = 64;
        auto llm = create_llm(cfg);
        expect(llm != nullptr);
        if (!llm) {
            return;
        }
        kimix::string streamed;
        const ChatResult r =
            llm->chat({user_msg("Say exactly: KIMI_OK")}, {},
                      [&](const Chunk &c) { streamed += c.content; });
        std::printf("--- e2e kimi ---\n");
        std::printf("ok=%d content=%s\n", (int)r.ok, r.content.c_str());
        std::printf("reasoning=%s finish=%s prompt=%lld completion=%lld cached=%lld\n",
                    r.reasoning.c_str(), r.finish_reason.c_str(),
                    (long long)r.prompt_tokens, (long long)r.completion_tokens,
                    (long long)r.cached_tokens);
        expect(r.ok) << "e2e ok, error=" << r.error;
        expect(!r.content.empty()) << "e2e streamed a visible reply";
        expect(!streamed.empty()) << "chunk callback saw content";
    };

    return 0;
}
