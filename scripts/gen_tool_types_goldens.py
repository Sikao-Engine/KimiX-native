"""Regenerate tests/unit/builtin_tools/tool_types_goldens.inc.

Every golden in the generated file is produced by *running the reference
implementation* -- never by hand and never from the kimix-base mirrors:

* ``kimi_cli.tools.file.output_utils`` (the pure-Python source of truth for the
  shared line-stream helpers: ``truncate_line``, ``fold_lines``,
  ``dedup_lines``, ``parse_rtk_rg_output``);
* ``kimi_cli.tools.file.grep_local._join_with_byte_limit`` -- ``output_utils.py``
  has no byte-budget join; this is the function the name refers to.  ``glob.py``
  contains the same algorithm inline (lines 631-637) and the generator asserts
  the two agree on the whole corpus before emitting a row;
* CPython itself for everything UTF-8: ``bytes.decode("utf-8")`` raises
  ``UnicodeDecodeError``, whose ``reason`` / ``start`` the native
  ``utf8_strict_error`` must reproduce, and ``len(text)`` / per-code-point byte
  offsets come from real ``str`` objects.

Usage:
    python scripts/gen_tool_types_goldens.py            # rewrite the .inc
    python scripts/gen_tool_types_goldens.py --check    # fail when stale
    python scripts/gen_tool_types_goldens.py --reference C:/dev/kimi-agent

Encoding used by the .inc (mirrored by the readers in test_tool_types.cpp):
* every string field is a C++ string literal produced by ``cpp_lit``: only
  printable ASCII plus octal escapes (``\\012``), so no source-encoding or
  hex-escape-run hazard exists, and a literal with an embedded NUL still has the
  right length through ``sizeof(lit) - 1``;
* a *line list* field is the lines joined with ``\\x1f`` and a trailing
  ``\\x1f`` (so an empty field is the empty list and ``"\\x1f"`` is one empty
  line); the generators assert that no corpus line contains ``\\x1f``.
"""

from __future__ import annotations

import argparse
import os
import random
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "tests" / "unit" / "builtin_tools" / "tool_types_goldens.inc"
DEFAULT_REFERENCE = os.environ.get("KIMI_AGENT_ROOT", "C:/dev/kimi-agent")

#: Separator of the line-list fields (see the module docstring).
LINE_SEP = "\x1f"


# ---------------------------------------------------------------------------
# reference import
# ---------------------------------------------------------------------------
def load_reference(root: str):
    src = Path(root) / "kimi-cli" / "src"
    if not (src / "kimi_cli" / "tools" / "file" / "output_utils.py").is_file():
        raise SystemExit(f"kimi-agent checkout not found under {root!r}")
    sys.path.insert(0, str(src))
    from kimi_cli.tools.file import grep_local, output_utils  # noqa: PLC0415

    return grep_local, output_utils


# ---------------------------------------------------------------------------
# encoding helpers
# ---------------------------------------------------------------------------
def cpp_lit(text) -> str:
    """Emit a C++ string literal holding the exact bytes of `text`."""
    raw = text.encode("utf-8") if isinstance(text, str) else bytes(text)
    out = ['"']
    for b in raw:
        if b == 0x22:
            out.append('\\"')
        elif b == 0x5C:
            out.append("\\\\")
        elif 0x20 <= b < 0x7F:
            out.append(chr(b))
        else:
            out.append("\\%03o" % b)
    out.append('"')
    return "".join(out)


def enc_lines(lines) -> str:
    """Encode a line list: `sep.join(...)` with a trailing separator."""
    for line in lines:
        assert LINE_SEP not in line, f"corpus line contains {LINE_SEP!r}: {line!r}"
    return "".join(line + LINE_SEP for line in lines)


def jesc(text: str) -> str:
    """JSON string escaping, mirrored byte for byte by test_tool_types.cpp."""
    out = []
    for ch in text:
        b = ord(ch)
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif b < 0x20:
            out.append("\\u%04x" % b)
        else:
            out.append(ch)
    return "".join(out)


def fmt_opt(value):
    """`None` -> JSON null; ints/strings rendered with jesc."""
    if value is None:
        return "null"
    if isinstance(value, int):
        return str(value)
    return '"' + jesc(value) + '"'


# ---------------------------------------------------------------------------
# corpora
# ---------------------------------------------------------------------------
# Interesting bytes for the UTF-8 corpus: every behaviour class of the codec
# (ASCII, every lead range, every continuation range boundary, non-bytes).
INTERESTING = [
    0x00, 0x09, 0x20, 0x41, 0x7A, 0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF,
    0xC0, 0xC1, 0xC2, 0xDF, 0xE0, 0xE1, 0xEC, 0xED, 0xEE, 0xEF, 0xF0, 0xF1,
    0xF3, 0xF4, 0xF5, 0xFE, 0xFF,
]
# One representative lead per codec behaviour class.
LEADS = [0x00, 0x41, 0x7F, 0x80, 0x8F, 0xBF, 0xC0, 0xC1, 0xC2, 0xDF,
         0xE0, 0xE1, 0xEC, 0xED, 0xEE, 0xEF, 0xF0, 0xF1, 0xF3, 0xF4, 0xF5, 0xFF]
# The 9-value continuation-byte set used by the 4-byte matrix.
INTERESTING4 = [0x00, 0x41, 0x7F, 0x80, 0x90, 0xA0, 0xBF, 0xC0, 0xFF]


