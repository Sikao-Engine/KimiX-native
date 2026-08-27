// stream_parser.h - Minimal Server-Sent Events (SSE) parser for the OpenAI
// Responses API. Header-only; uses yyjson for JSON parsing.
//
// Responses streams are `data:` JSON events. Fields handled here mirror
// openai_responses.py's stream loop: response.reasoning_text.delta / .done,
// response.output_text.delta, response.output_item.added / .done (message,
// reasoning, function_call), response.function_call_arguments.delta / .done,
// and response.completed (usage). Other events (created, in_progress,
// content_part.*, output_text.done, ...) parse with empty payloads and are
// ignored by the caller.

#pragma once

#include <cstddef>
#include <cstdint>

#include <core/kimix_core.h>

#include "yyjson.h"

namespace kimix::llm::openai_responses {

// Usage counters carried by the response.completed event.
struct UsageDelta {
    bool has = false;
    int64_t input_tokens = 0;
    int64_t output_tokens = 0;
    int64_t cached_tokens = 0;
    int64_t total_tokens = 0;
};

// One parsed SSE `data:` event from a /v1/responses stream.
struct StreamEvent {
    kimix::string type; // response.output_item.added | ... | response.completed
    // output_item.added / output_item.done
    kimix::string item_id;
    kimix::string item_type; // message | reasoning | function_call | ...
    kimix::string call_id;   // function_call
    kimix::string name;      // function_call
    kimix::string arguments; // function_call initial arguments
    // text deltas
    kimix::string delta;     // output_text / reasoning_text / summary_text / arguments delta
    kimix::string item_ref;  // item_id carried by delta/done events
    kimix::string text;      // reasoning_text.done full text / function_call_arguments.done
    // response.completed
    UsageDelta usage;
};

// Streaming SSE parser. Feed raw HTTP body bytes; complete events are returned
// immediately, partial events are buffered until finished.
class StreamParser {
public:
    kimix::vector<StreamEvent> feed(const char *data, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            if (data[i] != '\r') {
                buffer_.push_back(data[i]);
            }
        }
        return drain_();
    }

    // Flush any remaining buffered text (normally a no-op for well-formed
    // streams that terminate with a blank line).
    kimix::vector<StreamEvent> finish() {
        kimix::vector<StreamEvent> out = drain_();
        if (!event_lines_.empty()) {
            out.push_back(parse_event_lines_(event_lines_));
            event_lines_.clear();
        }
        return out;
    }

private:
    kimix::string buffer_;
    kimix::vector<kimix::string> event_lines_;

    kimix::vector<StreamEvent> drain_() {
        kimix::vector<StreamEvent> out;
        for (;;) {
            size_t pos = buffer_.find('\n');
            if (pos == kimix::string::npos) {
                break;
            }
            kimix::string line = buffer_.substr(0, pos);
            buffer_.erase(0, pos + 1);
            if (line.empty()) {
                if (!event_lines_.empty()) {
                    out.push_back(parse_event_lines_(event_lines_));
                    event_lines_.clear();
                }
            } else {
                event_lines_.push_back(std::move(line));
            }
        }
        return out;
    }

    static StreamEvent parse_event_lines_(const kimix::vector<kimix::string> &lines) {
        StreamEvent ev;
        kimix::string data;
        for (const auto &line : lines) {
            if (line.rfind("data:", 0) == 0) {
                kimix::string payload = line.substr(5);
                if (!payload.empty() && payload.front() == ' ') {
                    payload.erase(0, 1);
                }
                if (!data.empty()) {
                    data += '\n';
                }
                data += payload;
            }
            // event:/id:/retry: fields are ignored; data.type is authoritative.
        }
        parse_event_json_(data, ev);
        return ev;
    }

