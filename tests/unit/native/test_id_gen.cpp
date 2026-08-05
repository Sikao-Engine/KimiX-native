// Test for src/runtime/concurrency/id_gen.h (plan 008).
// This test covers:
// - monotonicity from a seed
// - reserve() contiguity (n consecutive ids)
// - uniqueness under 8 threads (next() and reserve())
// - disjointness of concurrent reserves (each RMW gets a disjoint range)

#include "ut/ut.hpp"
#include <runtime/concurrency/id_gen.h>

#include <atomic>
#include <thread>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::runtime::concurrency;

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "idgen_seed_and_monotonic"_test = [] {
        IdGenerator gen(100);
        expect(eq(gen.next(), 100u));
        expect(eq(gen.next(), 101u));
        expect(eq(gen.next(), 102u));
        IdGenerator zero;
        expect(eq(zero.next(), 0u));
    };

    "idgen_reserve_contiguity"_test = [] {
        IdGenerator gen(10);
        kimix::vector<uint64_t> ids;
        gen.reserve(5, ids);
        expect(eq(ids.size(), 5u));
        for (size_t i = 0; i < ids.size(); ++i) {
            expect(eq(ids[i], 10u + i));
        }
        // The next() continues after the reserved range.
        expect(eq(gen.next(), 15u));
        // reserve(0) is a no-op.
        gen.reserve(0, ids);
        expect(ids.empty());
    };

    "idgen_unique_under_8_threads_next"_test = [] {
        constexpr int kThreads = 8;
        constexpr int kPerThread = 5000;
        IdGenerator gen(0);
        kimix::vector<std::thread> threads;
        kimix::vector<kimix::vector<uint64_t>> per_thread(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                per_thread[static_cast<size_t>(t)].reserve(kPerThread);
                for (int i = 0; i < kPerThread; ++i) {
                    per_thread[static_cast<size_t>(t)].push_back(gen.next());
                }
            });
        }
        for (auto& th : threads) {
            th.join();
        }
        // Every id in [0, kThreads*kPerThread) appears exactly once.
        kimix::vector<bool> seen(static_cast<size_t>(kThreads) * kPerThread, false);
        bool unique = true;
        for (const auto& v : per_thread) {
            for (uint64_t id : v) {
                if (id >= seen.size() || seen[static_cast<size_t>(id)]) {
                    unique = false;
                } else {
                    seen[static_cast<size_t>(id)] = true;
                }
            }
        }
        expect(unique) << "all ids must be unique across threads";
        for (bool s : seen) {
            expect(s) << "every id in the range must be handed out";
        }
    };

    "idgen_concurrent_reserves_disjoint"_test = [] {
        constexpr int kThreads = 8;
        constexpr uint64_t kChunk = 1000;
        IdGenerator gen(0);
        std::atomic<bool> overlap{false};
        std::atomic<uint64_t> count{0};
        kimix::vector<std::thread> threads;
        // Each thread reserves kChunk ids and marks them in a shared bitmap
        // under a lock (the point is the RANGES must be disjoint).
        std::mutex mtx;
        kimix::vector<bool> seen(static_cast<size_t>(kThreads) * kChunk, false);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&] {
                kimix::vector<uint64_t> ids;
                gen.reserve(kChunk, ids);
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    for (uint64_t id : ids) {
                        if (id >= seen.size() || seen[static_cast<size_t>(id)]) {
                            overlap.store(true);
                        } else {
                            seen[static_cast<size_t>(id)] = true;
                        }
                    }
                }
                count.fetch_add(kChunk);
            });
        }
        for (auto& th : threads) {
            th.join();
        }
        expect(!overlap.load()) << "concurrent reserves must be disjoint";
        expect(eq(count.load(), static_cast<uint64_t>(kThreads) * kChunk));
        expect(eq(gen.next(), static_cast<uint64_t>(kThreads) * kChunk));
    };

    return 0;
}
