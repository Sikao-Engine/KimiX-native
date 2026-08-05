/*
 * py_search.cpp — Python bindings for the search kernels (runtime_py.search).
 *
 * BINDING-LAYER ONLY (same conventions as py_index.cpp): bytes in/bytes out,
 * GIL released around every kernel call, Python objects built only after the
 * release guard destructs.
 */

#include <pybind11/pybind11.h>

#include <runtime/runtime.h>
#include <runtime/common/gil.h>
#include <runtime/index/inverted_index.h>
#include <runtime/search/bm25.h>
#include <runtime/search/distance.h>
#include <runtime/search/fuzzy.h>
#include <runtime/search/hash_kernels.h>
#include <runtime/search/rerank.h>

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

// Parse a list of per-term postings: list[list[(doc_id, tf)]]. Builds
// owning vectors + spans. Pure-C++ work while the GIL is held (allocation is
// fine; no Python API calls).
bool parse_query_postings(py::list query_postings,
                          kimix::vector<kimix::vector<kimix::runtime::index::postings_entry>>& storage,
                          kimix::vector<kimix::span<const kimix::runtime::index::postings_entry>>& spans) {
    const Py_ssize_t n_terms = PyList_GET_SIZE(query_postings.ptr());
    storage.clear();
    storage.reserve(static_cast<size_t>(n_terms));
    spans.clear();
    spans.reserve(static_cast<size_t>(n_terms));
    for (Py_ssize_t t = 0; t < n_terms; ++t) {
        py::handle term_obj = PyList_GET_ITEM(query_postings.ptr(), t);
        if (!PyList_Check(term_obj.ptr())) {
            return false;
        }
        const Py_ssize_t n_pl = PyList_GET_SIZE(term_obj.ptr());
        kimix::vector<kimix::runtime::index::postings_entry> pl;
        pl.reserve(static_cast<size_t>(n_pl));
        for (Py_ssize_t i = 0; i < n_pl; ++i) {
            py::handle item = PyList_GET_ITEM(term_obj.ptr(), i);
            if (!PyTuple_Check(item.ptr()) || PyTuple_GET_SIZE(item.ptr()) != 2) {
                return false;
            }
            py::handle d = PyTuple_GET_ITEM(item.ptr(), 0);
            py::handle tf = PyTuple_GET_ITEM(item.ptr(), 1);
            if (!PyLong_Check(d.ptr()) || !PyLong_Check(tf.ptr())) {
                return false;
            }
            pl.push_back({static_cast<uint32_t>(PyLong_AsUnsignedLong(d.ptr())),
                          static_cast<uint32_t>(PyLong_AsUnsignedLong(tf.ptr()))});
        }
        storage.push_back(std::move(pl));
    }
    for (const auto& pl : storage) {
        spans.push_back(pl);
    }
    return true;
}

} // namespace

