"""Differential parity tests for the retrieve builtin tool (kimix-base <-> kimi-agent).

``src/builtin_tools/retrieve_tool.cpp`` ports the deterministic half of
kimi-agent's ``retrieve`` memory/history tool.  The heavy FTS5 / BM25 search
itself stays in Python (the C++ side receives an injected ``HistoryIndexView``),
so what this module compares is:

* the *whole user-visible output* of the reference tool
  (``kimi_cli/tools/memory/__init__.py``) -- query mode, ``id`` mode, the
  not-found text, the ``[compacted]`` / ``[current]`` marker and the
  ``f"{score:.2f}"`` relevance rendering -- against
  ``runtime_py.builtin_tools.web.format_retrieve_result`` (FOCUS 3/4).  The
  reference is driven through its own ``retrieve`` CallableTool2 with a stub
  ``HistoryIndex``, so the comparison is against the real code path, not a
  re-implementation of it.
* ``search_with_recency`` -- the recency decay, its composition with the BM25
  score, the sort and the truncation (FOCUS 2).  The reference side runs the
  *real* ``HistoryIndex.search_with_recency`` body (``history_index.py:585``)
  with ``time.time`` pinned and a stubbed ``search``; the C++ side is
  ``web.apply_recency_boost`` + ``web.sort_and_truncate``.
* ``id`` reference parsing -- ``HistoryIndex.get_by_id`` (FOCUS 4) against
  ``web.parse_turn_reference``.
* the BM25 kernels the search path is built on (FOCUS 1):
  ``search.bm25_idf`` / ``bm25_score`` / ``bm25_topk`` against
  ``kimix/retrieval.py::BM25Scorer`` (``_idf`` / ``_accumulate`` /
  ``score_topk``) over postings extracted from a real reference
  ``InvertedIndex``, plus the tokenizer normalisation the retrieval path
  depends on (FOCUS 5: ``NgramTokenizer``/``InvertedIndex``).

Provenance rules (same as the other parity modules):

* Everything is compared against the kimi-agent checkout
  (``C:/dev/kimi-agent``, override with ``KIMI_AGENT_ROOT``), never against
  ``python/kimix_native``'s ``_compat`` mirrors.
* ``kimix.retrieval`` short-circuits its tokenizer to
  ``<kimi-agent>/bin``'s *released* native module; ``_parity_ref.pure_python``
  turns that gate off for every reference call.
* Import order is load-bearing: ``_parity_ref.native()`` imports this
  checkout's ``runtime_py`` first and verifies ``runtime_py.__file__``, because
  importing any ``kimi_cli`` module inserts ``<kimi-agent>/bin`` (an older
  released ``runtime_py.pyd``) at ``sys.path[0]``.

Kernels of this tool that are *not* reachable from Python (``parse_params``,
``run_retrieve``'s orchestration -- ``k * 3`` candidate pool, the guidance /
not-found envelopes -- and the ``Retrieve`` Tool wrapper) are covered by the
golden vectors in ``tests/unit/builtin_tools/test_retrieve_tool.cpp`` (target
``test_builtin_retrieve``); every golden there is re-derived from the reference
by ``test_unexposed_kernels_match_reference_goldens`` below.
"""

from __future__ import annotations

import asyncio
import math
import os
import random
import sys
from unittest import mock

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import _parity_ref as pr  # noqa: E402

runtime_py = pr.native()
WEB = runtime_py.builtin_tools.web
SEARCH = runtime_py.search
INDEX = runtime_py.index

_REF_READY = pr.ref_available()

pytestmark = pytest.mark.skipif(
    not _REF_READY,
    reason="kimi-agent reference checkout not importable (set KIMI_AGENT_ROOT)",
)

#: The C++ extension must be THIS checkout's build, never kimi-agent's staged
#: ``runtime_py.pyd`` (``_parity_ref.native()`` already verified
#: ``runtime_py.__file__`` and purged any foreign extension).  The check is on
#: the *location* (``<this repo>/bin/<mode>``), not on the checkout directory's
#: name or the build mode: both vary per machine (``kimix-base`` vs
#: ``KimiX-native`` vs an agent worktree; ``release`` vs ``debug``).
assert pr.BIN_DIR is not None
assert pr.BIN_DIR.parent.name == "bin", pr.BIN_DIR
assert os.path.normcase(str(pr.BIN_DIR.parent.parent)) == os.path.normcase(
    str(pr.REPO_ROOT)
), f"{pr.BIN_DIR} is outside this checkout ({pr.REPO_ROOT})"


def _mem():
    return pr.ref("kimi_cli.tools.memory")


def _hi():
    return pr.ref("kimi_cli.soul.history_index")


def _retrieval():
    return pr.ref("kimix.retrieval")


# ---------------------------------------------------------------------------
# Reference harness: drive the real retrieve tool with a stub HistoryIndex
# ---------------------------------------------------------------------------

_MISSING = object()


def turn(turn_id, role="user", text="text", timestamp=0.0, score=0.0,
         is_compacted=False):
    """One history turn, in the shape ``HistoryIndex._row_to_turn`` returns."""
    return {
        "turn_id": turn_id,
        "role": role,
        "text": text,
        "timestamp": timestamp,
        "score": score,
        "is_compacted": is_compacted,
    }


class StubIndex:
    """Minimal stand-in for ``HistoryIndex`` (search + id lookup only)."""

    def __init__(self, turns=(), by_id=_MISSING):
        self.turns = [dict(t) for t in turns]
        self._by_id = by_id
        self.calls = []

    def search_with_recency(self, query, *, top_k=3, recency_weight=1.0):
        self.calls.append(("search_with_recency", query, top_k, recency_weight))
        return [dict(t) for t in self.turns]

    def get_by_id(self, ref):
        self.calls.append(("get_by_id", str(ref)))
        if self._by_id is not _MISSING:
            return None if self._by_id is None else dict(self._by_id)
        ref_str = str(ref)
        if ref_str.startswith("prune_"):
            ref_str = ref_str[len("prune_"):]
        for t in self.turns:
            if str(t["turn_id"]) == ref_str:
                return dict(t)
        return None


