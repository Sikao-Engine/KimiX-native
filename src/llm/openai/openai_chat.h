// openai_chat.h - OpenAI-compatible chat completion streaming workflow.
// Mirrors the streaming flow of kosong's openai_legacy provider: build the
// request body (including DeepSeek-style thinking keys), POST it with
// cpp-httplib, parse the SSE stream, and accumulate reasoning / content /
// tool calls / usage.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "llm/common.h"
#include "llm/openai/sse_parser.h"

namespace kimix::llm::openai {

// A function tool call made by the model.
struct ToolCall {
    kimix::string id;
    kimix::string type = "function";
    kimix::string name;
    kimix::string arguments; // JSON string
};

// One chat message sent to the API.
struct ChatMessage {
    kimix::string role; // system | user | assistant | tool
    kimix::string content;
    kimix::string tool_call_id;
    kimix::vector<ToolCall> tool_calls;
};

// A function tool definition offered to the model.
struct Tool {
    kimix::string name;
    kimix::string description;
    kimix::string parameters_json; // JSON object string, e.g. {"type":"object",...}
};

// Accumulated result of one streamed chat completion.
struct ChatResult {
    bool ok = false;
    kimix::string error;
    kimix::string content;
    kimix::string reasoning;
    kimix::vector<ToolCall> tool_calls;
    int64_t prompt_tokens = 0;
    int64_t completion_tokens = 0;
    int64_t total_tokens = 0;
};

// Called for every SSE event while streaming.
using ChunkCallback = kimix::function<void(const ChatChunk &)>;

// Stream one chat completion request. Each parsed SSE event is delivered to
// on_chunk (may be null); accumulated content/reasoning/tool_calls/usage are
// returned in the ChatResult.
ChatResult chat_completion_stream(const Config &cfg,
                                  const kimix::vector<ChatMessage> &messages,
                                  const kimix::vector<Tool> &tools,
                                  const ChunkCallback &on_chunk);

// Build the JSON request body (exposed for tests and debugging).
kimix::string build_chat_body(const Config &cfg,
                              const kimix::vector<ChatMessage> &messages,
                              const kimix::vector<Tool> &tools);

} // namespace kimix::llm::openai
