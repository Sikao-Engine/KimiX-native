/*
 * gil.h — RAII GIL-release guard.
 *
 * BINDING-LAYER ONLY. This header includes <pybind11/pybind11.h> and MUST
 * NEVER be included from kernel translation units: kernels are compiled into
 * runtime.dll, which has no Python dependency. Only the runtime_py binding
 * TUs (src/runtime/py/*.cpp) may include this header.
 *
 * Usage — every pure-computation kernel call made from a binding goes through
 * this guard so the GIL is released while the kernel runs:
 *
 *     m.def("count", [](py::bytes data) -> size_t {
 *         // Extract the buffer view BEFORE releasing the GIL (the bytes
 *         // object must not be touched while the GIL is released).
 *         char* buf = nullptr;
 *         Py_ssize_t len = 0;
 *         if (PyBytes_AsStringAndSize(data.ptr(), &buf, &len) < 0) {
 *             throw py::error_already_set();
 *         }
 *         kimix::string_view view(buf, static_cast<size_t>(len));
 *         kimix::runtime::common::gil_scoped_release release;
 *         return kimix::runtime::common::utf8_code_point_count(view);
 *     });
 *
 * Never call back into Python while a gil_scoped_release is alive.
 */

#pragma once

#include <pybind11/pybind11.h>

namespace kimix {
namespace runtime {
namespace common {

// RAII wrapper: releases the GIL on construction, reacquires on destruction.
// (Fully qualified on purpose: this header must not rely on a `py` alias
// that only exists in the binding TU.)
struct gil_scoped_release {
    pybind11::gil_scoped_release r;

    gil_scoped_release()
        : r() {}
};

} // namespace common
} // namespace runtime
} // namespace kimix
