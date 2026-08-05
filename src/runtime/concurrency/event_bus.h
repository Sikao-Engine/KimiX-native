/*
 * event_bus.h -- Bounded MPSC event bus (kimix::runtime::concurrency).
 *
 * Plan 008: native replacement for EventBus.emit's lock/snapshot fan-out
 * (server/bus.py:110-128 -- ~4 lock acquisitions + N put_nowait per event,
 * once per streamed chunk). This bus is single-producer / many-consumer:
 *
 *   - emit(): ONE bounded push + one seq bump under a short lock -- O(1)
 *     regardless of subscriber count (no per-subscriber work).
 *   - Subscribers hold a monotonically increasing read offset; poll()
 *     copies only the events the subscriber has not seen yet.
 *   - Events are immutable once published; offsets never rewind.
 *   - Drop policy: DROP_OLDEST. When the ring is full, the oldest event is
 *     overwritten; a slow subscriber skips dropped events (its offset
 *     jumps to the oldest still-available event) and stays consistent.
 *
 * Concurrency: std::mutex with a short critical section (emit/poll touch
 * only the ring metadata + one memcpy). GIL is released around all calls
 * in the binding layer.
 */

#pragma once

#include <core/kimix_core.h>

#include <mutex>

namespace kimix {
namespace runtime {
namespace concurrency {

class KIMIX_RUNTIME_API MpscEventBus {
public:
    explicit MpscEventBus(size_t capacity);
    ~MpscEventBus();

    MpscEventBus(const MpscEventBus&) = delete;
    MpscEventBus& operator=(const MpscEventBus&) = delete;

    // Producer side: publish one event. Drops the oldest buffered event when
    // the ring is full (DROP_OLDEST policy). Never blocks.
    void emit(kimix::string_view event_bytes) noexcept;

    // Consumer side: register a subscriber. Returns a subscriber id; the new
    // subscriber starts at the current tail (only future events are seen).
    // Ids are monotonically increasing and never reused.
    uint64_t subscribe() noexcept;

    // Remove a subscriber. Subsequent poll() with this id returns false.
    void unsubscribe(uint64_t id) noexcept;

    // Copy the next unseen event for `id` into `out`. Returns false when the
    // subscriber is unknown or has no pending events. Events dropped by the
    // ring while the subscriber was slow are skipped (offset advances past
    // them); the subscriber never sees partial/stale data.
    bool poll(uint64_t id, kimix::string& out) noexcept;

    // Total number of events emitted since construction (monotonic).
    uint64_t seq() const noexcept;

    size_t capacity() const noexcept { return _events.size(); }

private:
    kimix::vector<kimix::string> _events; // ring storage (fixed size)
    size_t _count = 0;                    // events currently in the ring
    uint64_t _seq = 0;                    // events emitted since construction

    // subscriber id -> 0-based index of the next event to read
    kimix::unordered_map<uint64_t, uint64_t> _offsets;
    uint64_t _next_sub_id = 1;

    mutable std::mutex _mtx;
};

} // namespace concurrency
} // namespace runtime
} // namespace kimix
