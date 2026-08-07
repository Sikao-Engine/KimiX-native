/*
 * py_glob.cpp -- Python bindings for the glob kernels (runtime_py.glob).
 *
 * BINDING-LAYER ONLY: this TU links against runtime.dll (pure C++ kernels)
 * and pybind11. Every kernel call releases the GIL via
 * kimix::runtime::common::gil_scoped_release; Python objects are built only
 * after the release guard destructs.
 */

#include <pybind11/pybind11.h>

#include <runtime/common/gil.h>
#include <runtime/glob/gitignore.h>

namespace py = pybind11;

namespace {

bool bytes_view(py::bytes data, kimix::string_view &view) {
  char *buf = nullptr;
  Py_ssize_t len = 0;
  if (PyBytes_AsStringAndSize(data.ptr(), &buf, &len) < 0) {
    return false;
  }
  view = kimix::string_view(buf, static_cast<size_t>(len));
  return true;
}

bool parse_rules(py::list rules,
                 kimix::vector<kimix::runtime::glob::gitignore_rule> &out) {
  const Py_ssize_t n = PyList_GET_SIZE(rules.ptr());
  out.clear();
  out.reserve(static_cast<size_t>(n));
  for (Py_ssize_t i = 0; i < n; ++i) {
    py::handle item = PyList_GET_ITEM(rules.ptr(), i);
    if (!PyTuple_Check(item.ptr()) || PyTuple_GET_SIZE(item.ptr()) != 4) {
      return false;
    }
    py::handle h_pattern = PyTuple_GET_ITEM(item.ptr(), 0);
    py::handle h_neg = PyTuple_GET_ITEM(item.ptr(), 1);
    py::handle h_anch = PyTuple_GET_ITEM(item.ptr(), 2);
    py::handle h_dir = PyTuple_GET_ITEM(item.ptr(), 3);
    if (!PyUnicode_Check(h_pattern.ptr())) {
      return false;
    }
    kimix::runtime::glob::gitignore_rule rule;
    rule.pattern = h_pattern.cast<kimix::string>();
    rule.negated = py::cast<bool>(h_neg);
    rule.anchored = py::cast<bool>(h_anch);
    rule.dir_only = py::cast<bool>(h_dir);
    out.push_back(std::move(rule));
  }
  return true;
}

bool parse_string_list(py::list src, kimix::vector<kimix::string> &out) {
  const Py_ssize_t n = PyList_GET_SIZE(src.ptr());
  out.clear();
  out.reserve(static_cast<size_t>(n));
  for (Py_ssize_t i = 0; i < n; ++i) {
    py::handle item = PyList_GET_ITEM(src.ptr(), i);
    if (!PyUnicode_Check(item.ptr())) {
      return false;
    }
    out.push_back(item.cast<kimix::string>());
  }
  return true;
}

bool parse_bool_list(py::list src, kimix::vector<bool> &out) {
  const Py_ssize_t n = PyList_GET_SIZE(src.ptr());
  out.clear();
  out.reserve(static_cast<size_t>(n));
  for (Py_ssize_t i = 0; i < n; ++i) {
    py::handle item = PyList_GET_ITEM(src.ptr(), i);
    out.push_back(py::cast<bool>(item));
  }
  return true;
}

} // namespace

