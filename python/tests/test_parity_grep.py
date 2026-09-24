"""Differential parity tests for the grep builtin tool (kimi-base <-> kimi-agent).

The C++ grep tool (``src/builtin_tools/grep_tool.{h,cpp}``) is a port of
``kimi-cli/src/kimi_cli/tools/file/grep_*.py`` + ``output_utils.py`` +
``utils/sensitive.py``.  Most of its kernels are not exposed to Python (they are
covered by the Boost.UT goldens in ``tests/unit/builtin_tools/test_grep_tool.cpp``
and the generated ``grep_goldens.inc``); the ones reachable through the
``runtime_py`` extension are checked here against the *original* implementation:

* ``runtime_py.builtin_tools.file.pattern_has_regex_newline`` — the grep tool's
  own kernel (``grep_tool.cpp``), reachable from the Python shim;
* ``runtime_py.tools.{pattern_has_regex_newline,multiline_pattern}`` — the port of
  ``grep_local._pattern_has_regex_newline`` / ``_multiline_pattern`` that the grep
  tool wires into its fallback path (re-declared inside grep_tool.cpp);
* ``runtime_py.tools.scan_lines`` / ``scan_lines_cb`` — the native line-offset
  scanner the grep tool uses instead of the blocked native regex matcher;
* ``runtime_py.tools.find_in_file`` — the kernel behind ``FindStr``, listed for
  this tool because it shares the grep line scanner.

Ground truth comes from the kimi-agent checkout only.  Where the reference body
is reachable as an importable function it is called directly; where kimi-agent
only has a nested/derived body (``find_str.py::find_in_file`` lives inside
``FindStr.__call__``) the *verbatim* source of that function is extracted with
``ast`` and executed.  The newline kernels additionally have an independent
oracle derived from the documented rule ("a literal LF, or ``\\n`` after an odd
backslash run"), which is what the port's own report claims to implement; the
kimi-agent ``_compat_*`` mirrors are compared too, but they share ancestry with
the port, so they are never the only evidence.
"""

from __future__ import annotations

import ast
import os
import re
import sys
import textwrap
from pathlib import Path

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from _parity_ref import KIMI_CLI_SRC, KIMIX_SRC, ref, ref_available  # noqa: E402

pytestmark = pytest.mark.skipif(
    not ref_available(), reason="kimi-agent checkout not available"
)

import runtime_py  # noqa: E402

FILE = runtime_py.builtin_tools.file
TOOLS = runtime_py.tools

# Importing the reference module also gives access to its call path.
grep_local = ref("kimi_cli.tools.file.grep_local")


# ---------------------------------------------------------------------------
# reference helpers
# ---------------------------------------------------------------------------

_REGEX_NEWLINE_RE = re.compile(r"(?<!\\)(?:\\\\)*\\n")


def oracle_has_regex_newline(pattern: str) -> bool:
    """Documented rule (grep_local.py::_pattern_has_regex_newline docstring)."""
    return "\n" in pattern or bool(_REGEX_NEWLINE_RE.search(pattern))


def oracle_multiline_pattern(pattern: str) -> str:
    """Documented rule (grep_local.py::_multiline_pattern docstring)."""
    if "\n" not in pattern and not _REGEX_NEWLINE_RE.search(pattern):
        return pattern
    p = pattern.replace("\r\n", "\n")
    p = _REGEX_NEWLINE_RE.sub(lambda _m: r"\r?\n", p)
    return p.replace("\n", r"\r?\n")


def _reference_function(module_path: Path, name: str, extra_globals=None):
    """Execute the verbatim source of a (possibly nested) reference function."""
    src = module_path.read_text(encoding="utf-8")
    tree = ast.parse(src)
    for node in ast.walk(tree):
        if isinstance(node, ast.FunctionDef) and node.name == name:
            segment = ast.get_source_segment(src, node)
            ns: dict = dict(extra_globals or {})
            exec(textwrap.dedent(segment), ns)  # noqa: S102 - reference source
            return ns[name]
    raise AssertionError(f"{name} not found in {module_path}")


# find_str.py::find_in_file is nested inside FindStr.__call__, so it is executed
# from source; its module globals are stubbed so the *pure-Python* body runs.
REFERENCE_FIND_IN_FILE = _reference_function(
    Path(KIMIX_SRC) / "kimix" / "tools" / "file" / "find_str.py",
    "find_in_file",
    {"_native_use_native": lambda *_a, **_k: False, "_NATIVE_TOOLS": None},
)


def oracle_scan_lines(content: bytes, pattern: bytes, case_insensitive: bool):
    """Contract of py_tools.cpp / kimix_native.tools.scan_lines (ASCII data).

    ``while start < n`` (an empty final segment is never scanned), ``\\n`` only
    line splitting, ASCII A-Z folding on both sides, (line_index, byte_offset,
    line_len) for every line containing the literal pattern.
    """
    if not pattern:
        return []
    needle = pattern.lower() if case_insensitive else pattern
    hits = []
    n = len(content)
    start = 0
    line_index = 0
    while start < n:
        nl = content.find(b"\n", start)
        line_end = n if nl < 0 else nl
        line = content[start:line_end]
        test = line.lower() if case_insensitive else line
        if needle in test:
            hits.append((line_index, start, line_end - start))
        start = line_end + 1 if nl >= 0 else n
        line_index += 1
    return hits


