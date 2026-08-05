/*
 * py_stream.cpp — Python bindings for the stream kernels (runtime_py.stream).
 *
 * BINDING-LAYER ONLY: this TU links against runtime.dll (pure C++ kernels)
 * and pybind11. Every kernel call releases the GIL via
 * kimix::runtime::common::gil_scoped_release; bytes are extracted with
 * PyBytes_AsStringAndSize BEFORE the release.
 *
 * Contract: bytes in, list[bytes] out. The Python shim (python/kimix_native/
 * stream.py) decodes to str and provides the `_compat` fallbacks.
 */

#include <pybind11/pybind11.h>

#include <runtime/runtime.h>
#include <runtime/common/gil.h>
#include <runtime/stream/ansi.h>
#include <runtime/stream/line_processor.h>

namespace py = pybind11;

namespace {

bool bytes_view(py::bytes data, kimix::string_view& view) {
    char* buf = nullptr;
    Py_ssize_t len = 0;
    if (PyBytes_AsStringAndSize(data.ptr(), &buf, &len) < 0) {
        return false;
    }
    view = kimix::string_view(buf, static_cast<size_t>(len));
    return true;
}

py::bytes to_bytes(const kimix::string& s) {
    return py::bytes(s.data(), s.size());
}

py::list lines_to_list(kimix::vector<kimix::string>& lines) {
    py::list out;
    for (auto& line : lines) {
        out.append(py::bytes(line.data(), line.size()));
    }
    return out;
}

} // namespace

void py_register_stream(py::module_& m) {
    m.doc() = "Stream kernels: ANSI strip + line stream processor (bytes in/out).";

    // ------------------------------------------------------------------
    // One-shot helpers
    // ------------------------------------------------------------------
    m.def("strip_ansi",
          [](py::bytes data) -> py::bytes {
              kimix::string_view view;
              if (!bytes_view(data, view)) {
                  throw py::error_already_set();
              }
              kimix::string result;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  result = kimix::runtime::stream::strip_ansi(view);
              }
              return to_bytes(result);
          },
          "One-shot ANSI escape stripping (no CRLF normalization).",
          py::arg("data"));

    m.def("filter_output",
          [](py::bytes data) -> py::bytes {
              kimix::string_view view;
              if (!bytes_view(data, view)) {
                  throw py::error_already_set();
              }
              kimix::string result;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  result = kimix::runtime::stream::filter_output(view);
              }
              return to_bytes(result);
          },
          "One-shot: strip ANSI + normalize CRLF/CR to LF (tools.common.py "
          "filter_output).",
          py::arg("data"));

    // ------------------------------------------------------------------
    // LineProcessor — py::class_ with the default unique_ptr holder keeps the
    // C++ object alive for the lifetime of the Python instance.
    // ------------------------------------------------------------------
    py::class_<kimix::runtime::stream::LineProcessor>(m, "LineProcessor")
        .def(py::init([](bool strip_ansi, uint32_t dedup_mode, uint32_t threshold,
                         uint32_t block_window, size_t max_bytes, size_t max_lines,
                         size_t fold_col) {
                 kimix::runtime::stream::process_options opts;
                 opts.strip_ansi = strip_ansi;
                 opts.dedup_mode = dedup_mode;
                 opts.threshold = threshold;
                 opts.block_window = block_window;
                 opts.max_bytes = max_bytes;
                 opts.max_lines = max_lines;
                 opts.fold_col = fold_col;
                 return kimix::runtime::stream::LineProcessor(opts);
             }),
             "Single-pass line stream processor (ANSI strip + CRLF + split + "
             "dedup + fold + budgets).",
             py::arg("strip_ansi") = true, py::arg("dedup_mode") = 0,
             py::arg("threshold") = 3, py::arg("block_window") = 3,
             py::arg("max_bytes") = 0, py::arg("max_lines") = 0,
             py::arg("fold_col") = 0)
        .def("feed",
             [](kimix::runtime::stream::LineProcessor& lp, py::bytes data) -> py::list {
                 kimix::string_view view;
                 if (!bytes_view(data, view)) {
                     throw py::error_already_set();
                 }
                 kimix::vector<kimix::string> lines;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     lp.feed(view, &lines);
                 }
                 return lines_to_list(lines);
             },
             "Process one byte chunk; returns completed lines (empty for "
             "dedup modes until flush).",
             py::arg("chunk"))
        .def("flush",
             [](kimix::runtime::stream::LineProcessor& lp) -> py::list {
                 kimix::vector<kimix::string> lines;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     lp.flush(&lines);
                 }
                 return lines_to_list(lines);
             },
             "End-of-stream: emit remaining lines (dedup modes emit here).")
        .def("reset", &kimix::runtime::stream::LineProcessor::reset,
             "Clear all state and counters.")
        .def("bytes_written", &kimix::runtime::stream::LineProcessor::bytes_written,
             "UTF-8 bytes of emitted line content (cumulative).")
        .def("code_points_written",
             &kimix::runtime::stream::LineProcessor::code_points_written,
             "Code points of emitted line content (cumulative).")
        .def("lines_written", &kimix::runtime::stream::LineProcessor::lines_written,
             "Number of emitted lines (cumulative).");
}
