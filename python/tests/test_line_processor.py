"""Parity tests for kimix_native.stream.LineProcessor (plan 003).

Compares the native LineProcessor against:
- the pure-Python reference ``_compat_dedup_output`` (mirror of
  tools/common.py::_dedup_output) on dedup golden vectors (counter + block)
- the ``_CompatLineProcessor`` fallback on fold / budgets / accounting
- feed/flush chunked == one-shot on a 5 MB synthetic stream (1000 lines)
- UTF-8 accounting with 4-byte chars
"""

import random

import pytest

import kimix_native
from kimix_native import stream


def _run_native(text, chunk_size=None, **kwargs):
    lp = stream.LineProcessor(**kwargs)
    data = text.encode("utf-8") if isinstance(text, str) else text
    if chunk_size is None:
        chunk_size = max(1, len(data))
    out = []
    for i in range(0, len(data), chunk_size):
        out += lp.feed(data[i : i + chunk_size])
    out += lp.flush()
    return list(out)  # shim feed/flush already return str lines


def _run_compat(text, **kwargs):
    lp = stream._CompatLineProcessor(**kwargs)
    data = text.encode("utf-8") if isinstance(text, str) else text
    lp.feed(data)
    return lp.flush()


# ---------------------------------------------------------------------------
# Dedup parity vs tools/common.py::_dedup_output
# ---------------------------------------------------------------------------

DEDUP_VECTORS = [
    # counter-mode golden vectors
    ("a\na\na\na\nb\na\n", dict(dedup_mode=1)),
    ("a\na\na\n", dict(dedup_mode=1)),
    ("err\nerr\n", dict(dedup_mode=1, threshold=1)),
    ("x\ny\nx\nz\n", dict(dedup_mode=1, threshold=1)),
    ("u\nv\nw\nu\nv\nw\n", dict(dedup_mode=1, threshold=1)),
    ("ERROR: timeout\nERROR: timeout\nERROR: timeout\nERROR: timeout\nok\n", dict(dedup_mode=1)),
    # block-mode golden vectors
    ("a\nb\na\nb\na\nb\n", dict(dedup_mode=2, threshold=1, block_window=2)),
    ("e\ne\ne\n", dict(dedup_mode=2, threshold=1, block_window=3)),
    ("x\ny\nx\ny\nx\ny\nx\ny\n", dict(dedup_mode=2, threshold=1, block_window=2)),
    ("u\nv\nw\nu\nv\nw\n", dict(dedup_mode=2, threshold=1, block_window=3)),
    ("a\nb\na\n", dict(dedup_mode=2, threshold=1, block_window=1)),
    # 3-line blocks
    ("p\nq\nr\np\nq\nr\np\nq\nr\n", dict(dedup_mode=2, threshold=1, block_window=3)),
    # mixed: unique + repeated block + unique
    ("head\na\nb\na\nb\na\nb\ntail\n", dict(dedup_mode=2, threshold=1, block_window=2)),
    # threshold boundaries
    ("z\nz\nz\n", dict(dedup_mode=2, threshold=3)),
    ("z\nz\nz\nz\n", dict(dedup_mode=2, threshold=3)),
    # CRLF input
    ("a\r\na\r\na\r\na\r\nb\r\n", dict(dedup_mode=1)),
]


@pytest.mark.parametrize("text,kwargs", DEDUP_VECTORS)
def test_dedup_parity_with_reference(text, kwargs):
    threshold = kwargs.get("threshold", 3)
    block = kwargs.get("block_window", 1 if kwargs["dedup_mode"] == 1 else 3)
    expected = stream._compat_dedup_output(text, threshold, max_block_lines=block)
    native = "\n".join(_run_native(text, **kwargs))
    assert native == expected, f"{text!r} kwargs={kwargs}"
    # chunked == one-shot (byte-by-byte chunks)
    assert _run_native(text, chunk_size=1, **kwargs) == _run_native(text, **kwargs)


def test_dedup_matches_compat_processor():
    rng = random.Random(5)
    lines_pool = ["a", "b", "c", "ERROR", "ok", "x" * 40, ""]
    for _ in range(60):
        n = rng.randint(1, 60)
        lines = [rng.choice(lines_pool) for _ in range(n)]
        text = "\n".join(lines) + "\n"
        for kwargs in (
            {"dedup_mode": 1},
            {"dedup_mode": 1, "threshold": 1},
            {"dedup_mode": 2, "threshold": 1, "block_window": 2},
            {"dedup_mode": 2, "threshold": 2, "block_window": 3},
        ):
            native = "\n".join(_run_native(text, **kwargs))
            compat = "\n".join(_run_compat(text, **kwargs))
            assert native == compat, f"{text!r} kwargs={kwargs}"


# ---------------------------------------------------------------------------
# Fold / budgets / accounting
# ---------------------------------------------------------------------------

