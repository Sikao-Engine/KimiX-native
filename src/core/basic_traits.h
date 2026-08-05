#pragma once

#include "stl/memory.h"
#include "stl/type_traits.h"

#include <cstdint>
#include <type_traits>
#include <concepts>

namespace kimix {

// ---------------------------------------------------------------------------
// always_false / always_true
// ---------------------------------------------------------------------------

template <typename...>
inline constexpr bool always_false_v = false;

template <typename... T>
struct always_false : std::false_type {};

template <typename...>
inline constexpr bool always_true_v = true;

template <typename... T>
struct always_true : std::true_type {};

// ---------------------------------------------------------------------------
// to_underlying
// ---------------------------------------------------------------------------

template <typename E>
    requires std::is_enum_v<E>
constexpr auto to_underlying(E e) noexcept {
    return static_cast<std::underlying_type_t<E>>(e);
}

// ---------------------------------------------------------------------------
// half type (placeholder — use float until half.hpp is vendored)
// ---------------------------------------------------------------------------

using half = float;
// half is currently an alias of float; guard duplicate explicit instantiations
#define KIMIX_HALF_IS_DISTINCT 0

// ---------------------------------------------------------------------------
// Basic type aliases
// ---------------------------------------------------------------------------

using byte = int8_t;
using ubyte = uint8_t;

using ushort = uint16_t;
using uint = uint32_t;

using slong = long long;
using ulong = unsigned long long;

// ---------------------------------------------------------------------------
// Type classification
// ---------------------------------------------------------------------------

template <typename T>
struct is_integral : std::is_integral<T> {};

template <typename T>
inline constexpr bool is_integral_v = is_integral<T>::value;

template <typename T>
struct is_floating_point : std::is_floating_point<T> {};

template <typename T>
inline constexpr bool is_floating_point_v = is_floating_point<T>::value;

template <typename T>
struct is_boolean : std::is_same<T, bool> {};

template <typename T>
inline constexpr bool is_boolean_v = is_boolean<T>::value;

template <typename T>
struct is_arithmetic : std::is_arithmetic<T> {};

template <typename T>
inline constexpr bool is_arithmetic_v = is_arithmetic<T>::value;

template <typename T>
struct is_signed : std::is_signed<T> {};

template <typename T>
inline constexpr bool is_signed_v = is_signed<T>::value;

template <typename T>
struct is_unsigned : std::is_unsigned<T> {};

template <typename T>
inline constexpr bool is_unsigned_v = is_unsigned<T>::value;

// ---------------------------------------------------------------------------
// Forward declarations of Vector and Matrix
// ---------------------------------------------------------------------------

template <typename T, size_t N>
struct Vector;

template <typename T, size_t N>
struct Matrix;

// ---------------------------------------------------------------------------
// is_vector / is_matrix traits
// ---------------------------------------------------------------------------

namespace detail {

template <typename T>
struct is_vector_impl : std::false_type {};

template <typename T, size_t N>
struct is_vector_impl<Vector<T, N>> : std::true_type {};

template <typename T>
struct is_matrix_impl : std::false_type {};

template <typename T, size_t N>
struct is_matrix_impl<Matrix<T, N>> : std::true_type {};

} // namespace detail

template <typename T>
struct is_vector : detail::is_vector_impl<std::remove_cvref_t<T>> {};

template <typename T>
inline constexpr bool is_vector_v = is_vector<T>::value;

template <typename T>
struct is_matrix : detail::is_matrix_impl<std::remove_cvref_t<T>> {};

template <typename T>
inline constexpr bool is_matrix_v = is_matrix<T>::value;

// ---------------------------------------------------------------------------
// Vector element type and dimension extractors
// ---------------------------------------------------------------------------

namespace detail {

template <typename T>
struct vector_traits;

template <typename T, size_t N>
struct vector_traits<Vector<T, N>> {
    using element_type = T;
    static constexpr size_t dimension = N;
};

} // namespace detail

template <typename T>
    requires is_vector_v<T>
using vector_element_t = typename detail::vector_traits<std::remove_cvref_t<T>>::element_type;

template <typename T>
    requires is_vector_v<T>
inline constexpr size_t vector_dimension_v = detail::vector_traits<std::remove_cvref_t<T>>::dimension;

// ---------------------------------------------------------------------------
// _or_vector variants — true if T is the given type or a vector thereof
// ---------------------------------------------------------------------------

template <typename T>
using is_integral_or_vector = std::disjunction<is_integral<T>, is_vector<T>>;

template <typename T>
inline constexpr bool is_integral_or_vector_v = is_integral_or_vector<T>::value;

template <typename T>
using is_floating_point_or_vector = std::disjunction<is_floating_point<T>, is_vector<T>>;

template <typename T>
inline constexpr bool is_floating_point_or_vector_v = is_floating_point_or_vector<T>::value;

template <typename T>
using is_arithmetic_or_vector = std::disjunction<is_arithmetic<T>, is_vector<T>>;

template <typename T>
inline constexpr bool is_arithmetic_or_vector_v = is_arithmetic_or_vector<T>::value;

template <typename T>
using is_boolean_or_vector = std::disjunction<is_boolean<T>, is_vector<T>>;

template <typename T>
inline constexpr bool is_boolean_or_vector_v = is_boolean_or_vector<T>::value;

template <typename T>
using is_signed_or_vector = std::disjunction<is_signed<T>, is_vector<T>>;

template <typename T>
inline constexpr bool is_signed_or_vector_v = is_signed_or_vector<T>::value;

template <typename T>
using is_unsigned_or_vector = std::disjunction<is_unsigned<T>, is_vector<T>>;

template <typename T>
inline constexpr bool is_unsigned_or_vector_v = is_unsigned_or_vector<T>::value;

} // namespace kimix
