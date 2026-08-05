"""Parity tests for kimix_native.index.HistoryIndex (plan 006).

Compares the native ``runtime_py.index.HistoryIndex`` against the pure-Python
``_compat`` mirror (reference history_index.py semantics + the plan 006
incremental/KNHIX1 contract) on:
- seeded 500-turn sessions (mixed roles incl. tool turns, mixed ASCII/CJK):
  >= 50 queries -> same top-k sets (score tolerance 1e-9; ordering
  (score desc, turn_id asc))
- save/load round-trip: identical subsequent search results
- native <-> _compat cross-loading of the KNHIX1 blob
- eviction order (501st append drops turn 0)
- mark_compacted visible in get_by_id
- get_by_id ref parsing ('42', 'prune_42', int), pop_front, reset
- the KIMIX_NATIVE_INDEX=0 fallback toggle

The real kimi-agent reference (kimi_cli.soul.history_index) is used when it
imports (guarded by os.path.exists + try/except); it needs kimix.retrieval +
kosong.message which are not shipped in kimi-agent's kimi-cli/src, so the
_compat mirror is normally used.
"""

import os
import random
import sys

import pytest

import kimix_native
from kimix_native import index

# ---------------------------------------------------------------------------
# Reference import attempt (guarded) — falls back to _compat.
# ---------------------------------------------------------------------------
_REF_HISTORY_INDEX = None
_KIMI_CLI_SRC = r"C:/dev/kimi-agent/kimi-cli/src"
if os.path.isdir(_KIMI_CLI_SRC) and _KIMI_CLI_SRC not in sys.path:
    sys.path.insert(0, _KIMI_CLI_SRC)
try:
    from kimi_cli.soul.history_index import HistoryIndex as _RefHistoryIndex  # noqa: E402
    _REF_HISTORY_INDEX = _RefHistoryIndex
except Exception:  # pragma: no cover - depends on external repo state
    _REF_HISTORY_INDEX = None

# ---------------------------------------------------------------------------
# Seeded session builders
# ---------------------------------------------------------------------------

_ROLES = ["user", "assistant", "tool"]
_WORDS = [
    "alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta",
    "iota", "kappa", "lambda", "mu", "nu", "xi", "omicron", "pi",
    "Hello", "World", "Alpha", "Beta",
    "中国", "中文", "你好", "世界", "搜索", "历史", "记忆", "模型", "检索", "对话",
]


def _make_turns(rng, count):
    turns = []
    for i in range(count):
        role = _ROLES[rng.randrange(len(_ROLES))]
        nwords = rng.randint(1, 10)
        text = " ".join(rng.choice(_WORDS) for _ in range(nwords))
        turns.append((i, 1600000000.0 + i * 1.5, role, False, text))
    return turns


def _queries(rng, count):
    return [
        " ".join(rng.choice(_WORDS) for _ in range(rng.randint(1, 4)))
        for _ in range(count)
    ]


def _seeded():
    rng = random.Random(20240606)
    return _make_turns(rng, 500), _queries(random.Random(77), 60)


def _same_topk(a, b, tol=1e-9):
    assert [t["turn_id"] for t in a] == [t["turn_id"] for t in b]
    for x, y in zip(a, b):
        assert abs(x["score"] - y["score"]) < tol, (x["score"], y["score"])
        assert x["role"] == y["role"]
        assert x["text"] == y["text"]
        assert x["is_compacted"] == y["is_compacted"]


# ---------------------------------------------------------------------------
# Search parity: native vs _compat
# ---------------------------------------------------------------------------


def test_search_parity_seeded_500_turns():
    turns, queries = _seeded()
    native = index.HistoryIndex()
    compat = index._CompatHistoryIndex()
    native.append_turns(turns)
    compat.append_turns(turns)
    assert native.turn_count() == compat.turn_count() == 500

    for q in queries:
        rn = native.search(q, 5)
        rc = compat.search(q, 5)
        _same_topk(rn, rc, tol=1e-9)
        # Ordering invariant: (score desc, turn_id asc).
        keys = [(t["score"], t["turn_id"]) for t in rn]
        assert keys == sorted(keys, key=lambda x: (-x[0], x[1])), q

    # Empty / no-match queries agree.
    assert native.search("", 3) == compat.search("", 3) == []
    assert native.search("zzzzz_nonexistent", 3) == compat.search("zzzzz_nonexistent", 3) == []


