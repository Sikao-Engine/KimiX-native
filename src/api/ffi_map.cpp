/*
 * ffi_map.cpp -- implementation of the opaque uint64 -> uint64 map C FFI
 * (api/ffi_map.h), over kimix::unordered_map (the ankerl dense table vendored in
 * core/stl/unordered_dense.h, aliased with the library's mimalloc allocator and
 * kimix::vector value storage in core/stl/unordered_map.h).
 *
 * THE HANDLE
 * ----------
 * `kimix_map` is the C tag the header declares; the struct below is its one and
 * only definition, at global namespace scope so the two names are the same type.
 * It holds the container plus ONE extra bookkeeping field, and it lives on this
 * library's mimalloc heap: mi_malloc_aligned + placement new in kimix_map_new(),
 * explicit destructor + mi_free in kimix_map_free() -- the same one-heap
 * discipline kimix_vec_new() uses for its placeholder, except that the object
 * here is never embedded in caller memory (its arrays move as the map grows,
 * which is why the area is a handle and not a placeholder; see the header).
 *
 * WHY dense_capacity IS THE CONTAINER'S REAL CAPACITY
 * ---------------------------------------------------
 * The accounting entry point needs the reserved size of the dense value array,
 * and the container does not publish its vector's capacity.  Instead of guessing
 * or poking at it, this file makes the capacity ITS OWN invariant: every growth of
 * the array is requested by the FFI through its own doubling policy (8 slots, then
 * 16, 32, ...) BEFORE the container could need room for one more entry, so the
 * container's vector never reallocates behind the FFI's back and `dense_capacity`
 * equals the real capacity after every entry point.  The policy is a strict
 * superset of the load-factor growth the table would apply on its own
 * (map::reserve(n) sizes the bucket index for n as well), so the pre-emptive
 * reserve costs nothing a plain insert would not have paid.
 *
 * ITERATION ORDER (what the walk guarantees, read off the container)
 * ------------------------------------------------------------------
 * core/stl/unordered_dense.h stores all entries in ONE hole-free dense vector
 * (`m_values`, its own comment) and iterates that vector, so entry_at(i) is
 * literally `m_values[i]`:
 *   - a NEW key is appended (emplace_back) => the walk is insertion order;
 *   - an EXISTING key is assigned in place => the entry keeps its index; the
 *     found path of operator[]/try_emplace neither rehashes nor moves;
 *   - reserve()/rehash() rebuild only the bucket index (the value array is
 *     relocated, order intact) => the walk is unchanged;
 *   - erase() is a backward-shift delete that MOVES THE LAST ENTRY INTO THE HOLE
 *     (`val = std::move(m_values.back()); ... m_values.pop_back();`) => a remove
 *     reorders, and an index at or above the hole may name a different pair
 *     afterwards.  The header documents exactly this and nothing stronger.
 *
 * OOM: like every other container area, none of these calls can report
 * KIMIX_ERR_OUT_OF_MEMORY -- kimix::allocator reports exhaustion through
 * kimix::allocation_failure() and aborts (core/stl/memory.h, no exceptions in this
 * build).  Only kimix_map_new(), which allocates the handle itself with
 * mi_malloc_aligned, can fail, and it reports that by returning NULL.
 *
 * Unity-build safety (same rule as ffi_vec.cpp / ffi_mem.cpp / ffi_repair.cpp):
 * every file-local helper is `map_`-prefixed inside an anonymous namespace; the
 * only global name this file adds is the `struct kimix_map` the header already
 * declares.
 */
#include <api/detail.h>
#include <api/ffi_map.h>
#include <core/stl/unordered_map.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <new>
#include <type_traits>

/* The object behind the opaque handle: the container, plus the exact reserved
 * slot count of its dense value array (see the WHY block above).  Default
 * construction allocates NOTHING: the table starts with no buckets and no value
 * storage, so an empty map's accounting is just the object term. */
struct kimix_map {
    kimix::unordered_map<std::uint64_t, std::uint64_t> entries;
    std::size_t dense_capacity = 0u;
};

