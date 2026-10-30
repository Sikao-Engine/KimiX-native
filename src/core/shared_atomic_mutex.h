/*
 * shared_atomic_mutex.h — a 4-byte, writer-preferring shared/exclusive lock for
 * SHORT, BOUNDED critical sections, plus its RAII guards.
 *
 * WHY IT EXISTS. std::shared_mutex is 8 bytes of SRW-lock state plus a kernel
 * fallback: a contended acquire may park the thread, which costs microseconds
 * even when the critical section it waits for is 20 ns long. This type is one
 * atomic<uint32_t> and NEVER parks — a waiter spins (pause, then an increasing
 * yield interval), so the hand-off cost stays in the tens of nanoseconds. It is
 * therefore the right lock for a per-object section that is a single container
 * operation, and it is the measured winner there: the graph_db module converted
 * its key index, its schema registry and its per-vertex content lock to this
 * type for exactly that reason (src/graph_db/README.md, epoch 5).
 *
 * WHEN TO USE IT:
 *   - every holder's critical section is BOUNDED and SMALL in wall time
 *     (target well under ~1 us, hard ceiling a few us);
 *   - no I/O, no callback into caller code, no unbounded loop inside a section;
 *   - no allocation inside lock()/lock_shared() themselves (a small bounded
 *     allocation inside the section is acceptable, but count it);
 *   - the section never waits on a blocking resource while spinning (a nested
 *     acquisition of a LOWER level of the documented hierarchy is fine);
 *   - no try_lock/std::lock/std::scoped_lock/condition_variable_any composition
 *     (try_lock()/try_lock_shared() DO exist now, but nothing else in the std
 *     lock library is aware of this type), and no recursive unique acquisition.
 *
 * WHEN NOT TO USE IT — keep std::shared_mutex:
 *   - a long, allocating, O(n)-cascading or blocking section (a bulk load, a
 *     full-registry snapshot, a payload copy of unknown size);
 *   - a section that can call back out of the module;
 *   - a lock whose longest holder cannot be bounded by review — the lock is
 *     per-OBJECT, so one long holder spins every reader of that object too.
 *
 * SHARED RECURSION. Two lock_shared() calls on the same thread are NOT an error
 * here (they just count twice) while they are undefined behaviour on
 * std::shared_mutex, so a double-shared-lock bug can hide behind a conversion.
 * The unique lock is non-recursive, like the standard one.
 *
 * ABI NOTE. The members below are `const noexcept` (a mutex is logically
 * mutable), which changes their MANGLED NAMES relative to the pre-hardening
 * declaration. Nothing outside this tree can link against them (kimix_api's C
 * ABI exposes no mutex), but a mixed build of an old and a new object file fails
 * at link time — rebuild everything after touching this header.
 *
 * Example:
 *   kimix::shared_atomic_mutex mutex;
 *   {
 *       kimix::shared_atomic_shared_lock guard(mutex);   // any number of readers
 *       read(state);
 *   }
 *   {
 *       kimix::shared_atomic_unique_lock guard(mutex);   // one writer
 *       write(state);
 *   }
 */
#pragma once
#include <core/dll_export.h>

#include <atomic>
#include <cstdint>
#include <mutex> // std::defer_lock_t / std::adopt_lock_t / std::try_to_lock_t

namespace kimix {

struct KIMIX_CORE_API shared_atomic_mutex {
public:
    using bitfield_t = uint32_t;
    using val_t = uint16_t;

    shared_atomic_mutex() noexcept;
    shared_atomic_mutex(const shared_atomic_mutex &) = delete;
    shared_atomic_mutex(shared_atomic_mutex &&) = delete;
    shared_atomic_mutex &operator=(const shared_atomic_mutex &) = delete;
    shared_atomic_mutex &operator=(shared_atomic_mutex &&) = delete;

    // --- unique lock --------------------------------------------------------
    // Acquire the unique lock, first waiting until all shared locks are unlocked.
    // A pending (or active) unique lock blocks NEW shared acquisitions immediately
    // — the writer preference that keeps a reader-heavy workload from starving a
    // writer, and the one semantic difference from std::shared_mutex.
    void lock() const noexcept;

    // Unlock the unique lock.
    void unlock() const noexcept;

    // Unique acquire without waiting: false (and NOTHING incremented, in
    // particular not the pending-writer counter) when the lock is not free.
    [[nodiscard]] bool try_lock() const noexcept;

    // Upgrade a previously-acquired shared lock to the unique lock. Blocks new
    // shared acquisitions while it waits, exactly like lock().
    void upgrade() const noexcept;

    // --- shared lock -------------------------------------------------------
    // Acquire a shared lock, first waiting until all unique locks are unlocked.
    void lock_shared() const noexcept;

