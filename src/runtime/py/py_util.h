/*
 * py_util.h - shared binding-layer conversion helpers (runtime_py).
 *
 * BINDING-LAYER ONLY (includes pybind11; kernels must never include this).
 *
 * These five helpers used to be copy-pasted into the anonymous namespace of
 * nearly every py_*.cpp TU. Under the runtime_py unity build (batch_size = 8)
 * two copies of the same helper can land in one merged TU, which is a hard
 * "redefinition" error — so they live here once, as inline functions in a
 * named namespace (ODR-safe across merged TUs). Each py_*.cpp adds a
 * using-declaration for the helpers it uses, so unqualified call sites are
 * unchanged.
 */
#pragma once

#include <pybind11/pybind11.h>

#include <core/stl/optional.h>
#include <core/stl/string.h>

namespace kimix_py_util {

namespace py = pybind11;

// Extract a string_view over a py::bytes WITHOUT copying (valid until the
// py::bytes object is destroyed; callers must not touch Python while the
// view is alive and the GIL is released).
inline bool bytes_view(py::bytes data, kimix::string_view& view) {
    char* buf = nullptr;
    Py_ssize_t len = 0;
    if (PyBytes_AsStringAndSize(data.ptr(), &buf, &len) < 0) {
        return false;
    }
    view = kimix::string_view(buf, static_cast<size_t>(len));
    return true;
}

// Convert a Python str to a UTF-8 kimix::string.
inline bool str_to_string(py::handle obj, kimix::string& out) {
    if (!PyUnicode_Check(obj.ptr())) {
        return false;
    }
    Py_ssize_t len = 0;
    const char* cstr = PyUnicode_AsUTF8AndSize(obj.ptr(), &len);
    if (cstr == nullptr) {
        return false;
    }
    out.assign(cstr, static_cast<size_t>(len));
    return true;
}

// UTF-8 kimix::string -> Python str (the kernels return UTF-8 kimix::string).
inline py::str to_py_str(const kimix::string& s) {
    return py::str(s.data(), s.size());
}

// UTF-8 kimix::string -> Python bytes.
inline py::bytes to_bytes(const kimix::string& s) {
    return py::bytes(s.data(), s.size());
}

// optional<kimix::string> -> str or None.
inline py::object opt_str_to_obj(const kimix::optional<kimix::string>& o) {
    if (o.has_value()) {
        return to_py_str(*o);
    }
    return py::none();
}

} // namespace kimix_py_util
