// openai_chat.cpp - OpenAI-compatible chat completion streaming workflow.
//
// <httplib.h> comes first so winsock2.h is included before
// <core/kimix_core.h> pulls in <windows.h> (windows.h-before-winsock2.h
// breaks ws2tcpip.h on Windows; unity build merges these TUs).

#include <httplib.h>

#include "llm/openai/openai_chat.h"

#include <chrono>
#include <cstdio>
#include <thread>

#include "yyjson.h"

#include "llm/yyjson_alc.h"

namespace kimix::llm::openai {

kimix::string build_chat_body(const Config &cfg,
                              const kimix::vector<ChatMessage> &messages,
                              const kimix::vector<Tool> &tools) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&kYYJsonAlcMi);
    if (!doc) {
        return {};
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "model", cfg.model.c_str());
    yyjson_mut_obj_add_bool(doc, root, "stream", true);

    yyjson_mut_val *stream_options = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, root, "stream_options", stream_options);
    yyjson_mut_obj_add_bool(doc, stream_options, "include_usage", true);

    yyjson_mut_val *msg_arr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "messages", msg_arr);
    for (const auto &m : messages) {
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, obj, "role", m.role.c_str());
        if (!m.tool_calls.empty() && m.content.empty()) {
            // OpenAI-compatible APIs allow assistant tool-call messages to omit
            // content, but many backends reject an empty string; use null.
            yyjson_mut_obj_add_null(doc, obj, "content");
        } else {
            yyjson_mut_obj_add_str(doc, obj, "content", m.content.c_str());
        }
        if (!m.tool_call_id.empty()) {
            yyjson_mut_obj_add_str(doc, obj, "tool_call_id", m.tool_call_id.c_str());
        }
        if (!m.tool_calls.empty()) {
            yyjson_mut_val *tc_arr = yyjson_mut_arr(doc);
            yyjson_mut_obj_add_val(doc, obj, "tool_calls", tc_arr);
            for (const auto &tc : m.tool_calls) {
                yyjson_mut_val *tc_obj = yyjson_mut_obj(doc);
                yyjson_mut_arr_append(tc_arr, tc_obj);
                yyjson_mut_obj_add_str(doc, tc_obj, "id", tc.id.c_str());
                yyjson_mut_obj_add_str(doc, tc_obj, "type", tc.type.c_str());
                yyjson_mut_val *fn = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_val(doc, tc_obj, "function", fn);
                yyjson_mut_obj_add_str(doc, fn, "name", tc.name.c_str());
                yyjson_mut_obj_add_str(doc, fn, "arguments", tc.arguments.c_str());
            }
        }
        yyjson_mut_arr_append(msg_arr, obj);
    }

    if (!tools.empty()) {
        yyjson_mut_val *tools_arr = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, root, "tools", tools_arr);
        for (const auto &t : tools) {
            yyjson_mut_val *tool_obj = yyjson_mut_obj(doc);
            yyjson_mut_arr_append(tools_arr, tool_obj);
            yyjson_mut_obj_add_str(doc, tool_obj, "type", "function");
            yyjson_mut_val *fn = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_val(doc, tool_obj, "function", fn);
            yyjson_mut_obj_add_str(doc, fn, "name", t.name.c_str());
            yyjson_mut_obj_add_str(doc, fn, "description", t.description.c_str());
            yyjson_doc *pdoc = yyjson_read_opts((char *)t.parameters_json.data(), t.parameters_json.size(), 0, &kYYJsonAlcMi, nullptr);
            if (pdoc) {
                yyjson_val *proot = yyjson_doc_get_root(pdoc);
                yyjson_mut_val *pmut = yyjson_val_mut_copy(doc, proot);
                if (pmut) {
                    yyjson_mut_obj_add_val(doc, fn, "parameters", pmut);
                } else {
                    yyjson_mut_obj_add_null(doc, fn, "parameters");
                }
                yyjson_doc_free(pdoc);
            } else {
                yyjson_mut_obj_add_null(doc, fn, "parameters");
            }
        }
    }

    // DeepSeek-style thinking / reasoning keys (mirrors openai_legacy.py's
    // extra_body: thinking, reasoning, chat_template_kwargs, reasoning_effort).
    yyjson_mut_val *thinking = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, root, "thinking", thinking);
    yyjson_mut_obj_add_str(doc, thinking, "type", "enabled");
    yyjson_mut_val *reasoning = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, root, "reasoning", reasoning);
    yyjson_mut_obj_add_str(doc, reasoning, "effort", cfg.thinking_effort.c_str());
    yyjson_mut_val *ctkw = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, root, "chat_template_kwargs", ctkw);
    yyjson_mut_obj_add_str(doc, ctkw, "reasoning_effort", cfg.thinking_effort.c_str());
    yyjson_mut_obj_add_str(doc, root, "reasoning_effort", cfg.thinking_effort.c_str());

    char *json = yyjson_mut_write_opts(doc, 0, &kYYJsonAlcMi, nullptr, nullptr);
    kimix::string body = json ? kimix::string(json) : kimix::string();
    mi_free(json);
    yyjson_mut_doc_free(doc);
    return body;
}

