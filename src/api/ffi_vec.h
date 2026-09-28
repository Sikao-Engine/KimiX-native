/*
 * ffi_vec.h -- kimix::vector<std::byte> (the mimalloc-backed byte buffer of
 * src/core/stl/vector.h) exported to C.
 *
 * WHY A PLACEHOLDER AND NOT AN OPAQUE HANDLE
 * ------------------------------------------
 * `kimix::vector<std::byte>` is `std::vector<std::byte, kimix::allocator<std::byte>>`:
 * its SIZE AND ALIGNMENT are compile-time constants of the library build, and
 * the buffer it owns lives on the mimalloc heap the library also owns.  That
 * makes the object small enough and stable enough to be stored INLINE in the
 * caller's own memory -- a struct field, an array element, a stack slot -- instead
 * of forcing a heap handle + dereference through every call.  The caller
 * therefore declares
 *
 *     kimix_vec v;                  // stack
 *     struct MyBuf { kimix_vec v; } // embedded in a caller struct
 *
 * and the FFI constructs / destructs the real C++ object inside
 * `v._storage` with placement new.  Because the object is constructed in
 * caller-provided raw storage, every entry point re-derives the C++ object
 * pointer with std::launder (the storage is a `unsigned char` array, so the
 * usual "access an object through a pointer derived from its storage byte
 * array" rule applies), and the RAII contract is explicit:
 *
 *     init  ->  use ...             ->  destroy          (mandatory pair)
 *
 * Using an uninitialised or already destroyed placeholder is reported as
 * KIMIX_ERR_INVALID_STATE instead of corrupting memory: the FFI keeps a guard
 * word in the tail of the storage that no C++ object occupies.
 *
 * ABI NOTES
 * ---------
 * * KIMIX_VEC_BYTES / KIMIX_VEC_ALIGN are generous FIXED numbers, not
 *   `sizeof(kimix::vector<std::byte>)`: the real size depends on the standard
 *   library and on the iterator-debug level of the build (MSVC debug builds
 *   store a debug proxy pointer, libstdc++ does not), so the placeholder is
 *   sized for every supported configuration and the library static_asserts it
 *   still fits.  Callers may embed, copy-by-init, or memset it freely; they
 *   must never read `_storage`.
 * * Move semantics are the only cheap transfer: a move leaves the source
 *   initialized and empty (a valid `kimix_vec`), and the buffer pointer travels
 *   with it, so the buffer is always freed by the library heap that allocated
 *   it.  Copying the raw bytes of a placeholder is NOT a copy of the vector --
 *   use kimix_vec_copy_init() for that.
 * * The byte vectors returned by other areas of this library (the repaired JSON
 *   of ffi_repair.h) are the very same `kimix_vec` type.
 */
#pragma once
#ifndef KIMIX_API_FFI_VEC_H
#define KIMIX_API_FFI_VEC_H

#include <api/ffi_common.h>

/* ---------------------------------------------------------------------------
 * The placeholder type
 * ------------------------------------------------------------------------- */

/* Fixed ABI size / alignment of the inline storage of one
 * kimix::vector<std::byte>.  A caller that wants to double-check them at
 * run time calls kimix_vec_abi_bytes() / kimix_vec_abi_align() or
 * kimix_api_layout_info(). */
#define KIMIX_VEC_BYTES 64u
#define KIMIX_VEC_ALIGN 16u

/* Portable alignment specifier.  C++ spells it `alignas` (MSVC's `cl` does NOT
 * accept the `_Alignas` keyword in C++ mode), C11 spells it `_Alignas`, and a
 * pre-C11 compiler needs its attribute spelling. */
#if defined(__cplusplus)
    #define KIMIX_ALIGNAS(N) alignas(N)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    #define KIMIX_ALIGNAS(N) _Alignas(N)
#elif defined(_MSC_VER)
    #define KIMIX_ALIGNAS(N) __declspec(align(N))
