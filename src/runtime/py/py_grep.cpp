/*
 * py_grep.cpp -- Python bindings for the native grep engine
 * (runtime_py.grep).
 *
 * BINDING-LAYER ONLY: this TU links against kimix-llm (pure C++ kernels:
 * builtin_tools/grep_engine + builtin_tools/regex_lite) and pybind11.
 * Every kernel call releases the GIL via
 * kimix::runtime::common::gil_scoped_release; Python objects are built only
 * after the release scope destructs.
 *
 * Exceptions are raised ONLY here (runtime_py is a kimix_exceptions_target);
 * the engine never throws -- failures travel in grep_result::status.
 *
 * INVARIANT: every engine string crosses into Python through to_py_str() --
 * a size-aware PyUnicode_DecodeUTF8(..., "surrogateescape") -- so embedded
 * NULs in content-mode text and ANSI-coded walk paths survive byte-exactly.
 * Pinned by python/tests/test_py_grep.py (test_nul_* / *utf8* cases).
 *
 * API:
 * grep.run(pattern: str, roots: list[str], work_dir: str,
 *          include_glob: str = "", mode: str = "files_with_matches",
 *          ignore_case: bool = False, ctx_before: int = 0,
 *          ctx_after: int = 0, head_limit: int = 250) -> dict
 *   keys: ok(bool) status(str) message(str) total_matches(int)
 *         files(list[tuple[str,int]]) lines(list[str]) line_match(list[int])
 * grep.pattern_supported(pattern: str, ignore_case: bool = False) -> bool
 */

#include <pybind11/pybind11.h>

#include <cstdint>

#include <runtime/common/gil.h>

#include <builtin_tools/grep_engine.h>
#include <builtin_tools/regex_lite.h>
#include <builtin_tools/tool_types.h>

namespace py = pybind11;

namespace {

namespace ggrep = kimix::builtin_tools::grep;

// Lowercase snake name of a tool_status enumerator (same spelling as the C++
// enumerator, so the Python side can compare against the engine vocabulary).
const char *status_name(kimix::builtin_tools::tool_status st) {
  switch (st) {
  case kimix::builtin_tools::tool_status::ok:
    return "ok";
  case kimix::builtin_tools::tool_status::invalid_input:
    return "invalid_input";
  case kimix::builtin_tools::tool_status::not_found:
    return "not_found";
  case kimix::builtin_tools::tool_status::no_change:
    return "no_change";
  case kimix::builtin_tools::tool_status::ambiguous:
    return "ambiguous";
  case kimix::builtin_tools::tool_status::blocked:
    return "blocked";
  case kimix::builtin_tools::tool_status::too_large:
    return "too_large";
  case kimix::builtin_tools::tool_status::unsupported:
    return "unsupported";
  case kimix::builtin_tools::tool_status::external_library:
    return "external_library";
  }
  return "invalid_input";
}

// str -> py str. SIZE-AWARE and byte-exact -- do not "simplify" this to
// py::str(s.data()) / a c_str() based ctor: engine text can legitimately
// contain embedded NULs (a file whose NUL sits past grep_engine's 64 KiB
// binary-sniff window is still scanned, and its matched line is rendered
// verbatim), and a NUL-terminated conversion would silently TRUNCATE the
// rendered line at that byte. Paths are also not guaranteed UTF-8: the engine
// renders walk paths through the narrow/ANSI convention (kimix::to_string of
// an fs::path), so a non-ASCII path arrives in the process ANSI code page.
// "surrogateescape" keeps both intact and re-encodable with
// .encode("utf-8", "surrogateescape") instead of raising UnicodeDecodeError
// out of the middle of the dict build. GIL must be held here (CPython API).
py::str to_py_str(const kimix::string &s) {
  PyObject *o = PyUnicode_DecodeUTF8(
      s.data(), static_cast<Py_ssize_t>(s.size()), "surrogateescape");
  if (o == nullptr) {
    throw py::error_already_set();
  }
  return py::reinterpret_steal<py::str>(o);
}

bool parse_mode(py::str mode, ggrep::grep_output_mode &out) {
  const kimix::string m = mode.cast<kimix::string>();
  if (m == "files_with_matches") {
    out = ggrep::grep_output_mode::files_with_matches;
    return true;
  }
  if (m == "count_matches") {
    out = ggrep::grep_output_mode::count_matches;
    return true;
  }
  if (m == "content") {
    out = ggrep::grep_output_mode::content;
    return true;
  }
  return false;
}

} // namespace

