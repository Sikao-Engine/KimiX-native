"""Parity tests for kimix_native.text sanitizer (plan 002).

Compares the native kernels against the pure-Python ``_compat`` mirrors
(which reproduce kimi_cli/safety_check.py exactly) on:
- hand-built vectors per pipeline step (surrogates, FDD0/FDEF, FFFE/FFFF,
  PUA planes, U+FFFD runs, zero-width set, C0/C1, NFC pairs e\\u0301,
  "A"*10000 collapse, truncation with/without msg, strip set, keep_newlines)
- >= 100 property cases incl. lone surrogates (surrogatepass encode)
- the KIMIX_NATIVE_TEXT=0 fallback toggle
"""

import random

import pytest

import kimix_native
from kimix_native import text


# ---------------------------------------------------------------------------
# Hand-built vectors per pipeline step
# ---------------------------------------------------------------------------

def test_surrogates_removed():
    cases = [
        "a\ud800b",
        "\udfff",
        "\ud800\udc00",  # surrogate pair halves (both invalid scalars)
        "x\ud800y\udfffb",
        "\ud800" * 10,
    ]
    for s in cases:
        assert text.sanitize_for_tokenizer(s) == text._compat_sanitize_for_tokenizer(s), repr(s)
        assert "\ud800" not in text.sanitize_for_tokenizer(s)
        assert "\udfff" not in text.sanitize_for_tokenizer(s)


def test_noncharacters_removed():
    cases = [
        "a\ufdd0b\ufdefc",
        "x\ufffey\uffffz",
        "\U0001fffe\U0001ffff",
        "\U00010fffe\U00010ffff",
        "\ufdd0",
        "ok\ufdcf\ufe00",  # NOT noncharacters -> kept
    ]
    for s in cases:
        assert text.sanitize_for_tokenizer(s) == text._compat_sanitize_for_tokenizer(s), repr(s)
    assert text.sanitize_for_tokenizer("a\ufdd0b") == "ab"
    assert text.sanitize_for_tokenizer("ok\ufdcf\ufe00") == "ok\ufdcf\ufe00"


def test_pua_removed():
    cases = [
        "a\ue000b\uf8ffc",
        "\U000f0000\U000ffffd",
        "\U00100000\U0010fffd",
        "\uf900",  # CJK compat ideograph — NOT PUA, kept
    ]
    for s in cases:
        assert text.sanitize_for_tokenizer(s) == text._compat_sanitize_for_tokenizer(s), repr(s)
    assert text.sanitize_for_tokenizer("a\ue000b") == "ab"
    # U+F900 is kept (not PUA), but NFC normalizes it to U+8C48 (compat ideograph).
    assert text.sanitize_for_tokenizer("\uf900") == "\u8c48"


def test_replacement_chars_removed():
    s = "a\ufffd\ufffdb\ufffd"
    assert text.sanitize_for_tokenizer(s) == "ab"
    assert text.sanitize_for_tokenizer(s) == text._compat_sanitize_for_tokenizer(s)


def test_zero_width_and_controls():
    zw = "\u200b\u200c\u200d\u2060\u00ad\ufeff\u200e\u200f\u202a\u202b\u202c\u202d\u202e\u2066\u2067\u2068\u2069"
    assert text.sanitize_for_tokenizer("a" + zw + "b") == "ab"
    ctl = "\x00\x01\x08\x0b\x0c\x0e\x1f\x7f\x80\x9f"
    assert text.sanitize_for_tokenizer("a" + ctl + "\nb\rc\td") == "a\nb\rc\td"
    # clean_text keep_newlines=False removes \n \r \t
    assert text.clean_text("a\nb\rc\td", keep_newlines=False) == "abcd"
    assert text.clean_text("a\nb\rc\td", keep_newlines=True) == "a\nb\rc\td"


def test_nfc_composing_pairs():
    # NFC hook: e\u0301 -> é (native path applies NFC between pre and post).
    s = "e\u0301" * 10
    assert text.sanitize_for_tokenizer(s) == "é" * 10
    assert text.sanitize_for_tokenizer(s) == text._compat_sanitize_for_tokenizer(s)
    assert text.clean_text(s) == "é" * 10
    # NFC with truncation (NFC changes code-point counts -> order matters)
    s2 = "e\u0301" * 50
    assert text.sanitize_for_tokenizer(s2, max_chars=75) == text._compat_sanitize_for_tokenizer(
        s2, max_chars=75
    )
    # ASCII fast path must not call NFC (result identical anyway)
    assert text.sanitize_for_tokenizer("hello world") == "hello world"


def test_dedupe_runs():
    assert text.sanitize_for_tokenizer("A" * 10000) == "A" * 100
    assert text.sanitize_for_tokenizer("A" * 100) == "A" * 100
    assert text.sanitize_for_tokenizer("A" * 101) == "A" * 100
    assert text.sanitize_for_tokenizer("aaabbbc") == "aaabbbc"
    assert text.sanitize_for_tokenizer("aaabbbc", max_repeat=1) == "abc"
    assert text.sanitize_for_tokenizer("aaabbbc", max_repeat=2) == "aabbc"
    # max_repeat=0 disables
    assert text.sanitize_for_tokenizer("B" * 500, max_repeat=0) == "B" * 500
    # CJK run collapse
    assert text.sanitize_for_tokenizer("你" * 250) == "你" * 100


