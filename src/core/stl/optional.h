#pragma once

/*
 * optional.h — kimix::optional<T>.
 *
 * Standalone: std::optional (plus kimix::nullopt / kimix::nullopt_t /
 * kimix::make_optional, which are the std names re-exported so a call site can
 * be written once and compile in both legs).
 *
 * THIRD-PARTY LEG (KIMIX_EXTERNAL_THIRDPARTY)
 *   kimix::optional<T> is luisa::optional<T> (eastl::optional). LC adds the
 *   heterogeneous comparison overloads std::optional has, so == against a value
 *   keeps working; code in that leg spells the empty state kimix::nullopt (or
 *   luisa::nullopt) rather than std::nullopt, because eastl::optional only
 *   accepts eastl::nullopt_t.
 */

#include <optional>

#if defined(KIMIX_EXTERNAL_THIRDPARTY)
#include <luisa/core/stl/optional.h>
#endif

namespace kimix {

#if defined(KIMIX_EXTERNAL_THIRDPARTY)

template <typename T>
using optional = luisa::optional<T>;

using luisa::make_optional;
using luisa::nullopt;
using luisa::nullopt_t;

#else

template <typename T>
using optional = std::optional<T>;

using std::make_optional;
using std::nullopt;
using std::nullopt_t;

#endif

} // namespace kimix