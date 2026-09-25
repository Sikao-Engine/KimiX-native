// openai_chat.cpp - OpenAI-compatible chat completion streaming workflow.
//
// <httplib.h> comes first so winsock2.h is included before
// <core/kimix_core.h> pulls in <windows.h> (windows.h-before-winsock2.h
// breaks ws2tcpip.h on Windows; unity build merges these TUs).

#include <httplib.h>

#include "llm/http_tls.h"

#include "llm/openai/openai_chat.h"

#include <chrono>
#include <cstdio>
#include <thread>

#include "yyjson.h"

#include "llm/yyjson_alc.h"

namespace kimix::llm::openai {

kimix::string build_chat_body(const Config &cfg,
                              const kimix::vector<ChatMessage> &messages,
                              const kimix::vector<Tool> &tools,
                              kimix::string *out_error) {
    // Every string on the wire goes through add_json_str(): yyjson refuses to
    // write a document holding invalid UTF-8 (and its strlen-based add_str
    // helpers stop at an embedded '\0'), so agent text is normalized here.
    kimix::string *err = out_error;
    kimix::string scratch;
    if (!err) {
        err = &scratch;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&kYYJsonAlcMi);
    if (!doc) {
        *err = "no memory for the JSON document";
        return {};
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    add_json_str(doc, root, "model", cfg.model);
    yyjson_mut_obj_add_bool(doc, root, "stream", true);

    yyjson_mut_val *stream_options = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, root, "stream_options", stream_options);
    yyjson_mut_obj_add_bool(doc, stream_options, "include_usage", true);

    yyjson_mut_val *msg_arr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "messages", msg_arr);
    for (const auto &m : messages) {
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        add_json_str(doc, obj, "role", m.role);
        if (!m.tool_calls.empty() && m.content.empty()) {
            // OpenAI-compatible APIs allow assistant tool-call messages to omit
            // content, but many backends reject an empty string; use null.
            yyjson_mut_obj_add_null(doc, obj, "content");
        } else {
            add_json_str(doc, obj, "content", m.content);
        }
        if (!m.tool_call_id.empty()) {
            add_json_str(doc, obj, "tool_call_id", m.tool_call_id);
        }
        if (!m.tool_calls.empty()) {
            yyjson_mut_val *tc_arr = yyjson_mut_arr(doc);
            yyjson_mut_obj_add_val(doc, obj, "tool_calls", tc_arr);
            for (const auto &tc : m.tool_calls) {
                yyjson_mut_val *tc_obj = yyjson_mut_obj(doc);
                yyjson_mut_arr_append(tc_arr, tc_obj);
                add_json_str(doc, tc_obj, "id", tc.id);
                add_json_str(doc, tc_obj, "type", tc.type);
                yyjson_mut_val *fn = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_val(doc, tc_obj, "function", fn);
                add_json_str(doc, fn, "name", tc.name);
                add_json_str(doc, fn, "arguments", tc.arguments);
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
            add_json_str(doc, fn, "name", t.name);
            add_json_str(doc, fn, "description", t.description);
            if (!add_json_fragment(doc, fn, "parameters", t.parameters_json)) {
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
    add_json_str(doc, reasoning, "effort", cfg.thinking_effort);
    yyjson_mut_val *ctkw = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, root, "chat_template_kwargs", ctkw);
    add_json_str(doc, ctkw, "reasoning_effort", cfg.thinking_effort);
    add_json_str(doc, root, "reasoning_effort", cfg.thinking_effort);

    return write_json_doc(doc, *err);
}

ChatResult chat_completion_stream(const Config &cfg,
                                  const kimix::vector<ChatMessage> &messages,
                                  const kimix::vector<Tool> &tools,
                                  const ChunkCallback &on_chunk) {
    ChatResult result;
    kimix::string why;
    const kimix::string body = build_chat_body(cfg, messages, tools, &why);
    if (body.empty()) {
        result.error = "failed to build request body";
        if (!why.empty()) {
            result.error += ": " + why;
        }
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
    install_windows_tls_verifier(cli, std::string(ep.host));
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

        // A 200 body from which nothing parsed (garbage / non-SSE / HTML error
    // page) is unusable; treat it like a transient failure and retry.
    const bool unusable = acc_tool_calls.empty() && result.content.empty()
            && result.reasoning.empty();
    const bool retriable = !res || is_retriable_status(res->status)
            || (res->status == 200 && unusable);
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

        if (unusable) {
            result.error = "backend returned an unusable response body "
                "(no parseable events: invalid JSON or empty stream)";
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
