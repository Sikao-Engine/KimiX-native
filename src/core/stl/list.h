#pragma once

/*
 * list.h — kimix::list<T> (mimalloc-backed std::list).
 *
 * kimix::list<T> is std::list<T, kimix::allocator<T>>.
 *
 * THIRD-PARTY LEG (KIMIX_EXTERNAL_THIRDPARTY)
 *   kimix::list<T> is luisa::list<T> — the same std::list over LC's allocator
 *   (the one mimalloc heap).
 */

#include "memory.h"

#include <list>

#if defined(KIMIX_EXTERNAL_THIRDPARTY)
#include <luisa/core/stl/list.h>
#endif

namespace kimix {

// ---------------------------------------------------------------------------
// list with mimalloc allocator
// ---------------------------------------------------------------------------

#if defined(KIMIX_EXTERNAL_THIRDPARTY)
template <typename T>
using list = luisa::list<T>;
#else
template <typename T>
using list = std::list<T, allocator<T>>;
#endif

} // namespace kimix