def ref_call(turns=(), *, query=_MISSING, id=_MISSING, by_id=_MISSING, k=None):
    """Run the reference ``retrieve`` tool; return (result, stub index)."""
    mem = _mem()
    index = StubIndex(turns, by_id)
    tool = mem.retrieve()
    tool.attach_history_index(index)
    kwargs = {}
    if query is not _MISSING:
        kwargs["query"] = query
    if id is not _MISSING:
        kwargs["id"] = id
    if k is not None:
        kwargs["k"] = k
    result = asyncio.run(tool(mem.Params(**kwargs)))
    return result, index


def ref_output(turns=(), *, query=_MISSING, id=_MISSING, by_id=_MISSING):
    result, _ = ref_call(turns, query=query, id=id, by_id=by_id)
    return result.output


def cpp_format(turns, ref_id=""):
    """``runtime_py.builtin_tools.web.format_retrieve_result`` on turn dicts."""
    return WEB.format_retrieve_result([dict(t) for t in turns], ref_id)


def cpp_score_text(value):
    """The rendered ``(relevance: X)`` field for a single-turn search output."""
    out = cpp_format([turn(0, "user", "body", score=value)])
    marker = "(relevance: "
    start = out.index(marker) + len(marker)
    return out[start:out.index(")", start)]


# ---------------------------------------------------------------------------
# FOCUS 3: search-mode rendering
# ---------------------------------------------------------------------------

SEARCH_CASES = {
    "single_current": [turn(0, "user", "hello world", score=0.12345)],
    "single_compacted": [turn(1, "assistant", "summary of the plan",
                              score=1.0, is_compacted=True)],
    "two_mixed": [
        turn(0, "user", "What did we decide about the API design?", score=0.1),
        turn(1, "assistant", "We decided to use REST over GraphQL.",
             is_compacted=True, score=2.5),
    ],
    "empty_text": [turn(0, "user", "", score=0.5)],
    "trailing_newline": [turn(0, "user", "end\n", score=0.5)],
    "leading_newline": [turn(0, "user", "\nstart", score=0.5)],
    "multiline": [turn(0, "user", "l1\nl2\n\nl4", score=0.5)],
    "crlf": [turn(0, "user", "a\r\nb", score=0.5)],
    "quoted_lines": [turn(0, "assistant", "> already quoted\nplain", score=0.5)],
    "tabs": [turn(0, "user", "a\tb", score=0.5)],
    "empty_role": [turn(0, "", "body", score=0.5)],
    "role_other": [turn(0, "tool", "body", score=0.5)],
    "unicode": [turn(0, "user", "café 中文 😀 naïve", score=0.5)],
    "score_zero": [turn(0, "user", "body", score=0.0)],
    "score_big": [turn(0, "user", "body", score=12345.6789)],
    "score_half_tie": [turn(0, "user", "body", score=0.005)],
    "many": [turn(i, "user" if i % 2 else "assistant", f"turn {i}",
                  score=i / 7.0, is_compacted=bool(i % 3 == 0)) for i in range(8)],
}


@pytest.mark.parametrize("case", sorted(SEARCH_CASES), ids=sorted(SEARCH_CASES))
def test_search_output_matches_reference(case):
    """Byte-exact query-mode output vs the real reference tool."""
    turns = SEARCH_CASES[case]
    expected = ref_output(turns, query="irrelevant query")
    assert cpp_format(turns, "") == expected


def test_search_marker_is_reference_marker():
    """The per-turn marker is ``[compacted]`` or ``[current]`` (memory:92)."""
    current = cpp_format([turn(0, "user", "body")])
    compacted = cpp_format([turn(0, "user", "body", is_compacted=True)])
    assert "[current]" in current
    assert "[compacted]" not in current
    assert "[compacted]" in compacted
    assert "[current]" not in compacted


def test_search_no_results_text():
    expected = ref_output([], query="nothing matches")
    # The empty index short-circuits in the reference tool, so pin its text.
    assert expected == "No matching results found in conversation history."
    assert cpp_format([], "") == expected


# ---------------------------------------------------------------------------
# FOCUS 4: id-mode rendering (hit, miss, reference repr)
# ---------------------------------------------------------------------------

ID_CASES = {
    "plain": ([turn(0, "user", "API design", is_compacted=False)], "0"),
    "prune": ([turn(3, "assistant", "turn body", is_compacted=True)], "prune_3"),
    "multiline": ([turn(7, "user", "a\nb\n")], "7"),
    "empty_text": ([turn(1, "user", "")], "1"),
    "quote_in_id": ([turn(2, "user", "b")], "it's"),
    "double_quote_in_id": ([turn(2, "user", "b")], 'a"b'),
    "backslash_in_id": ([turn(2, "user", "b")], "a\\b"),
    "newline_in_id": ([turn(2, "user", "b")], "a\nb"),
    "tab_in_id": ([turn(2, "user", "b")], "a\tb"),
    "unicode_id": ([turn(2, "user", "b")], "café"),
    # NOTE: an *empty* ref_id cannot be expressed through the binding (its
    # default "" means search mode) -- see
    # test_empty_ref_id_is_search_mode_in_the_binding and the C++ golden
    # run_retrieve_empty_id_is_an_id_lookup.
    "space_id": ([turn(2, "user", "b")], "  "),
    "prune_empty": ([turn(2, "user", "b")], "prune_"),
    "odd_chars_id": ([turn(2, "user", "b")], "\x01\x1f\x7f"),
}


