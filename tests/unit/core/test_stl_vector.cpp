// Test for stl/vector.h (kimix::vector<T>).
// This test covers:
// - Default construction
// - push_back, emplace_back
// - resize, reserve
// - Iteration
// - Copy/move
// - enlarge_by, size_bytes, vector_resize helpers

#include "ut/ut.hpp"
#include <core/kimix_core.h>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "vector_default_construction"_test = [] {
        kimix::vector<int> v;
        expect(v.empty()) << "default vector should be empty";
        expect(eq(v.size(), 0_u));
    };

    "vector_push_back"_test = [] {
        kimix::vector<int> v;
        v.push_back(1);
        v.push_back(2);
        v.push_back(3);
        expect(eq(v.size(), 3_u));
        expect(eq(v[0], 1));
        expect(eq(v[1], 2));
        expect(eq(v[2], 3));
    };

    "vector_emplace_back"_test = [] {
        kimix::vector<kimix::string> v;
        v.emplace_back("hello");
        v.emplace_back("world");
        expect(eq(v.size(), 2_u));
        expect(v[0] == "hello");
        expect(v[1] == "world");
    };

    "vector_resize"_test = [] {
        kimix::vector<int> v;
        v.resize(5, 42);
        expect(eq(v.size(), 5_u));
        for (size_t i = 0; i < 5; ++i) {
            expect(eq(v[i], 42)) << "all elements should be 42";
        }
    };

    "vector_reserve"_test = [] {
        kimix::vector<int> v;
        v.reserve(100);
        expect(v.capacity() >= 100_u) << "capacity should be at least 100";
        expect(eq(v.size(), 0_u)) << "size should still be 0 after reserve";
    };

    "vector_iteration"_test = [] {
        kimix::vector<int> v = {10, 20, 30, 40, 50};
        int sum = 0;
        for (auto& x : v) {
            sum += x;
        }
        expect(eq(sum, 150));

        // Iterator-based
        sum = 0;
        for (auto it = v.begin(); it != v.end(); ++it) {
            sum += *it;
        }
        expect(eq(sum, 150));
    };

    "vector_copy_construction"_test = [] {
        kimix::vector<int> v1 = {1, 2, 3, 4};
        kimix::vector<int> v2(v1);
        expect(eq(v2.size(), 4_u));
        for (size_t i = 0; i < 4; ++i) {
            expect(eq(v2[i], static_cast<int>(i + 1)));
        }
    };

    "vector_copy_assignment"_test = [] {
        kimix::vector<int> v1 = {5, 6, 7};
        kimix::vector<int> v2 = {0, 0};
        v2 = v1;
        expect(eq(v2.size(), 3_u));
        expect(eq(v2[0], 5));
        expect(eq(v2[1], 6));
        expect(eq(v2[2], 7));
    };

    "vector_move_construction"_test = [] {
        kimix::vector<int> v1 = {1, 2, 3};
        kimix::vector<int> v2(std::move(v1));
        expect(eq(v2.size(), 3_u));
        expect(eq(v2[0], 1));
        expect(eq(v2[1], 2));
        expect(eq(v2[2], 3));
    };

    "vector_move_assignment"_test = [] {
        kimix::vector<int> v1 = {10, 20};
        kimix::vector<int> v2 = {0};
        v2 = std::move(v1);
        expect(eq(v2.size(), 2_u));
        expect(eq(v2[0], 10));
        expect(eq(v2[1], 20));
    };

    "vector_enlarge_by"_test = [] {
        kimix::vector<int> v = {1, 2, 3};
        kimix::enlarge_by(v, 2);
        expect(eq(v.size(), 5_u));
        expect(eq(v[0], 1));
        expect(eq(v[1], 2));
        expect(eq(v[2], 3));
    };

    "vector_size_bytes"_test = [] {
        kimix::vector<int> v = {1, 2, 3, 4, 5};
        expect(eq(kimix::size_bytes(v), 5u * sizeof(int)));
    };

    "vector_vector_resize"_test = [] {
        kimix::vector<int> v;
        kimix::vector_resize(v, 3);
        expect(eq(v.size(), 3_u));
    };

    "vector_initializer_list"_test = [] {
        kimix::vector<double> v = {1.0, 2.0, 3.0};
        expect(eq(v.size(), 3_u));
        expect(std::abs(v[0] - 1.0) < 1e-9);
        expect(std::abs(v[1] - 2.0) < 1e-9);
        expect(std::abs(v[2] - 3.0) < 1e-9);
    };

}
