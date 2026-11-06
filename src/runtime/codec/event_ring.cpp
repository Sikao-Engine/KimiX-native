/*
 * event_ring.cpp - bounded byte ring of serialized wire frames.
 * See event_ring.h for the budget policy and the batch framing contract.
 *
 * Pure C++ kernel: no Python includes, no RTTI, no exceptions, kimix
 * containers. A frame is copied once on push and once on pop_batch (the batch
 * is the consumer's bytes object), so the steady-state cost per event is two
 * memcpys and no Python object at all.
 */

#include <core/kimix_core.h>

#include <runtime/codec/event_ring.h>
#include <runtime/common/le_bytes.h>

namespace kimix {
namespace runtime {
namespace codec {

namespace src_runtime_codec_event_ring {

using kimix::runtime::common::put_le32;

} // namespace src_runtime_codec_event_ring
using namespace src_runtime_codec_event_ring;

EventRing::EventRing(uint64_t capacity_bytes) : _capacity(capacity_bytes) {}

bool EventRing::push(kimix::string_view frame) {
    if (frame.size() > _capacity) {
        return false; // a single frame larger than the ring can never fit
    }
    while (!_frames.empty() && _bytes + frame.size() > _capacity) {
        _bytes -= _frames.front().size();
        _frames.pop_front();
        ++_dropped;
    }
    kimix::string copy;
    copy.assign(frame.data(), frame.size());
    _bytes += copy.size();
    _frames.push_back(std::move(copy));
    return true;
}

void EventRing::pop_batch(uint32_t max_items, kimix::string &packed) {
    packed.clear();
    uint32_t count = 0;
    size_t total = 4;
    for (const kimix::string &f : _frames) {
        if (count >= max_items) {
            break;
        }
        total += 4 + f.size();
        ++count;
    }
    packed.resize(total);
    char *p = packed.data();
    put_le32(p, count);
    p += 4;
    for (uint32_t i = 0; i < count; ++i) {
        const kimix::string &f = _frames.front();
        put_le32(p, static_cast<uint32_t>(f.size()));
        p += 4;
        if (!f.empty()) {
            memcpy(p, f.data(), f.size());
        }
        p += f.size();
        _bytes -= f.size();
        _frames.pop_front();
    }
}

uint64_t EventRing::bytes_owned() const noexcept {
    uint64_t total = static_cast<uint64_t>(_frames.size()) * sizeof(kimix::string);
    for (const kimix::string &f : _frames) {
        total += static_cast<uint64_t>(f.capacity());
    }
    return total;
}

void EventRing::clear() noexcept {
    _frames.clear();
    _bytes = 0;
}

} // namespace codec
} // namespace runtime
} // namespace kimix
