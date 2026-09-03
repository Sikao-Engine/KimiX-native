/*
 * py_parse.cpp - Python bindings for the parse kernels (runtime_py.parse).
 *
 * BINDING-LAYER ONLY: links against runtime.dll (pure C++ kernels) and
 * pybind11. Every kernel call releases the GIL via
 * kimix::runtime::common::gil_scoped_release. Contract: bytes in / bytes
 * out (the Python shim handles str<->bytes with surrogatepass).
 *
 * CRITICAL: no Python object may be created while the GIL is released - the
 * kernel runs inside `{ gil_scoped_release release; ... }` and Python
 * objects are built only after the scope closes (GIL reacquired).
 *
 * API (documented deviations from the plan, all additive):
 *   parse.comment_spans(lang: str, data: bytes) -> list[(int, int, int)]
 *       (start, end, kind) byte-offset spans; the shim slices content and
 *       computes 1-based line/column.
 *   parse.shell_scan(dialect: str, cmd: bytes) -> (edits, names, notes,
 *                                                   nul_notes)
 *       edits = list[(start, end, bytes)]; names/notes/nul_notes = list[bytes]
 *       (BASH_FIX only; empty for the other dialects). The plan's single
 *       edit list is returned as element 0 of a 4-tuple so fallback names,
 *       path notes and nul redirection targets (needed to rebuild the
 *       bash_fix prefix and warnings) travel with the scan.
 *   parse.shell_transform(dialect: str, cmd: bytes) -> bytes
 *       transformed command. For BASH_FIX the kernel cannot build the
 *       fallback-definitions prefix (the definitions live in the shim), so
 *       the binding applies the edits only; the shim composes the full
 *       fix_bash_command output.
 *   parse.pwsh_fix_warning(cmd: bytes) -> int
 *       repair outcome code (0 = valid, 1..9 kinds, 0x10 continuation flag,
 *       -1 = unrepairable); the shim maps codes to warning strings.
 */

#include <pybind11/pybind11.h>

#include <runtime/runtime.h>
#include <runtime/common/gil.h>
#include <runtime/parse/comment_scanner.h>
#include <runtime/parse/shell_scanner.h>

#include <string>

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

bool parse_lang(const std::string& name, kimix::runtime::parse::lang_kind& out) {
    if (name == "c") { out = kimix::runtime::parse::lang_kind::C; return true; }
    if (name == "python") { out = kimix::runtime::parse::lang_kind::PYTHON; return true; }
    if (name == "shell") { out = kimix::runtime::parse::lang_kind::SHELL; return true; }
    if (name == "sql") { out = kimix::runtime::parse::lang_kind::SQL; return true; }
    if (name == "html") { out = kimix::runtime::parse::lang_kind::HTML; return true; }
    if (name == "lisp") { out = kimix::runtime::parse::lang_kind::LISP; return true; }
    if (name == "pascal") { out = kimix::runtime::parse::lang_kind::PASCAL_LANG; return true; }
    return false;
}

bool parse_dialect(const std::string& name,
                   kimix::runtime::parse::shell_dialect& out) {
    if (name == "bash_fix") { out = kimix::runtime::parse::shell_dialect::BASH_FIX; return true; }
    if (name == "bash_process_unquoted") {
        out = kimix::runtime::parse::shell_dialect::BASH_PROCESS_UNQUOTED;
        return true;
    }
    if (name == "pwsh_fix") { out = kimix::runtime::parse::shell_dialect::PWSH_FIX; return true; }
    if (name == "pwsh_transform") {
        out = kimix::runtime::parse::shell_dialect::PWSH_TRANSFORM;
        return true;
    }
    return false;
}

py::list spans_to_list(const kimix::vector<kimix::runtime::parse::comment_span>& spans) {
    py::list out;
    for (const auto& s : spans) {
        out.append(py::make_tuple(s.start, s.end, s.kind));
    }
    return out;
}

