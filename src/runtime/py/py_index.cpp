/*
 * py_index.cpp — Python bindings for the index kernels (runtime_py.index).
 *
 * BINDING-LAYER ONLY: links against runtime.dll (pure C++ kernels) and
 * pybind11. Every kernel call releases the GIL via
 * kimix::runtime::common::gil_scoped_release. Contract: bytes in / bytes
 * out (the Python shim handles str<->bytes with surrogatepass and the NFKC
 * hook — see python/kimix_native/index.py).
 *
 * CRITICAL: no Python object may be created while the GIL is released — the
 * kernel runs inside `{ gil_scoped_release release; ... }` and Python
 * objects are built only after the scope closes (GIL reacquired).
 */

#include <pybind11/pybind11.h>

#include <runtime/runtime.h>
#include <runtime/common/gil.h>
#include <runtime/index/ngram_tokenizer.h>
#include <runtime/index/inverted_index.h>

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

// Extract one token buffer view from a py::bytes item.
bool item_view(py::handle item, kimix::string_view& view) {
    if (!PyBytes_Check(item.ptr())) {
        return false;
    }
    char* buf = nullptr;
    Py_ssize_t len = 0;
    if (PyBytes_AsStringAndSize(item.ptr(), &buf, &len) < 0) {
        return false;
    }
    view = kimix::string_view(buf, static_cast<size_t>(len));
    return true;
}

// Extract all token views from a py::list of py::bytes. The py objects stay
// alive for the whole call (they are held by the argument list), so the
// views are valid while the kernel runs.
bool list_of_views(py::list items, kimix::vector<kimix::string_view>& views) {
    const Py_ssize_t n = PyList_GET_SIZE(items.ptr());
    views.reserve(static_cast<size_t>(n));
    for (Py_ssize_t i = 0; i < n; ++i) {
        py::handle item = PyList_GET_ITEM(items.ptr(), i);
        kimix::string_view v;
        if (!item_view(item, v)) {
            return false;
        }
        views.push_back(v);
    }
    return true;
}

py::list postings_to_list(kimix::span<const kimix::runtime::index::postings_entry> pl) {
    py::list out;
    for (const auto& e : pl) {
        out.append(py::make_tuple(e.doc_id, e.tf));
    }
    return out;
}

} // namespace

