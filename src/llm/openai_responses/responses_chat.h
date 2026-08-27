// responses_chat.h - OpenAI Responses API streaming workflow.
// Mirrors kosong's openai_responses provider: build the request (model, input
// items, tools, reasoning config), POST it with cpp-httplib + Mbed TLS
// (kimix-mbedtls), parse the SSE stream, and accumulate text / reasoning /
// function calls / usage.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "llm/common.h"
#include "llm/openai_responses/stream_parser.h"

namespace kimix::llm::openai_responses {

// A function tool offered to the model (Responses API tool schema).
struct Tool {
    kimix::string name;
    kimix::string description;
    kimix::string parameters_json; // JSON object string, e.g. {"type":"object",...}
};

// A function_call item produced by the model.
struct FunctionCall {
    kimix::string call_id;
    kimix::string name;
    kimix::string arguments; // JSON string
};

// One item of the Responses API `input` array.
struct InputItem {
    kimix::string type;      // message | reasoning | function_call | function_call_output
    kimix::string role;      // system | user | assistant (message only)
    kimix::string content;   // message text / reasoning text / function_call_output output
    kimix::string item_id;   // reasoning item id (must match the streamed id)
    kimix::string call_id;   // function_call / function_call_output
    kimix::string name;      // function_call name
    kimix::string arguments; // function_call arguments JSON string
};

// Accumulated result of one streamed Responses request.
struct ChatResult {
    bool ok = false;
    kimix::string error;
    kimix::string text;
    kimix::string reasoning;
    kimix::string reasoning_item_id;
    kimix::vector<FunctionCall> tool_calls;
    int64_t input_tokens = 0;
    int64_t output_tokens = 0;
    int64_t cached_tokens = 0;
    int64_t total_tokens = 0;
};

// Called for every SSE event while streaming.
using EventCallback = kimix::function<void(const StreamEvent &)>;

// Stream one OpenAI Responses request. Each parsed SSE event is delivered to
// on_event (may be null); accumulated text/reasoning/tool_calls/usage are
// returned in the ChatResult.
ChatResult responses_completion_stream(const Config &cfg,
                                       const kimix::vector<InputItem> &input,
                                       const kimix::vector<Tool> &tools,
                                       const EventCallback &on_event);

// Build the JSON request body (exposed for tests and debugging).
kimix::string build_responses_body(const Config &cfg,
                                   const kimix::vector<InputItem> &input,
                                   const kimix::vector<Tool> &tools);

} // namespace kimix::llm::openai_responses
