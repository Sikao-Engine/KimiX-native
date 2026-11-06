// Test for src/runtime/codec/event_ring.{h,cpp} — the bounded wire-frame ring
// (report candidate A4).
//
// Covers capacity enforcement (drop whole oldest frames), the oversized-frame
// rejection, the dropped counter, the pop_batch framing round trip (u32 count |
// (u32 len | frame)*), batch limits and byte accounting.

#include "ut/ut.hpp"

#include <runtime/codec/event_ring.h>
#include <runtime/common/le_bytes.h>

using namespace boost::ut;
using namespace boost::ut::literals;
using kimix::runtime::codec::EventRing;

namespace tests_unit_native_test_event_ring {

kimix::vector<kimix::string> split_batch(kimix::string_view packed) {
    kimix::vector<kimix::string> frames;
    if (packed.size() < 4) {
        return frames;
    }
    const uint32_t count = kimix::runtime::common::read_le32(packed.data());
    size_t offset = 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (offset + 4 > packed.size()) {
            break;
        }
        const uint32_t len = kimix::runtime::common::read_le32(packed.data() + offset);
        offset += 4;
        frames.push_back(kimix::string(packed.data() + offset, len));
        offset += len;
    }
    return frames;
}

} // namespace tests_unit_native_test_event_ring
using namespace tests_unit_native_test_event_ring;

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "event_ring_push_pop"_test = [] {
        EventRing ring(1024);
        expect(ring.size() == 0u);
        expect(ring.push("frame-a"));
        expect(ring.push("frame-b"));
        expect(ring.size() == 2u);
        expect(ring.pending_bytes() == 14u);

        kimix::string batch;
        ring.pop_batch(10, batch);
        kimix::vector<kimix::string> frames = split_batch(batch);
        expect(frames.size() == 2u);
        expect(frames[0] == "frame-a");
        expect(frames[1] == "frame-b");
        expect(ring.size() == 0u);
        expect(ring.pending_bytes() == 0u);

        // An empty ring yields a well-formed empty batch.
        ring.pop_batch(10, batch);
        expect(batch.size() == 4u);
        expect(split_batch(batch).empty());
    };

    "event_ring_batch_limit"_test = [] {
        EventRing ring(1024);
        for (int i = 0; i < 5; ++i) {
            ring.push(kimix::format("f{}", i));
        }
        kimix::string batch;
        ring.pop_batch(2, batch);
        expect(split_batch(batch).size() == 2u);
        expect(ring.size() == 3u);
        ring.pop_batch(100, batch);
        kimix::vector<kimix::string> rest = split_batch(batch);
        expect(rest.size() == 3u);
        expect(rest[0] == "f2");
        expect(rest[2] == "f4");
    };

    "event_ring_drops_oldest"_test = [] {
        EventRing ring(20);
        expect(ring.push("0123456789"));    // 10 bytes
        expect(ring.push("abcdefghij"));    // 10 bytes -> full
        expect(ring.push("KLMNOPQRST"));    // drops the first frame
        expect(ring.size() == 2u);
        expect(ring.dropped() == 1u);
        expect(ring.pending_bytes() == 20u);
        kimix::string batch;
        ring.pop_batch(10, batch);
        kimix::vector<kimix::string> frames = split_batch(batch);
        expect(frames.size() == 2u);
        expect(frames[0] == "abcdefghij");
        expect(frames[1] == "KLMNOPQRST");
    };

    "event_ring_oversize_rejected"_test = [] {
        EventRing ring(8);
        expect(!ring.push("0123456789")); // bigger than the whole ring
        expect(ring.size() == 0u);
        expect(ring.dropped() == 0u);
        expect(!ring.push(kimix::string(9, 'x')));
        expect(ring.push("01234567")); // exactly the capacity
        expect(ring.size() == 1u);
    };

    "event_ring_accounting"_test = [] {
        EventRing ring(4096);
        for (int i = 0; i < 100; ++i) {
            ring.push(kimix::format("frame-{}", i));
        }
        const uint64_t pending = ring.pending_bytes();
        expect(pending > 0u);
        expect(pending <= ring.capacity_bytes());
        expect(ring.bytes_owned() >= pending);
        ring.clear();
        expect(ring.size() == 0u);
        expect(ring.pending_bytes() == 0u);
        expect(ring.capacity_bytes() == 4096u);
    };

    "event_ring_empty_frame"_test = [] {
        EventRing ring(64);
        expect(ring.push(""));
        expect(ring.size() == 1u);
        kimix::string batch;
        ring.pop_batch(4, batch);
        kimix::vector<kimix::string> frames = split_batch(batch);
        expect(frames.size() == 1u);
        expect(frames[0].empty());
        expect(ring.pending_bytes() == 0u);
    };

    return 0;
}
