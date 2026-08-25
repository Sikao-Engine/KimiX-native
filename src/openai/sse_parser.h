// sse_parser.h - Minimal Server-Sent Events (SSE) parser for OpenAI-compatible
// chat completion streams. Header-only; uses yyjson for JSON parsing.
//
// The parser turns raw HTTP body bytes into ChatChunk events. Each OpenAI SSE
// event is a `data:` JSON line terminated by a blank line; the terminal event is
// `data: [DONE]`. Fields handled here: role/content deltas, reasoning_content
// (DeepSeek/Kimi style), tool_calls deltas (accumulated by the caller),
// finish_reason, and the final usage chunk.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "yyjson.h"

namespace openai {

// One tool-call delta inside a streaming chunk. The arguments field may be
// split across multiple chunks; the workflow accumulates fragments by index.
struct ToolCallDelta {
    int index = 0;
    std::string id;
    std::string type;
    std::string name;
    std::string arguments;
};

// One parsed SSE `data:` event from a /chat/completions stream.
struct ChatChunk {
    bool ok = false;               // false when the data line was not JSON
    bool done = false;             // true for the terminal [DONE] event
    bool has_content = false;      // true when delta.content was a string
    std::string role;              // usually "assistant" on the first chunk
    std::string content;           // content delta for this chunk
    std::string reasoning_content; // reasoning delta (thinking) for this chunk
    std::vector<ToolCallDelta> tool_calls; // tool-call deltas for this chunk
    std::string finish_reason;     // "stop", "tool_calls", "length", ...
    bool has_usage = false;
    int64_t prompt_tokens = 0;
    int64_t completion_tokens = 0;
    int64_t total_tokens = 0;
};

// Streaming SSE parser. Feed raw bytes from the HTTP response body; complete
// events are returned immediately, partial events are buffered until finished.
class SseParser {
public:
    std::vector<ChatChunk> feed(const char *data, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            if (data[i] != '\r') {
                buffer_.push_back(data[i]);
            }
        }
        return drain_();
    }

    // Flush any remaining buffered text (normally a no-op for well-formed
    // streams that terminate with a blank line).
    std::vector<ChatChunk> finish() {
        std::vector<ChatChunk> out = drain_();
        if (!event_lines_.empty()) {
            out.push_back(parse_event_lines_(event_lines_));
            event_lines_.clear();
        }
        return out;
    }

private:
    std::string buffer_;
    std::vector<std::string> event_lines_;

    std::vector<ChatChunk> drain_() {
        std::vector<ChatChunk> out;
        for (;;) {
            size_t pos = buffer_.find('\n');
            if (pos == std::string::npos) {
                break;
            }
            std::string line = buffer_.substr(0, pos);
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

    static ChatChunk parse_event_lines_(const std::vector<std::string> &lines) {
        ChatChunk chunk;
        std::string data;
        for (const auto &line : lines) {
            if (line.rfind("data:", 0) == 0) {
                std::string payload = line.substr(5);
                if (!payload.empty() && payload.front() == ' ') {
                    payload.erase(0, 1);
                }
                if (!data.empty()) {
                    data += '\n';
                }
                data += payload;
            }
            // Other SSE fields (event:, id:, retry:) are ignored.
        }
        if (data == "[DONE]") {
            chunk.ok = true;
            chunk.done = true;
            return chunk;
        }
        parse_chunk_json_(data, chunk);
        return chunk;
    }

    static void parse_chunk_json_(const std::string &data, ChatChunk &chunk) {
        yyjson_doc *doc = yyjson_read(data.data(), data.size(), 0);
        if (!doc) {
            return; // chunk.ok stays false
        }
        yyjson_val *root = yyjson_doc_get_root(doc);
        if (!yyjson_is_obj(root)) {
            yyjson_doc_free(doc);
            return;
        }
        yyjson_val *choices = yyjson_obj_get(root, "choices");
        if (yyjson_is_arr(choices)) {
            yyjson_val *choice = yyjson_arr_get_first(choices);
            if (yyjson_is_obj(choice)) {
                yyjson_val *delta = yyjson_obj_get(choice, "delta");
                if (yyjson_is_obj(delta)) {
                    yyjson_val *v = yyjson_obj_get(delta, "role");
                    if (yyjson_is_str(v)) {
                        chunk.role.assign(yyjson_get_str(v), yyjson_get_len(v));
                    }
                    v = yyjson_obj_get(delta, "content");
                    if (yyjson_is_str(v)) {
                        chunk.content.assign(yyjson_get_str(v), yyjson_get_len(v));
                        chunk.has_content = true;
                    }
                    v = yyjson_obj_get(delta, "reasoning_content");
                    if (!yyjson_is_str(v)) {
                        v = yyjson_obj_get(delta, "reasoning");
                    }
                    if (yyjson_is_str(v)) {
                        chunk.reasoning_content.assign(yyjson_get_str(v), yyjson_get_len(v));
                    }
                    yyjson_val *tool_calls = yyjson_obj_get(delta, "tool_calls");
                    if (yyjson_is_arr(tool_calls)) {
                        yyjson_val *tc = nullptr;
                        size_t idx = 0;
                        size_t max = 0;
                        yyjson_arr_foreach(tool_calls, idx, max, tc) {
                            if (!yyjson_is_obj(tc)) {
                                continue;
                            }
                            ToolCallDelta tcd;
                            v = yyjson_obj_get(tc, "index");
                            if (yyjson_is_int(v)) {
                                tcd.index = (int)yyjson_get_int(v);
                            }
                            v = yyjson_obj_get(tc, "id");
                            if (yyjson_is_str(v)) {
                                tcd.id.assign(yyjson_get_str(v), yyjson_get_len(v));
                            }
                            v = yyjson_obj_get(tc, "type");
                            if (yyjson_is_str(v)) {
                                tcd.type.assign(yyjson_get_str(v), yyjson_get_len(v));
                            }
                            yyjson_val *fn = yyjson_obj_get(tc, "function");
                            if (yyjson_is_obj(fn)) {
                                v = yyjson_obj_get(fn, "name");
                                if (yyjson_is_str(v)) {
                                    tcd.name.assign(yyjson_get_str(v), yyjson_get_len(v));
                                }
                                v = yyjson_obj_get(fn, "arguments");
                                if (yyjson_is_str(v)) {
                                    tcd.arguments.assign(yyjson_get_str(v), yyjson_get_len(v));
                                }
                            }
                            chunk.tool_calls.push_back(std::move(tcd));
                        }
                    }
                }
                yyjson_val *finish = yyjson_obj_get(choice, "finish_reason");
                if (yyjson_is_str(finish)) {
                    chunk.finish_reason.assign(yyjson_get_str(finish), yyjson_get_len(finish));
                }
            }
        }
        yyjson_val *usage = yyjson_obj_get(root, "usage");
        if (yyjson_is_obj(usage)) {
            chunk.has_usage = true;
            yyjson_val *v = yyjson_obj_get(usage, "prompt_tokens");
            if (yyjson_is_int(v)) {
                chunk.prompt_tokens = yyjson_get_int(v);
            }
            v = yyjson_obj_get(usage, "completion_tokens");
            if (yyjson_is_int(v)) {
                chunk.completion_tokens = yyjson_get_int(v);
            }
            v = yyjson_obj_get(usage, "total_tokens");
            if (yyjson_is_int(v)) {
                chunk.total_tokens = yyjson_get_int(v);
            }
        }
        chunk.ok = true;
        yyjson_doc_free(doc);
    }
};

} // namespace openai
