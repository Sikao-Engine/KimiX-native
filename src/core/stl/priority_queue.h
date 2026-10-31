#pragma once

/*
 * priority_queue.h — kimix::priority_queue<T, Compare>.
 *
 * kimix::priority_queue<T, Compare> is std::priority_queue<T, kimix::vector<T>,
 * Compare> — Compare is the SECOND parameter, exactly as the standalone header
 * has always spelled it (a caller that passes a custom comparator keeps working).
 *
 * THIRD-PARTY LEG (KIMIX_EXTERNAL_THIRDPARTY)
 *   kimix::priority_queue<T, Compare> is
 *   luisa::priority_queue<T, kimix::vector<T>, Compare> — the same std:: adapter
 *   over LC's vector (eastl::vector, the one mimalloc heap). The template
 *   parameter ORDER stays kimix's (T, Compare) rather than LC's
 *   (T, Container, Compare); kimix::vector fills the container slot.
 */

#include "vector.h"

#include <functional>
#include <queue>

#if defined(KIMIX_EXTERNAL_THIRDPARTY)
#include <luisa/core/stl/priority_queue.h>
#endif

namespace kimix {

// ---------------------------------------------------------------------------
// priority_queue using kimix::vector
// ---------------------------------------------------------------------------

#if defined(KIMIX_EXTERNAL_THIRDPARTY)
template <typename T, typename Compare = std::less<T>>
using priority_queue = luisa::priority_queue<T, vector<T>, Compare>;
#else
template <typename T, typename Compare = std::less<T>>
using priority_queue = std::priority_queue<T, vector<T>, Compare>;
#endif

} // namespace kimix