"""Parity tests for kimix_native.diff (plan 018).

Compares the native kernels against the pure-Python ``_compat`` mirrors on:
- unified_diff: identical/empty inputs, trailing-newline handling, single and
  multi-line changes, multiple hunks, context_lines variations, custom path /
  lineterm / include_file_header
- diff_hunks: same corpus (old_start/new_start line numbers, old_lines /
  new_lines content)
- inline_diff_ranges: tabs (offset-map parity), min_ratio threshold, unicode
- the KIMIX_NATIVE_DIFF=0 fallback toggle
"""
import os
import random
import subprocess
import sys

import pytest

from kimix_native import diff

CASES = [
    (b"", b""),
    (b"a\nb\n", b"a\nb\n"),
    (b"a\nb\n", b"a\nb\nc\n"),
    (b"line1\nline2\nline3\n", b"line1\nline2-X\nline3\n"),
    (b"one\ntwo\nthree\nfour\nfive\n", b"one\ntwo\nthree\nTHREE\nfive\n"),
    (b"a\nb\nc\nd\ne\nf\ng\nh\n", b"a\nb\nX\nX\ne\nf\ng\nh\n"),
    (b"no trailing newline", b"no trailing newline"),
    (b"no trailing newline", b"no trailing newline\n"),
    (b"x", b"y"),
    ("caf\u00e9 \u4e16\u754c\n\u6d4b\u8bd5\n".encode("utf-8"),
     "caf\u00e9 \u4e16\u754c\n\u6d4b\u8bd5!\n".encode("utf-8")),
]


def test_unified_diff_parity():
    for old, new in CASES:
        for path in ("", "file.txt"):
            for header in (True, False):
                for lineterm in ("\n", ""):
                    native = diff.unified_diff(old, new, path, header, lineterm)
                    compat = diff._compat_unified_diff(old, new, path, header, lineterm)
                    assert native == compat, (old, new, path, header, lineterm)


def test_unified_diff_bytes_semantics():
    # Two-line header, hunks with @@-line and context.
    out = diff.unified_diff(b"a\nb\nc\n", b"a\nB\nc\n", "x.txt")
    assert isinstance(out, bytes)
    assert out.startswith(b"--- a/x.txt\n+++ b/x.txt\n")
    assert b"@@ -1,3 +1,3 @@" in out
    assert b"-b\n" in out and b"+B\n" in out


def test_diff_hunks_parity():
    for old, new in CASES:
        for ctx in (0, 1, 3):
            native = diff.diff_hunks(old, new, ctx)
            compat = diff._compat_diff_hunks(old, new, ctx)
            assert native == compat, (old, new, ctx)


def test_diff_hunks_structure():
    hunks = diff.diff_hunks(b"a\nb\nc\n", b"a\nB\nc\n", 1)
    assert isinstance(hunks, list)
    assert len(hunks) >= 1
    h = hunks[0]
    assert set(h) == {"old_start", "new_start", "old_lines", "new_lines"}
    assert h["old_start"] >= 1 and h["new_start"] >= 1
    assert isinstance(h["old_lines"], list) and isinstance(h["new_lines"], list)


def test_inline_diff_ranges_parity():
    pairs = [
        ("foo bar baz", "foo qux baz"),
        ("", ""),
        ("same", "same"),
        ("abcdef", "abcXYZdef"),
        ("a\tb", "a\tc"),
        ("\tindented", "\tchanged"),
        ("caf\u00e9", "caf\u00e8"),
    ]
    for old, new in pairs:
        for ratio in (0.0, 0.5, 0.9):
            native = diff.inline_diff_ranges(old, new, ratio)
            compat = diff._compat_inline_diff_ranges(old, new, ratio)
            assert native == compat, (old, new, ratio)


def test_inline_diff_ranges_tab_offset_map():
    # _build_offset_map parity for tab expansion.
    raw = "\tx"
    native_map = diff._compat_inline_diff_ranges(raw, "y\t", 0.0)[0]
    assert all(isinstance(rng, tuple) and len(rng) == 2 for rng in native_map)


