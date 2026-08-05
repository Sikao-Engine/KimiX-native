#pragma once

#include "deque.h"
#include <queue>

namespace kimix {

// ---------------------------------------------------------------------------
// queue using kimix::deque
// ---------------------------------------------------------------------------

template <typename T>
using queue = std::queue<T, deque<T>>;

} // namespace kimix
