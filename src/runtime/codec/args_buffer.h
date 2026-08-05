/*
 * args_buffer.h -- Incremental ACP args buffer (kimix::runtime::codec).
 *
 * Plan 010: replaces `self.args += args_part` O(n?) accumulation in the
 * ACP tool-call stream. Growing kimix::string (amortized O(1) append);
 * `delta_since` hands out only the bytes appended since the last call so
 * `_send_tool_call_part` can send the per-chunk delta instead of the full
 * accumulated args (kills O(n?) wire bytes).
 *
 * The watermark is CALLER-OWNED (passed by reference): the binding layer
 * keeps the watermark across calls so the Python API is
 * `delta_since() -> bytes` with no arguments. `reset()` clears the bytes;
 * the caller resets its watermark alongside (the binding does both).
 *
 * Header-only kernel (all inline): no .cpp, no DLL export needed.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace codec {

class ArgsBuffer {
public:
    ArgsBuffer() = default;

    // Append a chunk of args bytes (amortized O(1)).
    void append(kimix::string_view delta) noexcept {
        data_.append(delta.data(), delta.size());
    }

    // Full args accumulated so far (O(1) view).
    kimix::string_view snapshot() const noexcept {
        return kimix::string_view(data_);
    }

    // Bytes appended since the last delta_since call (or since reset).
    // Advances `watermark` to the end of the buffer. The returned view
    // stays valid until the next append/reset.
    kimix::string_view delta_since(size_t& watermark) noexcept {
        const size_t end = data_.size();
        kimix::string_view delta(data_.data() + watermark, end - watermark);
        watermark = end;
        return delta;
    }

    // Drop all bytes. The caller resets its watermark to 0 alongside.
    void reset() noexcept { data_.clear(); }

    bool empty() const noexcept { return data_.empty(); }

    size_t size() const noexcept { return data_.size(); }

private:
    kimix::string data_;
};

} // namespace codec
} // namespace runtime
} // namespace kimix