void py_register_index(py::module_& m) {
    m.doc() = "Index kernels: ngram tokenizer + incremental inverted index";

    // ------------------------------------------------------------------
    // NgramTokenizer
    // ------------------------------------------------------------------
    py::class_<kimix::runtime::index::NgramTokenizer>(m, "NgramTokenizer")
        .def(py::init<uint32_t>(), py::arg("default_n") = 2)
        .def("normalize", [](const kimix::runtime::index::NgramTokenizer& tok,
                             py::bytes data) -> py::bytes {
                 kimix::string_view view;
                 if (!bytes_view(data, view)) {
                     throw py::error_already_set();
                 }
                 kimix::string result;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     result = tok.normalize(view);
                 }
                 return to_bytes(result);
             },
             "ASCII-lowercase (kernel); the shim composes .lower() + NFKC on "
             "non-ASCII text to match the reference exactly.",
             py::arg("data"))
        .def("detect_n", [](const kimix::runtime::index::NgramTokenizer& tok,
                            py::bytes data) -> uint32_t {
                 kimix::string_view view;
                 if (!bytes_view(data, view)) {
                     throw py::error_already_set();
                 }
                 uint32_t result = 0;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     result = tok.detect_n(view);
                 }
                 return result;
             },
             "Auto-detect n-gram size (2 for CJK-dense, else max(n,3)).",
             py::arg("data"))
        .def("tokenize", [](const kimix::runtime::index::NgramTokenizer& tok,
                            py::bytes data, uint32_t n) -> py::list {
                 kimix::string_view view;
                 if (!bytes_view(data, view)) {
                     throw py::error_already_set();
                 }
                 kimix::vector<kimix::string_view> grams;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     tok.tokenize(view, n, grams);
                 }
                 py::list out;
                  for (kimix::string_view g : grams) {
                     out.append(py::bytes(g.data(), g.size()));
                 }
                 return out;
             },
             "Overlapping n-grams over code points (bytes in/out).",
             py::arg("data"), py::arg("n"))
        .def_property_readonly("default_n",
                               &kimix::runtime::index::NgramTokenizer::default_n);

    // ------------------------------------------------------------------
    // InvertedIndex
    // ------------------------------------------------------------------
    py::class_<kimix::runtime::index::InvertedIndex>(m, "InvertedIndex")
        .def(py::init<>())
        .def("add_document",
             [](kimix::runtime::index::InvertedIndex& idx, uint32_t doc_id,
                py::list tokens) {
                 kimix::vector<kimix::string_view> views;
                 if (!list_of_views(tokens, views)) {
                     throw py::type_error("tokens must be a list of bytes");
                 }
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     idx.add_document(doc_id, views);
                 }
             },
             "O(unique tokens), never touches finalized segments.",
             py::arg("doc_id"), py::arg("tokens"))
        .def("finalize", [](kimix::runtime::index::InvertedIndex& idx) {
                 kimix::runtime::common::gil_scoped_release release;
                 idx.finalize();
             },
             "Flush the delta buffer into a new immutable segment.")
        .def("finalized",
             [](const kimix::runtime::index::InvertedIndex& idx) { return idx.finalized(); })
        .def("get_postings",
             [](kimix::runtime::index::InvertedIndex& idx, py::bytes term) -> py::object {
                 kimix::string_view view;
                 if (!bytes_view(term, view)) {
                     throw py::error_already_set();
                 }
                 kimix::span<const kimix::runtime::index::postings_entry> pl;
                 bool has = false;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     pl = idx.get_postings(view);
                     has = idx.has_term(view);
                 }
                 if (pl.empty() && !has) {
                     return py::none();
                 }
                 return postings_to_list(pl);
             },
             "Merged (doc_id, tf) postings for a term, or None.",
             py::arg("term"))
        .def("has_term",
             [](const kimix::runtime::index::InvertedIndex& idx, py::bytes term) -> bool {
                 kimix::string_view view;
                 if (!bytes_view(term, view)) {
                     throw py::error_already_set();
                 }
                 return idx.has_term(view);
             },
             py::arg("term"))
        .def("doc_count",
             [](const kimix::runtime::index::InvertedIndex& idx) { return idx.doc_count(); })
        .def("max_doc_id",
             [](const kimix::runtime::index::InvertedIndex& idx) { return idx.max_doc_id(); })
        .def("doc_length",
             [](const kimix::runtime::index::InvertedIndex& idx, uint32_t doc_id) {
                 return idx.doc_length(doc_id);
             },
             py::arg("doc_id"))
        .def("sum_doc_lengths",
             [](const kimix::runtime::index::InvertedIndex& idx) {
                 return static_cast<uint64_t>(idx.sum_doc_lengths());
             })
        .def("avg_doc_len",
             [](const kimix::runtime::index::InvertedIndex& idx) { return idx.avg_doc_len(); })
        .def("total_postings",
             [](const kimix::runtime::index::InvertedIndex& idx) {
                 return idx.total_postings();
             })
        .def("segment_count",
             [](const kimix::runtime::index::InvertedIndex& idx) {
                 return idx.segment_count();
             })
        .def("compact", [](kimix::runtime::index::InvertedIndex& idx) {
                 kimix::runtime::common::gil_scoped_release release;
                 idx.compact();
             },
             "Merge all segments into one.")
        .def("save",
             [](kimix::runtime::index::InvertedIndex& idx) -> py::bytes {
                 kimix::string blob;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     idx.save_to(blob);
                 }
                 return to_bytes(blob);
             },
             "Serialize to a KNIDX1 blob (finalizes first if needed).")
        .def("load",
             [](kimix::runtime::index::InvertedIndex& idx, py::bytes blob) -> bool {
                 kimix::string_view view;
                 if (!bytes_view(blob, view)) {
                     throw py::error_already_set();
                 }
                 bool ok = false;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     ok = idx.load_from(view);
                 }
                 return ok;
             },
             "Deserialize a KNIDX1 blob; False on malformed input.",
             py::arg("blob"))
        .def("reset", [](kimix::runtime::index::InvertedIndex& idx) {
                 kimix::runtime::common::gil_scoped_release release;
                 idx.reset();
             });
}