void py_register_search(py::module_& m) {
    m.doc() = "Search kernels: BM25, string distance/similarity, fuzzy "
              "expansion, SimHash/MinHash, MMR/xQuAD re-ranking";

    // ------------------------------------------------------------------
    // BM25
    // ------------------------------------------------------------------
    m.def("bm25_idf", [](uint32_t doc_count, uint32_t df, double k1, double b) {
              return kimix::runtime::search::bm25_idf(doc_count, df, k1, b);
          },
          "idf = log(1 + (N - df + 0.5) / (df + 0.5))",
          py::arg("doc_count"), py::arg("df"), py::arg("k1") = 1.2, py::arg("b") = 0.75);

    m.def("bm25_score",
          [](py::list query_postings, py::list idf, py::list doc_lengths,
             double avg_doc_len, uint32_t doc_count, double k1, double b) -> py::list {
              kimix::vector<kimix::vector<kimix::runtime::index::postings_entry>> storage;
              kimix::vector<kimix::span<const kimix::runtime::index::postings_entry>> spans;
              if (!parse_query_postings(query_postings, storage, spans)) {
                  throw py::type_error("query_postings must be list[list[(int, int)]]");
              }
              kimix::vector<double> idfs;
              idfs.reserve(static_cast<size_t>(PyList_GET_SIZE(idf.ptr())));
              for (Py_ssize_t i = 0; i < PyList_GET_SIZE(idf.ptr()); ++i) {
                  idfs.push_back(PyFloat_AsDouble(PyList_GET_ITEM(idf.ptr(), i)));
              }
              kimix::vector<uint32_t> lengths;
              lengths.reserve(static_cast<size_t>(PyList_GET_SIZE(doc_lengths.ptr())));
              for (Py_ssize_t i = 0; i < PyList_GET_SIZE(doc_lengths.ptr()); ++i) {
                  lengths.push_back(static_cast<uint32_t>(
                      PyLong_AsUnsignedLong(PyList_GET_ITEM(doc_lengths.ptr(), i))));
              }
              kimix::vector<double> scores;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::search::Bm25Scorer scorer(k1, b);
                  scorer.score(spans, idfs, lengths, avg_doc_len, doc_count, scores);
              }
              py::list out;
              for (double s : scores) {
                  out.append(s);
              }
              return out;
          },
          "One-shot BM25 accumulation (double, numpy-exact op order). "
          "doc_lengths is indexed by doc_id and sized doc_count.",
          py::arg("query_postings"), py::arg("idf"), py::arg("doc_lengths"),
          py::arg("avg_doc_len"), py::arg("doc_count"), py::arg("k1") = 1.2,
          py::arg("b") = 0.75);

    m.def("bm25_topk",
          [](py::list scores, uint32_t k) -> py::list {
              kimix::vector<double> vals;
              vals.reserve(static_cast<size_t>(PyList_GET_SIZE(scores.ptr())));
              for (Py_ssize_t i = 0; i < PyList_GET_SIZE(scores.ptr()); ++i) {
                  vals.push_back(PyFloat_AsDouble(PyList_GET_ITEM(scores.ptr(), i)));
              }
              kimix::vector<uint32_t> docs;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  kimix::runtime::search::top_k(vals, k, docs);
              }
              py::list out;
              for (uint32_t d : docs) {
                  out.append(d);
              }
              return out;
          },
          "Top-k doc ids by (score desc, doc asc), nonzero scores only.",
          py::arg("scores"), py::arg("k"));

    // ------------------------------------------------------------------
    // Distance / similarity
    // ------------------------------------------------------------------
    m.def("damerau_levenshtein",
          [](py::bytes a, py::bytes b, int32_t max_dist) -> int32_t {
              kimix::string_view va, vb;
              if (!bytes_view(a, va) || !bytes_view(b, vb)) {
                  throw py::error_already_set();
              }
              int32_t result = 0;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  result = kimix::runtime::search::damerau_levenshtein(va, vb, max_dist);
              }
              return result;
          },
          "OSAbL Damerau-Levenshtein (exact reference port; max_dist >= 0 "
          "returns max_dist+1 when the distance exceeds it).",
          py::arg("a"), py::arg("b"), py::arg("max_dist") = -1);

    m.def("freq_lower_bound",
          [](py::bytes pattern, py::bytes term) -> int32_t {
              kimix::string_view vp, vt;
              if (!bytes_view(pattern, vp) || !bytes_view(term, vt)) {
                  throw py::error_already_set();
              }
              int32_t result = 0;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  result = kimix::runtime::search::freq_lower_bound(vp, vt);
              }
              return result;
          },
          "Char-multiset edit-distance lower bound (pattern-first, asymmetric).",
          py::arg("pattern"), py::arg("term"));

    auto def_sim = [&m](const char* name, const char* doc, auto fn) {
        m.def(name, fn, doc);
    };
    def_sim("jaro_similarity",
            "Jaro similarity (0.0-1.0).",
            [](py::bytes a, py::bytes b) -> double {
                kimix::string_view va, vb;
                if (!bytes_view(a, va) || !bytes_view(b, vb)) {
                    throw py::error_already_set();
                }
                double result = 0.0;
                {
                    kimix::runtime::common::gil_scoped_release release;
                    result = kimix::runtime::search::jaro_similarity(va, vb);
                }
                return result;
            });
    m.def("jaro_winkler",
          [](py::bytes a, py::bytes b, double prefix_scale) -> double {
              kimix::string_view va, vb;
              if (!bytes_view(a, va) || !bytes_view(b, vb)) {
                  throw py::error_already_set();
              }
              double result = 0.0;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  result = kimix::runtime::search::jaro_winkler(va, vb, prefix_scale);
              }
              return result;
          },
          "Jaro-Winkler (max_prefix=4, prefix_scale=p).",
          py::arg("a"), py::arg("b"), py::arg("prefix_scale") = 0.1);
    m.def("sorensen_dice",
          [](py::bytes a, py::bytes b) -> double {
              kimix::string_view va, vb;
              if (!bytes_view(a, va) || !bytes_view(b, vb)) {
                  throw py::error_already_set();
              }
              double result = 0.0;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  result = kimix::runtime::search::sorensen_dice(va, vb);
              }
              return result;
          },
          "Sorensen-Dice over bigram sets.");
    m.def("ngram_overlap",
          [](py::bytes a, py::bytes b, uint32_t n) -> double {
              kimix::string_view va, vb;
              if (!bytes_view(a, va) || !bytes_view(b, vb)) {
                  throw py::error_already_set();
              }
              double result = 0.0;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  result = kimix::runtime::search::ngram_overlap(va, vb, n);
              }
              return result;
          },
          "N-gram overlap (intersection / union).",
          py::arg("a"), py::arg("b"), py::arg("n") = 2);

    // ------------------------------------------------------------------
    // Symmetric-delete fuzzy expansion
    // ------------------------------------------------------------------
    py::class_<kimix::runtime::search::SymmetricDeleteIndex>(m, "SymmetricDeleteIndex")
        .def(py::init<>())
        .def("add_term",
             [](kimix::runtime::search::SymmetricDeleteIndex& sd, py::bytes term,
                uint32_t max_edits) {
                 kimix::string_view view;
                 if (!bytes_view(term, view)) {
                     throw py::error_already_set();
                 }
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     sd.add_term(view, max_edits);
                 }
             },
             "Index a term's delete variants (max_edits levels 1..max_edits).",
             py::arg("term"), py::arg("max_edits") = 2)
        .def("expand",
             [](const kimix::runtime::search::SymmetricDeleteIndex& sd, py::bytes query,
                uint32_t max_edits, uint32_t max_expansions) -> py::list {
                 kimix::string_view view;
                 if (!bytes_view(query, view)) {
                     throw py::error_already_set();
                 }
                 kimix::vector<kimix::runtime::search::fuzzy_candidate> out;
                 {
                     kimix::runtime::common::gil_scoped_release release;
                     sd.expand(view, max_edits, out, max_expansions);
                 }
                 py::list result;
                  for (const auto& fc : out) {
                     result.append(py::make_tuple(py::bytes(fc.term.data(), fc.term.size()),
                                                  fc.score));
                 }
                 return result;
             },
             "Fuzzy candidates (term, score) within max_edits edits, sorted "
             "(score desc, term asc).",
             py::arg("query"), py::arg("max_edits"),
             py::arg("max_expansions") = 50)
        .def("term_count",
             [](const kimix::runtime::search::SymmetricDeleteIndex& sd) {
                 return static_cast<uint64_t>(sd.term_count());
             })
        .def("has_term",
             [](const kimix::runtime::search::SymmetricDeleteIndex& sd, py::bytes term) -> bool {
                 kimix::string_view view;
                 if (!bytes_view(term, view)) {
                     throw py::error_already_set();
                 }
                 return sd.has_term(view);
             },
             py::arg("term"))
        .def("reset", [](kimix::runtime::search::SymmetricDeleteIndex& sd) {
                 kimix::runtime::common::gil_scoped_release release;
                 sd.reset();
             });

    // ------------------------------------------------------------------
    // SimHash / MinHash
    // ------------------------------------------------------------------
    m.def("simhash",
          [](py::list tokens, uint64_t seed) -> uint64_t {
              kimix::vector<kimix::string_view> views;
              views.reserve(static_cast<size_t>(PyList_GET_SIZE(tokens.ptr())));
              for (Py_ssize_t i = 0; i < PyList_GET_SIZE(tokens.ptr()); ++i) {
                  py::handle item = PyList_GET_ITEM(tokens.ptr(), i);
                  kimix::string_view v;
                  if (!PyBytes_Check(item.ptr())) {
                      throw py::type_error("tokens must be a list of bytes");
                  }
                  char* buf = nullptr;
                  Py_ssize_t len = 0;
                  if (PyBytes_AsStringAndSize(item.ptr(), &buf, &len) < 0) {
                      throw py::error_already_set();
                  }
                  views.emplace_back(buf, static_cast<size_t>(len));
              }
              uint64_t result = 0;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  result = kimix::runtime::search::simhash(views, seed);
              }
              return result;
          },
          "SimHash over (deduped) tokens — XXH3-64 contract.",
          py::arg("tokens"), py::arg("seed") = kimix::hash64_default_seed);
    m.def("minhash",
          [](py::list shingles, uint32_t k, uint64_t seed) -> py::list {
              kimix::vector<kimix::string_view> views;
              views.reserve(static_cast<size_t>(PyList_GET_SIZE(shingles.ptr())));
              for (Py_ssize_t i = 0; i < PyList_GET_SIZE(shingles.ptr()); ++i) {
                  py::handle item = PyList_GET_ITEM(shingles.ptr(), i);
                  if (!PyBytes_Check(item.ptr())) {
                      throw py::type_error("shingles must be a list of bytes");
                  }
                  char* buf = nullptr;
                  Py_ssize_t len = 0;
                  if (PyBytes_AsStringAndSize(item.ptr(), &buf, &len) < 0) {
                      throw py::error_already_set();
                  }
                  views.emplace_back(buf, static_cast<size_t>(len));
              }
              kimix::vector<uint64_t> sig;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  sig = kimix::runtime::search::minhash(views, k, seed);
              }
              py::list out;
              for (uint64_t v : sig) {
                  out.append(v);
              }
              return out;
          },
          "MinHash signature (deterministic XXH3-64 contract).",
          py::arg("shingles"), py::arg("k"), py::arg("seed") = kimix::hash64_default_seed);

    // ------------------------------------------------------------------
    // MMR / xQuAD re-ranking
    // ------------------------------------------------------------------
    m.def("mmr_rerank",
          [](py::list scores, py::list sim_matrix, double lambda_param,
             uint32_t k) -> py::list {
              kimix::vector<double> rel;
              rel.reserve(static_cast<size_t>(PyList_GET_SIZE(scores.ptr())));
              for (Py_ssize_t i = 0; i < PyList_GET_SIZE(scores.ptr()); ++i) {
                  rel.push_back(PyFloat_AsDouble(PyList_GET_ITEM(scores.ptr(), i)));
              }
              // Copy the matrix into a flat double buffer.
              const size_t n = rel.size();
              kimix::vector<double> matrix(n * n, 0.0);
              const Py_ssize_t rows = PyList_GET_SIZE(sim_matrix.ptr());
              for (Py_ssize_t r = 0; r < rows && static_cast<size_t>(r) < n; ++r) {
                  py::handle row = PyList_GET_ITEM(sim_matrix.ptr(), r);
                  if (!PyList_Check(row.ptr())) {
                      continue;
                  }
                  const Py_ssize_t cols = PyList_GET_SIZE(row.ptr());
                  for (Py_ssize_t c = 0; c < cols && static_cast<size_t>(c) < n; ++c) {
                      matrix[static_cast<size_t>(r) * n + static_cast<size_t>(c)] =
                          PyFloat_AsDouble(PyList_GET_ITEM(row.ptr(), c));
                  }
              }
              const size_t nn = n;
              kimix::runtime::search::similarity_fn sim =
                  [&matrix, nn](uint32_t a, uint32_t b) { return matrix[a * nn + b]; };
              // k == 0 -> all results (reference top_k=None -> len(results)).
              const uint32_t use_k = (k == 0) ? static_cast<uint32_t>(nn) : k;
              kimix::vector<uint32_t> sel;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  sel = kimix::runtime::search::mmr_rerank(rel, sim, lambda_param, use_k);
              }
              py::list out;
              for (uint32_t d : sel) {
                  out.append(d);
              }
              return out;
          },
          "Greedy MMR over (scores, sim_matrix); returns selected positions.",
          py::arg("scores"), py::arg("sim_matrix"), py::arg("lambda_param") = 0.5,
          py::arg("k") = 0);
    m.def("xquad_rerank",
          [](py::list scores, uint32_t k) -> py::list {
              kimix::vector<double> rel;
              rel.reserve(static_cast<size_t>(PyList_GET_SIZE(scores.ptr())));
              for (Py_ssize_t i = 0; i < PyList_GET_SIZE(scores.ptr()); ++i) {
                  rel.push_back(PyFloat_AsDouble(PyList_GET_ITEM(scores.ptr(), i)));
              }
              const uint32_t use_k = (k == 0) ? static_cast<uint32_t>(rel.size()) : k;
              kimix::vector<uint32_t> sel;
              {
                  kimix::runtime::common::gil_scoped_release release;
                  sel = kimix::runtime::search::xquad_rerank(rel, use_k);
              }
              py::list out;
              for (uint32_t d : sel) {
                  out.append(d);
              }
              return out;
          },
          "Score-only xQuAD (stable relevance-descending selection).",
          py::arg("scores"), py::arg("k") = 0);
}
