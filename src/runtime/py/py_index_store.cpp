/*
 * py_index_store.cpp - Python bindings for index.SqliteHistoryIndex and the
 * message-row kernels (runtime_py.index).
 *
 * BINDING-LAYER ONLY. Fills the `index` submodule that py_index.cpp /
 * py_history.cpp already share (same two-TU precedent).
 *
 * CONTRACT WITH THE SHIM (python/kimix_native/index.py):
 *
 *   index.SqliteHistoryIndex(path: str)
 *     .open() -> (bool ok, str error) ; .is_open ; .fts_stale
 *     .next_turn_id ; .reserve_turn_id() -> int ; .turn_count()
 *     .append_turns_packed(blob) -> int      # u32 count | {u32 id, f64 ts,
 *                                            #   u8 role, u8 flags, u32 len, text}
 *     .insert_turn_with_id(id, ts, role, flags, text) -> None
 *     .mark_compacted() ; .save() ; .rebuild_fts() ; .clear() ; .close()
 *     .search(query: bytes, top_k) -> packed hits
 *     .search_with_recency(query: bytes, top_k, weight) -> packed hits
 *         hits = u32 count | {u32 id, f64 ts, u8 role, u8 flags, f64 score,
 *                             f64 boosted, u32 len, text}
 *     .get_by_id(id) -> bytes|None        # the stored text, verbatim
 *     .non_compacted_turn_ids() -> packed u32 (soul.pack_u32 layout)
 *     .bytes_owned() -> int
 *
 *   index.context_rows(structure, history) -> bytes     # (json row, fts) pairs
 *   index.parse_rows(blob, history_handle) -> int       # into a soul.History
 *
 * Row/turn blobs are byte-identical to what the shim's `_compat` produces; the
 * layouts are documented here and in runtime/index/message_rows.h.
 */

#include <pybind11/pybind11.h>

#include <runtime/common/gil.h>
#include <runtime/common/le_bytes.h>
#include <runtime/index/history_index.h>
#include <runtime/index/message_rows.h>
#include <runtime/index/sqlite_history_index.h>
#include <runtime/runtime.h>
#include <runtime/soul/history_store.h>

#include <runtime/py/py_soul_bridge.h>
#include "py_util.h"

using kimix_py_util::bytes_view;
using kimix_py_util::str_to_string;
using kimix_py_util::to_bytes;
using kimix_py_util::to_py_str;

namespace py = pybind11;

namespace src_runtime_py_py_index_store {

using kimix::runtime::common::read_le32;
using kimix::runtime::common::read_le64;
using kimix::runtime::common::put_le32;
using kimix::runtime::common::put_le64;
using kimix::runtime::index::SqliteHistoryIndex;
using kimix::runtime::index::turn_meta;

// One packed input record: {u32 id, f64 ts, u8 role, u8 flags, u32 len, text}.
bool parse_turn_blob(kimix::string_view blob, kimix::vector<turn_meta> &out) {
    if (blob.size() < 4) {
        return false;
    }
    const uint32_t count = read_le32(blob.data());
    const char *p = blob.data() + 4;
    const char *end = blob.data() + blob.size();
    out.clear();
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (static_cast<size_t>(end - p) < 18) {
            return false;
        }
        turn_meta t;
        t.turn_id = read_le32(p);
        const uint64_t bits = read_le64(p + 4);
        double ts = 0.0;
        memcpy(&ts, &bits, sizeof(double));
        t.timestamp = ts;
        t.role = static_cast<uint8_t>(p[12]);
        t.is_compacted = (static_cast<uint8_t>(p[13]) & 1u) != 0;
        const uint32_t len = read_le32(p + 14);
        p += 18;
        if (static_cast<size_t>(end - p) < len) {
            return false;
        }
        t.text.assign(p, len);
        p += len;
        out.push_back(std::move(t));
    }
    return true;
}

