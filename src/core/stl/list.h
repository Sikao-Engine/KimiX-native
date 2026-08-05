#pragma once

#include "memory.h"
#include <list>

namespace kimix {

// ---------------------------------------------------------------------------
// list with mimalloc allocator
// ---------------------------------------------------------------------------

template <typename T>
using list = std::list<T, allocator<T>>;

} // namespace kimix
