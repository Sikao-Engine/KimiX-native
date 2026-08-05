#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#include <xxhash.h>

#include <core/stl/hash.h>
#include <core/stl/string.h>
#include <cstring>
#include <cassert>
#include <array>

namespace kimix {

uint64_t hash64(const void *ptr, size_t size, uint64_t seed) noexcept {
    return XXH3_64bits_withSeed(ptr, size, seed);
}

Hash128 hash128(const void *ptr, size_t size, uint64_t seed) noexcept {
    static_assert(sizeof(Hash128) == sizeof(XXH128_hash_t));
    auto result = XXH3_128bits_withSeed(ptr, size, seed);
    return reinterpret_cast<const Hash128 &>(result);
}

kimix::string Hash128::to_string() const {
    constexpr const char *hex = "0123456789abcdef";
    std::array<char, 32u> s{};
    for (auto i = 0u; i < 16u; ++i) {
        s[i * 2u + 0u] = hex[(_data[i] >> 4u) & 0x0fu];
        s[i * 2u + 1u] = hex[_data[i] & 0xfu];
    }
    return {s.data(), s.size()};
}

} // namespace kimix
