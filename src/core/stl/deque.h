#pragma once

/*
 * deque.h — kimix::deque<T> (mimalloc-backed std::deque).
 *
 * kimix::deque<T> is std::deque<T, kimix::allocator<T>>.
 *
 * THIRD-PARTY LEG (KIMIX_EXTERNAL_THIRDPARTY)
 *   kimix::deque<T> is luisa::deque<T> — the same std::deque, now over LC's
 *   allocator (the one mimalloc heap). Same container, same API, one allocator
 *   family instead of two.
 */

#include "memory.h"

#include <deque>

#if defined(KIMIX_EXTERNAL_THIRDPARTY)
#include <luisa/core/stl/deque.h>
#endif

namespace kimix {

// ---------------------------------------------------------------------------
// deque with mimalloc allocator
// ---------------------------------------------------------------------------

#if defined(KIMIX_EXTERNAL_THIRDPARTY)
template <typename T>
using deque = luisa::deque<T>;
#else
template <typename T>
using deque = std::deque<T, allocator<T>>;
#endif

} // namespace kimix