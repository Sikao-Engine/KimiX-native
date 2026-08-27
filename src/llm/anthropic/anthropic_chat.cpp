// anthropic_chat.cpp - Anthropic Messages API streaming workflow.
//
// Transport uses cpp-httplib; HTTPS is enabled by CPPHTTPLIB_MBEDTLS_SUPPORT
// (Mbed TLS provided by the kimix-mbedtls target), so this code is fully
// cross-platform. SSE bytes are fed into anthropic/stream_parser.h exactly
// like the OpenAI demo feeds its parser.
//
// <httplib.h> comes first so winsock2.h is included before
// <core/kimix_core.h> pulls in <windows.h> (windows.h-before-winsock2.h
// breaks ws2tcpip.h on Windows; unity build merges these TUs).

#include <httplib.h>

#include "llm/anthropic/anthropic_chat.h"

#include <chrono>
#include <cstdio>
#include <thread>

#include "yyjson.h"

#include "llm/yyjson_alc.h"

namespace kimix::llm::anthropic {

namespace detail {

// Map an effort level to a legacy thinking budget (mirrors anthropic.py).
int thinking_budget(const kimix::string &effort) {
    if (effort == "low") {
        return 1024;
    }
    if (effort == "medium") {
        return 4096;
    }
    if (effort == "xhigh") {
        return 64'000;
    }
    if (effort == "max") {
        return 128'000;
    }
    return 32'000; // high and default
}

} // namespace detail

kimix::string build_messages_body(const Config &cfg,
                                  const kimix::string &system,
                                  const kimix::vector<ChatMessage> &messages,
                                  const kimix::vector<Tool> &tools) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&kYYJsonAlcMi);
    if (!doc) {
        return {};
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "model", cfg.model.c_str());
    yyjson_mut_obj_add_int(doc, root, "max_tokens", cfg.max_tokens);
    yyjson_mut_obj_add_bool(doc, root, "stream", true);
    if (!system.empty()) {
        yyjson_mut_obj_add_str(doc, root, "system", system.c_str());
    }

    yyjson_mut_val *msg_arr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "messages", msg_arr);
    for (const auto &m : messages) {
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, obj, "role", m.role.c_str());
        if (m.role == "user" && !m.tool_result_id.empty()) {
            // User tool_result content block.
            yyjson_mut_val *content = yyjson_mut_arr(doc);
            yyjson_mut_obj_add_val(doc, obj, "content", content);
            yyjson_mut_val *block = yyjson_mut_obj(doc);
            yyjson_mut_arr_append(content, block);
            yyjson_mut_obj_add_str(doc, block, "type", "tool_result");
            yyjson_mut_obj_add_str(doc, block, "tool_use_id", m.tool_result_id.c_str());
            yyjson_mut_obj_add_str(doc, block, "content", m.tool_result_content.c_str());
        } else if (m.role == "assistant") {
            // Assistant content is a block list: thinking (required by some
            // backends, e.g. DeepSeek, when thinking mode is on), then text,
            // then tool_use blocks.
            yyjson_mut_val *content = yyjson_mut_arr(doc);
            yyjson_mut_obj_add_val(doc, obj, "content", content);
            if (!m.thinking.empty() || !m.thinking_signature.empty()) {
                yyjson_mut_val *block = yyjson_mut_obj(doc);
                yyjson_mut_arr_append(content, block);
                yyjson_mut_obj_add_str(doc, block, "type", "thinking");
                yyjson_mut_obj_add_str(doc, block, "thinking", m.thinking.c_str());
                if (!m.thinking_signature.empty()) {
                    yyjson_mut_obj_add_str(doc, block, "signature", m.thinking_signature.c_str());
                }
            }
            if (!m.text.empty()) {
                yyjson_mut_val *block = yyjson_mut_obj(doc);
                yyjson_mut_arr_append(content, block);
                yyjson_mut_obj_add_str(doc, block, "type", "text");
                yyjson_mut_obj_add_str(doc, block, "text", m.text.c_str());
            }
            for (const auto &tu : m.tool_uses) {
                yyjson_mut_val *block = yyjson_mut_obj(doc);
                yyjson_mut_arr_append(content, block);
                yyjson_mut_obj_add_str(doc, block, "type", "tool_use");
                yyjson_mut_obj_add_str(doc, block, "id", tu.id.c_str());
                yyjson_mut_obj_add_str(doc, block, "name", tu.name.c_str());
                yyjson_doc *idoc = yyjson_read_opts((char *)tu.input_json.data(), tu.input_json.size(), 0, &kYYJsonAlcMi, nullptr);
                yyjson_mut_val *input = nullptr;
                if (idoc) {
                    input = yyjson_val_mut_copy(doc, yyjson_doc_get_root(idoc));
                    yyjson_doc_free(idoc);
                }
                if (!input) {
                    input = yyjson_mut_obj(doc);
                }
                yyjson_mut_obj_add_val(doc, block, "input", input);
            }
        } else {
            // Plain user text.
            yyjson_mut_obj_add_str(doc, obj, "content", m.text.c_str());
        }
        yyjson_mut_arr_append(msg_arr, obj);
    }

    if (!tools.empty()) {
        yyjson_mut_val *tools_arr = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, root, "tools", tools_arr);
        for (const auto &t : tools) {
            yyjson_mut_val *tool_obj = yyjson_mut_obj(doc);
            yyjson_mut_arr_append(tools_arr, tool_obj);
            yyjson_mut_obj_add_str(doc, tool_obj, "name", t.name.c_str());
            yyjson_mut_obj_add_str(doc, tool_obj, "description", t.description.c_str());
            yyjson_doc *sdoc = yyjson_read_opts((char *)t.input_schema_json.data(), t.input_schema_json.size(), 0, &kYYJsonAlcMi, nullptr);
            yyjson_mut_val *schema = nullptr;
            if (sdoc) {
                schema = yyjson_val_mut_copy(doc, yyjson_doc_get_root(sdoc));
                yyjson_doc_free(sdoc);
            }
            if (!schema) {
                schema = yyjson_mut_obj(doc);
            }
            yyjson_mut_obj_add_val(doc, tool_obj, "input_schema", schema);
        }
    }

    // Legacy budget-based thinking (maps thinking_effort to a token budget,
    // mirroring anthropic.py's budgets table).
    yyjson_mut_val *thinking = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, root, "thinking", thinking);
    yyjson_mut_obj_add_str(doc, thinking, "type", "enabled");
    yyjson_mut_obj_add_int(doc, thinking, "budget_tokens", detail::thinking_budget(cfg.thinking_effort));

    char *json = yyjson_mut_write_opts(doc, 0, &kYYJsonAlcMi, nullptr, nullptr);
    kimix::string body = json ? kimix::string(json) : kimix::string();
    mi_free(json);
    yyjson_mut_doc_free(doc);
    return body;
}

