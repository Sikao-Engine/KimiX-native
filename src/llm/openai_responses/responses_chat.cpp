// responses_chat.cpp - OpenAI Responses API streaming workflow.
//
// Transport is the hand-written kimix::net HTTP(S) client (llm/http_client.h,
// raw sockets + the vendored mbedTLS), so this code is fully cross-platform.
// SSE bytes are fed into openai_responses/stream_parser.h. The http_client.h
// header pulls <core/kimix_core.h> first, preserving the
// winsock2-before-windows.h order in the unity batch.

#include "llm/http_client.h"

#include "llm/openai_responses/responses_chat.h"

#include <chrono>
#include <cstdio>

// Rate-limit backoff as a fiber-aware wait: it yields the calling fiber when a
// scheduler is bound and is a plain sleep otherwise (see the fiber skill).
#include <core/fiber.h>

#include "yyjson.h"

#include "llm/yyjson_alc.h"

namespace kimix::llm::openai_responses {

// ── E1/E2: content-part mapping (openai_responses.py:513-625) ──────────────
namespace detail {

// _parse_audio_url(url) -> (file_url, file_data, ext). data:audio/ URIs
// decode to (None, b64, ext) with ext "mp3"|"wav"|None; http(s) URLs to
// (url, None, None); anything else to (None, None, None).
void parse_audio_url(kimix::string_view url, kimix::string_view &file_url,
                     kimix::string_view &file_data, kimix::string_view &ext) {
    file_url = {};
    file_data = {};
    ext = {};
    constexpr kimix::string_view k_audio_prefix = "data:audio/";
    if (url.starts_with(k_audio_prefix)) {
        const size_t comma = url.find(',');
        if (comma == kimix::string_view::npos) {
            return;
        }
        const kimix::string_view header = url.substr(0, comma);
        const size_t slash = header.find('/');
        if (slash == kimix::string_view::npos) {
            return;
        }
        kimix::string_view subtype = header.substr(slash + 1);
        const size_t semi = subtype.find(';');
        if (semi != kimix::string_view::npos) {
            subtype = subtype.substr(0, semi);
        }
        if (subtype == "mp3" || subtype == "mpeg") {
            ext = "mp3";
        } else if (subtype == "wav") {
            ext = "wav";
        } else {
            return; // unsupported codec: no ext -> no item
        }
        file_data = url.substr(comma + 1);
        return;
    }
    if (url.starts_with("http://") || url.starts_with("https://")) {
        file_url = url;
    }
}

// One input_file block for an audio part; false when the URL maps to nothing
// (unsupported codec / scheme).
bool add_audio_block(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                     const kimix::llm::ContentPart &part, bool for_output) {
    kimix::string_view file_url;
    kimix::string_view file_data;
    kimix::string_view ext;
    parse_audio_url(part.url, file_url, file_data, ext);
    if (!file_url.empty()) {
        yyjson_mut_val *block = yyjson_mut_obj(doc);
        yyjson_mut_arr_append(arr, block);
        yyjson_mut_obj_add_str(doc, block, "type", "input_file");
        add_json_str(doc, block, "file_url", file_url);
        return true;
    }
    if (!file_data.empty()) {
        if (!for_output && ext.empty()) {
            return false; // _map_audio_url_to_input_item: no ext -> None
        }
        yyjson_mut_val *block = yyjson_mut_obj(doc);
        yyjson_mut_arr_append(arr, block);
        yyjson_mut_obj_add_str(doc, block, "type", "input_file");
        add_json_str(doc, block, "file_data", file_data);
        if (!for_output) {
            // item["filename"] = f"inline.{ext}". The string is a local, so
            // it must be COPIED into the document (add_json_str references
            // the caller's bytes, which would dangle by the time the body
            // is written).
            const kimix::string filename = "inline." + kimix::string(ext);
            yyjson_mut_obj_add_strncpy(doc, block, "filename", filename.data(),
                                       filename.size());
        }
        return true;
    }
    return false;
}

} // namespace detail

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
    // E6: the Responses API takes `max_output_tokens` (kimi_cli/llm.py
    // openai_responses branch); `max_tokens` is a Chat Completions parameter
    // and must NOT be sent here. 0 == unset (no key on the wire).
    if (cfg.max_tokens > 0) {
        yyjson_mut_obj_add_int(doc, root, "max_output_tokens", cfg.max_tokens);
    }

