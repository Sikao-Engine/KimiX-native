/*
 * py_builtin_file.cpp - Python bindings for the builtin file tool kernels
 * (runtime_py.file).
 *
 * BINDING-LAYER ONLY: links against kimix-llm (pure C++ kernels) and pybind11.
 * Every kernel call releases the GIL via kimix::runtime::common::gil_scoped_release;
 * Python objects are built only after the release scope closes.
 *
 * API:
 *   file.fnmatch_match(pattern: str, text: str, case_insensitive: bool = True) -> bool
 *   file.match_path_pattern(pattern: str, rel_path: str, case_insensitive: bool = True) -> bool
 *   file.is_unsafe_recursive_pattern(pattern: str) -> bool
 *   file.walk_matches_fs(root: str, pattern: str, include_dirs: bool = False,
 *                        max_matches: int = 0,
 *                        ignore_rules: list[tuple[str,bool,bool,bool]]|None = None) -> list[str]
 *   file.parse_gitignore(content: str, source_dir: str = "") -> list[tuple]
 *   file.pattern_has_regex_newline(pattern: str) -> tuple[str, bool]
 *   file.read_text(path: str, offset: int = 0, limit: int = 2000) -> str | None
 *   file.is_auto_generated_file_name(file_path: str) -> bool
 *   file.detect_auto_generated_marker(content: str, file_path: str) -> str | None
 *   file.check_json_format(text: str) -> str | None
 *   file.validate_format_by_path(file_path: str, text: str) -> tuple[str, str | None]
 *   file.build_unified_diff(old_text: str, new_text: str, path: str,
 *                           include_file_header: bool = True) -> str
 *   file.apply_edit(content: str, old_text: str, new_text: str,
 *                   replace_all: bool = False, max_replacements: int | None = None,
 *                   match_mode: str = "fuzzy") -> tuple[str, int, str | None]
 *   file.apply_diff_hunks(diff: str, content: str, allow_fuzzy: bool = True,
 *                         threshold: float = 0.75) -> tuple[str, int | None]
 *   file.sniff_image_dimensions(data: bytes) -> tuple[int,int,bool] | None
 *   file.detect_file_type(path: str, header: bytes | None = None) -> tuple[str, str]
 *   file.is_model_accepted_image_mime(mime: str) -> bool
 */

#include <pybind11/pybind11.h>

#include <core/binary_file_stream.h>

#include <runtime/runtime.h>
#include <runtime/common/gil.h>

#include <builtin_tools/glob_tool.h>
#include <builtin_tools/grep_tool.h>
#include <builtin_tools/read_tool.h>
#include <builtin_tools/write_tool.h>
#include <builtin_tools/edit_tool.h>
#include <builtin_tools/read_image_tool.h>

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

// Convert a Python bytes to a kimix::string_view (borrowed; data kept alive by
// the py::bytes object).  Only used when the caller holds the bytes object
// across the GIL release scope.
bool bytes_view(py::bytes data, kimix::string_view& view) {
    char* buf = nullptr;
    Py_ssize_t len = 0;
    if (PyBytes_AsStringAndSize(data.ptr(), &buf, &len) < 0) {
        return false;
    }
    view = kimix::string_view(buf, static_cast<size_t>(len));
    return true;
}

