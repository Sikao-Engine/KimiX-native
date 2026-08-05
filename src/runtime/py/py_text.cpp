/*
 * py_text.cpp — Python bindings for the text kernels (runtime_py.text).
 *
 * BINDING-LAYER ONLY: this TU links against runtime.dll (pure C++ kernels)
 * and pybind11. Every kernel call releases the GIL via
 * kimix::runtime::common::gil_scoped_release; bytes are extracted with
 * PyBytes_AsStringAndSize BEFORE the release.
 *
 * CRITICAL: no Python object (py::bytes / py::make_tuple / py::list) may be
 * created while the GIL is released — the kernel runs inside a scoped block
 * `{ gil_scoped_release release; ... }` and Python objects are built after
 * the scope closes (GIL reacquired).
 *
 * Contract: bytes in, bytes/int out. The Python shim (python/kimix_native/
 * text.py) handles str<->bytes and the NFC hook (unicodedata.normalize)
 * between sanitize_pre_nfc and sanitize_post_nfc.
 */

#include <pybind11/pybind11.h>

#include <runtime/runtime.h>
#include <runtime/common/gil.h>
#include <runtime/text/token_count.h>
#include <runtime/text/sanitize.h>

namespace py = pybind11;

namespace {

// Extract a string_view over a py::bytes WITHOUT copying (valid until the
// py::bytes object is destroyed; callers must not touch Python while the
// view is alive and the GIL is released).
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

} // namespace

void py_register_text(py::module_& m) {
    m.doc() = "Text kernels: heuristic token count + sanitizer (bytes in/out).";

    // ------------------------------------------------------------------
    // Heuristic token counting (plan 001)
    // ------------------------------------------------------------------
    m.def("count_tokens",
          [](py::bytes data, py::object /*model*/) -> int {
              kimix::string_view view;
              if (!bytes_view(data, view)) {
                  throw py::error_already_set();
              }
              int result = 0;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  result = kimix::runtime::text::estimate_chars_tokens(view);
              }
              return result;
          },
          "Heuristic token count (bytes in, int out). The optional model is "
          "handled by the Python shim (tiktoken branch); the kernel ignores it.",
          py::arg("data"), py::arg("model") = py::none());

    m.def("estimate_chars_tokens",
          [](py::bytes data) -> int {
              kimix::string_view view;
              if (!bytes_view(data, view)) {
                  throw py::error_already_set();
              }
              int result = 0;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  result = kimix::runtime::text::estimate_chars_tokens(view);
              }
              return result;
          },
          "Mirror of tokens.py::_estimate_chars_tokens (bytes in, int out).",
          py::arg("data"));

    m.def("is_cjk_text",
          [](py::bytes data, double threshold) -> bool {
              kimix::string_view view;
              if (!bytes_view(data, view)) {
                  throw py::error_already_set();
              }
              bool result = false;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  result = kimix::runtime::text::is_cjk_text(view, threshold);
              }
              return result;
          },
          "Mirror of tokens.py::_is_cjk_text (strict > threshold).",
          py::arg("data"), py::arg("threshold") = 0.15);

    m.def("is_cjk_cp",
          [](uint32_t cp) -> bool {
              return kimix::runtime::text::is_cjk_cp(cp);
          },
          "True when cp falls in one of the 7 _CJK_RE ranges.",
          py::arg("cp"));

    m.def("scan_utf8",
          [](py::bytes data) -> py::tuple {
              kimix::string_view view;
              if (!bytes_view(data, view)) {
                  throw py::error_already_set();
              }
              kimix::runtime::text::count_stats st;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  st = kimix::runtime::text::scan_utf8(view);
              }
              return py::make_tuple(st.code_points, st.ascii);
          },
          "One-pass scan: (code_points, ascii_count).",
          py::arg("data"));

    // ------------------------------------------------------------------
    // Sanitizer (plan 002)
    // ------------------------------------------------------------------
    m.def("sanitize_for_tokenizer",
          [](py::bytes data, uint32_t max_chars, uint32_t max_repeat,
             py::bytes truncate_msg) -> py::bytes {
              kimix::string_view view;
              kimix::string_view msg;
              if (!bytes_view(data, view) || !bytes_view(truncate_msg, msg)) {
                  throw py::error_already_set();
              }
              kimix::runtime::text::sanitize_options opts;
              opts.max_chars = max_chars;
              opts.max_repeat = max_repeat;
              opts.truncate_msg = msg;
              kimix::string result;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  result = kimix::runtime::text::sanitize_for_tokenizer(view, opts);
              }
              return to_bytes(result);
          },
          "Full pipeline sans NFC (exact for pure-ASCII input).",
          py::arg("data"), py::arg("max_chars") = 0,
          py::arg("max_repeat") = 100, py::arg("truncate_msg") = py::bytes());

    m.def("sanitize_pre_nfc",
          [](py::bytes data) -> py::bytes {
              kimix::string_view view;
              if (!bytes_view(data, view)) {
                  throw py::error_already_set();
              }
              kimix::string result;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  result = kimix::runtime::text::sanitize_pre_nfc(view);
              }
              return to_bytes(result);
          },
          "Steps 2-5, 6a, 6b: surrogates, noncharacters, PUA, U+FFFD, "
          "zero-width, controls. The shim runs NFC on the result, then "
          "sanitize_post_nfc.",
          py::arg("data"));

    m.def("sanitize_post_nfc",
          [](py::bytes data, uint32_t max_chars, uint32_t max_repeat,
             py::bytes truncate_msg) -> py::bytes {
              kimix::string_view view;
              kimix::string_view msg;
              if (!bytes_view(data, view) || !bytes_view(truncate_msg, msg)) {
                  throw py::error_already_set();
              }
              kimix::runtime::text::sanitize_options opts;
              opts.max_chars = max_chars;
              opts.max_repeat = max_repeat;
              opts.truncate_msg = msg;
              kimix::string result;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  result = kimix::runtime::text::sanitize_post_nfc(view, opts);
              }
              return to_bytes(result);
          },
          "Steps 6d (strip) + 7 (dedupe repeats) + 8 (truncate).",
          py::arg("data"), py::arg("max_chars") = 0,
          py::arg("max_repeat") = 100, py::arg("truncate_msg") = py::bytes());

    m.def("clean_text",
          [](py::bytes data, bool keep_newlines) -> py::bytes {
              kimix::string_view view;
              if (!bytes_view(data, view)) {
                  throw py::error_already_set();
              }
              kimix::string result;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  result = kimix::runtime::text::clean_text(view, keep_newlines);
              }
              return to_bytes(result);
          },
          "Zero-width + controls + strip (NFC applied by the shim hook).",
          py::arg("data"), py::arg("keep_newlines") = true);

    m.def("strip_controls",
          [](py::bytes data, bool keep_newlines) -> py::bytes {
              kimix::string_view view;
              if (!bytes_view(data, view)) {
                  throw py::error_already_set();
              }
              kimix::string result;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  result = kimix::runtime::text::strip_controls(view, keep_newlines);
              }
              return to_bytes(result);
          },
          "C0/C1 control removal only.",
          py::arg("data"), py::arg("keep_newlines") = true);
}
