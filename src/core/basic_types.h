/*
 * basic_types.h — kimix::Vector<T,N> and kimix::Matrix<T,N> with type aliases.
 *
 * kimix::Vector<T,N>:
 *   A fixed-size, aligned vector type. Supports construction from a
 *   single fill-value, from N individual arguments, or default (zeroed).
 *   Access elements via operator[] or named accessors (x(), y(), z(), w()).
 *   Arithmetic operators: +, -, *, /, unary -, compound +=/-=/*=/=.
 *   Comparison operators produce kimix::Vector<bool,N> (element-wise).
 *   Logical operators || and && on bool vectors.
 *   Free functions: kimix::any(v), kimix::all(v), kimix::none(v).
 *
 *   Type aliases:
 *     kimix::float2/3/4 (2-4 component float vectors, aligned)
 *     kimix::double2/3/4
 *     kimix::int2/3/4
 *     kimix::uint2/3/4
 *     kimix::short2/3/4
 *     kimix::ushort2/3/4
 *     kimix::bool2/3/4
 *
 *   sizeof guarantees:
 *     kimix::float4 = 16 bytes, alignof = 16
 *     kimix::float3 = 16 bytes (12 + padding), alignof = 16
 *     kimix::float2 = 8 bytes, alignof = 8
 *
 * kimix::Matrix<T, N>:
 *   An N×N column-major matrix. Construct with a single value for
 *   identity diagonal (explicit Matrix(1.0f)) or default (zeroed).
 *   Access columns via operator[] returning kimix::Vector<T,N>.
 *   Matrix * Vector multiplication, Matrix * Matrix multiplication,
 *   Matrix + Matrix / Matrix - Matrix arithmetic.
 *
 * Example:
 *   kimix::float4 v(1.0f, 2.0f, 3.0f, 4.0f);
 *   auto r = v + kimix::float4(4.0f, 5.0f, 6.0f, 7.0f);
 *   kimix::Matrix<float, 3> m(1.0f);  // identity
 *   kimix::float3 result = m * v;
 */
#pragma once

#include "basic_traits.h"
#include "stl/hash.h"
#include "stl/memory.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <type_traits>
#include <utility>
#include <algorithm>
#include <cassert>

namespace kimix {

// ---------------------------------------------------------------------------
// Vector storage and alignment
// ---------------------------------------------------------------------------

namespace detail {

template <typename T, size_t N>
struct vector_alignment {
    static constexpr size_t value = [] {
        if constexpr (sizeof(T) * N <= 4) return 4;
        if constexpr (sizeof(T) * N <= 8) return 8;
        if constexpr (sizeof(T) * N <= 16) return 16;
        return 32;
    }();
};

template <typename T, size_t N>
inline constexpr size_t vector_alignment_v = vector_alignment<T, N>::value;

template <typename T, size_t N>
struct alignas(vector_alignment_v<T, N>) VectorStorage {
    T _[N];

    constexpr VectorStorage() noexcept : _{} {}
    constexpr VectorStorage(const VectorStorage&) noexcept = default;
    constexpr VectorStorage& operator=(const VectorStorage&) noexcept = default;

    explicit constexpr VectorStorage(T fill) noexcept {
        for (size_t i = 0; i < N; ++i) { _[i] = fill; }
    }

    template <typename... Args>
        requires (sizeof...(Args) == N)
    explicit constexpr VectorStorage(Args... args) noexcept : _{static_cast<T>(args)...} {}

    constexpr const T& operator[](size_t i) const noexcept { return _[i]; }
    constexpr T& operator[](size_t i) noexcept { return _[i]; }

    static constexpr size_t size() noexcept { return N; }
};

} // namespace detail

// ---------------------------------------------------------------------------
// Vector<T, N>
// ---------------------------------------------------------------------------

template <typename T, size_t N>
struct Vector : detail::VectorStorage<T, N> {
    using value_type = T;
    using storage = detail::VectorStorage<T, N>;
    static constexpr size_t dimension = N;

    using storage::storage;
    using storage::operator[];

