"""Parity tests for kimix_native.search (plan 005).

Compares the native kernels against:
- the pure-Python ``_compat`` mirrors (written from the same reference
  source) on random strings and seeded corpora;
- a numpy implementation of the reference BM25 ``_accumulate`` (exact
  float64 operation order — scores must be bit-equal);
- golden vectors harvested from retrieval.py semantics.

Also covers the KIMIX_NATIVE_SEARCH=0 fallback toggle.
"""

import math
import os
import random
import struct
import subprocess
import sys

import numpy as np
import pytest

import kimix_native
from kimix_native import index, search

# ---------------------------------------------------------------------------
# Golden vectors (computed from the reference source)
# ---------------------------------------------------------------------------


def test_distance_golden():
    assert search.damerau_levenshtein("cat", "car") == 1
    assert search.damerau_levenshtein("ca", "abc") == 3
    assert search.damerau_levenshtein("ab", "ba") == 1
    assert search.damerau_levenshtein("abc", "x") == 1  # reference fast-path quirk
    assert search.damerau_levenshtein("kitten", "sitting") == 3
    assert search.damerau_levenshtein("abcd", "abdc") == 1
    assert search.damerau_levenshtein("dixon", "dicksonx") == 4
    assert search.damerau_levenshtein("", "abc") == 3
    assert search.damerau_levenshtein("abc", "abc") == 0
    assert search.damerau_levenshtein("abc", "x", max_dist=0) == 1  # > 0 -> 1
    assert search.damerau_levenshtein("cat", "car", max_dist=0) == 1
    assert search.damerau_levenshtein("cat", "car", max_dist=1) == 1
    assert search.damerau_levenshtein("cat", "car", max_dist=2) == 1
    assert search.jaro_similarity("MARTHA", "MARHTA") == pytest.approx(0.9444444444444445)
    assert search.jaro_similarity("DIXON", "DICKSONX") == pytest.approx(0.7666666666666666)
    assert search.jaro_similarity("", "") == 1.0
    assert search.jaro_winkler("MARTHA", "MARHTA") == pytest.approx(0.9611111111111111)
    assert search.jaro_winkler("DIXON", "DICKSONX") == pytest.approx(0.8133333333333332)
    assert search.sorensen_dice("night", "nacht") == pytest.approx(0.25)
    assert search.sorensen_dice("a", "a") == 0.0
    assert search.sorensen_dice("", "") == 1.0
    assert search.ngram_overlap("night", "nacht", 2) == pytest.approx(0.14285714285714285)
    assert search.ngram_overlap("ab", "abc", 2) == pytest.approx(0.5)


def test_distance_native_vs_compat():
    rng = random.Random(123)
    pairs = []
    for _ in range(400):
        n1, n2 = rng.randint(0, 10), rng.randint(0, 10)
        s1 = "".join(rng.choice("abcdefgh ") for _ in range(n1)).strip()
        s2 = "".join(rng.choice("abcdefgh ") for _ in range(n2)).strip()
        pairs.append((s1, s2))
    for a, b in pairs:
        assert search.damerau_levenshtein(a, b) == search._compat_damerau_levenshtein(a, b), (a, b)
        assert search.freq_lower_bound(a, b) == search._compat_freq_lower_bound(a, b), (a, b)
        assert search.jaro_similarity(a, b) == pytest.approx(
            search._compat_jaro_similarity(a, b), abs=0.0), (a, b)
        assert search.jaro_winkler(a, b) == pytest.approx(
            search._compat_jaro_winkler(a, b), abs=0.0), (a, b)
        assert search.sorensen_dice(a, b) == pytest.approx(
            search._compat_sorensen_dice(a, b), abs=0.0), (a, b)
        for n in (1, 2, 3):
            assert search.ngram_overlap(a, b, n) == pytest.approx(
                search._compat_ngram_overlap(a, b, n), abs=0.0), (a, b, n)
        for md in (-1, 0, 1, 2, 3):
            assert search.damerau_levenshtein(a, b, md) == search._compat_damerau_levenshtein(
                a, b, md), (a, b, md)


def test_distance_cjk():
    # Code-point semantics: "你" vs "他" is one substitution.
    assert search.damerau_levenshtein("你", "他") == 1
    assert search.damerau_levenshtein("a你", "a他") == 1
    assert search.damerau_levenshtein("你好世界", "你好世界") == 0
    assert search.jaro_similarity("你好", "你好") == 1.0


# ---------------------------------------------------------------------------
# BM25 parity vs the numpy reference accumulation
# ---------------------------------------------------------------------------


