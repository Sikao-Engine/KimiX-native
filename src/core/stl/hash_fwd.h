#pragma once

#include "../dll_export.h"
#include <cstddef>
#include <cstdint>

namespace kimix {

// Default 64-bit hash seed (prime: 2^61 - 1).
inline constexpr uint64_t hash64_default_seed = (1ull << 61ull) - 1ull;

// Hash a block of memory using xxHash (XXH3_64bits). Implemented in the core library.
KIMIX_CORE_API uint64_t hash64(const void* data, size_t size, uint64_t seed = hash64_default_seed) noexcept;

// Primary hash template — specialize for your type.
template <typename T>
struct hash;

} // namespace kimix
