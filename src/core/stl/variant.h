#pragma once

/*
 * variant.h — kimix::variant<Ts...> and the access helpers.
 *
 * Standalone: std::variant.
 *
 * THIRD-PARTY LEG (KIMIX_EXTERNAL_THIRDPARTY)
 *   kimix::variant<Ts...> is luisa::variant<Ts...> (eastl::variant) and
 *   kimix::get / get_if / holds_alternative / visit / monostate are LC's, so one
 *   variant implementation serves both libraries. The names below are re-exported
 *   in the standalone leg too, so a call site can say kimix::get / kimix::visit
 *   once and compile in both legs.
 */

#include <variant>

#if defined(KIMIX_EXTERNAL_THIRDPARTY)
#include <luisa/core/stl/variant.h>
#endif

namespace kimix {

#if defined(KIMIX_EXTERNAL_THIRDPARTY)

template <typename... Ts>
using variant = luisa::variant<Ts...>;

using luisa::get;
using luisa::get_if;
using luisa::holds_alternative;
using luisa::monostate;
using luisa::visit;

#else

template <typename... Ts>
using variant = std::variant<Ts...>;

using std::get;
using std::get_if;
using std::holds_alternative;
using std::monostate;
using std::visit;

#endif

} // namespace kimix