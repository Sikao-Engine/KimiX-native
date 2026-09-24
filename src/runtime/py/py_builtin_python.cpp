/*
 * py_builtin_python.cpp - Python bindings for the builtin Python tool kernels
 * (runtime_py.builtin_tools.python).
 *
 * BINDING-LAYER ONLY: links against kimix-llm (pure C++ kernels) and pybind11.
 * Pure-string kernels release the GIL via
 * kimix::runtime::common::gil_scoped_release; Python objects are only built
 * after the release scope closes.  Kernels that take an injected
 * file-existence probe are driven by an explicit allow-list (the same fake
 * filesystem the C++ Boost.UT goldens use), so the GIL is held throughout.
 *
 * API (mirrors src/builtin_tools/python_tool.h):
 * python.plan_script_path(base_dir: str, index: int, ext: str = ".py") -> str
 * python.resolve_python_exe(override: str, search_bases: list[str],
 *                           virtual_env: str, fallback: str,
 *                           existing_files: list[str]) -> str | None
 * python.scrub_child_env(env: dict[str, str]) -> dict[str, str]
 * python.prepare_python_env(python_exe: str, share_bin_dir: str,
 *                           current_path: str, path_sep: str,
 *                           existing_files: list[str])
 *     -> dict[str, str] | None      # the env *delta* (PATH / VIRTUAL_ENV)
 * python.module_not_found_hint(output: str, python_exe: str) -> str
 * python.build_session_output_block(task_id: str, status: str, output: str,
 *                                   exit_code: int | None = None,
 *                                   exit_code_meaning: str | None = None,
 *                                   failure_hint: str | None = None,
 *                                   wait_matched: bool | None = None,
 *                                   elapsed_seconds: float | None = None,
 *                                   output_path: str | None = None,
 *                                   output_truncated: bool = False,
 *                                   original_path: str | None = None) -> str
 * python.extract_export_path(output: str) -> str | None
 * python.classify_wait_pattern(pattern: str) -> str  # "literal"|"unsupported"
 * python.match_wait_pattern(pattern: str, buffer: str) -> tuple[str, bool]
 *     # (status, matched); status is "ok" / "unsupported" / "invalid_input"
 */
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstring>

#include <runtime/runtime.h>
#include <runtime/common/gil.h>

#include <builtin_tools/python_tool.h>

namespace py = pybind11;

namespace {

using kimix::builtin_tools::named_value;
using kimix::builtin_tools::tool_status;

// Convert a Python str to a UTF-8 kimix::string.
bool str_to_string(py::handle obj, kimix::string &out) {
    if (!PyUnicode_Check(obj.ptr())) {
        return false;
    }
    Py_ssize_t len = 0;
    const char *cstr = PyUnicode_AsUTF8AndSize(obj.ptr(), &len);
    if (cstr == nullptr) {
        return false;
    }
    out.assign(cstr, static_cast<size_t>(len));
    return true;
}

kimix::string as_string(py::handle obj, const char *what) {
    kimix::string out;
    if (!str_to_string(obj, out)) {
        throw py::type_error(std::string(what) + " must be str");
    }
    return out;
}

py::str to_py_str(const kimix::string &s) { return py::str(s.data(), s.size()); }

py::object opt_str_to_obj(const kimix::optional<kimix::string> &o) {
    if (o.has_value()) {
        return to_py_str(*o);
    }
    return py::none();
}

// list[str] -> kimix::vector<kimix::string>
kimix::vector<kimix::string> str_list(py::handle obj, const char *what) {
    if (!py::isinstance<py::list>(obj) && !py::isinstance<py::tuple>(obj)) {
        throw py::type_error(std::string(what) + " must be a list of str");
    }
    kimix::vector<kimix::string> out;
    for (py::handle item : obj) {
        out.push_back(as_string(item, what));
    }
    return out;
}

// dict[str, str] -> ordered named_value vector (insertion order preserved).
kimix::vector<named_value> env_from_dict(py::handle obj) {
    if (!py::isinstance<py::dict>(obj)) {
        throw py::type_error("env must be a dict[str, str]");
    }
    kimix::vector<named_value> out;
    for (auto item : py::reinterpret_borrow<py::dict>(obj)) {
        named_value nv;
        nv.name = as_string(item.first, "env key");
        nv.value = as_string(item.second, "env value");
        out.push_back(std::move(nv));
    }
    return out;
}

// A file-existence probe backed by an exact allow-list.
kimix::function<bool(kimix::string_view)> probe_from_list(
    const kimix::vector<kimix::string> &allowed) {
    return [allowed](kimix::string_view path) {
        for (const auto &a : allowed) {
            if (a.size() == path.size() &&
                std::memcmp(a.data(), path.data(), a.size()) == 0) {
                return true;
            }
        }
        return false;
    };
}

const char *wait_kind_name(kimix::builtin_tools::python::wait_pattern_kind k) {
    using kimix::builtin_tools::python::wait_pattern_kind;
    switch (k) {
    case wait_pattern_kind::literal:
        return "literal";
    case wait_pattern_kind::unsupported:
        return "unsupported";
    }
    return "unsupported";
}

const char *status_name(tool_status s) {
    switch (s) {
    case tool_status::ok:
        return "ok";
    case tool_status::invalid_input:
        return "invalid_input";
    case tool_status::not_found:
        return "not_found";
    case tool_status::no_change:
        return "no_change";
    case tool_status::ambiguous:
        return "ambiguous";
    case tool_status::blocked:
        return "blocked";
    case tool_status::too_large:
        return "too_large";
    case tool_status::unsupported:
        return "unsupported";
    case tool_status::external_library:
        return "external_library";
    }
    return "unknown";
}

kimix::optional<kimix::string> opt_str_arg(py::handle obj, const char *what) {
    if (obj.is_none()) {
        return kimix::optional<kimix::string>{};
    }
    return as_string(obj, what);
}

} // namespace

