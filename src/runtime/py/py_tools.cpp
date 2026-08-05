/*
 * py_tools.cpp - Python bindings for the tool kernels (runtime_py.tools).
 *
 * BINDING-LAYER ONLY: links against runtime.dll (pure C++ kernels) and
 * pybind11. Every kernel call releases the GIL via
 * kimix::runtime::common::gil_scoped_release; Python objects are built only
 * after the release scope closes.
 *
 * API:
 *   tools.line_hash(line: bytes, seed: int) -> int
 *       compute_line_hash(line, seed) & 0xFF (exact xxh32 recipe).
 *   tools.line_hashes(content: bytes, seed: int) -> list[int]
 *       chained per-line hashes (reference seed semantics).
 *   tools.find_in_file(content, needle, case_insensitive=True) -> list[(int,int,int)]
 *       (line_index 0-based, col 0-based, length) per line, overlapping,
 *       readlines() terminator semantics. ASCII-only folding; the shim routes
 *       non-ASCII to the _compat mirror.
 *   tools.scan_lines(content, pattern, case_insensitive=True) -> list[(int,int,int)]
 *       (line_index 0-based, byte_offset, line_len) for lines containing the
 *       literal pattern (substring matcher, ASCII fold).
 *   tools.scan_lines_cb(content, callback) -> list[(int,int,int)]
 *       same offsets, but the matcher is a Python callable invoked per line
 *       (GIL held; full regex semantics stay in Python, offsets stay native).
 */

#include <pybind11/pybind11.h>

#include <runtime/runtime.h>
#include <runtime/common/gil.h>
#include <runtime/tools/line_hash.h>
#include <runtime/tools/find_str.h>
#include <runtime/tools/grep_scan.h>
#include <runtime/tools/export_builder.h>
#include <runtime/py/py_soul_bridge.h>

namespace py = pybind11;

namespace {

// Convert a Python str to UTF-8 bytes held in a kimix::string (surrogate-ok
// via PyUnicode_AsUTF8AndSize -- strings are caller-provided).
bool str_to_string(py::handle obj, kimix::string& out) {
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

bool bytes_view(py::bytes data, kimix::string_view& view) {
    char* buf = nullptr;
    Py_ssize_t len = 0;
    if (PyBytes_AsStringAndSize(data.ptr(), &buf, &len) < 0) {
        return false;
    }
    view = kimix::string_view(buf, static_cast<size_t>(len));
    return true;
}

inline uint8_t fold_ascii(uint8_t c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<uint8_t>(c + 32) : c;
}

py::list matches_to_list(const kimix::vector<kimix::runtime::tools::find_match>& ms) {
    py::list out;
    for (const auto& m : ms) {
        out.append(py::make_tuple(m.line_index, m.col, m.length));
    }
    return out;
}

py::list hits_to_list(const kimix::vector<kimix::runtime::tools::grep_hit>& hs) {
    py::list out;
    for (const auto& h : hs) {
        out.append(py::make_tuple(h.line_index, h.byte_offset, h.line_len));
    }
    return out;
}

} // namespace

void py_register_tools(py::module_& m) {
    m.doc() = "Tool kernels (line hashing, string find, grep line scan)";

    m.def("line_hash",
          [](py::bytes line, uint32_t seed) -> uint32_t {
              kimix::string_view view;
              if (!bytes_view(line, view)) {
                  throw py::error_already_set();
              }
              uint32_t h = 0;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  h = kimix::runtime::tools::compute_line_hash(view, seed);
              }
              return h;
          },
          "Exact hash_line recipe for one line with a final 32-bit seed: "
          "strip trailing CR, filter whitespace, xxh32 & 0xFF.",
          py::arg("line"), py::arg("seed"));

