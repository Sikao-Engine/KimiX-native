// anthropic_chat.h - Anthropic Messages API streaming workflow.
// Mirrors the streaming flow of kosong's anthropic provider: build the request
// body (system prompt, messages as content blocks, tools, thinking config),
// POST it with cpp-httplib + Mbed TLS (kimix-mbedtls), parse the SSE stream,
// and accumulate text / thinking / tool_use / usage.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "llm/anthropic/stream_parser.h"
#include "llm/common.h"

namespace kimix::llm::anthropic {

// A function tool offered to the model (Anthropic input_schema).
struct Tool {
    kimix::string name;
    kimix::string description;
    kimix::string input_schema_json; // JSON object string, e.g. {"type":"object",...}
};

// A tool_use block produced by the model.
struct ToolUse {
    kimix::string id;
    kimix::string name;
    kimix::string input_json; // accumulated JSON string
};

// One chat message in Anthropic wire terms. For the demo this covers:
//   - user text (or a user tool_result block)
//   - assistant text + optional thinking block + optional tool_use blocks
struct ChatMessage {
    kimix::string role; // user | assistant
    kimix::string text;
    // Assistant thinking block; DeepSeek's /anthropic endpoint requires the
    // streamed thinking + signature to be passed back on the next assistant
    // message (mirrors anthropic.py's ThinkPart round-trip).
    kimix::string thinking;
    kimix::string thinking_signature;
    kimix::vector<ToolUse> tool_uses;
    // User tool_result block (tool_use_id + content).
    kimix::string tool_result_id;
    kimix::string tool_result_content;
};

// Accumulated result of one streamed Anthropic message.
struct ChatResult {
    bool ok = false;
    kimix::string error;
    kimix::string text;
    kimix::string thinking;
    kimix::string signature;
    kimix::vector<ToolUse> tool_uses;
    kimix::string stop_reason;
    int64_t input_tokens = 0;
    int64_t output_tokens = 0;
    int64_t cache_creation_input_tokens = 0;
    int64_t cache_read_input_tokens = 0;
};

// Called for every SSE event while streaming.
using EventCallback = kimix::function<void(const StreamEvent &)>;

// Stream one Anthropic Messages request. Each parsed SSE event is delivered to
// on_event (may be null); accumulated text/thinking/tool_uses/usage are
// returned in the ChatResult.
ChatResult chat_completion_stream(const Config &cfg,
                                  const kimix::string &system,
                                  const kimix::vector<ChatMessage> &messages,
                                  const kimix::vector<Tool> &tools,
                                  const EventCallback &on_event);

// Build the JSON request body (exposed for tests and debugging).
kimix::string build_messages_body(const Config &cfg,
                                  const kimix::string &system,
                                  const kimix::vector<ChatMessage> &messages,
                                  const kimix::vector<Tool> &tools);

} // namespace kimix::llm::anthropic
