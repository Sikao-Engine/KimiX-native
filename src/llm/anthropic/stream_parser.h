// stream_parser.h - Minimal Server-Sent Events (SSE) parser for Anthropic
// Messages API streams. Header-only; uses yyjson for JSON parsing.
//
// Anthropic streams each event as an optional `event:` line plus a `data:`
// JSON line, terminated by a blank line. The JSON `type` field is
// authoritative (message_start / content_block_start / content_block_delta /
// content_block_stop / message_delta / message_stop / ping). Fields handled
// here cover text, thinking (with signature), tool_use (with input_json
// deltas) and usage deltas, mirroring anthropic.py's streaming loop.

#pragma once

#include <cstddef>
#include <cstdint>

#include <core/kimix_core.h>

#include "yyjson.h"

#include "llm/yyjson_alc.h"

namespace kimix::llm::anthropic {

// Usage counters carried by message_start / message_delta events.
struct UsageDelta {
    bool has = false;
    int64_t input_tokens = 0;
    int64_t output_tokens = 0;
    int64_t cache_creation_input_tokens = 0;
    int64_t cache_read_input_tokens = 0;
};

// One parsed SSE `data:` event from a /v1/messages stream.
struct StreamEvent {
    kimix::string type; // message_start | content_block_start | ... | ping
    // message_start
    kimix::string message_id;
    UsageDelta usage;
    // content_block_start / content_block_delta / content_block_stop
    int index = 0;
    kimix::string block_type; // text | thinking | tool_use | redacted_thinking
    kimix::string block_id;   // tool_use id
    kimix::string block_name; // tool_use name
    kimix::string delta_type; // text_delta | thinking_delta | input_json_delta | signature_delta
    kimix::string text;       // initial text/thinking or delta payload
    kimix::string signature;  // signature_delta payload
    // message_delta
    kimix::string stop_reason;
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
        yyjson_doc *doc = yyjson_read_opts((char *)data.data(), data.size(), 0, &kYYJsonAlcMi, nullptr);
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

        if (ev.type == "message_start") {
            yyjson_val *message = yyjson_obj_get(root, "message");
            if (yyjson_is_obj(message)) {
                v = yyjson_obj_get(message, "id");
                if (yyjson_is_str(v)) {
                    ev.message_id.assign(yyjson_get_str(v), yyjson_get_len(v));
                }
                parse_usage_(yyjson_obj_get(message, "usage"), ev.usage);
            }
        } else if (ev.type == "content_block_start") {
            yyjson_val *idx = yyjson_obj_get(root, "index");
            if (yyjson_is_int(idx)) {
                ev.index = (int)yyjson_get_int(idx);
            }
            yyjson_val *block = yyjson_obj_get(root, "content_block");
            if (yyjson_is_obj(block)) {
                v = yyjson_obj_get(block, "type");
                if (yyjson_is_str(v)) {
                    ev.block_type.assign(yyjson_get_str(v), yyjson_get_len(v));
                }
                v = yyjson_obj_get(block, "id");
                if (yyjson_is_str(v)) {
                    ev.block_id.assign(yyjson_get_str(v), yyjson_get_len(v));
                }
                v = yyjson_obj_get(block, "name");
                if (yyjson_is_str(v)) {
                    ev.block_name.assign(yyjson_get_str(v), yyjson_get_len(v));
                }
                v = yyjson_obj_get(block, "thinking");
                if (yyjson_is_str(v)) {
                    ev.text.assign(yyjson_get_str(v), yyjson_get_len(v));
                }
                v = yyjson_obj_get(block, "text");
                if (yyjson_is_str(v)) {
                    ev.text.assign(yyjson_get_str(v), yyjson_get_len(v));
                }
            }
        } else if (ev.type == "content_block_delta") {
            yyjson_val *idx = yyjson_obj_get(root, "index");
            if (yyjson_is_int(idx)) {
                ev.index = (int)yyjson_get_int(idx);
            }
            yyjson_val *delta = yyjson_obj_get(root, "delta");
            if (yyjson_is_obj(delta)) {
                v = yyjson_obj_get(delta, "type");
                if (yyjson_is_str(v)) {
                    ev.delta_type.assign(yyjson_get_str(v), yyjson_get_len(v));
                }
                v = yyjson_obj_get(delta, "text");
                if (yyjson_is_str(v)) {
                    ev.text.assign(yyjson_get_str(v), yyjson_get_len(v));
                }
                v = yyjson_obj_get(delta, "thinking");
                if (yyjson_is_str(v)) {
                    ev.text.assign(yyjson_get_str(v), yyjson_get_len(v));
                }
                v = yyjson_obj_get(delta, "partial_json");
                if (yyjson_is_str(v)) {
                    ev.text.assign(yyjson_get_str(v), yyjson_get_len(v));
                }
                v = yyjson_obj_get(delta, "signature");
                if (yyjson_is_str(v)) {
                    ev.signature.assign(yyjson_get_str(v), yyjson_get_len(v));
                }
            }
        } else if (ev.type == "content_block_stop") {
            yyjson_val *idx = yyjson_obj_get(root, "index");
            if (yyjson_is_int(idx)) {
                ev.index = (int)yyjson_get_int(idx);
            }
        } else if (ev.type == "message_delta") {
            yyjson_val *delta = yyjson_obj_get(root, "delta");
            if (yyjson_is_obj(delta)) {
                v = yyjson_obj_get(delta, "stop_reason");
                if (yyjson_is_str(v)) {
                    ev.stop_reason.assign(yyjson_get_str(v), yyjson_get_len(v));
                }
            }
            parse_usage_(yyjson_obj_get(root, "usage"), ev.usage);
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
        v = yyjson_obj_get(usage, "cache_creation_input_tokens");
        if (yyjson_is_int(v)) {
            out.cache_creation_input_tokens = yyjson_get_int(v);
        }
        v = yyjson_obj_get(usage, "cache_read_input_tokens");
        if (yyjson_is_int(v)) {
            out.cache_read_input_tokens = yyjson_get_int(v);
        }
    }
};

} // namespace kimix::llm::anthropic
