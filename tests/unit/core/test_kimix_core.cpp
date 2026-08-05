// Test for kimix_core.h
// This test covers:
// - kimix::add(int, int)
// - kimix::multiply(int, int)
// - edge cases with negative numbers and zero

#include "ut/ut.hpp"
#include <core/kimix_core.h>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "add_basic"_test = [] {
        expect(eq(kimix::add(2, 3), 5)) << "2 + 3 should equal 5";
        expect(eq(kimix::add(0, 0), 0)) << "0 + 0 should equal 0";
        expect(eq(kimix::add(-1, 1), 0)) << "-1 + 1 should equal 0";
        expect(eq(kimix::add(100, 200), 300)) << "100 + 200 should equal 300";
    };

    "add_negative"_test = [] {
        expect(eq(kimix::add(-3, 7), 4)) << "-3 + 7 should equal 4";
        expect(eq(kimix::add(5, -8), -3)) << "5 + (-8) should equal -3";
        expect(eq(kimix::add(-10, -20), -30)) << "-10 + (-20) should equal -30";
    };

    "multiply_basic"_test = [] {
        expect(eq(kimix::multiply(4, 5), 20)) << "4 * 5 should equal 20";
        expect(eq(kimix::multiply(0, 100), 0)) << "0 * 100 should equal 0";
        expect(eq(kimix::multiply(1, 1), 1)) << "1 * 1 should equal 1";
    };

    "multiply_negative"_test = [] {
        expect(eq(kimix::multiply(-3, 4), -12)) << "-3 * 4 should equal -12";
        expect(eq(kimix::multiply(-5, -5), 25)) << "-5 * (-5) should equal 25";
        expect(eq(kimix::multiply(7, -2), -14)) << "7 * (-2) should equal -14";
    };
}
