/*
 * py_history.cpp — Python bindings for the HistoryIndex kernel (runtime_py.index).
 *
 * BINDING-LAYER ONLY (same conventions as py_index.cpp): bytes in/bytes out
 * for text, GIL released around every kernel call via
 * kimix::runtime::common::gil_scoped_release, Python objects built only after
 * the release guard destructs.
 *
 * Contract with the shim (python/kimix_native/index.py):
 *   - append_turns takes list[tuple[int, float, int, bool, bytes]] —
 *     (turn_id, timestamp, role 0..3, is_compacted, text_utf8). The text is
 *     ALREADY normalized (lowercased + NFKC'd) and stripped by the shim.
 *   - search(query_bytes, top_k=3) -> list[dict] with keys
 *     turn_id/timestamp/role(int)/is_compacted/text(bytes)/score(float).
 *     The shim converts role int -> str and text bytes -> str.
 *   - get_by_id(turn_id) -> dict (no "score" key) | None.
 *   - save() -> bytes (KNHIX1); load(bytes) -> bool (False on wrong magic).
 */

#include <pybind11/pybind11.h>

#include <runtime/runtime.h>
#include <runtime/common/gil.h>
#include <runtime/index/history_index.h>

namespace py = pybind11;

namespace {

namespace idx = kimix::runtime::index;

bool bytes_view(py::bytes data, kimix::string_view& view) {
    char* buf = nullptr;
    Py_ssize_t len = 0;
    if (PyBytes_AsStringAndSize(data.ptr(), &buf, &len) < 0) {
        return false;
    }
    view = kimix::string_view(buf, static_cast<size_t>(len));
    return true;
}

// Parse the append_turns argument list into owning turn_meta structs. Pure
// C++ (borrowed handles + copies) while the GIL is held — no Python API calls
// beyond the C-level extractors, so releasing the GIL for the kernel call is
// safe afterwards.
bool parse_turns(py::list turns, kimix::vector<idx::turn_meta>& out) {
    const Py_ssize_t n = PyList_GET_SIZE(turns.ptr());
    out.clear();
    out.reserve(static_cast<size_t>(n));
    for (Py_ssize_t i = 0; i < n; ++i) {
        py::handle item = PyList_GET_ITEM(turns.ptr(), i);
        if (!PyTuple_Check(item.ptr()) || PyTuple_GET_SIZE(item.ptr()) != 5) {
            return false;
        }
        py::handle h_id = PyTuple_GET_ITEM(item.ptr(), 0);
        py::handle h_ts = PyTuple_GET_ITEM(item.ptr(), 1);
        py::handle h_role = PyTuple_GET_ITEM(item.ptr(), 2);
        py::handle h_comp = PyTuple_GET_ITEM(item.ptr(), 3);
        py::handle h_text = PyTuple_GET_ITEM(item.ptr(), 4);
        if (!PyLong_Check(h_id.ptr()) || !PyBytes_Check(h_text.ptr()) ||
            !(PyFloat_Check(h_ts.ptr()) || PyLong_Check(h_ts.ptr())) ||
            !PyLong_Check(h_role.ptr())) {
            return false;
        }
        idx::turn_meta t;
        t.turn_id = static_cast<uint32_t>(PyLong_AsUnsignedLong(h_id.ptr()));
        t.timestamp = PyFloat_AsDouble(h_ts.ptr()); // works for int too
        t.role = static_cast<uint8_t>(PyLong_AsUnsignedLong(h_role.ptr()));
        t.is_compacted = PyObject_IsTrue(h_comp.ptr()) == 1;
        char* buf = nullptr;
        Py_ssize_t len = 0;
        if (PyBytes_AsStringAndSize(h_text.ptr(), &buf, &len) < 0) {
            return false;
        }
        t.text.assign(buf, static_cast<size_t>(len));
        out.push_back(std::move(t));
    }
    return true;
}

py::dict turn_to_dict(const idx::turn_meta& t, bool with_score) {
    py::dict d;
    d["turn_id"] = t.turn_id;
    d["timestamp"] = t.timestamp;
    d["role"] = t.role;
    d["text"] = py::bytes(t.text.data(), t.text.size());
    d["is_compacted"] = t.is_compacted;
    if (with_score) {
        d["score"] = t.score;
    }
    return d;
}

} // namespace

