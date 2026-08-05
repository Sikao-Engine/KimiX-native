#pragma once

// unordered_dense — alias for kimix::unordered_map.
// For a true high-performance dense hash map, consider integrating
// boost::unordered_flat_map or ankerl::unordered_dense.

#include "unordered_map.h"

namespace kimix {

template <typename Key, typename Value,
          typename Hash = hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
using unordered_dense = unordered_map<Key, Value, Hash, KeyEqual>;

} // namespace kimix