@pytest.mark.parametrize("case", sorted(ID_CASES), ids=sorted(ID_CASES))
def test_id_output_matches_reference_hit(case):
    turns, ref_id = ID_CASES[case]
    # The stub resolves *any* ref, so the reference renders the hit branch with
    # this exact ref_id (the interesting part is the f"id={ref_id!r}" repr).
    expected = ref_output(turns, id=ref_id, by_id=turns[0])
    assert cpp_format(turns, ref_id) == expected


def test_empty_ref_id_is_search_mode_in_the_binding():
    """Binding convention: ``format_retrieve_result(turns, "")`` is search mode.

    The reference has no such ambiguity -- ``Params(id="")`` is *not* None, so
    ``_retrieve_by_id("")`` looks up the empty reference and reports
    ``No turn found with id=''.``  The kernel keeps that behaviour on the
    ``run_retrieve`` path (pinned by the C++ golden
    ``run_retrieve_empty_id_is_an_id_lookup``); only the Python binding's
    ``ref_id=""`` default cannot express it.
    """
    turns = [turn(2, "user", "b")]
    assert ref_output(turns, id="", by_id=None) == "No turn found with id=''."
    assert ref_output(turns, query="q") == (
        "Retrieved 1 result(s):\n\n[Conversation history]\n"
        "> **user** [current] (relevance: 0.00)\n> b")
    assert cpp_format(turns, "") == ref_output(turns, query="q")
    # ... and the kernel does perform the id lookup for an empty id.
    assert cpp_format([], "") == ref_output([], query="q")



@pytest.mark.parametrize("case", sorted(ID_CASES), ids=sorted(ID_CASES))
def test_id_output_matches_reference_miss(case):
    """Unknown / unparsable refs: ``No turn found with id={ref!r}.``"""
    _, ref_id = ID_CASES[case]
    expected = ref_output([], id=ref_id, by_id=_MISSING)
    assert expected == f"No turn found with id={ref_id!r}."
    assert cpp_format([], ref_id) == expected


def test_id_repr_uses_python_repr_rules():
    """``f"id={ref!r}"`` (memory:109/117) -- quoting/escaping rules."""
    for ref_id in ["plain", "it's", 'both\'and"', "a\\b", "\x00", "\x7f",
                   "line\nbreak", "caf\u00e9"]:
        expected = ref_output([], id=ref_id, by_id=_MISSING)
        assert cpp_format([], ref_id) == expected, ref_id


# ---------------------------------------------------------------------------
# FOCUS 3: "{score:.2f}" rendering
# ---------------------------------------------------------------------------

SCORE_GOLDENS = [
    0.0, 0.005, 0.015, 0.025, 0.045, 0.055, 0.125, 0.375, 1.005, 2.675,
    99.995, 0.1, 0.30000000000000004, 1.0, 1.5, 2.5, 3.14159265358979,
    123456789.123456, 1e15, 1e-9, 0.999999999999,
]


@pytest.mark.parametrize("value", SCORE_GOLDENS, ids=[repr(v) for v in SCORE_GOLDENS])
def test_score_rendering_goldens(value):
    assert cpp_score_text(value) == f"{value:.2f}"


def test_score_rendering_fuzz():
    """2000 seeded doubles: the C++ text is CPython's ``format(v, '.2f')``."""
    rng = random.Random(20240924)
    bad = []
    for _ in range(2000):
        value = rng.uniform(0.0, 50.0)
        got = cpp_score_text(value)
        want = f"{value:.2f}"
        if got != want:
            bad.append((value, got, want))
    assert not bad, f"{len(bad)} mismatches, first: {bad[:5]}"


# ---------------------------------------------------------------------------
# FOCUS 2: recency boost + sort + truncation
# ---------------------------------------------------------------------------

NOW = 1_700_000_000.0
HOUR = 3600.0


def _ref_recency(candidates, now, top_k, recency_weight):
    """Run the real ``HistoryIndex.search_with_recency`` body deterministically.

    ``_use_fts`` is False by default (no ``db_path``), so the reference reads
    ``_turns_list`` and calls ``self.search`` -- both stubbed here; the
    boost/sort/truncate body under test is untouched.
    """
    hi = _hi()
    index = hi.HistoryIndex()
    index._turns_list = [{"turn_id": t["turn_id"]} for t in candidates]
    index.search = lambda query, top_k=3: [dict(t) for t in candidates]
    with mock.patch.object(hi.time, "time", return_value=now):
        return index.search_with_recency("query", top_k=top_k,
                                         recency_weight=recency_weight)


def _cpp_recency(candidates, now, top_k, recency_weight):
    turns = WEB.apply_recency_boost([dict(t) for t in candidates], recency_weight, now)
    return WEB.sort_and_truncate(turns, top_k)


RECENCY_CASES = {
    "newer_wins_tie_bm25": [
        turn(0, "user", "older", timestamp=NOW - 2 * HOUR, score=1.0),
        turn(2, "user", "newer", timestamp=NOW, score=1.0),
    ],
    "old_high_score_kept": [
        turn(1, "user", "old", timestamp=NOW - 48 * HOUR, score=3.0),
        turn(2, "user", "new", timestamp=NOW, score=1.0),
    ],
    "stable_ties": [
        turn(5, "user", "a", timestamp=NOW, score=2.0),
        turn(6, "user", "b", timestamp=NOW, score=2.0),
        turn(7, "user", "c", timestamp=NOW, score=2.0),
    ],
    "future_timestamp": [
        turn(1, "user", "future", timestamp=NOW + 5 * HOUR, score=1.0),
        turn(2, "user", "now", timestamp=NOW, score=1.0),
    ],
    "zero_scores": [
        turn(1, "user", "a", timestamp=NOW, score=0.0),
        turn(2, "user", "b", timestamp=NOW - HOUR, score=0.0),
    ],
    "negative_timestamp": [
        turn(1, "user", "a", timestamp=-1000.0, score=5.0),
        turn(2, "user", "b", timestamp=NOW, score=0.001),
    ],
    "sparse_ids": [
        turn(0, "user", "a", timestamp=NOW, score=1.0),
        turn(9, "user", "b", timestamp=NOW - HOUR, score=1.0),
        turn(40, "user", "c", timestamp=NOW - 3 * HOUR, score=1.0),
    ],
}


