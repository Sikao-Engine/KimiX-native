/*
 * request_builder.h - the provider request-body builder over the arena
 * (runtime_py.soul).
 *
 * Report candidate B1: three history-shaped copies per LLM step -
 * normalize_history (merge adjacent users), repair_tool_message_pairing (an id
 * map plus a second output list) and _convert_message (model_copy + model_dump)
 * - plus the SDK's own body serialisation. Over a 750-message history the
 * measured transients are 133 KB + 603 KB before the 844 KB body.
 *
 * Here one kernel emits the finished JSON body from the arena into ONE
 * growable output buffer: pairing repair -> id normalisation -> user merge ->
 * per-provider envelope -> yyjson serialisation. The arena is reused across
 * steps, so steady-state churn per step is the output buffer only.
 *
 * ENVELOPE CONTRACT (pinned by tests/unit/native/test_request_builder.cpp and
 * python/tests/data/request_bodies/*.json; keep this list in sync):
 *
 *   openai_legacy / kimi  (chat/completions shape, mirrors src/llm/openai and
 *                          src/llm/kimi build_chat_body)
 *     {"model", "stream", "stream_options":{"include_usage":true},
 *      "max_tokens"|"max_completion_tokens", "temperature",
 *      "messages":[{ "role", "content"|[…blocks]|null, "tool_calls", "tool_call_id" }],
 *      "tools":[…], "tool_choice", "reasoning_effort"}
 *     content is a plain string for a single visible TEXT part (the reference
 *     Message._serialize_content rule), otherwise an array of part blocks;
 *     think parts never appear (they become `reasoning_content` on the message
 *     when preserve_thinking is set and reasoning_key is non-empty).
 *
 *   anthropic  (messages shape, mirrors src/llm/anthropic build body)
 *     {"model", "stream", "max_tokens", "system"[str],
 *      "messages":[{"role":"user"|"assistant", "content":[…blocks]}],
 *      "tools":[…], "temperature"}
 *     blocks: {"type":"text","text"}, {"type":"thinking","thinking"},
 *     {"type":"tool_use","id","name","input"}, {"type":"tool_result",
 *     "tool_use_id","content":[{"type":"text","text"}]}; a system history
 *     message is folded into "system" / a <system>-wrapped user block.
 *
 *   openai_responses  (responses shape, mirrors src/llm/openai_responses)
 *     {"model", "stream", "instructions", "max_output_tokens", "temperature",
 *      "input":[{"role","content":[…]} | {"type":"function_call",…} |
 *               {"type":"function_call_output",…}], "tools":[…]}
 *
 * Byte-exact parity with a provider SDK body is NOT claimed by this kernel: the
 * SDK transport owns its own serialiser. The shim's `_compat_build_request`
 * mirror and the recorded goldens pin the NATIVE bytes, and every provider sits
 * behind its own `KIMIX_NATIVE_REQUEST_<PROVIDER>` gate, so a consumer records
 * the real SDK body for its case before flipping a provider on (see
 * python/kimix_native/soul.py --diff-bodies).
 *
 * Example:
 *   kimix::runtime::soul::request_options ro;
 *   ro.provider = kimix::runtime::soul::provider_kind::anthropic;
 *   ro.model = "claude-sonnet-4";
 *   ro.max_tokens = 8192;
 *   kimix::string body;
 *   if (build_request(history.views(), ro, body)) { ... }
 *
 * Pure C++ kernel: no Python includes, no RTTI, no exceptions, kimix
 * containers, KIMIX_RUNTIME_API exports.
 */

#pragma once

#include <core/kimix_core.h>

#include <runtime/soul/message_view.h>

namespace kimix {
namespace runtime {
namespace soul {

enum class provider_kind : uint8_t {
    openai_legacy = 0,
    openai_responses = 1,
    anthropic = 2,
    kimi = 3,
};

// Parse "openai_legacy" | "openai" | "openai_responses" | "responses" |
// "anthropic" | "kimi"; returns false on an unknown name.
KIMIX_RUNTIME_API bool parse_provider(kimix::string_view name,
                                      provider_kind &out) noexcept;
KIMIX_RUNTIME_API kimix::string_view provider_name(provider_kind kind) noexcept;

struct request_options {
    provider_kind provider = provider_kind::openai_legacy;
    bool preserve_thinking = false;
    bool exclude_none = true;
    bool stream = true;
    int64_t max_tokens = 0; // 0 == unset
    double temperature = 0.0;
    bool has_temperature = false;
    kimix::string_view model;
    kimix::string_view system_prompt;  // anthropic/claude + responses "instructions"
    kimix::string_view tool_choice;
    kimix::string_view reasoning_effort;
    // Reasoning field name on an assistant message ("reasoning_content" for the
    // DeepSeek/Kimi family); empty == do not emit one.
    kimix::string_view reasoning_key;
    // Already-serialized tool schema array (the caller owns the schema text).
    kimix::string_view tools_json;
};

// Merge adjacent user messages (the reference normalize_history): a
// <system-reminder> or notification user message is never merged, because the
// request-time boundary must match storage. Both outputs are fresh KMB2 blobs
// (the input blob stays untouched and may be the same pair).
KIMIX_RUNTIME_API void normalize_history(kimix::span<const message_view> in,
                                         kimix::string &structure,
                                         kimix::string &history) noexcept;

// Repair assistant tool_call / tool-result pairing (the reference
// repair_tool_message_pairing): tool results move directly behind their
// assistant message, a missing result becomes a synthetic placeholder, orphan
// and duplicate results are dropped. Fresh KMB2 blobs.
KIMIX_RUNTIME_API void repair_tool_pairing(kimix::span<const message_view> in,
                                           kimix::string &structure,
                                           kimix::string &history) noexcept;

// Repair -> normalize -> per-provider envelope -> JSON body in `out`.
// False when the body could not be produced (out is empty then).
KIMIX_RUNTIME_API bool build_request(kimix::span<const message_view> in,
                                     const request_options &opts,
                                     kimix::string &out) noexcept;

} // namespace soul
} // namespace runtime
} // namespace kimix
