// llm.h - Unified LLM interface over the OpenAI Chat Completions, OpenAI
// Responses, and Anthropic Messages providers. Mirrors kimi-cli's llm.py:
// the LLM wraps a chat provider and config.type decides which provider
// interface is used ("openai"|"openai_legacy" -> OpenAI Chat Completions,
// "openai_responses" -> OpenAI Responses, "anthropic" -> Anthropic).

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "llm/common.h"
#include "llm/anthropic/anthropic_chat.h"
#include "llm/openai/openai_chat.h"
#include "llm/openai_responses/responses_chat.h"

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
//
// E1/E2 invariant: `content` is the concatenated TEXT backbone of the
// message; `parts` is empty for plain-text messages and is populated only
// when non-text parts exist (then it carries ALL parts - text parts included,
// in order). message_parts()/message_set_parts() keep the two in sync.
struct Message {
    kimix::string role;                  // system | user | assistant | tool
    kimix::string content;
    kimix::string tool_call_id;          // tool-role results
    kimix::vector<ToolCall> tool_calls;  // assistant tool calls
    kimix::string thinking;              // anthropic thinking round-trip (assistant)
    kimix::string thinking_signature;    // anthropic signature round-trip (assistant)
    // Media adjunct (E1/E2): empty unless the message carries non-text parts.
    kimix::vector<ContentPart> parts;
};

// The content parts of a message (kosong Message.content). A plain-text
// message (no parts) reads back as the single TextPart carrying `content`.
kimix::vector<ContentPart> message_parts(const Message &msg);
// Replace the parts of a message, keeping the text backbone in sync: content
// becomes the concatenation of the text-kind parts (think parts feed the
// thinking round-trip field; media parts carry no text). Think parts without
// a signature leave thinking_signature unchanged.
void message_set_parts(Message &msg, kimix::vector<ContentPart> parts);

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

// G8 cancellation seam (run_soul's cancel_event at the transport level):
// the caller-owned abort check polled by the streaming providers inside
// their httplib ContentReceiver - when aborted() flips true the receiver
// returns false, cpp-httplib cancels the request (Error::Canceled) and the
// in-flight request returns promptly instead of draining the SSE stream.
// The agent layer passes a composite that ORs the turn's CancelToken with
// the steer wake event, so both cancellation and mid-stream steering stop
// the HTTP read. nullptr == never abort.
class AbortCheck {
public:
    virtual ~AbortCheck() = default;
    virtual bool aborted() const noexcept = 0;
};

// Machine-readable category of a failed request, replacing the bare error
// string in retry/classification decisions (kosong's APIConnectionError /
// APITimeoutError / APIEmptyResponseError / APIStatusError taxonomy). The
// providers set this together with error_status / retry_after_seconds.
enum class ChatErrorKind : uint8_t {
    none = 0,         // no error
    connection,       // transport-level connection failure (no HTTP status)
    timeout,          // read/connect timeout (no HTTP status)
    empty_response,   // 200 body with no usable content (empty / think-only)
    http, // non-200 HTTP response (see error_status)
    not_supported, // capability pre-flight refusal (LLMNotSupported)
    aborted, // cancelled mid-stream by the caller's AbortCheck (G8): the
             // provider stopped reading the response body and returned
             // promptly; never retryable, and the agent loop classifies it
             // as a cancelled turn rather than a step failure.
};

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
    int64_t cached_tokens = 0;     // cache-read input tokens
    int64_t cache_creation_tokens = 0; // anthropic cache-creation input tokens
    int64_t total_tokens = 0;
    // Structured error classification (set only when ok == false).
    ChatErrorKind error_kind = ChatErrorKind::none;
    int32_t error_status = 0;        // HTTP status for kind == http, else 0
    double retry_after_seconds = 0;  // Retry-After hint (429), 0 == absent
};

// The capabilities a message needs before it may be sent
// (soul/message.py check_message): today only the thinking blocks an
// assistant message may carry; image/video parts gate on Phase-5 media.
ModelCapabilities message_required_capabilities(
    const kimix::vector<Message> &messages) noexcept;

