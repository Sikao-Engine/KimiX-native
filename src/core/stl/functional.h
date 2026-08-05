#pragma once

#include <functional>
#include <utility>

namespace kimix {

// Standard function wrappers
template <typename Signature>
using function = std::function<Signature>;

#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L
template <typename Signature>
using move_only_function = std::move_only_function<Signature>;
#else
// Fallback to std::function for older standard library versions
template <typename Signature>
using move_only_function = std::function<Signature>;
#endif

} // namespace kimix
