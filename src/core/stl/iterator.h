#pragma once

// Convenience include — all standard iterator types available via std:: prefix.
#include <iterator>

// Third-party leg: LC's iterator door (luisa::range / default_sentinel on top of
// the standard iterators). Nothing here shadows the std names.
#if defined(KIMIX_EXTERNAL_THIRDPARTY)
#include <luisa/core/stl/iterator.h>
#endif
