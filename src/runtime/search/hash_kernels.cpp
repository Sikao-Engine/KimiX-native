/*
 * hash_kernels.cpp — implementation of SimHash / MinHash (see header).
 */

#include <runtime/search/hash_kernels.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

namespace kimix {
namespace runtime {
namespace search {

namespace {

// XXH3-64 of a byte buffer with a caller seed (kimix::hash64 contract).
inline uint64_t hash_bytes(const void* data, size_t size, uint64_t seed) noexcept {
    return kimix::hash64(data, size, seed);
}

} // namespace

uint64_t simhash(kimix::span<const kimix::string_view> tokens, uint64_t seed) noexcept {
    // Dedupe: the reference iterates set(text.split()) — order-independent
    // accumulation, each unique token contributes exactly once.
    kimix::unordered_set<uint64_t> seen;
    seen.reserve(tokens.size() * 2);

    // v[b] in [-n, +n]; n <= tokens.size() -> int32_t is plenty.
    int32_t v[64] = {0};
    for (kimix::string_view tok : tokens) {
        const uint64_t h = hash_bytes(tok.data(), tok.size(), seed);
        if (!seen.insert(h).second) {
            continue;
        }
        for (int b = 0; b < 64; ++b) {
            v[b] += ((h >> b) & 1u) ? 1 : -1;
        }
    }
    uint64_t result = 0;
    for (int b = 0; b < 64; ++b) {
        if (v[b] > 0) {
            result |= (1ull << b);
        }
    }
    return result;
}

kimix::vector<uint64_t> minhash(kimix::span<const kimix::string_view> shingles,
                                uint32_t k, uint64_t seed) noexcept {
    kimix::vector<uint64_t> sig;
    if (k == 0 || shingles.empty()) {
        sig.assign(k, 0); // reference: no shingles -> [0] * num_perm
        return sig;
    }
    sig.assign(k, std::numeric_limits<uint32_t>::max());

    // Pre-compute per-permutation seeds (deterministic contract).
    kimix::vector<uint64_t> perm_seeds;
    perm_seeds.reserve(k);
    for (uint32_t p = 0; p < k; ++p) {
        const uint32_t p_le = p; // little-endian bytes of p (x64 host)
        perm_seeds.push_back(hash_bytes(&p_le, sizeof(p_le), seed) & 0xFFFFFFFFull);
    }

    for (kimix::string_view sh : shingles) {
        const uint64_t sh_hash = hash_bytes(sh.data(), sh.size(), seed) & 0xFFFFFFFFull;
        for (uint32_t p = 0; p < k; ++p) {
            const uint64_t v = sh_hash ^ perm_seeds[p];
            if (v < sig[p]) {
                sig[p] = v;
            }
        }
    }
    return sig;
}

} // namespace search
} // namespace runtime
} // namespace kimix
