/*
 * recv_buffer.h -- Growable TCP receive buffer (kimix::runtime::codec).
 *
 * Plan 009: replaces `data += chunk` O(n?) byte-concat in
 * tcp_client.py/_recv_all and friends. Appends into a growing kimix::string
 * (amortized O(1)); consumed bytes are compacted away only when they exceed
 * half the buffer, so repeated small-frame reads never copy the whole
 * buffer.
 *
 * Framing (verified against tcp_client.py):
 *   - length-prefixed: header_size bytes (default 4) interpreted as
 *     BIG-ENDIAN payload length (Python: int.from_bytes(length_bytes,
 *     "big") / struct.pack("!I", length)); length == 0 is invalid;
 *     max message 10 MiB (10 * 1024 * 1024) -- `max_frame == 0` means the
 *     10 MiB default. Oversize frames are rejected WITHOUT consuming them
 *     (the caller may clear() to resync).
 *   - delimiter: a frame is everything up to and including the first
 *     occurrence of `delim` (the delimiter bytes are NOT included in `out`).
 *
 * debug_bytes_copied() reports an upper-bound estimate of bytes moved by
 * internal reallocations/compactions (diagnostic; used by the property
 * test asserting total copies ~ O(payload), no O(n?)).
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace codec {

class KIMIX_RUNTIME_API RecvBuffer {
public:
    RecvBuffer() = default;
    ~RecvBuffer() = default;

    RecvBuffer(const RecvBuffer&) = delete;
    RecvBuffer& operator=(const RecvBuffer&) = delete;

    // Default max frame size: 10 MiB (mirrors tcp_client.py's 10MB cap).
    static constexpr size_t kDefaultMaxFrame = 10u * 1024u * 1024u;

    // Append bytes (amortized O(1)).
    void append(kimix::string_view bytes) noexcept;

    // Total buffered bytes (consumed + unconsumed).
    size_t size() const noexcept { return _buf.size(); }

    // Whole buffered bytes (consumed prefix included).
    kimix::string_view peek() const noexcept { return kimix::string_view(_buf); }

    // Try to extract one length-prefixed frame (BIG-ENDIAN header_size-byte
    // length). Returns true with the payload in `out` on success; false
    // when the header is incomplete, the payload is incomplete, or the
    // declared length exceeds max_frame (nothing is consumed on false).
    bool take_frame_length_prefixed(uint32_t header_size, size_t max_frame,
                                    kimix::string& out) noexcept;

    // Try to extract one delimiter-terminated frame. `delim` bytes are
    // consumed but NOT included in `out`. Returns false when the delimiter
    // has not arrived yet or the frame would exceed max_frame.
    bool take_frame_delimiter(kimix::string_view delim, size_t max_frame,
                              kimix::string& out) noexcept;

    // Drop the consumed prefix (moves the remainder to the front). Runs
    // automatically inside take_frame_* when _consumed > size()/2.
    void compact() noexcept;

    // Drop everything.
    void clear() noexcept;

    // Diagnostic: upper-bound estimate of bytes copied by reallocations and
    // compactions since construction (used by the no-O(n?) property test).
    size_t debug_bytes_copied() const noexcept { return _copied_bytes; }

private:
    bool maybe_compact() noexcept;

    kimix::string _buf;
    size_t _consumed = 0;   // bytes at the front already handed out
    size_t _copied_bytes = 0; // diagnostic copy counter (upper bound)

    // Delimiter scanning resumes from this offset (relative to the unconsumed
    // region) instead of rescanning the whole buffer on every chunk. Without
    // it a frame whose delimiter arrives only after many small appends turns
    // take_frame_delimiter into an O(n^2) rescan. Reset on clear(), on a
    // successful take, and whenever length-prefixed extraction consumes
    // bytes (mixed-mode callers).
    size_t _delim_scan_pos = 0;
};

} // namespace codec
} // namespace runtime
} // namespace kimix
