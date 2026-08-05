/*
 * py_workspace.cpp — Python bindings for the runtime_py.workspace kernel.
 */

#include <runtime/common/gil.h>
#include <runtime/workspace/workspace.h>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <string>

namespace py = pybind11;
namespace krw = kimix::runtime::workspace;
using kimix::runtime::common::gil_scoped_release;

namespace {

// Convert a Python dict[str, bytes] into a C++ snapshot map.
bool snapshot_dict_to_map(py::dict src, krw::snapshot_t &dst) {
    for (auto item : src) {
        if (!py::isinstance<py::str>(item.first)) {
            PyErr_SetString(PyExc_TypeError, "snapshot keys must be str");
            return false;
        }
        if (!py::isinstance<py::bytes>(item.second)) {
            PyErr_SetString(PyExc_TypeError, "snapshot values must be bytes");
            return false;
        }

        const std::string key = item.first.cast<std::string>();
        const py::bytes value = item.second.cast<py::bytes>();

        char *buffer = nullptr;
        Py_ssize_t length = 0;
        if (PYBIND11_BYTES_AS_STRING_AND_SIZE(value.ptr(), &buffer, &length)) {
            return false;
        }

        dst.emplace(kimix::string(key.c_str(), key.size()),
                    kimix::string(buffer, static_cast<size_t>(length)));
    }
    return true;
}

// Convert a C++ snapshot map into a Python dict[str, bytes].
py::dict map_to_dict(const krw::snapshot_t &src) {
    py::dict dst;
    for (const auto &kv : src) {
        dst[kv.first.c_str()] = py::bytes(kv.second.data(), kv.second.size());
    }
    return dst;
}

} // namespace

void py_register_workspace(py::module_ &m) {
    m.doc() =
        "Workspace kernels: snapshot, diff, changed-files for swarm copy-mode";

    m.def(
        "snapshot",
        [](const std::string &root, py::list ignore_dirs,
           size_t max_file_bytes) -> py::dict {
            kimix::vector<kimix::string> ignores;
            ignores.reserve(ignore_dirs.size());
            for (auto item : ignore_dirs) {
                const std::string s = item.cast<std::string>();
                ignores.emplace_back(s.c_str(), s.size());
            }

            krw::snapshot_t result;
            {
                gil_scoped_release release;
                result = krw::snapshot(root, ignores, max_file_bytes);
            }
            return map_to_dict(result);
        },
        py::arg("root"), py::arg("ignore_dirs") = py::list(),
        py::arg("max_file_bytes") = static_cast<size_t>(8) * 1024 * 1024,
        "Walk a directory tree and return relative_path -> file content.");

    m.def(
        "diff_snapshots",
        [](py::dict before, py::dict after, py::object text_extensions,
           size_t context_lines) -> py::bytes {
            krw::snapshot_t before_map;
            krw::snapshot_t after_map;
            if (!snapshot_dict_to_map(before, before_map) ||
                !snapshot_dict_to_map(after, after_map)) {
                throw py::error_already_set();
            }

            krw::text_extensions_t text_exts;
            if (!text_extensions.is_none()) {
                text_exts.emplace();
                for (auto item : text_extensions.cast<py::set>()) {
                    const std::string s = item.cast<std::string>();
                    text_exts->emplace(s.c_str(), s.size());
                }
            }

            kimix::string result;
            {
                gil_scoped_release release;
                result = krw::diff_snapshots(before_map, after_map, text_exts,
                                             context_lines);
            }
            return py::bytes(result.data(), result.size());
        },
        py::arg("before"), py::arg("after"),
        py::arg("text_extensions") = py::none(),
        py::arg("context_lines") = static_cast<size_t>(3),
        "Return a combined unified diff of two snapshots.");

    m.def(
        "changed_files",
        [](py::dict before, py::dict after) -> py::list {
            krw::snapshot_t before_map;
            krw::snapshot_t after_map;
            if (!snapshot_dict_to_map(before, before_map) ||
                !snapshot_dict_to_map(after, after_map)) {
                throw py::error_already_set();
            }

            kimix::vector<std::pair<kimix::string, kimix::string>> files;
            {
                gil_scoped_release release;
                files = krw::changed_files(before_map, after_map);
            }

            py::list result;
            for (const auto &p : files) {
                result.append(py::make_tuple(p.first, p.second));
            }
            return result;
        },
        py::arg("before"), py::arg("after"),
        "Return copy-mode operations for files that differ.");
}
