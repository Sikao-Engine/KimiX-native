// responses_chat.cpp - OpenAI Responses API streaming workflow.
//
// Transport uses cpp-httplib; HTTPS is enabled by CPPHTTPLIB_MBEDTLS_SUPPORT
// (Mbed TLS provided by the kimix-mbedtls target), so this code is fully
// cross-platform. SSE bytes are fed into openai_responses/stream_parser.h.
//
// <httplib.h> comes first so winsock2.h is included before
// <core/kimix_core.h> pulls in <windows.h> (windows.h-before-winsock2.h
// breaks ws2tcpip.h on Windows; unity build merges these TUs).

#include <httplib.h>

#include "llm/http_tls.h"

#include "llm/openai_responses/responses_chat.h"

#include <chrono>
#include <cstdio>
#include <thread>

#include "yyjson.h"

#include "llm/yyjson_alc.h"

namespace kimix::llm::openai_responses {

// Derive the Responses API base URL. Some config files carry a provider-
// specific mount (e.g. ".../anthropic" for the Anthropic-compatible endpoint);
// the Responses API lives at the base + "/v1/responses", so strip that suffix.
kimix::string responses_base_url(const kimix::string &url) {
    kimix::string base = url;
    while (base.size() > 1 && base.back() == '/') {
        base.pop_back();
    }
    if (base.size() >= 10 && base.compare(base.size() - 10, 10, "/anthropic") == 0) {
        base.resize(base.size() - 10);
    }
    return base;
}

kimix::string build_responses_body(const Config &cfg,
                                   const kimix::vector<InputItem> &input,
                                   const kimix::vector<Tool> &tools,
                                   kimix::string *out_error) {
    // All text goes through add_json_str()/add_json_fragment(): yyjson refuses
    // to write a document that holds invalid UTF-8, and its strlen-based add_str
    // helpers end a string at an embedded '\0'.
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
    yyjson_mut_obj_add_bool(doc, root, "store", false);
    yyjson_mut_obj_add_int(doc, root, "max_output_tokens", cfg.max_tokens);

    yyjson_mut_val *input_arr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "input", input_arr);
    for (const auto &item : input) {
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        yyjson_mut_arr_append(input_arr, obj);

        if (item.type == "message") {
            add_json_str(doc, obj, "role", item.role);
            if (item.role == "assistant") {
                // Assistant messages round-trip as output_text content blocks.
                yyjson_mut_val *content = yyjson_mut_arr(doc);
                yyjson_mut_obj_add_val(doc, obj, "content", content);
                yyjson_mut_val *block = yyjson_mut_obj(doc);
                yyjson_mut_arr_append(content, block);
                yyjson_mut_obj_add_str(doc, block, "type", "output_text");
                add_json_str(doc, block, "text", item.content);
            } else {
                add_json_str(doc, obj, "content", item.content);
            }
        } else if (item.type == "reasoning") {
            // DeepSeek Responses: reasoning input items carry the verbatim
            // streamed chain of thought as reasoning_text content blocks.
            yyjson_mut_obj_add_str(doc, obj, "type", "reasoning");
            if (!item.item_id.empty()) {
                add_json_str(doc, obj, "id", item.item_id);
            }
            yyjson_mut_val *content = yyjson_mut_arr(doc);
            yyjson_mut_obj_add_val(doc, obj, "content", content);
            yyjson_mut_val *block = yyjson_mut_obj(doc);
            yyjson_mut_arr_append(content, block);
            yyjson_mut_obj_add_str(doc, block, "type", "reasoning_text");
            add_json_str(doc, block, "text", item.content);
        } else if (item.type == "function_call") {
            yyjson_mut_obj_add_str(doc, obj, "type", "function_call");
            add_json_str(doc, obj, "call_id", item.call_id);
            add_json_str(doc, obj, "name", item.name);
            add_json_str(doc, obj, "arguments", item.arguments);
        } else if (item.type == "function_call_output") {
            yyjson_mut_obj_add_str(doc, obj, "type", "function_call_output");
            add_json_str(doc, obj, "call_id", item.call_id);
            add_json_str(doc, obj, "output", item.content);
        }
    }

    if (!tools.empty()) {
        yyjson_mut_val *tools_arr = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, root, "tools", tools_arr);
        for (const auto &t : tools) {
            yyjson_mut_val *tool_obj = yyjson_mut_obj(doc);
            yyjson_mut_arr_append(tools_arr, tool_obj);
            yyjson_mut_obj_add_str(doc, tool_obj, "type", "function");
            add_json_str(doc, tool_obj, "name", t.name);
            add_json_str(doc, tool_obj, "description", t.description);
            if (!add_json_fragment(doc, tool_obj, "parameters",
                                   t.parameters_json)) {
                yyjson_mut_obj_add_val(doc, tool_obj, "parameters",
                                        yyjson_mut_obj(doc));
            }
            yyjson_mut_obj_add_bool(doc, tool_obj, "strict", false);
        }
    }

    // Always enable reasoning (mirrors openai_responses.py: this provider
    // always generates reasoning; effort comes from the config).
    yyjson_mut_val *reasoning = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, root, "reasoning", reasoning);
    add_json_str(doc, reasoning, "effort", cfg.thinking_effort);
    add_json_str(doc, reasoning, "summary", "auto");

    return write_json_doc(doc, *err);
}

ChatResult responses_completion_stream(const Config &cfg,
                                       const kimix::vector<InputItem> &input,
                                       const kimix::vector<Tool> &tools,
                                       const EventCallback &on_event) {
    ChatResult result;
    kimix::string why;
    const kimix::string body = build_responses_body(cfg, input, tools, &why);
    if (body.empty()) {
        result.error = "failed to build request body";
        if (!why.empty()) {
            result.error += ": " + why;
        }
        return result;
    }

    const kimix::string base = responses_base_url(cfg.url);
    if (base.empty()) {
        result.error = "invalid config url: " + cfg.url;
        return result;
    }

    // Split the base URL into scheme / host[:port] / path prefix.
    const Endpoint ep = parse_endpoint(base);
    if (ep.host.empty()) {
        result.error = "invalid config url: " + cfg.url;
        return result;
    }
    const kimix::string path = join_path(ep.path_prefix, "v1/responses");

    // httplib::Client("https://host:port") transparently picks SSLClient when
    // CPPHTTPLIB_MBEDTLS_SUPPORT is enabled.
    httplib::Client cli(std::string(ep.scheme) + "://" + std::string(ep.host) + ":"
                        + std::to_string(ep.port));
    install_windows_tls_verifier(cli, std::string(ep.host));
    cli.set_connection_timeout(30);
    cli.set_read_timeout(300, 0);
    cli.set_write_timeout(30, 0);

    httplib::Headers headers = {
        // Content-Type is added by Post() below; keep this map free of
        // duplicates (some gateways are picky).
        {"Accept", "text/event-stream"},
        {"Authorization", "Bearer " + std::string(cfg.api_key)},
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
        kimix::vector<FunctionCall> acc_tool_calls;
        kimix::map<kimix::string, size_t> tool_item_pos;
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

        httplib::Result res = cli.Post(std::string(path), headers, std::string(body),
                                       "application/json", receiver);
        for (const auto &ev : parser.finish()) {
            consume(ev);
        }

        // A 200 body from which nothing parsed (garbage / non-SSE / HTML error
        // page) is unusable; treat it like a transient failure and retry.
        const bool unusable = acc_tool_calls.empty() && result.text.empty()
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

} // namespace kimix::llm::openai_responses
