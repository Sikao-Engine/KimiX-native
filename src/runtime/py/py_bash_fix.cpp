/*
 * py_bash_fix.cpp - Python bindings for the Windows Git Bash compatibility
 * kernel (runtime_py.bash_fix).
 *
 * BINDING-LAYER ONLY: links against kimix-llm (the pure C++ bash_parser kernel
 * in src/llm/bash_parser) and pybind11.  Every kernel call releases the GIL via
 * kimix::runtime::common::gil_scoped_release; Python objects are built only
 * after the release scope closes.
 *
 * The kernel is the native port of the authoritative Python reference
 * kimi-agent `bin/kimix_native/shell_compat/*` behind `_shell_compat`, so the
 * shim (bin/kimix_native/_shell_compat.py) can route `fix_bash_command`,
 * `bash_compatibility_prelude` and `_process_unquoted` through this module when
 * the native runtime is available.
 *
 * API:
 *   bash_fix.fix_bash_command(command: str, temp_dir: str | None = None)
 *       -> (command, replacements, path_changes, shell_wrappers, nul_fixes,
 *           unsupported, cmd_conditional_fixes)
 *       The reference `BashFix` flattened into one tuple: element 0 is the
 *       rewritten command text, elements 1..6 are the report lists of the
 *       frozen dataclass in declaration order.  The shim rebuilds the
 *       `shell_compat.bash_core.BashFix` instance from it.
 *       `temp_dir` overrides the reference's `tempfile.gettempdir()` spelling
 *       (the shim always passes it, so the native /tmp mapping matches the
 *       Python host exactly — including tests that monkeypatch
 *       `_windows_temp_dir`); None = native host discovery.
 *   bash_fix.bash_compatibility_prelude() -> str
 *       Fallback definitions + `export -f` lines for a persistent interactive
 *       Git Bash shell (empty on non-Windows builds; the shim keeps the
 *       reference `sys.platform == "win32"` gate).
 *   bash_fix.process_unquoted(command: str) -> str
 *       The separate unquoted-backslash transform (`_process_unquoted`).
 *
 * Contract (identical to the Python reference): never raises on user input,
 * malformed/ambiguous input passes through byte-for-byte, unchanged results
 * carry the original bytes, report lists keep source-discovery order.
 */

#include <pybind11/pybind11.h>

#include <runtime/common/gil.h>
#include <llm/bash_parser/bash_parser.h>
#include "py_util.h"

using kimix_py_util::str_to_string;
using kimix_py_util::to_py_str;


namespace py = pybind11;

namespace src_runtime_py_py_bash_fix {



py::list str_list_to_py(const kimix::vector<kimix::string>& items) {
    py::list out;
    for (const auto& item : items) {
        out.append(to_py_str(item));
    }
    return out;
}

// Flatten a BashFix into the reference dataclass field order.
py::tuple bash_fix_to_tuple(const kimix::llm::bash_parser::BashFix& result) {
    return py::make_tuple(to_py_str(result.command()),
                          str_list_to_py(result.replacements()),
                          str_list_to_py(result.path_changes()),
                          str_list_to_py(result.shell_wrappers()),
                          str_list_to_py(result.nul_fixes()),
                          str_list_to_py(result.unsupported()),
                          str_list_to_py(result.cmd_conditional_fixes()));
}

} // namespace src_runtime_py_py_bash_fix
using namespace src_runtime_py_py_bash_fix;

void py_register_bash_fix(py::module_& m) {
    // `m` is the `bash_fix` submodule created by module.cpp; the functions are
    // registered directly on it (same layout as the other domain modules).
    m.doc() = "Windows Git Bash compatibility kernel (native port of the "
              "kimi-agent shell_compat fix_bash_command reference).";

    m.def(
        "fix_bash_command",
        [](py::str command, py::object temp_dir) -> py::tuple {
            kimix::string cmd;
            if (!str_to_string(command, cmd)) {
                throw py::type_error("command must be str");
            }
            kimix::llm::bash_parser::FixOptions options;
            if (!temp_dir.is_none()) {
                if (!str_to_string(temp_dir, options.temp_dir)) {
                    throw py::type_error("temp_dir must be str or None");
                }
            }
            kimix::llm::bash_parser::BashFix result;
            {
                kimix::runtime::common::gil_scoped_release release;
                result = kimix::llm::bash_parser::fix_bash_command(cmd, options);
            }
            return bash_fix_to_tuple(result);
        },
        "Full Git Bash transformation of command; returns (command, "
        "replacements, path_changes, shell_wrappers, nul_fixes, unsupported, "
        "cmd_conditional_fixes).",
        py::arg("command"), py::arg("temp_dir") = py::none());

    m.def(
        "bash_compatibility_prelude",
        []() -> py::str {
            kimix::string prelude;
            {
                kimix::runtime::common::gil_scoped_release release;
                prelude = kimix::llm::bash_parser::bash_compatibility_prelude();
            }
            return to_py_str(prelude);
        },
        "Fallback definitions + export -f lines for a persistent Git Bash shell "
        "(empty on non-Windows builds).");

    m.def(
        "process_unquoted",
        [](py::str command) -> py::str {
            kimix::string cmd;
            if (!str_to_string(command, cmd)) {
                throw py::type_error("command must be str");
            }
            kimix::string processed;
            {
                kimix::runtime::common::gil_scoped_release release;
                processed = kimix::llm::bash_parser::process_unquoted(cmd);
            }
            return to_py_str(processed);
        },
        "Unquoted-backslash transform (bash_tool._process_unquoted).",
        py::arg("command"));
}
