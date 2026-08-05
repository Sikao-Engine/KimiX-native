"""Parity tests for kimix_native.tools (plan 013).

Compares the native kernels against the pure-Python mirrors of the reference
algorithms (hash_line.py / find_str.py / grep_local.py backup_grep) and, for
the hash chain, against the real `xxhash` package when importable.

Coverage:
- compute_line_hash: exact uint32/nibble parity on golden (line, seed) pairs
  and random lines (incl. Unicode whitespace + CJK)
- find_in_file: identical match sets (case-sensitive + insensitive, needle at
  line boundaries, empty needle)
- scan_lines: identical offsets to a splitlines-based reference scan
  (no O(matches x n) counting)
"""

import random

import pytest

import kimix_native
from kimix_native import tools as T

try:
    import xxhash as _xxhash
except ImportError:  # pragma: no cover
    _xxhash = None

NIBBLE_STR = "ZPMQVRWSNKTXJBYH"


# ---------------------------------------------------------------------------
# line_hash
# ---------------------------------------------------------------------------

LINE_CORPUS = [
    "hello world",
    "  spaced  ",
    "no_alnum??",
    "12345",
    "line5",
    "",
    "tail",
    "a" * 30,
    "x" * 2000,
    "mixed 123 abc",
    "   ",
    "\t",
    "trailing\r",
    "a\rb",
    "\u00a0nbsp\u00a0",
    "\u4e2d\u6587abc",
    "caf\u00e9",
    "Z" * 300,
    "key = value",
    "line with  spaces",
]


def _ref_hash(line_num, line, prev_hash):
    """Reference recipe (hash_line.py 41-69) using the xxhash package."""
    if line.endswith("\r"):
        line = line[:-1]
    chars = []
    has_significant = False
    for c in line:
        if not c.isspace():
            chars.append(c)
            if not has_significant and c.isalnum():
                has_significant = True
    if prev_hash is not None:
        seed = 0
        for c in prev_hash:
            seed = ((seed * 256) + ord(c)) & 0xFFFFFFFF
    elif has_significant:
        seed = 0
    else:
        seed = line_num
    data = "".join(chars).encode("utf-8")
    if _xxhash is None:
        return T._compat_compute_line_hash(line_num, line, prev_hash)
    h = _xxhash.xxh32(data, seed).intdigest() & 0xFF
    return NIBBLE_STR[h >> 4] + NIBBLE_STR[h & 0x0F]


def test_line_hash_corpus_chain():
    prev = None
    expected = []
    for i, line in enumerate(LINE_CORPUS, 1):
        h = _ref_hash(i, line, prev)
        expected.append(h)
        prev = h
    content = "\n".join(LINE_CORPUS) + "\n"
    assert T.compute_line_hashes(content) == expected


def test_line_hash_individual_nibbles():
    # "hello world" -> filtered "helloworld" -> xxh32(...,0)&0xFF == 2 -> "ZM"
    assert T.line_hash(b"hello world", 0) == 2
    assert T._compat_compute_line_hash(1, "hello world", None) == "ZM"
    assert T.line_hash(b"", 0) == 5
    assert T.line_hash(b"", 42) == 184
    assert T.line_hash(b"abc", 12345) == 41


@pytest.mark.skipif(_xxhash is None, reason="xxhash package not installed")
def test_line_hash_random_vs_xxhash():
    rng = random.Random(1234)
    pool = "abc XYZ 12\t\n\r\u00a0\u2003\u3000\u2028e\u00e9\u4e2d"
    for _ in range(200):
        text = "".join(rng.choice(pool) for _ in range(rng.randint(0, 30)))
        seed = rng.randrange(2**32)
        t = text[:-1] if text.endswith("\r") else text
        chars = [c for c in t if not c.isspace()]
        ref = _xxhash.xxh32("".join(chars).encode("utf-8"), seed).intdigest() & 0xFF
        assert T.line_hash(text.encode("utf-8"), seed) == ref, repr(text)


def test_line_hash_random_chain():
    rng = random.Random(99)
    lines = [
        "".join(rng.choice("abc 12#\t") for _ in range(rng.randint(0, 25)))
        for _ in range(60)
    ]
    content = "\n".join(lines) + "\n"
    prev = None
    expected = []
    for i, line in enumerate(lines, 1):
        h = T._compat_compute_line_hash(i, line, prev)
        expected.append(h)
        prev = h
    assert T.compute_line_hashes(content) == expected


def test_line_hash_crlf_chain():
    lines = ["alpha", "beta", "gamma"]
    lf = T.compute_line_hashes("\n".join(lines) + "\n")
    crlf = T.compute_line_hashes("\r\n".join(lines) + "\r\n")
    assert lf == crlf


# ---------------------------------------------------------------------------
# find_in_file
# ---------------------------------------------------------------------------