def test_search_parity_cjk_and_role_roundtrip():
    turns = [
        (0, 1.0, "user", False, "你好世界 中文搜索"),
        (1, 2.0, "tool", False, "搜索 历史 记忆"),
        (2, 3.0, "assistant", False, "Hello World Alpha"),
    ]
    native = index.HistoryIndex()
    compat = index._CompatHistoryIndex()
    native.append_turns(turns)
    compat.append_turns(turns)
    for q in ("你好", "搜索", "世界 历史", "hello", "Hello World", "ALPHA"):
        rn = native.search(q, 3)
        rc = compat.search(q, 3)
        _same_topk(rn, rc, tol=1e-9)
    # Role int <-> str round-trip; get_by_id exposes the normalized text.
    d = native.get_by_id(1)
    assert d["role"] == "tool"
    assert d["text"] == "搜索 历史 记忆"
    assert d["is_compacted"] is False
    # Role accepted as an int too.
    h = index.HistoryIndex()
    h.append_turns([(0, 1.0, 2, False, "alpha beta")])
    assert h.get_by_id(0)["role"] == "tool"
    h.append_turns([(1, 1.0, 9, False, "gamma delta")])
    assert h.get_by_id(1)["role"] == "other"


# ---------------------------------------------------------------------------
# Persistence: save/load round-trip + cross-load
# ---------------------------------------------------------------------------


def test_save_load_roundtrip_search_identical():
    turns, queries = _seeded()
    native = index.HistoryIndex()
    native.append_turns(turns)
    native.mark_compacted()

    blob = native.save()
    assert blob[:6] == b"KNHIX1"

    native2 = index.HistoryIndex()
    assert native2.load(blob)
    assert native2.turn_count() == 500
    assert native2.get_by_id(0)["is_compacted"] is True
    assert native2.get_by_id(0)["turn_id"] == 0

    for q in queries[:30]:
        a = native.search(q, 5)
        b = native2.search(q, 5)
        assert [t["turn_id"] for t in a] == [t["turn_id"] for t in b], q
        for x, y in zip(a, b):
            assert abs(x["score"] - y["score"]) < 1e-9

    assert not native2.load(b"garbage")
    assert not native2.load(blob[:-1])

    # _compat round-trip.
    compat = index._CompatHistoryIndex()
    compat.append_turns(turns)
    compat_blob = compat.save()
    assert compat_blob[:6] == b"KNHIX1"
    compat2 = index._CompatHistoryIndex()
    assert compat2.load(compat_blob)
    for q in queries[:30]:
        a = compat.search(q, 5)
        b = compat2.search(q, 5)
        assert [t["turn_id"] for t in a] == [t["turn_id"] for t in b], q


def test_blob_byte_identity_native_compat():
    rng = random.Random(99)
    turns = _make_turns(rng, 120)
    native = index.HistoryIndex()
    compat = index._CompatHistoryIndex()
    native.append_turns(turns)
    compat.append_turns(turns)
    # One search per side triggers the single finalize -> identical segment
    # structure -> byte-identical KNHIX1 blobs.
    native.search("alpha", 3)
    compat.search("alpha", 3)
    assert native.save() == compat.save()


def test_cross_load_native_compat():
    turns, queries = _seeded()
    native = index.HistoryIndex()
    native.append_turns(turns)
    blob = native.save()

    # Native blob -> _compat.
    compat = index._CompatHistoryIndex()
    assert compat.load(blob)
    for q in queries[:20]:
        rn = native.search(q, 5)
        rc = compat.search(q, 5)
        assert [t["turn_id"] for t in rn] == [t["turn_id"] for t in rc], q

    # _compat blob -> native.
    compat_blob = compat.save()
    native2 = index.HistoryIndex()
    assert native2.load(compat_blob)
    for q in queries[:20]:
        rn = native.search(q, 5)
        rc = native2.search(q, 5)
        assert [t["turn_id"] for t in rn] == [t["turn_id"] for t in rc], q


