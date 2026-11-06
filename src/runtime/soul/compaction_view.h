/*
 * compaction_view.h - compaction / prune decision kernels over the arena
 * (runtime_py.soul).
 *
 * Report candidate B3. The kernels exist (kimix::builtin_tools::compact, see
 * src/builtin_tools/compact_tool.h) and are bound in `list[dict]` shape from
 * runtime_py.builtin_tools.web - the measured regression is the bridge itself
 * (0.43 ms / 76 KB in Python against 4.66 ms / 435 KB through the dict bridge).
 * These wrappers take `kimix::span<const message_view>` instead, which is what
 * soul.History hands out, so the decision path runs over the arena with no
 * per-message Python object.
 *
 * project() copies the views into a REUSED compaction_scratch: that copy is the
 * honest cost of not changing the compact kernel's storage, it is paid from a
 * buffer that is never freed between steps, and it therefore produces no GC
 * churn (the report's actual complaint). A follow-up can add
 * `compact::message_view` overloads if profiling ever asks for it.
 *
 * Example:
 *   kimix::runtime::soul::compaction_scratch scratch;
 *   kimix::vector<uint32_t> cuts;
 *   bool unbalanced = false;
 *   int64_t bad_index = 0;
 *   balanced_cut_indices(history.views(), cuts, unbalanced, bad_index);
 *
 * Pure C++ kernel: no Python includes, no RTTI, no exceptions, kimix
 * containers, KIMIX_RUNTIME_API exports.
 */

#pragma once

#include <core/kimix_core.h>

#include <builtin_tools/compact_tool.h>
#include <runtime/soul/message_view.h>

namespace kimix {
namespace runtime {
namespace soul {

// Reusable projection buffer: one allocation on first use, then steady-state
// zero allocations per call.
struct compaction_scratch {
    kimix::vector<builtin_tools::compact::message> msgs;
};

// Copy `in` into `s.msgs` (clearing it first). Role and part kinds are mapped
// onto the compact kernel's message shape: text -> "text", think -> "think",
// tool_call -> "tool_call" (the marker message_tool_call_delta counts); every
// other kind becomes an opaque "other" part carrying its serialized JSON.
KIMIX_RUNTIME_API void project(kimix::span<const message_view> in,
                               compaction_scratch &s) noexcept;

// tool_pairing.balanced_cut_indices: ascending cut positions (always includes
// 0 and size()). `unbalanced_index` is -1 when the history is balanced.
KIMIX_RUNTIME_API void balanced_cut_indices(kimix::span<const message_view> in,
                                            kimix::vector<uint32_t> &cuts,
                                            bool &unbalanced,
                                            int64_t &unbalanced_index) noexcept;

// Largest balanced cut <= index (clamped like the reference).
KIMIX_RUNTIME_API uint32_t nearest_balanced_cut_before(
    kimix::span<const message_view> in, uint32_t index, bool &unbalanced) noexcept;

// SimpleCompaction.prepare's boundary math (preserve-depth walk, balanced-cut
// snap, primacy re-insertion and its re-cut).
KIMIX_RUNTIME_API void resolve_preserve_split(kimix::span<const message_view> in,
                                              int32_t preserve_depth,
                                              bool balanced_cuts,
                                              builtin_tools::compact::preserve_split &out) noexcept;

// Token-count gate (compaction_trigger_config, not compaction_options: the
// trigger lives in its own struct in the kernel).
KIMIX_RUNTIME_API bool should_auto_compact(
    int64_t token_count,
    const builtin_tools::compact::compaction_trigger_config &cfg) noexcept;

// Assemble the compaction instruction prompt into `out`. Returns false when the
// kernel reported a failure (prepare_out untouched then).
KIMIX_RUNTIME_API bool build_compaction_prompt(
    kimix::span<const message_view> in,
    const builtin_tools::compact::compaction_options &opts,
    kimix::string_view custom_instruction, kimix::string_view prompt_compact,
    kimix::string_view prompt_compact_cascade,
    builtin_tools::compact::compaction_prompt_output &out) noexcept;

// Slice + prompt + legacy compact-message text. `preserve_start_index` is
// assumed to respect balanced tool pairing already (the caller computes it with
// resolve_preserve_split). The caller re-slices the VIEWS with the same index to
// get the two history blobs; the kernel result carries the texts.
KIMIX_RUNTIME_API bool prepare_compaction_input(
    kimix::span<const message_view> in, size_t preserve_start_index,
    const builtin_tools::compact::compaction_options &opts,
    kimix::string_view custom_instruction, kimix::string_view prompt_compact,
    kimix::string_view prompt_compact_cascade,
    builtin_tools::compact::prepare_result &out) noexcept;

// Join every TEXT part of every message with `sep` (the reference
// `sep.join(text parts)` over the whole history).
KIMIX_RUNTIME_API void extract_text(kimix::span<const message_view> in,
                                    kimix::string_view sep,
                                    kimix::string &out) noexcept;

// Arena analogue of count_message_tokens: sum of the heuristic token estimate
// over every TEXT part.
KIMIX_RUNTIME_API int64_t count_tokens(kimix::span<const message_view> in) noexcept;

// Legacy flattened compact_message text over an explicit view range.
KIMIX_RUNTIME_API void build_compact_message_text(
    kimix::span<const message_view> in, kimix::string_view prompt_text,
    kimix::string &out) noexcept;

} // namespace soul
} // namespace runtime
} // namespace kimix
