"""Regenerate tests/unit/builtin_tools/grep_goldens.inc (grep tool parity goldens).

Every golden in the generated file is produced by *running the reference
implementation* — the pure-Python modules of the kimi-agent checkout
(``kimi-cli/src/kimi_cli/tools/file/grep_*.py``, ``output_utils.py``,
``utils/sensitive.py``) — never by hand and never from the kimix-base
``kimix_native`` mirrors (those would mask a mistake shared with the port).

Usage::

    python scripts/gen_grep_goldens.py            # rewrite the .inc
    python scripts/gen_grep_goldens.py --check     # fail when stale
    python scripts/gen_grep_goldens.py --reference C:/dev/kimi-agent

Encoding exercised by the .inc (see the helper block in test_grep_tool.cpp):

* each case is a ``{ input, want }`` pair of C++ string literals;
* ``input`` is a TAB-separated field list; the bytes of every field are
  escaped with the 4-escape scheme (``\\\\`` -> backslash, ``\\n`` -> LF,
  ``\\r`` -> CR, ``\\t`` -> TAB), so a TAB byte is always a field separator;
* ``want`` is a JSON value (string / number / bool / null / array / object)
  rendered with the same escaping rules.

Cases whose input lies outside the native kernel's documented ASCII/uint32
domain are skipped here (the run-time gate returns ``tool_status::unsupported``
for them and the Python shim takes over); the gate itself is asserted by
hand-written tests in test_grep_tool.cpp.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "tests" / "unit" / "builtin_tools" / "grep_goldens.inc"

DEFAULT_REFERENCE = os.environ.get("KIMI_AGENT_ROOT", "C:/dev/kimi-agent")


# ---------------------------------------------------------------------------
# reference import
# ---------------------------------------------------------------------------


def load_reference(root: str):
    src = Path(root) / "kimi-cli" / "src"
    if not (src / "kimi_cli" / "tools" / "file" / "grep_selectors.py").is_file():
        raise SystemExit(f"kimi-agent checkout not found under {root!r}")
    sys.path.insert(0, str(src))
    from kimi_cli.tools.file import (  # noqa: E402
        grep_archive,
        grep_local,
        grep_output,
        grep_recorder,
        grep_selectors,
        output_utils,
    )
    from kimi_cli.utils import sensitive  # noqa: E402

    return {
        "S": grep_selectors,
        "O": grep_output,
        "R": grep_recorder,
        "U": output_utils,
        "A": grep_archive,
        "L": grep_local,
        "SENS": sensitive,
    }


# ---------------------------------------------------------------------------
# encoding helpers (mirrored byte for byte in test_grep_tool.cpp)
# ---------------------------------------------------------------------------

_ESC = {"\\": "\\\\", "\n": "\\n", "\r": "\\r", "\t": "\\t"}


def esc(s: str) -> str:
    return "".join(_ESC.get(ch, ch) for ch in s)


def jval(v) -> str:
    if v is None:
        return "null"
    if v is True:
        return "true"
    if v is False:
        return "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, str):
        return '"' + esc(v) + '"'
    if isinstance(v, (list, tuple)):
        return "[" + ",".join(jval(x) for x in v) + "]"
    if isinstance(v, dict):
        return "{" + ",".join('"%s":%s' % (esc(str(k)), jval(x)) for k, x in v.items()) + "}"
    raise TypeError(repr(v))


def cpp_lit(s: str) -> str:
    """Emit a C++ string literal: ASCII only, octal escapes for raw bytes."""
    out = ['"']
    for b in s.encode("utf-8"):
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


def fields(*items: str) -> str:
    return "\t".join(esc(str(i)) for i in items)


def ascii_only(text: str) -> bool:
    return text.isascii()


# ---------------------------------------------------------------------------
# corpus + reference answers
# ---------------------------------------------------------------------------


def fmt_range(r) -> str:
    return "%d-%s" % (r.start_line, "open" if r.end_line is None else r.end_line)


def fmt_ranges(rs) -> list:
    return [fmt_range(r) for r in rs]


def case_chunks(cases, chunk, S):
    """parse_line_range_chunk golden."""
    try:
        r = S.parse_line_range_chunk(chunk)
    except ValueError as exc:
        cases.append((fields(chunk), "err:" + str(exc)))
        return
    cases.append((fields(chunk), "none" if r is None else fmt_range(r)))


def case_ranges(cases, sel, S, fn):
    """parse_line_ranges / selector_line_ranges golden (they share a shape)."""
    try:
        rs = fn(sel)
    except ValueError as exc:
        cases.append((fields(sel), "err:" + str(exc)))
        return
    cases.append((fields(sel), "none" if rs is None else fmt_ranges(rs)))


CHUNK_CORPUS = [
    # reference test suite (kimi-cli/tests/tools/test_grep_selectors.py)
    "50-100",
    "50+10",
    "301-",
    "301",
    "1-1",
    "L42",
    "L42-L50",
    "42..100",
    "42..",
    "l42-L50",
    " 50-100 ",
    "50+",
    "0-5",
    "50-40",
    "50+0",
    "garbage",
    "",
    # grammar corners
    "\t42\n",
    "007",
    "1-",
    "1+",
    "1-L2",
    "L1-L1",
    "L0-5",
    "5+0003",
    "5-0007",
    "+5",
    "-5",
    "1..0",
    "1-2-3",
    "1--2",
    "1++2",
    "L",
    "LL42",
    "1,2",
    "1 2",
    "1..",
    ".",
    "..",
    "+",
    "-",
    "0",
    "0+1",
    "1+0",
    "0-0",
    "1..1",
    "1..4294967295",
    "4294967295",
    "20-10",
    "10-10",
    "l5..l9",
    "  L7   ",
]

RANGES_CORPUS = [
    "5-16,960-973",
    "1-3,3-5",
    "1-3,4-6",
    "10-,20-30",
    "20-30,1-5",
    "raw",
    "",
    "  ",
    " 1-3 , 5-6 ",
    "50-40",
    "1-3,garbage,7-8",
    "1-3,,7-8",
    ",1-3",
    "1-3,",
    "1-3,2-5,10-",
    "1-1,1-1",
    "3-5,1-2",
    "1-3,5-6,4-5",
    "2-,1-3",
    "1+2,1+3",
    "1,2,3",
    "5-8,5-",
    "5-,5-8",
    "5-6,5-8",
    "1-3,7-,9-10",
    "1-3,4-",
    "1-3,4",
    "garbage",
    "10-5",
    "0-1",
    "1-2,1+3",
    "l1..l3,L5+2",
]

LINE_IN_RANGES_CORPUS = [
    ("1-5,10-12", 1),
    ("1-5,10-12", 5),
    ("1-5,10-12", 6),
    ("1-5,10-12", 10),
    ("1-5,10-12", 12),
    ("1-5,10-12", 13),
    ("1-5,10-12", 0),
    ("301-", 300),
    ("301-", 301),
    ("301-", 10_000_000),
    ("5+3", 4),
    ("5+3", 5),
    ("5+3", 7),
    ("5+3", 8),
    ("garbage", 7),
    ("1-1", 1),
    ("1-1", 2),
]

SEL_RANGES_CORPUS = [
    "",
    "raw",
    "conflicts",
    "RAW",
    "Conflicts",
    "raw:50-100",
    "50-100:raw",
    "1-5",
    "50-40",
    "raw:conflicts",
    "raw:garbage",
    "garbage",
    "5-10:raw:20-30",
    "raw:20-30:5-10",
    "1-3:raw:5-6",
    "raw:5-",
    "conflicts:1-2",
    " :raw:",
    "raw:",
    "5-6",
]

SPLIT_CORPUS = [
    ("src/app.py", False),
    ("src/app.py:50-100", False),
    ("a/b.py:5-16,960-973", False),
    ("a/b.py:1-50:raw", False),
    ("a/b.py:raw:1-50", False),
    ("bundle.zip:src/foo.ts", False),
    (r"C:\dir\f.txt:50-100", False),
    ("C:", False),
    ("C::5-10", False),
    ("C:5-10", False),
    ("C:\\dir\\f.txt", False),
    ("ssh://h:2222", False),
    ("ssh://h/f:1-5", False),
    ("https://h:1-5", False),
    ("http://h/p:1-5", False),
    ("src/foo.py:hello world", False),
    ("", False),
    ("a.py:1-2", True),
    ("a.py:1-2", False),
    (":1-5", False),
    ("a:1-2:3-4", False),
    ("a:b:c", False),
    ("a.py:L5-L6", False),
    ("a.py:L5-L6:raw", False),
    ("a:1-2:raw:3-4", False),
    ("f.py: 1-5 ", False),
    ("f.py:raw: 1-5 ", False),
    ("f.py:", False),
    (":", False),
    ("dir/a.py:5..9", False),
    ("dir/a.py:5+2", False),
    ("dir/a.py:raw", False),
    ("x.7z:1-2:3-4", False),
    ("x.tar.gz:a/b:1-5", False),
    ("..\\a.py:1-2", False),
    ("a.py:1-2:raw:5-6:conflicts", False),
    ("a.py:1-5,7-9", False),
    ("a.py:1-5,7-9:raw", False),
    ("/abs/path.py:1-9", False),
    ("/abs/path.py:L1-L9", False),
]

EXPAND_STR_CORPUS = [
    '["a.py", "b.py"]',
    "src; tests",
    "src/a.py:1-2,3-4",
    "",
    "   ",
    "single.py",
    "[1, 2]",
    '["a"]',
    "[]",
    "[ ]",
    '[ ]',
    '["a",]',
    '["a" "b"]',
    '"a"',
    "[",
    '["a"] extra',
    '["a\\"b"]',
    '["a\\nb"]',
    '[" a ", "b"]',
    "src; ;tests",
    "  src ; tests  ",
    "a;a",
    "a;b;a",
    ";",
    ";;",
    '["a", "a"]',
    '[" ; ", "x"]',
    "[\n  \"a\",\n  \"b\"\n]",
    '["a\\tb"]',
    '["\\u0041"]',
    '["\\/slash"]',
    '["\\\\"]',
    "[\t\"a\"\t]",
]

EXPAND_LIST_CORPUS = [
    ["a.py", "b.py"],
    ["a.py", "a.py", "b.py"],
    ["  a.py  ", "b.py"],
    ["", "  ", "a.py"],
    [],
    ["src/a.py:1-2,3-4"],
    ["a", "b", "a", "c", "b"],
]

REMAP_CORPUS = [
    # (pairs, lines)
    ([], ["a.py:1:x"]),
    ([], []),
    ([("C:\\tmp\\scratch\\f.py", "bundle.zip:f.py")], ["C:\\tmp\\scratch\\f.py:1:hi", "C:/tmp/scratch/f.py-2-ctx", "other.py:3:x", "--"]),
    ([("/tmp/s/f.py", "z.zip:m.py"), ("/tmp/s/g.py", "z.zip:n.py")], ["/tmp/s/g.py:1:a", "/tmp/s/f.py:2:b"]),
    ([("/tmp/s/a", "A")], ["/tmp/s/ab:1:x", "/tmp/s/a:1:x"]),
    ([("", "X")], ["q:1:x"]),
    ([("/tmp/s/", "X:")], ["/tmp/s/x:1:y"]),
]

PARSE_CONTENT_CORPUS = [
    "a.py:1:alpha",
    "a.py-2-ctx",
    "a.py:1:",
    "a.py:1",
    "a.py:1:",
    "--",
    "-- ",
    "a.py--1--x",
    "a.py:01:x",
    "a.py:001:",
    "a.py:1:2:3",
    "a.py-1-2-3",
    "a.py:1-x",
    "a.py-1:x",
    "a.py:1.5:x",
    ":1:x",
    "-1-x",
    "1:x",
    ":1:",
    "",
    " ",
    "a b:12:text with spaces",
    "dir/sub/file.py:1000:long text",
    "a.py:1:x\ny",
    "a:1\nb:2:x",
    "a.py:1:x\r",
    "a.py:1:\r\n",
    "a.py:4294967295:x",
    "a.py:1:é",
    "café.py:1:x",
    "a.py:1:tab\there",
    "a.py:1:colon:inside",
    "a.py:1:-dash-inside",
    "x:1::",
    "x-1--",
    "x:1:y\nz:w:2:v",
    "a:1:b\nc",
    "..\\dir\\f.txt:3:code",
    "a.py:0:zero",
    "a.py:00:zero",
    "a.py:1:é\u2028x",
]

LINE_SHAPE_CORPUS = [
    "a.py:1:alpha",
    "a.py-2-ctx",
    "--",
    "a.py:1",
    "a.py:1:",
    "a.py:1:x\ny",
    ":1:x",
    "-1-x",
    "1:x",
    "",
    "a.py:1:x\r",
    "a:1:b\nc",
    "café.py:1:x",
    "a.py:4294967295:x",
    "dir/sub/f.py:10:hello",
    "path-with-dash:1:x",
    "path-with-dash-1-x",
]

FMT_MATCH_CORPUS = [
    (12, "x = 1", True),
    (13, "y = 2", False),
    (3, "a", True),
    (1000, "b", False),
    (0, "", True),
    (1, "", False),
    (7, "text with | pipes", True),
    (42, "unicode é", False),
    (4294967295, "big", True),
]

GROUP_LINES_CORPUS = [
    ["a.py:1:alpha", "b.py:2:beta", "a.py:2:alpha2"],
    ["a.py:1:alpha", "--", "a.py:3:alpha3"],
    ["--", "a.py:1:alpha"],
    ["a.py:1:alpha", "  ", "a.py:2:beta"],
    ["# a.py", "*1|alpha"],
    [],
    ["--"],
    ["a.py:1:alpha", "gap marker", "a.py:2:beta"],
    ["a.py:1:x", "b.py:1:y", "b.py:2:z", "--", "c.py:9:w"],
    ["1:text"],
    ["a.py:1:a", "a.py:1:b"],
    ["a.py-1-a", "a.py:2:b"],
    ["a.py:1:x", "--", "--", "a.py:2:y"],
    ["--", "--"],
    ["a.py:1:alpha", "b.py:2:beta", "a.py:3:gamma"],
    ["x y.py:1:with space"],
    ["a.py:1:x\ny", "a.py:2:z"],
]

GROUP_BLANK_CORPUS = [
    ["# a", "*1|x", "", "# b", "*2|y"],
    ["", "a", "b"],
    [],
    ["a", "b"],
    ["", "", ""],
    ["a", "", "", "b"],
    ["  ", "a", "\t", "b"],
    ["a", "   ", "b"],
    ["\u00a0", "a"],
]

RANGE_FILTER_CORPUS = [
    # (pairs, lines)
    ([], ["a.py:1:x", "--"]),
    ([("a.py", "1-3")], ["a.py:1:x", "a.py:4:y", "a.py:3:z", "b.py:9:w", "--", "a.py:2:v"]),
    ([("a.py", "1-3")], ["--", "a.py:1:x", "--", "--", "a.py:9:y", "--"]),
    ([("a.py", "1-")], ["a.py:1:x", "a.py:1000:y"]),
    ([("a.py", "5+2")], ["a.py:6:x", "a.py:7:y", "b.py:6:z"]),
    ([("b.py", "1-2")], ["a.py:1:x", "--", "b.py:5:y"]),
    ([("a.py", "1-3")], ["a.py-2-ctx", "a.py:4:drop", "--"]),
    ([("a b.py", "1-1")], ["a b.py:1:x", "a b.py:2:y"]),
    ([("a.py", "1-3")], ["not a content line", "a.py:2:keep", "--"]),
    ([("a.py", "1-1")], ["--"]),
    ([("a.py", "1-3"), ("b.py", "2-2")], ["a.py:1:x", "b.py:2:y", "b.py:3:z", "--", "a.py:2:w"]),
]

REATTACH_CORPUS = [
    # (prefix, lines)
    ("f.py", ["1:x", "2-y", "not a line", "--", "", "12:x"]),
    ("", ["1:x"]),
    ("dir/f.py", ["1:x", "x1:y", "007:z", "3-"]),
    ("f.py", []),
    ("p", ["1:2:3", "-1-x", ":1:x"]),
]

STRIP_PREFIX_CORPUS = [
    ("/home/user/project", ["/home/user/project/src/a.py:42:code", "/home/user/project/src/b.py-41-context", "--"]),
    ("C:\\repo", ["C:\\repo\\src\\a.py:42:code", "C:\\repo\\src\\b.py-41-context", "--"]),
    ("D:\\repo", ["D:/repo\\src\\a.py:42:code", "D:/repo\\src\\b.py-41-context", "--"]),
    ("/home/user/project", ["/other/path/file.py", "--"]),
    ("/tmp/dir/", ["/tmp/dir/file.py"]),
    ("/tmp/dir", ["/tmp/dir/file.py"]),
    ("/tmp/a", ["/tmp/abc/file.py", "/tmp/a/file.py"]),
    ("", ["/x:1:y", "a:1:b"]),
    ("C:\\", ["C:\\a.py:1:x", "C:a.py:1:x"]),
    ("/", ["/a.py:1:x", "a.py:1:x"]),
    ("//srv/share", ["//srv/share/f.py:1:x", "//srv/other/f.py:1:y"]),
    ("/a", ["/a", "/a/", "/a/b"]),
    ("/tmp/dir", []),
]

NORMALIZE_CORPUS = [
    ("content", True, ["C:\\w\\a.py:1:x", "C:\\w\\b.py-2-ctx", "--", "C:\\w\\c.py:3:y"]),
    ("files_with_matches", True, ["C:\\w\\a.py", "D:\\other\\b.py", "--"]),
    ("count_matches", True, ["C:\\w\\a.py:3", "D:\\x:y:1"]),
    ("content", False, ["C:\\w\\a.py:1:x"]),
    ("files_with_matches", False, ["C:\\w\\a.py"]),
    ("content", True, ["a.py:007:x", "a.py-08-y", "no content line"]),
    ("content", True, []),
    ("count_matches", True, ["--", "C:\\w\\a.py:2"]),
]

COLLECT_CORPUS = [
    ("content", ["a.py:1:x", "b.py:2:y", "a.py:3:z", "--"]),
    ("count_matches", ["a.py:3", "b.py:1", "a.py:9", "no-colon"]),
    ("files_with_matches", ["a.py", "b.py", "a.py"]),
    ("count_matches", [":5", "x:y:z"]),
    ("content", ["--"]),
    ("content", []),
    ("files_with_matches", ["--"]),
    ("count_matches", ["--"]),
]

RTK_PARSE_CORPUS = [
    [],
    ["42 matches in 3 files:", "", "a.py:1:x"],
    ["42 matches in 3 files:", "a.py:1:x"],
    ["42 matches in 3 files:"],
    ["42 matches in 3 files:", "", ""],
    ["0 matches in 0 files:", "", "--"],
    ["  +37 more in C:\\path\\file.py [see remaining: tail -n +26 <log>]"],
    ["+37 more in p.py [see remaining: whatever]"],
    ["\t+1 more in p.py [see remaining:   tail -n +5 /tmp/log  ]"],
    ["+133 more files [see remaining: tail -n +300 <log>]"],
    ["+133 more files [see remaining: no-hint]"],
    ["+133 more files [see remaining: ]"],
    ["+0 more files [see remaining: x]"],
    ["+1 more in a [see remaining: x] [see remaining: y]"],
    ["+1 more in a [see remaining: x] extra"],
    ["+1 more in a [see remaining: x"],
    ["+x more in a [see remaining: y]"],
    ["+1 more  in a [see remaining: y]"],
    ["+1 more in  [see remaining: y]"],
    ["+1 more in a [see remaining: tail -n +0 log]"],
    ["+1 more in a [see remaining: tail -n +7 two words]"],
    ["+1 more in a [see remaining: tail -n +7]"],
    ["+1 more in a [see remaining: tail -n 7 log]"],
    ["+1 more in a [see remaining: tail -n +x log]"],
    ["plain line", "a.py:1:x", "--", "42 matches in 3 files:", "", "b.py:2:y"],
    ["29 matches in 3 files:", "", "file.py:1:a", "  +37 more in file.py [see remaining: tail -n +26 /tmp/rtk.log]", "+133 more files [see remaining: tail -n +300 /tmp/rtk.log]"],
    ["no protocol here"],
    ["42 matches in 3 files:", "   "],
    [" +5 more files [see remaining: h]"],
    ["+5 more files [see remaining: tail -n +3 ]"],
]

# `_join_with_byte_limit`: (max_bytes, lines)
JOIN_CORPUS = [
    (1000, []),
    (1000, [""]),
    (1000, ["", "b"]),
    (1000, ["a", "", "b"]),
    (1000, ["", "", "c"]),
    (0, ["a"]),
    (0, [""]),
    (1, ["a", "b"]),
    (2, ["a", "b"]),
    (3, ["abc", "de"]),
    (4, ["abc", "de"]),
    (5, ["abc", "de"]),
    (6, ["abc", "de"]),
    (10, ["a", "b", "c", "d", "e"]),
    (1, ["", "", ""]),
    (2, ["", "x", ""]),
]

RTK_NOTE_CORPUS = [
    ("", []),
    ("", ["42 matches in 3 files:", "", "a.py:1:x"]),
    ("", ["+37 more in p.py [see remaining: tail -n +26 /tmp/log]"]),
    ("", ["+37 more in p.py [see remaining: plain text]"]),
    ("", ["+133 more files [see remaining: tail -n +300 /tmp/log]"]),
    ("", ["+133 more files [see remaining: plain]"]),
    ("", ["+133 more files [see remaining: ]"]),
    ("C:\\tmp\\rtk.log", ["+37 more in p.py [see remaining: tail -n +26 /tmp/log]"]),
    ("/tmp/x.log", ["+37 more in a.py [see remaining: tail -n +2 /l1]", "+9 more in b.py [see remaining: tail -n +5 /l2]", "+3 more files [see remaining: tail -n +9 /l3]"]),
    ("", ["+37 more in a.py [see remaining: tail -n +2 /l1]", "+9 more in b.py [see remaining: plain]"]),
    ("", ["+1 more in a.py [see remaining:   spaced hint  ]"]),
    ("", ["+1 more files [see remaining:   spaced hint  ]"]),
]

REC_RECORD_CORPUS = [
    ["b.py", "a.py", "b.py"],
    ["", "a.py"],
    ["a", "b", "c"],
    [],
    ["", ""],
    ["x"] * 3,
]

REC_MERGE_CORPUS = [
    # (cap, existing, fresh)
    (500, ["a.py"], ["b.py", "a.py"]),
    (500, [], []),
    (500, ["", "a"], ["", "b"]),
    (2, ["a", "b"], ["c"]),
    (3, ["a", "b", "c"], ["d", "e"]),
    (0, ["a"], ["b"]),
    (500, ["a", "a"], ["a"]),
    (1, ["a", "b", "c"], []),
]

SENSITIVE_CORPUS = [
    ".env",
    "a/.env",
    "a\\.env",
    ".env.local",
    ".env.example",
    ".env.sample",
    ".env.template",
    ".env.Example",
    "id_rsa",
    "dir/id_ed25519",
    "id_ecdsa",
    "credentials",
    "a/b/credentials",
    ".aws/credentials",
    "home/.aws/credentials",
    "x.aws/credentials",
    ".gcp/credentials",
    "id_rsa.pub",
    "notes.txt",
    "",
    ".",
    "..",
    "dir/",
    "/abs/.env",
    "C:/Users/x/.env",
    "C:\\Users\\x\\id_rsa",
    "ID_RSA",
    "Id_Rsa",
    ".ENV",
    "sub/.env.bak",
    "a/.aws/credentials/extra",
    "aws/credentials",
    "café/.env",
]

WARNING_CORPUS = [
    [],
    [".env"],
    [".env", "id_rsa"],
    ["a/.env", "b/.env", "c/id_rsa"],
    [".env", "id_rsa", "id_ecdsa", "credentials", ".aws/credentials", ".gcp/credentials"],
    ["a/.env", "b/.env", "c/.env", "d/.env", "e/.env", "f/.env", "g/.env"],
    ["notes.txt"],
    ["C:/x/.env", "C:/y/.env"],
]

NEWLINE_CORPUS = [
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
    "a\\\\\\\\nb",
    "a\\\\\\\\\\nb",
    "\\n\\n\\n",
    "",
    "n",
    "\\",
    "\\\\",
    "\\\\\\",
    "a\\",
    "a\\\\",
    "\\N",
    "\\\\N",
    "a\\\rb",
    "x\\n\\r\\ny",
    "\\x0a",
]


# ---------------------------------------------------------------------------
# independent oracle for the newline kernels (see report)
# ---------------------------------------------------------------------------

_REGEX_NEWLINE = None


def oracle_has_regex_newline(pattern: str, re_mod) -> bool:
    """Documented rule: a literal LF, or a `\\n` escape after an ODD backslash run."""
    global _REGEX_NEWLINE
    if _REGEX_NEWLINE is None:
        _REGEX_NEWLINE = re_mod.compile(r"(?<!\\)(?:\\\\)*\\n")
    return "\n" in pattern or bool(_REGEX_NEWLINE.search(pattern))


# ---------------------------------------------------------------------------
# table builders
# ---------------------------------------------------------------------------


RANGES_CORPUS += [
    # end_line == UINT32_MAX: Python's `r.start <= last.end + 1` must not wrap
    "4294967295-4294967295,4294967295-4294967295",
    "4294967294-4294967295,4294967295-4294967295",
    "4294967295-4294967295,1-1",
]

RTK_PARSE_CORPUS += [
    # `$` in the reference regex also matches before ONE trailing newline
    ["+133 more files [see remaining: log]\n"],
    ["42 matches in 3 files:\n"],
    ["+37 more in p.py [see remaining: tail -n +5 log]\n"],
    # `/S+` rejects every Python-/s byte, not just space/tab
    ["+37 more in p.py [see remaining: tail -n +5 lo\x0bg]"],
    ["+37 more in p.py [see remaining: tail -n +5 lo\x0cog]"],
    ["+37 more in p.py [see remaining: tail -n +5 lo\x1cog]"],
    ["+37 more in p.py [see remaining: tail -n +5\x0blog]"],
    ["+37 more in p.py [see remaining: tail -n +5\x0clog]"],
    ["+37 more in p.py [see remaining: tail -n +5\x1clog]"],
    ["+37 more in p.py [see remaining: tail -n +5\x0b]"],
]

EXPAND_STR_CORPUS += [
    '["caf\u00e9.py", "b.py"]',
    '[\u00e9]',
    "caf\u00e9; b.py",
    "\u00a0a\u00a0; b",
    '[ "\u65e5\u672c\u8a9e" ]',
    '["a", "\\ud800"]',
    '["a", "\\ud800\\udc00"]',
    '[\u00a0]',
    '["\u00e9", "\u00e9"]',
    '["a" , "b" ]',
    '["a"\t,\t"b"]',
    '["a", "b"];c',
    '["a;b"]',
    '["a\\\\"]',
    '["/", "//"]',
    '["\u00e9"]',
    '["\u0041\u0042"]',
    '[""]',
    "[   ]",
    '["a"]\n',
    '["a", "b", "c", "d"]',
    '["a.py:1-5", "b.py:raw"]',
]

SPLIT_CORPUS += [
    ("C::1-5:raw", False),
    ("a.py:1-5:hello world", False),
    ("a.py:raw:x", False),
    ("a.py:1-5:raw:", False),
    ("a.py:1-5:raw:7-9", False),
    ("a.py: 5 - 9 ", False),
    ("a.py:l5..l9", False),
    ("a.py:5+", False),
    ("a.py:5-", False),
    ("a.py:0-5", False),
    ("a.py:1-5,", False),
    ("a.py:,", False),
    ("a.py:1-5,7", False),
    (r"c:\dir\f.txt:1-5,L7-L9", False),
    ("/x/y.py:1-5:raw:2-3", False),
]

NORMALIZE_CORPUS += [
    ("content", True, [":1:x", r"\\server\share", r"a\b:1:x"]),
    ("files_with_matches", True, [":1:x", r"\\server\share", r"a\b"]),
    ("count_matches", True, [r"a\b:2", ":3", r"\\srv\sh"]),
    ("content", True, ["", "--", "a.py:1:x"]),
]

STRIP_PREFIX_CORPUS += [
    ("/long/prefix/here", ["/long", "/long/prefix", "/long/prefix/here/x.py:1:y"]),
    (r"\\srv\share\d", [r"\\srv\share\d\f.py:1:x", r"\\srv\share\e\f.py:1:x"]),
]

PARSE_CONTENT_CORPUS += [
    "a:12:3:4",
    "a.py:1:2-3",
    "a.py-1:x",
    "x:1:y\nz:w:2:v",
    "a\nb:1:x",
    ":0:",
    "a.py:1:  ",
    "a.py:1:\t",
    "::",
    "a.py:::1::",
    "1:1:1",
    "a.py:1:x\n",
    "a.py:1:x\r\n",
]

GROUP_LINES_CORPUS += [
    ["a.py:1:x\r", "a.py-2-y\r", "--"],
    ["b.py:1:x", "--", "b.py:2:y", "  ", "b.py:3:z"],
    ["a.py:1:x", "a.py:2:y", "b.py:1:z", "b.py:2:w", "a.py:3:v"],
]

COLLECT_CORPUS += [
    ("count_matches", ["x:", "y:0", ":"]),
    ("content", ["a.py:1:x", "a.py:1:x", "--", "--"]),
    ("files_with_matches", ["", "a.py", ""]),
]

REMAP_CORPUS += [
    ([("C:\\t\\f.py", "b.zip:f.py")], ["C:\\t\\f.py", "C:\\t\\f.pyx:1:y", "caf\u00e9"]),
    ([("/a", "X")], ["/a", "/ab"]),
]

SENSITIVE_CORPUS += [
    ".env.",
    ".env.local.dist",
    "x.env",
    "sub/dir/id_rsa/",
    "credentials.txt",
    "x/credentials",
    "aws/credentials",
    "a/.gcp/credentials",
    ".gcp/credentials",
    "dir/..",
    "a//b/.env",
    r"\\srv\share\.env",
    "C:.env",
    "C:_env",
]

WARNING_CORPUS += [
    ["a/.env", "b/id_rsa", "c/.env"],
    ["x/.env", "y/.env", "z/.env", "w/.env", "v/.env", "u/.env"],
    ["/a/.env", "/b/credentials"],
]

def build(ref, re_mod) -> list[tuple[str, list[tuple[str, str]]]]:
    S, O, R, U, A, L, SENS = (
        ref["S"],
        ref["O"],
        ref["R"],
        ref["U"],
        ref["A"],
        ref["L"],
        ref["SENS"],
    )
    tables: list[tuple[str, list[tuple[str, str]]]] = []

    # ---- selectors ------------------------------------------------------
    cases: list[tuple[str, str]] = []
    for chunk in CHUNK_CORPUS:
        case_chunks(cases, chunk, S)
    tables.append(("k_g_golden_lr_chunk", cases))

    cases = []
    for sel in RANGES_CORPUS:
        case_ranges(cases, sel, S, S.parse_line_ranges)
    tables.append(("k_g_golden_lr_ranges", cases))

    cases = []
    for spec, line in LINE_IN_RANGES_CORPUS:
        ranges = S.parse_line_ranges(spec)
        cases.append((fields(spec, line), S.is_line_in_ranges(line, ranges)))
    tables.append(("k_g_golden_line_in_ranges", cases))

    cases = []
    for sel in SEL_RANGES_CORPUS:
        case_ranges(cases, sel, S, S.selector_line_ranges)
    tables.append(("k_g_golden_sel_ranges", cases))

    cases = []
    for raw, exists in SPLIT_CORPUS:
        original = S._literal_exists
        S._literal_exists = (lambda flag: (lambda _p: flag))(exists)  # type: ignore[assignment]
        try:
            path, sel = S.split_path_and_sel(raw)
        finally:
            S._literal_exists = original  # type: ignore[assignment]
        cases.append((fields(raw, "1" if exists else "0"), [path, sel]))
    tables.append(("k_g_golden_split_sel", cases))

    cases = []
    for raw in EXPAND_STR_CORPUS:
        # unicode entries are NOT gated: the string form scans UTF-8 bytes
        cases.append((fields(raw), S.expand_path_entries(raw)))
    tables.append(("k_g_golden_expand_str", cases))

    cases = []
    for items in EXPAND_LIST_CORPUS:
        if not all(ascii_only(i) for i in items):
            continue
        # explicit count: the empty list and [""] must stay distinguishable
        cases.append((fields(str(len(items)), *items), S.expand_path_entries(list(items))))
    tables.append(("k_g_golden_expand_list", cases))

    # ---- content lines / rendering --------------------------------------
    cases = []
    for line in PARSE_CONTENT_CORPUS:
        parsed = L.parse_content_line(line)
        if parsed is None:
            cases.append((fields(line), "none"))
        else:
            path, no, text, is_match = parsed
            cases.append((fields(line), {"path": path, "line_no": no, "is_match": is_match, "text": text}))
    tables.append(("k_g_golden_parse_content", cases))

    cases = []
    for line in LINE_SHAPE_CORPUS:
        m = L._RG_LINE_RE.match(line)
        # the native kernel reports the path length in BYTES
        cases.append(
            (fields(line), "none" if m is None else len(m.group(1).encode("utf-8")))
        )
    tables.append(("k_g_golden_line_shape", cases))

    cases = []
    for n, text, is_match in FMT_MATCH_CORPUS:
        cases.append((fields(n, text, "1" if is_match else "0"), O.format_match_line(n, text, is_match)))
    tables.append(("k_g_golden_fmt_match", cases))

    cases = []
    for lines in GROUP_LINES_CORPUS:
        groups = O.group_lines_by_file(lines, L.parse_content_line)
        rendered = [[path, [[ln, is_match, text] for ln, text, is_match in body]] for path, body in groups]
        cases.append((fields(str(len(lines)), *lines), rendered))
    tables.append(("k_g_golden_group_files", cases))

    cases = []
    for lines in GROUP_LINES_CORPUS:
        groups = O.group_lines_by_file(lines, L.parse_content_line)
        cases.append((fields(str(len(lines)), *lines), O.format_grouped_output(groups)))
    tables.append(("k_g_golden_fmt_grouped", cases))

    cases = []
    for lines in GROUP_BLANK_CORPUS:
        cases.append((fields(str(len(lines)), *lines), O.group_line_indices_by_blank(lines)))
    tables.append(("k_g_golden_group_blank", cases))

    cases = []
    for spec_line in RANGE_FILTER_CORPUS:
        pairs, lines = spec_line
        ranges_map = {}
        for key, spec in pairs:
            ranges_map[key] = S.parse_line_ranges(spec)
        payload = [str(len(pairs))]
        for key, spec in pairs:
            payload += [key, spec]
        payload += [str(len(lines)), *lines]
        cases.append((fields(*payload), L._range_filter_lines(lines, ranges_map)))
    tables.append(("k_g_golden_range_filter", cases))

    cases = []
    for prefix, lines in REATTACH_CORPUS:
        cases.append(
            (fields(prefix, str(len(lines)), *lines), L._reattach_single_file_prefix(lines, prefix))
        )
    tables.append(("k_g_golden_reattach", cases))

    cases = []
    for base, lines in STRIP_PREFIX_CORPUS:
        cases.append((fields(base, str(len(lines)), *lines), L._strip_path_prefix(lines, base)))
    tables.append(("k_g_golden_strip_prefix", cases))

    cases = []
    for mode, on_win, lines in NORMALIZE_CORPUS:
        if on_win:
            if os.sep != "\\":
                continue  # the reference checks os.sep, so this host cannot show it
            want = L._normalize_slashes_content(lines, mode)
        else:
            want = list(lines)
        cases.append((fields(mode, "1" if on_win else "0", str(len(lines)), *lines), want))
    tables.append(("k_g_golden_normalize_slashes", cases))

    cases = []
    for mode, lines in COLLECT_CORPUS:
        cases.append((fields(mode, str(len(lines)), *lines), L._collect_record_files(lines, mode)))
    tables.append(("k_g_golden_collect_files", cases))

    cases = []
    for max_bytes, lines in JOIN_CORPUS:
        joined, truncated = L._join_with_byte_limit(lines, max_bytes)
        cases.append(
            (fields(str(max_bytes), str(len(lines)), *lines), [joined, truncated])
        )
    tables.append(("k_g_golden_join_limit", cases))

    cases = []
    for pairs, lines in REMAP_CORPUS:
        display_map = {scratch: display for scratch, display in pairs}
        payload = [str(len(pairs))]
        for scratch, display in pairs:
            payload += [scratch, display]
        payload += [str(len(lines)), *lines]
        cases.append((fields(*payload), L._remap_display(lines, display_map)))
    tables.append(("k_g_golden_remap_display", cases))

    # ---- rtk protocol ----------------------------------------------------
    cases = []
    for lines in RTK_PARSE_CORPUS:
        cleaned, meta = U.parse_rtk_rg_output(lines)
        cases.append(
            (
                fields(str(len(lines)), *lines),
                {
                    "cleaned": cleaned,
                    "total_matches": meta["total_matches"],
                    "total_files": meta["total_files"],
                    "folded_files": [
                        {
                            "path": f["path"],
                            "count": f["count"],
                            "log": f["log"],
                            "start_line": f["start_line"],
                        }
                        for f in meta["folded_files"]
                    ],
                    "skipped_files": meta["skipped_files"],
                    "skipped_log": meta["skipped_log"],
                },
            )
        )
    tables.append(("k_g_golden_rtk_parse", cases))

    cases = []
    for original_path, lines in RTK_NOTE_CORPUS:
        _cleaned, meta = U.parse_rtk_rg_output(lines)
        note = L._rtk_fold_note(meta, original_path=original_path or None)
        cases.append((fields(original_path, str(len(lines)), *lines), note))
    tables.append(("k_g_golden_rtk_note", cases))

    # ---- recorder --------------------------------------------------------
    cases = []
    for paths in REC_RECORD_CORPUS:
        rec = R.FileRecorder()
        for p in paths:
            rec.record(p)
        cases.append((fields(*paths), rec.list))
    tables.append(("k_g_golden_recorder_record", cases))

    cases = []
    for cap, existing, fresh in REC_MERGE_CORPUS:
        merged = R._merged(existing, fresh)
        if len(merged) > cap:
            merged = merged[-cap:] if cap else merged[len(merged):]
        cases.append(
            (
                fields(str(cap), str(len(existing)), *existing, str(len(fresh)), *fresh),
                merged,
            )
        )
    tables.append(("k_g_golden_recorder_merge", cases))

    # ---- sensitive files -------------------------------------------------
    cases = []
    for path in SENSITIVE_CORPUS:
        if not ascii_only(path):
            continue
        # on_windows=True is the host flavour: call the reference directly.
        cases.append((fields(path, "1"), SENS.is_sensitive_file(path)))
    tables.append(("k_g_golden_sensitive_win", cases))

    cases = []
    for paths in WARNING_CORPUS:
        if not all(ascii_only(p) for p in paths):
            continue
        cases.append((fields(str(len(paths)), *paths), SENS.sensitive_file_warning(list(paths))))
    tables.append(("k_g_golden_sensitive_warning", cases))

    # ---- pattern kernels -------------------------------------------------
    cases = []
    for pattern in NEWLINE_CORPUS:
        if not ascii_only(pattern):
            continue
        cases.append((fields(pattern), oracle_has_regex_newline(pattern, re_mod)))
    tables.append(("k_g_golden_pattern_newline", cases))

    return tables


def render(tables) -> str:
    lines = [
        "// GENERATED by scripts/gen_grep_goldens.py - do not edit by hand.",
        "//",
        "// Golden vectors for the grep tool's pure kernels, produced by running the",
        "// kimi-agent reference implementation (kimi-cli/src/kimi_cli/tools/file/*).",
        "// input : TAB-separated fields, 4-escape encoded (see test_grep_tool.cpp)",
        "// want  : JSON value rendered with the same escaping rules",
        "",
        "#pragma once",
        "",
    ]
    for name, cases in tables:
        lines.append(f"static const grep_golden {name}[] = {{")
        for inp, want in cases:
            lines.append(f"    {{{cpp_lit(inp)}, {cpp_lit(jval(want))}}},")
        lines.append("};")
        lines.append("")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--reference", default=DEFAULT_REFERENCE)
    ap.add_argument("--check", action="store_true", help="fail when the .inc is stale")
    ap.add_argument("-o", "--out", default=str(OUT))
    args = ap.parse_args()

    import regex as re_mod  # the reference's regex flavour

    ref = load_reference(args.reference)
    text = render(build(ref, re_mod))
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
    print(f"wrote {out} ({len(text.splitlines())} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
