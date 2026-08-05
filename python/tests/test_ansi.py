"""Parity tests for kimix_native.stream ANSI strip (plan 003).

Compares the native stripper against the pure-Python reference
``_compat_filter_output`` (mirror of tools/common.py) on:
- hand-built vectors: OSC hyperlinks, SGR, cursor moves, DCS/PM/APC, Fe,
  partial-escape-at-eof, CRLF/CR mixes
- a 200-byte sample split at EVERY byte offset (feed at split, feed rest,
  flush) == one-shot, via the native LineProcessor
- byte-by-byte feeding == one-shot
- the KIMIX_NATIVE_STREAM=0 fallback toggle
"""

import random

import pytest

import kimix_native
from kimix_native import stream

ESC = "\x1b"

# Corpus of ANSI-heavy samples (mirrors the kimi-agent `regex` behavior).
ANSI_CORPUS = [
    f"{ESC}[31mred{ESC}[0m",
    f"{ESC}]0;title\x07hello",
    f"{ESC}]0;title{ESC}\\hello",
    f"{ESC}Pabc\x07x",
    f"{ESC}Pabc{ESC}[31mx",
    f"{ESC}_ab\x07",
    f"{ESC}_",
    f"{ESC}P",
    f"{ESC}^",
    f"{ESC}]abc",
    f"{ESC}]a{ESC}[0m",
    f"{ESC}]a{ESC}{ESC}\\",
    f"{ESC}[1 2m",
    f"{ESC}[31m",
    f"{ESC}[?25l",
    f"{ESC}[2J{ESC}[H",
    f"{ESC}(0",
    f"{ESC}M",
    f"{ESC}-",
    f"{ESC}\\",
    f"{ESC}8",
    f"{ESC}7",
    f"{ESC}=",
    "plain text",
    f"{ESC}[31",
    f"{ESC}",
    f"{ESC}[31m{ESC}[0m{ESC}]11;rgb:ff/00/00\x07end",
    f"a{ESC}]0;title\x07b{ESC}Pq{ESC}\\c",
    f"{ESC}]8;;http://x{ESC}\\link{ESC}]8;;{ESC}\\",
    f"{ESC}[3",
    f"x{ESC}[31;1m{ESC}[Kbold{ESC}[0m",
    f"{ESC}]0;\x07",
    f"{ESC}P{ESC}\\",
    "line1\r\nline2\rline3\n",
    f"pre{ESC}[31m{ESC}[0m post",
    "\r\n\r\n",
    "",
    f"{ESC}[38;2;255;0;0mcolor{ESC}[0m",
    f"{ESC}]0;this {ESC} is not terminated\x07end",
]


def test_corpus_strip_parity():
    for s in ANSI_CORPUS:
        native = stream.strip_ansi(s)
        compat = stream._compat_filter_output(s)
        # strip_ansi does NOT do CRLF normalization; compare against the
        # regex-sub half of filter_output only when no \r is present.
        if "\r" not in s:
            assert native == compat, repr(s)


def test_corpus_filter_output_parity():
    for s in ANSI_CORPUS:
        native = stream.filter_output(s)
        compat = stream._compat_filter_output(s)
        assert native == compat, repr(s)


def test_filter_output_crlf():
    assert stream.filter_output("a\r\nb\rc\nd") == "a\nb\nc\nd"
    assert stream.filter_output("tail\r") == "tail\n"
    assert stream.filter_output("a\r\rb") == "a\n\nb"
    assert stream.filter_output("a\r\n\r\nb") == "a\n\nb"
    assert stream.filter_output(f"{ESC}[31mred{ESC}[0m\r\nnext") == "red\nnext"


def _oneshot_native(s):
    lp = stream.LineProcessor(strip_ansi=True, dedup_mode=0)
    out = lp.feed(s.encode("utf-8"))
    out += lp.flush()
    return "\n".join(b if isinstance(b, str) else b.decode("utf-8") for b in out)


