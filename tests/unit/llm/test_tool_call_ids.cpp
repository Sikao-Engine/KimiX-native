// Test for E4 tool-call pairing repair:
//   * kimix::llm::normalize_tool_call_ids - the port of
//     kosong/contrib/chat_provider/common.py normalize_tool_call_ids (charset
//     sanitizing to [A-Za-z0-9_-], 64-character truncation, "_2"/"_3"
//     collision suffixes, empty-id repair to "tool_call", consistent rewrite
//     of assistant tool_calls entries and their matching tool messages).
//   * the provider wire seams (openai_wire_messages / responses_wire_input /
//     anthropic_wire_request): the normalized ids reach the serialized request
//     bodies, and the Anthropic wire merges consecutive tool-result-only user
//     messages into ONE user message (the Anthropic spec requirement that
//     anthropic.py generate() enforces).
//
// The reference does NOT drop orphan tool_use/tool_result pairs - that
// behaviour is deliberately absent here too (only the id repair runs).
//
// No network access: only the normalization pass and the request-body builders
// are exercised; bodies are parsed with strict yyjson.

#include "ut/ut.hpp"

#include "llm/llm.h"

#include "yyjson.h"

#include "llm/yyjson_alc.h"

#include <cstdio>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using namespace kimix::llm;

namespace {

// True when `hay` contains the byte sequence `needle`.
bool contains(const kimix::string &hay, const char *needle) {
    return hay.find(needle) != kimix::string::npos;
}

// Strict-parse `body` and return the root (nullptr on failure). The caller
// frees the doc.
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
yyjson_val *get(yyjson_val *v) { return v; }
template <typename T, typename... Rest>
yyjson_val *get(yyjson_val *v, T first, Rest... rest) {
    return get(get_one(v, first), rest...);
}

kimix::string str(yyjson_val *v) {
    return (v != nullptr && yyjson_is_str(v))
               ? kimix::string(yyjson_get_str(v), yyjson_get_len(v))
               : kimix::string{};
}

Config make_config(const char *type) {
    Config cfg;
    cfg.type = type;
    cfg.model = "test-model";
    cfg.url = "http://127.0.0.1:9/v1";
    cfg.api_key = "k";
    return cfg;
}

// A minimal history: assistant call -> tool result, with customizable ids.
kimix::vector<Message> pair_history(const char *call_id, const char *result_id) {
    kimix::vector<Message> messages;
    Message assistant;
    assistant.role = "assistant";
    assistant.content = "";
    ToolCall tc;
    tc.id = call_id;
    tc.name = "read_file";
    tc.arguments = "{}";
    assistant.tool_calls.push_back(std::move(tc));
    messages.push_back(std::move(assistant));
    Message tool;
    tool.role = "tool";
    tool.content = "file body";
    tool.tool_call_id = result_id;
    messages.push_back(std::move(tool));
    return messages;
}

// Count tool_result blocks on the Anthropic wire.
size_t anthropic_tool_result_blocks(const kimix::string &body) {
    size_t n = 0;
    yyjson_doc *doc = nullptr;
    yyjson_val *root = root_of(body, &doc);
    if (root != nullptr) {
        yyjson_val *messages = get(root, "messages");
        const size_t n_msg = yyjson_is_arr(messages) ? yyjson_arr_size(messages) : 0;
        for (size_t i = 0; i < n_msg; ++i) {
            yyjson_val *content = get(messages, at{i}, "content");
            if (yyjson_is_arr(content)) {
                const size_t n_blk = yyjson_arr_size(content);
                for (size_t b = 0; b < n_blk; ++b) {
                    if (str(get(content, at{b}, "type")) == "tool_result") {
                        ++n;
                    }
                }
            }
        }
    }
    yyjson_doc_free(doc);
    return n;
}

// Count messages with role "user" on the wire.
size_t anthropic_user_messages(const kimix::string &body) {
    size_t n = 0;
    yyjson_doc *doc = nullptr;
    yyjson_val *root = root_of(body, &doc);
    yyjson_val *messages = root != nullptr ? get(root, "messages") : nullptr;
    const size_t count = yyjson_is_arr(messages) ? yyjson_arr_size(messages) : 0;
    for (size_t i = 0; i < count; ++i) {
        if (str(get(messages, at{i}, "role")) == "user") {
            ++n;
        }
    }
    yyjson_doc_free(doc);
    return n;
}

} // namespace

