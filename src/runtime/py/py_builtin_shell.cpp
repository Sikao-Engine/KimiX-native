/*
 * py_builtin_shell.cpp - Python bindings for the builtin shell tool kernels
 * (runtime_py.shell).
 *
 * BINDING-LAYER ONLY: links against kimix-llm (pure C++ kernels) and pybind11.
 * Every kernel call releases the GIL via kimix::runtime::common::gil_scoped_release;
 * Python objects are built only after the release scope closes.
 *
 * API:
 *   shell.interpret_exit_code(command: str, exit_code: int | None) -> str | None
 *   shell.is_expected_exit(command: str, exit_code: int | None) -> bool
 *   shell.annotate_failure(output: str, command: str, exit_code: int | None)
 *       -> str | None
 *   shell.find_error_line_index(output: str) -> int | None
 *   shell.truncate_lines(output: str, max_lines: int, preserve_errors: bool = True,
 *       error_context_lines: int = 2) -> str
 *   shell.split_shell_segments(command: str) -> list[tuple[str, str]]
 *   shell.is_known_rtk_command(name: str) -> bool
 *   shell.rewrite_shell_segment(segment: str, exclude_read: bool, pwsh: bool = False)
 *       -> tuple[str, bool]
 *   shell.maybe_rewrite_shell_command_with_rtk(
 *       command: str, token_kill: bool, rtk_available: bool,
 *       rtk_binary_path: str, exclude_read: bool = False, pwsh: bool = False)
 *       -> tuple[str, bool]
 *   shell.command_detection_variants(command: str) -> list[str]
 *   shell.check_hardline_blocked(command: str) -> tuple[bool, str | None]
 *   shell.foreground_background_guidance(command: str) -> str | None
 *
 * Pwsh mirrors / extras:
 *   shell.pwsh_command_detection_variants(command: str) -> list[str]
 *   shell.pwsh_check_hardline_blocked(command: str) -> tuple[bool, str | None]
 *   shell.pwsh_maybe_rewrite_with_rtk(
 *       command: str, token_kill: bool, rtk_available: bool,
 *       rtk_binary_path: str, exclude_read: bool = False)
 *       -> tuple[str, bool]
 *   shell.pwsh_transform(code: str) -> tuple[str, list[str]]
 *   shell.fix_pwsh_command(command: str) -> tuple[bool, bool, str, str]
 */

#include <pybind11/pybind11.h>

#include <runtime/runtime.h>
#include <runtime/common/gil.h>
#include <builtin_tools/bash_tool.h>
#include <builtin_tools/pwsh_tool.h>

namespace py = pybind11;

namespace {

// Convert a Python str to a UTF-8 kimix::string.
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

py::str to_py_str(const kimix::string& s) {
    return py::str(s.data(), s.size());
}

py::object opt_str_to_obj(const kimix::optional<kimix::string>& o) {
    if (o.has_value()) {
        return to_py_str(*o);
    }
    return py::none();
}

py::object opt_int_to_obj(const kimix::optional<int64_t>& o) {
    if (o.has_value()) {
        return py::int_(*o);
    }
    return py::none();
}

py::tuple bash_hardline_to_tuple(const kimix::builtin_tools::bash::hardline_result& r) {
    if (r.blocked && r.description.has_value()) {
        return py::make_tuple(true, to_py_str(*r.description));
    }
    return py::make_tuple(false, py::none());
}

py::tuple pwsh_hardline_to_tuple(const kimix::builtin_tools::pwsh::hardline_result& r) {
    if (r.blocked && !r.description.empty()) {
        return py::make_tuple(true, to_py_str(r.description));
    }
    return py::make_tuple(false, py::none());
}

py::tuple rewrite_to_tuple(const kimix::builtin_tools::bash::rewrite_result& r) {
    return py::make_tuple(to_py_str(r.segment), r.changed);
}

py::list variants_to_list(const kimix::vector<kimix::string>& variants) {
    py::list out;
    for (const auto& v : variants) {
        out.append(to_py_str(v));
    }
    return out;
}

py::list warnings_to_list(const kimix::vector<kimix::string>& warnings) {
    py::list out;
    for (const auto& w : warnings) {
        out.append(to_py_str(w));
    }
    return out;
}

kimix::optional<int64_t> parse_optional_exit_code(py::handle exit_code) {
    if (exit_code.is_none()) {
        return kimix::optional<int64_t>{};
    }
    if (!py::isinstance<py::int_>(exit_code)) {
        throw py::type_error("exit_code must be int or None");
    }
    return exit_code.cast<int64_t>();
}

} // namespace

