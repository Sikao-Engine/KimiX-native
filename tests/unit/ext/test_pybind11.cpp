// Test for pybind11 (Python binding library).
// This test verifies the pybind11 include path structure is correct.
// Note: Full pybind11 compilation requires Python development headers.
// This test performs structural checks only using __has_include.

#include "ut/ut.hpp"

// Use __has_include to verify the include path resolves correctly
// without actually parsing the headers (which would need Python.h)
#if defined(__has_include)
#  if !__has_include(<pybind11/pybind11.h>)
#    error "pybind11/pybind11.h not found - include path may be incorrect"
#  endif
#  if !__has_include(<pybind11/detail/common.h>)
#    error "pybind11/detail/common.h not found - include path may be incorrect"
#  endif
#  if !__has_include(<pybind11/attr.h>)
#    error "pybind11/attr.h not found - include path may be incorrect"
#  endif
#endif

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "pybind11_structure"_test = [] {
        expect(true) << "pybind11 include path resolves correctly";
    };
}