// Copy Python bytes into a kimix::string (for kernels that need to own the data).
bool bytes_to_string(py::bytes data, kimix::string& out) {
    char* buf = nullptr;
    Py_ssize_t len = 0;
    if (PyBytes_AsStringAndSize(data.ptr(), &buf, &len) < 0) {
        return false;
    }
    out.assign(buf, static_cast<size_t>(len));
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

py::object opt_int_to_obj(const kimix::optional<int32_t>& o) {
    if (o.has_value()) {
        return py::int_(*o);
    }
    return py::none();
}

py::object opt_size_t_to_obj(const kimix::optional<size_t>& o) {
    if (o.has_value()) {
        return py::int_(*o);
    }
    return py::none();
}

kimix::string tool_status_name(kimix::builtin_tools::tool_status s) {
    using kimix::builtin_tools::tool_status;
    switch (s) {
        case tool_status::ok: return "ok";
        case tool_status::invalid_input: return "invalid_input";
        case tool_status::not_found: return "not_found";
        case tool_status::no_change: return "no_change";
        case tool_status::ambiguous: return "ambiguous";
        case tool_status::blocked: return "blocked";
        case tool_status::too_large: return "too_large";
        case tool_status::unsupported: return "unsupported";
        case tool_status::external_library: return "external_library";
    }
    return "unknown";
}

kimix::string media_kind_name(kimix::builtin_tools::read_image::media_kind k) {
    using kimix::builtin_tools::read_image::media_kind;
    switch (k) {
        case media_kind::text: return "text";
        case media_kind::image: return "image";
        case media_kind::video: return "video";
        case media_kind::unknown: return "unknown";
    }
    return "unknown";
}

// Parse a Python list of ignore-rule tuples into the C++ struct.  Expected shape:
// [(pattern: str, negated: bool, anchored: bool, dir_only: bool), ...]
bool parse_ignore_rules_py(py::list rules, kimix::vector<kimix::builtin_tools::glob::ignore_rule>& out) {
    out.clear();
    out.reserve(static_cast<size_t>(PyList_GET_SIZE(rules.ptr())));
    for (py::handle item : rules) {
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
        kimix::builtin_tools::glob::ignore_rule rule;
        rule.pattern = h_pattern.cast<kimix::string>();
        rule.negated = py::cast<bool>(h_neg);
        rule.anchored = py::cast<bool>(h_anch);
        rule.dir_only = py::cast<bool>(h_dir);
        out.push_back(std::move(rule));
    }
    return true;
}

} // namespace