void py_register_grep(py::module_ &m) {
  m.doc() = "Grep kernels (native grep_engine scan + regex_lite pattern "
            "compile probe).";

  // ------------------------------------------------------------------
  // run() -- full recursive content scan over `roots`.
  // ------------------------------------------------------------------
  m.def(
      "run",
      [](py::str pattern, py::list roots, py::str work_dir, py::str include_glob,
         py::str mode, bool ignore_case, int64_t ctx_before, int64_t ctx_after,
         int64_t head_limit) -> py::dict {
        ggrep::grep_options opts;
        opts.pattern = pattern.cast<kimix::string>();
        opts.include_glob = include_glob.cast<kimix::string>();
        opts.ignore_case = ignore_case;
        opts.head_limit = head_limit;
        if (!parse_mode(mode, opts.mode)) {
          throw py::value_error(
              "mode must be one of files_with_matches|count_matches|content");
        }
        if (ctx_before < 0 || ctx_after < 0) {
          throw py::value_error("ctx_before/ctx_after must be >= 0");
        }
        opts.ctx_before = static_cast<uint32_t>(ctx_before);
        opts.ctx_after = static_cast<uint32_t>(ctx_after);

        kimix::vector<kimix::string> root_vec;
        const Py_ssize_t n = PyList_GET_SIZE(roots.ptr());
        root_vec.reserve(static_cast<size_t>(n));
        for (Py_ssize_t i = 0; i < n; ++i) {
          py::handle item = PyList_GET_ITEM(roots.ptr(), i);
          if (!PyUnicode_Check(item.ptr())) {
            throw py::type_error("roots must be list[str]");
          }
          root_vec.push_back(item.cast<kimix::string>());
        }
        const kimix::string base = work_dir.cast<kimix::string>();

        ggrep::grep_result res;
        kimix::builtin_tools::tool_status st =
            kimix::builtin_tools::tool_status::ok;
        {
          kimix::runtime::common::gil_scoped_release release;
          st = ggrep::run_grep(
              opts, kimix::span<const kimix::string>(root_vec.data(),
                                                     root_vec.size()),
              kimix::string_view(base.data(), base.size()), res);
        }

        // GIL re-acquired: build the Python objects now.
        py::dict out;
        out["ok"] = (st == kimix::builtin_tools::tool_status::ok);
        out["status"] = status_name(st);
        out["message"] = to_py_str(res.message);
        out["total_matches"] = res.total_matches;

        py::list files;
        for (const auto &f : res.files) {
          files.append(py::make_tuple(to_py_str(f.path), f.match_count));
        }
        out["files"] = files;

        py::list lines;
        for (const auto &l : res.lines) {
          lines.append(to_py_str(l));
        }
        out["lines"] = lines;

        py::list line_match;
        for (uint8_t flag : res.line_match) {
          line_match.append(static_cast<int>(flag));
        }
        out["line_match"] = line_match;
        return out;
      },
      "Run the native grep engine over `roots` (files or directories; relative "
      "entries resolve against work_dir). Returns a dict with keys ok, status, "
      "message, total_matches, files (list[(path, lines)]), lines, line_match. "
      "The GIL is released for the whole scan. Walk paths come back in the "
      "engine's narrow/ANSI form: they are decoded with the surrogateescape "
      "error handler, so a non-ASCII path never raises -- re-encode with "
      ".encode('utf-8', 'surrogateescape') to recover the engine's bytes.",
      py::arg("pattern"), py::arg("roots"), py::arg("work_dir"),
      py::arg("include_glob") = "",
      py::arg("mode") = "files_with_matches",
      py::arg("ignore_case") = false, py::arg("ctx_before") = 0,
      py::arg("ctx_after") = 0, py::arg("head_limit") = 250);

  // ------------------------------------------------------------------
  // pattern_supported() -- regex_lite compile probe (no scan).
  // ------------------------------------------------------------------
  m.def(
      "pattern_supported",
      [](py::str pattern, bool ignore_case) -> bool {
        const kimix::string pat = pattern.cast<kimix::string>();
        bool ok = false;
        {
          kimix::runtime::common::gil_scoped_release release;
          kimix::builtin_tools::regex_lite::Regex rx;
          kimix::string error;
          ok = rx.compile(kimix::string_view(pat.data(), pat.size()),
                          ignore_case, error) &&
               rx.valid();
        }
        return ok;
      },
      "True when regex_lite compiles `pattern` (no back-references, look-around, "
      "possessive quantifiers or named groups).",
      py::arg("pattern"), py::arg("ignore_case") = false);
}
