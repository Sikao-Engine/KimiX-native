/*
 * event_bus.cpp - see event_bus.h (plan 008).
 *
 * Event indices are 0-based: event i lives at ring slot i % capacity.
 * _seq is the total number of events emitted (the index of the NEXT event).
 * A subscriber's offset is the 0-based index of the next event it wants to
 * read. The oldest retained event has index _seq - _count. When the ring
 * is full, the next emit overwrites slot _seq % capacity (the oldest
 * event), so a subscriber whose offset falls below _seq - _count skips
 * the dropped events (DROP_OLDEST applies to the shared ring).
 */

#include <runtime/concurrency/event_bus.h>

namespace kimix {
namespace runtime {
namespace concurrency {

MpscEventBus::MpscEventBus(size_t capacity) {
    if (capacity == 0) {
        capacity = 1; // a degenerate bus must still function
    }
    _events.resize(capacity);
}

MpscEventBus::~MpscEventBus() = default;

void MpscEventBus::emit(kimix::string_view event_bytes) noexcept {
    std::lock_guard<std::mutex> lock(_mtx);
    const size_t slot = static_cast<size_t>(_seq) % _events.size();
    _events[slot].assign(event_bytes.data(), event_bytes.size());
    if (_count < _events.size()) {
        ++_count;
    }
    ++_seq;
}

uint64_t MpscEventBus::subscribe() noexcept {
    std::lock_guard<std::mutex> lock(_mtx);
    const uint64_t id = _next_sub_id++;
    _offsets.emplace(id, _seq); // start at the current tail (next event index)
    return id;
}

void MpscEventBus::unsubscribe(uint64_t id) noexcept {
    std::lock_guard<std::mutex> lock(_mtx);
    _offsets.erase(id);
}

bool MpscEventBus::poll(uint64_t id, kimix::string& out) noexcept {
    std::lock_guard<std::mutex> lock(_mtx);
    auto it = _offsets.find(id);
    if (it == _offsets.end()) {
        return false; // unknown/unsubscribed
    }
    uint64_t& offset = it->second;
    if (offset >= _seq) {
        return false; // caught up
    }
    const uint64_t oldest = _seq - static_cast<uint64_t>(_count);
    if (offset < oldest) {
        offset = oldest; // skip events dropped by the ring
    }
    if (offset >= _seq) {
        return false; // everything the subscriber missed was dropped
    }
    out.assign(_events[static_cast<size_t>(offset) % _events.size()]);
    ++offset;
    return true;
}

uint64_t MpscEventBus::seq() const noexcept {
    std::lock_guard<std::mutex> lock(_mtx);
    return _seq;
}

} // namespace concurrency
} // namespace runtime
} // namespace kimix