void py_register_builtin_file(py::module_& m) {
    auto file = m.def_submodule("file",
                                "Built-in file tool kernels (glob/grep/read/read_image/write/edit).");

    // ------------------------------------------------------------------
    // Glob kernels
    // ------------------------------------------------------------------
    file.def("fnmatch_match",
             [](py::str pattern, py::str text, bool case_insensitive) -> bool {
                 kimix::string pat, txt;
                 if (!str_to_string(pattern, pat) || !str_to_string(text, txt)) {
                     throw py::type_error("pattern and text must be str");
                 }
                 bool result = false;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     result = kimix::builtin_tools::glob::fnmatch_match(pat, txt, case_insensitive);
                 }
                 return result;
             },
             "Full-string fnmatch match mirroring pathlib/fnmatch semantics.",
             py::arg("pattern"), py::arg("text"), py::arg("case_insensitive") = true);

    file.def("match_path_pattern",
             [](py::str pattern, py::str rel_path, bool case_insensitive) -> bool {
                 kimix::string pat, rel;
                 if (!str_to_string(pattern, pat) || !str_to_string(rel_path, rel)) {
                     throw py::type_error("pattern and rel_path must be str");
                 }
                 bool result = false;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     result = kimix::builtin_tools::glob::match_path_pattern(pat, rel, case_insensitive);
                 }
                 return result;
             },
             "Parse a glob pattern and match a '/'-separated relative path in one shot.",
             py::arg("pattern"), py::arg("rel_path"), py::arg("case_insensitive") = true);

    file.def("is_unsafe_recursive_pattern",
             [](py::str pattern) -> bool {
                 kimix::string pat;
                 if (!str_to_string(pattern, pat)) {
                     throw py::type_error("pattern must be str");
                 }
                 bool result = false;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     result = kimix::builtin_tools::glob::is_unsafe_recursive_pattern(pat);
                 }
                 return result;
             },
             "True when a recursive pattern would match everything under the root.",
             py::arg("pattern"));

    file.def("walk_matches_fs",
             [](py::str root, py::str pattern, bool include_dirs, size_t max_matches,
                py::object ignore_rules) -> py::list {
                 kimix::string root_str, pattern_str;
                 if (!str_to_string(root, root_str) || !str_to_string(pattern, pattern_str)) {
                     throw py::type_error("root and pattern must be str");
                 }
                 kimix::vector<kimix::builtin_tools::glob::ignore_rule> rules;
                 const kimix::vector<kimix::builtin_tools::glob::ignore_rule>* rules_ptr = nullptr;
                 if (!ignore_rules.is_none()) {
                     if (!py::isinstance<py::list>(ignore_rules)) {
                         throw py::type_error("ignore_rules must be list or None");
                     }
                     if (!parse_ignore_rules_py(ignore_rules.cast<py::list>(), rules)) {
                         throw py::type_error(
                             "ignore_rules items must be (str, bool, bool, bool) tuples");
                     }
                     rules_ptr = &rules;
                 }
                 kimix::builtin_tools::glob::walk_options options;
                 options.include_dirs = include_dirs;
                 options.max_matches = max_matches;
                 options.ignore_rules = rules_ptr;
                 kimix::builtin_tools::tool_error parse_err;
                 kimix::vector<kimix::string> matches;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     auto result = kimix::builtin_tools::glob::walk_matches_fs(
                         root_str, pattern_str, options, parse_err);
                     if (!parse_err.failed()) {
                         matches.reserve(result.entries.size());
                         for (const auto& e : result.entries) {
                             matches.push_back(e.rel_path);
                         }
                     }
                 }
                 if (parse_err.failed()) {
                     throw py::value_error(parse_err.message.c_str());
                 }
                 py::list out;
                 for (const auto& p : matches) {
                     out.append(to_py_str(p));
                 }
                 return out;
             },
             "Walk the filesystem under root matching pattern. Returns a sorted list of "
             "relative paths. Raises ValueError on pattern parse failure.",
             py::arg("root"), py::arg("pattern"), py::arg("include_dirs") = false,
             py::arg("max_matches") = 0, py::arg("ignore_rules") = py::none());

    file.def("parse_gitignore",
             [](py::str content, py::str source_dir) -> py::list {
                 kimix::string text, src;
                 if (!str_to_string(content, text) || !str_to_string(source_dir, src)) {
                     throw py::type_error("content and source_dir must be str");
                 }
                 kimix::vector<kimix::builtin_tools::glob::ignore_rule> rules;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     rules = kimix::builtin_tools::glob::parse_ignore_rules(text, src);
                 }
                 py::list out;
                 for (const auto& r : rules) {
                     out.append(py::make_tuple(to_py_str(r.pattern), r.negated, r.anchored,
                                               r.dir_only, to_py_str(r.source_dir)));
                 }
                 return out;
             },
             "Parse .gitignore-style content into (pattern, negated, anchored, dir_only, "
             "source_dir) rules.",
             py::arg("content"), py::arg("source_dir") = "");

    // ------------------------------------------------------------------
    // Grep kernels
    // ------------------------------------------------------------------
    // The C++ grep tool does not expose a pure-text line scanner: the regex
    // search entry point (grep_search_lines) is intentionally BLOCKED and always
    // returns unsupported because PCRE2 is not vendored (see issue/grep.md).  The
    // Python shim keeps the exact Python matcher.  We therefore bind only the
    // lightweight pattern-analysis helpers below.
    file.def("pattern_has_regex_newline",
             [](py::str pattern) -> py::tuple {
                 kimix::string pat;
                 if (!str_to_string(pattern, pat)) {
                     throw py::type_error("pattern must be str");
                 }
                 bool out = false;
                 kimix::builtin_tools::tool_status status = kimix::builtin_tools::tool_status::ok;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     status = kimix::builtin_tools::grep::pattern_has_regex_newline(pat, out);
                 }
                 return py::make_tuple(to_py_str(tool_status_name(status)), out);
             },
             "Return (status, bool): True when the pattern contains a literal newline or "
             "an odd-backslash regex \\n escape.",
             py::arg("pattern"));

    // ------------------------------------------------------------------
    // Read kernels
    // ------------------------------------------------------------------
    file.def("read_text",
             [](py::str path, int64_t offset, int64_t limit) -> py::object {
                 kimix::string p;
                 if (!str_to_string(path, p)) {
                     throw py::type_error("path must be str");
                 }
                 if (offset < 0) {
                     throw py::value_error("offset must be >= 0");
                 }
                 if (limit < 0) {
                     throw py::value_error("limit must be >= 0");
                 }
                 kimix::string raw;
                 bool read_ok = false;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     kimix::BinaryFileStream stream(p);
                     if (stream) {
                         auto data = stream.read_all();
                         raw.assign(reinterpret_cast<const char*>(data.data()), data.size());
                         read_ok = true;
                     }
                 }
                 if (!read_ok) {
                     return py::none();
                 }
                 kimix::vector<kimix::string> lines;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     lines = kimix::builtin_tools::read::split_lines(raw);
                 }
                 const size_t start = static_cast<size_t>(offset);
                 if (start >= lines.size()) {
                     return py::str("");
                 }
                 const size_t count = static_cast<size_t>(limit) == 0
                                          ? lines.size() - start
                                          : std::min(static_cast<size_t>(limit), lines.size() - start);
                 kimix::string output;
                 output.reserve(raw.size());
                 for (size_t i = 0; i < count; ++i) {
                     if (i > 0) {
                         output.push_back('\n');
                     }
                     const auto& line = lines[start + i];
                     // read::split_lines keeps a trailing newline on every line
                     // except the last when the file does not end with a newline.
                     // Reconstruct by joining with '\n' and dropping the final
                     // newline that split_lines would have kept on the last line.
                     if (i + 1 == count) {
                         kimix::string_view view(line);
                         if (!view.empty() && view.back() == '\n') {
                             view.remove_suffix(1);
                             if (!view.empty() && view.back() == '\r') {
                                 view.remove_suffix(1);
                             }
                         }
                         output.append(view.data(), view.size());
                     } else {
                         kimix::string_view view(line);
                         if (!view.empty() && view.back() == '\r') {
                             view.remove_suffix(1);
                         }
                         if (!view.empty() && view.back() == '\n') {
                             view.remove_suffix(1);
                         }
                         output.append(view.data(), view.size());
                     }
                 }
                 return to_py_str(output);
             },
             "Read a text file and return the requested line window (offset is 0-based, "
             "limit 0 means unlimited). Returns None when the file cannot be read.",
             py::arg("path"), py::arg("offset") = 0, py::arg("limit") = 2000);

    // ------------------------------------------------------------------
    // Write kernels
    // ------------------------------------------------------------------
    file.def("is_auto_generated_file_name",
             [](py::str file_path) -> bool {
                 kimix::string p;
                 if (!str_to_string(file_path, p)) {
                     throw py::type_error("file_path must be str");
                 }
                 bool result = false;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     result = kimix::builtin_tools::write::is_auto_generated_file_name(p);
                 }
                 return result;
             },
             "True when the basename matches a known auto-generated filename pattern.",
             py::arg("file_path"));

    file.def("detect_auto_generated_marker",
             [](py::str content, py::str file_path) -> py::object {
                 kimix::string text, p;
                 if (!str_to_string(content, text) || !str_to_string(file_path, p)) {
                     throw py::type_error("content and file_path must be str");
                 }
                 kimix::optional<kimix::string> marker;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     marker = kimix::builtin_tools::write::detect_auto_generated_marker(text, p);
                 }
                 return opt_str_to_obj(marker);
             },
             "Detect a strong auto-generated header marker, or None.",
             py::arg("content"), py::arg("file_path"));

    file.def("check_json_format",
             [](py::str text) -> py::object {
                 kimix::string t;
                 if (!str_to_string(text, t)) {
                     throw py::type_error("text must be str");
                 }
                 kimix::optional<kimix::string> err;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     err = kimix::builtin_tools::write::check_json_format(t);
                 }
                 return opt_str_to_obj(err);
             },
             "Validate JSON text; returns None when valid or the decode error message.",
             py::arg("text"));

    file.def("validate_format_by_path",
             [](py::str file_path, py::str text) -> py::tuple {
                 kimix::string p, t;
                 if (!str_to_string(file_path, p) || !str_to_string(text, t)) {
                     throw py::type_error("file_path and text must be str");
                 }
                 kimix::string fmt_error;
                 kimix::builtin_tools::tool_status status;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     status = kimix::builtin_tools::write::validate_format_by_path(p, t, fmt_error);
                 }
                 py::object err_obj = fmt_error.empty() ? py::object(py::none())
                                                         : py::object(to_py_str(fmt_error));
                 return py::make_tuple(to_py_str(tool_status_name(status)), err_obj);
             },
             "Format validation dispatch by extension. Returns (status, fmt_error|None).",
             py::arg("file_path"), py::arg("text"));

    file.def("build_unified_diff",
             [](py::str old_text, py::str new_text, py::str path, bool include_file_header) -> py::str {
                 kimix::string old_t, new_t, p;
                 if (!str_to_string(old_text, old_t) || !str_to_string(new_text, new_t) ||
                     !str_to_string(path, p)) {
                     throw py::type_error("old_text, new_text and path must be str");
                 }
                 kimix::string result;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     result = kimix::builtin_tools::write::build_unified_diff(
                         old_t, new_t, p, include_file_header);
                 }
                 return to_py_str(result);
             },
             "Deterministic unified diff (empty string when identical).",
             py::arg("old_text"), py::arg("new_text"), py::arg("path"),
             py::arg("include_file_header") = true);

    // ------------------------------------------------------------------
    // Edit kernels
    // ------------------------------------------------------------------
    file.def("apply_edit",
             [](py::str content, py::str old_text, py::str new_text, bool replace_all,
                py::object max_replacements, py::str match_mode) -> py::tuple {
                 kimix::string c, old_t, new_t, mode;
                 if (!str_to_string(content, c) || !str_to_string(old_text, old_t) ||
                     !str_to_string(new_text, new_t) || !str_to_string(match_mode, mode)) {
                     throw py::type_error("content/old_text/new_text/match_mode must be str");
                 }
                 if (mode != "exact" && mode != "fuzzy") {
                     throw py::value_error("match_mode must be 'exact' or 'fuzzy'");
                 }
                 kimix::builtin_tools::edit::replace_edit_item edit;
                 edit.old_text = std::move(old_t);
                 edit.new_text = std::move(new_t);
                 edit.replace_all = replace_all;
                 edit.match_mode = std::move(mode);
                 if (!max_replacements.is_none()) {
                     if (!py::isinstance<py::int_>(max_replacements)) {
                         throw py::type_error("max_replacements must be int or None");
                     }
                     edit.max_replacements = max_replacements.cast<size_t>();
                 }
                 kimix::builtin_tools::edit::replace_result r;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     r = kimix::builtin_tools::edit::apply_edit(c, edit);
                 }
                 if (r.error.failed()) {
                     throw py::value_error(r.error.message.c_str());
                 }
                 return py::make_tuple(to_py_str(r.content), py::int_(r.replacements),
                                       opt_str_to_obj(r.suggestion));
             },
             "Apply a single replace edit (exact/fuzzy, first/all). Returns "
             "(new_content, replacements, suggestion|None).",
             py::arg("content"), py::arg("old_text"), py::arg("new_text"),
             py::arg("replace_all") = false, py::arg("max_replacements") = py::none(),
             py::arg("match_mode") = "fuzzy");

    file.def("apply_diff_hunks",
             [](py::str diff, py::str content, bool allow_fuzzy, double threshold) -> py::tuple {
                 kimix::string d, c;
                 if (!str_to_string(diff, d) || !str_to_string(content, c)) {
                     throw py::type_error("diff and content must be str");
                 }
                 kimix::builtin_tools::edit::hunks_result parsed;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     parsed = kimix::builtin_tools::edit::parse_diff_hunks(d);
                 }
                 if (parsed.error.failed()) {
                     throw py::value_error(parsed.error.message.c_str());
                 }
                 kimix::builtin_tools::edit::diff_apply_result r;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     r = kimix::builtin_tools::edit::apply_diff_hunks(
                         parsed.hunks, c, allow_fuzzy, threshold);
                 }
                 if (r.error.failed()) {
                     throw py::value_error(r.error.message.c_str());
                 }
                 return py::make_tuple(to_py_str(r.content), opt_int_to_obj(r.first_changed_line));
             },
             "Parse and apply a unified diff. Returns (new_content, first_changed_line|None).",
             py::arg("diff"), py::arg("content"), py::arg("allow_fuzzy") = true,
             py::arg("threshold") = 0.75);

    // ------------------------------------------------------------------
    // ReadImage kernels (pure metadata only; decode/encode needs third-party
    // image libraries that are not vendored in src/ext).
    // ------------------------------------------------------------------
    file.def("sniff_image_dimensions",
             [](py::bytes data) -> py::object {
                 kimix::string_view view;
                 if (!bytes_view(data, view)) {
                     throw py::error_already_set();
                 }
                 kimix::optional<kimix::builtin_tools::read_image::image_dimensions> dims;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     dims = kimix::builtin_tools::read_image::sniff_image_dimensions(view);
                 }
                 if (!dims.has_value()) {
                     return py::none();
                 }
                 return py::make_tuple(dims->width, dims->height, dims->transposed);
             },
             "Header-only image dimensions for PNG/GIF/BMP/WebP/JPEG. Returns "
             "(width, height, transposed) or None.",
             py::arg("data"));

    file.def("detect_file_type",
             [](py::str path, py::object header) -> py::tuple {
                 kimix::string p;
                 if (!str_to_string(path, p)) {
                     throw py::type_error("path must be str");
                 }
                 kimix::string hdr;
                 bool has_header = false;
                 if (!header.is_none()) {
                     if (py::isinstance<py::bytes>(header)) {
                         if (!bytes_to_string(header.cast<py::bytes>(), hdr)) {
                             throw py::error_already_set();
                         }
                         has_header = true;
                     } else if (py::isinstance<py::str>(header)) {
                         if (!str_to_string(header, hdr)) {
                             throw py::error_already_set();
                         }
                         has_header = true;
                     } else {
                         throw py::type_error("header must be bytes, str or None");
                     }
                 }
                 kimix::builtin_tools::read_image::file_type ft;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     ft = kimix::builtin_tools::read_image::detect_file_type(
                         p, hdr, has_header);
                 }
                 return py::make_tuple(to_py_str(media_kind_name(ft.kind)), to_py_str(ft.mime_type));
             },
             "Detect media kind and MIME type from suffix and/or header bytes.",
             py::arg("path"), py::arg("header") = py::none());

    file.def("is_model_accepted_image_mime",
             [](py::str mime) -> bool {
                 kimix::string m;
                 if (!str_to_string(mime, m)) {
                     throw py::type_error("mime must be str");
                 }
                 bool result = false;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     result = kimix::builtin_tools::read_image::is_model_accepted_image_mime(m);
                 }
                 return result;
             },
             "True when the normalized MIME is in the model-accepted image set.",
             py::arg("mime"));
}
