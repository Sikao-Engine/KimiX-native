// anthropic_chat.cpp - Anthropic Messages API streaming workflow.
//
// Transport is the hand-written kimix::net HTTP(S) client (llm/http_client.h,
// raw sockets + the vendored mbedTLS), so this code is fully cross-platform.
// SSE bytes are fed into anthropic/stream_parser.h exactly like the OpenAI
// demo feeds its parser. The http_client.h header pulls <core/kimix_core.h>
// first, preserving the winsock2-before-windows.h order in the unity batch.

#include "llm/http_client.h"

#include "llm/anthropic/anthropic_chat.h"

#include <chrono>
#include <cstdio>

// Rate-limit backoff as a fiber-aware wait: it yields the calling fiber when a
// scheduler is bound and is a plain sleep otherwise (see the fiber skill).
#include <core/fiber.h>

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

// E5 prompt caching marker: cache_control = {"type": "ephemeral"}
// (CacheControlEphemeralParam in anthropic.py).
void add_cache_control(yyjson_mut_doc *doc, yyjson_mut_val *block) {
    yyjson_mut_val *cc = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, block, "cache_control", cc);
    yyjson_mut_obj_add_str(doc, cc, "type", "ephemeral");
}

// ── E1/E2: _image_url_part_to_anthropic (anthropic.py:914-943) ─────────────
// data:[<media-type>][;base64],<data> -> a base64 image source; any other URL
// -> a url source. An invalid data URL or a media type outside the accepted
// set degrades to the reference's exact error TEXT block.
void add_image_block(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                     const kimix::llm::ContentPart &part) {
    yyjson_mut_val *block = yyjson_mut_obj(doc);
    yyjson_mut_arr_append(arr, block);
    const kimix::string_view url = part.url;
    constexpr kimix::string_view k_data = "data:";
    if (url.starts_with(k_data)) {
        const size_t sep = url.find(";base64,");
        if (sep == kimix::string_view::npos) {
            yyjson_mut_obj_add_str(doc, block, "type", "text");
            // Built strings are COPIED into the document (add_json_str
            // references the caller's bytes, which die with the local).
            const kimix::string error =
                "Error: Invalid data URL for image: " + kimix::string(url);
            yyjson_mut_obj_add_strncpy(doc, block, "text", error.data(),
                                       error.size());
            return;
        }
        const kimix::string_view media_type = url.substr(5, sep - 5);
        const kimix::string_view data = url.substr(sep + 8);
        const bool accepted =
            media_type == "image/png" || media_type == "image/jpeg" ||
            media_type == "image/gif" || media_type == "image/webp";
        if (!accepted) {
            yyjson_mut_obj_add_str(doc, block, "type", "text");
            const kimix::string error =
                "Error: Unsupported media type for base64 image: " +
                kimix::string(media_type) + ", url: " + kimix::string(url);
            yyjson_mut_obj_add_strncpy(doc, block, "text", error.data(),
                                       error.size());
            return;
        }
        yyjson_mut_obj_add_str(doc, block, "type", "image");
        yyjson_mut_val *source = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, block, "source", source);
        yyjson_mut_obj_add_str(doc, source, "type", "base64");
        add_json_str(doc, source, "data", data);
        add_json_str(doc, source, "media_type", media_type);
        return;
    }
    yyjson_mut_obj_add_str(doc, block, "type", "image");
    yyjson_mut_val *source = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, block, "source", source);
    yyjson_mut_obj_add_str(doc, source, "type", "url");
    add_json_str(doc, source, "url", url);
}

// The content-block array of one message carrying parts (the reference's
// user/assistant loop: text -> TextBlockParam, image -> image block,
// everything else skipped).
void add_part_blocks(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                     const kimix::vector<kimix::llm::ContentPart> &parts) {
    for (const auto &part : parts) {
        using K = kimix::llm::ContentPart::Kind;
        if (part.kind == K::text) {
            yyjson_mut_val *block = yyjson_mut_obj(doc);
            yyjson_mut_arr_append(arr, block);
            yyjson_mut_obj_add_str(doc, block, "type", "text");
            add_json_str(doc, block, "text", part.text);
        } else if (part.kind == K::image_url) {
            add_image_block(doc, arr, part);
        }
        // audio/video/think: `continue` in the reference loop.
    }
}

} // namespace detail