void py_register_history(py::module_& m) {
    py::class_<idx::HistoryIndex>(m, "HistoryIndex")
        .def(py::init<>())
        .def_property_readonly_static(
            "MAX_TURNS", [](py::object /*self*/) { return idx::HistoryIndex::MAX_TURNS; })
        .def("append_turns",
             [](idx::HistoryIndex& h, py::list turns) {
                 kimix::vector<idx::turn_meta> metas;
                 if (!parse_turns(turns, metas)) {
                     throw py::type_error(
                         "turns must be list[tuple[int, float, int, bool, bytes]]");
                 }
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     h.append_turns(metas);
                 }
             },
             "Append turns incrementally (turn_id, timestamp, role, "
             "is_compacted, text_utf8). Evicts the oldest beyond MAX_TURNS.",
             py::arg("turns"))
        .def("mark_compacted", [](idx::HistoryIndex& h) {
                 kimix::runtime::common::gil_scoped_release release;
                 h.mark_compacted();
             },
             "Set is_compacted=True on all currently-indexed turns.")
        .def("set_persist_path",
             [](idx::HistoryIndex& h, py::bytes path) {
                 kimix::string_view view;
                 if (!bytes_view(path, view)) {
                     throw py::error_already_set();
                 }
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     h.set_persist_path(view);
                 }
             },
             py::arg("path"))
        .def("search",
             [](idx::HistoryIndex& h, py::bytes query, uint32_t top_k) -> py::list {
                 kimix::string_view view;
                 if (!bytes_view(query, view)) {
                     throw py::error_already_set();
                 }
                 kimix::vector<idx::turn_meta> results;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     results = h.search(view, top_k);
                 }
                 py::list out;
                 for (const auto& t : results) {
                     out.append(turn_to_dict(t, true));
                 }
                 return out;
             },
             "Top-k BM25 matches: list of dicts with "
             "turn_id/timestamp/role(int)/is_compacted/text(bytes)/score. "
             "Ordered (score desc, turn_id asc).",
             py::arg("query"), py::arg("top_k") = 3)
        .def("get_by_id",
             [](idx::HistoryIndex& h, uint32_t turn_id) -> py::object {
                 const idx::turn_meta* found = nullptr;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     found = h.get_by_id(turn_id);
                 }
                 if (found == nullptr) {
                     return py::none();
                 }
                 return turn_to_dict(*found, false);
             },
             "Turn dict (no score) or None when evicted/unknown.",
             py::arg("turn_id"))
        .def("save",
             [](const idx::HistoryIndex& h) -> py::bytes {
                 kimix::string blob;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     h.save_to(blob);
                 }
                 return py::bytes(blob.data(), blob.size());
             },
             "Serialize to a KNHIX1 blob (turn metadata + KNIDX1 index blob).")
        .def("load",
             [](idx::HistoryIndex& h, py::bytes blob) -> bool {
                 kimix::string_view view;
                 if (!bytes_view(blob, view)) {
                     throw py::error_already_set();
                 }
                 bool ok = false;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     ok = h.load_from(view);
                 }
                 return ok;
             },
             "Deserialize a KNHIX1 blob (no re-tokenization); False on "
             "malformed input (the object is reset).",
             py::arg("blob"))
        .def("turn_count",
             [](const idx::HistoryIndex& h) { return h.turn_count(); })
        .def("pop_front", [](idx::HistoryIndex& h) {
                 kimix::runtime::common::gil_scoped_release release;
                 h.pop_front();
             },
             "Drop the oldest turn (O(1)); the index keeps stale postings.")
        .def("reset", [](idx::HistoryIndex& h) {
                 kimix::runtime::common::gil_scoped_release release;
                 h.reset();
             },
             "Clear all turns and the index.");
}
