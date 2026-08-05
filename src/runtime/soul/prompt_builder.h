/*
 * prompt_builder.h - compaction prompt builder (kimix::runtime::soul).
 *
 * Plan 016: one-pass string builder for soul/compaction.py
 * SimpleCompaction.prepare (319-390). Given the to-compact message slice
 * (Python computes preserve indices and slices), produces the exact text of
 * the compaction user message:
 *
 *   per message i: "## Message {i+1}\nRole: {role}\nContent:\n"
 *                  + concatenated TextPart texts
 *   then "\n" + prompts.COMPACT (or COMPACT_CASCADE when >= 3 messages carry
 *   the "Previous context has been compacted" marker)
 *   then "\n\n" + _MODE_GUIDANCE[balanced] (default mode).
 *
 * The optional `system_prompt` (when non-empty) is prepended followed by
 * "\n\n" -- the shim uses this to reproduce the full LLM prompt (system
 * message text + compact user message text); kosong sends the two halves as
 * separate messages, so the shim can split on the first "\n\n" or just keep
 * the compact text by passing an empty system prompt.
 */

#pragma once

#include <core/kimix_core.h>
#include <runtime/soul/message_view.h>

namespace kimix {
namespace runtime {
namespace soul {

// Build the compaction prompt text for `msgs` (the to-compact slice).
KIMIX_RUNTIME_API void build_compaction_prompt(
    kimix::span<const message_view> msgs,
    kimix::string_view system_prompt,
    kimix::string& out) noexcept;

} // namespace soul
} // namespace runtime
} // namespace kimix
