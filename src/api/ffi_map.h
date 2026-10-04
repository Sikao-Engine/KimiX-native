/*
 * ffi_map.h -- an opaque uint64 -> uint64 hash map exported to C, over
 * kimix::unordered_map (the ankerl dense-map port vendored in
 * src/core/stl/unordered_dense.h and aliased with the library's mimalloc
 * allocator in src/core/stl/unordered_map.h).
 *
 * SCOPE
 * -----
 * The shape every digest table in this project needs: a key is an already
 * computed 64-bit content digest, a value is an id, a count or an offset, and
 * the only operations are insert-or-overwrite, lookup, remove, count and an
 * ordered walk of the entries.  uint64 -> uint64 with no key/value marshalling
 * keeps the boundary free of pointers into caller memory: nothing crosses it
 * except scalars, so the surface cannot be mis-used with a bad length, and the
 * caller never has to keep a buffer alive for the lifetime of the map.
 *
 * The map is an OPAQUE HANDLE (`kimix_map *`), not an inline placeholder like
 * kimix_vec: a dense table owns a bucket array and a value array that live on
 * the library heap and MOVE as the map grows, so its C++ object is not something
 * a caller may embed by guessing a byte count (the placeholder trick of
 * api/ffi_vec.h works there only because a std::vector is three pointers and its
 * buffer is separately owned).  A handle is one pointer, allocated by
 * kimix_map_new() on this library's mimalloc heap and released by exactly one
 * kimix_map_free() -- the one-heap rule of api/ffi_common.h (rule 4).
 *
 * WHAT IS NOT HERE
 * ----------------
 * No iterator objects (a C binding would have to model a C++ iterator's
 * lifetime), no bulk transfer in or out, no hashing of caller bytes (hash a
 * digest in the caller and hand the result over), no erase-if-predicate, no
 * per-bucket control.  entry_at() + size() cover every
 * "walk the whole map" need, and the walk is in the container's own dense order
 * (see ITERATION ORDER below), which is what makes the walk cheap and storable.
 *
 * NULL AND ERROR BEHAVIOUR
 * ------------------------
 * Every entry point validates its handle.  Where the call returns kimix_status a
 * NULL map is KIMIX_ERR_INVALID_ARG and out-parameters stay untouched; where the
 * call returns a value the library's neutral-value convention applies (see
 * api/ffi_common.h): 0 / false / "nothing happened".  Concurrent mutation of the
 * SAME map from several threads is not supported -- the same rule as an STL
 * container (contract rule 6): different maps on different threads are fine.
 *
 * Valid for a C99/C11 compiler and for C++17 or newer; see api/ffi_common.h for
 * the rules shared by the whole surface.  The index is docs/ffi.md.
 */
#pragma once
#ifndef KIMIX_API_FFI_MAP_H
#define KIMIX_API_FFI_MAP_H

#include <api/ffi_common.h>

/* The map itself.  Incomplete on purpose: a caller only ever holds a pointer,
 * and the layout belongs to the library build (like yyjson_doc behind
 * api/ffi_yyjson.h). */
typedef struct kimix_map kimix_map;

KIMIX_FFI_BEGIN

/* ===========================================================================
 * 1. Lifetime
 * ======================================================================== */

/* Create an empty map on the LIBRARY heap (the object and its two arrays all
 * live there; nothing is allocated before the first insert).  Returns NULL when
 * the allocation failed -- the only allocation entry point of this area that can
 * report failure that way, exactly like kimix_vec_new().  Release with
 * kimix_map_free(); never with the caller's free(). */
KIMIX_FFI kimix_map *kimix_map_new(void);

/* Destroy the map and release the object plus both of its arrays to the library
 * heap.  NULL is ignored (so a caller's cleanup path may call it
 * unconditionally).  After this the pointer is dangling: every other entry point
 * validates the handle it is given, but it cannot tell a freed pointer from a
 * live one -- freeing twice is undefined behaviour, like freeing any object
 * twice. */
KIMIX_FFI void kimix_map_free(KIMIX_TRANSFER kimix_map *map);

/* ===========================================================================
 * 2. Capacity
 * ======================================================================== */

/* Reserve room for `count` entries: allocates the dense value array and the
 * bucket index up front so a caller that knows the cardinality pays for one
 * growth instead of log2(n) of them.  Like std::vector::reserve it only ever
 * GROWS the storage: a `count` at or below what the map already holds is a
 * successful no-op, and no capacity is ever released before
 * kimix_map_free().  Entries keep their order and their identity through a
 * reserve (the value array is relocated, not rebuilt).
 *
 * Returns KIMIX_ERR_INVALID_ARG for a NULL `map`, KIMIX_OK otherwise (the
 * allocator aborts on exhaustion instead of throwing, so there is no
 * OUT_OF_MEMORY path here -- see api/ffi_vec.h). */
KIMIX_FFI kimix_status kimix_map_reserve(KIMIX_IN_OUT kimix_map *map, uint64_t count);

/* Remove every entry.  O(n) over the dense array, and -- like
 * std::vector::clear / std::unordered_map::clear -- it RELEASES NOTHING: the
 * reserved capacity and the bucket array are kept for reuse, so
 * kimix_map_allocated_count() does NOT drop to 0 here (only kimix_map_free()
 * takes the storage back).  Afterwards the map behaves as brand new except for
 * that retained capacity, and the next insertions start a fresh insertion order.
 *
 * Returns KIMIX_ERR_INVALID_ARG for a NULL `map`, KIMIX_OK otherwise. */