    // Accessors
    constexpr T& x() noexcept requires (N >= 1) { return storage::operator[](0); }
    constexpr const T& x() const noexcept requires (N >= 1) { return storage::operator[](0); }
    constexpr T& y() noexcept requires (N >= 2) { return storage::operator[](1); }
    constexpr const T& y() const noexcept requires (N >= 2) { return storage::operator[](1); }
    constexpr T& z() noexcept requires (N >= 3) { return storage::operator[](2); }
    constexpr const T& z() const noexcept requires (N >= 3) { return storage::operator[](2); }
    constexpr T& w() noexcept requires (N >= 4) { return storage::operator[](3); }
    constexpr const T& w() const noexcept requires (N >= 4) { return storage::operator[](3); }

    // Unary operators
    constexpr Vector operator+() const noexcept {
        Vector result;
        for (size_t i = 0; i < N; ++i) { result[i] = +(*this)[i]; }
        return result;
    }

    constexpr Vector operator-() const noexcept {
        Vector result;
        for (size_t i = 0; i < N; ++i) { result[i] = -(*this)[i]; }
        return result;
    }

    constexpr Vector operator~() const noexcept requires std::is_integral_v<T> {
        Vector result;
        for (size_t i = 0; i < N; ++i) { result[i] = ~(*this)[i]; }
        return result;
    }

