// openai_chat.cpp - OpenAI-compatible chat completion streaming workflow.

#include "openai/openai_chat.h"

#include <httplib.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "yyjson.h"

namespace openai {

bool load_config(const std::string &path, LlmConfig &cfg) {
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        return false;
    }
    std::fseek(fp, 0, SEEK_END);
    long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(fp);
        return false;
    }
    std::vector<char> buf((size_t)size);
    size_t rd = std::fread(buf.data(), 1, (size_t)size, fp);
    std::fclose(fp);
    if (rd == 0) {
        return false;
    }

    yyjson_doc *doc = yyjson_read(buf.data(), rd, 0);
    if (!doc) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    bool ok = false;
    if (yyjson_is_obj(root)) {
        auto get_str = [&](const char *key) -> std::string {
            yyjson_val *v = yyjson_obj_get(root, key);
            if (yyjson_is_str(v)) {
                return std::string(yyjson_get_str(v), yyjson_get_len(v));
            }
            return {};
        };
        cfg.model = get_str("model");
        cfg.url = get_str("url");
        cfg.api_key = get_str("api_key");
        cfg.type = get_str("type");
        cfg.thinking_effort = get_str("thinking_effort");
        if (cfg.thinking_effort.empty()) {
            cfg.thinking_effort = "high";
        }
        yyjson_val *v = yyjson_obj_get(root, "show_thinking_stream");
        cfg.show_thinking_stream = yyjson_is_bool(v) && yyjson_get_bool(v);
        v = yyjson_obj_get(root, "max_context_size");
        if (yyjson_is_int(v)) {
            cfg.max_context_size = (int)yyjson_get_int(v);
        }
        ok = !cfg.model.empty() && !cfg.url.empty();
    }
    yyjson_doc_free(doc);
    return ok;
}

std::string build_chat_body(const LlmConfig &cfg,
                            const std::vector<ChatMessage> &messages,
                            const std::vector<Tool> &tools) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(nullptr);
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
            yyjson_doc *pdoc = yyjson_read(t.parameters_json.data(), t.parameters_json.size(), 0);
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

    char *json = yyjson_mut_write(doc, 0, nullptr);
    std::string body = json ? std::string(json) : std::string();
    std::free(json);
    yyjson_mut_doc_free(doc);
    return body;
}

ChatResult chat_completion_stream(const LlmConfig &cfg,
                                  const std::vector<ChatMessage> &messages,
                                  const std::vector<Tool> &tools,
                                  const ChunkCallback &on_chunk) {
    ChatResult result;
    const std::string body = build_chat_body(cfg, messages, tools);
    if (body.empty()) {
        result.error = "failed to build request body";
        return result;
    }

    // Split the config URL into scheme / host[:port] / path prefix.
    std::string rest = cfg.url;
    std::string scheme = "http";
    if (rest.rfind("https://", 0) == 0) {
        scheme = "https";
        rest = rest.substr(8);
    } else if (rest.rfind("http://", 0) == 0) {
        rest = rest.substr(7);
    }
    std::string host = rest;
    std::string path_prefix = "/";
    size_t slash = rest.find('/');
    if (slash != std::string::npos) {
        host = rest.substr(0, slash);
        path_prefix = rest.substr(slash);
    }
    int port = scheme == "https" ? 443 : 80;
    size_t colon = host.find(':');
    if (colon != std::string::npos) {
        port = std::atoi(host.substr(colon + 1).c_str());
        host = host.substr(0, colon);
    }
    if (host.empty()) {
        result.error = "invalid config url: " + cfg.url;
        return result;
    }
    std::string path = path_prefix;
    if (path.empty() || path.back() != '/') {
        path += '/';
    }
    path += "chat/completions";

    if (scheme == "https") {
        // cpp-httplib needs CPPHTTPLIB_OPENSSL_SUPPORT for https; the config
        // used by this demo is plain http, so report a clear error instead of
        // silently failing at connect time.
        result.error = "https endpoints require building cpp-httplib with "
                       "CPPHTTPLIB_OPENSSL_SUPPORT; current config url is: " + cfg.url;
        return result;
    }

    httplib::Client cli(host, port);
    cli.set_connection_timeout(30);
    cli.set_read_timeout(180, 0);
    cli.set_write_timeout(30, 0);

    httplib::Headers headers = {
        // Content-Type is added by Post() below; keep this map free of
        // duplicates (some OpenAI-compatible gateways are picky).
        {"Accept", "text/event-stream"},
        {"Authorization", "Bearer " + cfg.api_key},
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
        std::vector<ToolCall> acc_tool_calls;
        std::string finish_reason;

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

        httplib::Result res = cli.Post(path, headers, body, "application/json", receiver);
        for (const auto &chunk : parser.finish()) {
            consume(chunk);
        }

        const bool retriable = !res || res->status == 403 || res->status == 408
                               || res->status == 429 || res->status >= 500;
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

} // namespace openai
