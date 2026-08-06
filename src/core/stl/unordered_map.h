#pragma once

#include "hash.h"
#include "memory.h"
#include "vector.h"
#include "functional.h"
#include "unordered_dense.h"

namespace kimix {

// ---------------------------------------------------------------------------
// unordered_map — backed by ankerl::unordered_dense::map (robin-hood backward
// shift deletion, densely stored). Uses kimix::hash, kimix::allocator
// (mimalloc) for buckets, and kimix::vector as the value container.
// ---------------------------------------------------------------------------

template <typename Key, typename Value,
          typename Hash = hash<Key>,
          typename KeyEqual = std::equal_to<>>
using unordered_map = ankerl::unordered_dense::map<
    Key, Value, Hash, KeyEqual,
    allocator<std::pair<Key, Value>>,
    vector<std::pair<Key, Value>>>;

// ---------------------------------------------------------------------------
// unordered_set — backed by ankerl::unordered_dense::set. Uses kimix::hash,
// kimix::allocator (mimalloc) for buckets, and kimix::vector as the value
// container.
// ---------------------------------------------------------------------------

template <typename Key,
          typename Hash = hash<Key>,
          typename KeyEqual = std::equal_to<>>
using unordered_set = ankerl::unordered_dense::set<
    Key, Hash, KeyEqual,
    allocator<Key>,
    vector<Key>>;

} // namespace kimix
