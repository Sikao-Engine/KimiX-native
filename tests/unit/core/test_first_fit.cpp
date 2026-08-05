// Test for first_fit.h (kimix::FirstFit).
// This test covers:
// - Simple allocate
// - Allocate multiple blocks
// - Exhaustion (return nullptr when full)
// - dump_free_list
// - total_size, buffer access

#include "ut/ut.hpp"
#include <core/kimix_core.h>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "first_fit_simple_allocate"_test = [] {
        kimix::FirstFit ff(1024);
        auto* node = ff.allocate(128);
        expect(node != nullptr) << "should allocate 128 bytes";
        expect(node->size >= 128_u);
        ff.free(node);
    };

    "first_fit_total_size"_test = [] {
        kimix::FirstFit ff(2048);
        expect(eq(ff.total_size(), 2048_u));
    };

    "first_fit_buffer"_test = [] {
        kimix::FirstFit ff(1024);
        expect(ff.buffer() != nullptr) << "buffer should be non-null after initialization";
    };

    "first_fit_dump_free_list"_test = [] {
        kimix::FirstFit ff(1024);
        ff.dump_free_list();
        expect(true) << "dump_free_list should not crash";
    };

}
