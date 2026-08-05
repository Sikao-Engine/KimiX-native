#pragma once

#include <variant>

namespace kimix {

template <typename... Ts>
using variant = std::variant<Ts...>;

} // namespace kimix
