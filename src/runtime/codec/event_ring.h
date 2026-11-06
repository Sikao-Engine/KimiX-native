/*
 * event_ring.h - bounded byte ring of serialized wire frames
 * (runtime_py.codec).
 *
 * Report candidate A4. Today every streamed delta becomes a pydantic model, a
 * `model_dump_json()` string and an entry in an unbounded `Queue` (or one of
 * 2 048-deep subscriber queues): 130.1 B of peak allocation per delta, 628 173 B
 * live after 5 000 deltas, 247 425 B retained for 500 queued events.
 *
 * EventRing stores FRAMES (already-serialized bytes, produced by
 * codec.delta_frame / serialize_envelope) under an explicit byte budget: a push
 * that does not fit drops whole oldest frames first, so memory is a cap instead
 * of "queue depth x event size x subscribers". `pop_batch()` hands a batch to
 * Python as ONE bytes object (u32 count | (u32 len | bytes)*), so the consumer
 * splits it with memoryview and only materialises a Python object per frame it
 * actually sends.
 *
 * Example:
 *   kimix::runtime::codec::EventRing ring(1u << 20);
 *   ring.push(frame);
 *   kimix::string batch;
 *   ring.pop_batch(64, batch);
 *
 * Pure C++ kernel: no Python includes, no RTTI, no exceptions, kimix
 * containers, KIMIX_RUNTIME_API exports. Not thread-safe: the serve layer's bus
 * serialises access (the binding keeps the GIL around the call).
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace codec {

class KIMIX_RUNTIME_API EventRing {
public:
    explicit EventRing(uint64_t capacity_bytes);
    ~EventRing() = default;
    EventRing(const EventRing &) = delete;
    EventRing &operator=(const EventRing &) = delete;

    // Queue one frame. Frames that do not fit after dropping the oldest ones
    // are accepted as long as they fit the budget alone; a frame larger than
    // the whole ring is rejected (false, nothing queued).
    bool push(kimix::string_view frame);

    // `u32 count | count * (u32 len | len bytes)` - one allocation.
    void pop_batch(uint32_t max_items, kimix::string &packed);

    uint32_t size() const noexcept { return static_cast<uint32_t>(_frames.size()); }
    uint64_t pending_bytes() const noexcept { return _bytes; }
    uint64_t dropped() const noexcept { return _dropped; }
    uint64_t capacity_bytes() const noexcept { return _capacity; }
    uint64_t bytes_owned() const noexcept;
    void clear() noexcept;

private:
    kimix::deque<kimix::string> _frames;
    uint64_t _capacity = 0;
    uint64_t _bytes = 0;
    uint64_t _dropped = 0;
};

} // namespace codec
} // namespace runtime
} // namespace kimix
