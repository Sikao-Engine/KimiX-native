#pragma once

#include <concepts>
#include <type_traits>

namespace kimix {

// ---------------------------------------------------------------------------
// Basic C++20 concept definitions
// ---------------------------------------------------------------------------

template <typename T>
concept arithmetic = std::is_arithmetic_v<T>;

template <typename T>
concept floating_point = std::is_floating_point_v<T>;

template <typename T>
concept integral = std::is_integral_v<T>;

template <typename T>
concept signed_integral = std::is_integral_v<T> && std::is_signed_v<T>;

template <typename T>
concept unsigned_integral = std::is_integral_v<T> && std::is_unsigned_v<T>;

template <typename T>
concept boolean = std::is_same_v<T, bool>;

template <typename T>
concept enum_type = std::is_enum_v<T>;

template <typename T>
concept pointer_type = std::is_pointer_v<T>;

template <typename T, typename U>
concept same_as = std::same_as<T, U>;

template <typename Derived, typename Base>
concept derived_from = std::derived_from<Derived, Base>;

template <typename From, typename To>
concept convertible_to = std::convertible_to<From, To>;

template <typename T>
concept destructible = std::destructible<T>;

template <typename T, typename... Args>
concept constructible_from = std::constructible_from<T, Args...>;

template <typename T>
concept default_initializable = std::default_initializable<T>;

template <typename T>
concept move_constructible = std::move_constructible<T>;

template <typename T>
concept copy_constructible = std::copy_constructible<T>;

template <typename T>
concept equality_comparable = std::equality_comparable<T>;

template <typename T>
concept totally_ordered = std::totally_ordered<T>;

template <typename T, typename U>
concept assignable_from = std::assignable_from<T, U>;

} // namespace kimix
