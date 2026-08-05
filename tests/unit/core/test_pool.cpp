// Test for pool.h (kimix::Pool<T>).
// This test covers:
// - create/destroy objects
// - Object reuse (destroyed objects returned to pool)
// - Multiple allocations (beyond block_size)
// Note: T must be >= sizeof(void*) for pool to work correctly.

#include "ut/ut.hpp"
#include <core/kimix_core.h>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "pool_create_destroy"_test = [] {
        kimix::Pool<int64_t> pool;
        int64_t* p = pool.create(42);
        expect(p != nullptr) << "pool.create() should return non-null";
        expect(eq(*p, 42_i));
        pool.destroy(p);
    };

    "pool_create_default"_test = [] {
        struct TestObj {
            int64_t x = 0;
            int64_t y = 0;
        };
        kimix::Pool<TestObj> pool;
        auto* obj = pool.create();
        expect(obj != nullptr);
        expect(eq(obj->x, 0_i));
        expect(eq(obj->y, 0_i));
        pool.destroy(obj);
    };

    "pool_object_reuse"_test = [] {
        kimix::Pool<int64_t> pool;
        int64_t* p1 = pool.create(100);
        int64_t* addr1 = p1;
        pool.destroy(p1);

        int64_t* p2 = pool.create(200);
        expect(eq(p2, addr1)) << "freed object should be reused (same address)";
        expect(eq(*p2, 200_i));
        pool.destroy(p2);
    };

    "pool_multiple_allocations_beyond_block_size"_test = [] {
        kimix::Pool<int64_t> pool;
        constexpr int N = 200;
        int64_t* ptrs[N];

        for (int i = 0; i < N; ++i) {
            ptrs[i] = pool.create(static_cast<int64_t>(i));
            expect(ptrs[i] != nullptr) << "allocation " << i << " should succeed";
            expect(eq(*ptrs[i], static_cast<int64_t>(i)));
        }

        for (int i = 0; i < N; ++i) {
            pool.destroy(ptrs[i]);
        }
    };

    "pool_non_thread_safe"_test = [] {
        kimix::Pool<double, false> pool;
        auto* p = pool.create(3.14);
        expect(p != nullptr);
        expect(std::abs(*p - 3.14) < 1e-9);
        pool.destroy(p);
    };

}
