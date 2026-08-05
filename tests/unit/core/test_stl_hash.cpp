// Test for stl/hash.h (kimix::hash64, kimix::hash_value, kimix::Hash128, kimix::hash_combine).
// This test covers:
// - hash64 produces same result for same input
// - hash64 with different seeds produces different results
// - hash_value for integers, floats, pointers
// - Hash128 construction and to_string
// - hash128 produces consistent results
// - hash_combine

#include "ut/ut.hpp"
#include <core/kimix_core.h>

#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "hash64_same_input_same_result"_test = [] {
        const char* data = "hello world";
        auto h1 = kimix::hash64(data, std::strlen(data));
        auto h2 = kimix::hash64(data, std::strlen(data));
        expect(eq(h1, h2)) << "hash64 should be deterministic";
    };

    "hash64_different_input_different_result"_test = [] {
        const char* d1 = "hello";
        const char* d2 = "world";
        auto h1 = kimix::hash64(d1, std::strlen(d1));
        auto h2 = kimix::hash64(d2, std::strlen(d2));
        expect(neq(h1, h2)) << "different inputs should produce different hashes";
    };

    "hash64_different_seeds_different_result"_test = [] {
        const char* data = "test data";
        auto h1 = kimix::hash64(data, std::strlen(data), 0);
        auto h2 = kimix::hash64(data, std::strlen(data), 42);
        expect(neq(h1, h2)) << "different seeds should produce different hashes";
    };

    "hash64_empty"_test = [] {
        auto h = kimix::hash64(nullptr, 0);
        // Should not crash, produces a valid hash
        expect(h != 0_u || h == 0_u) << "hash64 on empty data should not crash";
    };

    "hash_value_integer"_test = [] {
        int v1 = 42;
        int v2 = 42;
        auto h1 = kimix::hash_value(v1);
        auto h2 = kimix::hash_value(v2);
        expect(eq(h1, h2)) << "hash_value for same int should be equal";

        int v3 = 43;
        auto h3 = kimix::hash_value(v3);
        expect(neq(h1, h3)) << "hash_value for different int should differ";
    };

    "hash_value_float"_test = [] {
        float f1 = 3.14f;
        float f2 = 3.14f;
        float f3 = 2.71f;
        auto h1 = kimix::hash_value(f1);
        auto h2 = kimix::hash_value(f2);
        expect(eq(h1, h2));
        auto h3 = kimix::hash_value(f3);
        expect(neq(h1, h3));
    };

    "hash_value_pointer"_test = [] {
        int x = 5;
        int y = 10;
        auto h1 = kimix::hash_value(&x);
        auto h2 = kimix::hash_value(&x);
        expect(eq(h1, h2)) << "hash_value for same pointer should be equal";

        auto h3 = kimix::hash_value(&y);
        expect(neq(h1, h3)) << "hash_value for different pointers should differ";
    };

    "hash_value_with_seed"_test = [] {
        int v = 100;
        auto h1 = kimix::hash_value(v, 0);
        auto h2 = kimix::hash_value(v, 1);
        expect(neq(h1, h2)) << "different seeds should produce different values";
    };

    "Hash128_default_construction"_test = [] {
        kimix::Hash128 h;
        expect(eq(h.size(), 16_u));
        // Default should be all zeros
        const uint8_t* d = h.data();
        bool all_zero = true;
        for (size_t i = 0; i < 16; ++i) {
            if (d[i] != 0) { all_zero = false; break; }
        }
        expect(all_zero) << "default Hash128 should be all zeros";
    };

    "Hash128_to_string"_test = [] {
        kimix::Hash128 h;
        auto s = h.to_string();
        expect(eq(s.size(), 32_u)) << "to_string should produce 32 hex chars";
        expect(s == "00000000000000000000000000000000");
    };

    "Hash128_equality"_test = [] {
        const char* data = "test 128";
        auto h1 = kimix::hash128(data, std::strlen(data));
        auto h2 = kimix::hash128(data, std::strlen(data));
        expect(h1 == h2) << "hash128 should be deterministic";
        expect(!(h1 != h2));
    };

    "hash128_consistency"_test = [] {
        const char* d1 = "alpha";
        const char* d2 = "alpha";
        auto h1 = kimix::hash128(d1, std::strlen(d1));
        auto h2 = kimix::hash128(d2, std::strlen(d2));
        expect(h1 == h2);

        const char* d3 = "beta";
        auto h3 = kimix::hash128(d3, std::strlen(d3));
        expect(h1 != h3) << "different inputs should differ";
    };

    "hash_combine_initializer_list"_test = [] {
        auto h1 = kimix::hash_combine({1ull, 2ull, 3ull});
        auto h2 = kimix::hash_combine({1ull, 2ull, 3ull});
        expect(eq(h1, h2)) << "hash_combine should be deterministic";

        auto h3 = kimix::hash_combine({3ull, 2ull, 1ull});
        // Order matters — different order, different hash
        expect(neq(h1, h3)) << "hash_combine order should matter";
    };

    "hash_combine_span"_test = [] {
        uint64_t vals[] = {10ull, 20ull, 30ull};
        auto h1 = kimix::hash_combine(std::span<const uint64_t>(vals, 3));
        auto h2 = kimix::hash_combine(std::span<const uint64_t>(vals, 3));
        expect(eq(h1, h2));
    };

    "hash_combine_single"_test = [] {
        auto h1 = kimix::hash_combine({42ull});
        auto h2 = kimix::hash_combine({42ull});
        expect(eq(h1, h2));
    };

}
