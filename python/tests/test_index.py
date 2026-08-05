"""Parity tests for kimix_native.index (plans 004).

Compares the native kernels against the pure-Python ``_compat`` mirrors
(reference retrieval.py algorithms + the plan 004 incremental contract) on:
- tokenizer golden vectors (normalize / detect_n / tokenize)
- >= 300 random mixed ASCII/CJK strings
- seeded corpora: index add/finalize/get_postings parity, add-after-finalize
- KNIDX1 blob cross-loading (native save -> _compat load and vice versa)
- the KIMIX_NATIVE_INDEX=0 fallback toggle
"""

import os
import random
import subprocess
import sys

import pytest

import kimix_native
from kimix_native import index

# ---------------------------------------------------------------------------
# Tokenizer golden vectors (retrieval.py semantics)
# ---------------------------------------------------------------------------

_CJK_SAMPLES = [
    "你", "好", "世", "界", "中", "文", "安", "녕", "하", "せ", "カ", "ナ",
    "Ａ", "Ｂ", "\U00020000", "\U0002ebef", "😀", "é", "ü",
]
_ASCII_SAMPLES = list(
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 \t.,;:!?-_"
)


def _random_string(rng, max_len=40):
    n = rng.randint(0, max_len)
    kind = rng.random()
    if kind < 0.35:
        pool = _ASCII_SAMPLES
    elif kind < 0.6:
        pool = _CJK_SAMPLES
    else:
        pool = _CJK_SAMPLES if rng.random() < 0.5 else _ASCII_SAMPLES
    return "".join(rng.choice(pool) for _ in range(n))


def test_tokenizer_golden():
    tok = index.NgramTokenizer()
    compat = index._CompatNgramTokenizer()
    assert tok.normalize("MiXeD Case 123") == "mixed case 123"
    assert tok.normalize("HÉLLO") == "héllo"  # NFKC+lower composed in the shim
    assert tok.normalize("ＡＢＣ") == "abc"  # fullwidth -> NFKC -> ascii
    assert tok.detect_n("") == 2
    assert tok.detect_n("hello world") == 3
    assert tok.detect_n("你好世界") == 2
    assert tok.tokenize("Hello World") == ["hel", "ell", "llo", "lo ", "o w",
                                           " wo", "wor", "orl", "rld"]
    assert tok.tokenize("你好世界") == ["你好", "好世", "世界"]
    assert tok.tokenize("ab") == ["ab"]  # len < n -> single token
    assert tok.tokenize("") == []
    assert tok.tokenize("  ") == []
    assert index.NgramTokenizer(4).detect_n("hello world") == 4
    assert index.NgramTokenizer(4).tokenize("hello") == ["hell", "ello"]


def test_tokenizer_native_vs_compat():
    rng = random.Random(2024)
    tok = index.NgramTokenizer()
    compat = index._CompatNgramTokenizer()
    for _ in range(300):
        s = _random_string(rng)
        assert tok.normalize(s) == compat.normalize(s), repr(s)
        assert tok.detect_n(s) == compat.detect_n(s), repr(s)
        for n in (None, 1, 2, 3, 5):
            assert tok.tokenize(s, n) == compat.tokenize(s, n), repr((s, n))


# ---------------------------------------------------------------------------
# InvertedIndex parity
# ---------------------------------------------------------------------------


def _random_docs(rng, count, vocab, max_tokens=30):
    docs = []
    for _ in range(count):
        n = rng.randint(1, max_tokens)
        docs.append([f"t{rng.randrange(vocab)}" for _ in range(n)])
    return docs


def _all_postings(idx, docs):
    terms = sorted({t for d in docs for t in d})
    return {t: idx.get_postings(t) for t in terms}


def test_index_parity_incremental():
    rng = random.Random(42)
    docs = _random_docs(rng, 60, vocab=80)
    native = index.InvertedIndex()
    compat = index._CompatInvertedIndex()
    # Interleave finalize() calls (the turn-per-append pattern).
    for i, doc in enumerate(docs):
        native.add_document(i, doc)
        compat.add_document(i, doc)
        if i % 7 == 0:
            native.finalize()
            compat.finalize()
    native.finalize()
    compat.finalize()
    assert native.doc_count() == compat.doc_count() == 60
    assert native.max_doc_id() == compat.max_doc_id() == 59
    assert native.sum_doc_lengths() == compat.sum_doc_lengths()
    assert native.avg_doc_len() == pytest.approx(compat.avg_doc_len())
    assert native.total_postings() == compat.total_postings()
    terms = sorted({t for d in docs for t in d})
    for term in terms:
        np_pl = native.get_postings(term)
        cp_pl = compat.get_postings(term)
        assert np_pl == cp_pl, term
    # Sorted by doc_id like Python's finalize list(zip)+sort.
    for term in terms:
        pl = native.get_postings(term)
        if pl:
            doc_ids = [d for d, _ in pl]
            assert doc_ids == sorted(doc_ids)


