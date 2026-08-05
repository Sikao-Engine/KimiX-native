/*
 * hash.h — kimix hash functions (hash64, hash_value, hash_combine, Hash128).
 *
 * kimix::hash64(data, size, seed)        — 64-bit hash (xxhash-based)
 * kimix::hash_value(value)                — hash a scalar/pointer value
 * kimix::hash_value(value, seed)          — with custom seed
 * kimix::hash_combine({vals...})          — combine multiple 64-bit hashes
 *                                        (initializer_list or span)
 * kimix::hash128(data, size, seed)        — 128-bit hash (XXH3-based)
 * kimix::Hash128                          — 128-bit hash container
 *   .to_string()                       — 32-char hex string
 *   .data(), .size() (=16), ==, !=
 *
 * kimix::hash<T> specializations:
 *   Arithmetic types, pointers, enums
 *   Types with .hash() method
 *   Vector<T,N>, Matrix<T,N>
 *
 * Example:
 *   auto h = kimix::hash64("hello", 5);
 *   auto combined = kimix::hash_combine({h1, h2, h3});
 *   kimix::Hash128 h128 = kimix::hash128("data", 4);
 *   auto s = h128.to_string();  // "..."
 */
#pragma once

#include "hash_fwd.h"
#include "memory.h"
#include "string.h"
#include "../dll_export.h"

#include <array>
#include <cstdint>
#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <initializer_list>

namespace kimix {

// ---------------------------------------------------------------------------
// hash_value helpers (call hash64 from hash_fwd.h / hash.cpp)
// ---------------------------------------------------------------------------

template <typename T>
inline uint64_t hash_value(T&& value) {
    return hash64(&value, sizeof(T));
}

template <typename T>
inline uint64_t hash_value(T&& value, uint64_t seed) {
    return hash64(&value, sizeof(T), seed);
}

// ---------------------------------------------------------------------------
// hash_combine
// ---------------------------------------------------------------------------

inline uint64_t hash_combine(std::initializer_list<uint64_t> hashes) {
    uint64_t result = hash64_default_seed;
    for (auto h : hashes) {
        result = hash64(&h, sizeof(h), result);
    }
    return result;
}

inline uint64_t hash_combine(std::span<const uint64_t> hashes) {
    uint64_t result = hash64_default_seed;
    for (auto h : hashes) {
        result = hash64(&h, sizeof(h), result);
    }
    return result;
}

// ---------------------------------------------------------------------------
// hash<T> specialization for arithmetic types
// ---------------------------------------------------------------------------

template <typename T>
    requires std::is_arithmetic_v<T>
struct hash<T> {
    uint64_t operator()(T value, uint64_t seed = hash64_default_seed) const noexcept {
        return hash64(&value, sizeof(T), seed);
    }
};

// ---------------------------------------------------------------------------
// hash<T> specialization for pointer types
// ---------------------------------------------------------------------------

template <typename T>
struct hash<T*> {
    uint64_t operator()(const T* p, uint64_t seed = hash64_default_seed) const noexcept {
        return hash64(&p, sizeof(p), seed);
    }
};

// ---------------------------------------------------------------------------
// hash<T> specialization for enum types
// ---------------------------------------------------------------------------

template <typename T>
    requires std::is_enum_v<T>
struct hash<T> {
    uint64_t operator()(T value, uint64_t seed = hash64_default_seed) const noexcept {
        auto underlying = static_cast<std::underlying_type_t<T>>(value);
        return hash64(&underlying, sizeof(underlying), seed);
    }
};

// ---------------------------------------------------------------------------
// hash<T> specialization for types with a .hash() method
// ---------------------------------------------------------------------------

template <typename T>
concept HasHashMethod = requires(const T& v) {
    { v.hash() } -> std::convertible_to<uint64_t>;
};

template <HasHashMethod T>
struct hash<T> {
    uint64_t operator()(const T& value, uint64_t /*seed*/ = hash64_default_seed) const noexcept {
        return value.hash();
    }
};

// ---------------------------------------------------------------------------
// Hash128 — 128-bit hash from XXH3
// ---------------------------------------------------------------------------

class Hash128 {
public:
    std::array<uint8_t, 16> _data;

    Hash128() noexcept : _data{} {}

    explicit Hash128(std::span<const uint8_t> data) noexcept {
        std::copy_n(data.data(), (std::min)(data.size(), size_t{16}), _data.begin());
    }

    explicit Hash128(std::string_view sv) noexcept {
        std::copy_n(reinterpret_cast<const uint8_t*>(sv.data()),
                    (std::min)(sv.size(), size_t{16}), _data.begin());
    }

    [[nodiscard]] const uint8_t* data() const noexcept { return _data.data(); }
    [[nodiscard]] uint8_t* data() noexcept { return _data.data(); }

    [[nodiscard]] size_t size() const noexcept { return 16; }

    string to_string() const;

    bool operator==(const Hash128& other) const noexcept {
        return _data == other._data;
    }

    bool operator!=(const Hash128& other) const noexcept {
        return !(*this == other);
    }
};

// 128-bit hash function
KIMIX_CORE_API Hash128 hash128(const void* data, size_t size, uint64_t seed = hash64_default_seed) noexcept;

} // namespace kimix