@pytest.mark.parametrize("case", sorted(RECENCY_CASES), ids=sorted(RECENCY_CASES))
@pytest.mark.parametrize("top_k", [1, 2, 3, 10])
def test_recency_ranking_matches_reference(case, top_k):
    candidates = RECENCY_CASES[case]
    expected = _ref_recency(candidates, NOW, top_k, 1.0)
    got = _cpp_recency(candidates, NOW, top_k, 1.0)
    assert [(t["turn_id"], t["score"], t["boosted_score"]) for t in got] == [
        (t["turn_id"], t["score"], t["boosted_score"]) for t in expected
    ]


@pytest.mark.parametrize("recency_weight", [0.0, 0.25, 1.0, 3.5, -1.0])
def test_recency_weight_matches_reference(recency_weight):
    candidates = RECENCY_CASES["newer_wins_tie_bm25"]
    expected = _ref_recency(candidates, NOW, 3, recency_weight)
    got = _cpp_recency(candidates, NOW, 3, recency_weight)
    assert [(t["turn_id"], t["boosted_score"]) for t in got] == [
        (t["turn_id"], t["boosted_score"]) for t in expected
    ]


def test_recency_decay_is_24h_exponential():
    """boost = 1 + w * exp(-hours_ago / 24) (history_index.py:610)."""
    candidates = [
        turn(1, "user", "24h", timestamp=NOW - 24 * HOUR, score=1.0),
        turn(2, "user", "48h", timestamp=NOW - 48 * HOUR, score=1.0),
        turn(3, "user", "now", timestamp=NOW, score=1.0),
    ]
    got = {t["turn_id"]: t["boosted_score"]
           for t in _cpp_recency(candidates, NOW, 3, 1.0)}
    assert got[1] == pytest.approx(1.0 + math.exp(-1.0), rel=0, abs=1e-15)
    assert got[2] == pytest.approx(1.0 + math.exp(-2.0), rel=0, abs=1e-15)
    assert got[3] == 2.0
    assert got[3] > got[1] > got[2]


def test_recency_sort_is_stable_over_bm25_order():
    """Equal boosted scores keep the BM25 (candidate) order -- sorted(key=...)."""
    candidates = [
        turn(11, "user", "first", timestamp=NOW, score=1.0),
        turn(12, "user", "second", timestamp=NOW, score=1.0),
        turn(13, "user", "third", timestamp=NOW, score=1.0),
    ]
    got = [t["turn_id"] for t in _cpp_recency(candidates, NOW, 3, 1.0)]
    expected = [t["turn_id"] for t in _ref_recency(candidates, NOW, 3, 1.0)]
    assert got == expected == [11, 12, 13]


# ---------------------------------------------------------------------------
# FOCUS 4: parse_turn_reference vs HistoryIndex.get_by_id
# ---------------------------------------------------------------------------

REF_STRINGS = [
    "0", "1", "42", "0007", "+42", "+0", "-5", " 42", "42 ", "\t42\n",
    "prune_0", "prune_42", "prune_+42", "prune_ 7", "prune_-3", "prune_",
    "prune_prune_1", "", " ", "abc", "prune_abc", "0x10", "42_", "_42",
    "1_0", "1__0", "4.2", "4 2", "42\nmore", "nan", "inf", "-0", "+ 42",
]


def _ref_get_by_id_turns(ids):
    """A real HistoryIndex whose legacy turn list holds *ids*."""
    hi = _hi()
    index = hi.HistoryIndex()
    index._turns_list = [
        {"turn_id": i, "role": "user", "text": f"t{i}", "timestamp": 0.0,
         "is_compacted": False}
        for i in ids
    ]
    return index


def test_turn_reference_parsing_matches_reference_lookup():
    """A ref the reference resolves must resolve in the C++ kernel (and v.v.)."""
    index = _ref_get_by_id_turns([0, 1, 3, 5, 7, 42, 100])
    for ref in REF_STRINGS:
        expected = index.get_by_id(ref)
        got = WEB.parse_turn_reference(ref)
        if expected is None:
            # None means "no turn" for the reference; the kernel reports -1
            # for unparsable refs and the binding maps it back to None.
            assert got is None or got not in (0, 1, 3, 5, 7, 42, 100), (
                ref, got)
        else:
            assert got == expected["turn_id"], ref


def test_turn_reference_matches_python_int_semantics():
    """The reference strips ``prune_`` and calls ``int()`` (history_index.py:551-557).

    The kernel returns the parsed integer and uses -1 as its "unparsable"
    sentinel; the Python binding maps every negative result back to None, so a
    negative parse (``int("-5") == -5``) is indistinguishable from a rejected
    one at the binding boundary.  Turn ids are never negative, so that
    ambiguity cannot be observed by the tool.
    """

    def reference(ref: str):
        ref_str = str(ref)
        if ref_str.startswith("prune_"):
            ref_str = ref_str[len("prune_"):]
        try:
            return int(ref_str)
        except ValueError:
            return None

    bad = []
    for ref in REF_STRINGS:
        want = reference(ref)
        got = WEB.parse_turn_reference(ref)
        if want is None or want < 0:
            if got is not None:
                bad.append((ref, got, want))
        elif got != want:
            bad.append((ref, got, want))
    assert not bad, bad


def test_turn_reference_ascii_gate_is_documented():
    """Known gap: Python's ``int()`` accepts non-ASCII decimal digits.

    ``int("\\uff11\\uff12") == 12`` (fullwidth) and ``int("\\u0664\\u0662") == 42``
    (Arabic-Indic).  The kernel implements the ASCII subset of ``int()``, like
    every other ASCII-gated kernel in this port, so it reports "unparsable".
    Pinned here so the difference is deliberate and visible.
    """
    assert WEB.parse_turn_reference("\uff11\uff12") is None
    assert int("\uff11\uff12") == 12
    assert WEB.parse_turn_reference("\u0664\u0662") is None
    assert int("\u0664\u0662") == 42