def test_truncation():
    assert text.sanitize_for_tokenizer("hello") == "hello"
    assert text.sanitize_for_tokenizer("hello", max_chars=3) == "hel"
    assert text.sanitize_for_tokenizer("hello world", max_chars=5, truncate_msg="...") == "he..."
    # msg len == max_chars -> dropped
    assert text.sanitize_for_tokenizer("hello", max_chars=3, truncate_msg="abc") == "hel"
    # msg longer than max_chars -> dropped
    assert text.sanitize_for_tokenizer("hello", max_chars=2, truncate_msg="long") == "he"
    # truncation counts code points (4-byte CJK)
    s = "\U00020000\U00020001\U00020002\U00020003"
    assert text.sanitize_for_tokenizer(s, max_chars=3) == "\U00020000\U00020001\U00020002"
    # strip happens before truncation
    assert text.sanitize_for_tokenizer("  hi there  ", max_chars=3) == "hi "


def test_strip_set():
    ws = "\u3000\u00a0\u2028\u2029\u2007\u205f\u1680\u0085"
    assert text.sanitize_for_tokenizer(ws + "hi" + ws) == "hi"
    assert text.sanitize_for_tokenizer(ws) == ""
    assert text.sanitize_for_tokenizer(" \t\n\v\f\r\x1c\x1d\x1e\x1fx \n") == "x"


def test_clean_text_matches_compat():
    cases = [
        "\u200bhello\u200b",
        "a\u00adb",
        "\ufeffleading",
        "mixed 中\u200d文",
        "  spaced  ",
        "e\u0301",
        "\u200b\u200b",
        "",
        "tab\there",
    ]
    for s in cases:
        assert text.clean_text(s) == text._compat_clean_text(s), repr(s)
        assert text.clean_text(s, keep_newlines=False) == text._compat_clean_text(
            s, keep_newlines=False
        ), repr(s)


# ---------------------------------------------------------------------------
# Property parity (>= 100 cases, incl. lone surrogates)
# ---------------------------------------------------------------------------

def _random_corpus(rng, count=120):
    pool_ascii = "abc XYZ012 \t\n.,;"
    pool_nonascii = ["é", "中", "你", "😀", "\u200b", "\u00ad", "\ufeff", "\ue000",
                     "\ufffd", "\ufdd0", "\uffff", "\u0301", "e", "\u2028", "\u00a0"]
    corpus = []
    while len(corpus) < count:
        n = rng.randint(0, 60)
        s = []
        for _ in range(n):
            if rng.random() < 0.6:
                s.append(rng.choice(pool_ascii))
            else:
                s.append(rng.choice(pool_nonascii))
            if rng.random() < 0.08:
                s.append("\ud800")  # lone surrogate
        corpus.append("".join(s))
    corpus += ["A" * n for n in (1, 99, 100, 101, 500, 10000)]
    corpus += ["e\u0301" * n for n in (1, 5, 50)]
    corpus += ["\u200b" * n for n in (1, 10)]
    return corpus


@pytest.mark.parametrize("seed", [1, 2, 3])
def test_property_parity_sanitize(seed):
    rng = random.Random(seed)
    corpus = _random_corpus(rng)
    assert len(corpus) >= 120
    for s in corpus:
        for kwargs in ({}, {"max_chars": 5}, {"max_repeat": 3}, {"max_chars": 10, "truncate_msg": "..."}):
            native = text.sanitize_for_tokenizer(s, **kwargs)
            compat = text._compat_sanitize_for_tokenizer(s, **kwargs)
            assert native == compat, f"{repr(s[:50])} kwargs={kwargs}"


@pytest.mark.parametrize("seed", [4, 5])
def test_property_parity_clean_text(seed):
    rng = random.Random(seed)
    corpus = _random_corpus(rng, count=100)
    for s in corpus:
        assert text.clean_text(s) == text._compat_clean_text(s), repr(s[:50])
        assert text.clean_text(s, keep_newlines=False) == text._compat_clean_text(
            s, keep_newlines=False
        ), repr(s[:50])


# ---------------------------------------------------------------------------
# Toggle
# ---------------------------------------------------------------------------

def test_toggle_fallback(monkeypatch):
    monkeypatch.setenv("KIMIX_NATIVE_TEXT", "0")
    assert kimix_native.use_native("TEXT") is False
    for s in ["hello world", "你好世界", "A" * 1000, "e\u0301", "\ud800x", "\u200b y \u200b"]:
        assert text.sanitize_for_tokenizer(s) == text._compat_sanitize_for_tokenizer(s), repr(s)
        assert text.clean_text(s) == text._compat_clean_text(s), repr(s)
