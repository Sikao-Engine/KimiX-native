// Compile gate for pybind11 (Python binding library).
// Note: Full pybind11 compilation requires Python development headers.
// This target performs structural checks only using __has_include.
//
// There is deliberately no Boost.UT runtime test: a tautological
// expect(true) "the include path resolves" is meaningless — the #error
// directives below already fail the build when the path is wrong, so a
// successful compile is the entire assertion.

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

int main() {
    return 0; // compiled => the pybind11 include path resolves
}
