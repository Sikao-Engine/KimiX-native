#pragma once

#include "vector.h"
#include <queue>

namespace kimix {

// ---------------------------------------------------------------------------
// priority_queue using kimix::vector
// ---------------------------------------------------------------------------

template <typename T, typename Compare = std::less<T>>
using priority_queue = std::priority_queue<T, vector<T>, Compare>;

} // namespace kimix
