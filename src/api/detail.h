/*
 * detail.h -- internal C++ plumbing of the kimix_api FFI library.
 *
 * NOT part of the C ABI: this header is C++ only and is included by the
 * translation units under src/api (ffi_vec.cpp, ffi_repair.cpp, ...).  It owns
 * the two facts that must never drift apart between those files:
 *
 *   1. what object actually lives inside a caller-provided `kimix_vec`
 *      placeholder, where in that storage it lives, and how it is constructed /
 *      re-derived / destroyed (placement new + std::launder + explicit dtor);
 *   2. the mimalloc-backed `yyjson_alc` the JSON FFI bakes into every call, so
 *      no allocator ever crosses the boundary.
 *
 * The storage map of a placeholder (KIMIX_VEC_BYTES bytes, KIMIX_VEC_ALIGN
 * aligned):
 *
 *   offset 0                     offset sizeof(vector)          KIMIX_VEC_BYTES
 *   +----------------------------+---------------------+---------+
 *   |  the C++ vector object     |  (unused padding)   |  guard  |
 *   +----------------------------+---------------------+---------+
 *                                                          ^ 8 bytes at the tail
 *
 * The guard word is what makes "use before init" and "use after destroy" an
 * error return instead of undefined behaviour.  It is read and written through
 * std::memcpy: the storage is an `unsigned char` array, so no uint64_t object
 * ever exists at that address and the access stays within the aliasing / object
 * lifetime rules.
 */
#pragma once
#ifndef KIMIX_API_DETAIL_H
#define KIMIX_API_DETAIL_H

#include <api/ffi_common.h>
#include <api/ffi_vec.h>
#include <core/stl/vector.h>
#include <mimalloc.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

/* The vendored yyjson header is C, and the yyjson target publishes its include
 * directory through kimix-core (see src/ext/xmake.lua, target "kimix-yyjson"). */
#include <yyjson.h>