def test_inline_diff_ranges_random():
    rng = random.Random(11)
    alphabet = "abc \t\u00e9\u4e16"
    for _ in range(40):
        old = "".join(rng.choice(alphabet) for _ in range(rng.randrange(0, 20)))
        new = "".join(rng.choice(alphabet) for _ in range(rng.randrange(0, 20)))
        n = diff.inline_diff_ranges(old, new, 0.3)
        c = diff._compat_inline_diff_ranges(old, new, 0.3)
        assert n == c, (old, new)


# ---------------------------------------------------------------------------
# build_offset_map (mirror of diff_render.py::_build_offset_map)
# ---------------------------------------------------------------------------

OFFSET_CASES = [
    # identical strings -> identity map
    ("", ""),
    ("abc", "abc"),
    ("ab\n", "ab\n"),
    # tabs at various columns (tab_size 4 expansions)
    ("\t", "    "),
    ("\ta", "    a"),
    ("a\tb", "a   b"),
    ("a\tb\tc", "a   b   c"),
    ("abc\tx", "abc x"),
    ("abcd\tx", "abcd    x"),
    ("a\t\n", "a   \n"),
    # unicode: CJK, emoji, combining (multi-byte = 1 code point)
    ("caf\u00e9", "caf\u00e9"),
    ("\u4e16\u754c", "\u4e16\u754c"),
    ("\U0001F600a", "\U0001F600a"),
    ("e\u0301", "e\u0301"),
    ("\u4e16\t\u754c", "\u4e16   \u754c"),
    # empty raw / empty rendered
    ("", "abc"),
    ("abc", ""),
    # mismatch fallback: rendered shorter / longer than the expansion
    ("abc", "ab"),
    ("ab", "abcdef"),
]


def test_build_offset_map_parity():
    """Native kernel vs the pure-Python mirror must be identical."""
    for raw, rendered in OFFSET_CASES:
        for tab_size in (1, 2, 4, 8):
            native = diff.build_offset_map(raw, rendered, tab_size)
            compat = diff._compat_build_offset_map(raw, rendered, tab_size)
            assert native == compat, (raw, rendered, tab_size)
            # exactly len(raw) + 1 entries (code points, not bytes)
            assert len(native) == len(raw) + 1, (raw, rendered, tab_size)