    yyjson_mut_val *input_arr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "input", input_arr);
    for (const auto &item : input) {
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        yyjson_mut_arr_append(input_arr, obj);

        if (item.type == "message") {
            add_json_str(doc, obj, "role", item.role);
            if (!item.parts.empty()) {
                // E1/E2: user content parts -> an input content block array
                // (_content_parts_to_input_items). Assistant content parts
                // round-trip as output_text blocks, media ignored
                // (_content_parts_to_output_items).
                yyjson_mut_val *content = yyjson_mut_arr(doc);
                yyjson_mut_obj_add_val(doc, obj, "content", content);
                const bool assistant = item.role == "assistant";
                for (const auto &part : item.parts) {
                    using K = kimix::llm::ContentPart::Kind;
                    if (part.kind == K::text) {
                        if (part.text.empty()) {
                            continue;
                        }
                        yyjson_mut_val *block = yyjson_mut_obj(doc);
                        yyjson_mut_arr_append(content, block);
                        add_json_str(doc, block, "type",
                                     assistant ? "output_text" : "input_text");
                        add_json_str(doc, block, "text", part.text);
                        if (assistant) {
                            yyjson_mut_obj_add_val(doc, block, "annotations",
                                                   yyjson_mut_arr(doc));
                        }
                    } else if (!assistant && part.kind == K::image_url) {
                        // default detail ("auto")
                        yyjson_mut_val *block = yyjson_mut_obj(doc);
                        yyjson_mut_arr_append(content, block);
                        yyjson_mut_obj_add_str(doc, block, "type",
                                               "input_image");
                        add_json_str(doc, block, "detail",
                                     part.detail.empty()
                                         ? kimix::string_view("auto")
                                         : kimix::string_view(part.detail));
                        add_json_str(doc, block, "image_url", part.url);
                    } else if (!assistant && part.kind == K::audio_url) {
                        detail::add_audio_block(doc, content, part, false);
                    }
                    // Unknown content - ignore (the reference's `continue`).
                }
            } else if (item.role == "assistant") {
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
            if (!item.parts.empty()) {
                // E1/E2: a tool result carrying media parts serializes its
                // output as a content item list
                // (_message_content_to_function_output_items).
                yyjson_mut_val *output_arr = yyjson_mut_arr(doc);
                yyjson_mut_obj_add_val(doc, obj, "output", output_arr);
                for (const auto &part : item.parts) {
                    using K = kimix::llm::ContentPart::Kind;
                    if (part.kind == K::text) {
                        if (part.text.empty()) {
                            continue;
                        }
                        yyjson_mut_val *block = yyjson_mut_obj(doc);
                        yyjson_mut_arr_append(output_arr, block);
                        yyjson_mut_obj_add_str(doc, block, "type",
                                               "input_text");
                        add_json_str(doc, block, "text", part.text);
                    } else if (part.kind == K::image_url) {
                        yyjson_mut_val *block = yyjson_mut_obj(doc);
                        yyjson_mut_arr_append(output_arr, block);
                        yyjson_mut_obj_add_str(doc, block, "type",
                                               "input_image");
                        add_json_str(doc, block, "image_url", part.url);
                    } else if (part.kind == K::audio_url) {
                        detail::add_audio_block(doc, output_arr, part, true);
                    }
                }
            } else {
                add_json_str(doc, obj, "output", item.content);
            }
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

    // Reasoning effort (mirrors openai_responses.py generate(): the effort is
    // routed through extra_body.reasoning {effort, summary:"auto"}). E7
    // thinking off (Config.enable_thinking == false / effort "off", the CLI's
    // --no-think): the reference maps with_thinking("off") to reasoning_effort
    // None and then sends NO reasoning parameter at all.
    if (thinking_enabled(cfg)) {
        yyjson_mut_val *reasoning = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, root, "reasoning", reasoning);
        add_json_str(doc, reasoning, "effort", cfg.thinking_effort);
        yyjson_mut_obj_add_str(doc, reasoning, "summary", "auto");
    }

    return write_json_doc(doc, *err);
}

  ChatResult responses_completion_stream(const Config &cfg,
                                         const kimix::vector<InputItem> &input,
                                         const kimix::vector<Tool> &tools,
                                         const EventCallback &on_event,
                                         const AbortCheck *abort) {
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

 // kimix::net::Client("https://host:port") - HTTPS is native (mbedTLS).
 kimix::net::Client cli(std::string(ep.scheme) + "://" + std::string(ep.host) +
 ":" + std::to_string(ep.port));
 cli.use_windows_certificate_verifier(std::string(ep.host));
    cli.set_connection_timeout(30);
    cli.set_read_timeout(300, 0);
    cli.set_write_timeout(30, 0);

    kimix::net::Headers headers = {
        // Content-Type is added by Post() below; keep this map free of
        // duplicates (some gateways are picky).
        {"Accept", "text/event-stream"},
        {"Authorization", "Bearer " + std::string(cfg.api_key)},
    };

    // Transient failures (403/408/429/5xx, dropped connections) are retried a
    // couple of times with the shared _RateLimitAwareWait backoff
    // (llm/common.h).
    constexpr int kMaxAttempts = 3;
    uint64_t backoff_rng = 0x5DEECE2D9911ull; // xorshift jitter state
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

 kimix::net::ContentReceiver receiver = [&](const char *data, size_t len) -> bool {
 // G8 cancellation: AbortCheck flipped mid-stream -> stop reading;
 // the client cancels the request (Error::canceled).
              if (abort != nullptr && abort->aborted()) {
                  return false;
              }
              for (const auto &ev : parser.feed(data, len)) {
                  consume(ev);
              }
              return true;
          };

 kimix::net::Result res = cli.Post(std::string(path), headers, std::string(body),
 "application/json", receiver);
          for (const auto &ev : parser.finish()) {
              consume(ev);
          }
          if (abort != nullptr && abort->aborted()) {
              result.text.clear();
              result.reasoning.clear();
              result.tool_calls.clear();
              result.error = "request aborted";
              return result;
          }

        // A 200 body from which nothing parsed (garbage / non-SSE / HTML error
        // page) is unusable; treat it like a transient failure and retry.
        const bool unusable = acc_tool_calls.empty() && result.text.empty()
                && result.reasoning.empty();
        const bool retriable = !res || is_retriable_status(res->status)
                || (res->status == 200 && unusable);
        if (retriable && attempt < kMaxAttempts) {
            // The backoff schedule is the shared _RateLimitAwareWait port
            // (llm/common.h) - the agent-level step retry policy sleeps with
            // the same policy instead of a hardcoded 300ms*attempt.
            const int32_t status = res ? res->status : 0;
            const double retry_after =
                res ? parse_retry_after_seconds(res->get_header_value("Retry-After"))
                    : 0.0;
                kimix::fiber::sleep_for(std::chrono::duration<double>(
                    rate_limit_aware_wait(attempt, status, retry_after, backoff_rng)));
            continue;
        }

        if (!res) {
            result.error_kind =
 res.error() == kimix::net::Error::timeout ||
 res.error() == kimix::net::Error::connection_timeout
 ? TransportErrorKind::timeout
 : TransportErrorKind::connection;
 result.error = "http error: " + std::string(kimix::net::to_string(res.error()));
            return result;
        }
        if (res->status != 200) {
            result.error_kind = TransportErrorKind::http;
            result.error_status = res->status;
            result.retry_after_seconds =
                parse_retry_after_seconds(res->get_header_value("Retry-After"));
            result.error = "http status " + std::to_string(res->status) + ": "
                           + res->body.substr(0, 500);
            return result;
        }

        if (unusable) {
            // No text, no reasoning, no tool calls: the empty / think-only
            // response the reference raises APIEmptyResponseError for.
            result.error_kind = TransportErrorKind::empty_response;
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
