#pragma once

#include "memory.h"
#include <map>
#include <set>
#include <functional>

namespace kimix {

// ---------------------------------------------------------------------------
// map with mimalloc allocator
// ---------------------------------------------------------------------------

template <typename Key, typename Value,
          typename Compare = std::less<Key>>
using map = std::map<
    Key, Value,
    Compare,
    allocator<std::pair<const Key, Value>>
>;

// ---------------------------------------------------------------------------
// set with mimalloc allocator
// ---------------------------------------------------------------------------

template <typename T,
          typename Compare = std::less<T>>
using set = std::set<
    T,
    Compare,
    allocator<T>
>;

} // namespace kimix