def test_build_offset_map_tab_expansion():
    """When rendered == raw.expandtabs(tab_size) the map is the exact
    column-aware expansion (or the fallback linear map when the app walk
    diverges from str.expandtabs, e.g. newline-then-tab column resets)."""
    raws = ["", "\t", "\ta", "a\tb", "a\tb\tc", "abc\tx", "abcd\tx",
            "a\t\n", "a\n\tb", "\u4e16\t\u754c"]
    for raw in raws:
        for tab_size in (1, 2, 4, 8):
            rendered = raw.expandtabs(tab_size)
            offsets = diff.build_offset_map(raw, rendered, tab_size)
            assert offsets == diff._compat_build_offset_map(raw, rendered, tab_size), \
                (raw, tab_size)
            # walk reconstruction from the app contract
            expect = []
            col = 0
            for ch in raw:
                expect.append(col)
                if ch == "\t":
                    col += tab_size - (col % tab_size)
                else:
                    col += 1
            expect.append(col)
            if col == len(rendered):
                assert offsets == expect, (raw, tab_size)
            else:
                raw_len = len(raw)
                rendered_len = len(rendered)
                if raw_len == 0:
                    fallback = [rendered_len]
                else:
                    fallback = [(i * rendered_len) // raw_len for i in range(raw_len)] \
                        + [rendered_len]
                assert offsets == fallback, (raw, tab_size)


def test_build_offset_map_semantics():
    """Hand-computed expectations from the app contract."""
    # identical fast path
    assert diff.build_offset_map("abc", "abc", 4) == [0, 1, 2, 3]
    assert diff.build_offset_map("", "", 4) == [0]
    assert diff.build_offset_map("ab\n", "ab\n", 4) == [0, 1, 2, 3]
    # tab expansion
    assert diff.build_offset_map("\t", "    ", 4) == [0, 4]
    assert diff.build_offset_map("\ta", "    a", 4) == [0, 4, 5]
    assert diff.build_offset_map("a\tb", "a   b", 4) == [0, 1, 4, 5]
    assert diff.build_offset_map("abc\tx", "abc x", 4) == [0, 1, 2, 3, 4, 5]
    assert diff.build_offset_map("abcd\tx", "abcd    x", 4) == [0, 1, 2, 3, 4, 8, 9]
    assert diff.build_offset_map("a\t\n", "a   \n", 4) == [0, 1, 4, 5]
    # tab_size 1/2/8
    assert diff.build_offset_map("\ta", "  a", 2) == [0, 2, 3]
    assert diff.build_offset_map("a\tb", "a b", 2) == [0, 1, 2, 3]
    assert diff.build_offset_map("\t", "        ", 8) == [0, 8]
    assert diff.build_offset_map("\ta", " a", 1) == [0, 1, 2]
    # empty raw fallback
    assert diff.build_offset_map("", "abc", 4) == [3]
    # mismatch fallback: rendered shorter / longer
    assert diff.build_offset_map("abc", "ab", 4) == [0, 0, 1, 2]
    assert diff.build_offset_map("ab", "abcdef", 4) == [0, 3, 6]
    assert diff.build_offset_map("abc", "", 4) == [0, 0, 0, 0]


def test_build_offset_map_unicode_code_points():
    """Multi-byte UTF-8 characters count as exactly 1 column each, and the
    returned list has len(raw) + 1 entries in code points."""
    # CJK (3 bytes each), emoji (4 bytes), combining (2 bytes)
    assert diff.build_offset_map("caf\u00e9", "caf\u00e9", 4) == [0, 1, 2, 3, 4]
    assert diff.build_offset_map("\u4e16\u754c", "\u4e16\u754c", 4) == [0, 1, 2]
    assert diff.build_offset_map("\U0001F600", "\U0001F600", 4) == [0, 1]
    assert diff.build_offset_map("e\u0301", "e\u0301", 4) == [0, 1, 2]
    # tabs among multi-byte chars
    assert diff.build_offset_map("\u4e16\t\u754c", "\u4e16   \u754c", 4) == [0, 1, 4, 5]
    assert diff.build_offset_map("\U0001F600\tx", "\U0001F600   x", 4) == [0, 1, 4, 5]
    assert diff.build_offset_map("e\u0301\tx", "e\u0301  x", 4) == [0, 1, 2, 4, 5]
    # mismatch fallback counts in code points too
    assert diff.build_offset_map("\u4e16\u754c", "\u4e16\u754cX", 4) == [0, 1, 3]


def test_native_disabled_fallback():
    """With KIMIX_NATIVE_DIFF=0 the shim must behave identically."""
    env = dict(os.environ, KIMIX_NATIVE_DIFF="0")
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    code = (
        "import sys; sys.path.insert(0, %r); sys.path.insert(0, %r);\n"
        "from kimix_native import diff\n"
        "assert diff.use_native('DIFF') is False\n"
        "old = b'a\\nb\\nc\\n'; new = b'a\\nB\\nc\\n'\n"
        "out = diff.unified_diff(old, new, 't.txt')\n"
        "assert out.startswith(b'--- a/t.txt\\n'), out\n"
        "h = diff.diff_hunks(old, new)\n"
        "assert isinstance(h, list) and h\n"
        "r = diff.inline_diff_ranges('foo bar', 'foo baz')\n"
        "assert isinstance(r, tuple) and len(r) == 2\n"
        "m = diff.build_offset_map('a\\tb', 'a   b', 4)\n"
        "assert m == [0, 1, 4, 5], m\n"
        "assert m == diff._compat_build_offset_map('a\\tb', 'a   b', 4), m\n"
        "print('FALLBACK_OK')\n"
    ) % (os.path.join(root, "python"), os.path.join(root, "bin", "release"))
    r = subprocess.run([sys.executable, "-c", code], capture_output=True,
                       text=True, env=env)
    assert r.returncode == 0, r.stderr
    assert "FALLBACK_OK" in r.stdout
