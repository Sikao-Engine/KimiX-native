"""Parity tests for kimix_native.text token counting (plan 001).

Compares the native kernels against the pure-Python ``_compat`` mirrors
(which reproduce kimi_cli/utils/tokens.py exactly) on:
- golden vectors harvested from kimi-cli/tests/utils/test_tokens.py
- >= 200 property strings (ASCII, mixed, 7 CJK ranges, 4-byte CJK Ext B,
  emoji, empty; totals 1..2000 rounding cases)
- the KIMIX_NATIVE_TEXT=0 fallback toggle
"""

import os
import random

import pytest

import kimix_native
from kimix_native import text


# ---------------------------------------------------------------------------
# Golden vectors from kimi-cli/tests/utils/test_tokens.py
# ---------------------------------------------------------------------------

def test_golden_is_cjk_text():
    cases = [
        ("", False),
        ("Hello world", False),
        ("Hello world 世", False),          # 1 CJK / 14 chars ≈ 0.07
        ("Hello世界", True),                 # 2/7 ≈ 0.285 > 0.15
        ("你好世界", True),
        ("안녕하세요", True),                 # Korean (Hangul range)
        ("こんにちは", True),                 # Japanese (Hiragana range)
        ("カタカナ", True),                   # Katakana range
        ("ＡＢＣ", True),                     # Fullwidth range
    ]
    for s, expected in cases:
        assert text.is_cjk_text(s) is expected, repr(s)
        assert text._compat_is_cjk_text(s) is expected, repr(s)


def test_golden_estimate():
    assert text.estimate_chars_tokens("") == 0
    assert text.estimate_chars_tokens("a" * 400) == 100            # 400 // 4
    assert text.estimate_chars_tokens("你" * 300) == 100           # 300 // 3
    s = "def foo():\n    return '你好'"
    assert text.estimate_chars_tokens(s) == max(1, int(len(s) / 3.5))
    s = "a" * 96 + "你" * 4
    assert text.estimate_chars_tokens(s) == max(1, len(s) // 4)    # ratio 0.96
    s = "a" * 95 + "你" * 5
    assert text.estimate_chars_tokens(s) == max(1, int(len(s) / 3.5))  # ratio 0.95
    # English estimate within 5% of len//4 (backwards-compat test)
    s = "The quick brown fox jumps over the lazy dog. " * 20
    assert abs(text.count_tokens(s) - len(s) // 4) / (len(s) // 4) <= 0.05


def test_golden_count_tokens():
    assert text.count_tokens("") == 0
    assert text.count_tokens("a" * 400) == 100
    assert text.count_tokens("你" * 300) == 100
    assert text.count_tokens("hello world", model="unknown-model-xyz") > 0


# ---------------------------------------------------------------------------
# Property parity: native == _compat on a large corpus
# ---------------------------------------------------------------------------

_CJK_SAMPLES = [
    "你", "好", "世", "界", "中", "文",
    "안", "녕", "하", "세", "요",
    "こ", "ん", "に", "ち", "は",
    "カ", "タ", "カ", "ナ",
    "Ａ", "Ｂ", "Ｃ",
    "\U00020000",  # CJK Ext B (4-byte)
    "\U0002ebef",  # CJK Ext B upper bound
    "😀", "🙂", "🎌",  # emoji — non-CJK, non-ASCII
    "é", "ü", "ñ",  # Latin-1
    "中",  # boundary dup
]
_ASCII_SAMPLES = list("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 \t\n.,;:!?()[]{}\"'`~@#$%^&*_-+=|/\\<>")


def _random_corpus(rng, count=220):
    corpus = []
    while len(corpus) < count:
        n = rng.randint(0, 120)
        kind = rng.random()
        if kind < 0.30:
            chars = [rng.choice(_ASCII_SAMPLES) for _ in range(n)]
        elif kind < 0.55:
            chars = [rng.choice(_CJK_SAMPLES) for _ in range(n)]
        else:
            chars = [
                rng.choice(_CJK_SAMPLES if rng.random() < 0.5 else _ASCII_SAMPLES)
                for _ in range(n)
            ]
        corpus.append("".join(chars))
    # Rounding sweep over totals 1..2000 in the /3.5 branch: "é" + (n-1) 'a'.
    corpus.append("")
    for n in range(1, 2001):
        corpus.append("é" + "a" * (n - 1))
    return corpus


def test_property_parity_token_count():
    rng = random.Random(1234)
    corpus = _random_corpus(rng)
    assert len(corpus) >= 2200  # 220 random + 1 empty + 2000 rounding
    mismatches = 0
    for s in corpus:
        native = text.estimate_chars_tokens(s)
        compat = text._compat_estimate(s)
        assert native == compat, repr(s[:60])
    # is_cjk_text parity
    for s in corpus:
        assert text.is_cjk_text(s) == text._compat_is_cjk_text(s), repr(s[:60])


def test_native_count_tokens_matches_compat():
    rng = random.Random(99)
    corpus = _random_corpus(rng, count=200)
    for s in corpus:
        assert text.count_tokens(s) == text._compat_estimate(s), repr(s[:60])


@pytest.mark.skipif(kimix_native._native is None,
                    reason="requires the native extension")
def test_scan_utf8_matches_python():
    rng = random.Random(7)
    corpus = _random_corpus(rng, count=150)
    for s in corpus:
        payload = s.encode("utf-8")
        cps, ascii_count = kimix_native._native.text.scan_utf8(payload)
        assert cps == len(s)
        assert ascii_count == sum(1 for c in s if ord(c) < 128)
        assert kimix_native._native.text.estimate_chars_tokens(payload) == text._compat_estimate(s)


# ---------------------------------------------------------------------------
# Toggle: KIMIX_NATIVE_TEXT=0 falls back to _compat with identical results
# ---------------------------------------------------------------------------

def test_toggle_fallback(monkeypatch):
    monkeypatch.setenv("KIMIX_NATIVE_TEXT", "0")
    assert kimix_native.use_native("TEXT") is False
    for s in ["hello world", "你好世界", "a" * 95 + "你" * 5, "", "😀" * 10]:
        assert text.count_tokens(s) == text._compat_estimate(s)
        assert text.estimate_chars_tokens(s) == text._compat_estimate(s)


def test_toggle_off_globally():
    # KIMIX_NATIVE=0 is honored at IMPORT time (the shim captures it when
    # deciding whether to load the compiled module) — spawn a subprocess.
    import subprocess
    import sys

    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    env = dict(os.environ)
    env["KIMIX_NATIVE"] = "0"
    code = (
        "import sys; sys.path.insert(0, {py!r}); "
        "from kimix_native import text; "
        "assert text.count_tokens('a' * 400) == 100; "
        "print('fallback-ok')"
    ).format(py=os.path.join(root, "python"))
    proc = subprocess.run(
        [sys.executable, "-c", code], cwd=root, env=env,
        capture_output=True, text=True, timeout=60,
    )
    assert proc.returncode == 0, proc.stderr
    assert "fallback-ok" in proc.stdout
