/*
 * recv_buffer.cpp -- see recv_buffer.h (plan 009).
 *
 * Length-prefix endianness: BIG-ENDIAN (tcp_client.py int.from_bytes(len,
 * "big") / struct.pack("!I", len)). Zero length is invalid (matches the
 * Python "length == 0" guard). max_frame == 0 -> 10 MiB default.
 */

#include <runtime/codec/recv_buffer.h>

#include <cstring>

namespace kimix {
namespace runtime {
namespace codec {

void RecvBuffer::append(kimix::string_view bytes) noexcept {
    if (bytes.empty()) {
        return;
    }
    // Diagnostic: if the append will force a reallocation, the existing
    // _buf.size() bytes get copied by the container (upper bound estimate).
    if (_buf.size() + bytes.size() > _buf.capacity()) {
        _copied_bytes += _buf.size();
    }
    _buf.append(bytes.data(), bytes.size());
}

bool RecvBuffer::maybe_compact() noexcept {
    if (_consumed > 0 && _consumed > _buf.size() / 2) {
        compact();
        return true;
    }
    return false;
}

void RecvBuffer::compact() noexcept {
    if (_consumed == 0) {
        return;
    }
    const size_t remaining = _buf.size() - _consumed;
    if (remaining > 0) {
        // Move the unconsumed remainder to the front (memmove: safe overlap).
        std::memmove(_buf.data(), _buf.data() + _consumed, remaining);
        _copied_bytes += remaining;
    }
    _buf.resize(remaining);
    _consumed = 0;
}

void RecvBuffer::clear() noexcept {
    _buf.clear();
    _consumed = 0;
}

bool RecvBuffer::take_frame_length_prefixed(uint32_t header_size,
                                            size_t max_frame,
                                            kimix::string& out) noexcept {
    if (header_size == 0 || header_size > 8) {
        return false; // unsupported header width
    }
    if (max_frame == 0) {
        max_frame = kDefaultMaxFrame;
    }
    const size_t avail = _buf.size() - _consumed;
    if (avail < header_size) {
        return false; // header not fully arrived yet
    }
    // Big-endian decode of the length prefix.
    uint64_t length = 0;
    for (uint32_t i = 0; i < header_size; ++i) {
        const unsigned char b = static_cast<unsigned char>(
            _buf[_consumed + i]);
        length = (length << 8) | b;
    }
    if (length == 0) {
        return false; // zero length is invalid (Python guard)
    }
    if (length > max_frame) {
        return false; // oversize -- nothing consumed; caller may clear()
    }
    const size_t frame_start = _consumed + header_size;
    if (_buf.size() < frame_start + length) {
        return false; // payload not fully arrived yet
    }
    out.assign(_buf.data() + frame_start, static_cast<size_t>(length));
    _consumed = frame_start + static_cast<size_t>(length);
    maybe_compact();
    return true;
}

bool RecvBuffer::take_frame_delimiter(kimix::string_view delim,
                                      size_t max_frame,
                                      kimix::string& out) noexcept {
    if (delim.empty()) {
        return false;
    }
    if (max_frame == 0) {
        max_frame = kDefaultMaxFrame;
    }
    const char* start = _buf.data() + _consumed;
    const size_t avail = _buf.size() - _consumed;
    const char* found = static_cast<const char*>(
        std::memchr(start, delim[0], avail));
    while (found != nullptr) {
        const size_t off = static_cast<size_t>(found - start);
        if (off + delim.size() > avail) {
            // Delimiter would cross the end of buffered data -- could still
            // complete on the next append, but if we already exceed max_frame
            // without a match this is an oversized frame.
            if (avail > max_frame) {
                return false;
            }
            return false; // incomplete
        }
        if (std::memcmp(found, delim.data(), delim.size()) == 0) {
            const size_t frame_len = off; // bytes before the delimiter
            if (frame_len > max_frame) {
                return false; // oversize -- nothing consumed
            }
            out.assign(start, frame_len);
            _consumed += off + delim.size();
            maybe_compact();
            return true;
        }
        found = static_cast<const char*>(
            std::memchr(found + 1, delim[0],
                        avail - (off + 1)));
    }
    // No delimiter anywhere in the buffered data yet.
    if (avail > max_frame) {
        return false; // would already be oversized once the delim arrives
    }
    return false; // incomplete
}

} // namespace codec
} // namespace runtime
} // namespace kimix