void py_register_glob(py::module_ &m) {
  m.doc() = "Glob kernels: gitignore parsing/matching, path filtering, git "
            "ls-files parser";

  // ------------------------------------------------------------------
  // Gitignore rule parsing
  // ------------------------------------------------------------------
  m.def(
      "parse_gitignore",
      [](py::bytes content, py::str /*source_dir*/) -> py::list {
        kimix::string_view view;
        if (!bytes_view(content, view)) {
          throw py::error_already_set();
        }
        kimix::vector<kimix::runtime::glob::gitignore_rule> rules;
        {
          kimix::runtime::common::gil_scoped_release release;
          rules = kimix::runtime::glob::parse_gitignore(view);
        }
        py::list out;
        for (const auto &rule : rules) {
          out.append(py::make_tuple(rule.pattern, rule.negated, rule.anchored,
                                    rule.dir_only));
        }
        return out;
      },
      "Parse .gitignore bytes into (pattern, negated, anchored, dir_only) "
      "rules.",
      py::arg("content"), py::arg("source_dir"));

  // ------------------------------------------------------------------
  // Single-path gitignore test
  // ------------------------------------------------------------------
  m.def(
      "is_ignored",
      [](py::str rel_path, bool is_dir, py::list rules,
         py::object case_insensitive) -> bool {
        kimix::vector<kimix::runtime::glob::gitignore_rule> parsed;
        if (!parse_rules(rules, parsed)) {
          throw py::type_error(
              "rules must be list[tuple[str, bool, bool, bool]]");
        }
        kimix::string path = rel_path.cast<kimix::string>();
        bool result = false;
        {
          kimix::runtime::common::gil_scoped_release release;
          if (case_insensitive.is_none()) {
            result =
                kimix::runtime::glob::is_ignored_path(path, is_dir, parsed);
          } else {
            result = kimix::runtime::glob::is_ignored_path(
                path, is_dir, parsed, py::cast<bool>(case_insensitive));
          }
        }
        return result;
      },
      "True if gitignore rules ignore this relative path.  case_insensitive=None "
      "(default) uses the platform default (True on Windows, False elsewhere), "
      "mirroring fnmatch.fnmatch.",
      py::arg("rel_path"), py::arg("is_dir"), py::arg("rules"),
      py::arg("case_insensitive") = py::none());

  // ------------------------------------------------------------------
  // Bulk path filter
  // ------------------------------------------------------------------
  m.def(
      "filter_paths",
      [](py::list paths, py::list is_dir_mask, py::list rules,
         py::object case_insensitive) -> py::list {
        kimix::vector<kimix::string> path_vec;
        kimix::vector<bool> dir_vec;
        kimix::vector<kimix::runtime::glob::gitignore_rule> parsed;
        if (!parse_string_list(paths, path_vec) ||
            !parse_bool_list(is_dir_mask, dir_vec) ||
            !parse_rules(rules, parsed)) {
          throw py::type_error("arguments must be list[str], list[bool], "
                               "list[tuple[str, bool, bool, bool]]");
        }
        kimix::vector<bool> mask;
        {
          kimix::runtime::common::gil_scoped_release release;
          if (case_insensitive.is_none()) {
            kimix::runtime::glob::filter_paths(path_vec, dir_vec, parsed, mask);
          } else {
            kimix::runtime::glob::filter_paths(path_vec, dir_vec, parsed, mask,
                                               py::cast<bool>(case_insensitive));
          }
        }
        py::list out;
        for (bool v : mask) {
          out.append(v);
        }
        return out;
      },
      "Bulk filter returning a boolean mask.  case_insensitive=None (default) "
      "uses the platform default (True on Windows, False elsewhere).",
      py::arg("paths"), py::arg("is_dir_mask"), py::arg("rules"),
      py::arg("case_insensitive") = py::none());

  // ------------------------------------------------------------------
  // Hard-coded ignored-name fast path
  // ------------------------------------------------------------------
  m.def(
      "is_ignored_name",
      [](py::str name) -> bool {
        kimix::string n = name.cast<kimix::string>();
        bool result = false;
        {
          kimix::runtime::common::gil_scoped_release release;
          result = kimix::runtime::glob::is_ignored_name(n);
        }
        return result;
      },
      "Hard-coded name + regex fast path.", py::arg("name"));

  // ------------------------------------------------------------------
  // git ls-files -z parser
  // ------------------------------------------------------------------
  m.def(
      "parse_ls_files_output",
      [](py::bytes stdout_bytes, bool filter_ignored) -> py::list {
        kimix::string_view view;
        if (!bytes_view(stdout_bytes, view)) {
          throw py::error_already_set();
        }
        kimix::vector<kimix::string> paths;
        {
          kimix::runtime::common::gil_scoped_release release;
          paths =
              kimix::runtime::glob::parse_ls_files_output(view, filter_ignored);
        }
        py::list out;
        for (const auto &p : paths) {
          out.append(py::str(p.data(), static_cast<py::ssize_t>(p.size())));
        }
        return out;
      },
      "Parse NUL-delimited git ls-files output, synthesise directory entries, "
      "and optionally drop ignored prefixes.",
      py::arg("stdout"), py::arg("filter_ignored") = true);
}
