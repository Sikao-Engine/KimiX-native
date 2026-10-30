/*
 * shared_atomic_mutex.cpp — the acquire/release loops and the spin policy (see
 * shared_atomic_mutex.h for the contract, the admission test and the guards).
 *
 * MEMORY ORDERING. The bitfield is not just a counter, it is the hand-off: a
 * reader that observes "no unique lock" must also observe every write the last
 * writer made inside its critical section. On x86 every atomic RMW is already an
 * acquire-and-release operation, so the orderings below change no instruction
 * there; on ARM/AArch64 the CAS is the only thing that emits the barrier, and a
 * relaxed success would leave the payload write unordered with the bitfield
 * store — a formally broken lock that no test on a strong-memory host shows.
 * Hence: acq_rel on every successful ACQUIRE (the acquire half pairs with the
 * previous holder's release store, the release half publishes this thread's
 * writes to the next), release on every UNLOCK, and relaxed only where the value
 * is either discarded (the failure re-read of a CAS) or purely informational
 * (num_*_locks / is_unique_locked). std::memory_order_seq_cst is deliberately
 * not used: nothing here needs a single total order on the bitfield.
 *
 * SPIN POLICY. Two stages, because a yield per iteration is a yield storm once
 * several cores wait: (1) a pure pause phase of kPausePhaseSpins spins — the
 * section being waited for is short, so this usually ends the wait; (2) after
 * that, pause every iteration but yield only every _yield_interval-th one, with
 * the interval DOUBLING up to kMaxYieldInterval. A waiter therefore stops
 * burning a core the (longer, but still bounded) holder needs, without handing
 * the OS scheduler a permanent invitation to deschedule it.
 */
#include <core/shared_atomic_mutex.h>
#include <core/intrin.h>
// std::this_thread::yield() below: <thread> is not pulled in transitively by
// the headers above on GCC/libstdc++ (MSVC happens to provide it).
#include <thread>

// assert() below: <cassert> is only reached transitively through <atomic> on some
// toolchains, so include it explicitly.
#include <cassert>