def utf8_corpus() -> list[bytes]:
    corpus: list[bytes] = []

    def add(b: bytes) -> None:
        corpus.append(b)

    # 1. the classic cases, spelled out (these are the ones a reader greps for).
    add(b"")
    for b in (b"\x80", b"\xbf", b"\xc0\x80", b"\xc1\xbf", b"\xf5\x80\x80\x80",
              b"\xf8\x88\x80\x80\x80", b"\xfe", b"\xff", b"\xc3", b"\xc3\x28",
              b"A\xc3\x28", b"\xe1\x80", b"\xe1\x80A", b"\xe0\xa0", b"\xe0\x80\x80",
              b"\xed\xa0", b"\xed\xa0\x80", b"\xed\xbf\xbf", b"\xee\x80\x80",
              b"\xf0\x90\x80", b"\xf0\x80\x80\x80", b"\xf4\x90\x80\x80",
              b"\xf0\x9f\x92", b"\xf0\x9f\x92A", b"\xef\xbf\xbf", b"\xf4\x8f\xbf\xbf",
              b"\xf0\x9f\x98\x80", b"\xe2\x86\x92", b"\xc3\xa9",
              b"ok\xe2\x86\x92ok", b"ab\xe0\x80\x80", b"\xe1\x80\xe1\x80",
              b"\xe1\x80\xc3", b"\xe0\xa0\x41", b"\xf0\x90\x41"):
        add(b)
    # 2. every single byte (covers all invalid-start and truncation-of-1 cases).
    for b in range(256):
        add(bytes([b]))
    # 3. every second byte for every representative lead (the truncated 2..4-byte
    #    sequences and the "invalid continuation byte" vs "unexpected end of
    #    data" split live here).
    for lead in LEADS:
        for second in range(256):
            add(bytes([lead, second]))
    # 4. the 3-byte matrix (lead x first continuation).
    for lead in INTERESTING:
        for b1 in INTERESTING:
            add(bytes([lead, b1]))
            add(bytes([lead, b1, 0x41]))
            add(bytes([lead, b1, 0xBF]))
    # 5. the 4-byte matrix (lead x continuation x continuation).  b1 is the
    #    9-value set (the 29-value sweep of section 4 already pins the first
    #    continuation byte of every lead), b2 the full range-of-interest.
    for lead in INTERESTING:
        for b1 in INTERESTING4:
            for b2 in INTERESTING4:
                add(bytes([lead, b1, b2]))
                add(bytes([lead, b1, b2, 0x41]))
    # 6. valid text carrying a bad byte in the middle (the error is NOT at 0, so
    #    the reported offset has to be right).
    for prefix in (b"a", b"abc", b"\xc3\xa9", b"\xe2\x86\x92", b"a\xc3\xa9\xe2\x86\x92"):
        for bad in (b"\x80", b"\xff", b"\xc3", b"\xe0\x80", b"\xe0\x80\x41",
                    b"\xf0\x9f\x92", b"\xf0\x9f\x92\x41", b"\xed\xa0", b"\xed\xa0\x80"):
            add(prefix + bad)
            add(prefix + bad + b"\xe2\x86\x92z")
    # 7. deterministic fuzz, biased to the interesting bytes.
    rng = random.Random(0x5EED_2026)
    for _ in range(4000):
        skew = rng.random() < 0.5
        n = rng.randint(0, 8)
        if skew:
            add(bytes(rng.choice(INTERESTING) for _ in range(n)))
        else:
            add(bytes(rng.randrange(256) for _ in range(n)))
    # 8. random VALID sequences (astral included) so the accepting path is
    #    exercised over the whole code-point space.
    for _ in range(2000):
        parts = []
        for _ in range(rng.randint(0, 6)):
            while True:
                cp = rng.randrange(0, 0x110000)
                if not (0xD800 <= cp <= 0xDFFF):
                    break
            parts.append(chr(cp))
        add("".join(parts).encode("utf-8"))
    return corpus


def utf8_valid_corpus() -> list[str]:
    """Valid *text* used for the counting / offset / decode goldens."""
    corpus = ["", "a", "abc", "hello world", "h\u00e9llo", "\u2192",
              "\u2192\u2192\u2192", "\u4e2d\u6587", "\U0001f600", "a\U0001f600b",
              "a b\tc", "\u00ff\u0100\u07ff\u0800\uffff", "\U00010000\U0010ffff"]
    rng = random.Random(0xA11CE_2026)
    for _ in range(700):
        parts = []
        for _ in range(rng.randint(0, 8)):
            while True:
                cp = rng.randrange(0, 0x110000)
                if not (0xD800 <= cp <= 0xDFFF):
                    break
            parts.append(chr(cp))
        corpus.append("".join(parts))
    # ASCII-only and multi-byte-heavy mixes
    corpus.append("x" * 40)
    corpus.append("\u2192" * 30)
    return corpus


