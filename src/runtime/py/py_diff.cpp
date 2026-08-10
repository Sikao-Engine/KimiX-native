/*
 * py_diff.cpp — Python bindings for the runtime_py.diff kernel.
 */

#include <runtime/common/gil.h>
#include <runtime/diff/diff_engine.h>
#include <runtime/runtime.h>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <string>

namespace py = pybind11;
namespace krd = kimix::runtime::diff;

using kimix::runtime::common::gil_scoped_release;

namespace {

bool bytes_view(py::bytes src, kimix::string_view& out) {
    char* buffer = nullptr;
    Py_ssize_t length = 0;
    if (PYBIND11_BYTES_AS_STRING_AND_SIZE(src.ptr(), &buffer, &length)) {
        return false;
    }
    out = kimix::string_view(buffer, static_cast<size_t>(length));
    return true;
}

py::str py_str_from_utf8_surrogatepass(const kimix::string& s) {
    PyObject* obj = PyUnicode_DecodeUTF8(s.data(), static_cast<Py_ssize_t>(s.size()), "surrogatepass");
    if (obj == nullptr) {
        throw py::error_already_set();
    }
    return py::reinterpret_steal<py::str>(obj);
}

py::dict hunk_to_dict(const krd::hunk& h) {
    py::dict d;
    d["old_start"] = h.old_start;
    d["new_start"] = h.new_start;

    py::list old_lines;
    for (const auto& line : h.old_lines) {
        old_lines.append(py_str_from_utf8_surrogatepass(line));
    }
    d["old_lines"] = old_lines;

    py::list new_lines;
    for (const auto& line : h.new_lines) {
        new_lines.append(py_str_from_utf8_surrogatepass(line));
    }
    d["new_lines"] = new_lines;

    return d;
}

} // namespace

void py_register_diff(py::module_& m) {
    m.doc() = "Diff and hunk rendering kernel";

    m.def(
        "unified_diff",
        [](py::bytes old_text,
           py::bytes new_text,
           const std::string& path,
           bool include_file_header,
           const std::string& lineterm) -> py::bytes {
            kimix::string_view old_view;
            kimix::string_view new_view;
            if (!bytes_view(old_text, old_view) || !bytes_view(new_text, new_view)) {
                throw py::error_already_set();
            }

            kimix::string result;
            {
                gil_scoped_release release;
                result = krd::unified_diff(old_view, new_view, path, include_file_header, lineterm);
            }
            return py::bytes(result.data(), result.size());
        },
        py::arg("old_text"),
        py::arg("new_text"),
        py::arg("path") = std::string(),
        py::arg("include_file_header") = true,
        py::arg("lineterm") = std::string("\n"),
        "Return a unified diff string.");

    m.def(
        "diff_hunks",
        [](py::bytes old_text, py::bytes new_text, size_t context_lines) -> py::list {
            kimix::string_view old_view;
            kimix::string_view new_view;
            if (!bytes_view(old_text, old_view) || !bytes_view(new_text, new_view)) {
                throw py::error_already_set();
            }

            kimix::vector<krd::hunk> hunks;
            {
                gil_scoped_release release;
                hunks = krd::diff_hunks(old_view, new_view, context_lines);
            }

            py::list result;
            for (const auto& h : hunks) {
                result.append(hunk_to_dict(h));
            }
            return result;
        },
        py::arg("old_text"),
        py::arg("new_text"),
        py::arg("context_lines") = 3,
        "Return a list of hunks.");

    m.def(
        "inline_diff_ranges",
        [](py::bytes old_line, py::bytes new_line, double min_ratio) -> py::tuple {
            kimix::string_view old_view;
            kimix::string_view new_view;
            if (!bytes_view(old_line, old_view) || !bytes_view(new_line, new_view)) {
                throw py::error_already_set();
            }

            kimix::vector<krd::offset_range> deletes;
            kimix::vector<krd::offset_range> inserts;
            {
                gil_scoped_release release;
                std::tie(deletes, inserts) = krd::inline_diff_ranges(old_view, new_view, min_ratio, 4);
            }

            auto make_list = [](const kimix::vector<krd::offset_range>& ranges) -> py::list {
                py::list lst;
                for (const auto& r : ranges) {
                    lst.append(py::make_tuple(r.start, r.end));
                }
                return lst;
            };

            return py::make_tuple(make_list(deletes), make_list(inserts));
        },
        py::arg("old_line"),
        py::arg("new_line"),
        py::arg("min_ratio") = 0.5,
        "Return (delete_ranges, insert_ranges) for inline diff highlighting.");

    m.def(
        "build_offset_map",
        [](py::bytes raw, py::bytes rendered, int tab_size) -> std::vector<int> {
            kimix::string_view raw_view;
            kimix::string_view rendered_view;
            if (!bytes_view(raw, raw_view) || !bytes_view(rendered, rendered_view)) {
                throw py::error_already_set();
            }

            kimix::vector<int> offsets;
            {
                gil_scoped_release release;
                krd::build_offset_map(raw_view, rendered_view, tab_size, offsets);
            }
            return std::vector<int>(offsets.begin(), offsets.end());
        },
        py::arg("raw"),
        py::arg("rendered"),
        py::arg("tab_size") = 4,
        "Return a list of length len(raw)+1 mapping raw code-point indices to "
        "rendered indices (mirror of diff_render.py::_build_offset_map).");
}