namespace {

/* The container the handle holds, and the two element types whose sizes the
 * accounting adds up.  Local to this file (and `map_`-prefixed) because the build
 * batches the src/api translation units into one. */
using map_impl = decltype(kimix_map::entries);
using map_value_type = map_impl::value_type;
using map_bucket_type = map_impl::bucket_type;

/* The facts the accounting and the walk depend on, frozen against this build:
 * that the handle really holds the library's own dense map, and the element
 * widths and layout the documented byte count and the O(1) index assume. */
static_assert(std::is_same_v<map_impl, kimix::unordered_map<std::uint64_t, std::uint64_t>>,
              "the handle must hold the library's own mimalloc-backed dense map");
static_assert(sizeof(std::uint64_t) == 8u, "the C surface's uint64 must be 8 bytes wide");
static_assert(sizeof(map_value_type) == 16u,
              "the dense-entry term assumes a 16-byte uint64/uint64 pair");
static_assert(std::is_same_v<typename map_value_type::first_type, std::uint64_t> &&
                  std::is_same_v<typename map_value_type::second_type, std::uint64_t>,
              "the dense term counts key+value slots; the pair must be two uint64s");
static_assert(sizeof(map_bucket_type) == 8u,
              "the index term assumes the standard 2 x uint32 bucket (distance and "
              "fingerprint + value index); a different size means the accounted-bytes "
              "formula needs re-reading, not that the map is broken");
/* The dense table hands out random-access iterators over its value vector, which
 * is what makes entry_at() an O(1) index instead of a walk. */
static_assert(std::is_same_v<typename std::iterator_traits<typename map_impl::const_iterator>::iterator_category,
                             std::random_access_iterator_tag>,
              "kimix_map_entry_at() indexes the dense array; it must be random access");

/* The capacity of the first non-empty reservation, and the ceiling the container
 * itself enforces (the standard bucket type stores a uint32 value index, so
 * table::max_size() is 2^32 and reserve() clamps there silently -- the FFI clamps
 * the same way to keep dense_capacity honest).  The ladder is 8, 16, 32, ..., so
 * dense_capacity is always 0 or a power of two >= 8, which is what makes the
 * reported byte count reproducible from the call sequence alone. */
constexpr std::size_t k_map_initial_capacity = 8u;
constexpr std::size_t k_map_max_capacity = map_impl::max_size();
static_assert(k_map_max_capacity == (std::size_t{1} << 32),
              "the ceiling clamp below assumes the standard bucket's 2^32 max_size()");

/* Reserve at least `requested` dense slots (never below the live entry count, so
 * a request the map already satisfies is the documented no-op), on the doubling
 * ladder.  Maintains the invariant `dense_capacity == the value array's capacity`
 * (hence `>= size()`): the insert path calls this BEFORE the container could need
 * a slot, so the dense vector never reallocates behind the recorded number. */
inline void map_reserve_slots(kimix_map &self, std::size_t requested) noexcept {
    std::size_t needed = requested > self.entries.size() ? requested : self.entries.size();
    if (needed <= self.dense_capacity) {
        return; // already roomy: capacity untouched, nothing moves
    }
    std::size_t capacity = k_map_initial_capacity;
    while (capacity < needed) {
        if (capacity >= k_map_max_capacity / 2u) {
            capacity = k_map_max_capacity; // saturate at the container's ceiling
            break;
        }
        capacity *= 2u;
    }
    /* std::vector::reserve(n) with n > capacity allocates exactly n, which is what
     * makes the recorded number the real one; a standard library that ever rounded
     * up would only make the accounting a lower bound, which is what the header
     * promises anyway. */
    self.entries.reserve(capacity);
    self.dense_capacity = capacity;
}

/* Spare room for one more entry before inserting: the growth ladder the container
 * would have walked itself, driven from here so the capacity stays known. */
inline void map_room_for_one_more(kimix_map &self) noexcept {
    if (self.entries.size() >= self.dense_capacity) {
        map_reserve_slots(self, self.entries.size() + 1u);
    }
}

} // namespace

KIMIX_FFI_BEGIN

// ===========================================================================
// 1. Lifetime
// ===========================================================================

kimix_map *kimix_map_new(void) {
    /* Aligned allocation of the object itself: the container inside it wants its
     * own alignment, and mi_malloc only guarantees word alignment for small blocks
     * (the same reason kimix_vec_new() uses mi_malloc_aligned). */
    auto *block = static_cast<kimix_map *>(mi_malloc_aligned(sizeof(kimix_map), alignof(kimix_map)));
    if (!block) {
        return nullptr;
    }
    return ::new (block) kimix_map();
}