def _numpy_bm25(query_postings, idf, doc_lengths, avg_doc_len, N, k1=1.2, b=0.75):
    """Exact port of retrieval.py::BM25Scorer._token_scores + _accumulate."""
    scores_arr = np.zeros(N, dtype=np.float64)
    if N == 0 or avg_doc_len == 0:
        return scores_arr
    denom_base = k1 * ((1.0 - b) + (b / avg_doc_len) * np.asarray(doc_lengths, dtype=np.float64))
    for postings, idf_t in zip(query_postings, idf):
        docs = np.asarray([d for d, _ in postings], dtype=np.int32)
        tfs = np.asarray([tf for _, tf in postings], dtype=np.float64)
        tfs_f = tfs.copy()
        denom = tfs_f + denom_base[docs]
        tfs_f *= idf_t * (k1 + 1.0)
        np.divide(tfs_f, denom, out=tfs_f)
        np.add.at(scores_arr, docs, tfs_f)
    return scores_arr


def _build_index(docs):
    idx = index.InvertedIndex()
    for i, doc in enumerate(docs):
        idx.add_document(i, doc)
    idx.finalize()
    return idx


def test_bm25_idf_golden():
    assert search.bm25_idf(1000, 10) == pytest.approx(4.557379522151743)
    assert search.bm25_idf(500, 50) == pytest.approx(2.2946327648035507)
    assert search.bm25_idf(100, 0) == pytest.approx(5.308267697401205)
    assert search.bm25_idf(1000, 10, k1=2.0, b=0.5) == pytest.approx(
        math.log(1 + (1000 - 10 + 0.5) / (10 + 0.5)))


def test_bm25_score_parity_numpy():
    rng = random.Random(555)
    vocab = 60
    docs = [[f"t{rng.randrange(vocab)}" for _ in range(rng.randint(1, 25))] for _ in range(40)]
    idx = _build_index(docs)
    N = idx.max_doc_id() + 1
    doc_lengths = [idx.doc_length(d) for d in range(N)]
    avg_doc_len = idx.avg_doc_len()
    k1, b = 1.2, 0.75

    for _ in range(25):
        query = [f"t{rng.randrange(vocab)}" for _ in range(rng.randint(1, 6))]
        # Dedupe and expand like Searcher.search does.
        query_postings = []
        idf = []
        for token in dict.fromkeys(query):
            pl = idx.get_postings(token)
            if pl is None:
                continue
            query_postings.append(pl)
            idf.append(search.bm25_idf(N, len(pl), k1, b))
        if not query_postings:
            continue
        native = search.bm25_score(query_postings, idf, doc_lengths, avg_doc_len, N, k1, b)
        reference = _numpy_bm25(query_postings, idf, doc_lengths, avg_doc_len, N, k1, b)
        assert len(native) == N
        for d in range(N):
            assert native[d] == pytest.approx(float(reference[d]), abs=0.0), (
                d, native[d], float(reference[d]))
        # top_k parity: (score desc, doc asc), nonzero only.
        expect_top = search._compat_top_k(native, 5)
        assert search.bm25_topk(native, 5) == expect_top


def test_bm25_sparse_doc_ids():
    # Python's N = max_doc_id + 1 and doc_lengths_arr indexed by doc_id.
    idx = index.InvertedIndex()
    idx.add_document(0, ["a"])
    idx.add_document(100, ["a", "b"])
    idx.finalize()
    N = idx.max_doc_id() + 1  # 101
    doc_lengths = [idx.doc_length(d) for d in range(N)]
    pl = idx.get_postings("a")
    native = search.bm25_score([pl], [search.bm25_idf(N, len(pl))],
                               doc_lengths, idx.avg_doc_len(), N)
    reference = _numpy_bm25([pl], [search.bm25_idf(N, len(pl))],
                            doc_lengths, idx.avg_doc_len(), N)
    for d in range(N):
        assert native[d] == pytest.approx(float(reference[d]), abs=0.0)


# ---------------------------------------------------------------------------
# Fuzzy expansion parity
# ---------------------------------------------------------------------------


def test_fuzzy_parity():
    rng = random.Random(31)
    vocab = [
        "hello", "help", "hell", "held", "world", "word", "worn", "work",
        "cat", "cut", "cot", "bat", "cart", "scar", "card", "car",
        "alpha", "alphabet", "alpine", "apple", "apply",
    ]
    sd_native = search.SymmetricDeleteIndex()
    sd_compat = search._CompatSymmetricDeleteIndex()
    for t in vocab:
        sd_native.add_term(t)
        sd_compat.add_term(t)
    assert sd_native.term_count() == sd_compat.term_count() == len(vocab)
    for q in vocab + ["helo", "wrold", "catt", "car", "aplha", "zzzz"]:
        for me in (1, 2):
            got = sd_native.expand(q, me, max_expansions=200)
            want = sd_compat.expand(q, me, max_expansions=200)
            assert [(t, round(s, 12)) for t, s in got] == [
                (t, round(s, 12)) for t, s in want
            ], (q, me)


