// anthropic_chat.h - Anthropic Messages API streaming workflow.
// Mirrors the streaming flow of kosong's anthropic provider: build the request
// body (system prompt, messages as content blocks, tools, thinking config),
// POST it with cpp-httplib + Mbed TLS (kimix-mbedtls), parse the SSE stream,
// and accumulate text / thinking / tool_use / usage.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "anthropic/stream_parser.h"

namespace anthropic {

// LLM backend config loaded from a JSON file (e.g. C:/dev/backup_ds_flash.json).
struct AnthropicConfig {
    std::string model;
    std::string url; // e.g. https://api.deepseek.com/anthropic
    std::string api_key;
    std::string type;
    std::string thinking_effort = "high";
    int max_tokens = 4096;
    int max_context_size = 0;
};

// Load and validate an Anthropic-compatible config from a JSON file.
bool load_config(const std::string &path, AnthropicConfig &cfg);

// A function tool offered to the model (Anthropic input_schema).
struct Tool {
    std::string name;
    std::string description;
    std::string input_schema_json; // JSON object string, e.g. {"type":"object",...}
};

// A tool_use block produced by the model.
struct ToolUse {
    std::string id;
    std::string name;
    std::string input_json; // accumulated JSON string
};

// One chat message in Anthropic wire terms. For the demo this covers:
//   - user text (or a user tool_result block)
//   - assistant text + optional thinking block + optional tool_use blocks
struct ChatMessage {
    std::string role; // user | assistant
    std::string text;
    // Assistant thinking block; DeepSeek's /anthropic endpoint requires the
    // streamed thinking + signature to be passed back on the next assistant
    // message (mirrors anthropic.py's ThinkPart round-trip).
    std::string thinking;
    std::string thinking_signature;
    std::vector<ToolUse> tool_uses;
    // User tool_result block (tool_use_id + content).
    std::string tool_result_id;
    std::string tool_result_content;
};

// Accumulated result of one streamed Anthropic message.
struct ChatResult {
    bool ok = false;
    std::string error;
    std::string text;
    std::string thinking;
    std::string signature;
    std::vector<ToolUse> tool_uses;
    std::string stop_reason;
    int64_t input_tokens = 0;
    int64_t output_tokens = 0;
    int64_t cache_creation_input_tokens = 0;
    int64_t cache_read_input_tokens = 0;
};

// Called for every SSE event while streaming.
using EventCallback = std::function<void(const StreamEvent &)>;

// Stream one Anthropic Messages request. Each parsed SSE event is delivered to
// on_event (may be null); accumulated text/thinking/tool_uses/usage are
// returned in the ChatResult.
ChatResult chat_completion_stream(const AnthropicConfig &cfg,
                                  const std::string &system,
                                  const std::vector<ChatMessage> &messages,
                                  const std::vector<Tool> &tools,
                                  const EventCallback &on_event);

// Build the JSON request body (exposed for tests and debugging).
std::string build_messages_body(const AnthropicConfig &cfg,
                                const std::string &system,
                                const std::vector<ChatMessage> &messages,
                                const std::vector<Tool> &tools);

} // namespace anthropic
