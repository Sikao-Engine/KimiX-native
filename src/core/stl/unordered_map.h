#pragma once

#include "hash.h"
#include "memory.h"

#include <unordered_map>
#include <unordered_set>
#include <functional>

namespace kimix {

// ---------------------------------------------------------------------------
// unordered_map with kimix::hash and mimalloc allocator
// ---------------------------------------------------------------------------

template <typename Key, typename Value,
          typename Hash = hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
using unordered_map = std::unordered_map<
    Key, Value,
    Hash, KeyEqual,
    allocator<std::pair<const Key, Value>>
>;

// ---------------------------------------------------------------------------
// unordered_set with kimix::hash and mimalloc allocator
// ---------------------------------------------------------------------------

template <typename T,
          typename Hash = hash<T>,
          typename KeyEqual = std::equal_to<T>>
using unordered_set = std::unordered_set<
    T,
    Hash, KeyEqual,
    allocator<T>
>;

} // namespace kimix