#elif defined(__GNUC__)
    #define KIMIX_ALIGNAS(N) __attribute__((aligned(N)))
#else
    #error "kimix_api: no alignment specifier known for this compiler"
#endif

/* Inline storage of one kimix::vector<std::byte>.  Treat the bytes as opaque:
 * only the kimix_vec_* functions below may touch them. */
typedef struct kimix_vec {
    KIMIX_ALIGNAS(KIMIX_VEC_ALIGN) unsigned char _storage[KIMIX_VEC_BYTES];
} kimix_vec;

/* Compile-time size/alignment of the placeholder as the CALLER sees it.  A
 * binding generator may assert on these; the library asserts that its own C++
 * object fits into them (see ffi_vec.cpp). */
#define KIMIX_VEC_SIZEOF ((size_t)sizeof(kimix_vec))

KIMIX_FFI_BEGIN

/* ===========================================================================
 * 1. ABI facts
 * ======================================================================== */

/* The fixed inline-storage size / alignment a caller must reserve
 * (KIMIX_VEC_BYTES / KIMIX_VEC_ALIGN). */
KIMIX_FFI size_t kimix_vec_abi_bytes(void);
KIMIX_FFI size_t kimix_vec_abi_align(void);

/* sizeof / alignof of the C++ object the library actually constructs inside the
 * storage.  Useful to notice an ABI change (a new STL, a debug proxy): it must
 * be <= kimix_vec_abi_bytes() and <= kimix_vec_abi_align() at all times. */
KIMIX_FFI size_t kimix_vec_inner_bytes(void);
KIMIX_FFI size_t kimix_vec_inner_align(void);

/* True when `v` currently holds a live C++ vector: initialised and not yet
 * destroyed.  Every other kimix_vec_* call performs this check itself and
 * returns KIMIX_ERR_INVALID_STATE on failure; this one is exposed so a caller
 * can assert in its own teardown path.  A NULL `v` reports false. */
KIMIX_FFI bool kimix_vec_is_initialized(KIMIX_IN const kimix_vec *v);

/* ===========================================================================
 * 2. Construction and destruction (the RAII pair)
 * ======================================================================== */

/* Construct an EMPTY vector in `v` (C++: `new (storage) vector<byte>()`).
 * `v` must be raw storage: not yet initialised, or memset to zero.
 * Returns KIMIX_ERR_INVALID_STATE if `v` already holds a live vector (that
 * would leak its buffer) and KIMIX_ERR_INVALID_ARG for a NULL `v`. */
KIMIX_FFI kimix_status kimix_vec_default_init(KIMIX_OUT kimix_vec *v);

/* Construct `v` as a copy of the bytes [data, data + len).  `data` may be NULL
 * only when len == 0.  Same pre-conditions as kimix_vec_default_init(). */
KIMIX_FFI kimix_status kimix_vec_init_from(KIMIX_OUT kimix_vec *v,
                                           KIMIX_IN const void *data,
                                           size_t len);

/* Construct `dst` (uninitialised storage) as a COPY of the live vector `src`.
 * Allocates len(src) bytes on the library heap. */
KIMIX_FFI kimix_status kimix_vec_copy_init(KIMIX_OUT kimix_vec *dst,
                                           KIMIX_IN const kimix_vec *src);

/* Construct `dst` (uninitialised storage) by MOVING the live vector `src`
 * (C++: `new (dst) vector<byte>(std::move(*src))`).  After a successful call
 * `src` is still initialized and valid but empty (a moved-from vector); it must
 * still be destroyed exactly once. */
KIMIX_FFI kimix_status kimix_vec_move_init(KIMIX_OUT kimix_vec *dst,
                                           KIMIX_TRANSFER kimix_vec *src);

/* Destroy the vector in `v` and release its buffer to the library heap
 * (C++: `v->~vector()` then clear the guard).  After this the placeholder is
 * raw storage again and may be re-initialised.  Destroying an already destroyed
 * or never initialised placeholder returns KIMIX_ERR_INVALID_STATE and touches
 * nothing. */
