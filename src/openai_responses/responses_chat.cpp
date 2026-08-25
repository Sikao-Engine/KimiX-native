// responses_chat.cpp - OpenAI Responses API streaming workflow.
//
// Transport uses cpp-httplib; HTTPS is enabled by CPPHTTPLIB_OPENSSL_SUPPORT
// (OpenSSL provided by the kimix-openssl target), so this code is fully
// cross-platform. SSE bytes are fed into openai_responses/stream_parser.h.

#include "openai_responses/responses_chat.h"

#include <httplib.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "yyjson.h"

namespace openai_responses {

bool load_config(const std::string &path, ResponsesConfig &cfg) {
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
        yyjson_val *v = yyjson_obj_get(root, "max_tokens");
        if (yyjson_is_int(v)) {
            cfg.max_tokens = (int)yyjson_get_int(v);
        }
        v = yyjson_obj_get(root, "max_context_size");
        if (yyjson_is_int(v)) {
            cfg.max_context_size = (int)yyjson_get_int(v);
        }
        ok = !cfg.model.empty() && !cfg.url.empty();
    }
    yyjson_doc_free(doc);
    return ok;
}

// Derive the Responses API base URL. Some config files carry a provider-
// specific mount (e.g. ".../anthropic" for the Anthropic-compatible endpoint);
// the Responses API lives at the base + "/v1/responses", so strip that suffix.
std::string responses_base_url(const std::string &url) {
    std::string base = url;
    while (base.size() > 1 && base.back() == '/') {
        base.pop_back();
    }
    if (base.size() >= 10 && base.compare(base.size() - 10, 10, "/anthropic") == 0) {
        base.resize(base.size() - 10);
    }
    return base;
}

std::string build_responses_body(const ResponsesConfig &cfg,
                                 const std::vector<InputItem> &input,
                                 const std::vector<Tool> &tools) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(nullptr);
    if (!doc) {
        return {};
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "model", cfg.model.c_str());
    yyjson_mut_obj_add_bool(doc, root, "stream", true);
    yyjson_mut_obj_add_bool(doc, root, "store", false);
    yyjson_mut_obj_add_int(doc, root, "max_output_tokens", cfg.max_tokens);

    yyjson_mut_val *input_arr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "input", input_arr);
    for (const auto &item : input) {
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        yyjson_mut_arr_append(input_arr, obj);

        if (item.type == "message") {
            yyjson_mut_obj_add_str(doc, obj, "role", item.role.c_str());
            if (item.role == "assistant") {
                // Assistant messages round-trip as output_text content blocks.
                yyjson_mut_val *content = yyjson_mut_arr(doc);
                yyjson_mut_obj_add_val(doc, obj, "content", content);
                yyjson_mut_val *block = yyjson_mut_obj(doc);
                yyjson_mut_arr_append(content, block);
                yyjson_mut_obj_add_str(doc, block, "type", "output_text");
                yyjson_mut_obj_add_str(doc, block, "text", item.content.c_str());
            } else {
                yyjson_mut_obj_add_str(doc, obj, "content", item.content.c_str());
            }
        } else if (item.type == "reasoning") {
            // DeepSeek Responses: reasoning input items carry the verbatim
            // streamed chain of thought as reasoning_text content blocks.
            yyjson_mut_obj_add_str(doc, obj, "type", "reasoning");
            if (!item.item_id.empty()) {
                yyjson_mut_obj_add_str(doc, obj, "id", item.item_id.c_str());
            }
            yyjson_mut_val *content = yyjson_mut_arr(doc);
            yyjson_mut_obj_add_val(doc, obj, "content", content);
            yyjson_mut_val *block = yyjson_mut_obj(doc);
            yyjson_mut_arr_append(content, block);
            yyjson_mut_obj_add_str(doc, block, "type", "reasoning_text");
            yyjson_mut_obj_add_str(doc, block, "text", item.content.c_str());
        } else if (item.type == "function_call") {
            yyjson_mut_obj_add_str(doc, obj, "type", "function_call");
            yyjson_mut_obj_add_str(doc, obj, "call_id", item.call_id.c_str());
            yyjson_mut_obj_add_str(doc, obj, "name", item.name.c_str());
            yyjson_mut_obj_add_str(doc, obj, "arguments", item.arguments.c_str());
        } else if (item.type == "function_call_output") {
            yyjson_mut_obj_add_str(doc, obj, "type", "function_call_output");
            yyjson_mut_obj_add_str(doc, obj, "call_id", item.call_id.c_str());
            yyjson_mut_obj_add_str(doc, obj, "output", item.content.c_str());
        }
    }

    if (!tools.empty()) {
        yyjson_mut_val *tools_arr = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, root, "tools", tools_arr);
        for (const auto &t : tools) {
            yyjson_mut_val *tool_obj = yyjson_mut_obj(doc);
            yyjson_mut_arr_append(tools_arr, tool_obj);
            yyjson_mut_obj_add_str(doc, tool_obj, "type", "function");
            yyjson_mut_obj_add_str(doc, tool_obj, "name", t.name.c_str());
            yyjson_mut_obj_add_str(doc, tool_obj, "description", t.description.c_str());
            yyjson_doc *sdoc = yyjson_read(t.parameters_json.data(), t.parameters_json.size(), 0);
            yyjson_mut_val *params = nullptr;
            if (sdoc) {
                params = yyjson_val_mut_copy(doc, yyjson_doc_get_root(sdoc));
                yyjson_doc_free(sdoc);
            }
            if (!params) {
                params = yyjson_mut_obj(doc);
            }
            yyjson_mut_obj_add_val(doc, tool_obj, "parameters", params);
            yyjson_mut_obj_add_bool(doc, tool_obj, "strict", false);
        }
    }

    // Always enable reasoning (mirrors openai_responses.py: this provider
    // always generates reasoning; effort comes from the config).
    yyjson_mut_val *reasoning = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, root, "reasoning", reasoning);
    yyjson_mut_obj_add_str(doc, reasoning, "effort", cfg.thinking_effort.c_str());
    yyjson_mut_obj_add_str(doc, reasoning, "summary", "auto");

    char *json = yyjson_mut_write(doc, 0, nullptr);
    std::string body = json ? std::string(json) : std::string();
    std::free(json);
    yyjson_mut_doc_free(doc);
    return body;
}

