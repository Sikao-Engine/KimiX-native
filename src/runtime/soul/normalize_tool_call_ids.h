/*
 * normalize_tool_call_ids.h - tool-call id normalization plan
 * (kimix::runtime::soul).
 *
 * Plan 014: single-pass scan implementing
 * kosong/chat_provider/kimi.py::_normalize_tool_call_ids (331-401):
 * sanitize invalid ids to [a-zA-Z0-9_-], truncate to 64 chars, dedupe with
 * _2/_3... suffixes, and report every id that must be rewritten. Returns a
 * PLAN (list of fixes) that Python applies to its pydantic messages -- the
 * kernel never mutates anything.
 */

#pragma once

#include <core/kimix_core.h>
#include <runtime/soul/message_view.h>

namespace kimix {
namespace runtime {
namespace soul {

// One id rewrite. call_index is the index into message.tool_calls; the
// sentinel UINT32_MAX means the fix applies to the message's tool_call_id
// field (a role="tool" message).
struct id_fix {
    uint32_t msg_index;
    uint32_t call_index;
    kimix::string new_id;
};

// Scan `msgs` and append every needed rewrite to `out` (index-ascending,
// tool_calls before tool_call_id per message -- the Python fix order).
// A message whose tool_calls span is empty is treated as tool_calls=None;
// an empty tool_call_id span as None.
KIMIX_RUNTIME_API void normalize_tool_call_ids(
    kimix::span<const message_view> msgs,
    kimix::vector<id_fix>& out) noexcept;

} // namespace soul
} // namespace runtime
} // namespace kimix
