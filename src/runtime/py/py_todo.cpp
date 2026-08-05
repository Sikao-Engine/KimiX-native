/*
 * py_todo.cpp — Python bindings for the todo kernels (runtime_py.todo).
 *
 * BINDING-LAYER ONLY: this TU links against runtime.dll (pure C++ kernels)
 * and pybind11. Every kernel call releases the GIL via
 * kimix::runtime::common::gil_scoped_release; Python objects are built before
 * the release or after it is reacquired.
 */

#include <pybind11/pybind11.h>

#include <runtime/runtime.h>
#include <runtime/common/gil.h>
#include <runtime/todo/todo.h>

#include <format>
#include <optional>
#include <string>

namespace py = pybind11;
using namespace py::literals;

namespace {

// Lightweight formatting helper that avoids the kimix::format overload ambiguity
// when arguments are kimix::string.
template <typename... Args>
kimix::string kformat(std::format_string<Args...> fmt, Args&&... args) {
    auto text = std::format(fmt, std::forward<Args>(args)...);
    return kimix::string(text.begin(), text.end());
}

// Convert a Python object to a kimix::string via py::str.
kimix::string py_to_kstring(const py::object& obj) {
    std::string s = py::cast<std::string>(py::str(obj));
    return kimix::string(s.begin(), s.end());
}

// In-place ASCII whitespace trim.
void trim_inplace(kimix::string& s) noexcept {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    if (start == s.size()) {
        s.clear();
        return;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    s.assign(s.begin() + static_cast<kimix::string::difference_type>(start),
             s.begin() + static_cast<kimix::string::difference_type>(end));
}

// Parse a single Python dict into a TodoItem. Returns an error message on
// failure, or std::nullopt on success.
std::optional<kimix::string> parse_todo_item(const py::dict& d, kimix::runtime::todo::TodoItem& out) {
    using kimix::runtime::todo::canonical_status;

    auto title_obj = d.attr("get")("title");
    if (title_obj.is_none()) {
        return kimix::string("Missing title");
    }
    kimix::string title = py_to_kstring(title_obj);
    trim_inplace(title);
    if (title.empty()) {
        return kimix::string("Title cannot be empty or contain only whitespace.");
    }
    out.title = std::move(title);

    auto status_obj = d.attr("get")("status");
    if (status_obj.is_none()) {
        return kimix::string("Missing status");
    }
    kimix::string status = py_to_kstring(status_obj);
    auto canon = canonical_status(status);
    if (!canon) {
        return kformat("Invalid status '{}'. Must be one of: pending, in_progress, done.", status);
    }
    out.status = std::move(*canon);

    out.has_notes = false;
    out.notes.clear();
    auto notes_obj = d.attr("get")("notes");
    if (!notes_obj.is_none()) {
        kimix::string notes = py_to_kstring(notes_obj);
        trim_inplace(notes);
        if (!notes.empty()) {
            out.has_notes = true;
            out.notes = std::move(notes);
        }
    }

    out.has_code = false;
    out.code.clear();
    auto code_obj = d.attr("get")("code");
    if (!code_obj.is_none()) {
        out.has_code = true;
        out.code = py_to_kstring(code_obj);
    }

    return std::nullopt;
}

// Convert a kernel TodoItem back into a Python dict.
py::dict item_to_dict(const kimix::runtime::todo::TodoItem& item) {
    py::dict d;
    d["title"] = item.title;
    d["status"] = item.status;
    d["notes"] = item.has_notes ? py::cast(item.notes) : py::none();
    d["code"] = item.has_code ? py::cast(item.code) : py::none();
    return d;
}

// Parse a list[dict] into a vector<TodoItem>. On failure returns a dict-shaped
// error result (caller should return it immediately).
py::object parse_item_list(
    const py::list& src,
    kimix::vector<kimix::runtime::todo::TodoItem>& out) {
    out.clear();
    out.reserve(static_cast<size_t>(src.size()));
    for (size_t i = 0; i < static_cast<size_t>(src.size()); ++i) {
        py::object entry = src[i];
        if (!py::isinstance<py::dict>(entry)) {
            return py::dict(
                "error"_a = kformat("Item at index {} is not a dict", i),
                "items"_a = py::list(),
                "warnings"_a = py::list(),
                "regressed"_a = py::list(),
                "archived"_a = py::list());
        }
        kimix::runtime::todo::TodoItem item;
        auto err = parse_todo_item(entry.cast<py::dict>(), item);
        if (err) {
            return py::dict(
                "error"_a = kformat("Invalid todo at index {}: {}", i, *err),
                "items"_a = py::list(),
                "warnings"_a = py::list(),
                "regressed"_a = py::list(),
                "archived"_a = py::list());
        }
        out.push_back(std::move(item));
    }
    return py::none();
}

kimix::map<kimix::string, kimix::vector<kimix::string>> parse_fuzzy_warnings(
    const py::dict& fuzzy_warnings) {
    kimix::map<kimix::string, kimix::vector<kimix::string>> out;
    for (auto item : fuzzy_warnings) {
        std::string key = py::cast<std::string>(py::str(item.first));
        py::list warnings = item.second.cast<py::list>();
        kimix::vector<kimix::string> vals;
        vals.reserve(static_cast<size_t>(warnings.size()));
        for (size_t i = 0; i < static_cast<size_t>(warnings.size()); ++i) {
            std::string w = py::cast<std::string>(py::str(warnings[i]));
            vals.emplace_back(w.begin(), w.end());
        }
        out.emplace(kimix::string(key.begin(), key.end()), std::move(vals));
    }
    return out;
}

} // namespace

void py_register_todo(py::module_& m) {
    using kimix::runtime::todo::MergeResult;
    using kimix::runtime::todo::TodoItem;
    using kimix::runtime::todo::canonical_status;
    using kimix::runtime::todo::format_summary;
    using kimix::runtime::todo::merge;
    using kimix::runtime::todo::status_counts;

    m.doc() = "Todo kernels: merge, status counts, plain-text summary.";

    // ------------------------------------------------------------------
    // merge(old_items, new_items, mode, fuzzy_warnings={})
    // ------------------------------------------------------------------
    m.def(
        "merge",
        [](py::list old_py,
           py::list new_py,
           const std::string& mode,
           py::dict fuzzy_py) -> py::dict {
            kimix::vector<TodoItem> old_items;
            kimix::vector<TodoItem> new_items;

            py::object err_result = parse_item_list(old_py, old_items);
            if (!err_result.is_none()) {
                return err_result.cast<py::dict>();
            }
            err_result = parse_item_list(new_py, new_items);
            if (!err_result.is_none()) {
                return err_result.cast<py::dict>();
            }

            auto fuzzy = parse_fuzzy_warnings(fuzzy_py);

            MergeResult result;
            {
                kimix::runtime::common::gil_scoped_release release;
                result = merge(old_items, new_items, mode, fuzzy);
            }

            py::dict out;
            out["error"] = result.error ? py::cast(*result.error) : py::none();

            py::list items;
            for (const auto& item : result.items) {
                items.append(item_to_dict(item));
            }
            out["items"] = std::move(items);

            py::list warnings;
            for (const auto& w : result.warnings) {
                warnings.append(w);
            }
            out["warnings"] = std::move(warnings);

            py::list regressed;
            for (const auto& r : result.regressed) {
                regressed.append(r);
            }
            out["regressed"] = std::move(regressed);

            py::list archived;
            for (const auto& item : result.archived) {
                archived.append(item_to_dict(item));
            }
            out["archived"] = std::move(archived);

            return out;
        },
        "Merge/update todo lists (mirrors TodoList._write_todos merge path).",
        py::arg("old_items"),
        py::arg("new_items"),
        py::arg("mode"),
        py::arg("fuzzy_warnings") = py::dict());

    // ------------------------------------------------------------------
    // status_counts(items)
    // ------------------------------------------------------------------
    m.def(
        "status_counts",
        [](py::list items_py) -> py::dict {
            kimix::vector<TodoItem> items;
            py::object err_result = parse_item_list(items_py, items);
            if (!err_result.is_none()) {
                py::dict err;
                err["pending"] = 0;
                err["in_progress"] = 0;
                err["done"] = 0;
                return err;
            }

            kimix::map<kimix::string, size_t> counts;
            {
                kimix::runtime::common::gil_scoped_release release;
                counts = status_counts(items);
            }

            py::dict out;
            out["pending"] = static_cast<Py_ssize_t>(counts["pending"]);
            out["in_progress"] = static_cast<Py_ssize_t>(counts["in_progress"]);
            out["done"] = static_cast<Py_ssize_t>(counts["done"]);
            return out;
        },
        "Count todos by canonical status.",
        py::arg("items"));

    // ------------------------------------------------------------------
    // format_summary(items, max_items=50)
    // ------------------------------------------------------------------
    m.def(
        "format_summary",
        [](py::list items_py, size_t max_items) -> py::str {
            kimix::vector<TodoItem> items;
            py::object err_result = parse_item_list(items_py, items);
            if (!err_result.is_none()) {
                return py::str("");
            }

            kimix::string result;
            {
                kimix::runtime::common::gil_scoped_release release;
                result = format_summary(items, max_items);
            }
            return py::cast(result);
        },
        "Plain-text summary of pending/in_progress todos.",
        py::arg("items"),
        py::arg("max_items") = 50);
}