TRUNCATE_CORPUS = [
    ("", 0), ("", 5), ("a", 0), ("a", 1), ("a", 2),
    ("short", 5), ("short", 4), ("short", 10), ("short", 6),
    ("a" * 60, 20), ("a" * 30, 5), ("x" * 14, 14), ("x" * 15, 14),
    ("\u2192" * 10, 6), ("\u2192" * 10, 5), ("\u2192" * 10, 1),
    ("h\u00e9llo w\u00f6rld \U0001f600 \u4e2d\u6587", 8),
    ("h\u00e9llo w\u00f6rld \U0001f600 \u4e2d\u6587", 20),
    ("h\u00e9llo w\u00f6rld \U0001f600 \u4e2d\u6587", 0),
    ("h\u00e9llo w\u00f6rld \U0001f600 \u4e2d\u6587", 1),
    ("h\u00e9llo w\u00f6rld \U0001f600 \u4e2d\u6587", 5),
    ("h\u00e9llo w\u00f6rld \U0001f600 \u4e2d\u6587", 12),
    ("h\u00e9llo w\u00f6rld \U0001f600 \u4e2d\u6587", 13),
    ("h\u00e9llo w\u00f6rld \U0001f600 \u4e2d\u6587", 14),
    ("a" * 1000, 500), ("a" * 1000, 499), ("a" * 1000, 12), ("a" * 1000, 13),
    ("\U0001f600" * 200, 100), ("\U0001f600" * 200, 3), ("\U0001f600" * 200, 2),
    ("ab\tcd\nef", 4), ("ab\tcd\nef", 3), ("ab\tcd\nef", 2),
]
# 4-digit removed counts (a 3-digit "… [+K chars]" marker, 14 code points).
TRUNCATE_CORPUS += [("z" * n, m) for n in (5000, 1200, 1001) for m in (6, 10, 11, 12, 13, 20, 500)]


def fold_cases():
    """(lines, max_lines, head, tail) tuples; head/tail are what the C++ caller
    passes (glob_tool builds the Python defaults itself: head = max(1, ml // 2),
    tail = ml - head)."""
    cases = []
    line_sets = [
        [],
        ["only"],
        ["1", "2"],
        ["1", "2", "3"],
        ["1", "2", "3", "4", "5"],
        [str(i) for i in range(1, 11)],
        [str(i) for i in range(1, 26)],
        ["a" * 30, "", "  ", "b", "b", "b"] + [str(i) for i in range(20)],
        [f"line{i:03d}" for i in range(120)],
        [f"\u4e2d\u6587{i}" for i in range(9)],
        # kimi-agent tests/test_token_filter.py:131-171 folds a 1000-line result
        # with max_lines 100 / 50 / 3.
        [f"line_{i}" for i in range(1000)],
    ]
    for lines in line_sets:
        big = len(lines) > 200  # keep the 1000-line rows few (they are ~9 KB each)
        for ml in ((3, 50, 100, 200, 500) if big else (0, 1, 2, 3, 4, 5, 7, 20, 200, 500)):
            variants = set()
            if ml > 0:
                dflt = max(1, ml // 2)
                variants.add((dflt, ml - dflt))
                variants.add((ml, ml))
                variants.add((0, 0))
                variants.add((0, ml))
                variants.add((ml, 0))
                variants.add((1, 1))
                variants.add((ml - 1, ml - 1))
                variants.add((ml + 3, 0))
            else:
                variants.add((0, 0))
                variants.add((5, 5))
            for head, tail in sorted(variants):
                cases.append((lines, ml, max(0, head), max(0, tail)))
    return cases


def dedup_cases():
    cases = []
    line_sets = [
        [],
        ["solo"],
        ["a", "a"],
        ["a", "a", "a"],
        ["a", "b", "b", "b", "b", "c", "d", "d", "e"],
        ["a"] * 12,
        ["", "", "", "x", ""],
        ["  ", "  ", "x"],
        ["dup"] * 5 + ["other"] + ["dup"] * 5,
        [f"l{i % 3}" for i in range(40)],
    ]
    for lines in line_sets:
        for mr in (0, 1, 2, 3, 4, 5, 6, 9, 12, 100):
            cases.append((lines, mr))
    return cases


def join_cases():
    cases = []
    line_sets = [
        [],
        [""],
        ["", ""],
        ["", "x", ""],
        ["aaaa", "bbbb", "cccc", "dddd"],
        ["x"],
        ["x" * 20],
        ["abc", "def"],
        ["\u2192\u2192", "\u2192"],
        ["\u4e2d\u6587\u4e2d\u6587", "a"],
        [f"line{i}" for i in range(30)],
        ["a" * 100, "b" * 100, "c" * 100],
    ]
    for lines in line_sets:
        for mb in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14, 20, 100, 1024, 102400):
            cases.append((lines, mb))
    return cases