def oracle_scan_lines_cb(content: bytes, matcher):
    """grep_local.backup_grep's non-multiline line loop (guarded content).

    ``lines = content.splitlines()`` — the caller guards that splitlines() is
    plain ``\\n`` splitting (``_use_native_line_scan``), which the corpus below
    respects.  That means an empty content has NO lines and a trailing ``\\n``
    does not add an empty last line — the native `while start < n` loop has the
    same shape.
    """
    lines = content.split(b"\n")
    if lines and lines[-1] == b"":
        lines.pop()
    hits = []
    offset = 0
    for index, line in enumerate(lines):
        if matcher(line):
            hits.append((index, offset, len(line)))
        offset += len(line) + 1
    return hits


# ---------------------------------------------------------------------------
# corpora
# ---------------------------------------------------------------------------

PATTERN_CORPUS = [
    "abc",
    "a\nb",
    "a\\nb",
    "a\\\\nb",
    "a\\\\\\nb",
    "a\nb\\nc",
    "\\n",
    "\\\\n",
    "\\\\\\n",
    "x\\ny",
    "a\r\nb",
    "a\r\nb\\nc",
    "\\\\\\n\\n",
    "a\\\\\\\\nb",  # 4 backslashes + n: even -> literal
    "a\\\\\\\\\\nb",  # 5 backslashes + n: odd
    "\\n\\n\\n",
    "",
    "n",
    "\\",
    "\\\\",
    "\\N",
    "a\\\rb",
    "a\r\n",
    "\\r?\\n",
    "a\\\r\nb",
    "x\n\\n\ny",
    "^\\n$",
    "[\\n]",
]

SCAN_LINES_CORPUS = [
    (b"", b"a", True),
    (b"a", b"a", True),
    (b"a\n", b"a", True),
    (b"a\nb", b"a", True),
    (b"a\nb\n", b"b", True),
    (b"a\nb\n", b"c", True),
    (b"a\nb\n", b"", True),
    (b"abc\ndef\nghi\n", b"b", True),
    (b"AbC\nDeF\n", b"abc", True),
    (b"AbC\nDeF\n", b"abc", False),
    (b"ABC\n", b"abc", False),
    (b"ABC\n", b"ABC", False),
    (b"line1\nline2\nline3", b"line", True),
    (b"line1\nline2\nline3", b"3", True),
    (b"overlap\ntail", b"lap", True),
    (b"x\ny\nz\n\n", b"", True),
    (b"\n", b"a", True),
    (b"\n\n", b"a", True),
    (b"a" * 40 + b"\n" + b"b" * 7, b"bbbb", True),
    (b"pattern\nPATTERN\n", b"pattern", True),
    (b"tail-without-newline", b"newline", True),
]

SCAN_CB_CORPUS = [
    (b"", r"a"),
    (b"a\nb\nc", r"^a"),
    (b"a\nb\nc", r"^b"),
    (b"a\nb\nc", r"c$"),
    (b"foo1\nbar\nfoo2\n", r"foo\d"),
    (b"foo1\nbar\nfoo2\n", r"ba."),
    (b"only\n", r"only"),
    (b"only\n", r"nope"),
    (b"\n\nx", r"x"),
    (b"abc\ndef", r"[a-c]+"),
    (b"line1\nline2\nline3\n", r"\d"),
    (b"a\n\nb\n", r"^$"),
    (b"a\n\nb", r"^$"),
    (b"\n\n", r"^$"),
    (b"", r"^$"),
    (b"", r"a"),
    (b"abc", r""),
    (b"a\nb\n", r"b"),
    (b"trailing\n\n", r"^$"),
]

FIND_IN_FILE_CORPUS = [
    (b"", b"a", True),
    (b"hello world", b"o", True),
    (b"hello\nworld\n", b"o", True),
    (b"aXaXa\n", b"a", True),
    (b"aXaXa\n", b"a", False),
    (b"MiXeD\ncase\n", b"mixed", True),
    (b"MiXeD\ncase\n", b"mixed", False),
    (b"aaa\n", b"aa", True),  # overlapping
    (b"one\ntwo\nthree\n", b"e", True),
    (b"no match here\n", b"zzz", True),
    (b"tab\there\n", b"\t", True),
    (b"CRLF\r\nline\r\n", b"line", True),
    (b"trailing\n", b"trailing", True),
    (b"a\n\nb\n", b"", True),
    (b"long " + b"x" * 30 + b"\n", b"xxx", True),
]


