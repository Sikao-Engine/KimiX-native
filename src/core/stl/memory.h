/*
 * memory.h — kimix::allocator<T> (mimalloc-backed) and utility aliases.
 *
 * kimix::allocator<T>:
 *   A stateless, STL-compatible allocator using mimalloc.
 *   Supports alignment via allocate(n, alignment, offset=0).
 *   All allocator instantiations compare equal.
 *   allocate(0) returns nullptr.
 *   allocate(n) never throws: kimix is built without C++ exceptions
 *   (kimix_enable_exception=false), so an exhausted allocator reports the
 *   failure through kimix::allocation_failure() and aborts the process - the
 *   same observable outcome as an uncaught std::bad_alloc in an
 *   exceptions-enabled build.
 *
 * Size literals (inline namespace kimix::size_literals):
 *   1_k, 2_M, 4_G — compile-time size constants.
 *
 * Helper functions:
 *   allocate_with_allocator<T>(n), deallocate_with_allocator(p)
 *   new_with_allocator<T>(args...), delete_with_allocator(p)
 *
 * Smart-pointer aliases:
 *   kimix::unique_ptr<T>, kimix::shared_ptr<T>, kimix::weak_ptr<T>
 *
 * Other utilities:
 *   kimix::span<T>, kimix::bit_cast<To>(from)
 *   kimix::align(s, a)  — round up to alignment
 *   kimix::pointer_hash<T>
 *
 * Example:
 *   kimix::allocator<int> alloc;
 *   int* p = alloc.allocate(10);
 *   alloc.deallocate(p, 10);
 */
#pragma once

// STL-compatible allocator backed by mimalloc, plus smart-pointer and utility aliases.
// Include this header instead of <memory> in KimixBase code.

#include <mimalloc.h>
#include "../dll_export.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>
#include <memory_resource>

namespace kimix {

// ---------------------------------------------------------------------------
// Allocation failure
// ---------------------------------------------------------------------------
//
// The project is compiled without C++ exceptions (kimix_enable_exception=false),
// so a failed allocation cannot be signalled with std::bad_alloc.  Allocation
// failure is not recoverable here: report it and abort, which is exactly what
// an uncaught std::bad_alloc did in the exceptions-enabled configuration.
[[noreturn]] inline void allocation_failure() noexcept {
    static const char k_message[] =
        "kimix: allocation failed (out of memory); aborting\n";
    std::fputs(k_message, stderr);
    std::fflush(stderr);
    std::abort(); // [[noreturn]]: the process never comes back
}

// ---------------------------------------------------------------------------
// STL-compatible mimalloc allocator
// ---------------------------------------------------------------------------

template <typename T>
class allocator {
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::true_type;

    constexpr allocator() noexcept = default;
    constexpr allocator(const allocator&) noexcept = default;
    template <typename U>
    constexpr allocator(const allocator<U>&) noexcept {}
    ~allocator() = default;

    [[nodiscard]] T* allocate(size_type n) {
        if (n == 0) { return nullptr; }
        void* p = mi_malloc(sizeof(T) * n);
        if (!p) { allocation_failure(); }
        return static_cast<T*>(p);
    }

    [[nodiscard]] T* allocate(size_type n, size_t alignment, size_t offset = 0) {
        if (n == 0) { return nullptr; }
        void* p = mi_malloc_aligned_at(sizeof(T) * n, alignment, offset);
        if (!p) { allocation_failure(); }
        return static_cast<T*>(p);
    }

    void deallocate(T* p, size_type /*n*/) noexcept {
        mi_free(p);
    }
};

// All kimix::allocator instances are stateless and equal.
template <typename T, typename U>
constexpr bool operator==(const allocator<T>&, const allocator<U>&) noexcept { return true; }

template <typename T, typename U>
constexpr bool operator!=(const allocator<T>&, const allocator<U>&) noexcept { return false; }

// ---------------------------------------------------------------------------
// size literals (K, M, G)
// ---------------------------------------------------------------------------

inline namespace size_literals {

constexpr size_t operator""_k(unsigned long long v) noexcept { return static_cast<size_t>(v * 1024ull); }
constexpr size_t operator""_M(unsigned long long v) noexcept { return static_cast<size_t>(v * 1024ull * 1024ull); }
constexpr size_t operator""_G(unsigned long long v) noexcept { return static_cast<size_t>(v * 1024ull * 1024ull * 1024ull); }

} // inline namespace size_literals

// ---------------------------------------------------------------------------
// Alignment helper
// ---------------------------------------------------------------------------

constexpr size_t align(size_t s, size_t a) noexcept {
    return (s + a - 1) & ~(a - 1);
}

// ---------------------------------------------------------------------------
// Allocator-based new/delete (raw allocation + construction)
// ---------------------------------------------------------------------------

template <typename T>
inline T* allocate_with_allocator(size_t n = 1) {
    return allocator<T>{}.allocate(n);
}

template <typename T>
inline void deallocate_with_allocator(T* p) {
    allocator<T>{}.deallocate(p, 1);
}

template <typename T, typename... Args>
inline T* new_with_allocator(Args&&... args) {
    T* p = allocate_with_allocator<T>(1);
    ::new (p) T(std::forward<Args>(args)...);
    return p;
}

template <typename T>
inline void delete_with_allocator(T* p) {
    if (p) {
        p->~T();
        deallocate_with_allocator(p);
    }
}

// ---------------------------------------------------------------------------
// Smart pointer and utility aliases
// ---------------------------------------------------------------------------

template <typename T>
using unique_ptr = std::unique_ptr<T>;

template <typename T>
using shared_ptr = std::shared_ptr<T>;

template <typename T>
using weak_ptr = std::weak_ptr<T>;

template <typename T>
using span = std::span<T>;

template <typename To, typename From>
constexpr To bit_cast(const From& from) noexcept {
    return std::bit_cast<To>(from);
}

// ---------------------------------------------------------------------------
// Pointer hash
// ---------------------------------------------------------------------------

template <typename T>
struct pointer_hash {
    size_t operator()(const T* p) const noexcept {
        return static_cast<size_t>(hash64(&p, sizeof(p)));
    }
};

// ---------------------------------------------------------------------------
// detail: allocator helper functions (exported)
// ---------------------------------------------------------------------------

namespace detail {

inline void* allocator_allocate(size_t size) noexcept {
    return mi_malloc(size);
}

inline void* allocator_allocate(size_t size, size_t alignment, size_t offset = 0) noexcept {
    return mi_malloc_aligned_at(size, alignment, offset);
}

inline void allocator_deallocate(void* p) noexcept {
    mi_free(p);
}

inline void* allocator_reallocate(void* p, size_t new_size) noexcept {
    return mi_realloc(p, new_size);
}

} // namespace detail

} // namespace kimix