void kimix_map_free(kimix_map *map) {
    if (!map) {
        return; // documented as a no-op, so a caller's cleanup path is unconditional
    }
    map->~kimix_map(); // releases the value array and the bucket index through mi_free
    mi_free(map);
}

// ===========================================================================
// 2. Capacity
// ===========================================================================

kimix_status kimix_map_reserve(kimix_map *map, uint64_t count) {
    if (!map) {
        return KIMIX_ERR_INVALID_ARG;
    }
    map_reserve_slots(*map, static_cast<std::size_t>(count));
    return KIMIX_OK;
}

kimix_status kimix_map_clear(kimix_map *map) {
    if (!map) {
        return KIMIX_ERR_INVALID_ARG;
    }
    /* Drops every entry; capacity is kept (the container's clear() is the STL one)
     * and dense_capacity stays correct, so the accounting does not drop here. */
    map->entries.clear();
    return KIMIX_OK;
}

// ===========================================================================
// 3. Insert, lookup, remove
// ===========================================================================

kimix_status kimix_map_set(kimix_map *map, uint64_t key, uint64_t value) {
    if (!map) {
        return KIMIX_ERR_INVALID_ARG;
    }
    map_room_for_one_more(*map);
    /* operator[] == try_emplace(key).first->second: an existing key is found and
     * returned in place (no rehash, no move, index stable), a new key is appended
     * at the end of the dense array -- and the pre-reserve above guarantees that
     * append never reallocates behind the recorded capacity. */
    map->entries[key] = value;
    return KIMIX_OK;
}

bool kimix_map_get(const kimix_map *map, uint64_t key, uint64_t *out_value) {
    if (!map || !out_value) {
        return false; // neutral value: a NULL out-parameter is "no value delivered"
    }
    const auto it = map->entries.find(key);
    if (it == map->entries.end()) {
        return false; // a miss leaves *out_value untouched
    }
    *out_value = it->second;
    return true;
}

bool kimix_map_remove(kimix_map *map, uint64_t key) {
    if (!map) {
        return false;
    }
    /* erase(key) returns the number of entries removed (0 or 1 here); the
     * backward-shift delete moves the last entry into the hole it leaves (see the
     * header's ITERATION ORDER note). */
    return map->entries.erase(key) != 0u;
}

bool kimix_map_contains(const kimix_map *map, uint64_t key) {
    return map != nullptr && map->entries.find(key) != map->entries.end();
}

uint64_t kimix_map_size(const kimix_map *map) {
    return map ? static_cast<uint64_t>(map->entries.size()) : 0u;
}

// ===========================================================================
// 4. Ordered walk
// ===========================================================================

kimix_status kimix_map_entry_at(const kimix_map *map, uint64_t index,
                                uint64_t *out_key, uint64_t *out_value) {
    if (!map || !out_key || !out_value) {
        return KIMIX_ERR_INVALID_ARG;
    }
    const auto count = map->entries.size();
    if (index >= static_cast<std::uint64_t>(count)) {
        return KIMIX_ERR_OUT_OF_RANGE; // out-parameters untouched
    }
    /* The dense array is random access, so this is a direct element reference, not
     * a walk from the front. */
    const auto &entry = *(map->entries.begin() + static_cast<std::ptrdiff_t>(index));
    *out_key = entry.first;
    *out_value = entry.second;
    return KIMIX_OK;
}

// ===========================================================================
// 5. Ownership accounting
// ===========================================================================

uint64_t kimix_map_allocated_count(const kimix_map *map) {
    if (!map) {
        return 0u; // nothing exists, so nothing is owned: a ledger balances to 0
    }
    /* The three pieces the object owns, exactly as the header documents them: the
     * handle's C++ struct, the dense entry array at its reserved capacity, and the
     * bucket index at the container's own public bucket count.  mimalloc's
     * per-block rounding is deliberately NOT part of the sum: a lower bound with a
     * fixed, build-independent formula beats a "real" number that would depend on
     * the allocator's size classes. */
    const auto object_bytes = static_cast<std::uint64_t>(sizeof(kimix_map));
    const auto dense_bytes = static_cast<std::uint64_t>(map->dense_capacity) *
                             static_cast<std::uint64_t>(sizeof(map_value_type));
    const auto bucket_bytes = static_cast<std::uint64_t>(map->entries.bucket_count()) *
                              static_cast<std::uint64_t>(sizeof(map_bucket_type));
    return object_bytes + dense_bytes + bucket_bytes;
}

KIMIX_FFI_END
