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

namespace kimix::llm {
class AbortCheck; // llm/llm.h - streaming abort hook
}

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

// One tool_result content block of a user message. The Anthropic spec
// requires the tool results of one assistant turn to live in a SINGLE user
// message, so the unified tool-role messages of that turn are merged into one
// wire message carrying several blocks (see AnthropicWireRequest in llm.h).
struct ToolResult {
    kimix::string tool_use_id;
    kimix::string content;
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
    // User tool_result block (tool_use_id + content). Kept for the common
    // single-result case; when `tool_results` is non-empty it carries the
    // merged blocks instead and these two fields are ignored.
    kimix::string tool_result_id;
    kimix::string tool_result_content;
    // Merged tool_result blocks (E4): consecutive tool-result-only user
    // messages become ONE user message with one block per tool result.
    kimix::vector<ToolResult> tool_results;
    // E1/E2 (kept LAST so positional aggregate initializers stay valid): when
    // non-empty the message content is a block list built from these parts
    // (TextBlockParam / ImageBlockParam via _image_url_part_to_anthropic;
    // audio/video are skipped - the reference's user/assistant loop
    // `continue`s on them). `text` above is ignored for the wire in that
    // case (the parts carry the text blocks). Think parts never serialize
    // here (the thinking round-trip fields own that block).
    kimix::vector<kimix::llm::ContentPart> parts;
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
      // Structured error classification (set only when ok == false); the LLM
      // adapter maps it onto the unified kimix::llm::ChatErrorKind.
      TransportErrorKind error_kind = TransportErrorKind::none;
      int32_t error_status = 0;       // HTTP status for kind == http, else 0
      double retry_after_seconds = 0; // Retry-After hint (429), 0 == absent
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
                                    const EventCallback &on_event,
                                    const AbortCheck *abort = nullptr);

// Build the JSON request body (exposed for tests and debugging). When
// `out_error` is given it receives the reason for a failed build (e.g. yyjson's
// "invalid utf-8 encoding in string"), so the caller never reports only
// "failed to build request body".
kimix::string build_messages_body(const Config &cfg,
                                  const kimix::string &system,
                                  const kimix::vector<ChatMessage> &messages,
                                  const kimix::vector<Tool> &tools,
                                  kimix::string *out_error = nullptr);

} // namespace kimix::llm::anthropic