    // Unlock a shared lock.
    void unlock_shared() const noexcept;

    // Shared acquire without waiting: false when a unique lock is held or a
    // writer is already waiting (the same writer preference as lock_shared()).
    [[nodiscard]] bool try_lock_shared() const noexcept;

    // --- introspection (for tests, asserts and diagnostics) -----------------
    // Returns the number of threads with an active shared lock.
    [[nodiscard]] val_t num_shared_locks() const noexcept;

    // Returns the number of threads waiting for a unique lock, one of which may have an active unique lock.
    [[nodiscard]] val_t num_unique_locks() const noexcept;

    // Returns whether the mutex is currently uniquely locked.
    [[nodiscard]] bool is_unique_locked() const noexcept;

private:
    void _acquire_unique() const noexcept;

    static const uint8_t num_shared_bitshift = 0;
    static const uint8_t num_unique_bitshift = 16;
    static const uint8_t unique_flag_bitshift = 31;

    static const bitfield_t num_shared_mask = bitfield_t(65535) << num_shared_bitshift;
    static const bitfield_t num_unique_mask = bitfield_t(32767) << num_unique_bitshift;
    static const bitfield_t unique_flag_mask = bitfield_t(1) << unique_flag_bitshift;
    // "a unique lock is held or a writer is waiting" — the test lock_shared() and
    // try_lock_shared() reject on.
    static const bitfield_t any_unique_mask = num_unique_mask | unique_flag_mask;

    static const bitfield_t one_shared_thread = bitfield_t(1) << num_shared_bitshift;
    static const bitfield_t one_unique_thread = bitfield_t(1) << num_unique_bitshift;
    static const bitfield_t one_unique_flag = bitfield_t(1) << unique_flag_bitshift;

    static void any_shared_no_unique(bitfield_t &bits) { bits &= num_shared_mask; }
    static void no_shared_no_unique(bitfield_t &bits) { bits &= num_unique_mask; }

    // logically mutable: every operation is a const member (see the ABI note).
    mutable std::atomic<bitfield_t> _bitfield;// consists of [ 1 bit unique lock flag, 15 bits # of unique locks, 16 bits # of shared locks]
};

/*
 * RAII guard for the UNIQUE lock — the std::unique_lock-shaped adapter the mutex
 * needs, since std::unique_lock itself is not specified for a non-standard
 * Lockable. Default-constructible and movable (with a null "owns nothing" state)
 * so a fixed-size array of guards works; NOT copyable.
 *
 * std::lock_guard<kimix::shared_atomic_mutex> also works (it only needs
 * lock()/unlock(), the BasicLockable requirement); std::shared_lock and
 * std::scoped_lock are NOT to be used with this type.
 */
class [[nodiscard]] shared_atomic_unique_lock {
public:
    shared_atomic_unique_lock() noexcept = default;
    explicit shared_atomic_unique_lock(shared_atomic_mutex &mutex) noexcept
        : _mutex(&mutex), _owns(true) {
        _mutex->lock();
    }
    // The std lock-construction tags, so `guard(m, std::defer_lock)` reads the same
    // as its std::unique_lock counterpart: defer_lock REMEMBERS the mutex without
    // owning it, adopt_lock takes over a lock the caller already holds, try_to_lock
    // owns it only when the non-blocking acquire succeeded.
    shared_atomic_unique_lock(shared_atomic_mutex &mutex, std::defer_lock_t) noexcept
        : _mutex(&mutex) {}
    shared_atomic_unique_lock(shared_atomic_mutex &mutex, std::adopt_lock_t) noexcept
        : _mutex(&mutex), _owns(true) {}
    shared_atomic_unique_lock(shared_atomic_mutex &mutex, std::try_to_lock_t) noexcept
        : _mutex(&mutex), _owns(mutex.try_lock()) {}
    shared_atomic_unique_lock(shared_atomic_unique_lock const &) = delete;
    shared_atomic_unique_lock &operator=(shared_atomic_unique_lock const &) = delete;
    shared_atomic_unique_lock(shared_atomic_unique_lock &&other) noexcept
        : _mutex(other._mutex), _owns(other._owns) {
        other._mutex = nullptr;
        other._owns = false;
    }
    shared_atomic_unique_lock &operator=(shared_atomic_unique_lock &&other) noexcept {
        if (this != &other) {
            unlock();
            _mutex = other._mutex;
            _owns = other._owns;
            other._mutex = nullptr;
            other._owns = false;
        }
        return *this;
    }
    ~shared_atomic_unique_lock() noexcept { unlock(); }

