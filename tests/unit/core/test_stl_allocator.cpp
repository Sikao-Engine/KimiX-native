// Test for stl/memory.h (kimix::allocator<T>).
// This test covers:
// - Basic allocate/deallocate
// - Allocate with alignment
// - Allocate returns non-null
// - vector with custom allocator
// - string with custom allocator
// - Allocator equality (all allocators equal)

#include "ut/ut.hpp"
#include <core/kimix_core.h>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "allocator_basic_allocate_deallocate"_test = [] {
        kimix::allocator<int> alloc;
        int* p = alloc.allocate(1);
        expect(p != nullptr) << "allocate should return non-null";
        *p = 42;
        expect(eq(*p, 42)) << "should be able to write through allocated pointer";
        alloc.deallocate(p, 1);
    };

    "allocator_allocate_multiple"_test = [] {
        kimix::allocator<int> alloc;
        int* p = alloc.allocate(5);
        expect(p != nullptr) << "allocate(5) should return non-null";
        for (int i = 0; i < 5; ++i) {
            p[i] = i * 10;
        }
        for (int i = 0; i < 5; ++i) {
            expect(eq(p[i], i * 10)) << "elements should hold assigned values";
        }
        alloc.deallocate(p, 5);
    };

    "allocator_allocate_non_null"_test = [] {
        kimix::allocator<double> alloc;
        double* p = alloc.allocate(10);
        expect(p != nullptr);
        alloc.deallocate(p, 10);
    };

    "allocator_allocate_zero"_test = [] {
        kimix::allocator<int> alloc;
        int* p = alloc.allocate(0);
        expect(p == nullptr) << "allocate(0) should return nullptr";
        alloc.deallocate(p, 0); // should be safe
    };

    "allocator_allocate_with_alignment"_test = [] {
        kimix::allocator<int> alloc;
        int* p = alloc.allocate(1, 64);
        expect(p != nullptr) << "aligned allocate should return non-null";
        expect((reinterpret_cast<uintptr_t>(p) % 64) == 0_i) << "pointer should be 64-byte aligned";
        *p = 99;
        expect(eq(*p, 99));
        alloc.deallocate(p, 1);
    };

    "allocator_vector_with_custom_allocator"_test = [] {
        kimix::vector<int> vec;
        for (int i = 0; i < 10; ++i) {
            vec.push_back(i * 5);
        }
        expect(eq(vec.size(), 10_u)) << "vector should have 10 elements";
        for (int i = 0; i < 10; ++i) {
            expect(eq(vec[i], i * 5)) << "vector element mismatch";
        }
    };

    "allocator_string_with_custom_allocator"_test = [] {
        kimix::string s = "hello world";
        expect(eq(s.size(), 11_u));
        expect(s == "hello world");
    };

    "allocator_equality"_test = [] {
        kimix::allocator<int> a1;
        kimix::allocator<int> a2;
        kimix::allocator<double> a3;

        expect(a1 == a2) << "same-type allocators should be equal";
        expect(a1 == a3) << "different-type allocators should be equal";
        expect(!(a1 != a2)) << "same-type allocators should not be unequal";
        expect(!(a1 != a3)) << "different-type allocators should not be unequal";
    };

}
