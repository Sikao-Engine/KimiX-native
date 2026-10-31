#pragma once

// Convenience include — all standard type traits available via std:: prefix.
#include <type_traits>

// Third-party leg: LC's type-trait door (luisa::is_constructible_v etc.). It also
// pulls the EASTL type-traits header, which is what the EASTL containers in this
// leg expect; nothing here shadows the std names.
#if defined(KIMIX_EXTERNAL_THIRDPARTY)
#include <luisa/core/stl/type_traits.h>
#endif
