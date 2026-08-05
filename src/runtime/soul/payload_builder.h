/*
 * payload_builder.h - OpenAI payload conversion (kimix::runtime::soul).
 *
 * Plan 014: one yyjson write pass over message views implementing
 * kosong/chat_provider/kimi.py::_convert_message EXACTLY:
 *   - reasoning split (ThinkPart -> reasoning_content, visible content keeps
 *     the remaining parts in order),
 *   - exclude_none=True field dropping (name/partial/tool_call_id absent
 *     when unset; tool_calls absent when empty),
 *   - the Kimi compat quirk: assistant + tool_calls + effectively-empty
 *     visible content drops `content` entirely,
 *   - reasoning_content backfill "" on every assistant message when
 *     preserved_thinking is enabled,
 *   - single-TextPart content serializes as a plain string (pydantic field
 *     serializer); multi-part content is a JSON array of part dicts.
 */

#pragma once

#include <core/kimix_core.h>
#include <runtime/soul/message_view.h>

namespace kimix {
namespace runtime {
namespace soul {

// Serialize the whole history as an OpenAI ChatCompletionMessageParam JSON
// array in ONE yyjson write pass. `out_json` receives the compact JSON
// bytes (key order follows pydantic field order: role, content, tool_calls,
// tool_call_id, then reasoning_content).
KIMIX_RUNTIME_API void build_payload(kimix::span<const message_view> msgs,
                                     bool preserved_thinking_enabled,
                                     kimix::string& out_json) noexcept;

} // namespace soul
} // namespace runtime
} // namespace kimix
