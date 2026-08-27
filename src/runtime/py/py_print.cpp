/*
 * py_print.cpp — Python bindings for the async print stream (runtime_py.print).
 *
 * BINDING-LAYER ONLY: this TU links against runtime.dll (pure C++ kernels)
 * and pybind11. The kernel call (enqueue to the concurrent queue) releases
 * the GIL via kimix::runtime::common::gil_scoped_release; bytes are extracted
 * with PyBytes_AsStringAndSize BEFORE the release.
 *
 * Contract: bytes in, None out. The Python shim (python/kimix_native/)
 * owns str<->bytes decoding. `native_print` writes the raw bytes as-is — it
 * does NOT append a newline or interpret printf-style format specifiers.
 */

#include <pybind11/pybind11.h>

#include <runtime/common/gil.h>
#include <runtime/print/print_stream.h>

namespace py = pybind11;

void py_register_print(py::module_& m) {
    m.doc() = "Async print stream (concurrent queue + background worker thread).";

    m.def("native_print",
          [](py::bytes data, bool flush) {
              char* buf = nullptr;
              Py_ssize_t len = 0;
              if (PyBytes_AsStringAndSize(data.ptr(), &buf, &len) < 0) {
                  throw py::error_already_set();
              }
              kimix::string_view view(buf, static_cast<size_t>(len));
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::print::print_stream().print(view, flush);
              }
          },
          "Queue raw UTF-8 bytes to the process-wide async print stream. The "
          "background worker writes them to stdout (fflushed when "
          "flush=True). No newline or printf formatting is added.",
          py::arg("data"), py::arg("flush") = true);
}