void py_register_builtin_shell(py::module_& m) {
    auto shell = m.def_submodule("shell",
                                 "Built-in shell tool kernels (bash/pwsh/python)");

    // ------------------------------------------------------------------
    // Bash kernels (output_enhance.py / safety.py)
    // ------------------------------------------------------------------
    shell.def("interpret_exit_code",
              [](py::str command, py::object exit_code) -> py::object {
                  kimix::string cmd;
                  if (!str_to_string(command, cmd)) {
                      throw py::type_error("command must be str");
                  }
                  kimix::optional<int64_t> code = parse_optional_exit_code(exit_code);
                  kimix::optional<kimix::string> result;
                  {
                      kimix::runtime::common::gil_scoped_release release;
                      result = kimix::builtin_tools::bash::interpret_exit_code(cmd, code);
                  }
                  return opt_str_to_obj(result);
              },
              "Explain a non-zero exit code for well-known commands, else None.",
              py::arg("command"), py::arg("exit_code"));

    shell.def("is_expected_exit",
              [](py::str command, py::object exit_code) -> bool {
                  kimix::string cmd;
                  if (!str_to_string(command, cmd)) {
                      throw py::type_error("command must be str");
                  }
                  kimix::optional<int64_t> code = parse_optional_exit_code(exit_code);
                  bool expected = false;
                  {
                      kimix::runtime::common::gil_scoped_release release;
                      expected = kimix::builtin_tools::bash::is_expected_exit(cmd, code);
                  }
                  return expected;
              },
              "True when exit_code is a normal, expected outcome for command.",
              py::arg("command"), py::arg("exit_code"));

    shell.def("annotate_failure",
              [](py::str output, py::str command, py::object exit_code) -> py::object {
                  kimix::string out, cmd;
                  if (!str_to_string(output, out) || !str_to_string(command, cmd)) {
                      throw py::type_error("output and command must be str");
                  }
                  kimix::optional<int64_t> code = parse_optional_exit_code(exit_code);
                  kimix::optional<kimix::string> hint;
                  {
                      kimix::runtime::common::gil_scoped_release release;
                      hint = kimix::builtin_tools::bash::annotate_failure(out, cmd, code);
                  }
                  return opt_str_to_obj(hint);
              },
              "Single actionable hint for common failure signatures, else None.",
              py::arg("output"), py::arg("command"), py::arg("exit_code"));

    shell.def("find_error_line_index",
              [](py::str output) -> py::object {
                  kimix::string out;
                  if (!str_to_string(output, out)) {
                      throw py::type_error("output must be str");
                  }
                  kimix::optional<int64_t> idx;
                  {
                      kimix::runtime::common::gil_scoped_release release;
                      idx = kimix::builtin_tools::bash::find_error_line_index(out);
                  }
                  return opt_int_to_obj(idx);
              },
              "1-based index of the first error line, or None.",
              py::arg("output"));

    shell.def("truncate_lines",
              [](py::str output, int64_t max_lines, bool preserve_errors,
                 int64_t error_context_lines) -> py::str {
                  kimix::string out;
                  if (!str_to_string(output, out)) {
                      throw py::type_error("output must be str");
                  }
                  kimix::string result;
                  {
                      kimix::runtime::common::gil_scoped_release release;
                      result = kimix::builtin_tools::bash::truncate_lines(
                          out, max_lines, preserve_errors, error_context_lines);
                  }
                  return to_py_str(result);
              },
              "Fold output to max_lines while preserving error context.",
              py::arg("output"), py::arg("max_lines"),
              py::arg("preserve_errors") = true,
              py::arg("error_context_lines") = 2);

    shell.def("split_shell_segments",
              [](py::str command) -> py::list {
                  kimix::string cmd;
                  if (!str_to_string(command, cmd)) {
                      throw py::type_error("command must be str");
                  }
                  kimix::vector<kimix::builtin_tools::bash::shell_segment> segs;
                  {
                      kimix::runtime::common::gil_scoped_release release;
                      kimix::builtin_tools::bash::split_shell_segments(cmd, segs);
                  }
                  py::list out;
                  for (const auto& s : segs) {
                      out.append(py::make_tuple(to_py_str(s.text), to_py_str(s.sep)));
                  }
                  return out;
              },
              "Split a shell command on ; / && / || (single | and & stay inside).",
              py::arg("command"));

    shell.def("is_known_rtk_command",
              [](py::str name) -> bool {
                  kimix::string n;
                  if (!str_to_string(name, n)) {
                      throw py::type_error("name must be str");
                  }
                  bool known = false;
                  {
                      kimix::runtime::common::gil_scoped_release release;
                      known = kimix::builtin_tools::bash::is_known_rtk_command(n);
                  }
                  return known;
              },
              "True when name is a known RTK top-level command.",
              py::arg("name"));

    shell.def("rewrite_shell_segment",
              [](py::str segment, bool exclude_read, bool pwsh) -> py::tuple {
                  kimix::string seg;
                  if (!str_to_string(segment, seg)) {
                      throw py::type_error("segment must be str");
                  }
                  kimix::builtin_tools::bash::rewrite_result r;
                  {
                      kimix::runtime::common::gil_scoped_release release;
                      r = kimix::builtin_tools::bash::rewrite_shell_segment(
                          seg, exclude_read, pwsh);
                  }
                  return rewrite_to_tuple(r);
              },
              "Rewrite one shell segment with the RTK prefix.",
              py::arg("segment"), py::arg("exclude_read"),
              py::arg("pwsh") = false);

    shell.def("maybe_rewrite_shell_command_with_rtk",
              [](py::str command, bool token_kill, bool rtk_available,
                 py::str rtk_binary_path, bool exclude_read, bool pwsh) -> py::tuple {
                  kimix::string cmd, path;
                  if (!str_to_string(command, cmd) ||
                      !str_to_string(rtk_binary_path, path)) {
                      throw py::type_error("command and rtk_binary_path must be str");
                  }
                  kimix::builtin_tools::bash::rewrite_result r;
                  {
                      kimix::runtime::common::gil_scoped_release release;
                      r = kimix::builtin_tools::bash::maybe_rewrite_shell_command_with_rtk(
                          cmd, token_kill, rtk_available, path, exclude_read, pwsh);
                  }
                  return rewrite_to_tuple(r);
              },
              "Rewrite a whole command with RTK when safe; returns (command, changed).",
              py::arg("command"), py::arg("token_kill"), py::arg("rtk_available"),
              py::arg("rtk_binary_path"), py::arg("exclude_read") = false,
              py::arg("pwsh") = false);

    shell.def("command_detection_variants",
              [](py::str command) -> py::list {
                  kimix::string cmd;
                  if (!str_to_string(command, cmd)) {
                      throw py::type_error("command must be str");
                  }
                  kimix::vector<kimix::string> variants;
                  {
                      kimix::runtime::common::gil_scoped_release release;
                      kimix::builtin_tools::bash::command_detection_variants(cmd, variants);
                  }
                  return variants_to_list(variants);
              },
              "Deobfuscation variants of command (at most 3).",
              py::arg("command"));

    shell.def("check_hardline_blocked",
              [](py::str command) -> py::tuple {
                  kimix::string cmd;
                  if (!str_to_string(command, cmd)) {
                      throw py::type_error("command must be str");
                  }
                  kimix::builtin_tools::bash::hardline_result r;
                  {
                      kimix::runtime::common::gil_scoped_release release;
                      r = kimix::builtin_tools::bash::check_hardline_blocked(cmd);
                  }
                  return bash_hardline_to_tuple(r);
              },
              "(True, description) when command matches a hardline pattern.",
              py::arg("command"));

    shell.def("foreground_background_guidance",
              [](py::str command) -> py::object {
                  kimix::string cmd;
                  if (!str_to_string(command, cmd)) {
                      throw py::type_error("command must be str");
                  }
                  kimix::optional<kimix::string> hint;
                  {
                      kimix::runtime::common::gil_scoped_release release;
                      hint = kimix::builtin_tools::bash::foreground_background_guidance(cmd);
                  }
                  return opt_str_to_obj(hint);
              },
              "Long-running-process hint or None.",
              py::arg("command"));

    // ------------------------------------------------------------------
    // Pwsh kernels (mirrors + pwsh-specific)
    // ------------------------------------------------------------------
    shell.def("pwsh_command_detection_variants",
              [](py::str command) -> py::list {
                  kimix::string cmd;
                  if (!str_to_string(command, cmd)) {
                      throw py::type_error("command must be str");
                  }
                  kimix::vector<kimix::string> variants;
                  {
                      kimix::runtime::common::gil_scoped_release release;
                      kimix::builtin_tools::pwsh::command_detection_variants(cmd, variants);
                  }
                  return variants_to_list(variants);
              },
              "Pwsh deobfuscation variants of command (at most 3).",
              py::arg("command"));

    shell.def("pwsh_check_hardline_blocked",
              [](py::str command) -> py::tuple {
                  kimix::string cmd;
                  if (!str_to_string(command, cmd)) {
                      throw py::type_error("command must be str");
                  }
                  kimix::builtin_tools::pwsh::hardline_result r;
                  {
                      kimix::runtime::common::gil_scoped_release release;
                      r = kimix::builtin_tools::pwsh::check_hardline_blocked(cmd);
                  }
                  return pwsh_hardline_to_tuple(r);
              },
              "(True, description) when a pwsh command matches a hardline pattern.",
              py::arg("command"));

    shell.def("pwsh_maybe_rewrite_with_rtk",
              [](py::str command, bool token_kill, bool rtk_available,
                 py::str rtk_binary_path, bool exclude_read) -> py::tuple {
                  kimix::string cmd, path;
                  if (!str_to_string(command, cmd) ||
                      !str_to_string(rtk_binary_path, path)) {
                      throw py::type_error("command and rtk_binary_path must be str");
                  }
                  kimix::builtin_tools::bash::rewrite_result r;
                  {
                      kimix::runtime::common::gil_scoped_release release;
                      r = kimix::builtin_tools::pwsh::maybe_rewrite_with_rtk(
                          cmd, token_kill, rtk_available, path, exclude_read);
                  }
                  return rewrite_to_tuple(r);
              },
              "Rewrite a pwsh command with RTK when safe; returns (command, changed).",
              py::arg("command"), py::arg("token_kill"), py::arg("rtk_available"),
              py::arg("rtk_binary_path"), py::arg("exclude_read") = false);

    shell.def("pwsh_transform",
              [](py::str code) -> py::tuple {
                  kimix::string c;
                  if (!str_to_string(code, c)) {
                      throw py::type_error("code must be str");
                  }
                  kimix::builtin_tools::pwsh::transform_result r;
                  {
                      kimix::runtime::common::gil_scoped_release release;
                      r = kimix::builtin_tools::pwsh::pwsh_transform(c);
                  }
                  return py::make_tuple(to_py_str(r.command), warnings_to_list(r.warnings));
              },
              "PowerShell 7.x -> 5.1 syntax transform; returns (command, warnings).",
              py::arg("code"));

    shell.def("fix_pwsh_command",
              [](py::str command) -> py::tuple {
                  kimix::string cmd;
                  if (!str_to_string(command, cmd)) {
                      throw py::type_error("command must be str");
                  }
                  kimix::builtin_tools::pwsh::fix_result r;
                  {
                      kimix::runtime::common::gil_scoped_release release;
                      r = kimix::builtin_tools::pwsh::fix_pwsh_command(cmd);
                  }
                  return py::make_tuple(r.valid, r.changed, to_py_str(r.command),
                                        to_py_str(r.warning));
              },
              "PowerShell command validator / auto-repair; returns "
              "(valid, changed, command, warning).",
              py::arg("command"));
}
