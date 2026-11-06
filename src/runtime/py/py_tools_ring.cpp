/*
 * py_tools_ring.cpp - Python bindings for tools.OutputRing (runtime_py.tools).
 *
 * BINDING-LAYER ONLY. Registers into the `tools` submodule that py_tools.cpp
 * owns (two-TU precedent: py_register_index + py_register_history fill `index`).
 *
 * CONTRACT WITH THE SHIM (python/kimix_native/tools.py::OutputRing):
 *   tools.OutputRing(cap_chars=2000000, head_ratio=0.4)
 *     .append(data: bytes) -> bool        # True when this append truncated
 *     .read(char_offset, max_chars) -> bytes
 *     .snapshot() -> bytes
 *     .char_count / .byte_count / .truncated / .cap_chars / .bytes_owned
 *     .clear()
 * The byte-side mirror is `_compat_bounded_append` (head 40 % / marker / tail
 * 60 %, code points), which python/tests/test_parity_job_output.py already pins
 * against the reference `bounded_append`.
 */

#include <pybind11/pybind11.h>

#include <runtime/common/gil.h>
#include <runtime/runtime.h>
#include <runtime/tools/output_ring.h>

#include "py_util.h"

using kimix_py_util::bytes_view;
using kimix_py_util::to_bytes;

namespace py = pybind11;

namespace src_runtime_py_py_tools_ring {

using kimix::runtime::tools::OutputRing;

} // namespace src_runtime_py_py_tools_ring
using namespace src_runtime_py_py_tools_ring;

void py_register_tools_ring(py::module_& m) {
    py::class_<OutputRing>(m, "OutputRing",
                           "Bounded background-job output ring: head 40 % / "
                           "marker / tail 60 % in code points (the reference "
                           "bounded_append semantics).")
        .def(py::init<uint64_t, double>(), py::arg("cap_chars") = 2000000,
             py::arg("head_ratio") = 0.4,
             "OutputRing(cap_chars=2000000, head_ratio=0.4)")
        .def("append",
             [](OutputRing &r, py::bytes data) -> bool {
                 kimix::string_view view;
                 if (!bytes_view(data, view)) {
                     throw py::error_already_set();
                 }
                 kimix::runtime::common::gil_scoped_release release;
                 return r.append(view);
             },
             "append(data: bytes) -> bool (True when the append truncated)",
             py::arg("data"))
        .def("read",
             [](const OutputRing &r, uint64_t char_offset, uint64_t max_chars) -> py::bytes {
                 kimix::string out;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     r.read(char_offset, max_chars, out);
                 }
                 return to_bytes(out);
             },
             "read(char_offset, max_chars) -> bytes",
             py::arg("char_offset"), py::arg("max_chars"))
        .def("snapshot",
             [](const OutputRing &r) -> py::bytes {
                 kimix::string out;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     r.snapshot(out);
                 }
                 return to_bytes(out);
             },
             "snapshot() -> bytes")
        .def("clear", &OutputRing::clear)
        .def("char_count", &OutputRing::char_count)
        .def("byte_count", &OutputRing::byte_count)
        .def("truncated", &OutputRing::truncated)
        .def("cap_chars", &OutputRing::cap_chars)
        .def("bytes_owned", &OutputRing::bytes_owned)
        .def("__len__", &OutputRing::char_count);
}
