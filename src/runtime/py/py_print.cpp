/*
 * py_print.cpp — Python bindings for the async print stream (runtime_py.print).
 *
 * BINDING-LAYER ONLY: this TU links against runtime.dll (pure C++ kernels)
 * and pybind11. The kernel call (enqueue to the concurrent queue) releases
 * the GIL via kimix::runtime::common::gil_scoped_release; the payload is
 * fully materialized as UTF-8 bytes BEFORE the release.
 *
 * Contract: `native_print` embeds the whole builtin-print contract that used
 * to live in kimix.ui.printing._native_print_func, so the Python layer is a
 * thin forwarder with no per-call logic:
 *   - every value is coerced with str() and the pieces are joined by `sep`
 *     (" " when None), then `end` ("\n" when None) is appended;
 *   - text is encoded UTF-8 with "surrogatepass" (lone surrogates tolerated,
 *     byte-identical to Python's .encode("utf-8", "surrogatepass"));
 *   - only stdout is natively supported: any other `file` (identity-compared
 *     against sys.stdout) delegates to builtins.print;
 *   - writes go through the process-wide async PrintStream — GIL released,
 *     fflushed when flush=True.
 *
 * Legacy signature preserved: native_print(b"raw", flush=True) still takes
 * the raw-payload path (bytes queued verbatim — no coercion, no newline
 * appended), matching the pre-refactor "bytes in" binding.
 */
#include <pybind11/pybind11.h>

#include <cstring>

#include <runtime/common/gil.h>
#include <runtime/print/print_stream.h>

namespace py = pybind11;

namespace {

// A UTF-8 byte slice. `owner` keeps whatever object backs `data` alive
// (either the source unicode/str object or an encoded py::bytes); the literal
// defaults used for None point at static storage, hence the raw-ctor overload.
struct utf8_piece {
    utf8_piece() : owner(), data(nullptr), size(0) {}
    utf8_piece(py::object obj, const char* d, size_t n)
        : owner(std::move(obj)), data(d), size(n) {}
    py::object owner;
    const char* data;
    size_t size;
};

// Encode an already-coerced unicode object to UTF-8 bytes. Fast path hands
// out CPython's internal buffer; lone surrogates fall back to
// obj.encode("utf-8", "surrogatepass") so the bytes match the Python layer.
utf8_piece encode_utf8_surrogatepass(py::object u) {
    Py_ssize_t size = 0;
    const char* data = PyUnicode_AsUTF8AndSize(u.ptr(), &size);
    if (data != nullptr) {
        return utf8_piece{std::move(u), data, static_cast<size_t>(size)};
    }
    PyErr_Clear();
    PyObject* encoded =
        PyObject_CallMethod(u.ptr(), "encode", "(ss)", "utf-8", "surrogatepass");
    if (encoded == nullptr) {
        throw py::error_already_set();
    }
    char* edata = nullptr;
    Py_ssize_t esize = 0;
    if (PyBytes_AsStringAndSize(encoded, &edata, &esize) < 0) {
        Py_DECREF(encoded);
        throw py::error_already_set();
    }
    return utf8_piece{py::reinterpret_steal<py::object>(encoded), edata,
                      static_cast<size_t>(esize)};
}

// Mirrors builtin print's TypeError wording: "sep must be None or a string".
void require_none_or_str(const char* name, const py::handle& obj) {
    if (!obj.is_none() && !PyUnicode_Check(obj.ptr())) {
        throw py::type_error(std::string(name) +
                             " must be None or a string, not " +
                             Py_TYPE(obj.ptr())->tp_name);
    }
}

} // namespace

