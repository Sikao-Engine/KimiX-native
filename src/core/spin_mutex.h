#pragma once

#include <atomic>

namespace kimix {

// ---------------------------------------------------------------------------
// spin_mutex — simple spinlock using std::atomic_flag
// ---------------------------------------------------------------------------

class spin_mutex {
public:
    spin_mutex() noexcept { _flag.clear(); }

    spin_mutex(const spin_mutex&) = delete;
    spin_mutex& operator=(const spin_mutex&) = delete;
    spin_mutex(spin_mutex&&) = delete;
    spin_mutex& operator=(spin_mutex&&) = delete;

    void lock() noexcept {
        while (_flag.test_and_set(std::memory_order_acquire)) {
            // spin until lock is acquired
            while (_flag.test(std::memory_order_relaxed)) {
                // hint for the CPU — reduces power consumption
#if defined(_MSC_VER)
                _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
                __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
                __builtin_arm_isb();
#endif
            }
        }
    }

    bool try_lock() noexcept {
        return !_flag.test_and_set(std::memory_order_acquire);
    }

    void unlock() noexcept {
        _flag.clear(std::memory_order_release);
    }

private:
    std::atomic_flag _flag;
};

} // namespace kimix