int main() {
    // -- pure normalization pass ---------------------------------------------
    "charset_replaced_with_underscore"_test = [] {
        const kimix::vector<Message> in = pair_history("Read:9", "Read:9");
        const kimix::vector<Message> out = normalize_tool_call_ids(in);
        expect(out.size() == 2u);
        expect(out[0].tool_calls[0].id == "Read_9");
        expect(out[1].tool_call_id == "Read_9");
        // Input is never mutated.
        expect(in[0].tool_calls[0].id == "Read:9");
        expect(in[1].tool_call_id == "Read:9");
    };

    "ids_truncated_to_64_chars"_test = [] {
        const kimix::string long_id(70, 'a');
        const kimix::vector<Message> in = pair_history(long_id.c_str(), long_id.c_str());
        const kimix::vector<Message> out = normalize_tool_call_ids(in);
        expect(out[0].tool_calls[0].id == kimix::string(64, 'a'));
        expect(out[1].tool_call_id == kimix::string(64, 'a'));
    };

    "collision_gets_underscore_two_suffix_within_budget"_test = [] {
        // Two DISTINCT raw ids that sanitize to the same 64-char shape: the
        // second one gets "_2" and the base shrinks so the id stays <= 64.
        const kimix::string a64(64, 'a');
        const kimix::string a65(65, 'a');
        kimix::vector<Message> in = pair_history(a64.c_str(), a64.c_str());
        Message assistant2;
        assistant2.role = "assistant";
        ToolCall tc;
        tc.id = a65;
        tc.name = "t";
        tc.arguments = "{}";
        assistant2.tool_calls.push_back(std::move(tc));
        in.push_back(std::move(assistant2));
        Message tool2;
        tool2.role = "tool";
        tool2.tool_call_id = a65;
        in.push_back(std::move(tool2));

        const kimix::vector<Message> out = normalize_tool_call_ids(in);
        expect(out[0].tool_calls[0].id == a64);
        expect(out[1].tool_call_id == a64);
        expect(out[2].tool_calls[0].id == a64.substr(0, 62) + "_2");
        expect(out[3].tool_call_id == a64.substr(0, 62) + "_2");
        expect(out[2].tool_calls[0].id.size() == 64u);
    };

    "valid_ids_keep_value_but_block_their_spelling"_test = [] {
        // First pass: "tool_1" is already valid and keeps it; "tool:1"
        // sanitizes onto the same shape and becomes "tool_1_2" (the reference's
        // two-pass mapping).
        kimix::vector<Message> in = pair_history("tool_1", "tool_1");
        in[1].tool_call_id = "tool:1";
        in[0].tool_calls[0].id = "tool_1";
        const kimix::vector<Message> out = normalize_tool_call_ids(in);
        expect(out[0].tool_calls[0].id == "tool_1");
        expect(out[1].tool_call_id == "tool_1_2");
    };

    "empty_id_repaired_to_tool_call"_test = [] {
        // A tool message with an empty id is repaired (the reference's
        // _EMPTY_TOOL_CALL_ID), never dropped.
        kimix::vector<Message> in = pair_history("call_1", "");
        const kimix::vector<Message> out = normalize_tool_call_ids(in);
        expect(out[1].tool_call_id == "tool_call");
    };

    "assistant_call_and_result_share_the_normalized_id"_test = [] {
        // Both sides carry the same (invalid) raw id -> both map onto the same
        // normalized id, keeping the pairing unambiguous.
        const kimix::vector<Message> in = pair_history("a b", "a b");
        const kimix::vector<Message> out = normalize_tool_call_ids(in);
        expect(out[0].tool_calls[0].id == "a_b");
        expect(out[1].tool_call_id == "a_b");
    };

    "already_valid_history_is_returned_unchanged"_test = [] {
        const kimix::vector<Message> in = pair_history("callu_01", "callu_01");
        const kimix::vector<Message> out = normalize_tool_call_ids(in);
        expect(out.size() == 2u);
        expect(out[0].tool_calls[0].id == "callu_01");
        expect(out[1].tool_call_id == "callu_01");
    };

    "history_without_ids_is_returned_unchanged"_test = [] {
        kimix::vector<Message> in;
        Message user;
        user.role = "user";
        user.content = "hi";
        in.push_back(std::move(user));
        const kimix::vector<Message> out = normalize_tool_call_ids(in);
        expect(out.size() == 1u);
        expect(out[0].content == "hi");
    };

    // -- ids reach every provider's serialized body ---------------------------
    "openai_body_carries_normalized_ids"_test = [] {
        const Config cfg = make_config("openai_legacy");
        kimix::vector<Message> in = pair_history("Read:9", "Read:9");
        const kimix::string body =
            openai::build_chat_body(cfg, openai_wire_messages(in), {});
        yyjson_doc *doc = nullptr;
        yyjson_val *root = root_of(body, &doc);
        expect(root != nullptr) << "body is not valid JSON";
        if (root != nullptr) {
            expect(str(get(root, "messages", at{0}, "tool_calls", at{0}, "id")) == "Read_9");
            expect(str(get(root, "messages", at{1}, "tool_call_id")) == "Read_9");
        }
        yyjson_doc_free(doc);
    };

    "responses_body_carries_normalized_ids"_test = [] {
        const Config cfg = make_config("openai_responses");
        kimix::vector<Message> in = pair_history("Read:9", "Read:9");
        const kimix::string body =
            openai_responses::build_responses_body(cfg, responses_wire_input(in), {});
        yyjson_doc *doc = nullptr;
        yyjson_val *root = root_of(body, &doc);
        expect(root != nullptr) << "body is not valid JSON";
        if (root != nullptr) {
            // The assistant text is empty, so the wire input is
            // [function_call, function_call_output].
            expect(str(get(root, "input", at{0}, "call_id")) == "Read_9"); // function_call
            expect(str(get(root, "input", at{1}, "call_id"))
                   == "Read_9"); // function_call_output
        }
        yyjson_doc_free(doc);
    };

    // -- Anthropic wire: merge of tool-result-only user messages --------------
    "anthropic_wire_merges_consecutive_tool_results"_test = [] {
        const Config cfg = make_config("anthropic");
        kimix::vector<Message> in = pair_history("call_1", "call_1");
        Message tool2;
        tool2.role = "tool";
        tool2.content = "second result";
        tool2.tool_call_id = "call_2";
        in.push_back(std::move(tool2));
        Message user;
        user.role = "user";
        user.content = "thanks";
        in.push_back(std::move(user));

        const AnthropicWireRequest wire = anthropic_wire_request(in);
        // [assistant(tool_use), user(2x tool_result merged), user(text)]
        expect(wire.messages.size() == 3u);
        expect(wire.messages[1].tool_results.size() == 2u);
        expect(wire.messages[1].tool_results[0].tool_use_id == "call_1");
        expect(wire.messages[1].tool_results[1].tool_use_id == "call_2");

        const kimix::string body =
            anthropic::build_messages_body(cfg, wire.system, wire.messages, {});
        expect(anthropic_tool_result_blocks(body) == 2u);
        // Exactly one user message holds both blocks: the merged one plus the
        // trailing text one.
        expect(anthropic_user_messages(body) == 2u);
    };

    "anthropic_wire_never_merges_across_plain_user_text"_test = [] {
        const Config cfg = make_config("anthropic");
        kimix::vector<Message> in = pair_history("call_1", "call_1");
        Message user;
        user.role = "user";
        user.content = "and now look at this";
        in.push_back(std::move(user));
        Message tool2;
        tool2.role = "tool";
        tool2.content = "second result";
        tool2.tool_call_id = "call_2";
        in.push_back(std::move(tool2));

        const AnthropicWireRequest wire = anthropic_wire_request(in);
        // [assistant, user(tool_result), user(text), user(tool_result)]
        expect(wire.messages.size() == 4u);
        const kimix::string body =
            anthropic::build_messages_body(cfg, wire.system, wire.messages, {});
        expect(anthropic_tool_result_blocks(body) == 2u);
    };

    "anthropic_wire_repairs_invalid_ids_and_pairs_blocks"_test = [] {
        const Config cfg = make_config("anthropic");
        // Anthropic 400s on "Read:9"-style ids; the merged message carries the
        // normalized ids on both sides of the pair.
        kimix::vector<Message> in = pair_history("Read:9", "Read:9");
        Message tool2;
        tool2.role = "tool";
        tool2.content = "second";
        tool2.tool_call_id = "list files:1";
        in.push_back(std::move(tool2));

        const AnthropicWireRequest wire = anthropic_wire_request(in);
        expect(wire.messages[1].tool_results.size() == 2u);
        expect(wire.messages[0].tool_uses[0].id == "Read_9");
        expect(wire.messages[1].tool_results[0].tool_use_id == "Read_9");
        expect(wire.messages[1].tool_results[1].tool_use_id == "list_files_1");

        const kimix::string body =
            anthropic::build_messages_body(cfg, wire.system, wire.messages, {});
        yyjson_doc *doc = nullptr;
        yyjson_val *root = root_of(body, &doc);
        expect(root != nullptr);
        if (root != nullptr) {
            expect(str(get(root, "messages", at{0}, "content", at{0}, "id")) == "Read_9");
            expect(str(get(root, "messages", at{1}, "content", at{0}, "tool_use_id"))
                   == "Read_9");
            expect(str(get(root, "messages", at{1}, "content", at{1}, "tool_use_id"))
                   == "list_files_1");
        }
        yyjson_doc_free(doc);
    };

    // The empty tool id is repaired to "tool_call" on the wire (the reference
    // repairs, it does not drop the message).
    "anthropic_wire_repairs_empty_tool_id"_test = [] {
        const Config cfg = make_config("anthropic");
        kimix::vector<Message> in = pair_history("call_1", "");
        const AnthropicWireRequest wire = anthropic_wire_request(in);
        expect(wire.messages.size() == 2u);
        expect(wire.messages[1].tool_result_id == "tool_call");
        const kimix::string body =
            anthropic::build_messages_body(cfg, wire.system, wire.messages, {});
        expect(contains(body, "\"tool_use_id\":\"tool_call\""));
    };

    // Anthropic system-role content still becomes the request-level system
    // string (joined with "\n" when multiple), never a message.
    "anthropic_wire_extracts_system"_test = [] {
        kimix::vector<Message> in;
        Message s1;
        s1.role = "system";
        s1.content = "rules";
        in.push_back(std::move(s1));
        Message u;
        u.role = "user";
        u.content = "hi";
        in.push_back(std::move(u));
        Message s2;
        s2.role = "system";
        s2.content = "more rules";
        in.push_back(std::move(s2));
        const AnthropicWireRequest wire = anthropic_wire_request(in);
        expect(wire.system == "rules\nmore rules");
        expect(wire.messages.size() == 1u);
        expect(wire.messages[0].role == "user");
    };

    return 0;
}