# ---------------------------------------------------------------------------
# grep tool kernel: runtime_py.builtin_tools.file.pattern_has_regex_newline
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("pattern", PATTERN_CORPUS)
def test_file_pattern_has_regex_newline_parity(pattern):
    status, value = FILE.pattern_has_regex_newline(pattern)
    assert status == "ok", (pattern, status)
    assert value == oracle_has_regex_newline(pattern), pattern


@pytest.mark.parametrize("pattern", PATTERN_CORPUS)
def test_file_pattern_has_regex_newline_matches_reference_call_path(pattern, monkeypatch):
    """The reference call path (grep_local) must agree when the kernel answers."""

    class _Native:
        @staticmethod
        def pattern_has_regex_newline(pat):
            status, value = FILE.pattern_has_regex_newline(pat)
            assert status == "ok"
            return value

    monkeypatch.setattr(grep_local, "_NATIVE_TOOLS", _Native)
    monkeypatch.setattr(grep_local, "_native_use_native", lambda *_a, **_k: True)
    assert grep_local._pattern_has_regex_newline(pattern) == oracle_has_regex_newline(
        pattern
    ), pattern


def test_file_pattern_has_regex_newline_gates_non_ascii():
    """Non-ASCII patterns are outside the native subset -> the shim falls back."""
    status, value = FILE.pattern_has_regex_newline("caf\u00e9\\n")
    assert status == "unsupported"
    assert value is False
    # ... and grep_local then answers from its compat mirror, as in production.
    assert grep_local._pattern_has_regex_newline("caf\u00e9\\n") is True


@pytest.mark.parametrize("pattern", PATTERN_CORPUS)
def test_tools_newline_kernels_parity(pattern):
    assert TOOLS.pattern_has_regex_newline(pattern) == oracle_has_regex_newline(pattern)
    assert TOOLS.multiline_pattern(pattern) == oracle_multiline_pattern(pattern)


@pytest.mark.parametrize("pattern", PATTERN_CORPUS)
def test_tools_newline_kernels_vs_agent_mirror(pattern):
    """Cross-check against kimi-agent's own pure-Python fallback (same lineage)."""
    compat = ref("kimix_native.tools")
    assert TOOLS.pattern_has_regex_newline(pattern) == (
        compat._compat_pattern_has_regex_newline(pattern)
    ), pattern
    assert TOOLS.multiline_pattern(pattern) == compat._compat_multiline_pattern(pattern), pattern


# ---------------------------------------------------------------------------
# tools.scan_lines / scan_lines_cb
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("content,pattern,case_insensitive", SCAN_LINES_CORPUS)
def test_scan_lines_parity(content, pattern, case_insensitive):
    got = [tuple(hit) for hit in TOOLS.scan_lines(content, pattern, case_insensitive)]
    assert got == oracle_scan_lines(content, pattern, case_insensitive), (
        content,
        pattern,
        case_insensitive,
    )


@pytest.mark.parametrize("content,pattern", SCAN_CB_CORPUS)
def test_scan_lines_cb_parity(content, pattern):
    import regex as regex_mod

    compiled = regex_mod.compile(pattern)

    def matcher(line: bytes, _index: int = 0) -> bool:
        return bool(compiled.search(line.decode("utf-8", "surrogatepass")))

    got = [tuple(hit) for hit in TOOLS.scan_lines_cb(content, matcher)]
    assert got == oracle_scan_lines_cb(content, matcher), (content, pattern)


def test_scan_lines_cb_offsets_cover_line_bytes():
    """The callback receives the line body and the offsets slice the content."""
    seen = []

    def matcher(line: bytes, index: int) -> bool:
        seen.append((index, line))
        return b"hit" in line

    content = b"a\nhit b\nc\nhitagain\n"
    hits = [tuple(h) for h in TOOLS.scan_lines_cb(content, matcher)]
    assert seen == [(0, b"a"), (1, b"hit b"), (2, b"c"), (3, b"hitagain")]
    for index, offset, length in hits:
        assert content[offset : offset + length] == seen[index][1]


# ---------------------------------------------------------------------------
# tools.find_in_file (FindStr kernel; shares the grep line scanner)
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("content,needle,case_sensitive", FIND_IN_FILE_CORPUS)
def test_find_in_file_parity(content, needle, case_sensitive, tmp_path):
    if b"\r" in content:
        pytest.skip("readlines() applies universal newlines; LF-only corpus here")
    path = tmp_path / "sample.txt"
    path.write_bytes(content)
    reference = REFERENCE_FIND_IN_FILE(str(path), needle.decode(), case_sensitive)
    expected = [
        (rec["line"] - 1, rec["column"] - 1, len(needle)) for rec in reference
    ]
    got = [
        tuple(hit)
        for hit in TOOLS.find_in_file(content, needle, case_insensitive=not case_sensitive)
    ]
    assert got == expected, (content, needle, case_sensitive)


def test_find_in_file_ascii_fold_only():
    """The kernel folds A-Z only (documented deviation) - non-ASCII goes to Python."""
    content = "\u00c9cole\n".encode()
    assert TOOLS.find_in_file(content, "\u00e9".encode(), True) == []
