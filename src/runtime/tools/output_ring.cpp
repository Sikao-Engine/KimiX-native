/*
 * output_ring.cpp - bounded background-job output buffer.
 * See output_ring.h for the retained-bytes contract and the reference
 * semantics (background/utils.py bounded_append).
 *
 * Pure C++ kernel: no Python includes, no RTTI, no exceptions. Character
 * counting uses the shared UTF-8 decoder, so a Python str's slicing semantics
 * (code points, surrogatepass bytes counted one each) are reproduced exactly.
 */

#include <core/kimix_core.h>

#include <runtime/common/utf8.h>
#include <runtime/tools/output_ring.h>

namespace kimix {
namespace runtime {
namespace tools {

namespace src_runtime_tools_output_ring {

inline uint64_t cp_count(kimix::string_view s) noexcept {
    if (kimix::runtime::common::is_ascii(s)) {
        return static_cast<uint64_t>(s.size());
    }
    return static_cast<uint64_t>(kimix::runtime::common::utf8_code_point_count(s));
}

// Byte offset of the `char_index`-th code point (s.size() when past the end).
inline size_t byte_offset_at(kimix::string_view s, uint64_t char_index) noexcept {
    if (kimix::runtime::common::is_ascii(s)) {
        return char_index > s.size() ? s.size() : static_cast<size_t>(char_index);
    }
    const char *it = s.data();
    const char *end = it + s.size();
    uint64_t idx = 0;
    while (it < end && idx < char_index) {
        kimix::runtime::common::decode_cp(it, end);
        ++idx;
    }
    return static_cast<size_t>(it - s.data());
}

// Drop leading code points from `s` until at most `keep` remain.
inline void trim_front_to(kimix::string &s, uint64_t keep) noexcept {
    const uint64_t n = cp_count(s);
    if (n <= keep) {
        return;
    }
    const size_t drop_bytes = byte_offset_at(kimix::string_view(s.data(), s.size()),
                                             n - keep);
    s.erase(0, drop_bytes);
}

} // namespace src_runtime_tools_output_ring
using namespace src_runtime_tools_output_ring;

OutputRing::OutputRing(uint64_t cap_chars, double head_ratio) : _cap(cap_chars) {
    double ratio = head_ratio;
    if (ratio < 0.0) {
        ratio = 0.0;
    }
    if (ratio > 1.0) {
        ratio = 1.0;
    }
    // int(cap * 0.4) - the reference's Python float multiply, truncated.
    _head_len = static_cast<int64_t>(static_cast<double>(_cap) * ratio);
    if (_head_len < 0) {
        _head_len = 0;
    }
    if (static_cast<uint64_t>(_head_len) > _cap) {
        _head_len = static_cast<int64_t>(_cap);
    }
    _tail_len = static_cast<int64_t>(_cap) - _head_len;
}

void OutputRing::marker(kimix::string &out) const noexcept {
    out = kimix::format(
        "\n[... (output truncated, keeping first {} and last {} chars)]\n",
        _head_len, _tail_len);
}

bool OutputRing::append(kimix::string_view chunk) noexcept {
    if (chunk.data() == nullptr) {
        chunk = kimix::string_view("", 0);
    }
    const uint64_t chunk_chars = cp_count(chunk);

    if (!_truncated) {
        if (_chars + chunk_chars <= _cap) {
            _buf.append(chunk.data(), chunk.size());
            _chars += chunk_chars;
            return false;
        }
        // First truncation: build head/tail out of the whole content.
        kimix::string full;
        full.reserve(_buf.size() + chunk.size());
        full.append(_buf.data(), _buf.size());
        full.append(chunk.data(), chunk.size());
        _buf.clear();
        build_truncated(full, _chars + chunk_chars);
        return true;
    }

    // Already truncated: only the tail moves (never the head, never a rebuild).
    _tail.append(chunk.data(), chunk.size());
    _tail_chars += chunk_chars;
    trim_tail();
    // After the first truncation the retained size is head + marker + tail,
    // which already exceeds the cap, so every append truncates again (exactly
    // what the reference's `if buf.tell() + len(text) > cap` does).
    return true;
}

void OutputRing::build_truncated(kimix::string &full, uint64_t total_chars) noexcept {
    if (total_chars <= _cap) {
        _buf = std::move(full);
        _chars = total_chars;
        return;
    }
    const kimix::string_view all(full.data(), full.size());
    const size_t head_bytes = byte_offset_at(all, static_cast<uint64_t>(_head_len));
    const size_t tail_bytes =
        _tail_len > 0
            ? byte_offset_at(all, total_chars - static_cast<uint64_t>(_tail_len))
            : full.size();
    _head.assign(full.data(), head_bytes);
    _tail.assign(full.data() + tail_bytes, full.size() - tail_bytes);
    _tail_chars = static_cast<uint64_t>(_tail_len);
    _truncated = true;

    kimix::string mk;
    marker(mk);
    _chars = static_cast<uint64_t>(_head_len) + cp_count(mk) +
             static_cast<uint64_t>(_tail_len);
}

void OutputRing::trim_tail() noexcept {
    if (_tail_len <= 0) {
        _tail.clear();
        _tail_chars = 0;
        return;
    }
    if (_tail_chars <= static_cast<uint64_t>(_tail_len)) {
        return;
    }
    trim_front_to(_tail, static_cast<uint64_t>(_tail_len));
    _tail_chars = static_cast<uint64_t>(_tail_len);
}

void OutputRing::snapshot(kimix::string &out) const noexcept {
    if (!_truncated) {
        out.assign(_buf.data(), _buf.size());
        return;
    }
    kimix::string mk;
    marker(mk);
    out.clear();
    out.reserve(_head.size() + mk.size() + _tail.size());
    out.append(_head.data(), _head.size());
    out.append(mk.data(), mk.size());
    out.append(_tail.data(), _tail.size());
}

void OutputRing::read(uint64_t char_offset, uint64_t max_chars,
                      kimix::string &out) const noexcept {
    out.clear();
    if (max_chars == 0) {
        return;
    }
    kimix::string full;
    snapshot(full);
    const kimix::string_view all(full.data(), full.size());
    const uint64_t n = char_count();
    if (char_offset >= n) {
        return;
    }
    uint64_t take = max_chars;
    if (take > n - char_offset) {
        take = n - char_offset;
    }
    const size_t begin = byte_offset_at(all, char_offset);
    const size_t end = byte_offset_at(all, char_offset + take);
    out.assign(full.data() + begin, end - begin);
}

uint64_t OutputRing::char_count() const noexcept {
    if (!_truncated) {
        return _chars;
    }
    kimix::string mk;
    marker(mk);
    return static_cast<uint64_t>(_head_len) + cp_count(mk) +
           static_cast<uint64_t>(_tail_len);
}

uint64_t OutputRing::byte_count() const noexcept {
    if (!_truncated) {
        return _buf.size();
    }
    kimix::string mk;
    marker(mk);
    return _head.size() + mk.size() + _tail.size();
}

uint64_t OutputRing::bytes_owned() const noexcept {
    return static_cast<uint64_t>(_buf.capacity()) +
           static_cast<uint64_t>(_head.capacity()) +
           static_cast<uint64_t>(_tail.capacity());
}

void OutputRing::clear() noexcept {
    _buf.clear();
    _head.clear();
    _tail.clear();
    _chars = 0;
    _tail_chars = 0;
    _truncated = false;
}

} // namespace tools
} // namespace runtime
} // namespace kimix