def rtk_cases():
    """(lines,) tuples for parse_rtk_rg_output; ASCII-only payloads (the native
    kernel gates non-ASCII protocol candidates to `unsupported`, which is
    asserted separately).

    The first block is *verbatim from kimi-agent's own test suite* -- the rtk
    fold protocol samples of tests/test_token_filter.py:812-843 and
    tests/test_tools_async.py:225-227 / 536-538."""
    header = "42 matches in 3 files:"
    return [
        # -- kimi-agent tests/test_token_filter.py + tests/test_tools_async.py --
        (["1 matches in 1 files:", "", "src/a.py:1:match",
          "  +5 more in src/b.py [see remaining: tail -n +2 /tmp/rtk.log]"],),
        (["10 matches in 15 files:", "", "src/a.py:1:match",
          "+14 more files [see remaining: tail -n +3 /tmp/rtk.log]"],),
        (["some [see remaining: tail -n +1 foo.log] text"],),
        (["3 matches in 2 files:", "", "src/a.py:1:match",
          "  +5 more in src/b.py [see remaining: tail -n +2 /tmp/rtk.log]"],),
        (["2 matches in 2 files:", "", "src/a.py:1:match",
          "  +5 more in src/b.py [see remaining: tail -n +2 /tmp/rtk.log]"],),
        (["plain output", "no folds here"],),
        # -- adversarial / structural cases --
        ([],),
        (["src/a.py:1:x"],),
        (["", "x", ""],),
        ([header],),
        ([header, ""],),
        ([header, "", "src/a.py:1:x"],),
        ([header, "src/a.py:1:x"],),
        (["  7 matches in 1 files:"],),
        (["7 matches in 1 files:"],),
        (["42 matches in 3 files extra"],),
        (["matches in 3 files:"],),
        (["  +37 more in src/a.py [see remaining: tail -n +26 C:/log/x.log]"],),
        (["+37 more in src/a.py [see remaining: tail -n +26 C:/log/x.log]"],),
        (["+133 more files [see remaining: tail -n +300 C:/log/x.log]"],),
        (["  +133 more files [see remaining: C:/log/x.log]"],),
        (["+133 more files [see remaining: ]"],),
        (["+0 more in p [see remaining: tail -n +1 log]"],),
        (["+3 more in path with spaces [see remaining: tail -n +2 my log]"],),
        (["+3 more in C:/x.py [see remaining: tail -n +2 C:/l.log] trailing]"],),
        ([header, "", "+5 more in b.py [see remaining: tail -n +9 L]",
          "+6 more in c.py [see remaining: tail -n +10 L]", "real:1:hit",
          "  +7 more files [see remaining: tail -n +300 L]"],),
        (["+2 more files [see remaining: tail -n +5 log]"],),
        (["  +2 more files [see remaining: no hint here]"],),
        (["+2 more files [see remaining: tail -n +5]"],),
        (["+2 more files"],),
        (["+2 more in a.py"],),
        (["+2 more in a.py [see remaining: ]"],),
        ([header, "", header, ""],),
        (["0 matches in 0 files:"],),
        (["000042 matches in 0003 files:"],),
        (["src/b.py-5-hit", "  +1 more in d.py [see remaining: tail -n +2 L]"],),
        (["\ttail -n +26 log"],),
        (["see remaining: x"],),
        (["+1 more in x.py [see remaining: tail -n +2 log]"],),
        (["\t+12 more in\t"],),
        (["+1 more in  [see remaining: tail -n +2 log]"],),
    ]


# ---------------------------------------------------------------------------
# JSON (ToolParams) fidelities: CPython's json module is the reference for what
# a well-formed tool-call payload means.  The native reader is yyjson with
# YYJSON_READ_NOFLAG, whose documented policy is:
#   * positive ints -> uint64, negative ints -> int64;
#   * an integer that fits neither is read as a *double*;
#   * "report error if double number is infinity"; strings must be valid UTF-8
#     (a lone \udXXX escape is an error).
# The canonical form below is language-agnostic (doubles compare by their IEEE
# bit pattern, object keys are sorted) so the C++ renderer can reproduce it
# byte for byte; every text is classified automatically, and a text that mixes
# two classification reasons is a hard generator error.
# ---------------------------------------------------------------------------
INT64_MIN = -(2**63)
UINT64_MAX = 2**64 - 1


def json_reasons(value):
    """Which representations the native ValueElement model cannot hold."""
    reasons = set()
    if value is None or isinstance(value, bool):
        return reasons
    if isinstance(value, int):
        if not (INT64_MIN <= value <= UINT64_MAX):
            reasons.add("lossy")  # yyjson reads it as a double
        return reasons
    if isinstance(value, float):
        if value != value or value in (float("inf"), float("-inf")):
            reasons.add("unrepresentable")  # yyjson rejects inf/nan
        return reasons
    if isinstance(value, str):
        try:
            value.encode("utf-8")
        except UnicodeEncodeError:
            reasons.add("unrepresentable")  # lone surrogate: not UTF-8
        return reasons
    if isinstance(value, list):
        for item in value:
            reasons |= json_reasons(item)
        return reasons
    if isinstance(value, dict):
        for key, item in value.items():
            reasons |= json_reasons(key)
            reasons |= json_reasons(item)
        return reasons
    raise TypeError(repr(value))


def json_canon(value) -> str:
    """Canonical rendering shared with the C++ renderer in test_tool.cpp."""
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return "i:" + str(value)
    if isinstance(value, float):
        import struct

        return "f:" + struct.pack(">d", value).hex()
    if isinstance(value, str):
        return "s:" + jesc(value)
    if isinstance(value, list):
        return "[" + ",".join(json_canon(v) for v in value) + "]"
    if isinstance(value, dict):
        items = [
            "k:" + jesc(str(k)) + "=" + json_canon(v)
            for k, v in sorted(value.items(), key=lambda kv: str(kv[0]))
        ]
        return "{" + ",".join(items) + "}"
    raise TypeError(repr(value))