KIMIX_FFI kimix_status kimix_vec_destroy(KIMIX_IN_OUT kimix_vec *v);

/* ===========================================================================
 * 3. Assignment (both sides must already hold a live vector)
 * ======================================================================== */

/* `dst = *src` (deep copy; dst's old content is freed on the library heap). */
KIMIX_FFI kimix_status kimix_vec_assign(KIMIX_IN_OUT kimix_vec *dst,
                                        KIMIX_IN const kimix_vec *src);

/* `dst = std::move(*src)`: dst's old content is freed, src becomes empty but
 * stays initialised. */
KIMIX_FFI kimix_status kimix_vec_move_assign(KIMIX_IN_OUT kimix_vec *dst,
                                             KIMIX_TRANSFER kimix_vec *src);

/* Replace the content with the bytes [data, data + len) (assign-range). */
KIMIX_FFI kimix_status kimix_vec_assign_bytes(KIMIX_IN_OUT kimix_vec *v,
                                              KIMIX_IN const void *data,
                                              size_t len);

/* ===========================================================================
 * 4. Observation
 * ======================================================================== */

/* Number of bytes stored (0 for an empty vector).
 * Returns (size_t)-1 when `v` is NULL or not initialised. */
KIMIX_FFI size_t kimix_vec_size(KIMIX_IN const kimix_vec *v);

/* Allocated capacity in bytes.  Returns (size_t)-1 on an invalid `v`. */
KIMIX_FFI size_t kimix_vec_capacity(KIMIX_IN const kimix_vec *v);

/* True when size() == 0.  Returns false on an invalid `v`.
 * CAUTION: a never-initialised or already-destroyed placeholder therefore looks
 * just like a LIVE but empty vector through this call.  Use
 * kimix_vec_is_initialized() to tell them apart, or kimix_vec_size(), whose
 * (size_t)-1 sentinel does report a dead placeholder. */
KIMIX_FFI bool kimix_vec_empty(KIMIX_IN const kimix_vec *v);

/* Pointer to the contiguous bytes.  The pointer stays valid (BORROWED from the
 * vector) until any call that resizes, reserves, assigns or destroys the
 * vector.  NULL is returned when `v` is invalid, and -- as with std::vector --
 * also for an empty vector; for an empty vector the returned pointer must not
 * be dereferenced. */
KIMIX_FFI unsigned char *kimix_vec_data(KIMIX_BORROWED kimix_vec *v);

/* Bounds-checked element address: `*out == &data()[index]`.  Writes nothing and
 * returns KIMIX_ERR_OUT_OF_RANGE when index >= size(),
 * KIMIX_ERR_INVALID_ARG/INVALID_STATE on a bad argument / dead placeholder. */
KIMIX_FFI kimix_status kimix_vec_at(KIMIX_BORROWED kimix_vec *v,
                                    size_t index,
                                    KIMIX_OUT unsigned char **out);

/* Copy min(len, available) bytes: `kimix_vec_read` copies out of the vector
 * starting at `offset` into `dst`; `kimix_vec_write` copies `len` bytes from
 * `src` INTO the vector starting at `offset`.  Both require the whole range
 * [offset, offset + len) to lie inside the vector (resize/reserve first) and
 * return KIMIX_ERR_OUT_OF_RANGE otherwise. */
KIMIX_FFI kimix_status kimix_vec_read(KIMIX_IN const kimix_vec *v,
                                      size_t offset,
                                      KIMIX_OUT void *dst,
                                      size_t len);
KIMIX_FFI kimix_status kimix_vec_write(KIMIX_IN_OUT kimix_vec *v,
                                       size_t offset,
                                       KIMIX_IN const void *src,
                                       size_t len);

/* Element-wise equality of two vectors (size and content).  Writes 1 / 0 to
 * `*out_equal`; returns an error only for a bad argument / dead placeholder. */