py::tuple scan_to_tuple(kimix::runtime::parse::shell_dialect d, py::bytes cmd) {
    kimix::string_view view;
    if (!bytes_view(cmd, view)) {
        throw py::error_already_set();
    }
    kimix::vector<kimix::runtime::parse::edit> edits;
    kimix::vector<kimix::string> names;
    kimix::vector<kimix::string> notes;
    kimix::vector<kimix::string> nul_notes;
    kimix::string transformed;
    int warning = 0;
    {
        kimix::runtime::common::gil_scoped_release release;
        kimix::runtime::parse::scan_shell(d, view, edits, &transformed, &names,
                                          &notes, &warning, nullptr, &nul_notes);
    }
    py::list el;
    for (const auto& e : edits) {
        el.append(py::make_tuple(e.start, e.end,
                                 py::bytes(e.replacement.data(), e.replacement.size())));
    }
    py::list nl;
    for (const auto& nm : names) {
        nl.append(py::bytes(nm.data(), nm.size()));
    }
    py::list nt;
    for (const auto& no : notes) {
        nt.append(py::bytes(no.data(), no.size()));
    }
    py::list nn;
    for (const auto& nu : nul_notes) {
        nn.append(py::bytes(nu.data(), nu.size()));
    }
    return py::make_tuple(el, nl, nt, nn);
}

} // namespace

void py_register_parse(py::module_& m) {
    m.doc() = "Parse kernels (comment parsers, bash/pwsh command scanners)";

    m.def("comment_spans",
          [](py::str lang, py::bytes data) -> py::list {
              kimix::runtime::parse::lang_kind kind;
              if (!parse_lang(lang.cast<std::string>(), kind)) {
                  throw py::value_error("unknown language: " + lang.cast<std::string>());
              }
              kimix::string_view view;
              if (!bytes_view(data, view)) {
                  throw py::error_already_set();
              }
              kimix::vector<kimix::runtime::parse::comment_span> spans;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::parse::scan_comments(kind, view, spans);
              }
              return spans_to_list(spans);
          },
          "Extract (start, end, kind) comment spans (UTF-8 byte offsets). "
          "kind: 0 = line, 1 = block, 2 = doc.",
          py::arg("lang"), py::arg("data"));

    m.def("shell_scan",
          [](py::str dialect, py::bytes cmd) -> py::tuple {
              kimix::runtime::parse::shell_dialect d;
              if (!parse_dialect(dialect.cast<std::string>(), d)) {
                  throw py::value_error("unknown dialect: " + dialect.cast<std::string>());
              }
              return scan_to_tuple(d, cmd);
          },
          "Scan a command; returns (edits, names, notes, nul_notes). edits = "
          "[(start, end, replacement_bytes)]; names/notes/nul_notes are "
          "BASH_FIX-only fallback names, path notes and nul redirection "
          "targets.",
          py::arg("dialect"), py::arg("cmd"));

    m.def("shell_transform",
          [](py::str dialect, py::bytes cmd) -> py::bytes {
              kimix::runtime::parse::shell_dialect d;
              if (!parse_dialect(dialect.cast<std::string>(), d)) {
                  throw py::value_error("unknown dialect: " + dialect.cast<std::string>());
              }
              kimix::string_view view;
              if (!bytes_view(cmd, view)) {
                  throw py::error_already_set();
              }
              kimix::vector<kimix::runtime::parse::edit> edits;
              kimix::string transformed;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::parse::scan_shell(d, view, edits, &transformed);
              }
              if (d == kimix::runtime::parse::shell_dialect::BASH_FIX) {
                  // The kernel cannot build the fallback prefix; apply the
                  // edits only (reverse index order keeps positions valid).
                  kimix::string out(cmd);
                  for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
                      out.replace(it->start, it->end - it->start, it->replacement);
                  }
                  return to_bytes(out);
              }
              return to_bytes(transformed);
          },
          "Apply the dialect transform; returns the transformed command "
          "bytes. BASH_FIX returns edits applied without the fallback "
          "prefix (the shim composes the full fix output).",
          py::arg("dialect"), py::arg("cmd"));

    m.def("pwsh_fix_warning",
          [](py::bytes cmd) -> int {
              kimix::string_view view;
              if (!bytes_view(cmd, view)) {
                  throw py::error_already_set();
              }
              kimix::vector<kimix::runtime::parse::edit> edits;
              int warning = 0;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::parse::scan_shell(
                      kimix::runtime::parse::shell_dialect::PWSH_FIX, view, edits,
                      nullptr, nullptr, nullptr, &warning);
              }
              return warning;
          },
          "pwsh_fix repair outcome: 0 = valid, 1..9 = warning kind, "
          "bit 0x10 = trailing-continuation newline appended, -1 = unrepairable.",
          py::arg("cmd"));
}