def test_index_add_after_finalize():
    native = index.InvertedIndex()
    compat = index._CompatInvertedIndex()
    for impl in (native, compat):
        impl.add_document(0, ["a", "b", "c"])
        impl.finalize()
        impl.add_document(1, ["b", "c", "d"])  # the headline fix
        impl.finalize()
        impl.add_document(2, ["c", "d", "e"])
    assert native.get_postings("c") == compat.get_postings("c") == [(0, 1), (1, 1), (2, 1)]
    assert native.get_postings("a") == compat.get_postings("a") == [(0, 1)]
    assert native.get_postings("e") == compat.get_postings("e") == [(2, 1)]
    assert native.get_postings("zzz") is None
    assert compat.get_postings("zzz") is None


def test_index_batch_reference_matches_incremental():
    rng = random.Random(7)
    docs = _random_docs(rng, 40, vocab=50)
    incremental = index.InvertedIndex()
    for i, doc in enumerate(docs):
        incremental.add_document(i, doc)
        incremental.finalize()  # every turn
    batch = index.InvertedIndex()
    for i, doc in enumerate(docs):
        batch.add_document(i, doc)
    batch.finalize()
    terms = sorted({t for d in docs for t in d})
    for term in terms:
        assert incremental.get_postings(term) == batch.get_postings(term), term


def test_blob_roundtrip_and_cross_load():
    rng = random.Random(99)
    docs = _random_docs(rng, 30, vocab=40)
    native = index.InvertedIndex()
    for i, doc in enumerate(docs):
        native.add_document(i, doc)
        if i % 5 == 0:
            native.finalize()
    blob = native.save()
    assert blob[:6] == b"KNIDX1"

    # Native round-trip: byte-identical re-save.
    native2 = index.InvertedIndex()
    assert native2.load(blob)
    assert native2.save() == blob
    terms = sorted({t for d in docs for t in d})
    for term in terms:
        assert native2.get_postings(term) == native.get_postings(term), term

    # Cross-load: native blob -> _compat index (shared blob format).
    compat = index._CompatInvertedIndex()
    assert compat.load(blob)
    for term in terms:
        assert compat.get_postings(term) == native.get_postings(term), term
    # _compat blob -> native index.
    compat_blob = compat.save()
    assert compat_blob == blob
    native3 = index.InvertedIndex()
    assert native3.load(compat_blob)
    for term in terms:
        assert native3.get_postings(term) == native.get_postings(term), term

    # Malformed blob is rejected.
    assert not native2.load(b"garbage")
    assert not native2.load(blob[:-1])


def test_toggle_fallback(monkeypatch):
    monkeypatch.setenv("KIMIX_NATIVE_INDEX", "0")
    assert kimix_native.use_native("INDEX") is False
    tok = index.NgramTokenizer()
    assert tok.tokenize("你好世界") == ["你好", "好世", "世界"]
    idx = index.InvertedIndex()
    idx.add_document(0, ["a", "b", "b"])
    idx.finalize()
    assert idx.get_postings("b") == [(0, 2)]
    assert idx.get_postings("a") == [(0, 1)]
    # Native path still works with the toggle on for TEXT (unrelated kernel).
    assert kimix_native.use_native("INDEX") is False


def test_toggle_off_globally():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    env = dict(os.environ)
    env["KIMIX_NATIVE"] = "0"
    code = (
        "import sys; sys.path.insert(0, {py!r}); "
        "from kimix_native import index; "
        "t = index.NgramTokenizer(); "
        "assert t.tokenize('你好世界') == ['你好', '好世', '世界']; "
        "i = index.InvertedIndex(); "
        "i.add_document(0, ['a','b','b']); i.finalize(); "
        "assert i.get_postings('b') == [(0, 2)]; "
        "print('fallback-ok')"
    ).format(py=os.path.join(root, "python"))
    proc = subprocess.run(
        [sys.executable, "-c", code], cwd=root, env=env,
        capture_output=True, text=True, timeout=60,
    )
    assert proc.returncode == 0, proc.stderr
    assert "fallback-ok" in proc.stdout
