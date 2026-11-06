/*
 * output_ring.h - bounded background-job output buffer (runtime_py.tools).
 *
 * Report candidate C1. Today a background job keeps an `io.StringIO` capped by
 * `bounded_append` (BACKGROUND_MAX_OUTPUT_CHARS = 2 000 000): every truncation
 * runs `truncate(0)` + `write(whole new content)` - a full rebuild - and every
 * `get_output()` poll copies the whole buffer again. A dozen live jobs is tens
 * of MB of pure buffer, and the copy is on the polling path.
 *
 * OutputRing owns the bytes natively and keeps the reference's exact semantics
 * (head 40 % / marker / tail 60 %, marker text and cap measured in CODE POINTS,
 * like Python str slicing):
 *
 *   - append() never rewrites the retained buffer: once truncated it appends to
 *     the tail only and trims the tail's front, so a 2 M-char ring fed by 1 KB
 *     chunks is O(chunk) per append instead of O(cap);
 *   - snapshot() is the only place that materialises head + marker + tail;
 *   - read(offset, n) serves an incremental drain without copying the ring.
 *
 * Example:
 *   kimix::runtime::tools::OutputRing ring(2'000'000);
 *   ring.append("line 1\n");
 *   kimix::string view;
 *   ring.snapshot(view);
 *
 * Pure C++ kernel: no Python includes, no RTTI, no exceptions, kimix
 * containers, KIMIX_RUNTIME_API exports.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace tools {

class KIMIX_RUNTIME_API OutputRing {
public:
    explicit OutputRing(uint64_t cap_chars, double head_ratio = 0.4);
    ~OutputRing() = default;
    OutputRing(const OutputRing &) = delete;
    OutputRing &operator=(const OutputRing &) = delete;

    // Append one chunk; true when this append truncated the buffer (the
    // reference's bounded_append return value).
    bool append(kimix::string_view chunk) noexcept;

    // The retained text (head + marker + tail once truncated).
    void snapshot(kimix::string &out) const noexcept;
    // Characters [char_offset, char_offset + max_chars) of the snapshot, cut on
    // code-point boundaries (a drain never copies the whole ring).
    void read(uint64_t char_offset, uint64_t max_chars, kimix::string &out) const noexcept;

    uint64_t char_count() const noexcept;
    uint64_t byte_count() const noexcept;
    bool truncated() const noexcept { return _truncated; }
    uint64_t cap_chars() const noexcept { return _cap; }
    uint64_t bytes_owned() const noexcept;
    void clear() noexcept;

private:
    void build_truncated(kimix::string &full, uint64_t total_chars) noexcept;
    void trim_tail() noexcept;
    void marker(kimix::string &out) const noexcept;

    uint64_t _cap = 0;
    int64_t _head_len = 0;
    int64_t _tail_len = 0;
    kimix::string _buf;  // pre-truncation content
    kimix::string _head; // first _head_len code points (post-truncation)
    kimix::string _tail; // last _tail_len code points (post-truncation)
    uint64_t _chars = 0;
    uint64_t _tail_chars = 0;
    bool _truncated = false;
};

} // namespace tools
} // namespace runtime
} // namespace kimix