def test_every_byte_offset_split():
    # 200-byte sample containing every escape family.
    sample = (
        f"line1{ESC}[31mred{ESC}[0m\n"
        f"{ESC}]0;title\x07osc-bel\n"
        f"{ESC}]0;title{ESC}\\osc-st\n"
        f"{ESC}Pdcs\x07dcs\n"
        f"{ESC}Pfall{ESC}[32mdcs-fall\n"
        f"{ESC}_apc\x07apc\n"
        f"{ESC}Mfe\n"
        f"{ESC}[?25lhide{ESC}[?25hshow\n"
        f"{ESC}[1 2mkept{ESC}[2J\n"
        "partial " + f"{ESC}[3" + " at eof"
    )
    while len(sample) < 200:
        sample += f"padding {ESC}[0m "
    one_shot = _oneshot_native(sample)
    assert one_shot
    data = sample.encode("utf-8")
    for split in range(len(data) + 1):
        lp = stream.LineProcessor(strip_ansi=True, dedup_mode=0)
        out = lp.feed(data[:split])
        out += lp.feed(data[split:])
        out += lp.flush()
        joined = "\n".join(out)  # shim returns str lines
        assert joined == one_shot, f"split at byte {split}"


def test_byte_by_byte_feed():
    sample = f"abc{ESC}[31mdef{ESC}]0;t\x07ghi{ESC}Pq{ESC}\\jkl{ESC}"
    data = sample.encode("utf-8")
    lp = stream.LineProcessor(strip_ansi=True, dedup_mode=0)
    out = []
    for i in range(len(data)):
        out += lp.feed(data[i : i + 1])
    out += lp.flush()
    joined = "".join(out)  # shim returns str lines
    assert joined == stream._compat_filter_output(sample)


def test_random_corpus_parity():
    rng = random.Random(42)
    esc_fragments = [
        f"{ESC}[31m", f"{ESC}[0m", f"{ESC}[?25l", f"{ESC}[?25h", f"{ESC}[2J",
        f"{ESC}]0;x\x07", f"{ESC}]0;x{ESC}\\", f"{ESC}P", f"{ESC}_", f"{ESC}M",
        f"{ESC}", f"{ESC}[", f"{ESC}[3", f"{ESC}[1 2", f"{ESC}]", f"{ESC}^",
    ]
    plain = ["text ", "line\n", "data ", "\r\n", "\r", "你 ", "😀 ", "\x07"]
    samples = []
    for _ in range(300):
        parts = []
        for _ in range(rng.randint(0, 12)):
            parts.append(rng.choice(esc_fragments) if rng.random() < 0.5 else rng.choice(plain))
        samples.append("".join(parts))
    for s in samples:
        assert stream.filter_output(s) == stream._compat_filter_output(s), repr(s[:80])
        # LineProcessor yields LINES (trailing newline dropped) — compare
        # against splitlines() of the reference filter_output.
        lp = stream.LineProcessor(strip_ansi=True, dedup_mode=0)
        out = lp.feed(s) + lp.flush()
        assert "\n".join(out) == "\n".join(stream._compat_filter_output(s).splitlines()), repr(s[:80])


def test_toggle_fallback(monkeypatch):
    monkeypatch.setenv("KIMIX_NATIVE_STREAM", "0")
    assert kimix_native.use_native("STREAM") is False
    for s in ANSI_CORPUS:
        assert stream.filter_output(s) == stream._compat_filter_output(s), repr(s)
        if "\r" not in s:
            assert stream.strip_ansi(s) == stream._compat_filter_output(s), repr(s)
    # LineProcessor falls back to _CompatLineProcessor
    lp = stream.LineProcessor(strip_ansi=True, dedup_mode=0)
    assert isinstance(lp._impl, stream._CompatLineProcessor)
    out = lp.feed(f"{ESC}[31mred{ESC}[0m\nnext") + lp.flush()
    assert out == ["red", "next"]
