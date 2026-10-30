// Test for shared_atomic_mutex.h (kimix::shared_atomic_mutex + its RAII guards).
// This test covers:
// - exclusive mutual exclusion (a 4-thread counter stress, exact final value)
// - shared mutual INCLUSION: N readers hold the lock at the same time
// - writer preference: a pending lock() blocks new lock_shared()/try_lock_shared()
// - the non-blocking forms: try_lock()/try_lock_shared() and their refusal paths
// - shared recursion (lock_shared twice counts twice) and the unique-then-shared
//   case that is a deadlock on a non-recursive unique lock
// - upgrade(): shared -> unique, and the bitfield it leaves behind
// - the RAII guards (unique / shared / upgrade): owns_lock, unlock, release, move,
//   the std lock tags, and default construction inside std::array
// - std::lock_guard<kimix::shared_atomic_mutex> (the BasicLockable contract)
//
// Everything runs in main() scope per the project's Boost.UT convention.

#include "ut/ut.hpp"

#include <core/shared_atomic_mutex.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "fresh_mutex_is_unlocked"_test = [] {
        kimix::shared_atomic_mutex mutex;
        expect(!mutex.is_unique_locked());
        expect(eq(mutex.num_shared_locks(), 0)) << "a fresh mutex has no shared holders";
        expect(eq(mutex.num_unique_locks(), 0)) << "a fresh mutex has no pending writer";
    };

    "exclusive_mutual_exclusion"_test = [] {
        kimix::shared_atomic_mutex mutex;
        int64_t counter = 0;
        int64_t unlocked_counter = 0; // guarded by nothing: proves the lock was taken
        constexpr int kThreads = 4;
        constexpr int kIterations = 50000;
        std::array<std::thread, kThreads> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads[static_cast<size_t>(t)] = std::thread([&mutex, &counter, &unlocked_counter] {
                for (int i = 0; i < kIterations; ++i) {
                    mutex.lock();
                    ++counter;              // protected
                    ++unlocked_counter;     // same section, same guarantee
                    mutex.unlock();
                }
            });
        }
        for (auto &thread : threads) {
            thread.join();
        }
        expect(eq(counter, static_cast<int64_t>(kThreads * kIterations)))
            << "every increment under the unique lock must be counted";
        expect(eq(unlocked_counter, counter));
        expect(!mutex.is_unique_locked());
    };

    "shared_readers_are_mutually_inclusive"_test = [] {
        kimix::shared_atomic_mutex mutex;
        std::atomic<int> observed_max{0};
        constexpr int kReaders = 4;
        std::array<std::thread, kReaders> threads;
        for (int t = 0; t < kReaders; ++t) {
            threads[static_cast<size_t>(t)] = std::thread([&mutex, &observed_max] {
                kimix::shared_atomic_shared_lock guard(mutex);
                int const here = mutex.num_shared_locks();
                int previous = observed_max.load();
                while (here > previous && !observed_max.compare_exchange_weak(previous, here)) {
                }
                // Hold the shared lock long enough for the others to join in: a lock
                // that excluded readers could never reach more than 1 here.
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            });
        }
        for (auto &thread : threads) {
            thread.join();
        }
        expect(eq(observed_max.load(), kReaders))
            << "all readers must be inside the shared section at the same time";
        expect(!mutex.is_unique_locked());
        expect(eq(mutex.num_shared_locks(), 0)) << "every shared lock must be released";
    };

    "writer_preference_blocks_new_readers"_test = [] {
        kimix::shared_atomic_mutex mutex;
        mutex.lock_shared();
        std::atomic<bool> acquired{false};
        std::thread writer([&mutex, &acquired] {
            mutex.lock();
            acquired.store(true, std::memory_order_release);
            mutex.unlock();
        });
        // lock() increments the pending-writer count BEFORE it spins, so this poll
        // is the writer's registration, not a timing guess.
        while (mutex.num_unique_locks() == 0) {
            std::this_thread::yield();
        }
        expect(!acquired.load()) << "the writer must wait for the shared holder";
        expect(!mutex.try_lock_shared()) << "writer preference: a pending writer blocks a NEW reader";
        mutex.unlock_shared();
        writer.join();
        expect(acquired.load()) << "the writer must run once the reader released";
        expect(!mutex.is_unique_locked());
        expect(mutex.try_lock_shared()) << "with no writer waiting a reader is admitted again";
        mutex.unlock_shared();
    };

    "try_lock_refuses_instead_of_waiting"_test = [] {
        kimix::shared_atomic_mutex mutex;
        expect(mutex.try_lock());
        expect(mutex.is_unique_locked());
        expect(eq(mutex.num_unique_locks(), 1)) << "a held unique lock is one pending/active writer";
        expect(!mutex.try_lock()) << "the unique lock is not recursive";
        expect(!mutex.try_lock_shared()) << "unique then shared would deadlock, so the try-form refuses";
        mutex.unlock();
        expect(!mutex.is_unique_locked());
        expect(eq(mutex.num_unique_locks(), 0));
        expect(mutex.try_lock_shared());
        expect(mutex.try_lock_shared()) << "shared acquisition is countable, so twice is legal";
        expect(eq(mutex.num_shared_locks(), 2));
        expect(!mutex.try_lock()) << "shared holders exclude the unique lock";
        mutex.unlock_shared();
        mutex.unlock_shared();
        expect(eq(mutex.num_shared_locks(), 0));
        expect(mutex.try_lock()) << "a fully released mutex is free again";
        mutex.unlock();
    };

    "upgrade_turns_a_shared_lock_into_the_unique_lock"_test = [] {
        kimix::shared_atomic_mutex mutex;
        mutex.lock_shared();
        expect(eq(mutex.num_shared_locks(), 1));
        mutex.upgrade();
        expect(mutex.is_unique_locked());
        expect(eq(mutex.num_shared_locks(), 0)) << "the upgrader left the shared count";
        expect(eq(mutex.num_unique_locks(), 1));
        expect(!mutex.try_lock_shared()) << "the upgraded lock is a real unique lock";
        mutex.unlock();
        expect(!mutex.is_unique_locked());
        expect(eq(mutex.num_unique_locks(), 0));
    };

    "unique_guard_owns_unlocks_and_moves"_test = [] {
        kimix::shared_atomic_mutex mutex;
        {
            kimix::shared_atomic_unique_lock guard; // default: owns nothing
            expect(!guard.owns_lock());
            expect(!static_cast<bool>(guard));
            guard.unlock(); // a no-op, not a crash
            expect(guard.mutex() == nullptr);

            kimix::shared_atomic_unique_lock owned(mutex);
            expect(owned.owns_lock());
            expect(mutex.is_unique_locked());

            kimix::shared_atomic_unique_lock moved(std::move(owned));
            expect(moved.owns_lock());
            expect(!owned.owns_lock()) << "the moved-from guard must own nothing";
            expect(mutex.is_unique_locked()) << "a move must not release the lock";

            guard = std::move(moved);
            expect(guard.owns_lock());
            expect(!moved.owns_lock());
            expect(mutex.is_unique_locked());

            guard.unlock();
            expect(!guard.owns_lock());
            expect(!mutex.is_unique_locked());
        }
        expect(!mutex.is_unique_locked());
    };

    "shared_guard_tags_and_release"_test = [] {
        kimix::shared_atomic_mutex mutex;
        {
            kimix::shared_atomic_shared_lock reader(mutex);
            expect(reader.owns_lock());
            expect(eq(mutex.num_shared_locks(), 1));
            // try_to_lock against a mutex the reader already holds shared: refused,
            // owns nothing.
            kimix::shared_atomic_unique_lock refused(mutex, std::try_to_lock);
            expect(!refused.owns_lock());
            // defer_lock builds the guard WITHOUT touching the bitfield.
            kimix::shared_atomic_unique_lock deferred(mutex, std::defer_lock);
            expect(!deferred.owns_lock());
            expect(eq(mutex.num_shared_locks(), 1)) << "defer_lock must not change the shared count";
            expect(eq(mutex.num_unique_locks(), 0)) << "defer_lock must not register a pending writer";
            // release() disowns without unlocking.
            kimix::shared_atomic_mutex *const raw = reader.release();
            expect(raw == &mutex);
            expect(!reader.owns_lock());
            expect(eq(mutex.num_shared_locks(), 1)) << "release() must NOT unlock";
            mutex.unlock_shared();
        }
        expect(eq(mutex.num_shared_locks(), 0));
    };

    "upgrade_guard_unlocks_the_unique_lock"_test = [] {
        kimix::shared_atomic_mutex mutex;
        mutex.lock_shared();
        {
            kimix::shared_atomic_upgrade_lock guard(mutex);
            expect(mutex.is_unique_locked());
            expect(guard.mutex() == &mutex);
        }
        expect(!mutex.is_unique_locked());
        expect(eq(mutex.num_unique_locks(), 0));
        expect(mutex.try_lock_shared()) << "the upgrade guard left the mutex unlocked";
        mutex.unlock_shared();
    };

    "guards_work_inside_a_fixed_array"_test = [] {
        // The reason the guards are default-constructible and movable: a batch that
        // touches N objects needs std::array<guard, N>.
        kimix::shared_atomic_mutex mutexes[4];
        std::array<kimix::shared_atomic_shared_lock, 4> guards;
        for (size_t i = 0; i < guards.size(); ++i) {
            guards[i] = kimix::shared_atomic_shared_lock(mutexes[i]);
        }
        for (size_t i = 0; i < guards.size(); ++i) {
            expect(guards[i].owns_lock());
            expect(eq(mutexes[i].num_shared_locks(), 1));
        }
    };

    "std_lock_guard_composes_with_the_mutex"_test = [] {
        kimix::shared_atomic_mutex mutex;
        {
            std::lock_guard<kimix::shared_atomic_mutex> guard(mutex);
            expect(mutex.is_unique_locked());
        }
        expect(!mutex.is_unique_locked());
        expect(eq(mutex.num_unique_locks(), 0));
    };
}