void py_register_print(py::module_& m) {
    m.doc() = "Async print stream (concurrent queue + background worker thread).";
    m.def("native_print",
          [](py::args values,
             const py::object& sep,
             const py::object& end,
             const py::object& file,
             bool flush) {
              using kimix::runtime::print::print_stream;

              // ---- Legacy raw-bytes path: one positional `bytes` argument
              // queues verbatim (pre-refactor contract: no coercion, no
              // appended newline, no printf interpretation).
              if (values.size() == 1 && PyBytes_Check(values[0].ptr())) {
                  char* buf = nullptr;
                  Py_ssize_t len = 0;
                  if (PyBytes_AsStringAndSize(values[0].ptr(), &buf, &len) < 0) {
                      throw py::error_already_set();
                  }
                  kimix::string_view view(buf, static_cast<size_t>(len));
                  {
                      kimix::runtime::common::gil_scoped_release release;
                      print_stream().print(view, flush);
                  }
                  return;
              }

              // ---- Only stdout is natively supported: any other file object
              // falls back to builtins.print (identity check, like
              // `file is sys.stdout`). sep/end=None are valid for both paths.
              bool to_stdout = true;
              if (!file.is_none()) {
                  py::object sys_stdout =
                      py::module_::import("sys").attr("stdout");
                  to_stdout = file.is(sys_stdout);
              }
              if (!to_stdout) {
                  py::object builtin_print =
                      py::module_::import("builtins").attr("print");
                  py::dict kwargs;
                  kwargs["sep"] = sep;
                  kwargs["end"] = end;
                  kwargs["file"] = file;
                  kwargs["flush"] = flush;
                  PyObject* result =
                      PyObject_Call(builtin_print.ptr(), values.ptr(),
                                    kwargs.ptr());
                  if (result == nullptr) {
                      throw py::error_already_set();
                  }
                  Py_DECREF(result);
                  return;
              }

              // ---- Validate argument types up front (like builtin print),
              // then materialize every value as a UTF-8 piece while the GIL
              // is held.
              require_none_or_str("sep", sep);
              require_none_or_str("end", end);
              const utf8_piece sep_piece = sep.is_none()
                                               ? utf8_piece{py::object(), " ", 1}
                                               : encode_utf8_surrogatepass(sep);
              const utf8_piece end_piece = end.is_none()
                                               ? utf8_piece{py::object(), "\n", 1}
                                               : encode_utf8_surrogatepass(end);

              kimix::vector<utf8_piece> parts;
              parts.reserve(values.size());
              size_t total = end_piece.size;
              for (auto handle : values) {
                  PyObject* coerced = PyObject_Str(handle.ptr());
                  if (coerced == nullptr) {
                      throw py::error_already_set();
                  }
                  utf8_piece part = encode_utf8_surrogatepass(
                      py::reinterpret_steal<py::object>(coerced));
                  total += part.size +
                           (parts.empty() ? size_t(0) : sep_piece.size);
                  parts.push_back(std::move(part));
              }

              // Stitch sep / values / end into one payload so the enqueue
              // below stays a single queue operation.
              kimix::string payload;
              payload.resize(total);
              char* dst = payload.data();
              const auto append = [&dst](const utf8_piece& piece) {
                  if (piece.size != 0) {
                      std::memcpy(dst, piece.data, piece.size);
                      dst += piece.size;
                  }
              };
              for (size_t i = 0; i < parts.size(); ++i) {
                  if (i != 0) {
                      append(sep_piece);
                  }
                  append(parts[i]);
              }
              append(end_piece);

              const kimix::string_view view(payload.data(), payload.size());
              {
                  kimix::runtime::common::gil_scoped_release release;
                  print_stream().print(view, flush);
              }
          },
          "Print like builtin print() through the process-wide async print\n"
          "stream: every value is str()-coerced, pieces are joined by `sep`\n"
          "(\" \" when None), `end` (\"\\n\" when None) is appended, and the\n"
          "result is encoded UTF-8 with surrogatepass before it is queued.\n"
          "The background worker writes to stdout (fflushed when flush=True)\n"
          "with the GIL released. Any `file` other than sys.stdout falls back\n"
          "to builtins.print. Passing a single positional bytes object uses\n"
          "the legacy raw-payload path (queued verbatim).",
          py::arg("sep") = py::str(" "),
          py::arg("end") = py::str("\n"),
          py::arg("file") = py::none(),
          py::arg("flush") = true);
}