    static void parse_event_json_(const kimix::string &data, StreamEvent &ev) {
        yyjson_doc *doc = yyjson_read(data.data(), data.size(), 0);
        if (!doc) {
            return; // ev.type stays empty
        }
        yyjson_val *root = yyjson_doc_get_root(doc);
        if (!yyjson_is_obj(root)) {
            yyjson_doc_free(doc);
            return;
        }
        yyjson_val *v = yyjson_obj_get(root, "type");
        if (yyjson_is_str(v)) {
            ev.type.assign(yyjson_get_str(v), yyjson_get_len(v));
        }

        if (ev.type == "response.output_item.added"
            || ev.type == "response.output_item.done") {
            yyjson_val *item = yyjson_obj_get(root, "item");
            if (yyjson_is_obj(item)) {
                v = yyjson_obj_get(item, "id");
                if (yyjson_is_str(v)) {
                    ev.item_id.assign(yyjson_get_str(v), yyjson_get_len(v));
                }
                v = yyjson_obj_get(item, "type");
                if (yyjson_is_str(v)) {
                    ev.item_type.assign(yyjson_get_str(v), yyjson_get_len(v));
                }
                if (ev.item_type == "function_call") {
                    v = yyjson_obj_get(item, "call_id");
                    if (yyjson_is_str(v)) {
                        ev.call_id.assign(yyjson_get_str(v), yyjson_get_len(v));
                    }
                    v = yyjson_obj_get(item, "name");
                    if (yyjson_is_str(v)) {
                        ev.name.assign(yyjson_get_str(v), yyjson_get_len(v));
                    }
                    v = yyjson_obj_get(item, "arguments");
                    if (yyjson_is_str(v)) {
                        ev.arguments.assign(yyjson_get_str(v), yyjson_get_len(v));
                    }
                } else if (ev.item_type == "reasoning" && ev.type == "response.output_item.done") {
                    // DeepSeek attaches plaintext reasoning as content[]
                    // (reasoning_text parts); OpenAI uses summary[]. Concatenate
                    // either into ev.text for the fallback path.
                    yyjson_val *parts = yyjson_obj_get(item, "content");
                    if (!yyjson_is_arr(parts)) {
                        parts = yyjson_obj_get(item, "summary");
                    }
                    if (yyjson_is_arr(parts)) {
                        yyjson_val *part = nullptr;
                        size_t idx = 0;
                        size_t max = 0;
                        yyjson_arr_foreach(parts, idx, max, part) {
                            yyjson_val *text = yyjson_obj_get(part, "text");
                            if (yyjson_is_str(text)) {
                                ev.text.append(yyjson_get_str(text), yyjson_get_len(text));
                            }
                        }
                    }
                }
            }
        } else if (ev.type == "response.output_text.delta"
                   || ev.type == "response.reasoning_text.delta"
                   || ev.type == "response.reasoning_summary_text.delta"
                   || ev.type == "response.function_call_arguments.delta") {
            v = yyjson_obj_get(root, "delta");
            if (yyjson_is_str(v)) {
                ev.delta.assign(yyjson_get_str(v), yyjson_get_len(v));
            }
            v = yyjson_obj_get(root, "item_id");
            if (yyjson_is_str(v)) {
                ev.item_ref.assign(yyjson_get_str(v), yyjson_get_len(v));
            }
        } else if (ev.type == "response.reasoning_text.done") {
            v = yyjson_obj_get(root, "text");
            if (yyjson_is_str(v)) {
                ev.text.assign(yyjson_get_str(v), yyjson_get_len(v));
            }
            v = yyjson_obj_get(root, "item_id");
            if (yyjson_is_str(v)) {
                ev.item_ref.assign(yyjson_get_str(v), yyjson_get_len(v));
            }
        } else if (ev.type == "response.function_call_arguments.done") {
            v = yyjson_obj_get(root, "arguments");
            if (yyjson_is_str(v)) {
                ev.text.assign(yyjson_get_str(v), yyjson_get_len(v));
            }
            v = yyjson_obj_get(root, "item_id");
            if (yyjson_is_str(v)) {
                ev.item_ref.assign(yyjson_get_str(v), yyjson_get_len(v));
            }
        } else if (ev.type == "response.completed") {
            yyjson_val *response = yyjson_obj_get(root, "response");
            if (yyjson_is_obj(response)) {
                parse_usage_(yyjson_obj_get(response, "usage"), ev.usage);
            }
        }
        yyjson_doc_free(doc);
    }

    static void parse_usage_(yyjson_val *usage, UsageDelta &out) {
        if (!yyjson_is_obj(usage)) {
            return;
        }
        out.has = true;
        yyjson_val *v = yyjson_obj_get(usage, "input_tokens");
        if (yyjson_is_int(v)) {
            out.input_tokens = yyjson_get_int(v);
        }
        v = yyjson_obj_get(usage, "output_tokens");
        if (yyjson_is_int(v)) {
            out.output_tokens = yyjson_get_int(v);
        }
        v = yyjson_obj_get(usage, "total_tokens");
        if (yyjson_is_int(v)) {
            out.total_tokens = yyjson_get_int(v);
        }
        yyjson_val *details = yyjson_obj_get(usage, "input_tokens_details");
        if (yyjson_is_obj(details)) {
            v = yyjson_obj_get(details, "cached_tokens");
            if (yyjson_is_int(v)) {
                out.cached_tokens = yyjson_get_int(v);
            }
        }
    }
};

} // namespace kimix::llm::openai_responses