void py_register_builtin_python(py::module_ &m) {
    namespace btpy = kimix::builtin_tools::python;

    auto mod = m.def_submodule(
        "python",
        "Built-in Python tool kernels (py/__init__.py, common.py, security.py)");

    mod.def(
        "plan_script_path",
        [](py::str base_dir, uint64_t index, py::str ext) -> py::str {
            kimix::string base = as_string(base_dir, "base_dir");
            kimix::string extension = as_string(ext, "ext");
            kimix::string out;
            {
                kimix::runtime::common::gil_scoped_release release;
                out = btpy::plan_script_path(base, index, extension);
            }
            return to_py_str(out);
        },
        "Path arithmetic for the shared temp-script naming scheme.",
        py::arg("base_dir"), py::arg("index"), py::arg("ext") = ".py");

    mod.def(
        "resolve_python_exe",
        [](py::str override_exe, py::list search_bases, py::str virtual_env,
           py::str fallback, py::list existing_files) -> py::object {
            kimix::string ovr = as_string(override_exe, "override");
            kimix::string venv = as_string(virtual_env, "virtual_env");
            kimix::string fb = as_string(fallback, "fallback");
            kimix::vector<kimix::string> bases = str_list(search_bases, "search_bases");
            kimix::vector<kimix::string> allowed =
                str_list(existing_files, "existing_files");
            kimix::optional<kimix::string> result;
            {
                kimix::runtime::common::gil_scoped_release release;
                result = btpy::resolve_python_exe(
                    ovr, bases, venv, fb, probe_from_list(allowed));
            }
            return opt_str_to_obj(result);
        },
        "Interpreter resolution: override > .venv walk-up > VIRTUAL_ENV > fallback.",
        py::arg("override_exe"), py::arg("search_bases"), py::arg("virtual_env"),
        py::arg("fallback"), py::arg("existing_files"));

    mod.def(
        "scrub_child_env",
        [](py::dict env) -> py::dict {
            kimix::vector<named_value> in = env_from_dict(env);
            kimix::vector<named_value> out;
            {
                kimix::runtime::common::gil_scoped_release release;
                out = btpy::scrub_child_env(in);
            }
            py::dict result;
            for (const auto &e : out) {
                result[to_py_str(e.name)] = to_py_str(e.value);
            }
            return result;
        },
        "Drop credential-looking variables (security.py scrub_child_env).",
        py::arg("env"));

    mod.def(
        "prepare_python_env",
        [](py::str python_exe, py::str share_bin_dir, py::str current_path,
           py::str path_sep, py::list existing_files) -> py::object {
            kimix::string exe = as_string(python_exe, "python_exe");
            kimix::string share = as_string(share_bin_dir, "share_bin_dir");
            kimix::string path = as_string(current_path, "current_path");
            kimix::string sep = as_string(path_sep, "path_sep");
            kimix::vector<kimix::string> allowed =
                str_list(existing_files, "existing_files");
            kimix::optional<kimix::vector<btpy::env_change>> delta;
            {
                kimix::runtime::common::gil_scoped_release release;
                delta = btpy::prepare_python_env(exe, share, path, sep,
                                                 probe_from_list(allowed));
            }
            if (!delta.has_value()) {
                return py::none();
            }
            py::dict result;
            for (const auto &c : *delta) {
                result[to_py_str(c.name)] = to_py_str(c.value);
            }
            return result;
        },
        "Env delta for the child process (PATH / VIRTUAL_ENV), or None for the "
        "zero-copy fast path (_build_env).",
        py::arg("python_exe"), py::arg("share_bin_dir"), py::arg("current_path"),
        py::arg("path_sep"), py::arg("existing_files"));

    mod.def(
        "module_not_found_hint",
        [](py::str output, py::str python_exe) -> py::str {
            kimix::string out = as_string(output, "output");
            kimix::string exe = as_string(python_exe, "python_exe");
            kimix::string hint;
            {
                kimix::runtime::common::gil_scoped_release release;
                hint = btpy::module_not_found_hint(out, exe);
            }
            return to_py_str(hint);
        },
        "Pip remediation hint for a ModuleNotFoundError, else \"\".",
        py::arg("output"), py::arg("python_exe"));

    mod.def(
        "build_session_output_block",
        [](py::str task_id, py::str status, py::str output, py::object exit_code,
           py::object exit_code_meaning, py::object failure_hint,
           py::object wait_matched, py::object elapsed_seconds,
           py::object output_path, bool output_truncated,
           py::object original_path) -> py::str {
            btpy::session_output_block block;
            block.task_id = as_string(task_id, "task_id");
            block.status = as_string(status, "status");
            block.output = as_string(output, "output");
            if (!exit_code.is_none()) {
                block.exit_code = static_cast<int32_t>(exit_code.cast<int64_t>());
            }
            block.exit_code_meaning = opt_str_arg(exit_code_meaning, "exit_code_meaning");
            block.failure_hint = opt_str_arg(failure_hint, "failure_hint");
            if (!wait_matched.is_none()) {
                block.wait_matched = wait_matched.cast<bool>();
            }
            if (!elapsed_seconds.is_none()) {
                block.elapsed_seconds = elapsed_seconds.cast<double>();
            }
            block.output_path = opt_str_arg(output_path, "output_path");
            block.output_truncated = output_truncated;
            block.original_path = opt_str_arg(original_path, "original_path");
            kimix::string rendered;
            {
                kimix::runtime::common::gil_scoped_release release;
                rendered = btpy::build_session_output_block(block);
            }
            return to_py_str(rendered);
        },
        "Render the YAML-like result block (common.py _build_session_output_block).",
        py::arg("task_id"), py::arg("status"), py::arg("output"),
        py::arg("exit_code") = py::none(),
        py::arg("exit_code_meaning") = py::none(),
        py::arg("failure_hint") = py::none(),
        py::arg("wait_matched") = py::none(),
        py::arg("elapsed_seconds") = py::none(),
        py::arg("output_path") = py::none(),
        py::arg("output_truncated") = false,
        py::arg("original_path") = py::none());

    mod.def(
        "extract_export_path",
        [](py::str output) -> py::object {
            kimix::string out = as_string(output, "output");
            kimix::optional<kimix::string> result;
            {
                kimix::runtime::common::gil_scoped_release release;
                result = btpy::extract_export_path(out);
            }
            return opt_str_to_obj(result);
        },
        "Path captured by an export message, or None.",
        py::arg("output"));

    mod.def(
        "classify_wait_pattern",
        [](py::str pattern) -> py::str {
            kimix::string p = as_string(pattern, "pattern");
            btpy::wait_pattern_kind kind = btpy::wait_pattern_kind::unsupported;
            {
                kimix::runtime::common::gil_scoped_release release;
                kind = btpy::classify_wait_pattern(p);
            }
            return py::str(wait_kind_name(kind));
        },
        "\"literal\" (native, exact) or \"unsupported\" (needs the regex engine).",
        py::arg("pattern"));

    mod.def(
        "match_wait_pattern",
        [](py::str pattern, py::str buffer) -> py::tuple {
            kimix::string p = as_string(pattern, "pattern");
            kimix::string b = as_string(buffer, "buffer");
            kimix::builtin_tools::tool_error err;
            bool matched = false;
            {
                kimix::runtime::common::gil_scoped_release release;
                err = btpy::match_wait_pattern(p, b, matched);
            }
            return py::make_tuple(py::str(status_name(err.status)), matched);
        },
        "Match a wait_for_pattern against the accumulated buffer -> (status, matched).",
        py::arg("pattern"), py::arg("buffer"));
}
