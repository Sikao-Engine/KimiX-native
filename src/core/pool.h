/*
 * pool.h — kimix::Pool<T, ThreadSafe> object pool.
 *
 * A fixed-block-size object pool (block_size = 64 nodes per block).
 * Supports thread-safe (default) and non-thread-safe (ThreadSafe = false)
 * modes.
 *
 *   allocate()               — get raw uninitialized T*.
 *   deallocate(ptr)          — return to the free list.
 *   create(args...)          — allocate + construct in-place.
 *   destroy(ptr)             — destruct + deallocate.
 *   allocated_count()        — number of currently allocated objects.
 *
 * Freed objects are reused (LIFO free list) before new blocks are
 * allocated. T must be at least sizeof(void*) for the free list to fit.
 *
 * Example:
 *   kimix::Pool<int64_t> pool;
 *   int64_t* p = pool.create(42);
 *   pool.destroy(p);
 */
#pragma once

#include "stl/vector.h"
#include "spin_mutex.h"

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>
#include <atomic>
#include <variant>

namespace kimix {

// ---------------------------------------------------------------------------
// Pool<T, ThreadSafe> — thread-safe object pool
// Uses local free lists (block_size=64) for fast allocation/deallocation.
// ---------------------------------------------------------------------------

template <typename T, bool ThreadSafe = true>
class Pool {
public:
    using value_type = T;
    static constexpr size_t block_size = 64;

    Pool() = default;

    ~Pool() {
        // Free all blocks
        constexpr auto alignment = std::max(alignof(T), sizeof(T*));
        for (auto* block : _blocks) {
            ::operator delete(block, std::align_val_t{alignment});
        }
    }

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
    Pool(Pool&&) = delete;
    Pool& operator=(Pool&&) = delete;

    // Allocate a raw T* (uninitialized memory)
    T* allocate() {
        if constexpr (ThreadSafe) {
            std::lock_guard<spin_mutex> lock(_mutex);
            return allocate_impl();
        } else {
            return allocate_impl();
        }
    }

    // Deallocate a T* (return to pool)
    void deallocate(T* ptr) {
        if constexpr (ThreadSafe) {
            std::lock_guard<spin_mutex> lock(_mutex);
            deallocate_impl(ptr);
        } else {
            deallocate_impl(ptr);
        }
    }

    // Create a fully constructed T
    template <typename... Args>
    T* create(Args&&... args) {
        T* ptr = allocate();
        if (ptr) {
            ::new (ptr) T(std::forward<Args>(args)...);
        }
        return ptr;
    }

    // Destroy and return to pool
    void destroy(T* ptr) {
        if (ptr) {
            ptr->~T();
            deallocate(ptr);
        }
    }

    size_t allocated_count() const noexcept {
        if constexpr (ThreadSafe) {
            return _allocated.load(std::memory_order_relaxed);
        } else {
            return _allocated.value;
        }
    }

private:
    T* allocate_impl() {
        if (_free_list) {
            T* result = _free_list;
            _free_list = *reinterpret_cast<T**>(_free_list);
            if constexpr (ThreadSafe) {
                _allocated.fetch_add(1, std::memory_order_relaxed);
            } else {
                ++_allocated.value;
            }
            return result;
        }

        // Allocate a new block
        T* block = static_cast<T*>(::operator new(sizeof(T) * block_size, std::align_val_t{std::max(alignof(T), sizeof(T*))}));
        _blocks.push_back(block);

        // Thread free list through the block
        for (size_t i = 0; i < block_size; ++i) {
            deallocate_impl(&block[i]);
        }

        return allocate_impl();
    }

    void deallocate_impl(T* ptr) {
        *reinterpret_cast<T**>(ptr) = _free_list;
        _free_list = ptr;
        if constexpr (ThreadSafe) {
            _allocated.fetch_sub(1, std::memory_order_relaxed);
        } else {
            --_allocated.value;
        }
    }
    T* _free_list = nullptr;
    vector<T*> _blocks;

    struct NoAtomic { size_t value = 0; };
    using Counter = std::conditional_t<ThreadSafe, std::atomic<size_t>, NoAtomic>;

    Counter _allocated{};
    [[no_unique_address]] std::conditional_t<ThreadSafe, spin_mutex, std::monostate> _mutex;
};

namespace detail {
void memory_pool_check_memory_leak(size_t expected, size_t actual) noexcept;
} // namespace detail

} // namespace kimix
