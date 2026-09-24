#!/usr/bin/env python3
"""Regenerate the `read` tool goldens (tests/unit/builtin_tools/read_goldens.inc).

Every expected value is produced by running the *real* Python implementation
from the kimi-agent checkout (never a kimix_native mirror, never a hand-typed
constant):

  * kimi_cli.tools.file.read          - _render_forward / _render_tail /
                                        _render_result / _apply_char_window,
                                        Params._validate_value
  * kimi_cli.tools.utils              - truncate_line (the "..." variant)
  * aiofiles / Python text-mode read  - universal-newline line splitting
  * kimi_cli.tools.file.hash_line     - compute_line_hash / _cumulative_hashes
                                        (native fast path disabled)
  * kimi_cli.tools.file.read_profiles - render_cpu_profile /
                                        render_sample_profile
  * kimi_cli.tools.file.read_markit   - markdown_to_text

The per-render bookkeeping fields (total_lines / max_lines_reached /
max_bytes_reached / end_of_file / truncated_line_numbers) are captured by
wrapping ReadFile._render_result and recording the keyword arguments the
reference itself passes - they are not re-derived here.

Usage:
    python scripts/gen_read_goldens.py                 # write the .inc
    python scripts/gen_read_goldens.py --check         # fail if stale
    python scripts/gen_read_goldens.py --reference DIR # kimi-agent checkout

The generated file is pure ASCII (every byte outside the printable ASCII range
is emitted as a 3-digit octal escape, which is never ambiguous with the
following character) and every literal is chunked below MSVC's 16 KiB string
literal limit.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import random
import sys
import tempfile
import types
from pathlib import Path

DEFAULT_REFERENCE = os.environ.get("KIMI_AGENT_ROOT", r"C:/dev/kimi-agent")
DEFAULT_OUT = Path("tests/unit/builtin_tools/read_goldens.inc")

MAX_LINES = 5000


# ---------------------------------------------------------------------------
# reference import
# ---------------------------------------------------------------------------


def import_reference(root: Path):
    for p in (str(root / "kimi-cli" / "src"), str(root / "src")):
        if p not in sys.path:
            sys.path.insert(0, p)

    import kimi_cli.tools.file.hash_line as hash_line
    import kimi_cli.tools.file.read as read
    import kimi_cli.tools.file.read_markit as read_markit
    import kimi_cli.tools.file.read_profiles as read_profiles
    import kimi_cli.tools.utils as tools_utils
    from kosong.tooling import ToolOk

    return {
        "read": read,
        "hash_line": hash_line,
        "read_markit": read_markit,
        "read_profiles": read_profiles,
        "tools_utils": tools_utils,
        "ToolOk": ToolOk,
    }


class Ref:
    """Holds the reference modules, a dummy `self`, and the render recorder."""

    def __init__(self, mods):
        self.read = mods["read"]
        self.hash_line = mods["hash_line"]
        self.read_markit = mods["read_markit"]
        self.read_profiles = mods["read_profiles"]
        self.tools_utils = mods["tools_utils"]
        self.ToolOk = mods["ToolOk"]
        # _render_forward / _render_tail only use self._render_result.
        self.self = self.read.ReadFile.__new__(self.read.ReadFile)
        self.captured = None
        self._disable_native()
        self._install_recorder()

    def _disable_native(self):
        # _cumulative_hashes short-circuits to kimix_native when available; the
        # parity target is the pure-Python loop.
        for attr in ("_native_use_native",):
            if hasattr(self.hash_line, attr):
                setattr(self.hash_line, attr, lambda *_a, **_k: False)
        for attr in ("_NATIVE_TOOLS", "_NATIVE", "_native"):
            if hasattr(self.hash_line, attr):
                setattr(self.hash_line, attr, None)

    def _install_recorder(self):
        original = self.read.ReadFile._render_result
        ref = self

        def wrapper(self_ignored, candidates, display_path, n_lines, start_line,
                    **kwargs):
            ref.captured = {
                "candidates": list(candidates),
                "display_path": display_path,
                "n_lines": n_lines,
                "start_line": start_line,
            }
            ref.captured.update(kwargs)
            return original(self_ignored, candidates, display_path, n_lines,
                            start_line, **kwargs)

        self.read.ReadFile._render_result = wrapper

    def take_capture(self):
        captured = self.captured
        self.captured = None
        return captured


# ---------------------------------------------------------------------------
# reference-side helpers
# ---------------------------------------------------------------------------


def py_lines_from_bytes(data: bytes) -> list[str]:
    """Split bytes exactly like aiofiles/KaosPath text-mode iteration.

    aiofiles.open(path, encoding="utf-8", errors="replace") without an explicit
    ``newline`` uses universal-newline translation, so this writes the bytes to
    a real file and iterates the real Python text reader (ground truth for
    split_lines).
    """
    fd, path = tempfile.mkstemp(prefix="read_golden_", suffix=".txt")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.readlines()
    finally:
        os.unlink(path)


async def _render(ref: Ref, lines, display_path, offset, n_lines, show, note):
    async def gen():
        for line in lines:
            yield line

    if offset < 0:
        result = await ref.read.ReadFile._render_tail(
            ref.self, gen(), display_path, offset, n_lines,
            show_line_numbers=show, note=note,
        )
    else:
        result = await ref.read.ReadFile._render_forward(
            ref.self, gen(), display_path, offset, n_lines,
            show_line_numbers=show, note=note,
        )
    res, window, start = result
    captured = ref.take_capture()
    assert captured is not None, "render recorder missed a call"
    return res, window, start, captured


# ---------------------------------------------------------------------------
# C++ literal emission
# ---------------------------------------------------------------------------


def _esc(byte: int) -> str:
    if byte == 0x22:
        return '\\"'
    if byte == 0x5C:
        return "\\\\"
    if byte == 0x0A:
        return "\\n"
    if byte == 0x0D:
        return "\\r"
    if byte == 0x09:
        return "\\t"
    if 0x20 <= byte <= 0x7E:
        return chr(byte)
    return "\\%03o" % byte


def lit(data, indent: str = "        ") -> str:
    """A C++ string literal for *data* (str or bytes), chunked and ASCII-only."""
    if isinstance(data, str):
        data = data.encode("utf-8")
    elif not isinstance(data, (bytes, bytearray)):
        data = str(data).encode("utf-8")
    if not data:
        return '""'
    chunks: list[str] = []
    cur = ""
    for byte in data:
        piece = _esc(byte)
        if len(cur) + len(piece) > 900:
            chunks.append(cur)
            cur = ""
        cur += piece
    chunks.append(cur)
    if len(chunks) == 1:
        return '"' + chunks[0] + '"'
    return ("\n" + indent).join('"%s"' % c for c in chunks)


def join01(items) -> str:
    return "\x01".join(items)


def fmt_ints(values) -> str:
    return " ".join(str(v) for v in values)


# Big corpora would otherwise be stored byte-for-byte twice (input + output).
# They are described by a tiny recipe instead; `expand_spec` mirrors the C++
# expansion and the generator asserts the two agree before writing the golden.
SPEC_SEP = "\x02"


def make_spec(lines):
    """Return (spec, expanded) for *lines*, or ("", lines) when not compressible.

    Recipes mirror `rd_expand_spec` in tests/unit/builtin_tools/test_read_tool.cpp:
      repeat<sep>N<sep>LINE          N copies of LINE
      numbered<sep>N<sep>PREFIX      PREFIX + str(i) (+ "\\n") for i in 1..N
    """
    if lines and len(set(lines)) == 1:
        spec = "repeat%s%d%s%s" % (SPEC_SEP, len(lines), SPEC_SEP, lines[0])
        spec_expanded = [lines[0]] * len(lines)
    else:
        spec = ""
        spec_expanded = lines
        for prefix in ("Line ", "l", "L"):
            for suffix in ("\n", ""):
                if all(line == "%s%d%s" % (prefix, i, suffix)
                       for i, line in enumerate(lines, 1)):
                    spec = "numbered%s%d%s%s%s" % (SPEC_SEP, len(lines), SPEC_SEP,
                                                   prefix, suffix)
                    spec_expanded = ["%s%d%s" % (prefix, i, suffix)
                                     for i in range(1, len(lines) + 1)]
                    break
            if spec:
                break
    assert spec_expanded == lines, "spec expansion mismatch"
    return spec, spec_expanded


# ---------------------------------------------------------------------------
# corpora
# ---------------------------------------------------------------------------

SAMPLE_LINES = [
    "Line 1: Hello World\n",
    "Line 2: This is a test file\n",
    "Line 3: With multiple lines\n",
    "Line 4: For testing purposes\n",
    "Line 5: End of file",
]

UNICODE_LINES = [
    "Hello \u4e16\u754c \U0001f30d\n",
    "Unicode test: caf\u00e9, na\u00efve, r\u00e9sum\u00e9",
]

CRLF_LINES = ["a\r\n", "b\r\n", "c\r\n"]
LONE_CR_LINES = ["a\r", "b\r", "c"]

LONG = "A" * 4100
LONG_LINES = [LONG + "\n", "Short line\n", "B" * 4200]

MANY_LINES = ["Line %d\n" % i for i in range(1, MAX_LINES + 2)]
BYTE_LINES = ["x" * 999 + "\n"] * 200

def render_corpus():
    """(name, lines, display_path, offset, n_lines, show, note) tuples."""
    cases = []

    def add(name, lines, offset=1, n_lines=2000, show=True, note="",
            path="sample.txt"):
        cases.append((name, list(lines), path, offset, n_lines, show, note))

    # -- forward (positive offset) --
    add("fwd_sample_default", SAMPLE_LINES)
    add("fwd_offset_3", SAMPLE_LINES, offset=3)
    add("fwd_offset_5", SAMPLE_LINES, offset=5)
    add("fwd_offset_2_limit_2", SAMPLE_LINES, offset=2, n_lines=2)
    add("fwd_offset_2_limit_1", SAMPLE_LINES, offset=2, n_lines=1)
    add("fwd_offset_beyond_eof", SAMPLE_LINES, offset=10)
    add("fwd_offset_1_limit_1", SAMPLE_LINES, n_lines=1)
    add("fwd_offset_1_limit_4", SAMPLE_LINES, n_lines=4)
    add("fwd_empty_file", [], path="empty.txt")
    add("fwd_offset_beyond_empty", [], offset=3, path="empty.txt")
    add("fwd_unicode", UNICODE_LINES)
    add("fwd_crlf", CRLF_LINES)
    add("fwd_lone_cr", LONE_CR_LINES)
    add("fwd_no_final_newline", ["a\n", "b\n", "c"])
    add("fwd_blanks", ["\n", "\n", "x\n", "\n", "y"])
    add("fwd_no_line_numbers", SAMPLE_LINES, show=False)
    add("fwd_no_line_numbers_limit_2", SAMPLE_LINES, n_lines=2, show=False)
    add("fwd_note", SAMPLE_LINES, note=" (extracted from notes.docx)")
    # line truncation
    add("fwd_truncated_single", [LONG + "\n"])
    add("fwd_truncated_multi", LONG_LINES)
    add("fwd_truncated_unicode", ["\u6c49\u5b57" * 60 + "\n", "tail\n"])
    add("fwd_truncated_multibyte_boundary",
        ["a" * 3990 + "\u6c49\u5b57" * 10 + "b" * 100 + "\n"])
    add("fwd_exactly_max_line_length", ["y" * 4000 + "\n"])
    add("fwd_one_over_max_line_length", ["y" * 4001 + "\n"])
    add("fwd_truncated_crlf", ["z" * 4005 + "\r\n"])
    add("fwd_truncated_tail_offset", LONG_LINES, offset=2)
    add("fwd_truncated_only_line", ["w" * 5000 + "\n"], offset=1, n_lines=1)
    # MAX_LINES / MAX_BYTES budgets (large corpora -> recipe-built input)
    add("fwd_max_lines_boundary", MANY_LINES, n_lines=MAX_LINES + 5)
    add("fwd_max_bytes_boundary", BYTE_LINES, n_lines=2000)
    # -- tail (negative offset) --
    add("tail_last_2", SAMPLE_LINES, offset=-2)
    add("tail_last_3_limit_2", SAMPLE_LINES, offset=-3, n_lines=2)
    add("tail_exceeds_file", SAMPLE_LINES, offset=-10)
    add("tail_last_1", SAMPLE_LINES, offset=-1)
    add("tail_last_2_limit_5", SAMPLE_LINES, offset=-2, n_lines=5)
    add("tail_last_2_no_numbers", SAMPLE_LINES, offset=-2, show=False)
    add("tail_empty_file", [], offset=-2, path="empty.txt")
    add("tail_unicode", UNICODE_LINES, offset=-1)
    add("tail_crlf", CRLF_LINES, offset=-2)
    add("tail_note", SAMPLE_LINES, offset=-2, note=" (tail)")
    add("tail_max_lines", MANY_LINES, offset=-(MAX_LINES + 1000), n_lines=MAX_LINES + 5)
    add("tail_max_bytes", BYTE_LINES, offset=-200)
    add("tail_truncated_line", LONG_LINES, offset=-2)
    add("tail_truncated_line_all", LONG_LINES, offset=-3)
    add("tail_limit_gt_tail", SAMPLE_LINES, offset=-2, n_lines=1)
    for (name, lines, path, offset, n_lines, show, note) in             fuzz_render_cases():
        add(name, lines, offset=offset, n_lines=n_lines, show=show, note=note,
            path=path)
    return cases


CHAR_WINDOW_CASES = [
    ("head", "abcdef", 0, 3),
    ("middle", "abcdef", 2, 2),
    ("tail", "abcdef", 4, 10),
    ("exact_fit", "abcdef", 0, 6),
    ("whole", "abcdef", 0, 100),
    ("offset_at_end", "abcdef", 6, 10),
    ("offset_past_end", "abcdef", 9, 4),
    ("zero_max_char", "abcdef", 0, 0),
    ("zero_max_char_offset", "abcdef", 3, 0),
    ("empty_output", "", 0, 0),
    ("empty_output_budget", "", 0, 5),
    ("empty_output_offset", "", 1, 0),
    ("unicode_head", "\u6c49\u5b57abc", 0, 2),
    ("unicode_middle", "\u6c49\u5b57abc", 1, 2),
    ("unicode_tail", "\u6c49\u5b57abc", 2, 100),
    ("unicode_astral", "\U0001f30d\U0001f30e\U0001f30f", 1, 1),
    ("newline_head", "a\nb\nc\n", 0, 2),
    ("newline_middle", "a\nb\nc\n", 2, 3),
    ("tabs", " 1\ta\n 2\tb\n", 0, 4),
]

TRUNCATE_CASES = [
    ("empty_zero", "", 0),
    ("empty_ten", "", 10),
    ("short_a", "a", 0),
    ("short_a_one", "a", 1),
    ("at_budget", "abc", 3),
    ("over_budget", "abc", 2),
    ("way_over", "abc", 0),
    ("nl_only_budget", "a\n", 1),
    ("nl_raised", "abcdef\n", 3),
    ("nl_raised_two", "abcdef\n", 4),
    ("nl_raised_exact", "abcdef\n", 5),
    ("nl_raised_six", "abcdef\n", 6),
    ("crlf_raised", "x" * 50 + "\r\n", 5),
    ("crlf_raised_one", "x" * 50 + "\r\n", 1),
    ("blank_run", "line\n\n", 2),
    ("multi_nl", "abc\n\n\n", 3),
    ("cjk", "\u6c49\u5b57\u6c49\u5b57", 3),
    ("cjk_nl", "\u6c49\u5b57\u6c49\u5b57\n", 4),
    ("accented", "\u00e9" * 10 + "\r", 6),
    ("max_line_length", "0" * 4005, 4000),
    ("at_max_line_length", "A" * 4000, 4000),
    ("one_over", "A" * 4001, 4000),
    ("tab_end", "tab\t\n", 2),
    ("only_newlines", "\n\n\n", 1),
    ("space_budget", "abc def", 4),
]

SPLIT_CASES = [
    ("empty", b""),
    ("single_char", b"a"),
    ("lf_terminated", b"a\n"),
    ("two_lines", b"a\nb"),
    ("two_lines_lf", b"a\nb\n"),
    ("crlf", b"a\r\nb\r\nc"),
    ("crlf_trailing", b"a\r\n"),
    ("lone_cr", b"a\rb\rc"),
    ("lone_cr_trailing", b"a\r"),
    ("mixed", b"a\n\r\nb\r"),
    ("only_lf", b"\n"),
    ("only_cr", b"\r"),
    ("only_crlf", b"\r\n"),
    ("double_lf", b"\n\n"),
    ("cr_lf_split", b"a\r\n\nb"),
    ("invalid_utf8", b"ab\xffcd\nnext\n"),
    ("truncated_sequence", b"\xc3"),
    ("truncated_2byte", b"\xe6\xb1"),
    ("surrogate_bytes", b"\xed\xa0\x80\n"),
    ("overlong", b"\xc0\xaf\n"),
    ("nul_byte", b"a\x00b\n"),
    ("utf8_bom", b"\xef\xbb\xbfBOM line\nsecond\n"),
    ("cjk", "\u6c49\u5b57\n\u4e16\u754c".encode()),
    ("astral", "\U0001f30d moon\n".encode()),
    ("vertical_tab", b"a\x0bb\nc"),
    ("form_feed", b"a\x0cb\nc"),
    ("nfc_crlf", "caf\u00e9\r\nna\u00efve\r\n".encode()),
    ("no_trailing_newline", b"x\ny\nz"),
    ("many_lines", b"".join(b"line %d\n" % i for i in range(50))),
    # Unicode "maximal subpart" cases for errors="replace".
    ("truncated_4byte", b"\xf0\x9f\x8c"),
    ("truncated_3byte_mid", b"a\xe6\xb1\nb"),
    ("bad_continuation", b"\xe6\xb1A"),
    ("bad_continuation_4byte", b"\xf0\x9fA"),
    ("valid_3byte", b"\xe6\xb1\x89\n"),
    ("valid_4byte", b"\xf0\x9f\x8c\x8d\n"),
    ("lead_then_ascii", b"\xe6A\n"),
    ("surrogate_then_ascii", b"\xed\xa0\x80A"),
    ("lone_continuation", b"\x80\x81\n"),
    ("ff_byte", b"a\xffb\n"),
]

HASH_CASES = [
    "line one\nline two\nline three",
    "a",
    "",
    "   \n  \nx=1\n\ny",
    "caf\u00e9 \u2192 unicode\nfoo",
    "a\r\nb\r\n",
    "\n",
    "\n\n",
    " \n \n",
    "\t\n",
    "abc\n\ndef\n  \nghi\n",
    "0\n0\n",
    "0\n1\n",
    "\u6c49\u5b57\n\u4e16\u754c\n",
    "\u00e9\u00e9\u00e9\n",
    "def foo():\n    return 1\n",
    "\x1b[31mred\x1b[0m\ntext\n",
    "x" * 300 + "\n" + "y" * 30,
    "a\nb\nc\nd\ne\nf\ng\nh\n",
    "  leading\n\ttab\n   \n",
    "\u3000\n\u3000\u3000\n",
    "\u00a0x\u00a0\n",
    # Python str.isspace() includes the file separators U+001C..U+001F.
    "\x1c\n",
    "a\x1cb\n",
    "\x1c\x1d\x1e\x1f\n",
    "x\x1cy\nz\n",
    # Non-ASCII non-alnum content: str.isalnum() is False, so the seed is the
    # 1-based line number (the alnum table must not claim these are word chars).
    "\U0001f30d\n",
    "\U0001f30d\n\U0001f30d\n",
    "x\n\U0001f30d\n",
    "\u2014\n",
    "\u2192\n",
    "\U0001f600 test\n",
    "\u4e16\u754c\n\U0001f30d\n",
]

CPU_CASES = [
    (
        "samples_and_deltas",
        {
            "nodes": [
                {"id": 1, "callFrame": {"functionName": "(root)", "url": "", "lineNumber": -1}},
                {"id": 2, "parent": 1, "callFrame": {"functionName": "compute", "url": "core.js", "lineNumber": 42}},
                {"id": 3, "parent": 2, "callFrame": {"functionName": "inner", "url": "core.js", "lineNumber": 44}},
                {"id": 4, "parent": 1, "callFrame": {"functionName": "(idle)", "url": "", "lineNumber": -1}},
            ],
            "samples": [2, 2, 2, 3, 3, 4],
            "timeDeltas": [100, 100, 200, 300, 100, 200],
            "startTime": 0,
            "endTime": 1000,
            "root": 1,
        },
    ),
    (
        "devtools_wrapper",
        {
            "profile": {
                "nodes": [
                    {"id": 1, "callFrame": {"functionName": "(root)", "url": "", "lineNumber": -1}},
                    {"id": 2, "parent": 1, "callFrame": {"functionName": "hot", "url": "app.js", "lineNumber": 7}},
                ],
                "samples": [2, 2, 2],
                "timeDeltas": [250, 250, 250],
                "startTime": 5000,
                "endTime": 5750,
            }
        },
    ),
    (
        "deep_tree_and_threshold",
        {
            "nodes": [
                {"id": 1, "callFrame": {"functionName": "(root)", "url": "", "lineNumber": -1}},
                {"id": 2, "parent": 1, "callFrame": {"functionName": "a", "url": "m.js", "lineNumber": 1}},
                {"id": 3, "parent": 2, "callFrame": {"functionName": "b", "url": "m.js", "lineNumber": 2}},
                {"id": 4, "parent": 3, "callFrame": {"functionName": "c", "url": "m.js", "lineNumber": 3}},
                {"id": 5, "parent": 4, "callFrame": {"functionName": "d", "url": "m.js", "lineNumber": 4}},
                {"id": 6, "parent": 5, "callFrame": {"functionName": "e", "url": "m.js", "lineNumber": 5}},
                {"id": 7, "parent": 6, "callFrame": {"functionName": "f", "url": "m.js", "lineNumber": 6}},
                {"id": 8, "parent": 7, "callFrame": {"functionName": "g", "url": "m.js", "lineNumber": 7}},
                {"id": 9, "parent": 8, "callFrame": {"functionName": "h", "url": "m.js", "lineNumber": 8}},
                {"id": 10, "parent": 9, "callFrame": {"functionName": "i", "url": "m.js", "lineNumber": 9}},
                {"id": 11, "parent": 10, "callFrame": {"functionName": "j", "url": "m.js", "lineNumber": 10}},
            ],
            "samples": [11] * 10,
            "timeDeltas": [100] * 10,
            "startTime": 0,
            "endTime": 1000,
        },
    ),
    (
        "duration_fallback",
        {
            "nodes": [
                {"id": 1, "callFrame": {"functionName": "(root)", "url": "", "lineNumber": -1}},
                {"id": 2, "parent": 1, "callFrame": {"functionName": "work", "url": "w.js", "lineNumber": 3}},
            ],
            "samples": [2, 2],
            "startTime": 100,
            "endTime": 500,
        },
    ),
    (
        "deltas_length_mismatch",
        {
            "nodes": [
                {"id": 1, "callFrame": {"functionName": "(root)", "url": "", "lineNumber": -1}},
                {"id": 2, "parent": 1, "callFrame": {"functionName": "work", "url": "w.js", "lineNumber": 3}},
            ],
            "samples": [2, 2],
            "timeDeltas": [100],
            "startTime": 0,
            "endTime": 900,
        },
    ),
    (
        "empty_samples_no_hitcount",
        {
            "nodes": [
                {"id": 1, "callFrame": {"functionName": "(root)", "url": "", "lineNumber": -1}},
                {"id": 2, "parent": 1, "callFrame": {"functionName": "never", "url": "n.js", "lineNumber": 1}},
            ],
            "samples": [],
            "startTime": 0,
            "endTime": 1000,
        },
    ),
    (
        "empty_samples_with_hitcount",
        {
            "nodes": [
                {"id": 1, "callFrame": {"functionName": "(root)", "url": "", "lineNumber": -1}, "hitCount": 0},
                {"id": 2, "parent": 1, "callFrame": {"functionName": "work", "url": "a.js", "lineNumber": 3}, "hitCount": 5},
            ],
            "samples": [],
            "timeDeltas": [],
            "startTime": 1000000,
            "endTime": 1004000,
        },
    ),
    (
        "no_root_key_promotes_by_name",
        {
            "nodes": [
                {"id": 1, "callFrame": {"functionName": "(root)", "url": "", "lineNumber": -1}},
                {"id": 2, "parent": 1, "callFrame": {"functionName": "child", "url": "c.js", "lineNumber": 2}},
            ],
            "samples": [2, 2, 2],
            "timeDeltas": [100, 100, 100],
            "startTime": 0,
            "endTime": 300,
        },
    ),
    (
        "root_not_in_nodes",
        {
            "nodes": [
                {"id": 1, "callFrame": {"functionName": "(root)", "url": "", "lineNumber": -1}},
                {"id": 2, "parent": 1, "callFrame": {"functionName": "child", "url": "c.js", "lineNumber": 2}},
            ],
            "samples": [2],
            "timeDeltas": [100],
            "startTime": 0,
            "endTime": 100,
            "root": 99,
        },
    ),
    (
        "anonymous_and_missing_url",
        {
            "nodes": [
                {"id": 1, "callFrame": {"functionName": "(root)", "url": "", "lineNumber": -1}},
                {"id": 2, "parent": 1, "callFrame": {"url": "anon.js", "lineNumber": 5}},
                {"id": 3, "parent": 2, "callFrame": {"functionName": "named"}},
            ],
            "samples": [3, 3, 2],
            "timeDeltas": [100, 100, 100],
            "startTime": 0,
            "endTime": 300,
        },
    ),
    (
        "orphan_parent_ignored",
        {
            "nodes": [
                {"id": 1, "callFrame": {"functionName": "(root)", "url": "", "lineNumber": -1}},
                {"id": 2, "parent": 777, "callFrame": {"functionName": "orphan", "url": "o.js", "lineNumber": 1}},
                {"id": 3, "parent": 1, "callFrame": {"functionName": "kid", "url": "k.js", "lineNumber": 2}},
            ],
            "samples": [2, 3, 3],
            "timeDeltas": [100, 100, 100],
            "startTime": 0,
            "endTime": 300,
        },
    ),
    (
        "root_is_string",
        {
            "nodes": [
                {"id": 1, "callFrame": {"functionName": "(root)", "url": "", "lineNumber": -1}},
                {"id": 2, "parent": 1, "callFrame": {"functionName": "child", "url": "c.js", "lineNumber": 2}},
            ],
            "samples": [2, 2, 2],
            "timeDeltas": [100, 100, 100],
            "startTime": 0,
            "endTime": 300,
            "root": "1",
        },
    ),
    (
        "root_is_null",
        {
            "nodes": [
                {"id": 1, "callFrame": {"functionName": "(root)", "url": "", "lineNumber": -1}},
                {"id": 2, "parent": 1, "callFrame": {"functionName": "child", "url": "c.js", "lineNumber": 2}},
            ],
            "samples": [2, 2, 2],
            "timeDeltas": [100, 100, 100],
            "startTime": 0,
            "endTime": 300,
            "root": None,
        },
    ),
    (
        "root_is_float_id",
        {
            "nodes": [
                {"id": 1, "callFrame": {"functionName": "(root)", "url": "", "lineNumber": -1}},
                {"id": 2, "parent": 1, "callFrame": {"functionName": "child", "url": "c.js", "lineNumber": 2}},
            ],
            "samples": [2, 2, 2],
            "timeDeltas": [100, 100, 100],
            "startTime": 0,
            "endTime": 300,
            "root": 1.0,
        },
    ),
    (
        "no_root_no_root_named_node",
        {
            "nodes": [
                {"id": 7, "callFrame": {"functionName": "alpha", "url": "a.js", "lineNumber": 1}},
                {"id": 8, "parent": 7, "callFrame": {"functionName": "beta", "url": "a.js", "lineNumber": 2}},
            ],
            "samples": [7, 8, 8],
            "timeDeltas": [100, 100, 100],
            "startTime": 0,
            "endTime": 300,
        },
    ),
    ("array_root", [1, 2, 3]),
    ("string_root", "hello"),
    ("nodes_not_array", {"nodes": "nope", "samples": []}),
    ("samples_not_array", {"nodes": [{"id": 1}], "samples": "nope"}),
    ("empty_nodes", {"nodes": [], "samples": []}),
    ("node_without_id", {"nodes": [{"callFrame": {"functionName": "x"}}], "samples": []}),
    ("profile_not_object", {"profile": "nope"}),
    (
        "float_deltas",
        {
            "nodes": [
                {"id": 1, "callFrame": {"functionName": "(root)", "url": "", "lineNumber": -1}},
                {"id": 2, "parent": 1, "callFrame": {"functionName": "f", "url": "f.js", "lineNumber": 1}},
            ],
            "samples": [2, 2, 2],
            "timeDeltas": [10.5, 20.25, 0],
            "startTime": 0,
            "endTime": 100,
        },
    ),
    (
        "negative_line_number",
        {
            "nodes": [
                {"id": 1, "callFrame": {"functionName": "(root)", "url": "root.js", "lineNumber": -1}},
                {"id": 2, "parent": 1, "callFrame": {"functionName": "f", "url": "f.js", "lineNumber": 0}},
            ],
            "samples": [2],
            "timeDeltas": [50],
            "startTime": 0,
            "endTime": 50,
        },
    ),
]

SAMPLE_PROFILE_CASES = [
    (
        "realistic_multi_thread",
        "Analysis of sampling MyApp (pid 4231) every 1 millisecond\n"
        "Process:         MyApp [4231]\n"
        "Path:            /Applications/MyApp.app/Contents/MacOS/MyApp\n"
        "Load Address:    0x100000000\n"
        "Identifier:      com.example.MyApp\n"
        "Version:         1.0 (1)\n"
        "Code Type:       ARM64\n"
        "Parent Process:  launchd [1]\n"
        "\n"
        "Call graph:\n"
        "    1000 Thread_4231   DispatchQueue_1: com.apple.main-thread  (serial)\n"
        "    + 1000 start  (in dyld) + 1903  [0x18a3c0000]\n"
        "    +   1000 main  (in MyApp) + 20  [0x100002000]\n"
        "    +     600 compute  (in MyApp) + 120  [0x100004000]\n"
        "    +     ! 600 _ZN5MyApp7computeEv  (in MyApp) + 44  [0x100004044]\n"
        "    +     400 __pthread_cond_wait  (in libsystem_pthread.dylib) + 32  [0x18a5b0000]\n"
        "    800 Thread_4242\n"
        "    + 800 _pthread_start  (in libsystem_pthread.dylib) + 100  [0x18a5b1000]\n"
        "    +   800 worker  (in MyApp) + 12  [0x100006000]\n"
        "    +     500 nanosleep  (in libsystem_c.dylib) + 8  [0x18a610000]\n"
        "    +     300 _RINvNtCs1234_5alloc  (in MyApp) + 4  [0x100006100]\n"
        "\n"
        "Total number in stack (recursive counted multiple, when >=5):\n"
        "        2000       start  (in dyld) + 1903\n"
        "        2000       main  (in MyApp) + 20\n",
    ),
    (
        "preamble_only",
        "Analysis of sampling MyApp (pid 1) every 1 millisecond\n"
        "Process:         MyApp [1]\n",
    ),
    (
        "call_graph_only_no_frames",
        "Call graph:\nTotal number in stack (recursive count):\n",
    ),
    (
        "thread_lines_start_with_thread",
        "Call graph:\n"
        "Thread_1\n"
        " + 1 alpha (in a.out) + 0 [0x1]\n"
        "Thread_2\n"
        " + 1 beta (in a.out) + 4 [0x2]\n",
    ),
    (
        "shallower_branch_resets_stack",
        "Analysis of sampling x 1 every 1 millisecond\n"
        "Call graph:\n"
        "    + 5 one (in a.out) + 1 [0x1]\n"
        "    +   5 two (in a.out) + 2 [0x2]\n"
        "    +     5 three (in a.out) + 3 [0x3]\n"
        "    +   5 two_b (in a.out) + 4 [0x4]\n",
    ),
    (
        "module_with_suffix",
        "Call graph:\n"
        "    + 7 foo (in bar.dylib) + 12 + 13\n"
        "    + 3 baz  (in  qux ) + 1\n",
    ),
    (
        "unknown_frame_shapes",
        "Call graph:\n"
        "    + 1 bare_symbol\n"
        "    + 1 sym + 8\n"
        "    + 1 ??  (in ???) + 0 [0x0]\n"
        "    + 1 _ZN3foo3barEv (in a.out) + 1\n"
        "    + 1 _ZNbadE (in a.out) + 1\n",
    ),
    (
        "unrecognized",
        "just some text\nno profile here\n",
    ),
    (
        "empty",
        "",
    ),
]

MARKDOWN_CASES = [
    "plain text",
    "",
    "# Heading\n\nSome **bold** and *italic* text.",
    "## Sub __bold__ heading\n\nlist:\n- a\n- b",
    "```python\nprint(1)\nprint(2)\n```\n\nafter",
    "```\nline\n```",
    "```unclosed\nline\n",
    "See `foo_bar` and _emphasis_ but not foo_bar baz.",
    "`a` `b` `c`",
    "`` empty-ish ``",
    "**bold**",
    "***both***",
    "***a**",
    "**a*b**",
    "__a_b__",
    "___a__",
    "_a_b_",
    "_under_ _score_",
    "prefix_under_suffix",
    "\u6c49\u5b57_italic_\u6c49\u5b57",
    "[link](http://example.com)",
    "[link](http://example.com) and [two](b)",
    "[a]()",
    "[]()",
    "[a](b)c)",
    "[a[b]](c)",
    "![alt](img.png)",
    "![](img.png)",
    "![]()",
    "! [x](y)",
    "####",
    "#### title",
    "# ",
    "#\n## x",
    "# \nx",
    "####\ny",
    "a\n####\nb",
    "##\n",
    "text\n#\nmore",
    "###\t \nx",
    "#\t#",
    "\n#\n\n#\n",
    "---",
    "---\n\ntext\n\n---",
    "a\n---\nb",
    "a\n--\nb",
    "a\n-----\nb",
    "a\n- - -\nb",
    "x\n\n\n\ny",
    "  \n\nx\n\n\n\n\ny\n\n  ",
    "# Intro\n\nSome **bold** and *italic* and `code`.\n\n"
    "- item one\n- item two\n\n"
    "```py\nx = 1\n```\n\n"
    "[docs](https://example.com/docs) and ![pic](p.png)\n\n"
    "---\n\n## Details\n\n| a | b |\n| - | - |\n| 1 | 2 |\n",
    "line with trailing spaces   \nnext line\t\n",
    "**multi\nline**",
    "*multi\nline*",
    "_multi\nline_",
    "a *b* c *d* e",
    "\u00e9**\u00e9**\u00e9",
    # Python \w is [alnum_] only: connector punctuation (U+203F) is *not* a
    # word character, so the underscore emphasis lookarounds must fire.
    "a\u203f_b_",
    "\u203f_a_",
    "x\u2040_y_",
    # Python \s (and str.strip) treat U+001C..U+001F as whitespace.
    "\x1cplain\x1c",
    "#\x1cx",
    "a\x1c_b_",
    # The inline-code placeholder is "\x00CODE{n}\x00": the leading NUL is not
    # whitespace, so a heading pass / strip must not eat it (a plain
    # "\x00CODE" literal would decay to "\x0CODE" and 0x0C *is* whitespace).
    "#`x`",
    "#`CODE0` x",
    "`\u5b57`",
    "# `x`",
    "##`a b`",
    "`x`\n#`y`",
    # Emoji are not \w for the reference `regex` module either, so the
    # underscore lookarounds must fire next to them.
    "\U0001f30d_a_",
    "_a_\U0001f30d",
    "x_(\U0001f30d)_y",
    "\u2014_a_",
    "a_\u2014",
]

TOOL_CASES = [
    ("tool_sample_default", b"line1\nline2\nline3\n", 1, 10, 16000, 0, True),
    ("tool_no_final_newline", b"a\nb\nc", 1, 2000, 16000, 0, True),
    ("tool_crlf", b"a\r\nb\r\nc\r\n", 1, 2000, 16000, 0, True),
    ("tool_lone_cr", b"a\rb\rc", 1, 2000, 16000, 0, True),
    ("tool_tail", b"a\nb\nc\nd\n", -2, 2000, 16000, 0, True),
    ("tool_tail_beyond", b"a\nb\n", -9, 2000, 16000, 0, True),
    ("tool_offset_limit", b"".join(b"line %d\n" % i for i in range(1, 11)), 4, 3, 16000, 0, True),
    ("tool_char_window_head", b"0123456789ABCDEFGHIJ\n", 1, 2000, 5, 0, False),
    ("tool_char_window_middle", b"0123456789ABCDEFGHIJ\n", 1, 2000, 5, 3, False),
    ("tool_char_window_tail", b"0123456789ABCDEFGHIJ\n", 1, 2000, 100, 8, False),
    ("tool_char_window_zero", b"0123456789\n", 1, 2000, 0, 0, False),
    ("tool_unicode", "Hello \u4e16\u754c \U0001f30d\nUnicode: caf\u00e9\n".encode(), 1, 2000, 16000, 0, True),
    ("tool_invalid_utf8", b"ok\nbad \xff\xfe bytes\n", 1, 2000, 16000, 0, True),
    ("tool_empty_file", b"", 1, 2000, 16000, 0, True),
    ("tool_truncation", (b"A" * 4100 + b"\n" + b"short\n" + b"B" * 4100), 1, 2000, 16000, 0, True),
    ("tool_long_line_no_numbers", (b"x" * 4100 + b"\n"), 1, 2000, 16000, 0, False),
    ("tool_crlf_unicode_tail", "caf\u00e9\r\n\u4e16\u754c\r\nend\r\n".encode(), -2, 2000, 16000, 0, True),
    ("tool_show_numbers_false_tail", b"a\nb\nc\n", -2, 2000, 16000, 0, False),
]


# ---------------------------------------------------------------------------
# deterministic fuzz corpora (fixed seed -> reproducible goldens)
# ---------------------------------------------------------------------------

FUZZ_SEED = 20240613

_TEXT_ATOMS = [
    "a", "b", "Z", "0", "9", " ", "\t", "\u00e9", "\u6c49", "\u5b57",
    "\U0001f30d", "_", "*", "#", "-", "`", "[", "]", "(", ")", "!", "'",
    "\"", "\\", "+", "|", "\x1c", "\u3000", "\u00a0", "line", "x" * 40,
]
_BYTES_ATOMS = [
    b"a", b"b", b"Z", b"0", b" ", b"\t", b"\r", b"\n", b"\r\n",
    "\u00e9".encode(), "\u6c49".encode(),
    b"\xff", b"\xc3", b"\xe6\xb1", b"\xf0\x9f\x8c", b"\x80", b"\xed\xa0\x80",
    b"word", b"-", b"", b"\x0b", b"\x0c", b"\x1c",
]


def fuzz_split_cases(count=120):
    rnd = random.Random(FUZZ_SEED)
    cases = []
    for i in range(count):
        parts = [rnd.choice(_BYTES_ATOMS) for _ in range(rnd.randint(0, 12))]
        cases.append(("fuzz_split_%d" % i, b"".join(parts)))
    return cases


def fuzz_render_cases(count=120):
    rnd = random.Random(FUZZ_SEED + 1)
    cases = []
    for i in range(count):
        lines = []
        for _ in range(rnd.randint(0, 6)):
            text = "".join(rnd.choice(_TEXT_ATOMS)
                           for _ in range(rnd.randint(0, 5)))
            lines.append(text + rnd.choice(["\n", "\n", "\r\n", "", "\r"]))
        offset = rnd.choice([-6, -3, -2, -1, 1, 2, 3, 6])
        n_lines = rnd.choice([1, 2, 3, 5, 10])
        show = rnd.random() < 0.8
        note = rnd.choice(["", "", " (note)"])
        cases.append(("fuzz_render_%d" % i, lines, "fuzz.txt", offset, n_lines,
                      show, note))
    return cases


def fuzz_char_window_cases(count=60):
    rnd = random.Random(FUZZ_SEED + 2)
    cases = []
    for i in range(count):
        text = "".join(rnd.choice(_TEXT_ATOMS)
                       for _ in range(rnd.randint(0, 25)))
        char_offset = rnd.randint(0, len(text))
        max_char = rnd.choice([0, 1, 2, 3, 5, 10, 100])
        cases.append(("fuzz_window_%d" % i, text, char_offset, max_char))
    return cases


def fuzz_markdown_cases(count=150):
    rnd = random.Random(FUZZ_SEED + 3)
    cases = []
    for i in range(count):
        text = "".join(rnd.choice(_TEXT_ATOMS)
                       for _ in range(rnd.randint(0, 30)))
        if rnd.random() < 0.4:
            text = "\n" * rnd.randint(1, 4) + text + "\n" * rnd.randint(0, 4)
        cases.append(("fuzz_markdown_%d" % i, text))
    return cases


_SAMPLE_FRAMES = [
    "start  (in dyld) + 1903",
    "main (in MyApp) + 20",
    "_ZN5MyApp7computeEv (in MyApp) + 44",
    "__pthread_cond_wait (in libsystem_pthread.dylib) + 32",
    "?? (in ???) + 0",
    "bare_symbol",
    "sleep + 4",
    "_RINvNtCs1234_5alloc (in MyApp) + 4",
    "foo (in bar) + 12 + 13",
    "worker  (in  MyApp ) + 1",
]


def fuzz_cpu_cases(count=40):
    """Random well-formed V8 profiles (parents only point at earlier ids, so the
    tree is acyclic — a cycle would hang the C++ aggregation)."""
    rnd = random.Random(FUZZ_SEED + 5)
    cases = []
    for i in range(count):
        n_nodes = rnd.randint(1, 12)
        ids = rnd.sample(range(1, 40), n_nodes)
        nodes = []
        for j, node_id in enumerate(ids):
            node = {"id": node_id, "callFrame": {
                "functionName": rnd.choice(["", "f", "g", "(root)", "(idle)", "hot", "x_y"])}}
            if rnd.random() < 0.8:
                node["callFrame"]["url"] = rnd.choice(["a.js", "", "b.js"])
            if rnd.random() < 0.8:
                node["callFrame"]["lineNumber"] = rnd.choice([-1, 0, 5, 42])
            if j > 0 and rnd.random() < 0.8:
                node["parent"] = rnd.choice(ids[:j])
            if rnd.random() < 0.3:
                node["hitCount"] = rnd.randint(0, 5)
            nodes.append(node)
        samples = [rnd.choice(ids) for _ in range(rnd.randint(0, 8))]
        profile = {"nodes": nodes, "samples": samples}
        if rnd.random() < 0.7:
            profile["timeDeltas"] = [rnd.choice([0, 50, 100, 250])
                                     for _ in samples]
        else:
            start = rnd.randint(0, 1000)
            profile["startTime"] = start
            profile["endTime"] = start + rnd.randint(0, 5000)
        if rnd.random() < 0.4:
            profile["root"] = rnd.choice(ids)
        cases.append(("fuzz_cpu_%d" % i, profile))
    return cases


def fuzz_sample_cases(count=25):
    rnd = random.Random(FUZZ_SEED + 6)
    cases = []
    for i in range(count):
        lines = ["Analysis of sampling x 1 every 1 millisecond",
                 "Process: x [1]", "", "Call graph:"]
        for _ in range(rnd.randint(0, 8)):
            if rnd.random() < 0.15:
                lines.append("%d Thread_%d" % (rnd.randint(1, 99),
                                               rnd.randint(1, 9)))
            else:
                indent = " " * (4 + 2 * rnd.randint(0, 4))
                decorator = rnd.choice(["+", "!", ":", "|"])
                lines.append("%s%s %s" % (indent, decorator,
                                          rnd.choice(_SAMPLE_FRAMES)))
        cases.append(("fuzz_sample_%d" % i, chr(10).join(lines) + chr(10)))
    return cases

def fuzz_hash_cases(count=50):
    rnd = random.Random(FUZZ_SEED + 4)
    cases = []
    for i in range(count):
        lines = []
        for _ in range(rnd.randint(0, 5)):
            lines.append("".join(rnd.choice(_TEXT_ATOMS)
                                 for _ in range(rnd.randint(0, 8))))
        text = "\n".join(lines)
        if rnd.random() < 0.5:
            text += "\n"
        cases.append(("fuzz_hash_%d" % i, text))
    return cases


# ---------------------------------------------------------------------------
# expected-value computation
# ---------------------------------------------------------------------------


def compute_validate(ref: Ref):
    out = []
    for name, value in [
        ("offset", -5001), ("offset", -5000), ("offset", -4999), ("offset", -1),
        ("offset", 0), ("offset", 1), ("offset", 2), ("offset", 4999),
        ("offset", 5000), ("offset", 5001), ("offset", -100000),
        ("limit", -1), ("limit", 0), ("limit", 1), ("limit", 2), ("limit", 5000),
        ("max_char", -1), ("max_char", 0), ("max_char", 1), ("max_char", 16000),
        ("char_offset", -1), ("char_offset", 0), ("char_offset", 1),
        ("char_offset", 100),
    ]:
        try:
            ref.read.Params._validate_value(name, value)
            out.append((name, value, True, ""))
        except ValueError as exc:
            out.append((name, value, False, str(exc)))
    return out


def compute_truncate(ref: Ref):
    return [
        (name, text, max_len, ref.tools_utils.truncate_line(text, max_len))
        for name, text, max_len in TRUNCATE_CASES
    ]


def compute_split():
    cases = list(SPLIT_CASES) + fuzz_split_cases()
    return [(name, data, py_lines_from_bytes(data)) for name, data in cases]


def compute_render(ref: Ref, cases):
    async def run_all():
        results = []
        for (name, lines, path, offset, n_lines, show, note) in cases:
            res, window, start, captured = await _render(
                ref, lines, path, offset, n_lines, show, note
            )
            results.append((name, lines, path, offset, n_lines, show, note,
                            res, window, start, captured))
        return results

    return asyncio.run(run_all())


def compute_char_window(ref: Ref):
    out = []
    for name, output, char_offset, max_char in list(CHAR_WINDOW_CASES) +             fuzz_char_window_cases():
        res = ref.ToolOk(output=output, message="", brief="Read file")
        res = ref.read._apply_char_window(res, char_offset, max_char)
        out.append((name, output, char_offset, max_char, res.output, res.message))
    return out


def compute_hashes(ref: Ref):
    out = []
    for name, content in [("case_%d" % i, c) for i, c in enumerate(HASH_CASES)] +             fuzz_hash_cases():
        lines = content.split("\n")
        if content.endswith("\n"):
            lines.pop()
        if content == "":
            lines = []
        out.append((name, content, ref.hash_line._cumulative_hashes(lines)))
    return out


def compute_cpu(ref: Ref):
    """CPU profile goldens.

    One corpus shape (`samples == []` with an int `hitCount` on a node) makes
    the reference raise AttributeError at read_profiles.py:213 (`n.hitCount` on
    a dict) while rendering its error text. Those cases are recorded in
    `python_raises` and their expectation comes from a *source-patched* copy of
    read_profiles.py (`n.hitCount` -> `n.get("hitCount", 0)`) that implements
    the clearly intended behavior; the C++ kernel implements that intent (see
    reports/read.md deviation 3).
    """
    patched = patched_cpu_renderer(ref.read_profiles)
    out, raises = [], []
    for name, payload in list(CPU_CASES) + fuzz_cpu_cases():
        text = payload if isinstance(payload, str) else json.dumps(payload)
        try:
            expected = ref.read_profiles.render_cpu_profile(text)
        except Exception as exc:
            raises.append((name, text, "%s: %s" % (type(exc).__name__, exc)))
            expected = patched(text)
        out.append((name, text, expected is not None, expected or ""))
    return out, raises


def patched_cpu_renderer(read_profiles):
    src = Path(read_profiles.__file__).read_text(encoding="utf-8")
    needle = ("n.hitCount for n in nodes if isinstance(n, dict) "
              "and isinstance(n.get(\"hitCount\"), int)")
    assert needle in src, "read_profiles.py sample_count expression changed"
    fixed = src.replace(
        needle,
        "n.get(\"hitCount\", 0) for n in nodes if isinstance(n, dict) "
        "and isinstance(n.get(\"hitCount\"), int)")
    # The hitCount fallback gate is truthiness-based
    # (`all(... n.get("hitCount") ...)`), so one node with hitCount 0 disables
    # it. The C++ kernel requires "every node carries an int hitCount" - the
    # intended reading, and what the pinned cpu_profile_hitcount_fallback test
    # asserts - so patch that too and keep the expectation consistent with the
    # documented deviation 3.
    fallback_old = ("all(isinstance(n, dict) and n.get(\"hitCount\") "
                    "for n in nodes)")
    fallback_new = ("all(isinstance(n, dict) and "
                    "isinstance(n.get(\"hitCount\"), int) for n in nodes)")
    assert fallback_old in fixed, "hitCount fallback gate changed"
    fixed = fixed.replace(fallback_old, fallback_new, 1)
    mod = types.ModuleType("read_profiles_intended")
    mod.__file__ = read_profiles.__file__
    sys.modules["read_profiles_intended"] = mod
    exec(compile(fixed, "<read_profiles_intended>", "exec"), mod.__dict__)
    return mod.render_cpu_profile


def compute_sample(ref: Ref):
    out = []
    for name, text in list(SAMPLE_PROFILE_CASES) + fuzz_sample_cases():
        expected = ref.read_profiles.render_sample_profile(text)
        out.append((name, text, expected is not None, expected or ""))
    return out


def compute_markdown(ref: Ref):
    out = [("case_%d" % i, text, ref.read_markit.markdown_to_text(text))
           for i, text in enumerate(MARKDOWN_CASES)]
    out += [(name, text, ref.read_markit.markdown_to_text(text))
            for name, text in fuzz_markdown_cases()]
    return out


def compute_tool(ref: Ref):
    async def run_all():
        results = []
        for (name, data, offset, limit, max_char, char_offset, show) in TOOL_CASES:
            lines = py_lines_from_bytes(data)
            res, _window, _start, captured = await _render(
                ref, lines, "sample.txt", offset, limit, show, ""
            )
            res = ref.read._apply_char_window(res, char_offset, max_char)
            results.append((name, data, offset, limit, max_char, char_offset,
                            show, res.output, res.message, captured))
        return results

    return asyncio.run(run_all())


# ---------------------------------------------------------------------------
# emission
# ---------------------------------------------------------------------------


def render_entry(name, lines, path, offset, n_lines, show, note, res, window,
                 captured) -> str:
    truncated = [c[0] for c in captured["candidates"] if c[2]]
    total_lines = captured["total_lines"]

    def encode(items):
        """(literal, spec, total line bytes) - big uniform lists become a recipe."""
        byte_len = sum(len(line.encode("utf-8")) for line in items)
        spec, expanded = make_spec(items)
        assert expanded == items, "input recipe does not reproduce the corpus"
        if spec and byte_len > 20000:
            return "", spec, byte_len
        return join01(items), "", byte_len

    lines_payload, lines_spec, input_len = encode(lines)
    window_payload, window_spec, window_len = encode(list(window))
    return (
        "    {%s, %s, %s, %d, %d, %d, %s,\n"
        "     %s,\n"
        "     %s,\n"
        "     %s, %d, %d, %d, %d, %d,\n"
          "     %s, %d, %d, %s, %d, %d, %s},\n"
          % (
              lit(name), lit(lines_payload), lit(path), offset, n_lines,
              1 if show else 0, lit(note),
              lit(res.output),
              lit(res.message),
              lit(window_payload),
              captured["start_line"],
              -1 if total_lines is None else total_lines,
              1 if captured["max_lines_reached"] else 0,
              1 if captured["max_bytes_reached"] else 0,
              1 if captured["end_of_file"] else 0,
              lit(fmt_ints(truncated)),
              input_len,
              len(lines),
              lit(lines_spec),
              window_len,
              len(list(window)),
              lit(window_spec),
          )
      )


RENDER_STRUCT = (
    "// _render_forward / _render_tail / _render_result golden\n"
    "struct rd_g_render {\n"
    "    const char *name;\n"
    "    const char *lines;        // input lines joined with '\\x01'\n"
    "    const char *display_path;\n"
    "    long long offset;\n"
    "    long long n_lines;\n"
    "    int show_line_numbers;\n"
    "    const char *note;\n"
    "    const char *output;\n"
    "    const char *message;\n"
    "    const char *window;       // raw window lines joined with '\\x01'\n"
    "    long long start_line;\n"
    "    long long total_lines;    // -1 == None\n"
    "    int max_lines_reached;\n"
    "    int max_bytes_reached;\n"
    "    int end_of_file;\n"
    "    const char *truncated;    // space separated line numbers\n"
    "    long long input_len;      // total UTF-8 bytes of the input lines\n"
    "    long long input_lines;    // number of input lines\n"
    "    const char *lines_spec;   // recipe used when `lines` is empty\n"
    "    long long window_len;     // total UTF-8 bytes of the window lines\n"
    "    long long window_lines;   // number of window lines\n"
    "    const char *window_spec;  // recipe used when `window` is empty\n"
    "};\n"
    "\n"
)

TOOL_STRUCT = (
    "// Full text-mode tool path (split_lines + render + char window + message)\n"
    "struct rd_g_tool {\n"
    "    const char *name;\n"
    "    const char *content;      // raw file bytes (split with universal newlines)\n"
    "    long long offset;\n"
    "    long long limit;\n"
    "    long long max_char;\n"
    "    long long char_offset;\n"
    "    int show_line_numbers;\n"
    "    const char *output;\n"
    "    const char *message;\n"
    "    long long start_line;\n"
    "    long long total_lines;\n"
    "    int max_lines_reached;\n"
    "    int max_bytes_reached;\n"
    "    int end_of_file;\n"
    "    const char *truncated;\n"
    "};\n"
    "\n"
)


def generate(ref: Ref) -> str:
    parts: list[str] = []

    parts.append(
        "// Parameter validation (read.py::Params._validate_value, 278-294)\n"
        "struct rd_g_validate {\n"
        "    const char *name;\n"
        "    long long value;\n"
        "    int ok;\n"
        "    const char *message;  // Python ValueError text when ok == 0\n"
        "};\n"
        "\n"
        "const rd_g_validate rd_validate_goldens[] = {\n"
    )
    for name, value, ok, message in compute_validate(ref):
        parts.append("    {%s, %d, %d, %s},\n"
                     % (lit(name), value, 1 if ok else 0, lit(message)))
    parts.append("};\n\n")

    parts.append(
        "// truncate_line (kimi_cli/tools/utils.py, the \"...\" variant read.py imports)\n"
        "struct rd_g_truncate {\n"
        "    const char *name;\n"
        "    const char *text;\n"
        "    long long max_len;\n"
        "    const char *expected;\n"
        "};\n"
        "\n"
        "const rd_g_truncate rd_truncate_goldens[] = {\n"
    )
    for name, text, max_len, expected in compute_truncate(ref):
        parts.append("    {%s, %s, %d, %s},\n"
                     % (lit(name), lit(text), max_len, lit(expected)))
    parts.append("};\n\n")

    parts.append(
        "// Universal-newline line splitting (aiofiles text-mode readlines)\n"
        "struct rd_g_split {\n"
        "    const char *name;\n"
        "    const char *input;      // raw file bytes\n"
        "    const char *expected;   // lines joined with '\\x01'\n"
        "};\n"
        "\n"
        "const rd_g_split rd_split_goldens[] = {\n"
    )
    for name, data, lines in compute_split():
        parts.append("    {%s, %s, %s},\n"
                     % (lit(name), lit(data), lit(join01(lines))))
    parts.append("};\n\n")

    forward, tail = [], []
    for case in compute_render(ref, render_corpus()):
        (name, lines, path, offset, n_lines, show, note, res, window, _start,
         captured) = case
        entry = render_entry(name, lines, path, offset, n_lines, show, note,
                             res, window, captured)
        (tail if offset < 0 else forward).append(entry)

    parts.append(RENDER_STRUCT)
    parts.append("const rd_g_render rd_forward_goldens[] = {\n")
    for entry in forward:
        parts.append(entry)
    parts.append("};\n\n")
    parts.append("const rd_g_render rd_tail_goldens[] = {\n")
    for entry in tail:
        parts.append(entry)
    parts.append("};\n\n")

    parts.append(
        "// _apply_char_window (read.py 349-385)\n"
        "struct rd_g_charwin {\n"
        "    const char *name;\n"
        "    const char *output;\n"
        "    long long char_offset;\n"
        "    long long max_char;\n"
        "    const char *expected_output;\n"
        "    const char *expected_note;\n"
        "};\n"
        "\n"
        "const rd_g_charwin rd_charwin_goldens[] = {\n"
    )
    for (name, output, char_offset, max_char, exp_out,
         exp_note) in compute_char_window(ref):
        parts.append(
            "    {%s, %s, %d, %d,\n     %s,\n     %s},\n"
            % (lit(name), lit(output), char_offset, max_char, lit(exp_out),
               lit(exp_note))
        )
    parts.append("};\n\n")

    parts.append(
        "// hash_line::_cumulative_hashes (2-char nibble strings, space separated)\n"
        "struct rd_g_hash {\n"
        "    const char *name;\n"
        "    const char *input;\n"
        "    const char *expected;\n"
        "};\n"
        "\n"
        "const rd_g_hash rd_hash_goldens[] = {\n"
    )
    for name, content, hashes in compute_hashes(ref):
        parts.append("    {%s, %s, %s},\n"
                     % (lit(name), lit(content), lit(" ".join(hashes))))
    parts.append("};\n\n")

    profile_struct = (
        "// render_cpu_profile / render_sample_profile / markdown_to_text\n"
        "struct rd_g_text {\n"
        "    const char *name;\n"
        "    const char *input;\n"
        "    int ok;             // 0 == Python returned None (unsupported)\n"
        "    const char *expected;\n"
        "};\n"
        "\n"
    )
    parts.append(profile_struct)
    cpu_goldens, cpu_raises = compute_cpu(ref)
    parts.append("const rd_g_text rd_cpu_goldens[] = {\n")
    for name, text, ok, expected in cpu_goldens:
        parts.append("    {%s, %s, %d,\n     %s},\n"
                     % (lit(name), lit(text), 1 if ok else 0, lit(expected)))
    parts.append("};\n\n")

    parts.append(
        "// Inputs where the *Python* reference raises instead of returning a\n"
        "// summary (read_profiles.py:213 reads `n.hitCount` on a dict). The C++\n"
        "// kernel implements the intended `n.get(\"hitCount\", 0)` semantics; the\n"
        "// expected summary for these inputs is the matching rd_cpu_goldens row\n"
        "// (produced by a source-patched copy of read_profiles.py).\n"
        "struct rd_g_raise {\n"
        "    const char *name;\n"
        "    const char *input;\n"
        "    const char *exception_text;\n"
        "};\n"
        "\n"
        "const rd_g_raise rd_cpu_python_raises[] = {\n"
    )
    for name, text, exception_text in cpu_raises:
        parts.append("    {%s, %s,\n     %s},\n"
                     % (lit(name), lit(text), lit(exception_text)))
    parts.append("};\n\n")

    parts.append("const rd_g_text rd_sample_goldens[] = {\n")
    for name, text, ok, expected in compute_sample(ref):
        parts.append("    {%s, %s, %d,\n     %s},\n"
                     % (lit(name), lit(text), 1 if ok else 0, lit(expected)))
    parts.append("};\n\n")

    parts.append(
        "struct rd_g_markdown {\n"
        "    const char *name;\n"
        "    const char *input;\n"
        "    const char *expected;\n"
        "};\n"
        "\n"
        "const rd_g_markdown rd_markdown_goldens[] = {\n"
    )
    for name, text, expected in compute_markdown(ref):
        parts.append("    {%s, %s,\n     %s},\n"
                     % (lit(name), lit(text), lit(expected)))
    parts.append("};\n\n")

    parts.append(TOOL_STRUCT)
    parts.append("const rd_g_tool rd_tool_goldens[] = {\n")
    for (name, data, offset, limit, max_char, char_offset, show, output, message,
         captured) in compute_tool(ref):
        truncated = [c[0] for c in captured["candidates"] if c[2]]
        total_lines = captured["total_lines"]
        parts.append(
            "    {%s, %s, %d, %d, %d, %d, %d,\n"
            "     %s,\n"
            "     %s,\n"
            "     %d, %d, %d, %d, %d,\n"
            "     %s},\n"
            % (
                lit(name), lit(data), offset, limit, max_char, char_offset,
                1 if show else 0,
                lit(output), lit(message),
                captured["start_line"],
                -1 if total_lines is None else total_lines,
                1 if captured["max_lines_reached"] else 0,
                1 if captured["max_bytes_reached"] else 0,
                1 if captured["end_of_file"] else 0,
                lit(fmt_ints(truncated)),
            )
        )
    parts.append("};\n")

    header = (
        "// GENERATED by scripts/gen_read_goldens.py - DO NOT EDIT BY HAND.\n"
        "//\n"
        "// Every expected value comes from running the real Python implementation\n"
        "// in the kimi-agent checkout (kimi_cli.tools.file.read / hash_line /\n"
        "// read_profiles / read_markit plus kimi_cli.tools.utils.truncate_line and\n"
        "// the Python text-mode reader). Regenerate with:\n"
        "//     python scripts/gen_read_goldens.py\n"
        "//\n"
        "// Conventions:\n"
        "//   * fields holding several strings join them with '\\x01'\n"
        "//   * 'truncated' holds space separated line numbers\n"
        "//   * all bytes outside printable ASCII use 3-digit octal escapes, so the\n"
        "//     file is pure ASCII (no UTF-8 BOM needed) and the escapes are never\n"
        "//     ambiguous with a following hex digit\n"
        "\n"
    )
    return header + "".join(parts)


def alnum_table_text() -> str:
    """The `rd_alnum_ranges` initializer (Python str.isalnum = L* + N*).

    NOTE: the table that shipped in read_tool.cpp was corrupt from cp 0x66F on
    (91 values missing, so every later pair was shifted and the binary search
    returned true for huge fake ranges - e.g. emoji counted as alphanumeric).
    Regenerating it from unicodedata is the only way to keep it exact; the
    runtime twin in src/runtime/tools/line_hash.cpp has the same corruption.
    """
    ranges = []
    start = None
    for cp in range(0x110000):
        if 0xD800 <= cp <= 0xDFFF:
            ok = False
        else:
            ok = chr(cp).isalnum()
        if ok and start is None:
            start = cp
        elif not ok and start is not None:
            ranges.append((start, cp - 1))
            start = None
    if start is not None:
        ranges.append((start, 0x10FFFF))
    assert all(a <= b for a, b in ranges)
    assert all(ranges[i - 1][1] < ranges[i][0] for i in range(1, len(ranges)))
    values = [v for pair in ranges for v in pair]
    lines = []
    for i in range(0, len(values), 12):
        chunk = values[i:i + 12]
        lines.append("    " + " ".join("0x%04X," % v for v in chunk))
    body = chr(10).join(lines)
    return ("// >>> BEGIN GENERATED:RD-ALNUM-TABLE >>>" + chr(10)
            + "constexpr uint32_t rd_alnum_ranges[][2] = {" + chr(10)
            + body + chr(10)
            + "};" + chr(10)
            + "// <<< END GENERATED:RD-ALNUM-TABLE <<<")


def write_alnum_table(cpp_path: Path) -> int:
    import re
    src = cpp_path.read_text(encoding="utf-8")
    pattern = re.compile(
        r"constexpr uint32_t rd_alnum_ranges\[\]\[2\] = \{.*?\n\};",
        re.S)
    match = pattern.search(src)
    if match is None:
        raise SystemExit("rd_alnum_ranges table not found in %s" % cpp_path)
    new = pattern.sub(lambda _m: alnum_table_text(), src, count=1)
    cpp_path.write_text(new, encoding="utf-8", newline=chr(10))
    return len(new) - len(src)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="regenerate read goldens")
    parser.add_argument("--reference", default=DEFAULT_REFERENCE,
                        help="kimi-agent checkout root")
    parser.add_argument("--out", default=str(DEFAULT_OUT),
                        help="output .inc path")
    parser.add_argument("--check", action="store_true",
                        help="exit 1 when the checked-in file is stale")
    parser.add_argument("--write-alnum-table", action="store_true",
                        help="regenerate the rd_alnum_ranges table in "
                             "src/builtin_tools/read_tool.cpp")
    args = parser.parse_args(argv)

    root = Path(args.reference)
    if not (root / "kimi-cli" / "src" / "kimi_cli").is_dir():
        parser.error("not a kimi-agent checkout: %s" % root)

    if args.write_alnum_table:
        cpp = Path("src/builtin_tools/read_tool.cpp")
        delta = write_alnum_table(cpp)
        print("rewrote rd_alnum_ranges in %s (%+d bytes)" % (cpp, delta))
        return 0

    ref = Ref(import_reference(root))
    text = generate(ref)
    out_path = Path(args.out)
    if args.check:
        if not out_path.is_file() or out_path.read_text(encoding="utf-8") != text:
            print("STALE: %s" % out_path, file=sys.stderr)
            return 1
        print("up to date: %s" % out_path)
        return 0
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("wrote %s (%d bytes)" % (out_path, len(text.encode("utf-8"))))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