# ---------------------------------------------------------------------------
# Eviction, mark_compacted, misc API
# ---------------------------------------------------------------------------


def test_eviction_501st_drops_oldest():
    rng = random.Random(5)
    turns = _make_turns(rng, 501)
    native = index.HistoryIndex()
    native.append_turns(turns)
    assert native.turn_count() == 500
    assert native.get_by_id(0) is None
    assert native.get_by_id(1) is not None
    assert native.get_by_id(500) is not None

    compat = index._CompatHistoryIndex()
    compat.append_turns(turns)
    assert compat.turn_count() == 500
    assert compat.get_by_id(0) is None
    assert compat.get_by_id(1) is not None
    assert compat.get_by_id(500) is not None


def test_mark_compacted_visible_in_get_by_id():
    turns = [
        (0, 1.0, "user", False, "alpha beta"),
        (1, 2.0, "tool", False, "gamma delta"),
    ]
    for impl in (index.HistoryIndex(), index._CompatHistoryIndex()):
        impl.append_turns(turns)
        assert impl.get_by_id(0)["is_compacted"] is False
        impl.mark_compacted()
        assert impl.get_by_id(0)["is_compacted"] is True
        assert impl.get_by_id(1)["is_compacted"] is True
        # Turns added after mark_compacted are not flagged.
        impl.append_turns([(2, 3.0, "assistant", False, "epsilon zeta")])
        assert impl.get_by_id(2)["is_compacted"] is False
        assert impl.get_by_id(1)["is_compacted"] is True


def test_get_by_id_ref_parsing_pop_front_reset():
    h = index.HistoryIndex()
    h.append_turns([(42, 1.0, "user", False, "alpha beta")])
    assert h.get_by_id(42)["turn_id"] == 42
    assert h.get_by_id("42")["turn_id"] == 42
    assert h.get_by_id("prune_42")["turn_id"] == 42
    assert h.get_by_id("prune_99") is None
    assert h.get_by_id("not-a-number") is None
    assert h.turn_count() == 1
    h.pop_front()
    assert h.turn_count() == 0
    assert h.get_by_id(42) is None
    assert h.search("alpha", 3) == []
    h.append_turns([(7, 1.0, "user", False, "x y z")])
    h.reset()
    assert h.turn_count() == 0
    assert h.search("x", 3) == []


def test_toggle_fallback_history(monkeypatch):
    monkeypatch.setenv("KIMIX_NATIVE_INDEX", "0")
    assert kimix_native.use_native("INDEX") is False
    h = index.HistoryIndex()
    h.append_turns([
        (0, 1.0, "user", False, "alpha beta gamma"),
        (1, 2.0, "assistant", False, "beta gamma delta"),
    ])
    assert h.turn_count() == 2
    r = h.search("beta gamma", 2)
    assert len(r) == 2
    assert r[0]["turn_id"] == 0  # identical scores -> turn_id asc
    blob = h.save()
    assert blob[:6] == b"KNHIX1"
    h2 = index.HistoryIndex()
    assert h2.load(blob)
    assert h2.turn_count() == 2


# ---------------------------------------------------------------------------
# Real reference parity (skipped when kimi-agent's history_index.py cannot
# import — kimix.retrieval / kosong.message are not shipped with kimi-cli).
# ---------------------------------------------------------------------------


@pytest.mark.skipif(_REF_HISTORY_INDEX is None,
                    reason="kimi-agent reference not importable")
def test_parity_with_real_reference():
    from kosong.message import Message  # noqa: E402
    from kimi_cli.wire.types import TextPart  # noqa: E402

    rng = random.Random(123)
    turns = _make_turns(rng, 200)
    queries = _queries(random.Random(321), 30)

    ref = _REF_HISTORY_INDEX()
    ref.index_messages([
        Message(role=role, content=[TextPart(text=text)])
        for _tid, _ts, role, _comp, text in turns
    ])

    native = index.HistoryIndex()
    native.append_turns(turns)

    for q in queries:
        rr = ref.search(q, 5)
        rn = native.search(q, 5)
        assert [t["turn_id"] for t in rr] == [t["turn_id"] for t in rn], q
        for a, b in zip(rr, rn):
            assert abs(a["score"] - b["score"]) < 1e-9
