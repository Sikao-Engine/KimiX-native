// responses_chat.h - OpenAI Responses API streaming workflow.
// Mirrors kosong's openai_responses provider: build the request (model, input
// items, tools, reasoning config), POST it with cpp-httplib + Mbed TLS
// (kimix-mbedtls), parse the SSE stream, and accumulate text / reasoning /
// function calls / usage.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "openai_responses/stream_parser.h"

namespace openai_responses {

// LLM backend config loaded from a JSON file (e.g. C:/dev/backup_ds_flash.json).
struct ResponsesConfig {
    std::string model;
    std::string url; // provider base; a trailing "/anthropic" mount is stripped
    std::string api_key;
    std::string type;
    std::string thinking_effort = "high";
    int max_tokens = 4096;
    int max_context_size = 0;
};

// Load and validate an OpenAI-compatible config from a JSON file.
bool load_config(const std::string &path, ResponsesConfig &cfg);

// A function tool offered to the model (Responses API tool schema).
struct Tool {
    std::string name;
    std::string description;
    std::string parameters_json; // JSON object string, e.g. {"type":"object",...}
};

// A function_call item produced by the model.
struct FunctionCall {
    std::string call_id;
    std::string name;
    std::string arguments; // JSON string
};

// One item of the Responses API `input` array.
struct InputItem {
    std::string type;      // message | reasoning | function_call | function_call_output
    std::string role;      // system | user | assistant (message only)
    std::string content;   // message text / reasoning text / function_call_output output
    std::string item_id;   // reasoning item id (must match the streamed id)
    std::string call_id;   // function_call / function_call_output
    std::string name;      // function_call name
    std::string arguments; // function_call arguments JSON string
};

// Accumulated result of one streamed Responses request.
struct ChatResult {
    bool ok = false;
    std::string error;
    std::string text;
    std::string reasoning;
    std::string reasoning_item_id;
    std::vector<FunctionCall> tool_calls;
    int64_t input_tokens = 0;
    int64_t output_tokens = 0;
    int64_t cached_tokens = 0;
    int64_t total_tokens = 0;
};

// Called for every SSE event while streaming.
using EventCallback = std::function<void(const StreamEvent &)>;

// Stream one OpenAI Responses request. Each parsed SSE event is delivered to
// on_event (may be null); accumulated text/reasoning/tool_calls/usage are
// returned in the ChatResult.
ChatResult responses_completion_stream(const ResponsesConfig &cfg,
                                       const std::vector<InputItem> &input,
                                       const std::vector<Tool> &tools,
                                       const EventCallback &on_event);

// Build the JSON request body (exposed for tests and debugging).
std::string build_responses_body(const ResponsesConfig &cfg,
                                 const std::vector<InputItem> &input,
                                 const std::vector<Tool> &tools);

} // namespace openai_responses