// Packed search result (see the file header for the layout).
py::bytes pack_hits(const kimix::vector<turn_meta> &hits) {
    size_t total = 4;
    for (const turn_meta &t : hits) {
        total += 34 + t.text.size();
    }
    kimix::string buf;
    buf.resize(total);
    char *p = buf.data();
    put_le32(p, static_cast<uint32_t>(hits.size()));
    p += 4;
    for (const turn_meta &t : hits) {
        put_le32(p, t.turn_id);
        const double ts = t.timestamp;
        uint64_t bits = 0;
        memcpy(&bits, &ts, sizeof(double));
        put_le64(p + 4, bits);
        p[12] = static_cast<char>(t.role);
        p[13] = static_cast<char>(t.is_compacted ? 1 : 0);
        const double score = t.score;
        memcpy(&bits, &score, sizeof(double));
        put_le64(p + 14, bits);
        const double boosted = t.boosted_score;
        memcpy(&bits, &boosted, sizeof(double));
        put_le64(p + 22, bits);
        put_le32(p + 30, static_cast<uint32_t>(t.text.size()));
        p += 34;
        if (!t.text.empty()) {
            memcpy(p, t.text.data(), t.text.size());
        }
        p += t.text.size();
    }
    return py::bytes(buf.data(), buf.size());
}

} // namespace src_runtime_py_py_index_store
using namespace src_runtime_py_py_index_store;

