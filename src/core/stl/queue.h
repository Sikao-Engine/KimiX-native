#pragma once

/*
 * queue.h — kimix::queue<T> (FIFO adapter over kimix::deque).
 *
 * kimix::queue<T> is std::queue<T, kimix::deque<T>>.
 *
 * THIRD-PARTY LEG (KIMIX_EXTERNAL_THIRDPARTY)
 *   kimix::queue<T> is luisa::queue<T, kimix::deque<T>> (eastl::queue over the
 *   same deque container). Same push/pop/front/back/empty/size surface; the
 *   container is still kimix::deque, i.e. LC's one mimalloc heap.
 */

#include "deque.h"

#include <queue>

#if defined(KIMIX_EXTERNAL_THIRDPARTY)
#include <luisa/core/stl/queue.h>
#endif

namespace kimix {

// ---------------------------------------------------------------------------
// queue using kimix::deque
// ---------------------------------------------------------------------------

#if defined(KIMIX_EXTERNAL_THIRDPARTY)
template <typename T>
using queue = luisa::queue<T, deque<T>>;
#else
template <typename T>
using queue = std::queue<T, deque<T>>;
#endif

} // namespace kimix