ChatResult responses_completion_stream(const ResponsesConfig &cfg,
                                       const std::vector<InputItem> &input,
                                       const std::vector<Tool> &tools,
                                       const EventCallback &on_event) {
    ChatResult result;
    const std::string body = build_responses_body(cfg, input, tools);
    if (body.empty()) {
        result.error = "failed to build request body";
        return result;
    }

    const std::string base = responses_base_url(cfg.url);
    if (base.empty()) {
        result.error = "invalid config url: " + cfg.url;
        return result;
    }

    // Split the base URL into scheme / host[:port] / path prefix.
    std::string rest = base;
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
    path += "v1/responses";

    // httplib::Client("https://host:port") transparently picks SSLClient when
    // CPPHTTPLIB_OPENSSL_SUPPORT is enabled.
    httplib::Client cli(scheme + "://" + host + ":" + std::to_string(port));
    cli.set_connection_timeout(30);
    cli.set_read_timeout(300, 0);
    cli.set_write_timeout(30, 0);

    httplib::Headers headers = {
        // Content-Type is added by Post() below; keep this map free of
        // duplicates (some gateways are picky).
        {"Accept", "text/event-stream"},
        {"Authorization", "Bearer " + cfg.api_key},
    };

    // Transient failures (403/408/429/5xx, dropped connections) are retried a
    // couple of times with a short pause.
    constexpr int kMaxAttempts = 3;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        // Reset accumulators for this attempt.
        result.text.clear();
        result.reasoning.clear();
        result.reasoning_item_id.clear();
        result.tool_calls.clear();
        result.input_tokens = 0;
        result.output_tokens = 0;
        result.cached_tokens = 0;
        result.total_tokens = 0;

        StreamParser parser;
        std::vector<FunctionCall> acc_tool_calls;
        std::map<std::string, size_t> tool_item_pos;
        bool reasoning_streamed = false;

        const auto consume = [&](const StreamEvent &ev) {
            if (on_event) {
                on_event(ev);
            }
            if (ev.type == "response.output_item.added") {
                if (ev.item_type == "reasoning") {
                    if (result.reasoning_item_id.empty()) {
                        result.reasoning_item_id = ev.item_id;
                    }
                } else if (ev.item_type == "function_call") {
                    size_t pos = acc_tool_calls.size();
                    tool_item_pos[ev.item_id] = pos;
                    acc_tool_calls.push_back({ev.call_id, ev.name, ev.arguments});
                }
            } else if (ev.type == "response.reasoning_text.delta") {
                result.reasoning += ev.delta;
                reasoning_streamed = true;
            } else if (ev.type == "response.reasoning_text.done") {
                if (!reasoning_streamed) {
                    result.reasoning = ev.text;
                }
            } else if (ev.type == "response.reasoning_summary_text.delta") {
                // OpenAI-style summarized reasoning (DeepSeek uses
                // reasoning_text deltas; this covers other backends).
                result.reasoning += ev.delta;
            } else if (ev.type == "response.output_text.delta") {
                result.text += ev.delta;
            } else if (ev.type == "response.function_call_arguments.delta") {
                auto it = tool_item_pos.find(ev.item_ref);
                if (it != tool_item_pos.end()) {
                    acc_tool_calls[it->second].arguments += ev.delta;
                } else if (!acc_tool_calls.empty()) {
                    acc_tool_calls.back().arguments += ev.delta;
                }
            } else if (ev.type == "response.function_call_arguments.done") {
                auto it = tool_item_pos.find(ev.item_ref);
                if (it != tool_item_pos.end()) {
                    acc_tool_calls[it->second].arguments = ev.text;
                }
            } else if (ev.type == "response.output_item.done") {
                if (ev.item_type == "reasoning") {
                    if (result.reasoning_item_id.empty()) {
                        result.reasoning_item_id = ev.item_id;
                    }
                    // Fallback: backends that only attach reasoning text to the
                    // completed item (no delta events).
                    if (result.reasoning.empty() && !ev.text.empty()) {
                        result.reasoning = ev.text;
                    }
                }
            } else if (ev.type == "response.completed") {
                if (ev.usage.has) {
                    result.input_tokens = ev.usage.input_tokens;
                    result.output_tokens = ev.usage.output_tokens;
                    result.cached_tokens = ev.usage.cached_tokens;
                    result.total_tokens = ev.usage.total_tokens;
                }
            }
        };

        httplib::ContentReceiver receiver = [&](const char *data, size_t len) -> bool {
            for (const auto &ev : parser.feed(data, len)) {
                consume(ev);
            }
            return true;
        };

        httplib::Result res = cli.Post(path, headers, body, "application/json", receiver);
        for (const auto &ev : parser.finish()) {
            consume(ev);
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

} // namespace openai_responses