void py_register_index_store(py::module_& m) {
    // ------------------------------------------------------------------
    // W3: the durable FTS5 backend (deleting kimix.retrieval needs it)
    // ------------------------------------------------------------------
    py::class_<SqliteHistoryIndex>(m, "SqliteHistoryIndex",
                                   "Durable FTS5 history index (the apsw backend "
                                   "of kimi_cli/soul/history_index.py).")
        .def(py::init([](py::str path) {
                 kimix::string narrow;
                 if (!str_to_string(path, narrow)) {
                     throw py::error_already_set();
                 }
                 kimix::filesystem::path p;
                 if (!kimix::path_from_narrow(narrow, p)) {
                     throw py::value_error("path is not representable on this host");
                 }
                 return kimix::unique_ptr<SqliteHistoryIndex>(
                     new SqliteHistoryIndex(std::move(p)));
             }),
             py::arg("path"))
        .def("open",
             [](SqliteHistoryIndex &idx) -> py::tuple {
                 kimix::string error;
                 bool ok = false;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     ok = idx.open(error);
                 }
                 return py::make_tuple(ok, to_py_str(error));
             },
             "open() -> (ok, error)")
        .def("close",
             [](SqliteHistoryIndex &idx) {
                 kimix::runtime::common::gil_scoped_release release;
                 idx.close();
             })
        .def_property_readonly("is_open", &SqliteHistoryIndex::is_open)
        .def_property_readonly("fts_stale", &SqliteHistoryIndex::fts_stale)
        .def_property_readonly("next_turn_id", &SqliteHistoryIndex::next_turn_id)
        .def("reserve_turn_id", &SqliteHistoryIndex::reserve_turn_id)
        .def("turn_count", &SqliteHistoryIndex::turn_count)
        .def("append_turns_packed",
             [](SqliteHistoryIndex &idx, py::bytes blob) -> int {
                 kimix::string_view view;
                 if (!bytes_view(blob, view)) {
                     throw py::error_already_set();
                 }
                 kimix::vector<turn_meta> turns;
                 if (!parse_turn_blob(view, turns)) {
                     throw py::value_error("invalid packed turn blob");
                 }
                 const int before = static_cast<int>(idx.turn_count());
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     idx.append_turns(turns);
                 }
                 return static_cast<int>(idx.turn_count()) - before;
             },
             "append_turns_packed(blob) -> rows appended", py::arg("blob"))
        .def("insert_turn_with_id",
             [](SqliteHistoryIndex &idx, uint32_t turn_id, double timestamp,
                uint32_t role, py::bytes text) {
                 kimix::string_view view;
                 if (!bytes_view(text, view)) {
                     throw py::error_already_set();
                 }
                 turn_meta t;
                 t.turn_id = turn_id;
                 t.timestamp = timestamp;
                 t.role = static_cast<uint8_t>(role);
                 t.text.assign(view.data(), view.size());
                 kimix::runtime::common::gil_scoped_release release;
                 idx.insert_turn_with_id(t);
             },
             py::arg("turn_id"), py::arg("timestamp"), py::arg("role"),
             py::arg("text"))
        .def("mark_compacted",
             [](SqliteHistoryIndex &idx) {
                 kimix::runtime::common::gil_scoped_release release;
                 idx.mark_compacted();
             })
        .def("save",
             [](SqliteHistoryIndex &idx) {
                 kimix::runtime::common::gil_scoped_release release;
                 idx.save();
             })
        .def("rebuild_fts",
             [](SqliteHistoryIndex &idx) {
                 kimix::runtime::common::gil_scoped_release release;
                 idx.rebuild_fts();
             })
        .def("clear",
             [](SqliteHistoryIndex &idx) {
                 kimix::runtime::common::gil_scoped_release release;
                 idx.clear();
             })
        .def("search",
             [](SqliteHistoryIndex &idx, py::bytes query, uint32_t top_k) -> py::bytes {
                 kimix::string_view view;
                 if (!bytes_view(query, view)) {
                     throw py::error_already_set();
                 }
                 kimix::vector<turn_meta> hits;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     hits = idx.search(view, top_k);
                 }
                 return pack_hits(hits);
             },
             py::arg("query"), py::arg("top_k"))
        .def("search_with_recency",
             [](SqliteHistoryIndex &idx, py::bytes query, uint32_t top_k,
                double weight) -> py::bytes {
                 kimix::string_view view;
                 if (!bytes_view(query, view)) {
                     throw py::error_already_set();
                 }
                 kimix::vector<turn_meta> hits;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     hits = idx.search_with_recency(view, top_k, weight);
                 }
                 return pack_hits(hits);
             },
             py::arg("query"), py::arg("top_k"), py::arg("weight") = 1.0)
        .def("get_by_id",
             [](const SqliteHistoryIndex &idx, uint32_t turn_id) -> py::object {
                 kimix::optional<turn_meta> found;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     found = idx.get_by_id(turn_id);
                 }
                 if (!found.has_value()) {
                     return py::none();
                 }
                 return to_bytes(found->text);
             },
             py::arg("turn_id"))
        .def("non_compacted_turn_ids",
             [](const SqliteHistoryIndex &idx) -> py::bytes {
                 kimix::vector<uint32_t> ids;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     ids = idx.non_compacted_turn_ids();
                 }
                 return kimix_soul_bridge::pack_u32(ids);
             })
        .def("bytes_owned",
             [](const SqliteHistoryIndex &idx) -> uint64_t {
                 // The durable backend's own bytes are the SQLite pages on
                 // disk; the handle itself owns no arena. Reported as 0 so
                 // mem_bench shows "no native mirror" instead of a guess.
                 (void)idx;
                 return 0;
             });

    // ------------------------------------------------------------------
    // W7: message rows (serialisation only; schema stays in Python)
    // ------------------------------------------------------------------
    m.def("context_rows",
          [](py::object structure, py::object history) -> py::bytes {
              if (!py::isinstance<py::bytes>(history)) {
                  throw py::type_error("history must be bytes");
              }
              kimix_soul_bridge::bridge b;
              if (!kimix_soul_bridge::parse_structure_any(structure, history, b)) {
                  throw py::value_error("invalid structure/history");
              }
              kimix::vector<kimix::runtime::soul::message_view> msgs;
              kimix::string packed;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  msgs = kimix_soul_bridge::assemble(b);
                  kimix::runtime::index::context_rows(msgs, packed);
              }
              if (packed.empty()) {
                  throw py::value_error("context_rows failed");
              }
              return to_bytes(packed);
          },
          "context_rows(structure, history) -> packed (json row, fts text) pairs",
          py::arg("structure"), py::arg("history"));

    m.def("parse_rows",
          [](py::bytes blob, kimix::runtime::soul::HistoryStore &store) -> int {
              kimix::string_view view;
              if (!bytes_view(blob, view)) {
                  throw py::error_already_set();
              }
              const uint32_t expected = view.size() >= 4
                                            ? read_le32(view.data())
                                            : 0;
              uint32_t added = 0;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  added = kimix::runtime::index::parse_rows(view, store);
              }
              if (added != expected) {
                  throw py::value_error(
                      "parse_rows failed: malformed rows (the history now holds "
                      "the rows decoded before the bad one)");
              }
              return static_cast<int>(added);
          },
          "parse_rows(blob, history_handle) -> messages appended",
          py::arg("blob"), py::arg("history_handle"));
}