KIMIX_FFI kimix_status kimix_vec_equals(KIMIX_IN const kimix_vec *a,
                                        KIMIX_IN const kimix_vec *b,
                                        KIMIX_OUT bool *out_equal);

/* ===========================================================================
 * 5. Modification
 * ======================================================================== */

/* v.resize(n) -- grows with zero bytes or shrinks; may reallocate (invalidates
 * a previously returned kimix_vec_data() pointer). */
KIMIX_FFI kimix_status kimix_vec_resize(KIMIX_IN_OUT kimix_vec *v, size_t n);

/* v.resize(n, fill). */
KIMIX_FFI kimix_status kimix_vec_resize_fill(KIMIX_IN_OUT kimix_vec *v,
                                             size_t n,
                                             unsigned char fill);

/* v.reserve(n) -- capacity only, size unchanged; may reallocate. */
KIMIX_FFI kimix_status kimix_vec_reserve(KIMIX_IN_OUT kimix_vec *v, size_t n);

/* v.shrink_to_fit() -- release unused capacity. */
KIMIX_FFI kimix_status kimix_vec_shrink_to_fit(KIMIX_IN_OUT kimix_vec *v);

/* v.clear() -- size becomes 0, capacity is kept. */
KIMIX_FFI kimix_status kimix_vec_clear(KIMIX_IN_OUT kimix_vec *v);

/* v.push_back(b). */
KIMIX_FFI kimix_status kimix_vec_push_back(KIMIX_IN_OUT kimix_vec *v,
                                           unsigned char b);

/* v.pop_back(); KIMIX_ERR_OUT_OF_RANGE when the vector is empty. */
KIMIX_FFI kimix_status kimix_vec_pop_back(KIMIX_IN_OUT kimix_vec *v);

/* Append the bytes [data, data + len) (insert-range at the end). */
KIMIX_FFI kimix_status kimix_vec_append(KIMIX_IN_OUT kimix_vec *v,
                                        KIMIX_IN const void *data,
                                        size_t len);

/* Insert the bytes [data, data + len) before index (0 <= index <= size()). */
KIMIX_FFI kimix_status kimix_vec_insert(KIMIX_IN_OUT kimix_vec *v,
                                        size_t index,
                                        KIMIX_IN const void *data,
                                        size_t len);

/* Remove the [index, index + len) byte range (len may be 0; the range must lie
 * inside the vector). */
KIMIX_FFI kimix_status kimix_vec_erase(KIMIX_IN_OUT kimix_vec *v,
                                       size_t index,
                                       size_t len);

/* Swap the two vectors' buffers in O(1).  Both stay initialised; neither is
 * copied and no allocation happens. */
KIMIX_FFI kimix_status kimix_vec_swap(KIMIX_IN_OUT kimix_vec *a,
                                      KIMIX_IN_OUT kimix_vec *b);

/* ===========================================================================
 * 6. Heap convenience (for callers that prefer a handle over inline storage)
 * ======================================================================== */

/* Allocate one placeholder on the LIBRARY heap (the same mimalloc instance every
 * other area uses, aligned to KIMIX_VEC_ALIGN) and default-construct an empty
 * vector in it.  NULL on allocation failure.  Release with kimix_vec_free(). */
KIMIX_FFI kimix_vec *kimix_vec_new(void);

/* Same, initialised with the bytes [data, data + len). */
KIMIX_FFI kimix_vec *kimix_vec_new_from(KIMIX_IN const void *data, size_t len);

/* Destroy the vector and release the placeholder storage.  NULL is ignored;
 * `v` must have come from kimix_vec_new() / kimix_vec_new_from() (a
 * stack-allocated placeholder must be released with kimix_vec_destroy()). */
KIMIX_FFI void kimix_vec_free(KIMIX_TRANSFER kimix_vec *v);

KIMIX_FFI_END

#endif /* KIMIX_API_FFI_VEC_H */