kimix::string build_messages_body(const Config &cfg,
                                  const kimix::string &system,
                                  const kimix::vector<ChatMessage> &messages,
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
    yyjson_mut_obj_add_int(doc, root, "max_tokens", cfg.max_tokens);
    yyjson_mut_obj_add_bool(doc, root, "stream", true);

    // E11: sampling controls (kimi_cli/llm.py routes temperature/top_p through
    // extra_body for anthropic, which puts them at the top level of the body);
    // only serialized when configured (0 == unset).
    if (cfg.temperature != 0.0) {
        yyjson_mut_obj_add_real(doc, root, "temperature", cfg.temperature);
    }
    if (cfg.top_p != 0.0) {
        yyjson_mut_obj_add_real(doc, root, "top_p", cfg.top_p);
    }

    // E5 prompt caching: the system prompt is a one-block text array whose
    // block carries cache_control = {"type":"ephemeral"} (anthropic.py
    // generate()). With the feature disabled the plain-string form is kept.
    if (!system.empty()) {
        if (cfg.anthropic_cache_control) {
            yyjson_mut_val *system_arr = yyjson_mut_arr(doc);
            yyjson_mut_obj_add_val(doc, root, "system", system_arr);
            yyjson_mut_val *block = yyjson_mut_obj(doc);
            yyjson_mut_arr_append(system_arr, block);
            yyjson_mut_obj_add_str(doc, block, "type", "text");
            add_json_str(doc, block, "text", system);
            detail::add_cache_control(doc, block);
        } else {
            add_json_str(doc, root, "system", system);
        }
    }

    // The last content block of the serialized conversation gets cache_control
    // (E5): the reference walks the CONVERTED messages and tags the last block
    // of the last message unless it is a thinking block. Track it while
    // building; a message whose content is a plain string (plain user text) is
    // left untouched, exactly like the reference's `isinstance(last_content,
    // list)` guard.
    yyjson_mut_val *last_content_arr = nullptr;
    yyjson_mut_val *last_block = nullptr;
    bool last_block_is_thinking = false;

    yyjson_mut_val *msg_arr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "messages", msg_arr);
    for (const auto &m : messages) {
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        add_json_str(doc, obj, "role", m.role);
        // Reset the per-message block tracking; only array content updates the
        // conversation-level "last block".
        yyjson_mut_val *content_arr = nullptr;
        last_block = nullptr;
        last_block_is_thinking = false;
        if (m.role == "user" &&
            (!m.tool_results.empty() || !m.tool_result_id.empty())) {
            // User tool_result content block(s). `tool_results` carries the
            // merged blocks of one assistant turn (E4); `tool_result_id` is
            // the single-result legacy form kept for compatibility.
            content_arr = yyjson_mut_arr(doc);
            yyjson_mut_obj_add_val(doc, obj, "content", content_arr);
            if (!m.tool_results.empty()) {
                for (const auto &tr : m.tool_results) {
                    yyjson_mut_val *block = yyjson_mut_obj(doc);
                    yyjson_mut_arr_append(content_arr, block);
                    yyjson_mut_obj_add_str(doc, block, "type", "tool_result");
                    add_json_str(doc, block, "tool_use_id", tr.tool_use_id);
                    add_json_str(doc, block, "content", tr.content);
                    last_block = block;
                    last_block_is_thinking = false;
                }
            } else {
                yyjson_mut_val *block = yyjson_mut_obj(doc);
                yyjson_mut_arr_append(content_arr, block);
                yyjson_mut_obj_add_str(doc, block, "type", "tool_result");
                add_json_str(doc, block, "tool_use_id", m.tool_result_id);
                add_json_str(doc, block, "content", m.tool_result_content);
                last_block = block;
            }
        } else if (m.role == "assistant") {
            // Assistant content is a block list: thinking (required by some
            // backends, e.g. DeepSeek, when thinking mode is on), then text,
            // then tool_use blocks.
            content_arr = yyjson_mut_arr(doc);
            yyjson_mut_obj_add_val(doc, obj, "content", content_arr);
            if (!m.thinking.empty() || !m.thinking_signature.empty()) {
                yyjson_mut_val *block = yyjson_mut_obj(doc);
                yyjson_mut_arr_append(content_arr, block);
                yyjson_mut_obj_add_str(doc, block, "type", "thinking");
                add_json_str(doc, block, "thinking", m.thinking);
                if (!m.thinking_signature.empty()) {
                    add_json_str(doc, block, "signature", m.thinking_signature);
                }
                // A thinking block never receives cache_control (the
                // reference's `case "thinking" | "redacted_thinking": pass`).
                last_block = block;
                last_block_is_thinking = true;
            }
            if (!m.parts.empty()) {
                // E1/E2: the parts carry the text blocks (and any media);
                // `m.text` is the same backbone and is skipped to avoid
                // doubling it.
                detail::add_part_blocks(doc, content_arr, m.parts);
                last_block = nullptr;
                last_block_is_thinking = false;
            } else if (!m.text.empty()) {
                yyjson_mut_val *block = yyjson_mut_obj(doc);
                yyjson_mut_arr_append(content_arr, block);
                yyjson_mut_obj_add_str(doc, block, "type", "text");
                add_json_str(doc, block, "text", m.text);
                last_block = block;
                last_block_is_thinking = false;
            }
            for (const auto &tu : m.tool_uses) {
                yyjson_mut_val *block = yyjson_mut_obj(doc);
                yyjson_mut_arr_append(content_arr, block);
                yyjson_mut_obj_add_str(doc, block, "type", "tool_use");
                add_json_str(doc, block, "id", tu.id);
                add_json_str(doc, block, "name", tu.name);
                if (!add_json_fragment(doc, block, "input", tu.input_json)) {
                    yyjson_mut_obj_add_val(doc, block, "input",
                                           yyjson_mut_obj(doc));
                }
                last_block = block;
                last_block_is_thinking = false;
            }
        } else if (!m.parts.empty()) {
            // E1/E2: user content parts -> a block list (text + image blocks;
            // the reference's loop appends text blocks unconditionally, so a
            // non-empty parts list never yields an empty array).
            content_arr = yyjson_mut_arr(doc);
            yyjson_mut_obj_add_val(doc, obj, "content", content_arr);
            detail::add_part_blocks(doc, content_arr, m.parts);
        } else {
            // Plain user text: a string content, not a block list - the
            // reference never tags it with cache_control.
            add_json_str(doc, obj, "content", m.text);
        }
        yyjson_mut_arr_append(msg_arr, obj);
        if (content_arr != nullptr) {
            // Array content: this message becomes the cache-control candidate
            // (the last message wins).
            last_content_arr = content_arr;
        }
    }

    // E5: tag the last content block of the last message with cache_control
    // (skipped for thinking blocks and for string content) - anthropic.py
    // generate() "inject cache control in the last content".
    if (cfg.anthropic_cache_control && last_content_arr != nullptr &&
        last_block != nullptr && !last_block_is_thinking) {
        detail::add_cache_control(doc, last_block);
    }

    if (!tools.empty()) {
        yyjson_mut_val *tools_arr = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, root, "tools", tools_arr);
        for (const auto &t : tools) {
            yyjson_mut_val *tool_obj = yyjson_mut_obj(doc);
            yyjson_mut_arr_append(tools_arr, tool_obj);
            add_json_str(doc, tool_obj, "name", t.name);
            add_json_str(doc, tool_obj, "description", t.description);
            if (!add_json_fragment(doc, tool_obj, "input_schema",
                                   t.input_schema_json)) {
                yyjson_mut_obj_add_val(doc, tool_obj, "input_schema",
                                        yyjson_mut_obj(doc));
            }
        }
        // E5: the LAST tool definition carries cache_control (anthropic.py
        // generate(): tools_[-1]["cache_control"] = ...).
        if (cfg.anthropic_cache_control) {
            detail::add_cache_control(doc, yyjson_mut_arr_get_last(tools_arr));
        }
    }

    // Thinking configuration. E7: when thinking is OFF (Config.enable_thinking
    // == false or thinking_effort "off", the CLI's --no-think) NO thinking
    // parameters/budget are sent at all - the API treats an absent thinking
    // object as disabled, and several OpenAI-compatible /anthropic endpoints
    // reject thinking blocks they did not enable. When ON, the legacy
    // budget-based thinking maps thinking_effort to a token budget (mirrors
    // anthropic.py's budgets table).
    if (thinking_enabled(cfg)) {
        yyjson_mut_val *thinking = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, root, "thinking", thinking);
        yyjson_mut_obj_add_str(doc, thinking, "type", "enabled");
        yyjson_mut_obj_add_int(doc, thinking, "budget_tokens",
                               detail::thinking_budget(cfg.thinking_effort));
    }

    return write_json_doc(doc, *err);
}

  ChatResult chat_completion_stream(const Config &cfg,
                                    const kimix::string &system,
                                    const kimix::vector<ChatMessage> &messages,
                                    const kimix::vector<Tool> &tools,
                                    const EventCallback &on_event,
                                    const AbortCheck *abort) {
    ChatResult result;
    kimix::string why;
    const kimix::string body =
        build_messages_body(cfg, system, messages, tools, &why);
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
    const kimix::string path = join_path(ep.path_prefix, "v1/messages");

    // kimix::net::Client("https://host:port") - HTTPS is native (mbedTLS).
    // On Windows the full-chain session verifier checks the peer chain with
    // the system cert engine (llm/http_client.cpp policy).
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
        {"x-api-key", std::string(cfg.api_key)},
        {"anthropic-version", "2023-06-01"},
    };

    // Transient failures (403/408/429/5xx, dropped connections) are retried a
    // couple of times with the shared _RateLimitAwareWait backoff
    // (llm/common.h).
    constexpr int kMaxAttempts = 3;
    uint64_t backoff_rng = 0xC0FFEE1234567ull; // xorshift jitter state
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
              result.thinking.clear();
              result.tool_uses.clear();
              result.error = "request aborted";
              return result;
          }

        // A 200 body from which nothing parsed (garbage / non-SSE / HTML error
        // page) is unusable; treat it like a transient failure and retry.
        const bool unusable = acc_tool_uses.empty() && result.text.empty()
                && result.thinking.empty();
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

        result.thinking = std::move(thinking);
        result.signature = std::move(signature);
        if (unusable) {
            // No text, no thinking, no tool calls: the empty / think-only
            // response the reference raises APIEmptyResponseError for.
            result.error_kind = TransportErrorKind::empty_response;
            result.error = "backend returned an unusable response body "
                "(no parseable events: invalid JSON or empty stream)";
            return result;
        }
        result.tool_uses = std::move(acc_tool_uses);
        result.ok = true;
        return result;
    }

    result.error = "exhausted retries";
    return result;
}

} // namespace kimix::llm::anthropic
