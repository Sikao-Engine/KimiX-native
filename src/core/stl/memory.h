/*
 * memory.h — kimix::allocator<T> (backed by kimix::allocate) and utility aliases.
 *
 * kimix::allocator<T>:
 *   A stateless, STL-compatible allocator using the kimix::allocate gateway
 *   (core/memory.h → mimalloc), so it is tracked when KIMIX_MEMORY_STRICT is
 *   on and never touches the CRT heap.
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
 *   (std types in both legs; see the third-party-leg note below)
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
 *
 * THIRD-PARTY LEG (KIMIX_EXTERNAL_THIRDPARTY)
 *   kimix::allocator<T> stays this class in both legs — same signatures, same
 *   "never throws, abort through allocation_failure()" contract, same
 *   is_always_equal — but kimix::detail::allocator_* (stl/memory.cpp) forwards
 *   to the kimix gateway (core/memory.h), which in that leg is compiled against
 *   the mimalloc LC exports from luisa-core. LC's own luisa::allocator uses that
 *   same mimalloc heap, so the two spellings are one allocator family, not two.
 *   span and bit_cast pick up LC's (EASTL) types in that leg; the owning smart
 *   pointers deliberately stay the std ones (a plain `new` pointee cannot be
 *   released by an EASTL deleter) — see the alias block below.
 */
#pragma once

// STL-compatible allocator backed by kimix::allocate, plus smart-pointer and utility aliases.
// Include this header instead of <memory> in KimixBase code.

#include "../memory.h"
#include "../dll_export.h"

#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>
#include <memory_resource>

// <memory> is unconditional in BOTH legs: the owning smart-pointer aliases below
// are std::unique_ptr / std::shared_ptr / std::weak_ptr in each of them (see the
// alias block), so this header always needs the std declarations. Only the
// NON-OWNING utilities come from the third-party tree.
#include <memory>

// Third-party leg (KIMIX_EXTERNAL_THIRDPARTY): kimix::span / kimix::bit_cast
// below are LC's (EASTL) ones, so a kimix::span is a luisa::span and one span
// implementation serves the whole graph. The owning pointers stay the std ones
// in that leg too — see the alias block below for why. Standalone keeps every
// std type (unchanged behaviour).
#if defined(KIMIX_EXTERNAL_THIRDPARTY)
#include <luisa/core/stl/memory.h>
#else
#include <span>
#include <bit>
#endif

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
        void* p = kimix::allocate(sizeof(T) * n);
        if (!p) { allocation_failure(); }
        return static_cast<T*>(p);
    }

    [[nodiscard]] T* allocate(size_type n, size_t alignment, size_t offset = 0) {
        if (n == 0) { return nullptr; }
        void* p = kimix::allocate_aligned_at(sizeof(T) * n, alignment, offset);
        if (!p) { allocation_failure(); }
        return static_cast<T*>(p);
    }

    void deallocate(T* p, size_type /*n*/) noexcept {
        kimix::deallocate(p);
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
    T* p = kimix::allocate_with_allocator<T>(1);
    ::new (p) T(std::forward<Args>(args)...);
    return p;
}

template <typename T>
inline void delete_with_allocator(T* p) {
    if (p) {
        p->~T();
        // Qualified on purpose: with a T that lives in the luisa namespace (its
        // containers are kimix's containers in the third-party leg) an
        // unqualified call would be ambiguous with luisa::deallocate_with_allocator
        // through ADL.
        kimix::deallocate_with_allocator(p);
    }
}

// ---------------------------------------------------------------------------
// Smart pointer and utility aliases
// ---------------------------------------------------------------------------

#if defined(KIMIX_EXTERNAL_THIRDPARTY)

// Only the NON-OWNING utilities are LC's here.
//
// The owning pointers stay the std ones in both legs, deliberately. kimix code
// allocates its pointees with a plain `new` (several types — e.g.
// graph_db::Vertex — have their own over-aligned operator new routed through the
// kimix gateway), while EASTL's default_delete / eastl::allocator hand the block
// back through luisa::detail::allocator_deallocate, i.e. through mimalloc rather
// than through the CRT operator delete that `new` chose. Mapping these aliases
// would silently change the alloc/free pair of every existing call site and abort
// with mimalloc's "invalid pointer" at the first destruction — measured, not
// theorised: kimix's graph_db suite dies there. unique_ptr / shared_ptr /
// weak_ptr own nothing themselves, are not a container backend, and keep their
// observable semantics in this leg.
template <typename T>
using unique_ptr = std::unique_ptr<T>;

template <typename T>
using shared_ptr = std::shared_ptr<T>;

template <typename T>
using weak_ptr = std::weak_ptr<T>;

// span / bit_cast carry no ownership at all, so they ARE mapped: kimix::span is
// a luisa::span (EASTL) and one span implementation serves both libraries.
template <typename T>
using span = luisa::span<T>;

template <typename To, typename From>
constexpr To bit_cast(const From& from) noexcept {
    return luisa::bit_cast<To>(from);
}

#else

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

#endif

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
// detail: allocator helper functions (exported, defined in stl/memory.cpp)
// ---------------------------------------------------------------------------
//
// One declaration and one definition per name — the alignment-parameterised
// forms are the ones SharedFunction (core/shared_function.h) and the typed
// allocator use. They are out-of-line on purpose: the whole point of the
// gateway is that a single translation unit (core/memory.cpp) owns the heap
// entry point, so a header-inline copy here would fork it again.
//
// An alignment the platform already guarantees takes the plain path; only a
// larger one pays for the aligned mimalloc entry point.

namespace detail {

KIMIX_CORE_API void* allocator_allocate(size_t size) noexcept;

KIMIX_CORE_API void* allocator_allocate(size_t size, size_t alignment) noexcept;

KIMIX_CORE_API void allocator_deallocate(void* p) noexcept;

KIMIX_CORE_API void* allocator_reallocate(void* p, size_t new_size) noexcept;

KIMIX_CORE_API void* allocator_reallocate(void* p, size_t new_size, size_t alignment) noexcept;

} // namespace detail

} // namespace kimix
