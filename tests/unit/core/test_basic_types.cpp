// Test for basic_types.h (kimix::Vector<T,N>, kimix::Matrix<T,N>).
// This test covers:
// - Vector construction and access
// - Vector operators (+, -, *, /, comparison)
// - any(), all(), none() for bool vectors
// - Matrix construction and access
// - Matrix identity
// - sizeof/alignof checks (float4, float3, float2)

#include "ut/ut.hpp"
#include <core/kimix_core.h>

#include <cmath>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "vector_default_construction"_test = [] {
        kimix::float4 v;
        for (size_t i = 0; i < 4; ++i) {
            expect(std::abs(v[i] - 0.0f) < 1e-9f) << "default vector elements should be 0";
        }
    };

    "vector_value_construction"_test = [] {
        kimix::float4 v(1.0f);
        for (size_t i = 0; i < 4; ++i) {
            expect(std::abs(v[i] - 1.0f) < 1e-9f) << "fill-constructed elements should be 1.0";
        }
    };

    "vector_multi_arg_construction"_test = [] {
        kimix::float3 v(1.0f, 2.0f, 3.0f);
        expect(std::abs(v[0] - 1.0f) < 1e-9f);
        expect(std::abs(v[1] - 2.0f) < 1e-9f);
        expect(std::abs(v[2] - 3.0f) < 1e-9f);
    };

    "vector_named_accessors"_test = [] {
        kimix::float4 v(1.0f, 2.0f, 3.0f, 4.0f);
        expect(std::abs(v.x() - 1.0f) < 1e-9f);
        expect(std::abs(v.y() - 2.0f) < 1e-9f);
        expect(std::abs(v.z() - 3.0f) < 1e-9f);
        expect(std::abs(v.w() - 4.0f) < 1e-9f);
    };

    "vector_mutable_access"_test = [] {
        kimix::float3 v;
        v.x() = 10.0f;
        v.y() = 20.0f;
        v.z() = 30.0f;
        expect(std::abs(v[0] - 10.0f) < 1e-9f);
        expect(std::abs(v[1] - 20.0f) < 1e-9f);
        expect(std::abs(v[2] - 30.0f) < 1e-9f);
    };

    "vector_addition"_test = [] {
        kimix::float3 a(1.0f, 2.0f, 3.0f);
        kimix::float3 b(4.0f, 5.0f, 6.0f);
        auto r = a + b;
        expect(std::abs(r[0] - 5.0f) < 1e-9f);
        expect(std::abs(r[1] - 7.0f) < 1e-9f);
        expect(std::abs(r[2] - 9.0f) < 1e-9f);
    };

    "vector_subtraction"_test = [] {
        kimix::float3 a(5.0f, 7.0f, 9.0f);
        kimix::float3 b(1.0f, 2.0f, 3.0f);
        auto r = a - b;
        expect(std::abs(r[0] - 4.0f) < 1e-9f);
        expect(std::abs(r[1] - 5.0f) < 1e-9f);
        expect(std::abs(r[2] - 6.0f) < 1e-9f);
    };

    "vector_multiplication"_test = [] {
        kimix::float2 a(2.0f, 3.0f);
        kimix::float2 b(4.0f, 5.0f);
        auto r = a * b;
        expect(std::abs(r[0] - 8.0f) < 1e-9f);
        expect(std::abs(r[1] - 15.0f) < 1e-9f);
    };

    "vector_scalar_multiplication"_test = [] {
        kimix::float3 a(1.0f, 2.0f, 3.0f);
        auto r = a * 2.0f;
        expect(std::abs(r[0] - 2.0f) < 1e-9f);
        expect(std::abs(r[1] - 4.0f) < 1e-9f);
        expect(std::abs(r[2] - 6.0f) < 1e-9f);
    };

    "vector_scalar_division"_test = [] {
        kimix::float3 a(6.0f, 8.0f, 10.0f);
        auto r = a / 2.0f;
        expect(std::abs(r[0] - 3.0f) < 1e-9f);
        expect(std::abs(r[1] - 4.0f) < 1e-9f);
        expect(std::abs(r[2] - 5.0f) < 1e-9f);
    };

    "vector_unary_negation"_test = [] {
        kimix::float3 a(1.0f, -2.0f, 3.0f);
        auto r = -a;
        expect(std::abs(r[0] + 1.0f) < 1e-9f);
        expect(std::abs(r[1] - 2.0f) < 1e-9f);
        expect(std::abs(r[2] + 3.0f) < 1e-9f);
    };

    "vector_compound_addition"_test = [] {
        kimix::float3 a(1.0f, 2.0f, 3.0f);
        a += kimix::float3(4.0f, 5.0f, 6.0f);
        expect(std::abs(a[0] - 5.0f) < 1e-9f);
        expect(std::abs(a[1] - 7.0f) < 1e-9f);
        expect(std::abs(a[2] - 9.0f) < 1e-9f);
    };

    "vector_comparison"_test = [] {
        kimix::float3 a(1.0f, 2.0f, 3.0f);
        kimix::float3 b(0.0f, 2.0f, 4.0f);
        auto lt = a < b;
        expect(!lt[0]);  // 1 < 0 = false
        expect(!lt[1]);  // 2 < 2 = false
        expect(lt[2]);   // 3 < 4 = true

        auto eq = a == b;
        expect(!eq[0]);
        expect(eq[1]);   // 2 == 2 = true
        expect(!eq[2]);
    };

    "vector_bool_any"_test = [] {
        kimix::bool3 v(false, false, false);
        expect(!kimix::any(v));
        v[1] = true;
        expect(kimix::any(v));
    };

    "vector_bool_all"_test = [] {
        kimix::bool3 v(true, true, true);
        expect(kimix::all(v));
        v[1] = false;
        expect(!kimix::all(v));
    };

    "vector_bool_none"_test = [] {
        kimix::bool3 v(false, false, false);
        expect(kimix::none(v));
        v[2] = true;
        expect(!kimix::none(v));
    };

    "vector_bool_logical_or"_test = [] {
        kimix::bool3 a(false, true, false);
        kimix::bool3 b(false, false, true);
        auto r = a || b;
        expect(!r[0]);
        expect(r[1]);
        expect(r[2]);
    };

    "vector_bool_logical_and"_test = [] {
        kimix::bool3 a(false, true, true);
        kimix::bool3 b(true, true, false);
        auto r = a && b;
        expect(!r[0]);
        expect(r[1]);
        expect(!r[2]);
    };

    "matrix_default_construction"_test = [] {
        kimix::Matrix<float, 3> m;
        for (size_t c = 0; c < 3; ++c) {
            for (size_t r = 0; r < 3; ++r) {
                expect(std::abs(m[c][r] - 0.0f) < 1e-9f);
            }
        }
    };

    "matrix_identity"_test = [] {
        kimix::Matrix<float, 3> m(1.0f);
        for (size_t c = 0; c < 3; ++c) {
            for (size_t r = 0; r < 3; ++r) {
                if (c == r) {
                    expect(std::abs(m[c][r] - 1.0f) < 1e-9f);
                } else {
                    expect(std::abs(m[c][r] - 0.0f) < 1e-9f);
                }
            }
        }
    };

    "matrix_access"_test = [] {
        kimix::Matrix<float, 2> m;
        m[0] = kimix::float2(1.0f, 2.0f);
        m[1] = kimix::float2(3.0f, 4.0f);
        expect(std::abs(m[0][0] - 1.0f) < 1e-9f);
        expect(std::abs(m[0][1] - 2.0f) < 1e-9f);
        expect(std::abs(m[1][0] - 3.0f) < 1e-9f);
        expect(std::abs(m[1][1] - 4.0f) < 1e-9f);
    };

    "matrix_vector_multiplication"_test = [] {
        kimix::Matrix<float, 2> m(1.0f); // identity
        kimix::float2 v(3.0f, 4.0f);
        auto r = m * v;
        expect(std::abs(r[0] - 3.0f) < 1e-9f);
        expect(std::abs(r[1] - 4.0f) < 1e-9f);
    };

    "matrix_matrix_multiplication"_test = [] {
        kimix::Matrix<float, 2> a(1.0f);
        kimix::Matrix<float, 2> b(1.0f);
        auto r = a * b;
        for (size_t c = 0; c < 2; ++c) {
            for (size_t row = 0; row < 2; ++row) {
                if (c == row) {
                    expect(std::abs(r[c][row] - 1.0f) < 1e-9f);
                } else {
                    expect(std::abs(r[c][row] - 0.0f) < 1e-9f);
                }
            }
        }
    };

    "matrix_addition"_test = [] {
        kimix::Matrix<float, 2> a(1.0f);
        kimix::Matrix<float, 2> b(2.0f);
        auto r = a + b;
        for (size_t c = 0; c < 2; ++c) {
            for (size_t row = 0; row < 2; ++row) {
                if (c == row) {
                    expect(std::abs(r[c][row] - 3.0f) < 1e-9f);
                } else {
                    expect(std::abs(r[c][row] - 0.0f) < 1e-9f);
                }
            }
        }
    };

    "matrix_subtraction"_test = [] {
        kimix::Matrix<float, 2> a(3.0f);
        kimix::Matrix<float, 2> b(1.0f);
        auto r = a - b;
        for (size_t c = 0; c < 2; ++c) {
            for (size_t row = 0; row < 2; ++row) {
                if (c == row) {
                    expect(std::abs(r[c][row] - 2.0f) < 1e-9f);
                } else {
                    expect(std::abs(r[c][row] - 0.0f) < 1e-9f);
                }
            }
        }
    };

    "sizeof_float4"_test = [] {
        expect(eq(sizeof(kimix::float4), 16_u)) << "float4 should be 16 bytes";
    };

    "sizeof_float3"_test = [] {
        expect(eq(sizeof(kimix::float3), 16_u)) << "float3 should be 16 bytes (12 + alignment padding)";
    };

    "sizeof_float2"_test = [] {
        expect(eq(sizeof(kimix::float2), 8_u)) << "float2 should be 8 bytes";
    };

    "alignof_float4"_test = [] {
        expect(eq(alignof(kimix::float4), 16_u)) << "float4 should be 16-byte aligned";
    };

    "alignof_float2"_test = [] {
        expect(eq(alignof(kimix::float2), 8_u)) << "float2 should be 8-byte aligned";
    };

}
