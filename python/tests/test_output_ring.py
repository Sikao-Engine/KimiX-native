"""tools.OutputRing — the bounded background-job output buffer (report C1).

``bounded_append(content, text, cap)`` is the stateless kernel the reference
call site uses; ``OutputRing`` is the stateful form a live job owns. This module
pins them against each other (the kernel chain IS the reference semantics, and
``test_parity_job_output.py`` pins that chain against kimi-agent's own
``background/utils.py`` when the sibling checkout is importable) plus the
Python mirror used when ``KIMIX_NATIVE_TOOLS=0``.
"""

from __future__ import annotations

import random

import pytest

from kimix_native import tools


def chained_reference(chunks, cap: int) -> str:
    content = ""
    for chunk in chunks:
        content, _truncated = tools.bounded_append(content, chunk, cap)
    return content


def test_ring_matches_bounded_append_chain():
    rng = random.Random(7)
    alphabet = ["a", "b", "\n", "日", "🙂", "x" * 7]
    for _ in range(40):
        cap = rng.choice([10, 37, 100, 4096])
        chunks = [
            "".join(rng.choice(alphabet) for _ in range(rng.randrange(0, 30)))
            for _ in range(rng.randrange(1, 8))
        ]
        expected = chained_reference(chunks, cap)
        ring = tools.OutputRing(cap)
        compat = tools._compat_OutputRing(cap)
        for chunk in chunks:
            assert ring.append(chunk) == compat.append(chunk)
        assert ring.snapshot() == expected
        assert compat.snapshot() == expected
        assert ring.char_count() == len(expected)
        assert ring.byte_count() == len(expected.encode("utf-8", "surrogatepass"))


def test_ring_fast_path_and_truncation_flag():
    ring = tools.OutputRing(100)
    assert ring.append("hello ") is False
    assert ring.append("world") is False
    assert ring.truncated() is False
    assert ring.snapshot() == "hello world"
    big = "z" * 200
    assert ring.append(big) is True
    assert ring.truncated() is True
    assert "output truncated" in ring.snapshot()


def test_ring_read_slices():
    ring = tools.OutputRing(64)
    ring.append("0123456789" * 20)
    text = ring.snapshot()
    assert ring.read(0, 10) == text[:10]
    assert ring.read(10, 25) == text[10:35]
    assert ring.read(len(text) + 5, 10) == ""
    assert ring.read(0, 0) == ""


def test_ring_clear_and_capacity():
    ring = tools.OutputRing(8)
    assert ring.cap_chars() == 8
    ring.append("0123456789")
    assert ring.truncated() is True
    ring.clear()
    assert ring.snapshot() == ""
    assert ring.char_count() == 0
    assert len(ring) == 0
    assert ring.bytes_owned() >= 0


def test_ring_multibyte_boundaries():
    ring = tools.OutputRing(8)
    for _ in range(4):
        ring.append("😀")
    text = ring.snapshot()
    for offset in range(ring.char_count()):
        one = ring.read(offset, 1)
        assert len(one) == 1 or one == ""


def test_ring_fallback_matches_native(monkeypatch):
    chunks = ["alpha", "beta", "gamma", "日" * 30, "tail"]
    native = tools.OutputRing(40)
    for chunk in chunks:
        native.append(chunk)
    monkeypatch.setenv("KIMIX_NATIVE_TOOLS", "0")
    fallback = tools.OutputRing(40)
    for chunk in chunks:
        fallback.append(chunk)
    assert fallback.snapshot() == native.snapshot()
    assert fallback.bytes_owned() == 0
