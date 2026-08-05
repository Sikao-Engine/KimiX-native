// Test for EASTL (EA Standard Template Library).
// This test covers:
// - eastl::vector basic operations
// - eastl::shared_ptr basic operations
// - eastl::array basic operations
// - eastl::optional basic operations
// - eastl::algorithm (sort, find, count)

#include "ut/ut.hpp"

#include <EASTL/vector.h>
#include <EASTL/shared_ptr.h>
#include <EASTL/array.h>
#include <EASTL/optional.h>
#include <EASTL/algorithm.h>
#include <EASTL/sort.h>

#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "eastl_vector"_test = [] {
        eastl::vector<int> v;
        expect(v.empty());
        expect(eq(v.size(), 0u));

        v.push_back(10);
        v.push_back(20);
        v.push_back(30);
        expect(eq(v.size(), 3u));
        expect(!v.empty());

        expect(eq(v[0], 10));
        expect(eq(v[1], 20));
        expect(eq(v[2], 30));
        expect(eq(v.front(), 10));
        expect(eq(v.back(), 30));

        int sum = 0;
        for (auto x : v) { sum += x; }
        expect(eq(sum, 60));

        v.pop_back();
        expect(eq(v.size(), 2u));
        expect(eq(v.back(), 20));

        int *data = v.data();
        expect(eq(data[0], 10));
        expect(eq(data[1], 20));

        v.clear();
        expect(v.empty());
        expect(eq(v.size(), 0u));
    };

    "eastl_shared_ptr"_test = [] {
        eastl::shared_ptr<int> sp = eastl::make_shared<int>(42);
        expect(sp != nullptr);
        expect(eq(*sp, 42));

        eastl::shared_ptr<int> sp2 = sp;
        expect(eq(*sp2, 42));
        expect(eq(sp.use_count(), 2));

        sp2.reset();
        expect(sp2 == nullptr);
        expect(eq(sp.use_count(), 1));

        sp.reset(new int(100));
        expect(eq(*sp, 100));
    };

    "eastl_array"_test = [] {
        eastl::array<int, 3> a = {{1, 2, 3}};
        expect(eq(a.size(), 3u));
        expect(eq(a[0], 1));
        expect(eq(a[1], 2));
        expect(eq(a[2], 3));
        expect(eq(a.front(), 1));
        expect(eq(a.back(), 3));

        a.fill(0);
        expect(eq(a[0], 0));
        expect(eq(a[1], 0));
        expect(eq(a[2], 0));
    };

    "eastl_optional"_test = [] {
        eastl::optional<int> opt;
        expect(!opt.has_value());

        opt = 42;
        expect(opt.has_value());
        expect(eq(*opt, 42));
        expect(eq(opt.value(), 42));

        eastl::optional<int> empty;
        expect(eq(empty.value_or(-1), -1));

        opt.reset();
        expect(!opt.has_value());
    };

    "eastl_algorithm"_test = [] {
        eastl::vector<int> v = {5, 2, 8, 1, 9};

        auto it = eastl::find(v.begin(), v.end(), 8);
        expect(neq(it, v.end()));
        expect(eq(*it, 8));

        it = eastl::find(v.begin(), v.end(), 99);
        expect(eq(it, v.end()));

        auto count = eastl::count(v.begin(), v.end(), 2);
        expect(eq(count, 1));

        eastl::sort(v.begin(), v.end());
        expect(eq(v[0], 1));
        expect(eq(v[1], 2));
        expect(eq(v[2], 5));
        expect(eq(v[3], 8));
        expect(eq(v[4], 9));
    };

    "eastl_compare"_test = [] {
        char s1[] = "hello";
        char s2[] = "hello";
        char s3[] = "world";
        expect(eq(strcmp(s1, s2), 0));
        expect(neq(strcmp(s1, s3), 0));
    };
}
