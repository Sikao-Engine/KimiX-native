/*
 * normalize_plan.h - history normalization plan (kimix::runtime::soul).
 *
 * Plan 015: single-pass scan implementing
 * soul/dynamic_injection.py::normalize_history (58-84): adjacent user
 * messages are merged (dynamic injections are stored as standalone user
 * messages), unless either side is a notification message. Assistant / tool
 * messages are never merged (their tool_calls / tool_call_id form linked
 * pairs). Returns a PLAN of per-message steps Python applies.
 *
 * Step ops:
 *   0 = keep (no action; target_index = own index)
 *   1 = merge_into_target: append this message's parts to the output
 *       message that originated at target_index
 *   3 = drop (reserved; not emitted by normalize_history)
 */

#pragma once

#include <core/kimix_core.h>
#include <runtime/soul/message_view.h>

namespace kimix {
namespace runtime {
namespace soul {

enum : uint8_t {
    kNormalizeKeep = 0,
    kNormalizeMergeInto = 1,
    kNormalizeDrop = 3,
};

struct normalize_step {
    uint32_t index;       // message index in the input history
    uint8_t op;           // kNormalizeKeep / kNormalizeMergeInto / kNormalizeDrop
    uint32_t target_index; // for merge: output message originating at this index
};

// Build the merge plan for `msgs` (index-ascending, one step per message).
// Python applies it by rebuilding the list: keep steps copy the message;
// merge steps append the parts of the current message to the output message
// that originated at target_index.
KIMIX_RUNTIME_API void build_normalize_plan(
    kimix::span<const message_view> msgs,
    kimix::vector<normalize_step>& out) noexcept;

} // namespace soul
} // namespace runtime
} // namespace kimix
