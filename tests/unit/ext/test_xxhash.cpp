// Test for xxHash (fast hash library).
// This test covers:
// - XXH32 with various inputs
// - XXH64 with various inputs
// - XXH3_64bits (when available)
// - Stream-based hashing
// - Empty input handling
// - Deterministic output verification

#include "ut/ut.hpp"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "xxh32_basic"_test = [] {
        XXH32_hash_t h_empty1 = XXH32("", 0, 0);
        XXH32_hash_t h_empty2 = XXH32("", 0, 0);
        expect(eq(h_empty1, h_empty2)) << "XXH32('', seed=0) should be deterministic";

        XXH32_hash_t h1 = XXH32("abc", 3, 0);
        XXH32_hash_t h2 = XXH32("abc", 3, 0);
        expect(eq(h1, h2)) << "XXH32 should be deterministic";

        XXH32_hash_t h3 = XXH32("abc", 3, 42);
        expect(neq(h1, h3)) << "different seeds should produce different hashes";

        XXH32_hash_t h_long = XXH32("Hello, World!", 13, 0);
        expect(neq(h_long, 0u)) << "non-zero hash for non-empty input";

        XXH32_hash_t h_null = XXH32(nullptr, 0, 0);
        expect(eq(h_null, h_empty1)) << "XXH32(nullptr, 0) should match empty";
    };

    "xxh32_full"_test = [] {
        const char *data = "The quick brown fox jumps over the lazy dog";
        size_t len = strlen(data);

        XXH32_hash_t h = XXH32(data, len, 0);
        expect(neq(h, 0u));

        XXH32_hash_t h2 = XXH32(data, len, 0);
        expect(eq(h, h2));
    };

    "xxh64_basic"_test = [] {
        XXH64_hash_t h_empty1 = XXH64("", 0, 0);
        XXH64_hash_t h_empty2 = XXH64("", 0, 0);
        expect(eq(h_empty1, h_empty2)) << "XXH64('', seed=0) should be deterministic";

        XXH64_hash_t h1 = XXH64("abc", 3, 0);
        XXH64_hash_t h2 = XXH64("abc", 3, 0);
        expect(eq(h1, h2));

        XXH64_hash_t h3 = XXH64("abc", 3, 42);
        expect(neq(h1, h3));

        XXH64_hash_t h_null = XXH64(nullptr, 0, 0);
        expect(eq(h_null, h_empty1)) << "XXH64(nullptr, 0) should match empty";
    };

    "xxh64_full"_test = [] {
        const char *data = "The quick brown fox jumps over the lazy dog";
        size_t len = strlen(data);

        XXH64_hash_t h = XXH64(data, len, 0);
        expect(neq(h, 0u));

        XXH64_hash_t h2 = XXH64(data, len, 0);
        expect(eq(h, h2));
    };

    "xxh3_64"_test = [] {
        XXH64_hash_t h = XXH3_64bits("", 0);
        expect(neq(h, 0u));

        h = XXH3_64bits("abc", 3);
        expect(neq(h, 0u));

        XXH64_hash_t h2 = XXH3_64bits("abc", 3);
        expect(eq(h, h2));

        XXH64_hash_t h3 = XXH3_64bits_withSeed("abc", 3, 42);
        expect(neq(h, h3));
    };

    "xxh3_128"_test = [] {
        XXH128_hash_t h = XXH3_128bits("", 0);
        expect(neq(h.high64, 0u) || neq(h.low64, 0u));

        h = XXH3_128bits("test data", 9);
        expect(neq(h.high64, 0u) || neq(h.low64, 0u));

        XXH128_hash_t h2 = XXH3_128bits("test data", 9);
        expect(eq(h.high64, h2.high64));
        expect(eq(h.low64, h2.low64));

        XXH128_hash_t h3 = XXH3_128bits_withSeed("test data", 9, 42);
        expect(neq(h.low64, h3.low64));
    };

    "xxh_stream"_test = [] {
        XXH64_state_t *state = XXH64_createState();
        expect(state != nullptr);

        XXH64_reset(state, 0);

        const char *part1 = "Hello ";
        const char *part2 = "World!";

        XXH_errorcode err = XXH64_update(state, part1, strlen(part1));
        expect(eq(err, XXH_OK));
        err = XXH64_update(state, part2, strlen(part2));
        expect(eq(err, XXH_OK));

        XXH64_hash_t stream_hash = XXH64_digest(state);
        XXH64_hash_t single_hash = XXH64("Hello World!", 12, 0);
        expect(eq(stream_hash, single_hash)) << "streaming hash should match single-shot";

        XXH64_freeState(state);
    };

    "xxh_canonical"_test = [] {
        XXH64_hash_t h = XXH64("test", 4, 0);

        XXH64_canonical_t c;
        XXH64_canonicalFromHash(&c, h);

        XXH64_hash_t h2 = XXH64_hashFromCanonical(&c);
        expect(eq(h, h2)) << "canonical round-trip should preserve hash";
    };
}