namespace kimix {

namespace src_core_shared_atomic_mutex {

// Stage 1: spins that only pause (no scheduler involvement at all).
constexpr uint32_t kPausePhaseSpins = 64;
// Stage 2: first/largest yield interval (in spins) once the pause phase is over.
constexpr uint32_t kFirstYieldInterval = 8;
constexpr uint32_t kMaxYieldInterval = 64;

// The two-stage backoff described in the file comment. One instance per acquire
// attempt (it is a few counters on the waiter's own stack, no shared state).
class backoff {
public:
    void wait() noexcept {
        KIMIX_INTRIN_PAUSE();
        if (_spins >= kPausePhaseSpins) {
            if (++_since_yield >= _yield_interval) {
                std::this_thread::yield();
                _since_yield = 0;
                if (_yield_interval < kMaxYieldInterval) {
                    _yield_interval *= 2;
                }
            }
        }
        ++_spins;
    }

private:
    uint32_t _spins{0};
    uint32_t _since_yield{0};
    uint32_t _yield_interval{kFirstYieldInterval};
};

} // namespace src_core_shared_atomic_mutex
using namespace src_core_shared_atomic_mutex;

shared_atomic_mutex::shared_atomic_mutex() noexcept
    : _bitfield(0) {
}

// Acquire the unique lock, first waiting until all shared locks are unlocked.
void shared_atomic_mutex::lock() const noexcept {
    // Increment the unique lock count (which is the number of threads waiting for a unique lock).
    // Incrementing immediately, whether actively uniquely locked or not, blocks any new thread asking for a new shared lock.
    _bitfield.fetch_add(one_unique_thread, std::memory_order_acq_rel);
    _acquire_unique();
}

void shared_atomic_mutex::unlock() const noexcept {
    assert(num_unique_locks() > 0 && is_unique_locked());
    _bitfield.fetch_sub(one_unique_flag + one_unique_thread, std::memory_order_release);
}

bool shared_atomic_mutex::try_lock() const noexcept {
    bitfield_t oldval = _bitfield.load(std::memory_order_relaxed);
    for (;;) {
        // Free only when no shared lock is active and no unique lock is held; a
        // FAILED attempt must not touch the bitfield at all — in particular it must
        // not add to the pending-writer count, which would block the readers the
        // caller is about to retry against.
        if ((oldval & num_shared_mask) != 0 || (oldval & unique_flag_mask) != 0) {
            return false;
        }
        // Take the unique lock in ONE store: count and flag together.
        bitfield_t const newval = oldval + one_unique_thread + one_unique_flag;
        if (_bitfield.compare_exchange_weak(oldval, newval, std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
            return true;
        }
    }
}

void shared_atomic_mutex::upgrade() const noexcept {
    static_assert(one_unique_thread > one_shared_thread, "This section of code assumes one_unique_thread > one_shared_thread for subtraction");
    // increment the unique lock count and remove this thread from the shared lock thread count
    _bitfield.fetch_add(one_unique_thread - one_shared_thread, std::memory_order_acq_rel);
    _acquire_unique();
}

void shared_atomic_mutex::lock_shared() const noexcept {
    // Unlike the unique lock, don't immediately increment the shared count (wait until there are no unique lock requests before adding).
    bitfield_t oldval = _bitfield.load(std::memory_order_relaxed);
    bitfield_t newval = oldval;
    backoff spins;
    for (;;) {
        // Proceed if any number of shared locks and no unique locks are waiting or active.
        any_shared_no_unique(oldval);
        // New value is expected value with a new shared lock (increment the shared lock count).
        newval = oldval + one_shared_thread;
        // If _bitfield==oldval (there are no unique locks) then store newval in _bitfield (add a shared lock).
        // Otherwise update oldval with the latest value of _bitfield and run the test loop again.
        if (_bitfield.compare_exchange_weak(oldval, newval, std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
            return;
        }
        // A failed CAS overwrote oldval with the CURRENT bitfield value; the
        // writer-preference masks above are re-applied at the top of the loop.
        spins.wait();
    }
}

bool shared_atomic_mutex::try_lock_shared() const noexcept {
    bitfield_t oldval = _bitfield.load(std::memory_order_relaxed);
    for (;;) {
        // Writer preference: a pending writer already blocks a new reader, so the
        // try-form rejects that case exactly like lock_shared() would block on it.
        if ((oldval & any_unique_mask) != 0) {
            return false;
        }
        bitfield_t const newval = oldval + one_shared_thread;
        if (_bitfield.compare_exchange_weak(oldval, newval, std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
            return true;
        }
    }
}

void shared_atomic_mutex::unlock_shared() const noexcept {
    assert(num_shared_locks() > 0);
    _bitfield.fetch_sub(one_shared_thread, std::memory_order_release);
}

shared_atomic_mutex::val_t shared_atomic_mutex::num_shared_locks() const noexcept {
    const auto mask = _bitfield.load(std::memory_order_relaxed) & num_shared_mask;
    return val_t(mask >> num_shared_bitshift);
}

shared_atomic_mutex::val_t shared_atomic_mutex::num_unique_locks() const noexcept {
    const auto mask = _bitfield.load(std::memory_order_relaxed) & num_unique_mask;
    return val_t(mask >> num_unique_bitshift);
}

bool shared_atomic_mutex::is_unique_locked() const noexcept {
    const auto mask = _bitfield.load(std::memory_order_relaxed) & unique_flag_mask;
    return (mask >> unique_flag_bitshift) != 0;
}

void shared_atomic_mutex::_acquire_unique() const noexcept {
    // Relaxed is enough for the FIRST read: the CAS below is the operation that
    // acquires the lock (and re-reads on failure anyway).
    bitfield_t oldval = _bitfield.load(std::memory_order_relaxed);
    bitfield_t newval = oldval;
    backoff spins;
    for (;;) {
        // Proceed if there are no shared locks and the unique lock is available (unique lock flag is 0).
        no_shared_no_unique(oldval);
        // Set the unique lock flag to 1.
        newval = oldval + one_unique_flag;
        // If _bitfield==oldval (there are no active shared locks and no thread has a unique lock) then store newval in _bitfield (get the unique lock).
        // Otherwise update oldval with the latest value of _bitfield and run the test loop again.
        if (_bitfield.compare_exchange_weak(oldval, newval, std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
            return;
        }
        spins.wait();
    }
}

}// namespace kimix