FIND_CORPUS = [
    ("ab\nxab\nABC\nzz\n", "ab"),
    ("ab\nxab\nABC\nzz\n", "AB"),
    ("aaa\n", "aa"),
    ("abab\n", "ab"),
    ("a\nb\n", "b"),
    ("line1\nline2\nline3\n", "line"),
    ("hello world\n", "world"),
    ("x\n", "x"),
    ("\n", "x"),
    ("", "x"),
    ("ab\n", "b\n"),
    ("AB\nab\n", "ab"),
    ("one two three\n", " "),
    ("end\n", "end"),
]


def test_find_in_file_parity_ci():
    for content, needle in FIND_CORPUS:
        native = T.find_in_file(content, needle, case_sensitive=False)
        compat = T._compat_find_in_file(content, needle, False)
        assert native == compat, (content, needle)
        for rec in native:
            assert rec["line"] >= 1 and rec["column"] >= 1


def test_find_in_file_parity_cs():
    for content, needle in FIND_CORPUS:
        native = T.find_in_file(content, needle, case_sensitive=True)
        compat = T._compat_find_in_file(content, needle, True)
        assert native == compat, (content, needle)


def test_find_in_file_empty_needle():
    content = "a\n"
    native = T.find_in_file(content, "", case_sensitive=False)
    compat = T._compat_find_in_file(content, "", False)
    assert native == compat
    assert len(native) == 3  # cols 1..3 for line "a\n"


def test_find_in_file_overlapping():
    content = "aaaa\n"
    native = T.find_in_file(content, "aa", case_sensitive=False)
    assert [(r["line"], r["column"]) for r in native] == [(1, 1), (1, 2), (1, 3)]


def test_find_in_file_non_ascii_routes_to_compat():
    # native folds ASCII only; non-ASCII content goes through the mirror
    content = "caf\u00e9\nCAF\u00c9\n"
    native = T.find_in_file(content, "caf\u00e9", case_sensitive=False)
    compat = T._compat_find_in_file(content, "caf\u00e9", False)
    assert native == compat


# ---------------------------------------------------------------------------
# scan_lines
# ---------------------------------------------------------------------------

SCAN_CORPUS = [
    "line1\nhas pattern here\nline3\nPATTERN again\n",
    "a\nb\na\n",
    "aaa\nbb\naaa\n",
    "x\n",
    "\n",
    "",
    "one\ntwo\nthree\n",
    "pattern\npattern\n",
    "no match here\n",
    "P\np\nPp\n",
]


def test_scan_lines_parity():
    for content in SCAN_CORPUS:
        native = T.scan_lines(content, "pattern", case_insensitive=True)
        compat = T._compat_scan_lines(content, "pattern", True)
        assert native == compat, repr(content)
        native_cs = T.scan_lines(content, "pattern", case_insensitive=False)
        compat_cs = T._compat_scan_lines(content, "pattern", False)
        assert native_cs == compat_cs, repr(content)


def test_scan_lines_offsets_incremental():
    """Offsets are cumulative line starts (no count(\"\\n\") per match)."""
    content = "aaa\nbb\naaa\n"
    hits = T.scan_lines(content, "aaa", case_insensitive=True)
    assert hits == [(0, 0, 3), (2, 7, 3)]


def test_scan_lines_empty_pattern():
    content = "a\nb\n"
    assert T.scan_lines(content, "") == []
    assert T._compat_scan_lines(content, "", True) == []


def test_scan_lines_cb():
    content = "a\nbb\nccc\n"
    hits = T.scan_lines_cb(content, lambda line, idx: line == b"bb")
    assert hits == [(1, 2, 2)]
    # callback sees the raw line bytes without the terminator
    seen = []
    T.scan_lines_cb("x\ny\n", lambda line, idx: seen.append((line, idx)) or False)
    assert seen == [(b"x", 0), (b"y", 1)]


def test_scan_lines_non_ascii_routes_to_compat():
    content = "caf\u00e9\ncafe\n"
    native = T.scan_lines(content, "caf\u00e9", case_insensitive=True)
    compat = T._compat_scan_lines(content, "caf\u00e9", True)
    assert native == compat
    # "café" only matches line 1 ("cafe" is a different string)
    assert len(native) == 1


def test_tools_native_disabled(monkeypatch):
    monkeypatch.setenv("KIMIX_NATIVE_TOOLS", "0")
    assert kimix_native.use_native("TOOLS") is False
    # line_hash falls back to the pure-Python xxh32
    assert T.line_hash(b"hello world", 0) == 2
    content = "ab\nxab\nABC\n"
    assert T.find_in_file(content, "ab", False) == T._compat_find_in_file(content, "ab", False)
    assert T.scan_lines("a\nb\n", "b") == T._compat_scan_lines("a\nb\n", "b", True)