    m.def("line_hashes",
          [](py::bytes content, uint32_t seed) -> py::list {
              kimix::string_view view;
              if (!bytes_view(content, view)) {
                  throw py::error_already_set();
              }
              kimix::vector<uint32_t> hashes;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::tools::compute_line_hashes(view, seed, hashes);
              }
              py::list out;
              for (uint32_t h : hashes) {
                  out.append(h);
              }
              return out;
          },
          "Chained per-line hashes (reference seed semantics).",
          py::arg("content"), py::arg("seed"));

    m.def("find_in_file",
          [](py::bytes content, py::bytes needle, bool case_insensitive) -> py::list {
              kimix::string_view cview, nview;
              if (!bytes_view(content, cview) || !bytes_view(needle, nview)) {
                  throw py::error_already_set();
              }
              kimix::vector<kimix::runtime::tools::find_match> ms;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::tools::find_in_file(cview, nview, case_insensitive, ms);
              }
              return matches_to_list(ms);
          },
          "All per-line occurrences of the needle (overlapping; lines keep "
          "their terminators). ASCII-only case folding.",
          py::arg("content"), py::arg("needle"), py::arg("case_insensitive") = true);

    m.def("scan_lines",
          [](py::bytes content, py::bytes pattern, bool case_insensitive) -> py::list {
              kimix::string_view cview, pview;
              if (!bytes_view(content, cview) || !bytes_view(pattern, pview)) {
                  throw py::error_already_set();
              }
              kimix::vector<uint8_t> pf;
              pf.reserve(pview.size());
              for (unsigned char c : pview) {
                  pf.push_back(case_insensitive ? fold_ascii(c) : c);
              }
              kimix::vector<kimix::runtime::tools::grep_hit> hits;
              const bool empty_pattern = pf.empty();
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::tools::scan_lines(
                      cview,
                      [&](kimix::string_view line, uint32_t) -> bool {
                          if (empty_pattern) {
                              return false; // backup_grep rejects empty patterns
                          }
                          // substring test with on-the-fly ASCII folding
                          const size_t n = line.size();
                          const size_t m = pf.size();
                          if (m > n) {
                              return false;
                          }
                          for (size_t i = 0; i + m <= n; ++i) {
                              size_t k = 0;
                              while (k < m) {
                                  uint8_t lc = static_cast<uint8_t>(line[i + k]);
                                  if (case_insensitive) {
                                      lc = fold_ascii(lc);
                                  }
                                  if (lc != pf[k]) {
                                      break;
                                  }
                                  ++k;
                              }
                              if (k == m) {
                                  return true;
                              }
                          }
                          return false;
                      },
                      hits);
              }
              return hits_to_list(hits);
          },
          "Lines containing the literal pattern (substring matcher, ASCII "
          "fold). Returns (line_index 0-based, byte_offset, line_len).",
          py::arg("content"), py::arg("pattern"), py::arg("case_insensitive") = true);

    m.def("scan_lines_cb",
          [](py::bytes content, py::object callback) -> py::list {
              kimix::string_view cview;
              if (!bytes_view(content, cview)) {
                  throw py::error_already_set();
              }
              if (!PyCallable_Check(callback.ptr())) {
                  throw py::type_error("callback must be callable");
              }
              kimix::vector<kimix::runtime::tools::grep_hit> hits;
              // GIL is held for the whole call: the callback needs it, and the
              // kernel itself never touches Python.
              kimix::runtime::tools::scan_lines(
                  cview,
                  [&](kimix::string_view line, uint32_t line_index) -> bool {
                      py::object result =
                          callback(py::bytes(line.data(), line.size()), line_index);
                      return result.cast<bool>();
                  },
                  hits);
              return hits_to_list(hits);
          },
          "Per-line matcher callback (called with (line_bytes, line_index)); "
          "offsets are computed natively.",
          py::arg("content"), py::arg("callback"));

    // ------------------------------------------------------------------
    // build_export_markdown (plan 016) -- full session export markdown.
    // ------------------------------------------------------------------
    m.def("build_export_markdown",
          [](py::bytes history, py::dict structure, py::dict opts) -> py::bytes {
              kimix_soul_bridge::bridge b;
              if (!kimix_soul_bridge::parse_structure(structure, history, b)) {
                  throw py::value_error("invalid structure/history");
              }
              kimix::vector<kimix::runtime::soul::message_view> msgs =
                  kimix_soul_bridge::assemble(b);

              kimix::string session_id, work_dir, exported_at;
              uint64_t token_count = 0;
              bool include_timestamps = false;
              if (opts.contains("session_id") &&
                  !str_to_string(opts["session_id"], session_id)) {
                  throw py::type_error("session_id must be str");
              }
              if (opts.contains("work_dir") &&
                  !str_to_string(opts["work_dir"], work_dir)) {
                  throw py::type_error("work_dir must be str");
              }
              if (opts.contains("exported_at") &&
                  !str_to_string(opts["exported_at"], exported_at)) {
                  throw py::type_error("exported_at must be str");
              }
              if (opts.contains("token_count")) {
                  token_count = opts["token_count"].cast<uint64_t>();
              }
              if (opts.contains("include_timestamps")) {
                  include_timestamps = opts["include_timestamps"].cast<bool>();
              }

              kimix::runtime::tools::export_options eo;
              eo.session_id = session_id;
              eo.work_dir = work_dir;
              eo.exported_at = exported_at;
              eo.token_count = token_count;
              eo.include_timestamps = include_timestamps;

              kimix::string out;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::tools::build_export_markdown(msgs, eo, out);
              }
              return kimix_soul_bridge::bridge_to_bytes(out);
          },
          "Full session export markdown (reference export.py build_export_"
          "markdown); opts keys: session_id, work_dir, exported_at, "
          "token_count, include_timestamps.",
          py::arg("history"), py::arg("structure"), py::arg("opts"));
}
