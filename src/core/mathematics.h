#pragma once

#include "constants.h"

#include <cmath>
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <type_traits>
#include <concepts>
#include <numbers>

namespace kimix {

// ---------------------------------------------------------------------------
// next_pow2 / is_pow2
// ---------------------------------------------------------------------------

template <std::integral T>
constexpr T next_pow2(T v) noexcept {
    if (v <= 1) { return 1; }
    if constexpr (sizeof(T) <= 4) {
        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        return v + 1;
    } else {
        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v |= v >> 32;
        return v + 1;
    }
}

template <std::integral T>
constexpr bool is_pow2(T v) noexcept {
    return v > 0 && (v & (v - 1)) == 0;
}

// ---------------------------------------------------------------------------
// clamp
// ---------------------------------------------------------------------------

template <typename T>
constexpr const T& clamp(const T& value, const T& lo, const T& hi) noexcept {
    return value < lo ? lo : (value > hi ? hi : value);
}

// ---------------------------------------------------------------------------
// lerp (linear interpolation)
// ---------------------------------------------------------------------------

template <std::floating_point T>
constexpr T lerp(T a, T b, T t) noexcept {
    return a + (b - a) * t;
}

// ---------------------------------------------------------------------------
// min / max
// ---------------------------------------------------------------------------

template <typename T>
constexpr const T& min(const T& a, const T& b) noexcept {
    return (b < a) ? b : a;
}

template <typename T>
constexpr const T& max(const T& a, const T& b) noexcept {
    return (a < b) ? b : a;
}

// ---------------------------------------------------------------------------
// degrees / radians
// ---------------------------------------------------------------------------

template <std::floating_point T>
constexpr T radians(T degrees) noexcept {
    return degrees * static_cast<T>(pi / 180.0);
}

template <std::floating_point T>
constexpr T degrees(T radians) noexcept {
    return radians * static_cast<T>(180.0 / pi);
}

// ---------------------------------------------------------------------------
// sign
// ---------------------------------------------------------------------------

template <typename T>
constexpr int sign(T val) noexcept {
    return (T(0) < val) - (val < T(0));
}

// ---------------------------------------------------------------------------
// floor / ceil for integers
// ---------------------------------------------------------------------------

template <typename T, typename U>
    requires std::integral<T> && std::integral<U>
constexpr T floor_div(T a, U b) noexcept {
    T q = a / static_cast<T>(b);
    T r = a % static_cast<T>(b);
    if (r != 0 && ((a < 0) != (static_cast<T>(b) < 0))) {
        --q;
    }
    return q;
}

template <typename T, typename U>
    requires std::integral<T> && std::integral<U>
constexpr T ceil_div(T a, U b) noexcept {
    T q = a / static_cast<T>(b);
    T r = a % static_cast<T>(b);
    if (r != 0 && ((a < 0) == (static_cast<T>(b) < 0))) {
        ++q;
    }
    return q;
}

// ---------------------------------------------------------------------------
// align (power-of-two alignment)
// ---------------------------------------------------------------------------

template <std::integral T>
constexpr T align_up(T value, T alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

template <std::integral T>
constexpr T align_down(T value, T alignment) noexcept {
    return value & ~(alignment - 1);
}

// ---------------------------------------------------------------------------
// frac (fractional part)
// ---------------------------------------------------------------------------

template <std::floating_point T>
constexpr T frac(T value) noexcept {
    return value - std::floor(value);
}

// ---------------------------------------------------------------------------
// smoothstep
// ---------------------------------------------------------------------------

template <std::floating_point T>
constexpr T smoothstep(T edge0, T edge1, T x) noexcept {
    T t = clamp((x - edge0) / (edge1 - edge0), T(0), T(1));
    return t * t * (T(3) - T(2) * t);
}

// ---------------------------------------------------------------------------
// saturate (clamp to [0, 1])
// ---------------------------------------------------------------------------

template <std::floating_point T>
constexpr T saturate(T x) noexcept {
    return clamp(x, T(0), T(1));
}

} // namespace kimix
