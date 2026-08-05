#pragma once

#include "memory.h"
#include <deque>

namespace kimix {

// ---------------------------------------------------------------------------
// deque with mimalloc allocator
// ---------------------------------------------------------------------------

template <typename T>
using deque = std::deque<T, allocator<T>>;

} // namespace kimix