# ---------------------------------------------------------------------------
# FOCUS 1: BM25 kernels vs kimix.retrieval.BM25Scorer
# ---------------------------------------------------------------------------

#: Turn-like documents (text inspired by kimi-agent's own history_index tests).
BM25_DOCS = [
    (0, "How do I compile Python?"),
    (1, "Use pyinstaller or cx_Freeze."),
    (2, "older document about alpha programming"),
    (3, "unrelated answer about java"),
    (4, "newer document about alpha programming"),
    (5, "another unrelated answer about golang"),
    (6, "The exact original text must be preserved."),
    (7, "What did we decide about the API design?"),
    (8, "We decided to use REST over GraphQL."),
    (9, "cache invalidation and naming things"),
    (10, "!!! ??? ..."),
]

BM25_QUERIES = [
    "alpha programming",
    "python compile",
    "REST GraphQL API design",
    "zzz_nonexistent_zzz",
    "alpha",
    "alpha alpha programming",
    "a",
    "the document about",
    "preserved text original",
    "java golang",
]


def _ref_index_and_tokenizer():
    """A reference InvertedIndex + tokenizer with the native gate disabled."""
    ret = _retrieval()
    tokenizer = ret.NgramTokenizer(n=2)
    index = ret.InvertedIndex()
    ctx = pr.pure_python(ret)
    ctx.__enter__()
    try:
        for doc_id, text in BM25_DOCS:
            index.add_document(doc_id, tokenizer.tokenize(text))
        index.finalize()
    finally:
        ctx.__exit__(None, None, None)
    return ret, index, tokenizer


def _cpp_index_from_reference_tokenizer(tokenizer):
    """The C++ InvertedIndex fed with the reference's own tokens.

    Uses the shim composition (``_cpp_tokenize``) so the comparison is about
    the index, not about the tokenizer (which the test above covers).
    """
    index = INDEX.InvertedIndex()
    cpp_tok = _cpp_tokenizer(2)
    for doc_id, text in BM25_DOCS:
        index.add_document(doc_id, [g.encode() for g in
                                    _cpp_tokenize(cpp_tok, text)])
    index.finalize()
    return index


def _ref_scorer():
    ret, index, tokenizer = _ref_index_and_tokenizer()
    return ret, index, tokenizer, ret.BM25Scorer(index)


def _pipeline_tokens(tokenizer, query):
    """The tokens the reference pipeline actually scores.

    ``Searcher.search`` deduplicates after tokenizing
    (``unique_query = list(dict.fromkeys(query_tokens))``), which is exactly the
    ``q_weight == 1`` model the C++ binding exposes (bm25.h:12).
    """
    return list(dict.fromkeys(tokenizer.tokenize(query)))


def _cpp_bm25_scores(index, scorer, query_tokens, k1=1.2, b=0.75):
    """Feed the reference's postings/idf/doc lengths to the C++ kernel."""
    postings = []
    idfs = []
    for token in query_tokens:
        pl = index.get_postings(token)
        if pl is None or len(pl[0]) == 0:
            continue
        docs, tfs = pl
        postings.append([(int(d), int(t)) for d, t in zip(docs.tolist(),
                                                          tfs.tolist())])
        idfs.append(scorer._idf(len(docs), index.N))
    lengths = [int(v) for v in index.doc_lengths_arr.tolist()]
    scores = SEARCH.bm25_score(postings, idfs, lengths, index.avgdl, index.N,
                               k1, b)
    return scores, postings, idfs, lengths


def test_bm25_query_weight_contract():
    """The kernel's ``q_weight == 1`` model needs caller-side deduplication.

    Repeating a term's postings is *not* bit-identical to the reference's
    Counter/q_weight accumulation (round(2x/d) summed once differs from
    round(x/d) summed twice), which is why ``Searcher.search`` deduplicates
    before scoring.  Pinned so the shim keeps doing the same.
    """
    ret, index, tokenizer, scorer = _ref_scorer()
    tokens = tokenizer.tokenize("alpha alpha programming")
    unique = list(dict.fromkeys(tokens))
    assert len(unique) < len(tokens), "query must repeat a token"

    with pr.pure_python(ret):
        grouped = scorer.score(tokens)          # Counter(q_weight) path
        deduped = scorer.score(unique)          # what the pipeline scores
    assert grouped != deduped  # the q_weight=2 path really differs

    counts = {}
    for token in tokens:
        counts[token] = counts.get(token, 0) + 1
    postings, idfs = [], []
    for token in unique:
        pl = index.get_postings(token)
        if pl is None:
            continue
        docs, tfs = pl
        entry = [(int(d), int(t)) for d, t in zip(docs.tolist(), tfs.tolist())]
        for _ in range(counts[token]):
            postings.append(entry)
            idfs.append(scorer._idf(len(docs), index.N))
    lengths = [int(v) for v in index.doc_lengths_arr.tolist()]
    repeated = SEARCH.bm25_score(postings, idfs, lengths, index.avgdl,
                                 index.N, 1.2, 0.75)
    shared = [d for d in range(index.N) if deduped.get(d) and grouped.get(d)]
    assert shared
    for doc in shared:
        assert repeated[doc] == pytest.approx(grouped[doc], rel=1e-14)