ChatResult chat_completion_stream(const Config &cfg,
                                  const kimix::vector<ChatMessage> &messages,
                                  const kimix::vector<Tool> &tools,
                                  const ChunkCallback &on_chunk) {
    ChatResult result;
    const kimix::string body = build_chat_body(cfg, messages, tools);
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
    const kimix::string path = join_path(ep.path_prefix, "chat/completions");

    // httplib::Client("https://host:port") transparently picks SSLClient when
    // CPPHTTPLIB_MBEDTLS_SUPPORT is enabled (kimix-llm links kimix-mbedtls).
    httplib::Client cli(std::string(ep.scheme) + "://" + std::string(ep.host) + ":"
                        + std::to_string(ep.port));
    cli.set_connection_timeout(30);
    cli.set_read_timeout(180, 0);
    cli.set_write_timeout(30, 0);

    httplib::Headers headers = {
        // Content-Type is added by Post() below; keep this map free of
        // duplicates (some OpenAI-compatible gateways are picky).
        {"Accept", "text/event-stream"},
        {"Authorization", "Bearer " + std::string(cfg.api_key)},
    };

    // Transient failures (first-connection 403 from some gateways, 429, 5xx,
    // dropped connections) are retried a couple of times with a short pause.
    constexpr int kMaxAttempts = 3;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        // Reset accumulators for this attempt.
        result.content.clear();
        result.reasoning.clear();
        result.tool_calls.clear();
        result.prompt_tokens = 0;
        result.completion_tokens = 0;
        result.total_tokens = 0;

        SseParser parser;
        kimix::vector<ToolCall> acc_tool_calls;
        kimix::string finish_reason;

        const auto consume = [&](const ChatChunk &chunk) {
            if (on_chunk) {
                on_chunk(chunk);
            }
            if (chunk.done) {
                return;
            }
            result.content += chunk.content;
            result.reasoning += chunk.reasoning_content;
            for (const auto &tcd : chunk.tool_calls) {
                if ((size_t)tcd.index >= acc_tool_calls.size()) {
                    acc_tool_calls.resize((size_t)tcd.index + 1);
                }
                ToolCall &acc = acc_tool_calls[(size_t)tcd.index];
                if (!tcd.id.empty()) {
                    acc.id = tcd.id;
                }
                if (!tcd.type.empty()) {
                    acc.type = tcd.type;
                }
                if (!tcd.name.empty()) {
                    acc.name = tcd.name;
                }
                acc.arguments += tcd.arguments;
            }
            if (!chunk.finish_reason.empty()) {
                finish_reason = chunk.finish_reason;
            }
            if (chunk.has_usage) {
                result.prompt_tokens = chunk.prompt_tokens;
                result.completion_tokens = chunk.completion_tokens;
                result.total_tokens = chunk.total_tokens;
            }
        };

        httplib::ContentReceiver receiver = [&](const char *data, size_t len) -> bool {
            for (const auto &chunk : parser.feed(data, len)) {
                consume(chunk);
            }
            return true;
        };

        httplib::Result res = cli.Post(std::string(path), headers, std::string(body),
                                       "application/json", receiver);
        for (const auto &chunk : parser.finish()) {
            consume(chunk);
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

        result.tool_calls = std::move(acc_tool_calls);
        result.ok = true;
        return result;
    }

    result.error = "exhausted retries";
    return result;
}

} // namespace kimix::llm::openai