KIMIX_FFI kimix_status kimix_map_clear(KIMIX_IN_OUT kimix_map *map);

/* ===========================================================================
 * 3. Insert, lookup, remove
 * ======================================================================== */

/* Insert-or-overwrite (upsert): after the call the map holds value under key.
 * A NEW key is appended at the end of the dense entry array (insertion order);
 * an EXISTING key has its value replaced IN PLACE, which keeps its position in
 * kimix_map_entry_at() -- see ITERATION ORDER in the header comment.
 *
 * Returns KIMIX_ERR_INVALID_ARG for a NULL `map`, KIMIX_OK otherwise. */
KIMIX_FFI kimix_status kimix_map_set(KIMIX_IN_OUT kimix_map *map, uint64_t key, uint64_t value);

/* Look up `key`.  The library's neutral-value convention (api/ffi_common.h): a
 * HIT writes *out_value and returns true; a MISS writes NOTHING to *out_value
 * (so the caller's own pre-initialised sentinel survives a miss) and returns
 * false.  A NULL `map` returns false.  A NULL `out_value` also returns false:
 * there is no status code to report it with, and "no value delivered" is exactly
 * what a miss means -- a caller that must distinguish the two checks the handle
 * itself before calling. */
KIMIX_FFI bool kimix_map_get(KIMIX_IN const kimix_map *map, uint64_t key, KIMIX_OUT uint64_t *out_value);

/* True (and removes the entry) when `key` was present; false and no change when
 * it was not.  A NULL `map` returns false.
 *
 * REMOVAL REORDERS the dense array: the container's backward-shift delete moves
 * the LAST entry into the hole the removed entry left and pops the back, so
 * entry_at() walks stay a permutation of the surviving entries but an index
 * above the hole may name a different pair after the call.  Never cache an index
 * across a remove. */
KIMIX_FFI bool kimix_map_remove(KIMIX_IN_OUT kimix_map *map, uint64_t key);

/* Whether `key` is present.  Neutral value (false) for a NULL `map`. */
KIMIX_FFI bool kimix_map_contains(KIMIX_IN const kimix_map *map, uint64_t key);

/* The number of entries (0 for a NULL or empty map).  Never fails. */
KIMIX_FFI uint64_t kimix_map_size(KIMIX_IN const kimix_map *map);

/* ===========================================================================
 * 4. Ordered walk
 * ======================================================================== */

/* The entry at dense index `index` (0 <= index < kimix_map_size(map)), written
 * through BOTH out-parameters.  The walk order is the container's own storage
 * order, i.e. insertion order for entries that were never removed (see ITERATION
 * ORDER in the header comment); it is NOT hash order and it is stable across
 * kimix_map_set() of an existing key and across kimix_map_reserve().
 *
 * Returns KIMIX_ERR_INVALID_ARG for a NULL `map` or a NULL `out_key` /
 * `out_value` (the out-parameters are untouched on every error), and
 * KIMIX_ERR_OUT_OF_RANGE when index >= size(). */
KIMIX_FFI kimix_status kimix_map_entry_at(KIMIX_IN const kimix_map *map, uint64_t index,
                                          KIMIX_OUT uint64_t *out_key,
                                          KIMIX_OUT uint64_t *out_value);

/* ===========================================================================
 * 5. Ownership accounting
 * ======================================================================== */

/* The bytes this map currently owns on the LIBRARY heap, as a pure layout sum of
 * the three pieces the object holds:
 *
 *     sizeof(the map object)                        // the handle's C++ struct
 *   + dense_capacity  * sizeof(pair<uint64,uint64>) // the entry array
 *   + bucket_count()  * sizeof(bucket)              // the uint32 index array
 *
 * `dense_capacity` is the number of element slots the entry array has reserved
 * (>= size()), and it is EXACTLY the capacity the library asked for: this FFI
 * drives every growth of the array through its own reserve() policy (doubling
 * from 8 slots, or the caller's kimix_map_reserve()), so the container never
 * reallocates behind the FFI's back and the term is a deterministic function of
 * the call sequence, not a peek at a private member.  `bucket_count()` is the
 * container's public bucket count, which the same reserve() calls size.
 *
 * What is deliberately NOT counted: mimalloc's per-block rounding (the real
 * blocks are >= these sizes), heap/thread-local bookkeeping, and anything the
 * caller allocated for its own keys or values.  Two consequences a leak ledger
 * must know: the number is a LOWER bound on the bytes the map occupies, and it
 * does not fall on kimix_map_clear() (clear keeps capacity) nor on remove()
 * (removal never shrinks either).
 *
 * Properties: it is written by no mutation and read by nothing but const calls,
 * so it is stable across any sequence of observations that does not change the
 * map; it never falls while the map exists (no entry point releases capacity);
 * and it drops to 0 with the map, since kimix_map_free() releases all three
 * pieces and
 * kimix_map_allocated_count(NULL) is 0 -- which is what makes a per-instance
 * ledger balance to 0 across new/set/free.
 *
 * Never fails; returns 0 for a NULL `map`. */
KIMIX_FFI uint64_t kimix_map_allocated_count(KIMIX_IN const kimix_map *map);

KIMIX_FFI_END

#endif /* KIMIX_API_FFI_MAP_H */
