// openai_chat.h - OpenAI-compatible chat completion streaming workflow.
// Mirrors the streaming flow of kosong's openai_legacy provider: build the
// request body (including DeepSeek-style thinking keys), POST it with
// cpp-httplib, parse the SSE stream, and accumulate reasoning / content /
// tool calls / usage.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "openai/sse_parser.h"

namespace openai {

// LLM backend config loaded from a JSON file (e.g. C:/dev/ds_flash.json).
struct LlmConfig {
    std::string model;
    std::string url; // e.g. http://host:port/llmproxy
    std::string api_key;
    std::string type;
    std::string thinking_effort = "high";
    bool show_thinking_stream = true;
    int max_context_size = 0;
};

// Load and validate an LLM config from a JSON file.
bool load_config(const std::string &path, LlmConfig &cfg);

// A function tool call made by the model.
struct ToolCall {
    std::string id;
    std::string type = "function";
    std::string name;
    std::string arguments; // JSON string
};

// One chat message sent to the API.
struct ChatMessage {
    std::string role; // system | user | assistant | tool
    std::string content;
    std::string tool_call_id;
    std::vector<ToolCall> tool_calls;
};

// A function tool definition offered to the model.
struct Tool {
    std::string name;
    std::string description;
    std::string parameters_json; // JSON object string, e.g. {"type":"object",...}
};

// Accumulated result of one streamed chat completion.
struct ChatResult {
    bool ok = false;
    std::string error;
    std::string content;
    std::string reasoning;
    std::vector<ToolCall> tool_calls;
    int64_t prompt_tokens = 0;
    int64_t completion_tokens = 0;
    int64_t total_tokens = 0;
};

// Called for every SSE event while streaming.
using ChunkCallback = std::function<void(const ChatChunk &)>;

// Stream one chat completion request. Each parsed SSE event is delivered to
// on_chunk (may be null); accumulated content/reasoning/tool_calls/usage are
// returned in the ChatResult.
ChatResult chat_completion_stream(const LlmConfig &cfg,
                                  const std::vector<ChatMessage> &messages,
                                  const std::vector<Tool> &tools,
                                  const ChunkCallback &on_chunk);

// Build the JSON request body (exposed for tests and debugging).
std::string build_chat_body(const LlmConfig &cfg,
                            const std::vector<ChatMessage> &messages,
                            const std::vector<Tool> &tools);

} // namespace openai
