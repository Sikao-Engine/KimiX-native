#pragma once

#include <type_traits>
#include <utility>
#include <mutex>

namespace kimix {

// ---------------------------------------------------------------------------
// conditional_mutex_t — real mutex when ThreadSafe=true, no-op otherwise
// ---------------------------------------------------------------------------

template <bool ThreadSafe, typename Mutex>
class conditional_mutex_t {
public:
    void lock() {
        if constexpr (ThreadSafe) { _mutex.lock(); }
    }
    void unlock() {
        if constexpr (ThreadSafe) { _mutex.unlock(); }
    }
    bool try_lock() {
        if constexpr (ThreadSafe) { return _mutex.try_lock(); }
        return true;
    }

    Mutex& mutex() noexcept { return _mutex; }
    const Mutex& mutex() const noexcept { return _mutex; }

private:
    Mutex _mutex;
};

// Specialization for ThreadSafe = false — all no-ops
template <typename Mutex>
class conditional_mutex_t<false, Mutex> {
public:
    void lock() {}
    void unlock() {}
    bool try_lock() { return true; }

    Mutex& mutex() noexcept { return _dummy; }
    const Mutex& mutex() const noexcept { return _dummy; }

private:
    static inline Mutex _dummy{};
};

// ---------------------------------------------------------------------------
// thread_safety<Mutex> — CRTP mixin providing with_lock(f) pattern
// ---------------------------------------------------------------------------

template <typename Mutex>
class thread_safety {
public:
    template <typename F>
    decltype(auto) with_lock(F&& f) {
        std::lock_guard<Mutex> lock(_mutex);
        return f();
    }

    template <typename F>
    decltype(auto) with_lock(F&& f) const {
        std::lock_guard<Mutex> lock(_mutex);
        return f();
    }

    Mutex& mutex() noexcept { return _mutex; }
    const Mutex& mutex() const noexcept { return _mutex; }

private:
    mutable Mutex _mutex;
};

} // namespace kimix
