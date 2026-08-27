// llm.h - Unified LLM interface over the OpenAI Chat Completions, OpenAI
// Responses, and Anthropic Messages providers. Mirrors kimi-cli's llm.py:
// the LLM wraps a chat provider and config.type decides which provider
// interface is used ("openai"|"openai_legacy" -> OpenAI Chat Completions,
// "openai_responses" -> OpenAI Responses, "anthropic" -> Anthropic).

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "llm/common.h"

namespace kimix::llm {

// A function tool call made by the model (unified across providers).
struct ToolCall {
    kimix::string id;
    kimix::string type = "function";
    kimix::string name;
    kimix::string arguments; // JSON string
};

// A function tool definition offered to the model (unified schema).
struct Tool {
    kimix::string name;
    kimix::string description;
    kimix::string parameters_json; // JSON schema object string
};

// One chat message in unified terms. Round-trip fields from the Anthropic
// provider (thinking + signature) are kept on assistant messages.
struct Message {
    kimix::string role;                  // system | user | assistant | tool
    kimix::string content;
    kimix::string tool_call_id;          // tool-role results
    kimix::vector<ToolCall> tool_calls;  // assistant tool calls
    kimix::string thinking;              // anthropic thinking round-trip (assistant)
    kimix::string thinking_signature;    // anthropic signature round-trip (assistant)
};

// Unified streamed delta delivered to on_chunk.
struct Chunk {
    bool ok = false;
    bool done = false;
    kimix::string role;
    kimix::string content;               // text delta
    kimix::string reasoning;             // reasoning/thinking delta
    kimix::vector<ToolCall> tool_calls;  // tool-call deltas
    kimix::string finish_reason;
    bool has_usage = false;
    int64_t prompt_tokens = 0;
    int64_t completion_tokens = 0;
    int64_t total_tokens = 0;
};

using ChunkCallback = kimix::function<void(const Chunk &)>;

// Unified accumulated result of one streamed request.
struct ChatResult {
    bool ok = false;
    kimix::string error;
    kimix::string content;
    kimix::string reasoning;
    kimix::vector<ToolCall> tool_calls;
    kimix::string finish_reason; // openai finish_reason / anthropic stop_reason
    kimix::string signature;     // anthropic thinking signature (round-trip)
    int64_t prompt_tokens = 0;   // input tokens
    int64_t completion_tokens = 0; // output tokens
    int64_t cached_tokens = 0;
    int64_t total_tokens = 0;
};

// Abstract chat provider interface (analogue of kosong's ChatProvider).
class ChatProvider {
public:
    virtual ~ChatProvider() = default;
    virtual kimix::string model_name() const = 0;
    virtual ChatResult chat(const kimix::vector<Message> &messages,
                            const kimix::vector<Tool> &tools,
                            const ChunkCallback &on_chunk) const = 0;
};

// Unified LLM wrapper (analogue of the Python LLM dataclass).
class LLM {
public:
    LLM(kimix::unique_ptr<ChatProvider> provider, Config config);
    kimix::string model_name() const;            // delegates to provider
    const Config &config() const;
    int32_t max_context_size() const;
    ChatResult chat(const kimix::vector<Message> &messages,
                    const kimix::vector<Tool> &tools = {},
                    const ChunkCallback &on_chunk = {}) const;

private:
    kimix::unique_ptr<ChatProvider> provider_;
    Config config_;
};

// config.type selects the provider: "openai"|"openai_legacy" -> OpenAI Chat
// Completions; "openai_responses" -> OpenAI Responses; "anthropic" -> Anthropic.
// Returns null on unknown type / missing model or url (mirrors Python's None).
kimix::unique_ptr<LLM> create_llm(Config config);
kimix::unique_ptr<LLM> create_llm_from_file(const kimix::string &path);

} // namespace kimix::llm