namespace kimix::api {

// ---------------------------------------------------------------------------
// The object inside a kimix_vec placeholder
// ---------------------------------------------------------------------------

/* What kimix::vector<std::byte> means for this library: the byte buffer every
 * FFI area hands back. */
using byte_vector = kimix::vector<std::byte>;

/* kimix::repair()'s own return type (core/json_repair.h).  Same size, same
 * alignment, same allocator, different element type -- see the bridge below. */
using char_vector = kimix::vector<char>;

/* The guard word written into the tail of the storage while the vector object
 * is alive, and the value written when it dies.  Any other value (a
 * caller-memset zero, stack garbage) means "no live object here". */
inline constexpr std::uint64_t k_vec_live_guard = 0x4B49585F56454354ULL; // "KIX_VECT"
inline constexpr std::uint64_t k_vec_dead_guard = 0;

/* The guard slot: the last 8 bytes of the placeholder storage.  sizeof() of the
 * array is known at compile time; the offset is the array size minus the guard,
 * so the C++ object and the guard can never overlap as long as the static
 * assert below holds (ffi_vec.cpp checks it against the real build). */
inline constexpr std::size_t k_vec_guard_offset = KIMIX_VEC_BYTES - sizeof(std::uint64_t);

inline void *vec_raw_storage(kimix_vec *v) noexcept {
    return static_cast<void *>(v->_storage);
}

inline const void *vec_raw_storage(const kimix_vec *v) noexcept {
    return static_cast<const void *>(v->_storage);
}

inline std::uint64_t vec_read_guard(const kimix_vec *v) noexcept {
    std::uint64_t guard = 0;
    std::memcpy(&guard, v->_storage + k_vec_guard_offset, sizeof(guard));
    return guard;
}

inline void vec_write_guard(kimix_vec *v, std::uint64_t guard) noexcept {
    std::memcpy(v->_storage + k_vec_guard_offset, &guard, sizeof(guard));
}

/* Re-derive the live C++ object from the storage bytes.  std::launder is what
 * makes the resulting pointer track the object that placement new created at
 * this address (the pointer we pass in is derived from the `unsigned char`
 * array, i.e. from a subsystem of the placeholder object, not from the vector
 * itself).  Only call it when the guard says an object is alive. */
inline byte_vector *vec_object(kimix_vec *v) noexcept {
    return std::launder(reinterpret_cast<byte_vector *>(vec_raw_storage(v)));
}

inline const byte_vector *vec_object(const kimix_vec *v) noexcept {
    return std::launder(reinterpret_cast<const byte_vector *>(vec_raw_storage(v)));
}

/* NULL `v`, or a placeholder that does not currently hold a live vector, yields
 * a null result; the caller turns that into KIMIX_ERR_INVALID_ARG /
 * KIMIX_ERR_INVALID_STATE. */
inline byte_vector *vec_live(kimix_vec *v) noexcept {
    return (v && vec_read_guard(v) == k_vec_live_guard) ? vec_object(v) : nullptr;
}

inline const byte_vector *vec_live(const kimix_vec *v) noexcept {
    return (v && vec_read_guard(v) == k_vec_live_guard) ? vec_object(v) : nullptr;
}

/* A placeholder must be raw (no live object) to be constructed into. */
inline bool vec_is_raw_storage(const kimix_vec *v) noexcept {
    return v && vec_read_guard(v) != k_vec_live_guard;
}

// -- construction (every one of these assumes the storage is raw) -----------

inline void vec_construct(kimix_vec *v) noexcept {
    ::new (vec_raw_storage(v)) byte_vector();
    vec_write_guard(v, k_vec_live_guard);
}

inline void vec_construct(kimix_vec *v, const byte_vector &src) {
    ::new (vec_raw_storage(v)) byte_vector(src);
    vec_write_guard(v, k_vec_live_guard);
}

inline void vec_construct_move(kimix_vec *v, byte_vector &&src) noexcept {
    ::new (vec_raw_storage(v)) byte_vector(std::move(src));
    vec_write_guard(v, k_vec_live_guard);
}

inline bool vec_construct(kimix_vec *v, const std::byte *first, const std::byte *last) {
    ::new (vec_raw_storage(v)) byte_vector(first, last);
    vec_write_guard(v, k_vec_live_guard);
    return true;
}

inline void vec_destruct(kimix_vec *v) noexcept {
    if (auto *object = vec_live(v)) {
        object->~vector();
        vec_write_guard(v, k_vec_dead_guard);
    }
}

// ---------------------------------------------------------------------------
// The vector<char> <-> vector<std::byte> bridge
// ---------------------------------------------------------------------------
/* kimix::repair() (core/json_repair.h) returns a vector<char>.  The two types
 * are the same std::vector specialization over the same mimalloc allocator with
 * element types of the same size and alignment, so their representations are
 * identical: `sizeof` and `alignof` are asserted equal by the user of this
 * helper, and the buffer a vector<char> owns may be reinterpreted as a
 * vector<std::byte> in place.  The move steals that buffer; `src` is left as a
 * moved-from vector (valid, size 0, owns nothing), so its own destructor frees
 * nothing and the single owner of the bytes is the object constructed in `v`.
 * Ownership stays inside the library heap, so one kimix_vec_destroy() releases
 * it. */
inline void vec_construct_move_from_char(kimix_vec *v, char_vector &&src) noexcept {
    /* Address of the caller's live vector<char>, reinterpreted as the
     * bit-compatible vector<std::byte> and re-derived with std::launder (the
     * object at that address was created as a vector<char>; we only touch it
     * through the byte flavour, and only until the move empties it). */
    auto *bytes = std::launder(reinterpret_cast<byte_vector *>(std::addressof(src)));
    vec_construct_move(v, std::move(*bytes));
}

/* yyjson's allocator, baked into every JSON call: mimalloc, exactly like the
 * rest of the library.  Never exposed through the C ABI (see ffi_yyjson.h). */
inline constexpr yyjson_alc yyjson_mi_alloc() noexcept {
    return yyjson_alc{
        /*malloc  =*/ [](void *, std::size_t size) -> void * { return mi_malloc(size); },
        /*realloc =*/ [](void *, void *p, std::size_t, std::size_t size) -> void * { return mi_realloc(p, size); },
        /*free    =*/ [](void *, void *p) { mi_free(p); },
        /*ctx     =*/ nullptr,
    };
}

inline const yyjson_alc &yyjson_mi_alc() noexcept {
    static const yyjson_alc alc = yyjson_mi_alloc();
    return alc;
}

} // namespace kimix::api

#endif /* KIMIX_API_DETAIL_H */
