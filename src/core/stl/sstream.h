#pragma once

/*
 * sstream.h — kimix::ostringstream / istringstream / stringstream.
 *
 * Standalone: the std streams (CRT allocator).
 *
 * THIRD-PARTY LEG (KIMIX_EXTERNAL_THIRDPARTY)
 *   LC's streams: std::basic_(i|o)?stringstream over luisa::allocator, i.e. the
 *   one mimalloc heap, so a stream buffer is the same kind of block as a
 *   kimix::string. The surface (str(), <<, >>, seekp...) is the standard one.
 */

#include <sstream>

#if defined(KIMIX_EXTERNAL_THIRDPARTY)
#include <luisa/core/stl/sstream.h>
#endif

namespace kimix {

#if defined(KIMIX_EXTERNAL_THIRDPARTY)

using ostringstream = luisa::ostringstream;
using istringstream = luisa::istringstream;
using stringstream = luisa::stringstream;

#else

using ostringstream = std::ostringstream;
using istringstream = std::istringstream;
using stringstream = std::stringstream;

#endif

} // namespace kimix