#pragma once

// Convenience include — all standard algorithms available via std:: prefix.
#include <algorithm>

// Third-party leg: LC's algorithm door (std::sort/swap/transform re-exports plus
// luisa::sort over its own pdqsort). kimix::sort below stays the preferred entry
// point in kimix code; both resolve to the same upstream pdqsort implementation.
#if defined(KIMIX_EXTERNAL_THIRDPARTY)
#include <luisa/core/stl/algorithm.h>
#endif

// kimix::sort (pdqsort) is this project's preferred sorting entry point over
// std::sort; it is declared in stl/pdqsort.h (see the comment there for the
// benchmark behind it) and re-exposed here so "algorithm-like" includes get it.
#include "pdqsort.h" // same directory (src/core/stl/)