    [[nodiscard]] bool owns_lock() const noexcept { return _owns; }
    explicit operator bool() const noexcept { return _owns; }
    // Release the lock early (idempotent; a no-op on a guard that owns nothing).
    void unlock() noexcept {
        if (_owns) {
            _mutex->unlock();
            _owns = false;
        }
    }
    void swap(shared_atomic_unique_lock &other) noexcept {
        shared_atomic_mutex *const mine = _mutex;
        const bool owned_by_me = _owns;
        _mutex = other._mutex;
        _owns = other._owns;
        other._mutex = mine;
        other._owns = owned_by_me;
    }
    // The mutex this guard refers to (nullptr on a default-constructed guard), even
    // after release() — mirroring std::unique_lock.
    [[nodiscard]] shared_atomic_mutex *mutex() const noexcept { return _mutex; }
    // Disown WITHOUT unlocking — the caller becomes the unlocker. Returns the
    // mutex (nullptr on a default-constructed guard).
    [[nodiscard]] shared_atomic_mutex *release() noexcept {
        _owns = false;
        return _mutex;
    }

private:
    shared_atomic_mutex *_mutex{nullptr};
    bool _owns{false};
};

// RAII guard for the SHARED lock (the std::shared_lock counterpart).
class [[nodiscard]] shared_atomic_shared_lock {
public:
    shared_atomic_shared_lock() noexcept = default;
    explicit shared_atomic_shared_lock(shared_atomic_mutex &mutex) noexcept
        : _mutex(&mutex), _owns(true) {
        _mutex->lock_shared();
    }
    shared_atomic_shared_lock(shared_atomic_mutex &mutex, std::defer_lock_t) noexcept
        : _mutex(&mutex) {}
    shared_atomic_shared_lock(shared_atomic_mutex &mutex, std::adopt_lock_t) noexcept
        : _mutex(&mutex), _owns(true) {}
    shared_atomic_shared_lock(shared_atomic_mutex &mutex, std::try_to_lock_t) noexcept
        : _mutex(&mutex), _owns(mutex.try_lock_shared()) {}
    shared_atomic_shared_lock(shared_atomic_shared_lock const &) = delete;
    shared_atomic_shared_lock &operator=(shared_atomic_shared_lock const &) = delete;
    shared_atomic_shared_lock(shared_atomic_shared_lock &&other) noexcept
        : _mutex(other._mutex), _owns(other._owns) {
        other._mutex = nullptr;
        other._owns = false;
    }
    shared_atomic_shared_lock &operator=(shared_atomic_shared_lock &&other) noexcept {
        if (this != &other) {
            unlock();
            _mutex = other._mutex;
            _owns = other._owns;
            other._mutex = nullptr;
            other._owns = false;
        }
        return *this;
    }
    ~shared_atomic_shared_lock() noexcept { unlock(); }

    [[nodiscard]] bool owns_lock() const noexcept { return _owns; }
    explicit operator bool() const noexcept { return _owns; }
    void unlock() noexcept {
        if (_owns) {
            _mutex->unlock_shared();
            _owns = false;
        }
    }
    void swap(shared_atomic_shared_lock &other) noexcept {
        shared_atomic_mutex *const mine = _mutex;
        const bool owned_by_me = _owns;
        _mutex = other._mutex;
        _owns = other._owns;
        other._mutex = mine;
        other._owns = owned_by_me;
    }
    [[nodiscard]] shared_atomic_mutex *mutex() const noexcept { return _mutex; }
    [[nodiscard]] shared_atomic_mutex *release() noexcept {
        _owns = false;
        return _mutex;
    }

private:
    shared_atomic_mutex *_mutex{nullptr};
    bool _owns{false};
};

/*
 * RAII guard for the UPGRADE path: the caller ALREADY holds the shared lock on
 * this thread, and the guard turns it into the unique lock (and then unlocks it).
 * It is not default-constructible on purpose — there is no meaningful null state
 * for "a shared lock that became unique".
 */
class [[nodiscard]] shared_atomic_upgrade_lock {
public:
    explicit shared_atomic_upgrade_lock(shared_atomic_mutex &mutex) noexcept
        : _mutex(mutex) {
        _mutex.upgrade();
    }
    shared_atomic_upgrade_lock(shared_atomic_upgrade_lock const &) = delete;
    shared_atomic_upgrade_lock &operator=(shared_atomic_upgrade_lock const &) = delete;
    shared_atomic_upgrade_lock(shared_atomic_upgrade_lock &&) = delete;
    shared_atomic_upgrade_lock &operator=(shared_atomic_upgrade_lock &&) = delete;
    ~shared_atomic_upgrade_lock() noexcept { _mutex.unlock(); }

    // The guarded mutex (every mutex operation is const, so a const pointer is
    // enough here).
    [[nodiscard]] shared_atomic_mutex const *mutex() const noexcept { return &_mutex; }

private:
    shared_atomic_mutex &_mutex;
};

}// namespace kimix
