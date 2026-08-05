/*
 * py_json.cpp -- Python bindings for the JSON kernels (runtime_py.json).
 *
 * BINDING-LAYER ONLY: links against runtime.dll (pure C++ kernels) and
 * pybind11. feed() releases the GIL (the lexer scans every byte of the
 * accumulated buffer); value_span returns BYTE OFFSETS into the kernel's
 * internal buffer -- the shim copies the bytes out immediately (spans are
 * documented as invalidated by the next feed).
 *
 * Plan 016 additions: JsonStore (one yyjson document per task file),
 * scan_notifications (single-parse JSONL batch scan + sort), and the
 * SchemaOps bytes-in/bytes-out helpers (deref_json_schema /
 * ensure_property_types).
 */

#include <pybind11/pybind11.h>

#include <runtime/common/gil.h>
#include <runtime/json/incremental_lexer.h>
#include <runtime/json/json_store.h>
#include <runtime/json/schema_ops.h>

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

// Parse a JSON bytes object into a Python object (GIL released for the
// parse; used to convert the notification scan rows).
py::object json_bytes_to_object(const kimix::string& s) {
    py::object json_mod = py::module_::import("json");
    py::bytes b(s.data(), s.size());
    return json_mod.attr("loads")(b);
}

} // namespace

void py_register_json(py::module_& m) {
    m.doc() = "JSON kernels: incremental lexer (streaming completeness, "
              "top-level key extraction, relaxed parse)";

    py::class_<kimix::runtime::json::IncrementalJsonLexer>(m, "IncrementalJsonLexer",
        "Incremental JSON lexer: feed() chunks, is_complete() detects a "
        "complete top-level value, value_span()/top_level_keys() extract "
        "top-level key -> value byte spans WITHOUT reparsing. Relaxed: "
        "trailing commas + // and /* */ comments; raw newlines allowed "
        "inside strings.")
        .def(py::init<>())
        .def("feed",
             [](kimix::runtime::json::IncrementalJsonLexer& lex, py::bytes chunk) {
                 kimix::string_view view;
                 if (!bytes_view(chunk, view)) {
                     throw py::error_already_set();
                 }
                 kimix::runtime::common::gil_scoped_release release;
                 lex.feed(view);
             },
             "Feed one chunk of UTF-8/JSON text (state is incremental).",
             py::arg("chunk"))
        .def("is_complete",
             &kimix::runtime::json::IncrementalJsonLexer::is_complete)
        .def("has_error",
             &kimix::runtime::json::IncrementalJsonLexer::has_error)
        .def("value_span",
             [](const kimix::runtime::json::IncrementalJsonLexer& lex,
                py::str key) -> py::object {
                 Py_ssize_t key_len = 0;
                 const char* key_cstr = PyUnicode_AsUTF8AndSize(key.ptr(), &key_len);
                 if (key_cstr == nullptr) {
                     throw py::error_already_set();
                 }
                 size_t start = 0;
                 size_t end = 0;
                 const bool found = lex.value_span(
                     kimix::string_view(key_cstr, static_cast<size_t>(key_len)),
                     start, end);
                 if (!found) {
                     return py::none();
                 }
                 return py::make_tuple(static_cast<size_t>(start), static_cast<size_t>(end));
             },
             "Byte span (start, end) of a top-level key's value in the "
             "internal buffer, or None. Offsets are invalidated by the next "
             "feed -- copy the bytes you need first.",
             py::arg("key"))
        .def("top_level_keys",
             [](const kimix::runtime::json::IncrementalJsonLexer& lex) -> py::list {
                 kimix::vector<kimix::string> keys;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     keys = lex.top_level_keys();
                 }
                 py::list out;
                 for (const auto& k : keys) {
                     out.append(py::str(k.data(), k.size()));
                 }
                 return out;
             },
             "Keys of the top-level object, in document order (only "
             "completed pairs).")
        .def("reset", &kimix::runtime::json::IncrementalJsonLexer::reset);

    // ------------------------------------------------------------------
    // JsonStore (plan 016) -- one yyjson document per task file.
    // ------------------------------------------------------------------
    py::class_<kimix::runtime::json::JsonStore>(m, "JsonStore",
        "Native JSON document store: load() parses once, update() deep-merges "
        "a partial object, get()/save_atomic() serialize with orjson "
        "OPT_INDENT_2-compatible bytes (atomic tmp+rename write).")
        .def(py::init<>())
        .def("load",
             [](kimix::runtime::json::JsonStore& store, py::bytes data) {
                 kimix::string_view view;
                 if (!bytes_view(data, view)) {
                     throw py::error_already_set();
                 }
                 kimix::runtime::common::gil_scoped_release release;
                 store.load(view);
             },
             "Parse `bytes` as the new document (invalid JSON resets to {}).",
             py::arg("data"))
        .def("update",
             [](kimix::runtime::json::JsonStore& store, py::bytes data) {
                 kimix::string_view view;
                 if (!bytes_view(data, view)) {
                     throw py::error_already_set();
                 }
                 kimix::runtime::common::gil_scoped_release release;
                 store.update(view);
             },
             "Deep-merge a partial JSON object into the document.",
             py::arg("data"))
        .def("get",
             [](const kimix::runtime::json::JsonStore& store) -> py::bytes {
                 kimix::string out;
                 {
                     // Nested block: the GIL must be reacquired before the
                     // return value is built (T6 lesson -- never build Python
                     // objects while the GIL is released).
                     kimix::runtime::common::gil_scoped_release release;
                     store.get(out);
                 }
                 return to_bytes(out);
             },
             "Pretty (2-space indent) JSON bytes of the document.")
        .def("keys",
             [](const kimix::runtime::json::JsonStore& store) -> py::list {
                 kimix::vector<kimix::string> keys;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     keys = store.keys();
                 }
                 py::list out;
                 for (const auto& k : keys) {
                     out.append(py::str(k.data(), k.size()));
                 }
                 return out;
             },
             "Top-level keys in document order.")
        .def("save_atomic",
             [](const kimix::runtime::json::JsonStore& store, py::str path)
                 -> py::bytes {
                 Py_ssize_t len = 0;
                 const char* cstr = PyUnicode_AsUTF8AndSize(path.ptr(), &len);
                 if (cstr == nullptr) {
                     throw py::error_already_set();
                 }
                 kimix::string blob;
                 bool ok = false;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     ok = store.save_atomic(
                         kimix::string_view(cstr, static_cast<size_t>(len)), blob);
                 }
                 if (!ok) {
                     throw py::value_error("save_atomic failed");
                 }
                 return to_bytes(blob);
             },
             "Atomically write the pretty serialization to `path` (tmp + "
             "rename); returns the written bytes.",
             py::arg("path"))
        .def("clear",
             [](kimix::runtime::json::JsonStore& store) {
                 kimix::runtime::common::gil_scoped_release release;
                 store.clear();
             },
             "Reset to an empty document.")
        .def("loaded", &kimix::runtime::json::JsonStore::loaded);

    // ------------------------------------------------------------------
    // scan_notifications (plan 016) -- one parse, one sort.
    // ------------------------------------------------------------------
    m.def("scan_notifications",
          [](py::bytes jsonl, uint64_t now_ms) -> py::list {
              kimix::string_view view;
              if (!bytes_view(jsonl, view)) {
                  throw py::error_already_set();
              }
              kimix::vector<kimix::runtime::json::notification_row> rows;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::json::scan_notifications(view, now_ms, rows);
              }
              py::list out;
              for (const auto& r : rows) {
                  py::dict item;
                  item["id"] = py::str(r.id.data(), r.id.size());
                  item["created_at"] = r.created_at;
                  item["event"] = json_bytes_to_object(r.event_json);
                  item["delivery"] = r.delivery_json.empty()
                                          ? py::dict()
                                          : json_bytes_to_object(r.delivery_json);
                  out.append(item);
              }
              return out;
          },
          "One parse + one sort of a JSONL of {event, delivery} notification "
          "views; returns [{id, created_at, event, delivery}] sorted by "
          "created_at descending.",
          py::arg("jsonl"), py::arg("now_ms") = 0);

    // ------------------------------------------------------------------
    // SchemaOps (plan 016) -- bytes in / bytes out.
    // ------------------------------------------------------------------
    m.def("deref_json_schema",
          [](py::bytes schema, py::list registry) -> py::bytes {
              kimix::string_view view;
              if (!bytes_view(schema, view)) {
                  throw py::error_already_set();
              }
              kimix::vector<kimix::string_view> regs;
              const Py_ssize_t n = PyList_GET_SIZE(registry.ptr());
              regs.reserve(static_cast<size_t>(n));
              for (Py_ssize_t i = 0; i < n; ++i) {
                  py::object item = py::reinterpret_borrow<py::object>(
                      PyList_GET_ITEM(registry.ptr(), i));
                  if (!PyBytes_Check(item.ptr())) {
                      throw py::type_error("registry items must be bytes");
                  }
                  char* buf = nullptr;
                  Py_ssize_t len = 0;
                  if (PyBytes_AsStringAndSize(item.ptr(), &buf, &len) < 0) {
                      throw py::error_already_set();
                  }
                  regs.push_back(kimix::string_view(buf, static_cast<size_t>(len)));
              }
              kimix::string out;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::json::deref_json_schema(view, regs, out);
              }
              return to_bytes(out);
          },
          "Inline local $ref pointers and drop dead definition buckets "
          "(compact JSON bytes out).",
          py::arg("schema"), py::arg("registry") = py::list());

    m.def("ensure_property_types",
          [](py::bytes schema) -> py::bytes {
              kimix::string_view view;
              if (!bytes_view(schema, view)) {
                  throw py::error_already_set();
              }
              kimix::string out;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::json::ensure_property_types(view, out);
              }
              return to_bytes(out);
          },
          "Deep copy with an explicit `type` on every nested property schema "
          "(compact JSON bytes out).",
          py::arg("schema"));
}
