// llm/stream_filter.h - E9 (audit row 722): the reusable empty-part stream
// filter. The reference wraps every chat call in _EmptyPartFilteredChatProvider
// / _aiter_without_empty_parts so a provider emitting blank deltas (empty
// content AND empty reasoning AND no tool-call payload) can never truncate a
// downstream accumulator. The OpenAI Chat provider's index-keyed accumulator
// is structurally immune to blank deltas (pinned by
// empty_deltas_do_not_truncate_tool_arguments), but the unified adapters in
// llm.cpp route ALL providers' chunks through this filter so a new consumer
// (or a provider change) cannot regress the guarantee.

#pragma once

#include "llm/llm.h"

namespace kimix::llm {

// True when the chunk carries no information a consumer could act on: no
// text, no reasoning, no tool-call delta, no role change, no finish reason
// and no usage. Such chunks are exactly what the reference filters out.
inline bool chunk_is_empty(const Chunk &chunk) noexcept {
    return !chunk.done && chunk.role.empty() && chunk.content.empty() &&
           chunk.reasoning.empty() && chunk.tool_calls.empty() &&
           chunk.finish_reason.empty() && !chunk.has_usage;
}

// A ChunkCallback wrapper that forwards every non-empty chunk to `next`
// (the reference's _aiter_without_empty_parts, as a synchronous filter).
class EmptyPartFilter {
public:
    explicit EmptyPartFilter(const ChunkCallback &next) : _next(next) {}

    void operator()(const Chunk &chunk) const {
        if (chunk_is_empty(chunk)) {
            return;
        }
        if (_next) {
            _next(chunk);
        }
    }

private:
    const ChunkCallback &_next;
};

} // namespace kimix::llm
