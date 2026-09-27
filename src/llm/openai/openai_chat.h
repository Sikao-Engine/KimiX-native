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

namespace kimix::llm {
class AbortCheck; // llm/llm.h - streaming abort hook
}

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
    // E1/E2 (kept LAST so positional aggregate initializers stay valid): when
    // non-empty the wire `content` is a block ARRAY built from these parts
    // (mirroring kosong Message._serialize_content's part list); `content`
    // above stays the text backbone and is ignored for the wire in that
    // case. Think parts never serialize here (the reasoning fields are the
    // thinking channel).
    kimix::vector<kimix::llm::ContentPart> parts;
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
    // Moonshot/Kimi-style cache-read input tokens (-1 == the usage object
    // carried no cached_tokens at all). Only the Kimi provider's stream
    // function (llm/kimi/kimi_chat.cpp) fills it; openai_chat leaves it at
    // the default and its own flow ignores it.
    int64_t cached_tokens = -1;
    // Structured error classification (set only when ok == false); the LLM
    // adapter maps it onto the unified kimix::llm::ChatErrorKind.
    TransportErrorKind error_kind = TransportErrorKind::none;
    int32_t error_status = 0;       // HTTP status for kind == http, else 0
    double retry_after_seconds = 0; // Retry-After hint (429), 0 == absent
};

// Called for every SSE event while streaming.
using ChunkCallback = kimix::function<void(const ChatChunk &)>;

// Stream one chat completion request. Each parsed SSE event is delivered to
// on_chunk (may be null); accumulated content/reasoning/tool_calls/usage are
// returned in the ChatResult.
  ChatResult chat_completion_stream(const Config &cfg,
                                    const kimix::vector<ChatMessage> &messages,
                                    const kimix::vector<Tool> &tools,
                                    const ChunkCallback &on_chunk,
                                    const AbortCheck *abort = nullptr);

// Build the JSON request body (exposed for tests and debugging). When
// `out_error` is given it receives the reason for a failed build (e.g. yyjson's
// "invalid utf-8 encoding in string"), so the caller never reports only
// "failed to build request body".
kimix::string build_chat_body(const Config &cfg,
                              const kimix::vector<ChatMessage> &messages,
                              const kimix::vector<Tool> &tools,
                              kimix::string *out_error = nullptr);

} // namespace kimix::llm::openai