ChatResult chat_completion_stream(const Config &cfg,
                                  const kimix::string &system,
                                  const kimix::vector<ChatMessage> &messages,
                                  const kimix::vector<Tool> &tools,
                                  const EventCallback &on_event) {
    ChatResult result;
    const kimix::string body = build_messages_body(cfg, system, messages, tools);
    if (body.empty()) {
        result.error = "failed to build request body";
        return result;
    }

    // Split the config URL into scheme / host[:port] / path prefix.
    const Endpoint ep = parse_endpoint(cfg.url);
    if (ep.host.empty()) {
        result.error = "invalid config url: " + cfg.url;
        return result;
    }
    const kimix::string path = join_path(ep.path_prefix, "v1/messages");

    // httplib::Client("https://host:port") transparently picks SSLClient when
    // CPPHTTPLIB_MBEDTLS_SUPPORT is enabled. On Windows root certificates are
    // loaded automatically from the system store (cpp-httplib's
    // CPPHTTPLIB_WINDOWS_AUTOMATIC_ROOT_CERTIFICATES_UPDATE).
    httplib::Client cli(std::string(ep.scheme) + "://" + std::string(ep.host) + ":"
                        + std::to_string(ep.port));
    cli.set_connection_timeout(30);
    cli.set_read_timeout(300, 0);
    cli.set_write_timeout(30, 0);

    httplib::Headers headers = {
        // Content-Type is added by Post() below; keep this map free of
        // duplicates (some gateways are picky).
        {"Accept", "text/event-stream"},
        {"x-api-key", std::string(cfg.api_key)},
        {"anthropic-version", "2023-06-01"},
    };

    // Transient failures (403/408/429/5xx, dropped connections) are retried a
    // couple of times with a short pause.
    constexpr int kMaxAttempts = 3;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        // Reset accumulators for this attempt.
        result.text.clear();
        result.thinking.clear();
        result.signature.clear();
        result.tool_uses.clear();
        result.input_tokens = 0;
        result.output_tokens = 0;
        result.cache_creation_input_tokens = 0;
        result.cache_read_input_tokens = 0;

        StreamParser parser;
        kimix::vector<ToolUse> acc_tool_uses;
        // Anthropic indexes ALL content blocks (thinking, text, tool_use)
        // sequentially, so a tool_use block may sit at any index. Map each
        // tool_use block index to its position in acc_tool_uses.
        kimix::map<int, size_t> tool_block_pos;
        kimix::string thinking, signature;

        const auto consume = [&](const StreamEvent &ev) {
            if (on_event) {
                on_event(ev);
            }
            if (ev.type == "message_start") {
                if (ev.usage.has) {
                    result.input_tokens = ev.usage.input_tokens;
                    result.cache_creation_input_tokens = ev.usage.cache_creation_input_tokens;
                    result.cache_read_input_tokens = ev.usage.cache_read_input_tokens;
                }
            } else if (ev.type == "content_block_start") {
                if (ev.block_type == "tool_use") {
                    size_t pos = acc_tool_uses.size();
                    tool_block_pos[ev.index] = pos;
                    acc_tool_uses.push_back({ev.block_id, ev.block_name, ev.text});
                }
            } else if (ev.type == "content_block_delta") {
                if (ev.delta_type == "text_delta") {
                    result.text += ev.text;
                } else if (ev.delta_type == "thinking_delta") {
                    thinking += ev.text;
                } else if (ev.delta_type == "signature_delta") {
                    signature += ev.signature;
                } else if (ev.delta_type == "input_json_delta") {
                    auto it = tool_block_pos.find(ev.index);
                    if (it != tool_block_pos.end()) {
                        acc_tool_uses[it->second].input_json += ev.text;
                    } else if (!acc_tool_uses.empty()) {
                        acc_tool_uses.back().input_json += ev.text;
                    }
                }
            } else if (ev.type == "message_delta") {
                if (!ev.stop_reason.empty()) {
                    result.stop_reason = ev.stop_reason;
                }
                if (ev.usage.has) {
                    result.output_tokens = ev.usage.output_tokens;
                    if (ev.usage.input_tokens > 0) {
                        result.input_tokens = ev.usage.input_tokens;
                    }
                    result.cache_creation_input_tokens = ev.usage.cache_creation_input_tokens;
                    result.cache_read_input_tokens = ev.usage.cache_read_input_tokens;
                }
            }
        };

        httplib::ContentReceiver receiver = [&](const char *data, size_t len) -> bool {
            for (const auto &ev : parser.feed(data, len)) {
                consume(ev);
            }
            return true;
        };

        httplib::Result res = cli.Post(std::string(path), headers, std::string(body),
                                       "application/json", receiver);
        for (const auto &ev : parser.finish()) {
            consume(ev);
        }

        const bool retriable = !res || is_retriable_status(res->status);
        if (retriable && attempt < kMaxAttempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300 * attempt));
            continue;
        }

        if (!res) {
            result.error = "http error: " + httplib::to_string(res.error());
            return result;
        }
        if (res->status != 200) {
            result.error = "http status " + std::to_string(res->status) + ": "
                           + res->body.substr(0, 500);
            return result;
        }

        result.thinking = std::move(thinking);
        result.signature = std::move(signature);
        result.tool_uses = std::move(acc_tool_uses);
        result.ok = true;
        return result;
    }

    result.error = "exhausted retries";
    return result;
}

} // namespace kimix::llm::anthropic
