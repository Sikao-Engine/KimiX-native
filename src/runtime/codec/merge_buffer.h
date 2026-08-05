/*
 * merge_buffer.h -- Incremental wire merge buffer (kimix::runtime::codec).
 *
 * Plan 007: replaces `copy.deepcopy(msg)` + `merge_in_place` per streamed
 * chunk in WireSoulSide.send (wire/__init__.py:76-98). The buffer holds a
 * single accumulating JSON/text payload; the merge rules (what is
 * mergeable) stay Python-side, this kernel only stores bytes:
 *
 *   - append(kind, delta): returns true when the part was appended.
 *     An EMPTY buffer accepts any kind (starts a new merge group).
 *     A non-empty buffer appends only when `kind` equals the stored kind
 *     AND kind is mergeable. Mergeable kinds: "text", "args" (the plan's
 *     same-kind text/args rule). Any other case returns false and leaves
 *     the buffer untouched -- the caller must flush() and retry.
 *   - snapshot(): O(1) view of the accumulated bytes (no deepcopy).
 *   - reset(): drop kind + bytes (start a new group).
 *
 * Appends are amortized O(1) (growing kimix::string, no per-chunk
 * reallocation of the whole prefix -- this is what kills the Python
 * O(n?) `function.arguments += part` / deepcopy pattern).
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace codec {

class KIMIX_RUNTIME_API WireMergeBuffer {
public:
    WireMergeBuffer() = default;
    ~WireMergeBuffer() = default;

    WireMergeBuffer(const WireMergeBuffer&) = delete;
    WireMergeBuffer& operator=(const WireMergeBuffer&) = delete;

    // Append a part. Returns false when the part is not mergeable with the
    // current group (caller must flush + retry). See header comment.
    bool append(kimix::string_view kind, kimix::string_view delta) noexcept;

    // Accumulated bytes of the current merge group (O(1) view).
    kimix::string_view snapshot() const noexcept;

    // Drop the current group (kind + bytes).
    void reset() noexcept;

    bool empty() const noexcept;

    // The kind of the current merge group (empty when no group).
    kimix::string_view kind() const noexcept;

private:
    kimix::string kind_;
    kimix::string data_;
};

} // namespace codec
} // namespace runtime
} // namespace kimix
