#pragma once

#include "deque.h"
#include <stack>

namespace kimix {

// ---------------------------------------------------------------------------
// stack using kimix::deque
// ---------------------------------------------------------------------------

template <typename T>
using stack = std::stack<T, deque<T>>;

} // namespace kimix
