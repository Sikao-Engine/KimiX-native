#pragma once

/*
 * stack.h — kimix::stack<T> (LIFO adapter over kimix::deque).
 *
 * kimix::stack<T> is std::stack<T, kimix::deque<T>>.
 *
 * THIRD-PARTY LEG (KIMIX_EXTERNAL_THIRDPARTY)
 *   kimix::stack<T> is luisa::stack<T>, i.e. std::stack over luisa::vector (the
 *   container LC's own stack uses; kimix::vector is that same type in this leg).
 *   push/top/pop/empty/size are unchanged — only the backing container differs,
 *   which LC's adapter fixes, not kimix.
 */

#include "deque.h"
#include "vector.h"

#include <stack>

#if defined(KIMIX_EXTERNAL_THIRDPARTY)
#include <luisa/core/stl/stack.h>
#endif

namespace kimix {

// ---------------------------------------------------------------------------
// stack using kimix::deque (standalone) / LC's vector-backed stack (external)
// ---------------------------------------------------------------------------

#if defined(KIMIX_EXTERNAL_THIRDPARTY)
template <typename T>
using stack = luisa::stack<T>;
#else
template <typename T>
using stack = std::stack<T, deque<T>>;
#endif

} // namespace kimix