    constexpr Vector operator!() const noexcept {
        Vector result;
        for (size_t i = 0; i < N; ++i) { result[i] = !(*this)[i]; }
        return result;
    }
};

// ---------------------------------------------------------------------------
// Vector binary operators (scalar + vector, vector + scalar, vector + vector)
// ---------------------------------------------------------------------------

#define KIMIX_VECTOR_BINARY_OP(op)                                                        \
    template <typename T, size_t N>                                                    \
    constexpr Vector<T, N> operator op(const Vector<T, N>& a, const Vector<T, N>& b) { \
        Vector<T, N> result;                                                           \
        for (size_t i = 0; i < N; ++i) { result[i] = a[i] op b[i]; }                  \
        return result;                                                                 \
    }                                                                                  \
    template <typename T, size_t N, typename U>                                        \
        requires std::is_convertible_v<U, T>                                           \
    constexpr Vector<T, N> operator op(const Vector<T, N>& a, U b) {                   \
        Vector<T, N> result;                                                           \
        for (size_t i = 0; i < N; ++i) { result[i] = a[i] op static_cast<T>(b); }     \
        return result;                                                                 \
    }                                                                                  \
    template <typename T, size_t N, typename U>                                        \
        requires std::is_convertible_v<U, T>                                           \
    constexpr Vector<T, N> operator op(U a, const Vector<T, N>& b) {                   \
        Vector<T, N> result;                                                           \
        for (size_t i = 0; i < N; ++i) { result[i] = static_cast<T>(a) op b[i]; }     \
        return result;                                                                 \
    }

#define KIMIX_VECTOR_COMPOUND_OP(op)                                                      \
    template <typename T, size_t N>                                                    \
    constexpr Vector<T, N>& operator op(Vector<T, N>& a, const Vector<T, N>& b) {     \
        for (size_t i = 0; i < N; ++i) { a[i] op b[i]; }                             \
        return a;                                                                      \
    }                                                                                  \
    template <typename T, size_t N, typename U>                                        \
        requires std::is_convertible_v<U, T>                                           \
    constexpr Vector<T, N>& operator op(Vector<T, N>& a, U b) {                       \
        for (size_t i = 0; i < N; ++i) { a[i] op static_cast<T>(b); }                \
        return a;                                                                      \
    }

#define KIMIX_VECTOR_COMPARISON_OP(op)                                                    \
    template <typename T, size_t N>                                                    \
    constexpr Vector<bool, N> operator op(const Vector<T, N>& a, const Vector<T, N>& b) { \
        Vector<bool, N> result;                                                        \
        for (size_t i = 0; i < N; ++i) { result[i] = a[i] op b[i]; }                 \
        return result;                                                                 \
    }                                                                                  \
    template <typename T, size_t N, typename U>                                        \
        requires std::is_convertible_v<U, T>                                           \
    constexpr Vector<bool, N> operator op(const Vector<T, N>& a, U b) {               \
        Vector<bool, N> result;                                                        \
        for (size_t i = 0; i < N; ++i) { result[i] = a[i] op static_cast<T>(b); }    \
        return result;                                                                 \
    }                                                                                  \
    template <typename T, size_t N, typename U>                                        \
        requires std::is_convertible_v<U, T>                                           \
    constexpr Vector<bool, N> operator op(U a, const Vector<T, N>& b) {               \
        Vector<bool, N> result;                                                        \
        for (size_t i = 0; i < N; ++i) { result[i] = static_cast<T>(a) op b[i]; }    \
        return result;                                                                 \
    }

KIMIX_VECTOR_BINARY_OP(+)
KIMIX_VECTOR_BINARY_OP(-)
KIMIX_VECTOR_BINARY_OP(*)
KIMIX_VECTOR_BINARY_OP(/)
KIMIX_VECTOR_BINARY_OP(%)
KIMIX_VECTOR_BINARY_OP(&)
KIMIX_VECTOR_BINARY_OP(|)
KIMIX_VECTOR_BINARY_OP(^)

KIMIX_VECTOR_COMPOUND_OP(+=)
KIMIX_VECTOR_COMPOUND_OP(-=)
KIMIX_VECTOR_COMPOUND_OP(*=)
KIMIX_VECTOR_COMPOUND_OP(/=)
KIMIX_VECTOR_COMPOUND_OP(%=)
KIMIX_VECTOR_COMPOUND_OP(&=)
KIMIX_VECTOR_COMPOUND_OP(|=)
KIMIX_VECTOR_COMPOUND_OP(^=)

KIMIX_VECTOR_COMPARISON_OP(<)
KIMIX_VECTOR_COMPARISON_OP(>)
KIMIX_VECTOR_COMPARISON_OP(<=)
KIMIX_VECTOR_COMPARISON_OP(>=)
KIMIX_VECTOR_COMPARISON_OP(==)
KIMIX_VECTOR_COMPARISON_OP(!=)

#undef KIMIX_VECTOR_BINARY_OP
#undef KIMIX_VECTOR_COMPOUND_OP
#undef KIMIX_VECTOR_COMPARISON_OP

// Logical operators on bool vectors
template <size_t N>
constexpr Vector<bool, N> operator||(const Vector<bool, N>& a, const Vector<bool, N>& b) {
    Vector<bool, N> result;
    for (size_t i = 0; i < N; ++i) { result[i] = a[i] || b[i]; }
    return result;
}

template <size_t N>
constexpr Vector<bool, N> operator&&(const Vector<bool, N>& a, const Vector<bool, N>& b) {
    Vector<bool, N> result;
    for (size_t i = 0; i < N; ++i) { result[i] = a[i] && b[i]; }
    return result;
}

// ---------------------------------------------------------------------------
// any / all / none for bool vectors
// ---------------------------------------------------------------------------

template <size_t N>
constexpr bool any(const Vector<bool, N>& v) noexcept {
    for (size_t i = 0; i < N; ++i) { if (v[i]) return true; }
    return false;
}

template <size_t N>
constexpr bool all(const Vector<bool, N>& v) noexcept {
    for (size_t i = 0; i < N; ++i) { if (!v[i]) return false; }
    return true;
}

template <size_t N>
constexpr bool none(const Vector<bool, N>& v) noexcept {
    return !any(v);
}

// ---------------------------------------------------------------------------
// hash<Vector<T, N>>
// ---------------------------------------------------------------------------

template <typename T, size_t N>
struct hash<Vector<T, N>> {
    uint64_t operator()(const Vector<T, N>& v, uint64_t seed = hash64_default_seed) const noexcept {
        return hash64(&v, sizeof(v), seed);
    }
};

// ---------------------------------------------------------------------------
// Matrix<T, N>
// ---------------------------------------------------------------------------

template <typename T, size_t N>
struct Matrix {
    using value_type = T;
    static constexpr size_t dimension = N;

    Vector<T, N> cols[N];

    constexpr Matrix() noexcept : cols{} {}

    explicit constexpr Matrix(T diag) noexcept {
        for (size_t i = 0; i < N; ++i) {
            for (size_t j = 0; j < N; ++j) {
                cols[j][i] = (i == j) ? diag : static_cast<T>(0);
            }
        }
    }

    constexpr Vector<T, N>& operator[](size_t col) noexcept { return cols[col]; }
    constexpr const Vector<T, N>& operator[](size_t col) const noexcept { return cols[col]; }

