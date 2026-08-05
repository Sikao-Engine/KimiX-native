/*
 * hash_kernels.h — SimHash / MinHash kernels (kimix::runtime::search).
 *
 * Plan 005: native ports of retrieval.py::SimHash._compute (lines 1276-1286)
 * and MinHash._compute (lines 2080-2092).
 *
 * Hashing contract (DOCUMENTED, deterministic):
 *   - Tokens/shingles are hashed with kimix::hash64 — XXH3_64bits_withSeed
 *     (genuine XXH3-64, src/core/stl/hash.cpp) with the caller-supplied seed
 *     (default kimix::hash64_default_seed = 2^61-1).
 *   - The Python _compat mirror reproduces this exactly with the `xxhash`
 *     package: xxhash.xxh3_64(data, seed=seed).intdigest().
 *
 * Deviations from the reference (documented):
 *   - SimHash._compute hashes with xxhash.xxh64 (a DIFFERENT algorithm than
 *     XXH3) and iterates set(text.split()); the kernel takes pre-tokenized
 *     shingles/tokens and uses XXH3, so VALUES differ from the reference but
 *     are exactly reproducible between native and _compat.
 *   - MinHash._compute uses Python's built-in hash() on shingles and on the
 *     permutation indices — that is PYTHONHASHSEED-dependent (random across
 *     processes), so the reference itself is not reproducible. The kernel
 *     defines a deterministic contract instead (below), which _compat
 *     mirrors bit-for-bit.
 *
 * minhash contract: num_perm permutations p in 0..k-1:
 *     seed_p = (uint32_t)(XXH3_64(&p_le32, 4, seed))
 *     sig[p] = min over shingles of (XXH3_64(shingle, seed) ^ seed_p)
 *            & 0xFFFFFFFF   (both operands masked to 32 bits, like the
 *             reference's hash(...) & 0xFFFFFFFF)
 *
 * Pure C++ kernel: no Python includes; GIL released in the binding layer.
 */

#pragma once

#include <core/kimix_core.h>

namespace kimix {
namespace runtime {
namespace search {

// SimHash: 64-bit accumulation. `tokens` are deduplicated first (the
// reference iterates set(text.split()) — each unique token contributes once).
// For each unique token h = hash64(token, seed):
//   v[b] += ((h >> b) & 1) ? +1 : -1   for b in 0..63
// result bit b = (v[b] > 0). seed defaults to kimix::hash64_default_seed.
KIMIX_RUNTIME_API uint64_t simhash(kimix::span<const kimix::string_view> tokens,
                                   uint64_t seed = kimix::hash64_default_seed) noexcept;

// MinHash: k-minimum signature of length k (num_perm), see the contract in
// the file comment. Returns sig[0..k). k == 0 -> empty vector.
KIMIX_RUNTIME_API kimix::vector<uint64_t> minhash(
    kimix::span<const kimix::string_view> shingles, uint32_t k, uint64_t seed) noexcept;

} // namespace search
} // namespace runtime
} // namespace kimix