def json_texts():
    """Candidate JSON texts; `classify_json_texts` decides the bucket, so a
    misunderstanding here cannot silently weaken an assertion."""
    deep = "1"
    for _ in range(64):
        deep = '{"a":%s}' % deep
    many = "{" + ",".join('"k%d":%d' % (i, i) for i in range(100)) + "}"
    parity = [
        "{}",
        '{"a":1}', '{"a":null}', '{"a":true}', '{"a":false}',
        '{"a":0}', '{"a":-0}', '{"a":-1}', '{"a":1.0}', '{"a":-1.0}',
        '{"a":0.0}', '{"a":-0.0}', '{"a":3.14}', '{"a":1e300}', '{"a":1e-300}',
        '{"a":5e-324}', '{"a":1.7976931348623157e308}', '{"a":1E2}',
        '{"a":2.5e-3}', '{"a":100.0}', '{"a":1.5e-8}', '{"a":0.1}',
        '{"a":1e-999}',   # underflows to 0.0 in both CPython and yyjson
        '{"a":9007199254740993}',   # 2^53 + 1: exact only as an integer
        '{"a":9223372036854775807}', '{"a":-9223372036854775808}',
        '{"a":18446744073709551615}', '{"a":12345678901234567890}',
        '{"a":""}', '{"a":"x"}', '{"a":"esc \\" \\\\ \\/ \\b \\f \\n \\r \\t"}',
        '{"a":"\\u0041\\u00e9\\u2192"}', '{"a":"\\ud83d\\ude00"}',
        '{"a":"\\ud83d\\ude00\\ud83d\\ude01"}', '{"a":"\\u0000"}',
        '{"a":"\\u0000b\\u0000"}', '{"a":"\\u4e2d\\u6587 \\u00e9 \\u2192 \\ud83d\\ude00"}',
        '{"a":[]}', '{"a":[1,2,3]}', '{"a":[[]]}', '{"a":[[1],[2,[3]]]}',
        '{"a":[{"b":1},{"c":[null,true,"x"]}]}', '{"a":[1,"2",3.0,null,false]}',
        '{"a":{"b":{"c":{"d":[1,{"e":"f"}]}}}}', deep,
        '{"a":1,"b":2,"c":3,"d":4}', '{"z":0,"a":1,"m":2}', many,
        '{"a":1,"a":2}',              # duplicate key: last one wins (Python dict)
        '{"a":1,"a":2,"a":3}',
        '{"\\u0000":1}', '{"a b":2}', '{"":3}', '{"\\u00e9":"x"}',
        '{"A":1,"a":2}',
        '  { "a" : 1 , "b" : [ 1 , 2 ] }  ', '{"a":1}\n', '\t{"a":1}\r\n',
        '{"a":"%s"}' % ("x" * 5000),
        '{"a":1,"b":{"c":[1,2,{"d":null}]},"e":"\\u00e9"}',
    ]
    reject = [
        "", " ", "[1,2,3]", '"str"', "42", "null", "true", "false",
        "{", '{"a":1', '{"a":1,}', '{"a" 1}', "{'a':1}", '{"a":+1}',
        '{"a":01}', '{"a":.5}', '{"a":1.}', '{"a":--1}', '{"a":1e}',
        '{"a":nul}', '{"a":tru}', '{"a":"unterminated}', '{"a":"\\q"}',
        '{"a":"\\u12"}', '{"a":"\\uZZZZ"}', '{"a":"\\ud83d"x}',
        '{"a":1} x', '{"a":1}{"b":2}', '{"a":1} /*c*/', '// c\n{"a":1}',
        '{a:1}', '[1,2', '{"a":[1,2}', '{"a":}',
        '{"a":"\\x41"}', '\ufeff{"a":1}',
        '{"a":1,,"b":2}', '{"a"::1}', '{,}', "{'a': 'b'}",
        '{"a":[,1]}', '{"a":[1,]}', '{"a" : }', '  ',
    ]
    unrepresentable = [
        '{"a":NaN}', '{"a":Infinity}', '{"a":-Infinity}', '{"a":1e999}',
        '{"a":-1e999}', '{"a":1e309}',
        '{"a":"\\ud800"}', '{"a":"\\udc00"}', '{"a":"\\ud800\\ud800"}',
        '{"a":"\\ud800x\\udc00"}', '{"a":"x\\udfff"}',
        '{"\\ud800":1}', '{"a":["\\ud800"]}', '{"a":{"b":"\\udc00"}}',
        '{"a":1e999,"b":2}',
    ]
    lossy = [
        '{"a":123456789012345678901234567890}',
        '{"a":-123456789012345678901234567890}',
        '{"a":18446744073709551616}',
        '{"a":-9223372036854775809}',
        '{"a":{"b":[99999999999999999999999999]}}',
        '{"a":-18446744073709551615}',
    ]
    return parity, reject, unrepresentable, lossy


def classify_json_texts():
    """Return the buckets with the reference value already rendered.

    Buckets:
      parity          both parsers accept and produce the same value
      reject          both parsers reject (malformed JSON / BOM / trailing data)
      nonobject       CPython accepts, the native reader rejects by contract
                      (ToolParams is a JSON *object* body)
      unrepresentable CPython accepts, the native reader rejects because the
                      value cannot be held (inf/nan literal, lone surrogate)
      lossy           CPython accepts, the native reader accepts it as a double
                      (integer outside [INT64_MIN, UINT64_MAX])
    """
    import json as _json

    parity, reject, unrepresentable, lossy = json_texts()
    out = {k: [] for k in ("parity", "reject", "nonobject", "unrepresentable", "lossy")}
    for text in parity + reject + unrepresentable + lossy:
        try:
            value = _json.loads(text)
        except ValueError:
            out["reject"].append(text)
            continue
        if not isinstance(value, dict):
            out["nonobject"].append((text, ascii(repr(value))))
            continue
        reasons = json_reasons(value)
        if not reasons:
            out["parity"].append((text, json_canon(value)))
            continue
        if len(reasons) > 1:
            raise SystemExit(f"mixed divergence reasons {reasons} for {text!r}")
        bucket = reasons.pop()
        if bucket == "lossy":
            # canon is UTF-8 safe here: a mix of reasons was rejected above.
            out["lossy"].append((text, json_canon(value), ascii(repr(value))))
        else:
            out[bucket].append((text, ascii(repr(value))))
    return out