    // Matrix multiplication
    constexpr Matrix operator*(const Matrix& other) const noexcept {
        Matrix result;
        for (size_t i = 0; i < N; ++i) {
            for (size_t j = 0; j < N; ++j) {
                T sum = static_cast<T>(0);
                for (size_t k = 0; k < N; ++k) {
                    sum += cols[k][i] * other.cols[j][k];
                }
                result[j][i] = sum;
            }
        }
        return result;
    }

    // Matrix * Vector
    constexpr Vector<T, N> operator*(const Vector<T, N>& v) const noexcept {
        Vector<T, N> result;
        for (size_t i = 0; i < N; ++i) {
            T sum = static_cast<T>(0);
            for (size_t k = 0; k < N; ++k) {
                sum += cols[k][i] * v[k];
            }
            result[i] = sum;
        }
        return result;
    }
};

// Matrix arithmetic
template <typename T, size_t N>
constexpr Matrix<T, N> operator+(const Matrix<T, N>& a, const Matrix<T, N>& b) noexcept {
    Matrix<T, N> result;
    for (size_t i = 0; i < N; ++i) {
        result[i] = a[i] + b[i];
    }
    return result;
}

template <typename T, size_t N>
constexpr Matrix<T, N> operator-(const Matrix<T, N>& a, const Matrix<T, N>& b) noexcept {
    Matrix<T, N> result;
    for (size_t i = 0; i < N; ++i) {
        result[i] = a[i] - b[i];
    }
    return result;
}

// hash<Matrix<T, N>>
template <typename T, size_t N>
struct hash<Matrix<T, N>> {
    uint64_t operator()(const Matrix<T, N>& m, uint64_t seed = hash64_default_seed) const noexcept {
        return hash64(&m, sizeof(m), seed);
    }
};

// ---------------------------------------------------------------------------
// Type aliases
// ---------------------------------------------------------------------------

// bool vectors
using bool2 = Vector<bool, 2>;
using bool3 = Vector<bool, 3>;
using bool4 = Vector<bool, 4>;

// float vectors
using float2 = Vector<float, 2>;
using float3 = Vector<float, 3>;
using float4 = Vector<float, 4>;

// int vectors
using int2 = Vector<int32_t, 2>;
using int3 = Vector<int32_t, 3>;
using int4 = Vector<int32_t, 4>;

// uint vectors
using uint2 = Vector<uint32_t, 2>;
using uint3 = Vector<uint32_t, 3>;
using uint4 = Vector<uint32_t, 4>;

// short vectors
using short2 = Vector<int16_t, 2>;
using short3 = Vector<int16_t, 3>;
using short4 = Vector<int16_t, 4>;

// ushort vectors
using ushort2 = Vector<uint16_t, 2>;
using ushort3 = Vector<uint16_t, 3>;
using ushort4 = Vector<uint16_t, 4>;

// byte vectors
using byte2 = Vector<int8_t, 2>;
using byte3 = Vector<int8_t, 3>;
using byte4 = Vector<int8_t, 4>;

// ubyte vectors
using ubyte2 = Vector<uint8_t, 2>;
using ubyte3 = Vector<uint8_t, 3>;
using ubyte4 = Vector<uint8_t, 4>;

// double vectors
using double2 = Vector<double, 2>;
using double3 = Vector<double, 3>;
using double4 = Vector<double, 4>;

// long vectors
using long2 = Vector<int64_t, 2>;
using long3 = Vector<int64_t, 3>;
using long4 = Vector<int64_t, 4>;

// ulong vectors
using ulong2 = Vector<uint64_t, 2>;
using ulong3 = Vector<uint64_t, 3>;
using ulong4 = Vector<uint64_t, 4>;

// half vectors
using half2 = Vector<half, 2>;
using half3 = Vector<half, 3>;
using half4 = Vector<half, 4>;

// Matrices (float only)
using float2x2 = Matrix<float, 2>;
using float3x3 = Matrix<float, 3>;
using float4x4 = Matrix<float, 4>;

// ---------------------------------------------------------------------------
// basic_types tuple
// ---------------------------------------------------------------------------

using basic_types = std::tuple<
    float, double,
    int8_t, uint8_t,
    int16_t, uint16_t,
    int32_t, uint32_t,
    int64_t, uint64_t,
    bool,
    half
>;

} // namespace kimix