def test_bm25_idf_matches_reference():
    ret = _retrieval()
    with pr.pure_python(ret):
        for n_docs in [0, 1, 2, 10, 100, 1000, 50000]:
            for df in [0, 1, 2, n_docs // 2, n_docs, n_docs + 1, n_docs * 2 + 3]:
                expected = ret.BM25Scorer._idf(df, n_docs)
                got = SEARCH.bm25_idf(n_docs, df)
                assert got == expected, (n_docs, df, got, expected)


def test_bm25_idf_ignores_k1_and_b():
    """``bm25_idf``'s signature carries k1/b; CPython's ``_idf`` ignores both."""
    for k1, b in [(1.2, 0.75), (2.0, 0.5), (0.0, 1.0)]:
        assert SEARCH.bm25_idf(100, 10, k1, b) == SEARCH.bm25_idf(100, 10)


def test_bm25_scores_match_reference_bit_for_bit():
    ret, index, tokenizer, scorer = _ref_scorer()
    for query in BM25_QUERIES:
        tokens = _pipeline_tokens(tokenizer, query)
        with pr.pure_python(ret):
            expected = scorer.score(tokens)
        got, _, _, _ = _cpp_bm25_scores(index, scorer, tokens)
        assert len(got) == index.N
        for doc_id in range(index.N):
            want = expected.get(doc_id, 0.0)
            assert got[doc_id] == want, (query, doc_id, got[doc_id], want)


def test_bm25_scores_match_reference_with_candidate_docs():
    """``candidate_docs`` filtering keeps the *global* df for the idf."""
    ret, index, tokenizer, scorer = _ref_scorer()
    tokens = _pipeline_tokens(tokenizer, "alpha programming")
    with pr.pure_python(ret):
        expected = scorer.score(tokens, candidate_docs={0, 2, 4, 6})
    # The kernel has no candidate filter; scoring every doc and masking must
    # agree on the filtered docs *and* use the same (global) idf.
    got, postings, idfs, _ = _cpp_bm25_scores(index, scorer, tokens)
    for doc_id, want in expected.items():
        assert got[doc_id] == want, doc_id
    for token, idf in zip(tokens, idfs):
        docs, _ = index.get_postings(token)
        assert idf == scorer._idf(len(docs), index.N)


def test_bm25_topk_matches_reference_ordering():
    """Ranking agrees with ``score_topk`` wherever the reference is ordered.

    ``score_topk``'s dense ``top_k < N`` branch ends in
    ``np.argsort(-top_scores)`` over an ``argpartition`` selection, which is
    *not* order-stable for equal scores, so ties are compared as a set; the
    C++ kernel promises the deterministic (score desc, doc asc) order that the
    reference's own sparse path uses (``heapq.nlargest(key=(score, -doc_id))``).
    """
    ret, index, tokenizer, scorer = _ref_scorer()
    for query in BM25_QUERIES:
        tokens = _pipeline_tokens(tokenizer, query)
        with pr.pure_python(ret):
            expected = scorer.score_topk(tokens, 3)
        got_scores, _, _, _ = _cpp_bm25_scores(index, scorer, tokens)
        got = [(doc, got_scores[doc]) for doc in SEARCH.bm25_topk(got_scores, 3)]
        want = [(int(d), float(s)) for d, s in expected]
        assert sorted(got) == sorted(want), query
        if len({s for _, s in want}) == len(want):
            assert got == want, query


def test_bm25_topk_dense_branch_matches_reference():
    """``top_k >= N``: same documents, and the ordering difference is pinned.

    The reference's dense ``top_k >= N`` branch returns ``np.flatnonzero``
    order (ascending doc id); the kernel always promises (score desc, doc asc),
    which is what the reference's sparse ``top_k >= len(scores)`` branch does
    (``sorted(..., key=lambda x: (-x[1], x[0]))``).  Only the order differs --
    the pipeline re-ranks by boosted score straight afterwards.
    """
    ret, index, tokenizer, scorer = _ref_scorer()
    tokens = _pipeline_tokens(tokenizer, "alpha programming")
    with pr.pure_python(ret):
        expected = scorer.score_topk(tokens, index.N + 5)
    got_scores, _, _, _ = _cpp_bm25_scores(index, scorer, tokens)
    got = SEARCH.bm25_topk(got_scores, index.N + 5)
    assert [int(d) for d, _ in expected] == sorted(int(d) for d, _ in expected)
    assert sorted(int(d) for d, _ in expected) == sorted(got)
    assert {d: got_scores[d] for d in got} == {
        int(d): float(s) for d, s in expected}


def test_bm25_topk_ties_break_by_doc_id():
    """``heapq.nlargest(key=(score, -doc_id))`` -> equal scores, doc asc."""
    scores = [0.0, 2.0, 2.0, 1.0, 2.0, 0.0, 1.0]
    assert SEARCH.bm25_topk(scores, 3) == [1, 2, 4]
    assert SEARCH.bm25_topk(scores, 10) == [1, 2, 4, 3, 6]
    assert SEARCH.bm25_topk(scores, 0) == []
    assert SEARCH.bm25_topk([], 3) == []
    assert SEARCH.bm25_topk([0.0, -1.0, float("nan")], 3) == []



def test_bm25_sparse_doc_ids_match_reference():
    """doc_count = N = max_doc_id + 1; missing doc ids score 0 (retrieval.py:746)."""
    ret = _retrieval()
    with pr.pure_python(ret):
        tokenizer = ret.NgramTokenizer(n=2)
        index = ret.InvertedIndex()
        for doc_id in (0, 5, 9):
            index.add_document(doc_id, tokenizer.tokenize("shared token here"))
        index.finalize()
        scorer = ret.BM25Scorer(index)
        tokens = tokenizer.tokenize("shared")
        expected = scorer.score(tokens)
    assert index.N == 10
    got, _, _, _ = _cpp_bm25_scores(index, scorer, tokens)
    assert len(got) == 10
    assert [d for d in range(10) if got[d] != 0.0] == sorted(expected)
    for doc_id, want in expected.items():
        assert got[doc_id] == want


def test_bm25_scorer_uses_reference_k1_b_defaults():
    ret, index, tokenizer, scorer = _ret_scorer()
    assert scorer.k1 == 1.2 and scorer.b == 0.75
    got_scores, _, _, _ = _cpp_bm25_scores(index, scorer,
                                           tokenizer.tokenize("alpha programming"))
    other, _, _, _ = _cpp_bm25_scores(index, scorer,
                                      tokenizer.tokenize("alpha programming"),
                                      k1=2.0, b=0.5)
    assert got_scores != other


def _ret_scorer():
    return _ref_scorer()


# ---------------------------------------------------------------------------
# FOCUS 1/5: tokenizer normalisation + index contract
# ---------------------------------------------------------------------------

TOKENIZER_TEXTS = [
    "How do I compile Python?",
    "  padded text  ",
    "UPPER lower MiXeD",
    "tab\tand\nnewline",
    "punctuation!!! ??? ,,, ---",
    "a",
    "ab",
    "abc",
    "0123456789",
    "caf\u00e9 na\u00efve",
    "\u4e2d\u6587\u6587\u672c\u6d4b\u8bd5",
    "\u4e2d\u6587 mixed ascii",
    "",
    "   ",
]


def _cpp_tokenizer(default_n=2):
    return INDEX.NgramTokenizer(default_n)


def _cpp_tokenize(tokenizer, text, n=None):
    """The shim's composition (index.py:343-350): normalize -> strip -> n-grams.

    The kernel's ``tokenize`` deliberately does neither (ngram_tokenizer.h:22-27),
    so the parity target is the composed call.
    """
    norm = tokenizer.normalize(text.encode("utf-8", "surrogatepass")).strip()
    if not norm:
        return []
    use_n = n if n is not None else tokenizer.detect_n(norm)
    return [g.decode() for g in tokenizer.tokenize(norm, use_n)]


@pytest.mark.parametrize("text", TOKENIZER_TEXTS, ids=[repr(t) for t in TOKENIZER_TEXTS])
@pytest.mark.parametrize("n", [1, 2, 3])
def test_ngram_tokenizer_matches_reference(text, n):
    """ASCII-only corpus: ``normalize`` = lower + strip, overlapping n-grams."""
    ret = _retrieval()
    with pr.pure_python(ret):
        tok = ret.NgramTokenizer(n=n)
        expected_norm = tok.normalize(text) if text.isascii() else None
        expected_tokens = tok.tokenize(text, n) if text.isascii() else None

    cpp = _cpp_tokenizer(n)
    if expected_norm is not None:
        assert cpp.normalize(text.encode()).decode() == expected_norm, text
        assert _cpp_tokenize(cpp, text, n) == expected_tokens, (text, n)


@pytest.mark.parametrize("text", TOKENIZER_TEXTS, ids=[repr(t) for t in TOKENIZER_TEXTS])
def test_ngram_tokenizer_autodetect_matches_reference(text):
    """``tokenize(text, None)`` auto-detects n exactly like ``_detect_n``."""
    if not text.isascii():
        return
    ret = _retrieval()
    with pr.pure_python(ret):
        expected = ret.NgramTokenizer(n=2).tokenize(text)
    assert _cpp_tokenize(_cpp_tokenizer(2), text) == expected, text



def test_ngram_tokenizer_detect_n_matches_reference():
    ret = _retrieval()
    cases = ["abc", "compiling python code", "a", "",
             "\u4e2d\u6587\u6587\u672c", "mixed \u4e2d\u6587 text here"]
    for text in cases:
        with pr.pure_python(ret):
            expected = ret.NgramTokenizer(n=2)._detect_n(text)
        got = INDEX.NgramTokenizer(2).detect_n(text.encode())
        assert got == expected, text


def test_inverted_index_stats_match_reference():
    """doc_count/max_doc_id/doc_length/avgdl agree with the reference index."""
    ret, index, tokenizer = _ref_index_and_tokenizer()
    cpp = _cpp_index_from_reference_tokenizer(tokenizer)

    assert cpp.doc_count() == index.N
    assert cpp.max_doc_id() == index._max_doc_id
    assert cpp.sum_doc_lengths() == index._sum_doc_lengths
    assert cpp.avg_doc_len() == index.avgdl
    for doc_id, text in BM25_DOCS:
        assert cpp.doc_length(doc_id) == len(tokenizer.tokenize(text))


def test_inverted_index_postings_match_reference_for_kept_terms():
    """Postings for every term the reference keeps (it prunes stop-ngrams)."""
    ret, index, tokenizer = _ref_index_and_tokenizer()
    cpp = _cpp_index_from_reference_tokenizer(tokenizer)

    checked = 0
    for term in sorted(index._term_to_id):
        docs, tfs = index.get_postings(term)
        expected = [(int(d), int(t)) for d, t in zip(docs.tolist(), tfs.tolist())]
        got = cpp.get_postings(term.encode())
        got = [] if got is None else [(int(d), int(t)) for d, t in got]
        assert got == expected, term
        checked += 1
    assert checked > 20
    # Terms the reference pruned are still reachable natively (documented
    # deviation in inverted_index.h: the incremental index keeps all terms).
    from collections import Counter

    df = Counter()
    for _doc_id, text in BM25_DOCS:
        df.update(set(tokenizer.tokenize(text)))
    stop = [t for t, c in df.items() if c > len(BM25_DOCS) * 0.5]
    stop += [t for t in df if t and not t.isalpha()
             and all(not (ch.isalnum() or ch.isspace()) for ch in t)][:5]
    assert stop, "expected the reference to prune at least one stop-ngram"
    for term in stop:
        assert term not in index._term_to_id, term
        assert cpp.get_postings(term.encode()) is not None, term



# ---------------------------------------------------------------------------
# Reference-derived goldens for the kernels that are not exposed to Python
# ---------------------------------------------------------------------------


def test_unexposed_kernels_match_reference_goldens():
    """Re-derive the C++ test file's goldens from the reference tool.

    ``tests/unit/builtin_tools/test_retrieve_tool.cpp`` pastes these strings;
    this test recomputes them from ``kimi_cli.tools.memory`` so a wrong golden
    cannot survive.
    """
    # Golden 1: search-mode output.
    turns = [turn(1, "user", "hello world", score=0.12345)]
    assert ref_output(turns, query="q") == (
        "Retrieved 1 result(s):\n\n[Conversation history]\n"
        "> **user** [current] (relevance: 0.12)\n> hello world"
    )

    # Golden 2: id-mode hit.
    turns = [turn(3, "user", "found turn", is_compacted=True)]
    assert ref_output(turns, id="prune_3") == (
        "Retrieved turn id='prune_3':\n> **user** [compacted]\n> found turn"
    )

    # Golden 3: id-mode miss.
    assert ref_output([], id="prune_999") == "No turn found with id='prune_999'."

    # Golden 4: blank query guidance (ToolOk, not an error).
    result, index = ref_call([turn(1, "user", "x")], query="")
    assert result.output == "No query provided. Pass a `query` string or an `id`."
    assert result.message == "No query"
    assert not result.is_error
    assert index.calls == []

    # Golden 5: default params (nothing provided) take the same path.
    result, index = ref_call([turn(1, "user", "x")])
    assert result.output == "No query provided. Pass a `query` string or an `id`."
    assert result.message == "No query"

    # Golden 6: whitespace-only query is blank for ``str.strip()``.
    for query in ["   ", "\n\t ", "\u00a0", "\u3000"]:
        result, _ = ref_call([turn(1, "user", "x")], query=query)
        assert result.output == "No query provided. Pass a `query` string or an `id`."

    # Golden 7: no results (query mode) / unknown id (id mode) messages.
    result, _ = ref_call([], query="zzz_nonexistent_zzz")
    assert (result.output, result.message) == (
        "No matching results found in conversation history.", "No results")
    assert not result.is_error
    result, _ = ref_call([], id="prune_999")
    assert (result.output, result.message) == (
        "No turn found with id='prune_999'.", "No results")
    assert not result.is_error

    # Golden 8: found messages + the k*3 candidate pool / recency_weight=1.0.
    result, index = ref_call([turn(1, "user", "x")], query="q", k=4)
    assert result.message == "Found 1 result(s)"
    assert index.calls == [("search_with_recency", "q", 4, 1.0)]
    result, index = ref_call([turn(1, "user", "x")], id="1")
    assert result.message == "Found turn id='1'"

    # Golden 9: an empty ``id`` is still an id lookup (``is not None``).
    result, _ = ref_call([turn(1, "user", "x")], id="")
    assert result.output == "No turn found with id=''."
    assert result.message == "No results"

    # Golden 10: id wins over query.
    result, index = ref_call([turn(1, "user", "x")], query="q", id="1")
    assert index.calls == [("get_by_id", "1")]


def test_k_clamp_bounds():
    """``k`` is validated to 1..10 by the pydantic Params model."""
    mem = _mem()
    with pytest.raises(Exception):
        mem.Params(query="q", k=0)
    with pytest.raises(Exception):
        mem.Params(query="q", k=11)
    assert mem.Params(query="q", k=10).k == 10
    assert mem.Params(query="q").k == 3


# ---------------------------------------------------------------------------
# FOCUS 1 (continued): the fuzzy pre-filter the search path uses
# ---------------------------------------------------------------------------


def _load_shim_package(pkg_dir, alias):
    """Load kimi-agent's ``bin/kimix_native`` package under a private alias.

    Loaded like ``test_parity_bash.py`` does: by path, so neither the
    kimix-base ``python/kimix_native`` mirror nor a cached ``sys.modules``
    entry can substitute the reference implementation.
    """
    import importlib.util

    spec = importlib.util.spec_from_file_location(
        alias, pkg_dir / "__init__.py",
        submodule_search_locations=[str(pkg_dir)])
    if spec is None or spec.loader is None:  # pragma: no cover - defensive
        pytest.skip(f"cannot load reference package {pkg_dir}")
    package = importlib.util.module_from_spec(spec)
    sys.modules[alias] = package
    spec.loader.exec_module(package)
    return package


@pytest.fixture(scope="module")
def reference_search_mirror():
    """``<kimi-agent>/bin/kimix_native/search.py`` (the pure-Python bodies)."""
    shim_dir = pr.KIMI_AGENT_ROOT / "bin" / "kimix_native"
    if not (shim_dir / "search.py").is_file():
        pytest.skip(f"{shim_dir / 'search.py'} not found")
    import importlib

    _load_shim_package(shim_dir, "_parity_retrieve_native")
    return importlib.import_module("_parity_retrieve_native.search")


def test_freq_lower_bound_matches_reference(reference_search_mirror):
    """The fuzzy pre-filter both search paths call (retrieval.py:927-931)."""
    corpus = [
        ("abc", "abc"), ("abc", ""), ("", "abc"), ("abc", "abcabc"),
        ("abc", "abd"), ("a", "abcd"), ("compil", "compile"),
        ("python", "pyhton"), ("caf\u00e9", "cafe"), ("aaaa", "aa"),
        ("", ""), ("rest", "graphql"), ("\u4e2d\u6587", "\u4e2d\u6587\u672c"),
    ]
    rng = random.Random(4242)
    for _ in range(400):
        corpus.append(("".join(rng.choice("abcde ") for _ in range(rng.randint(0, 6))),
                       "".join(rng.choice("abcde ") for _ in range(rng.randint(0, 6)))))
    bad = []
    for pattern, term in corpus:
        want = reference_search_mirror._compat_freq_lower_bound(pattern, term)
        got = SEARCH.freq_lower_bound(pattern.encode(), term.encode())
        if got != want:
            bad.append((pattern, term, got, want))
    assert not bad, bad[:5]
    # The retrieval.py class method routes to the same kernel (native path in
    # the reference checkout; with the gate off it uses the same mirror).
    assert reference_search_mirror._compat_freq_lower_bound("abc", "abcabc") == \
        SEARCH.freq_lower_bound(b"abc", b"abcabc")

