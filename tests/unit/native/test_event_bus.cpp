// Test for src/runtime/concurrency/event_bus.h (plan 008).
// This test covers:
// - basic emit/poll ordering, seq monotonicity
// - ring wrap-around correctness
// - DROP_OLDEST on the shared ring: when full, the oldest event is
//   overwritten; any subscriber (fast or slow) whose offset falls behind
//   skips the dropped events and stays consistent
// - subscriber added mid-stream starts at the current offset (tail)
// - unsubscribe; poll after unsubscribe is false; ids never reused
// - thread test: producer thread + 4 consumer threads, no lost/dup events
//   (capacity >= event count, so no drop can occur), per-subscriber
//   ordering preserved

#include "ut/ut.hpp"
#include <runtime/concurrency/event_bus.h>

#include <atomic>
#include <thread>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::concurrency;

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "bus_basic_order_and_seq"_test = [] {
        MpscEventBus bus(64);
        const uint64_t sub = bus.subscribe();
        bus.emit("e1");
        bus.emit("e2");
        bus.emit("e3");
        expect(eq(bus.seq(), 3u));
        kimix::string out;
        expect(bus.poll(sub, out));
        expect(eq(out, kimix::string("e1")));
        expect(bus.poll(sub, out));
        expect(eq(out, kimix::string("e2")));
        expect(bus.poll(sub, out));
        expect(eq(out, kimix::string("e3")));
        expect(!bus.poll(sub, out)) << "caught up";
    };

    "bus_ring_wraparound"_test = [] {
        MpscEventBus bus(4);
        const uint64_t sub = bus.subscribe();
        kimix::string out;
        for (int i = 0; i < 100; ++i) {
            bus.emit("evt" + std::to_string(i));
        }
        expect(eq(bus.seq(), 100u));
        // Ring capacity 4: only the last 4 events survive; the subscriber
        // skips the dropped 96 and reads the last 4 in order.
        int count = 0;
        int last = -1;
        while (bus.poll(sub, out)) {
            const int n = std::stoi(std::string(out.data() + 3, out.size() - 3));
            expect(gt(n, last)) << "order preserved";
            last = n;
            ++count;
        }
        expect(eq(count, 4));
        expect(eq(last, 99));
    };

    "bus_drop_oldest_shared_ring"_test = [] {
        MpscEventBus bus(2);
        const uint64_t sub = bus.subscribe();
        bus.emit("a");
        bus.emit("b");
        bus.emit("c"); // ring full -> overwrites "a"
        bus.emit("d"); // ring full -> overwrites "b"
        kimix::string out;
        // DROP_OLDEST applies to the shared ring: only c,d survive.
        expect(bus.poll(sub, out));
        expect(eq(out, kimix::string("c")));
        expect(bus.poll(sub, out));
        expect(eq(out, kimix::string("d")));
        expect(!bus.poll(sub, out));
    };

    "bus_subscribe_mid_stream_starts_at_tail"_test = [] {
        MpscEventBus bus(8);
        bus.emit("old1");
        bus.emit("old2");
        const uint64_t sub = bus.subscribe(); // must NOT see old events
        bus.emit("new1");
        kimix::string out;
        expect(bus.poll(sub, out));
        expect(eq(out, kimix::string("new1")));
        expect(!bus.poll(sub, out));
    };

    "bus_unsubscribe_unknown_id"_test = [] {
        MpscEventBus bus(8);
        const uint64_t sub = bus.subscribe();
        bus.emit("x");
        bus.unsubscribe(sub);
        kimix::string out;
        expect(!bus.poll(sub, out)) << "unsubscribed id never polls";
        // Unknown ids are rejected too.
        expect(!bus.poll(9999, out));
        // Subscribe after unsubscribe gets a NEW id (never reused).
        const uint64_t sub2 = bus.subscribe();
        expect(neq(sub, sub2));
        bus.emit("y");
        expect(bus.poll(sub2, out));
    };

    "bus_seq_monotonic_many_subs"_test = [] {
        MpscEventBus bus(16);
        kimix::vector<uint64_t> subs;
        for (int i = 0; i < 20; ++i) {
            subs.push_back(bus.subscribe());
        }
        for (int i = 0; i < 50; ++i) {
            bus.emit("m" + std::to_string(i));
        }
        expect(eq(bus.seq(), 50u));
        // Every subscriber sees exactly the last 16 events in order.
        kimix::string out;
        for (uint64_t s : subs) {
            int count = 0;
            int last = -1;
            while (bus.poll(s, out)) {
                const int n = std::stoi(std::string(out.data() + 1, out.size() - 1));
                expect(gt(n, last));
                last = n;
                ++count;
            }
            expect(eq(count, 16));
            expect(eq(last, 49));
        }
    };

    "bus_thread_producer_4_consumers"_test = [] {
        // Producer thread emits 20k events; 4 consumer threads each poll
        // their own subscriber. Ring capacity >= event count, so no drop can
        // ever occur: no event is lost, no event is duplicated, and
        // per-subscriber order is strictly increasing.
        constexpr size_t kCapacity = 20000;
        constexpr int kEvents = 20000;
        constexpr int kConsumers = 4;

        MpscEventBus bus(kCapacity);
        kimix::vector<uint64_t> subs;
        for (int i = 0; i < kConsumers; ++i) {
            subs.push_back(bus.subscribe());
        }

        std::atomic<bool> done{false};
        std::thread producer([&] {
            for (int i = 0; i < kEvents; ++i) {
                bus.emit("evt" + std::to_string(i));
            }
            done.store(true);
        });

        kimix::vector<std::thread> consumers;
        std::atomic<bool> all_ok{true};
        for (int c = 0; c < kConsumers; ++c) {
            consumers.emplace_back([&, c] {
                kimix::string out;
                int count = 0;
                while (true) {
                    if (bus.poll(subs[static_cast<size_t>(c)], out)) {
                        const int n = std::stoi(std::string(out.data() + 3, out.size() - 3));
                        if (n != count) {
                            all_ok.store(false); // lost or duplicated
                        }
                        ++count;
                    } else if (done.load()) {
                        // Producer finished and we are caught up.
                        if (bus.poll(subs[static_cast<size_t>(c)], out)) {
                            continue; // last chance
                        }
                        break;
                    } else {
                        std::this_thread::yield();
                    }
                }
                if (count != kEvents) {
                    all_ok.store(false); // drop policy off -> all 20k
                }
            });
        }
        producer.join();
        for (auto& t : consumers) {
            t.join();
        }
        expect(all_ok.load()) << "no lost/dup events, ordering preserved";
        expect(eq(bus.seq(), static_cast<uint64_t>(kEvents)));
    };

    return 0;
}