// ---------------------------------------------------------------------------
// E4: tool-call pairing repair
// (kosong/contrib/chat_provider/common.py normalize_tool_call_ids)
// ---------------------------------------------------------------------------
// Rewrite invalid historical tool-call ids to a safe, portable shape BEFORE a
// provider serializes the messages. Histories persisted from other providers
// (or older sessions) can contain ids strict backends reject (Anthropic and
// Moonshot 400 on ids like "Read:9" or ids longer than 64 chars), so ids are
// sanitized to [a-zA-Z0-9_-] (every other character becomes '_'), truncated to
// 64 characters, and made unique with "_2"/"_3"... suffixes; assistant
// tool_calls entries and their matching tool messages are rewritten
// consistently (ids are mapped by their RAW value, so a tool_call and its tool
// result that share a raw id keep sharing the normalized one). A tool-role
// message whose id is empty is repaired to "tool_call" (+"_2"/"_3" on
// collision) - the reference's _EMPTY_TOOL_CALL_ID. Every provider applies the
// same normalization defensively so cross-provider histories replay anywhere;
// the input vector is never mutated.
kimix::vector<Message> normalize_tool_call_ids(const kimix::vector<Message> &history);

// The Anthropic wire request derived from unified messages: the `system`
// prompt (system-role content joined with "\n") plus the converted messages.
struct AnthropicWireRequest {
    kimix::string system;
    kimix::vector<anthropic::ChatMessage> messages;
};

// Wire-seam helpers (exposed for tests): normalize_tool_call_ids + the
// per-provider conversion, exactly what each provider's chat() does before
// building the request body.
kimix::vector<openai::ChatMessage> openai_wire_messages(
    const kimix::vector<Message> &messages);
kimix::vector<openai_responses::InputItem> responses_wire_input(
    const kimix::vector<Message> &messages);
AnthropicWireRequest anthropic_wire_request(const kimix::vector<Message> &messages);

// Abstract chat provider interface (analogue of kosong's ChatProvider).
class ChatProvider {
public:
    virtual ~ChatProvider() = default;
    virtual kimix::string model_name() const = 0;
    virtual ChatResult chat(const kimix::vector<Message> &messages,
                            const kimix::vector<Tool> &tools,
                            const ChunkCallback &on_chunk,
                            const AbortCheck *abort = nullptr) const = 0;
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
                    const ChunkCallback &on_chunk = {},
                    const AbortCheck *abort = nullptr) const;

    // The provider's output-token budget (Config.max_tokens). The soul's
    // think-only retry escalation (kimisoul.py _before_step_retry_sleep)
    // reads and raises it through set_output_token_budget(); the provider
    // reads max_tokens from the SAME shared instance, so the escalation
    // reaches the wire.
    int32_t output_token_budget() const { return config_->max_tokens; }
    void set_output_token_budget(int32_t tokens) const;

    // Shared-config constructor: the provider must already hold the same
    // pointer, so the think-only budget escalation reaches the request the
    // provider builds. create_llm wires that up; other callers should prefer
    // the plain Config constructor.
    LLM(kimix::unique_ptr<ChatProvider> provider,
        kimix::shared_ptr<Config> config);

private:
    kimix::unique_ptr<ChatProvider> provider_;
    kimix::shared_ptr<Config> config_; // mutable through set_output_token_budget
};

// config.type selects the provider: "openai"|"openai_legacy" -> OpenAI Chat
// Completions; "openai_responses" -> OpenAI Responses; "anthropic" -> Anthropic.
// Returns null on unknown type / missing model or url (mirrors Python's None).
kimix::unique_ptr<LLM> create_llm(Config config);
kimix::unique_ptr<LLM> create_llm_from_file(const kimix::string &path);

// ---------------------------------------------------------------------------
// A11: typed create_llm failures (no exceptions)
// ---------------------------------------------------------------------------
// The reference create_llm (kimi_cli/llm.py) logs
// "Cannot create LLM: missing base_url or model (provider_type=...)" and
// returns None; its callers then surface soul/__init__.py's LLMNotSet, whose
// message is exactly "LLM not set". The enum + out-params carry the same
// information as structured data; the texts below are the reference wordings.
enum class CreateLlmError : uint8_t {
    none = 0,
    // Missing model or url: create_llm returned None -> callers raise
    // LLMNotSet("LLM not set").
    llm_not_set = 1,
    // config.type names no provider; in the reference the LLM would be
    // constructed with a null chat provider and fail the same way.
    unknown_provider_type = 2,
};

// Reference wording for `kind`:
//   llm_not_set           -> "LLM not set"
//   unknown_provider_type -> "LLM not set (unknown provider type '<type>')"
kimix::string create_llm_error_text(CreateLlmError kind,
                                    kimix::string_view provider_type = {});

// Typed factory: on success returns CreateLlmError::none and moves the LLM
// into `out`; on failure `out` is reset to null and, when `error` is given,
// it receives create_llm_error_text(kind, config.type).
CreateLlmError create_llm(const Config &config, kimix::unique_ptr<LLM> &out,
                          kimix::string *error = nullptr);

} // namespace kimix::llm