def test_fuzzy_deterministic_order():
    sd = search.SymmetricDeleteIndex()
    for t in ["cat", "cut", "cot", "cart", "scar"]:
        sd.add_term(t)
    out = sd.expand("cat", 1)
    assert out[0][0] == "cat"  # exact match first (score 1.0)
    scores = [s for _, s in out]
    assert scores == sorted(scores, reverse=True)


# ---------------------------------------------------------------------------
# SimHash / MinHash parity (XXH3-64 contract)
# ---------------------------------------------------------------------------


def test_simhash_minhash_parity():
    rng = random.Random(77)
    token_pool = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta",
                  "eta", "theta", "iota", "kappa"]
    for _ in range(40):
        tokens = [rng.choice(token_pool) for _ in range(rng.randint(0, 12))]
        seed = rng.randint(0, 2**63)
        assert search.simhash(tokens, seed) == search._compat_simhash(tokens, seed)
    # minhash: seeded shingle sets.
    shingle_pool = ["ab", "cd", "ef", "gh", "ij", "kl", "mn", "op"]
    for _ in range(20):
        shingles = [rng.choice(shingle_pool) for _ in range(rng.randint(0, 8))]
        k = rng.randint(1, 16)
        seed = rng.randint(0, 2**63)
        assert search.minhash(shingles, k, seed) == search._compat_minhash(shingles, k, seed)
    assert search.minhash([], 3, 1) == [0, 0, 0]
    assert search.simhash([], 1) == 0


def test_simhash_known_contract():
    # Golden under the documented XXH3-64 contract (default seed 2^61-1).
    assert search.simhash(["hello"]) == 12036767014363270904
    assert search.simhash(["a"]) == 0x8D314BCB92C80589
    assert search.minhash(["ab", "cd"], 2, 42) == [1457630054, 1166070587]


# ---------------------------------------------------------------------------
# MMR / xQuAD parity
# ---------------------------------------------------------------------------


def test_mmr_xquad_parity():
    rng = random.Random(9)
    for _ in range(30):
        n = rng.randint(1, 8)
        scores = [rng.random() for _ in range(n)]
        sim = [[rng.random() if i != j else 1.0 for j in range(n)] for i in range(n)]
        for lam in (0.0, 0.5, 1.0):
            assert search.mmr_rerank(scores, sim, lam, 0) == search._compat_mmr_rerank(
                scores, sim, lam, 0), (scores, lam)
            assert search.mmr_rerank(scores, sim, lam, 3) == search._compat_mmr_rerank(
                scores, sim, lam, 3), (scores, lam)
        assert search.xquad_rerank(scores, 0) == search._compat_xquad_rerank(scores, 0)


def test_mmr_golden():
    scores = [0.9, 0.8, 0.7, 0.6, 0.5]
    sim = [
        [1.0, 0.9, 0.1, 0.1, 0.0],
        [0.9, 1.0, 0.1, 0.0, 0.1],
        [0.1, 0.1, 1.0, 0.2, 0.0],
        [0.1, 0.0, 0.2, 1.0, 0.1],
        [0.0, 0.1, 0.0, 0.1, 1.0],
    ]
    assert search.mmr_rerank(scores, sim, 0.5) == [0, 2, 4, 3, 1]
    assert search.mmr_rerank(scores, sim, 1.0) == [0, 1, 2, 3, 4]
    assert search.mmr_rerank(scores, sim, 0.0) == [0, 4, 2, 3, 1]
    assert search.mmr_rerank(scores, sim, 0.5, 2) == [0, 2]
    assert search.xquad_rerank([3.0, 1.0, 2.0]) == [0, 2, 1]


# ---------------------------------------------------------------------------
# Toggle
# ---------------------------------------------------------------------------


def test_toggle_fallback(monkeypatch):
    monkeypatch.setenv("KIMIX_NATIVE_SEARCH", "0")
    assert kimix_native.use_native("SEARCH") is False
    assert search.damerau_levenshtein("cat", "car") == 1
    assert search.jaro_winkler("MARTHA", "MARHTA") == pytest.approx(0.9611111111111111)
    assert search.simhash(["hello"]) == 12036767014363270904
    sd = search.SymmetricDeleteIndex()
    sd.add_term("cat")
    assert [t for t, _ in sd.expand("cat", 1)] == ["cat"]


def test_toggle_off_globally():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    env = dict(os.environ)
    env["KIMIX_NATIVE"] = "0"
    code = (
        "import sys; sys.path.insert(0, {py!r}); "
        "from kimix_native import search; "
        "assert search.damerau_levenshtein('kitten', 'sitting') == 3; "
        "assert search.jaro_winkler('MARTHA', 'MARHTA') == "
        "0.9611111111111111; "
        "print('fallback-ok')"
    ).format(py=os.path.join(root, "python"))
    proc = subprocess.run(
        [sys.executable, "-c", code], cwd=root, env=env,
        capture_output=True, text=True, timeout=60,
    )
    assert proc.returncode == 0, proc.stderr
    assert "fallback-ok" in proc.stdout