# ---------------------------------------------------------------------------
# rendering
# ---------------------------------------------------------------------------
def render(grep_local, output_utils) -> str:
    L = []
    add = L.append

    add("// GENERATED by scripts/gen_tool_types_goldens.py - do not edit by hand.")
    add("//")
    add("// Golden vectors for the SHARED built-in tool infrastructure")
    add("// (builtin_tools/tool_types.h + utf8_util.h).  Produced by running the")
    add("// kimi-agent reference implementation / CPython itself:")
    add("//   * output_utils.truncate_line / fold_lines / dedup_lines /")
    add("//     parse_rtk_rg_output (kimi-cli/src/kimi_cli/tools/file/output_utils.py)")
    add("//   * grep_local._join_with_byte_limit (grep_local.py 618-632); the")
    add("//     generator asserts glob.py's identical inline loop agrees")
    add("//   * bytes.decode('utf-8') / len(str) for every UTF-8 vector")
    add("//")
    add("// Encoding: every string field is a C++ literal emitted by cpp_lit (printable")
    add("// ASCII + octal escapes), read with `sizeof(lit) - 1` so embedded NULs keep")
    add("// their length through tt_cstr().  A line list is `lines` joined with '\\x1f'")
    add("// plus a trailing '\\x1f' (empty == empty list).")
    add("//")
    add("// The file has two guarded halves so each test binary compiles only its own:")
    add("//   KIMIX_TT_GOLDEN_NO_LINE_VECTORS  skips utf8/line-stream vectors")
    add("//                                    (test_tool.cpp uses this)")
    add("//   KIMIX_TT_GOLDEN_NO_JSON_VECTORS  skips the ToolParams vectors")
    add("//                                    (test_tool_types.cpp uses this)")
    add("")
    add("#pragma once")
    add("")
    add("// Byte-exact string view over a generated literal (NUL-safe).")
    add("#ifndef KIMIX_TT_GOLDEN_HELPERS")
    add("#define KIMIX_TT_GOLDEN_HELPERS")
    add("template <size_t N>")
    add("constexpr kimix::string_view tt_cstr(const char (&lit)[N]) noexcept {")
    add("    return kimix::string_view(lit, N - 1u);")
    add("}")
    add("#endif")
    add("")

    add("#if !defined(KIMIX_TT_GOLDEN_NO_LINE_VECTORS)")
    add("")
    # ---- 1. UTF-8 decode errors (CPython UnicodeDecodeError) ----------------
    rows = []
    for data in utf8_corpus():
        try:
            data.decode("utf-8")
            ok, start, reason = True, 0, ""
        except UnicodeDecodeError as exc:
            ok, start, reason = False, exc.start, exc.reason
        rows.append((data, ok, start, reason))
    add("// CPython `bytes.decode('utf-8')`: ok / UnicodeDecodeError.start / .reason.")
    add("struct tt_utf8_error_golden {")
    add("    kimix::string_view data;")
    add("    bool ok;")
    add("    size_t offset;")
    add("    kimix::string_view reason;")
    add("};")
    add("static const tt_utf8_error_golden k_tt_utf8_error_golden[] = {")
    for data, ok, start, reason in rows:
        add("    {tt_cstr(%s), %s, %d, tt_cstr(%s)}," % (
            cpp_lit(data), "true" if ok else "false", start, cpp_lit(reason)))
    add("};")
    add("")

    # ---- 2. UTF-8 code-point counts / offsets / decoded code points --------
    valid = utf8_valid_corpus()
    add("// `text` is VALID UTF-8: code_points == len(text), cps_csv == every ord(),")
    add("// offsets_csv == utf8_byte_offset_of_code_point(text, k) for k = 0..cp+2")
    add("// (the last two probe the clamp past the end).")
    add("struct tt_utf8_valid_golden {")
    add("    kimix::string_view text;")
    add("    size_t code_points;")
    add("    kimix::string_view cps_csv;")
    add("    kimix::string_view offsets_csv;")
    add("};")
    add("static const tt_utf8_valid_golden k_tt_utf8_valid_golden[] = {")
    for text in valid:
        cps = [str(ord(c)) for c in text]
        offs = []
        for k in range(len(text) + 3):
            offs.append(str(len(text[:k].encode("utf-8"))))
        add("    {tt_cstr(%s), %d, tt_cstr(%s), tt_cstr(%s)}," % (
            cpp_lit(text), len(text), cpp_lit(",".join(cps)), cpp_lit(",".join(offs))))
    add("};")
    add("")

    # ---- 3. truncate_line --------------------------------------------------
    add("// output_utils.truncate_line(line, max_len) over the raw code points.")
    add("struct tt_truncate_golden {")
    add("    kimix::string_view text;")
    add("    size_t max_len;")
    add("    kimix::string_view want;")
    add("};")
    add("static const tt_truncate_golden k_tt_truncate_golden[] = {")
    for text, max_len in TRUNCATE_CORPUS:
        want = output_utils.truncate_line(text, max_len)
        add("    {tt_cstr(%s), %d, tt_cstr(%s)}," % (cpp_lit(text), max_len, cpp_lit(want)))
    add("};")
    add("")

    # ---- 4. fold_lines -----------------------------------------------------
    add("// output_utils.fold_lines(lines, max_lines, head=H, tail=T): the C++ port")
    add("// takes head/tail explicitly, so the golden calls pass them explicitly too.")
    add("// want is the folded line list; omitted is the second half of the tuple.")
    add("struct tt_fold_golden {")
    add("    kimix::string_view lines;")
    add("    size_t max_lines;")
    add("    size_t head;")
    add("    size_t tail;")
    add("    kimix::string_view want;")
    add("    size_t omitted;")
    add("};")
    add("static const tt_fold_golden k_tt_fold_golden[] = {")
    for lines, ml, head, tail in fold_cases():
        folded, omitted = output_utils.fold_lines(list(lines), ml, head=head, tail=tail)
        add("    {tt_cstr(%s), %d, %d, %d, tt_cstr(%s), %d}," % (
            cpp_lit(enc_lines(lines)), ml, head, tail, cpp_lit(enc_lines(folded)), omitted))
    add("};")
    add("")

    # ---- 5. dedup_lines ----------------------------------------------------
    add("// output_utils.dedup_lines(lines, min_repeats=m).")
    add("struct tt_dedup_golden {")
    add("    kimix::string_view lines;")
    add("    size_t min_repeats;")
    add("    kimix::string_view want;")
    add("    size_t saved;")
    add("};")
    add("static const tt_dedup_golden k_tt_dedup_golden[] = {")
    for lines, mr in dedup_cases():
        out, saved = output_utils.dedup_lines(list(lines), min_repeats=mr)
        add("    {tt_cstr(%s), %d, tt_cstr(%s), %d}," % (
            cpp_lit(enc_lines(lines)), mr, cpp_lit(enc_lines(out)), saved))
    add("};")
    add("")

    # ---- 6. join_with_byte_limit -------------------------------------------
    add("// grep_local._join_with_byte_limit(lines, max_bytes) -> (text, truncated);")
    add("// `omitted` is the C++ extension (lines left over after the crossing line).")
    add("struct tt_join_golden {")
    add("    kimix::string_view lines;")
    add("    size_t max_bytes;")
    add("    kimix::string_view want;")
    add("    bool truncated;")
    add("    size_t omitted;")
    add("};")
    add("static const tt_join_golden k_tt_join_golden[] = {")
    for lines, mb in join_cases():
        want, truncated = grep_local._join_with_byte_limit(list(lines), mb)
        # cross-check glob.py's inline loop (glob.py 631-637), same algorithm.
        acc: list[str] = []
        n_bytes = 0
        glob_truncated = False
        for line in lines:
            sep = 1 if acc else 0
            acc.append(line)
            n_bytes += sep + len(line.encode("utf-8"))
            if n_bytes >= mb:
                glob_truncated = True
                break
        assert (want, truncated) == ("\n".join(acc), glob_truncated), (lines, mb)
        omitted = len(lines) - len(acc)
        add("    {tt_cstr(%s), %d, tt_cstr(%s), %s, %d}," % (
            cpp_lit(enc_lines(lines)), mb, cpp_lit(want),
            "true" if truncated else "false", omitted))
    add("};")
    add("")

    # ---- 7. parse_rtk_rg_output --------------------------------------------
    add("// output_utils.parse_rtk_rg_output(lines) -> (cleaned, metadata).")
    add("// `folded_json` uses the same escaping as the reader in test_tool_types.cpp:")
    add("//   [{\"path\":\"..\",\"count\":N,\"log\":null|\"..\",\"start_line\":null|N},..]")
    add("// status is \"ok\", or \"unsupported\" when the native kernel declines the")
    add("// input (non-ASCII protocol candidate) and Python has to take over.")
    add("struct tt_rtk_golden {")
    add("    kimix::string_view lines;")
    add("    kimix::string_view status;")
    add("    kimix::string_view cleaned;")
    add("    bool has_total_matches;")
    add("    size_t total_matches;")
    add("    bool has_total_files;")
    add("    size_t total_files;")
    add("    kimix::string_view folded_json;")
    add("    bool has_skipped_files;")
    add("    size_t skipped_files;")
    add("    bool has_skipped_log;")
    add("    kimix::string_view skipped_log;")
    add("};")
    add("static const tt_rtk_golden k_tt_rtk_golden[] = {")
    for (lines,) in rtk_cases():
        cleaned, meta = output_utils.parse_rtk_rg_output(list(lines))
        folded_json = "[" + ",".join(
            '{"path":"%s","count":%d,"log":%s,"start_line":%s}' % (
                jesc(f["path"]), f["count"], fmt_opt(f["log"]), fmt_opt(f["start_line"]))
            for f in meta["folded_files"]) + "]"
        for f in meta["folded_files"]:
            assert 0x20 <= max((ord(c) for c in f["path"]), default=0x20) < 0x7F
        add("    {tt_cstr(%s), tt_cstr(\"ok\"), tt_cstr(%s), %s, %d, %s, %d, tt_cstr(%s), %s, %d, %s, tt_cstr(%s)}," % (
            cpp_lit(enc_lines(lines)), cpp_lit(enc_lines(cleaned)),
            "true" if meta["total_matches"] is not None else "false",
            meta["total_matches"] or 0,
            "true" if meta["total_files"] is not None else "false",
            meta["total_files"] or 0,
            cpp_lit(folded_json),
            "true" if meta["skipped_files"] is not None else "false",
            meta["skipped_files"] or 0,
            "true" if meta["skipped_log"] is not None else "false",
            cpp_lit(meta["skipped_log"] or "")))
    add("};")
    add("")

    add("#endif // !KIMIX_TT_GOLDEN_NO_LINE_VECTORS")
    add("")
    add("#if !defined(KIMIX_TT_GOLDEN_NO_JSON_VECTORS)")
    add("")
    # ---- 8. JSON / ToolParams fidelity (CPython json module) ---------------
    buckets = classify_json_texts()
    add("// CPython json.loads vs ToolParams::deserialize.  `canon` is a")
    add("// language-agnostic rendering of the parsed value: null/bool, i:<dec> for")
    add("// integers, f:<16 hex digits of the IEEE-754 bits> for reals, s:<json-escaped>")
    add("// for strings, [..] / {..} with OBJECT KEYS SORTED (the native map is")
    add("// unordered).  See json_canon() in scripts/gen_tool_types_goldens.py.")
    add("struct tt_json_golden {")
    add("    kimix::string_view text;")
    add("    kimix::string_view canon;")
    add("};")
    add("static const tt_json_golden k_tt_json_golden[] = {")
    for text, canon in buckets["parity"]:
        add("    {tt_cstr(%s), tt_cstr(%s)}," % (cpp_lit(text), cpp_lit(canon)))
    add("};")
    add("")
    add("// Both parsers reject these (malformed JSON / non-object root / BOM /")
    add("// trailing content): CPython raises ValueError, the native reader must")
    add("// return false.  `ref` is CPython's ascii(repr(...)) of the exception")
    add("// value for the ..divergence arrays below only.")
    add("struct tt_json_reject_golden {")
    add("    kimix::string_view text;")
    add("};")
    add("struct tt_json_reference_golden {")
    add("    kimix::string_view text;")
    add("    kimix::string_view ref; // CPython's ascii(repr(value)), documentation only")
    add("};")
    add("static const tt_json_reject_golden k_tt_json_reject_golden[] = {")
    for text in buckets["reject"]:
        add("    {tt_cstr(%s)}," % cpp_lit(text))
    add("};")
    add("")
    add("// Documented divergence: CPython json.loads accepts these, the native")
    add("// reader rejects them by contract - ToolParams is a JSON *object* body")
    add("// (\"root must be a JSON object\").  `ref` records CPython's value.")
    add("static const tt_json_reference_golden k_tt_json_nonobject_golden[] = {")
    for text, ref in buckets["nonobject"]:
        add("    {tt_cstr(%s), tt_cstr(%s)}," % (cpp_lit(text), cpp_lit(ref)))
    add("};")
    add("")
    add("// Documented divergence: CPython json.loads accepts these, the native")
    add("// reader rejects them because the ValueElement model holds only")
    add("// null/bool/int64/uint64/double/UTF-8 string (yyjson: \"report error if")
    add("// double number is infinity\", strings must be valid UTF-8, so a lone")
    add("// \\\\udXXX escape is an error).  `ref` records what CPython produced; a")
    add("// caller that needs these must stay on the Python mirror.")
    add("static const tt_json_reference_golden k_tt_json_unrepresentable_golden[] = {")
    for text, ref in buckets["unrepresentable"]:
        add("    {tt_cstr(%s), tt_cstr(%s)}," % (cpp_lit(text), cpp_lit(ref)))
    add("};")
    add("")
    add("// Documented divergence: an integer outside [INT64_MIN, UINT64_MAX] is")
    add("// read by yyjson as a DOUBLE (its documented policy), so the native value")
    add("// is a real where CPython has an exact int.  Every vector here must parse")
    add("// natively, must render with at least one f: (a real) and must NOT equal")
    add("// CPython's canonical integer value - the reader is pinned against a")
    add("// silent precision/type change in yyjson.")
    add("struct tt_json_lossy_golden {")
    add("    kimix::string_view text;")
    add("    kimix::string_view canon; // CPython json.loads + json_canon")
    add("    kimix::string_view ref;   // ascii(repr(value)), documentation")
    add("};")
    add("static const tt_json_lossy_golden k_tt_json_lossy_number_golden[] = {")
    for text, canon, ref in buckets["lossy"]:
        add("    {tt_cstr(%s), tt_cstr(%s), tt_cstr(%s)}," % (
            cpp_lit(text), cpp_lit(canon), cpp_lit(ref)))
    add("};")
    add("")
    add("#endif // !KIMIX_TT_GOLDEN_NO_JSON_VECTORS")
    return "\n".join(L)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--reference", default=DEFAULT_REFERENCE)
    ap.add_argument("--check", action="store_true", help="fail when the .inc is stale")
    ap.add_argument("-o", "--out", default=str(OUT))
    args = ap.parse_args()
    grep_local, output_utils = load_reference(args.reference)
    text = render(grep_local, output_utils)
    out = Path(args.out)
    if args.check:
        current = out.read_text(encoding="utf-8") if out.is_file() else ""
        if current != text:
            print(f"stale: {out}")
            return 1
        print(f"up to date: {out}")
        return 0
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {out} ({len(text.splitlines())} lines, {len(text)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
