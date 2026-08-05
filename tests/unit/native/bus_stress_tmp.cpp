// TEMPORARY stress reproduction - removed after debugging.
#include <runtime/concurrency/event_bus.h>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

using namespace kimix::runtime::concurrency;

int main() {
    for (int trial = 0; trial < 8; ++trial) {
        MpscEventBus bus(20000);
        kimix::vector<uint64_t> subs;
        for (int i = 0; i < 4; ++i) subs.push_back(bus.subscribe());
        std::atomic<bool> done{false};
        std::thread producer([&] {
            for (int i = 0; i < 10000; ++i) bus.emit("evt-" + std::to_string(i));
            done.store(true);
        });
        std::atomic<int> min_count{100000};
        std::vector<std::thread> cs;
        for (int c = 0; c < 4; ++c) {
            cs.emplace_back([&, c] {
                kimix::string out;
                int count = 0;
                while (true) {
                    if (bus.poll(subs[(size_t)c], out)) {
                        ++count;
                    } else if (done.load()) {
                        if (bus.poll(subs[(size_t)c], out)) { continue; }
                        break;
                    }
                }
                int cur = min_count.load();
                while (count < cur && !min_count.compare_exchange_weak(cur, count)) {}
            });
        }
        producer.join();
        for (auto& t : cs) t.join();
        printf("trial %d: min_count=%d seq=%llu\n", trial, min_count.load(),
               (unsigned long long)bus.seq());
    }
    return 0;
}
