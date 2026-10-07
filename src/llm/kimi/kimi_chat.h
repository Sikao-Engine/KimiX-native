// kimi_chat.h - Kimi (Moonshot) chat provider: the C++ port of kosong's
// `kosong/chat_provider/kimi.py`. Kimi speaks the OpenAI Chat Completions
// wire format, so the message / tool-call / SSE-chunk types are shared with
// the OpenAI Chat provider (llm/openai/openai_chat.h + sse_parser.h); what
// differs is the request-body contract and the thinking round-trip rules:
//
//  * `thinking` is a first-class object on this contract:
//    {"type":"enabled","effort":<rank>} or {"type":"disabled"} - the effort
//    rank rides INSIDE thinking (no top-level `reasoning_effort`), concrete
//    ranks pass through verbatim (Kimi.with_thinking).
//  * `thinking.keep` (Moonshot preserved thinking): "all" + thinking not
//    disabled requires a `reasoning_content` field on EVERY assistant message
//    (empty string backfill); messages that carry thinking pass the reasoning
//    text back so Moonshot's thinking-mode histories replay.
//  * Assistant tool-call messages whose visible content is effectively empty
//    OMIT `content` entirely (the compat layer 400s on empty text parts).
//  * Tool names starting with '$' serialize as builtin_function entries;
//    other tool parameter schemas are normalized for Moonshot's strict
//    validator (local $ref inlining + explicit `type` completion).
//  * max_tokens is normalized to max_completion_tokens (clamped to
//    384000); neither goes on the wire when unset.
//  * The session id rides on the top-level `prompt_cache_key`.
//  * Usage: Moonshot's legacy top-level `cached_tokens` (or
//    `prompt_tokens_details.cached_tokens`) is reported as cache-read input.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "llm/common.h"
#include "llm/openai/openai_chat.h"

namespace kimix::llm {
class AbortCheck; // llm/llm.h - streaming abort hook
}

namespace kimix::llm::kimi {

// The reference's _MAX_OUTPUT_TOKENS clamp (kosong/chat_provider/
// openai_common.py): output budgets above this are capped before the wire.
constexpr int64_t kMaxOutputTokens = 384000;

// The Kimi shape of an OpenAI chat-completion message. Kimi's provider adds
// fields to the plain openai::ChatMessage: the `reasoning_content`
// round-trip text plus the flags distinguishing "omit content" from
// "content: null" (the reference pops the key for an assistant tool-call
// message whose visible content is effectively empty; the plain OpenAI Chat
// provider sends JSON null there instead).
struct ChatMessage {
    kimix::string role; // system | user | assistant | tool
    kimix::string content; // text backbone (ignored on the wire when parts)
    kimix::string tool_call_id;
    kimix::vector<openai::ToolCall> tool_calls;
    kimix::vector<kimix::llm::ContentPart> parts; // non-empty => content array
    // The assistant reasoning round-trip. `has_reasoning` distinguishes an
    // explicitly empty reasoning (ThinkPart(think="") must round-trip as
// "") from no reasoning at all.
    kimix::string reasoning_content;
    bool has_reasoning = false;
    // True => the `content` key is omitted from the wire object entirely
    // (assistant tool-call with effectively empty visible content).
    bool omit_content = false;
    // True => `content` serializes as JSON null.
    bool null_content = false;
};

// Convert one already-normalized OpenAI chat-completion message (plus the
// unified thinking round-trip text) into the Kimi wire shape, applying the
// Kimi-specific rules of Kimi._convert_message:
//   - the think parts and the `thinking` argument are split out into
//     `reasoning_content` (has_reasoning even when the text is empty - an
//     explicitly empty reasoning must round-trip),
//   - an assistant tool-call message whose visible content is effectively
//     empty (no visible parts, or text parts that are whitespace-only) omits
//     `content` entirely (omit_content),
//   - a content-less plain message falls back to `content: null`
//     (null_content) like the OpenAI Chat provider,
//   - `preserved_thinking_enabled` (thinking.keep == "all", thinking not
//     disabled) backfills an empty reasoning_content onto every assistant
//     message lacking one.
// `m.parts` carries ALL parts (text and think included) when non-empty; the
// text backbone stays in `m.content`.
ChatMessage convert_message(const openai::ChatMessage &m, kimix::string_view thinking,
                            bool preserved_thinking_enabled);

// kosong openai_common.is_effectively_empty_content_parts: True when the
// visible parts carry no non-whitespace text (only text parts can be empty;
// any media part is never effectively empty). `has_parts` selects the parts
// view (the backbone stands for the implicit single text part otherwise).
bool visible_content_effectively_empty(const kimix::vector<ContentPart> &parts,
                                       kimix::string_view backbone, bool has_parts);

// Build the Kimi JSON request body (exposed for tests and debugging). When
// `out_error` is given it receives the reason for a failed build.
// The thinking configuration follows Kimi.with_thinking / with_extra_body:
// thinking {type,effort[,keep]} while thinking is enabled (effort taken from
// Config.thinking_effort), {"type":"disabled"[,"keep"]} while disabled; no
// top-level `reasoning_effort` is ever sent on this contract.
kimix::string build_chat_body(const Config &cfg, const kimix::vector<ChatMessage> &messages,
                              const kimix::vector<openai::Tool> &tools,
                              kimix::string *out_error = nullptr);

// Convert one Kimi-style tool definition (kosong kimi._convert_tool): names
// starting with '$' become builtin_function entries (name only, no
// description / parameters); otherwise the OpenAI function shape is returned
// with its parameters schema normalized by normalize_tool_parameters. The
// return value is the serialized JSON of the whole tool object ("" on
// failure). `openai::Tool` carries the same three fields as the unified
// kimix::llm::Tool, so the wire seam passes either through unchanged.
kimix::string convert_tool_json(const openai::Tool &tool);

// Moonshot-compatibility normalizer for a tool parameter schema (the port of
// kosong.utils.jsonschema.deref_json_schema + ensure_property_types applied
// by kimi._convert_tool): inline local `#/$defs/...` and
// `#/definitions/...` `$ref` pointers (sibling keys win over the resolved
// definition, cycles keep their `$ref`), drop the definition buckets when no
// unresolved pointer into them remains, then ensure every nested property
// schema declares a `type` (inferred from enum/const values, then from
// structural keywords, falling back to "string"; combinator nodes are left
// alone) and repair an explicit `type` contradicting the enum/const values.
// Returns the normalized schema serialized; on unparseable input an empty
// string (callers keep the original).
kimix::string normalize_tool_parameters(kimix::string_view parameters_json);

// Stream one Kimi chat completion request (the transport mirrors
// openai::chat_completion_stream: same client setup, retries and backoff);
// the body is built by build_chat_body and the usage's Moonshot
// cached_tokens field is mapped onto ChatResult.cached_tokens.
openai::ChatResult chat_completion_stream(const Config &cfg,
                                          const kimix::vector<ChatMessage> &messages,
                                          const kimix::vector<openai::Tool> &tools,
                                          const openai::ChunkCallback &on_chunk,
                                          const AbortCheck *abort = nullptr);

} // namespace kimix::llm::kimi