def test_fold():
    # native vs compat fold semantics
    assert _run_native("abcdef\n", fold_col=3) == ["abc", "def"]
    assert _run_compat("abcdef\n", fold_col=3) == ["abc", "def"]
    # 4-byte chars never split
    s = "😀😀😀\n"
    assert _run_native(s, fold_col=2) == ["😀😀", "😀"]
    assert _run_native(s, fold_col=2) == _run_compat(s, fold_col=2)
    # short line unchanged; empty line preserved
    assert _run_native("ab\n", fold_col=3) == ["ab"]
    assert _run_native("a\n\nb\n", fold_col=1) == ["a", "", "b"]
    # folding before/after dedup: dedup sees raw lines, then the ANNOTATION
    # line is folded too (fold is content-preserving)
    text = "zz\n" * 5
    native = _run_native(text, dedup_mode=1, threshold=1, fold_col=1)
    compat = _run_compat(text, dedup_mode=1, threshold=1, fold_col=1)
    assert native == compat
    assert "".join(native) == "zz  (5 repeats)"  # line is "zz"


def test_budgets():
    # max_lines truncation at line boundaries
    assert _run_native("a\nb\nc\nd\n", max_lines=2) == ["a", "b"]
    assert _run_native("a\nb\nc\nd\n", max_lines=2) == _run_compat("a\nb\nc\nd\n", max_lines=2)
    # max_bytes: 6 bytes -> "aaa"(3) + "bb"(2) fit; "ccc"(3) does not
    assert _run_native("aaa\nbb\nccc\n", max_bytes=6) == ["aaa", "bb"]
    assert _run_native("aaa\nbb\nccc\n", max_bytes=6) == _run_compat("aaa\nbb\nccc\n", max_bytes=6)
    # byte budget with 4-byte UTF-8 (each 😀 = 4 bytes)
    assert _run_native("😀\n😀\n😀\n", max_bytes=8) == ["😀", "😀"]
    assert _run_native("😀\n😀\n😀\n", max_bytes=8) == _run_compat("😀\n😀\n😀\n", max_bytes=8)


def test_accounting():
    lp = stream.LineProcessor(strip_ansi=True, dedup_mode=0)
    lp.feed("\x1b[31mred\x1b[0m\n")
    lp.feed("héllo\n")  # 5 code points, 6 bytes (no terminator)
    lp.flush()
    assert lp.bytes_written() == 9
    assert lp.code_points_written() == 8
    assert lp.lines_written() == 2
    lp.reset()
    assert lp.bytes_written() == 0
    assert lp.code_points_written() == 0
    assert lp.lines_written() == 0
    # compat counters match
    c = stream._CompatLineProcessor(strip_ansi=True)
    c.feed("red\n")
    c.feed("héllo\n")
    c.flush()
    assert c.bytes_written() == 9
    assert c.code_points_written() == 8
    assert c.lines_written() == 2


def test_strip_ansi_line_processor_equals_filter_output():
    samples = [
        f"a\x1b[31mb\x1b[0m\nc\r\nd\re\n",
        "plain\nlines\n",
        f"\x1b]0;t\x07title\x1b[0m\n",
    ]
    for s in samples:
        lines = _run_native(s)
        # LineProcessor yields LINES (trailing newline dropped) — compare
        # against splitlines() of the reference filter_output.
        assert "\n".join(lines) == "\n".join(stream._compat_filter_output(s).splitlines()), repr(s)
        assert lines == _run_compat(s)


# ---------------------------------------------------------------------------
# 5 MB stream: feed/flush chunked == one-shot
# ---------------------------------------------------------------------------

def test_5mb_chunked_equals_oneshot():
    k_lines = 1000
    parts = []
    for i in range(k_lines):
        parts.append(f"line-{i}: " + chr(ord("a") + i % 26) * 5000 + "\n")
    text = "".join(parts)
    assert len(text) > 5_000_000  # ~5 MB (line content is 5000 chars)
    for mode in (0, 1, 2):
        kwargs = {"dedup_mode": mode, "threshold": 3, "block_window": 3}
        one_shot = _run_native(text, **kwargs)
        for chunk in (1, 7, 4096, 65536):
            assert _run_native(text, chunk_size=chunk, **kwargs) == one_shot, (
                f"mode={mode} chunk={chunk}"
            )
        # dedup modes match the Python reference output on this stream
        if mode == 1:
            assert "\n".join(one_shot) == stream._compat_dedup_output(text, 3)


# ---------------------------------------------------------------------------
# Toggle
# ---------------------------------------------------------------------------

def test_toggle_fallback(monkeypatch):
    monkeypatch.setenv("KIMIX_NATIVE_STREAM", "0")
    assert kimix_native.use_native("STREAM") is False
    lp = stream.LineProcessor(strip_ansi=True, dedup_mode=1, threshold=1)
    assert isinstance(lp._impl, stream._CompatLineProcessor)
    out = lp.feed("a\na\na\nb\n") + lp.flush()
    assert out == ["a  (3 repeats)", "b"]
