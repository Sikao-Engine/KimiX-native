"""Differential parity: kimix-base's write kernels vs kimi-agent's write tool.

The native kernels under ``src/builtin_tools/write_tool.*`` claim to mirror
``kimi_cli/tools/file/check_fmt.py`` (``check_json_text`` via orjson),
``kimi_cli/utils/diff.py`` (``format_unified_diff`` via difflib) and
``kimi_cli/tools/file/auto_generated.py``.  This module compares

    native runtime_py.builtin_tools.file.check_json_format(text)
    native runtime_py.builtin_tools.file.build_unified_diff(old, new, path, hdr)
    native runtime_py.builtin_tools.file.validate_format_by_path(path, text)
    native runtime_py.builtin_tools.file.detect_auto_generated_marker(content, path)
    native runtime_py.builtin_tools.file.is_auto_generated_file_name(path)

against the *real* kimi-agent functions (never the ``kimix_native`` mirrors):

* the orjson ``check_json_text`` (the shipped validator) - so the comparison is
  against orjson's own error wording/line/colno, not a paraphrase;
* ``format_unified_diff`` with the native DIFF kernel disabled
  (``_parity_ref.pure_python``) so difflib's body runs, not an older build of
  the same kernel family;
* ``auto_generated.detect_auto_generated_marker`` /
  ``is_auto_generated_file_name`` verbatim.

The conflict-marker kernels (scan/splice/tokens/render/summary/URI/bulk/write
guard) are not exposed through ``runtime_py``; they are pinned instead by
``tests/unit/builtin_tools/test_write_tool.cpp`` against
``tests/unit/builtin_tools/write_goldens.inc``
(``scripts/gen_write_goldens.py``).

Deliberate exclusions (documented deviations, see the port report):

* orjson reports "memory allocation failed" for an unterminated *array* nested
  deeper than ~370 levels (an orjson quirk, e.g. ``"[" * 400``); yyjson reports
  the real "unexpected end of data".  Those inputs are excluded from the strict
  corpus.
* a trailing comma whose closing bracket is followed by a comma in a *nested*
  container with whitespace before the bracket (``"[[1, ] ,]"``) is reported one
  token later by orjson than by the port; decision parity still holds.
* the header marker scanners use ASCII ``\\b``/``\\w`` semantics, so a marker
  directly adjacent to a non-ASCII *word* character can differ from
  ``regex``'s Unicode classes; the strict corpus is ASCII.
"""

from __future__ import annotations

import os
import random
import sys
from pathlib import Path

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------------------
# Import the freshly built extension BEFORE anything imports kimi_cli.
#
# ``kimi_cli.native_loader`` stages ITS OWN native library (``<kimi-agent>/bin``)
# where an older released ``runtime_py.pyd`` may live; any ``import runtime_py``
# after that would resolve to the foreign library and silently compare the port
# against an older build of itself.
# ---------------------------------------------------------------------------
_REPO_ROOT = Path(__file__).resolve().parents[2]
for _mode in ("release", "releasedbg", "debug", "check"):
    _cand = _REPO_ROOT / "bin" / _mode
    if (_cand / "runtime_py.pyd").is_file() or (_cand / "runtime_py.so").is_file():
        _cand_str = str(_cand)
        while _cand_str in sys.path:
            sys.path.remove(_cand_str)
        sys.path.insert(0, _cand_str)
        break

import runtime_py  # noqa: E402

_NATIVE_FILE = Path(runtime_py.__file__).resolve()
if _REPO_ROOT not in _NATIVE_FILE.parents:
    raise RuntimeError(
        f"runtime_py resolved to a foreign build ({_NATIVE_FILE}); the parity "
        f"result would compare the port against an older release of itself. "
        f"Build it first: python scripts/build_locked.py -- xmake build runtime_py"
    )

from _parity_ref import KIMI_AGENT_ROOT, pure_python, ref, ref_available  # noqa: E402

pytestmark = pytest.mark.skipif(
    not ref_available(), reason=f"kimi-agent checkout not found at {KIMI_AGENT_ROOT}"
)

FILE = runtime_py.builtin_tools.file


# ---------------------------------------------------------------------------
# JSON validation (check_fmt.check_json_text, orjson)
# ---------------------------------------------------------------------------

_JSON_VALID = [
    "{}",
    "[]",
    "null",
    "true",
    "false",
    "0",
    "-0",
    "1e10",
    "1E+2",
    "0.5",
    '{"a":1}',
    '{"a":1,"b":[1,2,3]}',
    '  {"a": 1}  ',
    '\n\n{"a":1}\n',
    '{"nested":{"deep":{"deeper":[[[]]]}}}',
    '{"a":1,"a":2}',
    '{"a":1,"a":2,"a":3}',
    '"str"',
    r'"\u00e9"',
    r'"\ud83d\ude00"',
    r'"\u0000"',
    '{"unicode":"h\u00e9llo"}',
    '{"emoji":"\U0001F389"}',
    '12345678901234567890123456789012345678901234567890',
    str(2**200),
    "1.7976931348623157e308",
    "-1.7976931348623157e308",
    "2.2250738585072014e-308",
    "1e-999",
    "0." + "0" * 40 + "1",
    '{"a":1}\n\n',
    "[" * 1024 + "]" * 1024,
    '{"a":' * 1024 + "1" + "}" * 1024,
]

_JSON_INVALID = [
    '{"a":1,}',
    "[1,2,]",
    '{"a":1,,}',
    "[,]",
    "{,}",
    '{,,"a":1}',
    '{"a":1} {"b":2}',
    '{"a":1}x',
    "{} {}",
    "null null",
    "1 2",
    "",
    "   ",
    "\n",
    "\t\n  ",
    '{"a":1,,"b":2}',
    '{"a": 1, }',
    "[1 2]",
    '{"a" 1}',
    "{a:1}",
    "{'a':1}",
    '{"a":1}}',
    "[1,2]]",
    '{"a":}',
    '{"a"',
    "{",
    "[",
    "]",
    "}",
    "NaN",
    "Infinity",
    "-Infinity",
    '{"a": NaN}',
    '{"a": Infinity}',
    '{"a": -Infinity}',
    "[NaN]",
    "[Infinity]",
    "undefined",
    "None",
    "True",
    "False",
    "nan",
    "inf",
    "-inf",
    '"unterminated',
    r'"\x"',
    r'"\l"',
    r'"\uZZZZ"',
    r'"\u12"',
    r'"\u"',
    r'"\u123g"',
    '"tab\tliteral"',
    '{"a":1}\n{"b":2}',
    '{"a":1,\n"b":2,}',
    "//comment\n{}",
    "/*c*/{}",
    "#c\n{}",
    '{"a":1} // trailing',
    '{"a":/*x*/1}',
    '{"a":1,/*x*/}',
    '{"a":01}',
    '{"a":+1}',
    '{"a":.5}',
    '{"a":1.}',
    '{"a":1e}',
    '{"a":1e+}',
    '{"a":0x1}',
    '{"a":1_000}',
    "01",
    "00",
    "-01",
    "0x10",
    "1_000",
    "1.2.3",
    "1e1e1",
    "1e999",
    "-1e999",
    "1" * 400,
    "[1e400]",
    '{"a":1e309}',
    "\ufeff{}",
    "\ufeff{'a':1}",
    "\ufeff1",
    " \ufeff{}",
    "{}\ufeff",
    "{}/**/",
    "{}//x",
    "/**/",
    '{"a":1}"',
    "[[]",
    '{"a":{"b":1}',
    '"\\"',
    '{"a":"\\',
    "tru",
    "nul",
    "fals",
    "t",
    "n",
    "f",
    "{} \x0b",
    "\x0c{}",
    "{}\xa0",
    "\u2028{}",
    "\u2029{}",
    "{}\x1c",
    "\x1d{}",
    "{\x00:1}",
    '{"a":\x01}',
    "[\x0b]",
    '{"\\u0000":1}',
    r'"\ud800"',
    r'"\udc00"',
    r'"\ud800x"',
    r'"x\ud800y"',
    r'["\ud800"]',
    r'{"a":"\ud800"}',
    r'"\ud83d\ud83d"',
    r'"\uD83Dx"',
    r'"\uD83D\u0041"',
    r'"\uD83D\uZZZZ"',
    r'"\uD83D\u"',
    r'"\uD83D\q"',
    r'"\ud800\\u0041"',
    '{"a":1}\x00',
    "[" * 1025 + "]" * 1025,
    '{"a":' * 1025 + "1" + "}" * 1025,
    "[" * 1026 + "]" * 1025,
]

_JSON_NON_ASCII = [
    '{"\u00e9":1,}',
    '{"\u65e5\u672c\u8a9e":"\u30c6\u30b9\u30c8",}',
    '{"a":"\U0001F389\U0001F389",}',
    '["\u00e9",]',
    '  {"\u00e9":1}   x',
    '{"\u00e9"}\n{"b"}',
    '{"\u00e9\u00e9\u00e9":1} x',
    '{"\u043a\u043b\u044e\u0447":"\u0437\u043d\u0430\u0447\u0435\u043d\u0438\u0435",}',
    '{"a":"\\u00e9",}',
    "\u00e9{}",
    '{"a" \u00e9}',
    '{"a":1}\u00e9',
    '["\U0001F389","\U0001F389",]',
    "[\u00e9,1]",
    '{"\u00e9\u00e9":1}x',
    "\u00e9\n{\"a\":1,}",
    '{"a":\u00e9}',
]

_TRAILING_COMMA_CASES = [
    "[1,]",
    "[1,2,]",
    "[1,2,3,]",
    '{"a":1,}',
    '{"a":[1,]}',
    "[1,],",
    "[1,],2",
    "[1, ] ,",
    "[1, ],",
    "[[1,],2]",
    '{"a":1,},',
    "[1,2,3,]45E]",
    "[1,2,],,4{5+]",
    '{"a":1,"b":[1,],{l"c":"d}]}',
]


def _json_corpus() -> list[str]:
    corpus = list(_JSON_VALID) + list(_JSON_INVALID) + list(_JSON_NON_ASCII)
    corpus += _TRAILING_COMMA_CASES
    rnd = random.Random(20240607)
    alphabet = list('{}[],:"\\ 0123456789.eE+-tfnul') + [
        "\u00e9", "\U0001F389", "\n", "\t", "\ufeff", "'", "A", "x", "m", "\r",
    ]
    for _ in range(4000):
        corpus.append("".join(rnd.choice(alphabet) for _ in range(rnd.randint(0, 24))))
    base_docs = [
        '{"a":1,"b":[1,2,{"c":"d"}]}',
        "[1,2,3,4,5]",
        '{"x":{"y":{"z":[true,false,null]}}}',
        '"a\\u00e9b"',
        '{"k":"v","n":-1.5e-3}',
    ]
    for _ in range(2000):
        s = list(rnd.choice(base_docs))
        for _ in range(rnd.randint(1, 4)):
            op = rnd.random()
            i = rnd.randrange(len(s))
            if op < 0.4:
                del s[i]
            elif op < 0.8:
                s.insert(i, rnd.choice(alphabet))
            else:
                s[i] = rnd.choice(alphabet)
        corpus.append("".join(s))
    return corpus


def test_check_json_format_matches_orjson():
    """Decision, wording and 1-based line/column parity with orjson."""
    check_json_text = ref("kimi_cli.tools.file.check_fmt").check_json_text
    checked = 0
    for text in _json_corpus():
        expected = check_json_text(text)
        got = FILE.check_json_format(text)
        assert got == expected, (
            f"check_json_format mismatch for {text!r}\n"
            f"  python: {expected!r}\n  native: {got!r}"
        )
        checked += 1
    assert checked > 5000


def test_check_json_format_depth_limit_matches_orjson():
    """orjson caps container nesting at 1024 levels; yyjson is unlimited."""
    check_json_text = ref("kimi_cli.tools.file.check_fmt").check_json_text
    cases = []
    for n in (1023, 1024, 1025, 1026, 1100, 2000):
        cases.append("[" * n + "]" * n)
        cases.append('{"a":' * n + "1" + "}" * n)
    cases += [
        "[" * 1024 + "x" + "]" * 1024,
        "[" + '{"a":' * 1024 + "1" + "}" * 1024 + "]",
        "[" * 1023 + '{"a":1}' + "]" * 1023,
        "\n" + "[" * 1025 + "]" * 1025,
        '"' + "\u00e9" * 10 + '"' + "[" * 1025 + "]" * 1025,
        '"' + "[" * 3000 + '"',
    ]
    for text in cases:
        expected = check_json_text(text)
        got = FILE.check_json_format(text)
        assert got == expected, (
            f"depth-parity mismatch for {text[:40]!r} (len {len(text)})\n"
            f"  python: {expected!r}\n  native: {got!r}"
        )


def test_check_json_format_empty_document_wording():
    check_json_text = ref("kimi_cli.tools.file.check_fmt").check_json_text
    assert FILE.check_json_format("") == check_json_text("")
    assert FILE.check_json_format("") == (
        "JSON decode error at line 1, column 1: Input is a zero-length, empty document"
    )
    # whitespace-only input keeps the shared "input data is empty" wording
    assert FILE.check_json_format("  \n") == check_json_text("  \n")


def test_check_json_format_trailing_comma_columns():
    """The orjson column for a trailing comma (yyjson points elsewhere)."""
    check_json_text = ref("kimi_cli.tools.file.check_fmt").check_json_text
    for text in _TRAILING_COMMA_CASES:
        expected = check_json_text(text)
        got = FILE.check_json_format(text)
        assert got == expected, f"{text!r}\n  python: {expected!r}\n  native: {got!r}"


def test_validate_format_by_path_dispatch():
    """write.py 290-300: .json -> check; other formats -> Python-side fallback."""
    check_json_text = ref("kimi_cli.tools.file.check_fmt").check_json_text
    for path in ("x.json", "X.JSON", "a/b.JSON", "x.json.bak", "x.txt", "x", "x.py"):
        for text in ('{"a":1}', '{"a":1,}', ""):
            status, err = FILE.validate_format_by_path(path, text)
            if path.lower().endswith(".json"):
                assert status == "ok"
                assert err == check_json_text(text)
            else:
                assert status == "ok"
                assert err is None
    # no vendored YAML/TOML/XML parsers: the caller must fall back to Python
    for path in ("x.yaml", "x.YML", "x.toml", "x.xml"):
        status, err = FILE.validate_format_by_path(path, "a: [1")
        assert status == "unsupported"
        assert err is None


# ---------------------------------------------------------------------------
# Unified diff (utils/diff.py format_unified_diff)
# ---------------------------------------------------------------------------


def _diff_corpus():
    cases = [
        ("a\n", "b\n", "f.txt"),
        ("", "", "f.txt"),
        ("a\n", "a\n", "f.txt"),
        ("", "b\n", "f.txt"),
        ("a\n", "", "f.txt"),
        ("", "", ""),
        ("a\nb\nc\n", "a\nB\nc\n", "x/y.txt"),
        ("a\nb\nc\nd\ne\nf\ng\nh\n", "a\nb\nc\nd\nE\nf\ng\nh\n", "f"),
        ("a", "b", "f"),
        ("a\nb", "a\nb", "f"),
        ("a", "a\n", "f"),
        ("a\n", "a", "f"),
        ("a\r\nb\r\n", "a\r\nB\r\n", "f"),
        ("a\r\nb", "b\r\na", "f"),
        ("line\n" * 20, "line\n" * 19, "f"),
        ("line\n" * 20, "line\n" * 21, "f"),
        ("x\n" * 5 + "y\n" + "x\n" * 5, "x\n" * 5 + "z\n" + "x\n" * 5, "f"),
        ("\u00e9\n", "\u00e8\n", "h\u00e9llo.txt"),
        ("\u65e5\u672c\n\u8a9e\n", "\u65e5\u672c\n\u8a9e2\n", "f"),
        ("\U0001F389\n", "\U0001F389\U0001F389\n", "f"),
        ("a\n\n\n\nb\n", "a\nb\n", "f"),
        ("\n", "", "f"),
        ("", "\n", "f"),
    ]
    rnd = random.Random(7)
    pool = ["a", "b", "c", "d", "e", "x", "aa", "ab", "A", "\u00e9", "\U0001F389", ""]
    for _ in range(1500):
        old = "".join(rnd.choice(pool) + "\n" for _ in range(rnd.randint(0, 12)))
        new = "".join(rnd.choice(pool) + "\n" for _ in range(rnd.randint(0, 12)))
        if rnd.random() < 0.2:
            old = old.rstrip("\n")
        if rnd.random() < 0.2:
            new = new.rstrip("\n")
        cases.append((old, new, rnd.choice(["f.txt", "", "dir/f", "a\\b"])))
    return cases


def test_build_unified_diff_matches_difflib():
    diff_mod = ref("kimi_cli.utils.diff")
    with pure_python(diff_mod):
        # the helper must have disabled the native acceleration gate, so the
        # expected values below come from the difflib body, not from an older
        # build of a kernel.
        assert not diff_mod._native_use_native("DIFF")
        for old, new, path in _diff_corpus():
            for header in (True, False):
                expected = diff_mod.format_unified_diff(
                    old, new, path, include_file_header=header
                )
                got = FILE.build_unified_diff(old, new, path, header)
                assert got == expected, (
                    f"unified diff mismatch old={old!r} new={new!r} path={path!r} "
                    f"header={header}\n  python: {expected!r}\n  native: {got!r}"
                )


# ---------------------------------------------------------------------------
# auto-generated guard (auto_generated.py)
# ---------------------------------------------------------------------------

_AG_NAMES = [
    "zz_generated.go",
    "foo.pb.go",
    "bar_pb2.py",
    "baz_pb2_grpc.py",
    "x.gen.ts",
    "generated.py",
    "api.swagger.json",
    "svc.openapi.json",
    "svc.mock.go",
    "svc.mocks.ts",
    "main.go",
    "generated_report.go",
    "pb.go.bak",
    "mymocks_test.go",
    "gen.py",
    "ZZ_GENERATED.GO",
    "FOO.PB.GO",
    "a.PB.H",
    "GENERATED.PY",
    "A.SWAGGER.JSON",
    "a.mocks.js",
    "a.mock.py",
    "dir/zz_generated.go",
    "dir\\zz_generated.go",
    "generated.py/",
    "dir/zz_generated.go/",
    "",
    ".",
    "dir/",
    "zz_generated",
    "myzz_generated.go",
    "x.pb.go.bak",
    "_pb2.py",
    "a_pb2_grp.py",
    "x.gen.go.txt",
]

_AG_MARKERS = [
    ("# @generated\ncode here\n", "x.py"),
    ("// Code generated by protoc-gen-go. DO NOT EDIT.\npackage x\n", "x.go"),
    ("-- This file was automatically generated\nSELECT 1;\n", "q.sql"),
    ("// Generated by sqlc\npackage db\n", "db.go"),
    ("<!-- @generated -->\n<html></html>\n", "page.html"),
    ("// CODE GENERATED BY MOCKERY v2\npackage x\n", "x.go"),
    ("/* @generated */\nint x;\n", "x.c"),
    ("/*\n * @generated\n */\nint x;\n", "x.c"),
    ("// Generated by hand by the team\npackage x\n", "x.go"),
    ("\n".join(f"line {i}" for i in range(50)) + "\n# code generated by tool\n", "x.py"),
    ("// auto-generated\npackage x\n", "x.go"),
    ("# generated by hand\nprint(1)\n", "x.py"),
    ("// This module handles generated report output.\npackage x\n", "x.go"),
    ("package x\n\nfunc main() {}\n", "x.go"),
    ("\n".join(f"// filler {i}" for i in range(45)) + "\n// @generated\n", "x.go"),
    ("\n".join(f"// filler {i}" for i in range(39)) + "\n// @generated\n", "x.go"),
    ("anything", "zz_generated.go"),
    ("#!/usr/bin/env python3\n# @generated\nprint(1)\n", "x.py"),
    ("\ufeff# @generated\nprint(1)\n", "x.py"),
    ("// header\n\ncode line\n// later comment\n", "x.go"),
    ("# part one\n\n# part two\nx = 1\n", "x.py"),
    ("code only\n", "x.py"),
    ("// " + "x" * 1024 + "\n// @generated\n", "x.go"),
    ("# @generatedx @generated\n", "x.py"),
    ("# @generatedz\n# @generated\n", "x.py"),
    ("# @generated_helper\n", "x.py"),
    ("# @generated\n", "x.PY"),
    ("# @generated\n", "Makefile"),
    ("# @generated\n", "Dockerfile"),
    ("", "x.py"),
    ("\n\n\n", "x.go"),
    ("# @generated", "x.py"),
    ("#@generated", "x.py"),
    ("@generated\n", "x.py"),
    ("#  @generated\n", "x.py"),
    ("# Code generated by   protoc   \n", "x.go"),
    ("# code generated\nby sqlc\n", "x.py"),
    ("# generated by protoc-gen-go\n", "x.py"),
    ("# generated by protoc-gen-\n", "x.py"),
    ("/* @generated\n * more\n */\ncode\n", "x.c"),
    ("<!--\n@generated\n-->\ncode\n", "x.html"),
    ("# generated by unknown-tool\n", "x.py"),
    ("# generated by buf\n", "x.py"),
    ("# generated by openapi-generator\n", "x.py"),
    ("# generated by napi-rs\n", "x.py"),
    ("# generated by mockery\n", "x.py"),
    ("# generated by stringer\n", "x.py"),
    ("# generated by easyjson\n", "x.py"),
    ("# generated by deepcopy-gen\n", "x.py"),
    ("# generated by lister-gen\n", "x.py"),
    ("# generated by kysely-codegen\n", "x.py"),
    ("# generated by swagger-codegen\n", "x.py"),
    ("# generated by swagger\n", "x.py"),
    ("# generated by grpc-gateway\n", "x.py"),
    ("# generated by protoc-gen-x\n", "x.py"),
    ("# generated by protoc\n", "x.py"),
    ("# generated by generated\n", "x.py"),
    ("# generated by sqlcard\n", "x.py"),
    ("# this  file   was  automatically   generated\n", "x.py"),
    ("# This file was automatically generated.\n", "x.py"),
    ("# this file was automatically generatedx\n", "x.py"),
    ("# xthis file was automatically generated\n", "x.py"),
    ("# codegenerated by protoc\n", "x.py"),
    ("# @generatedx\n", "x.py"),
    ("# @generated_x\n", "x.py"),
    ("#\n#\n# @generated\n", "x.py"),
    ("   \n# @generated\n", "x.py"),
    # 1 KiB prefix: characters (not bytes) - Python slices 1024 chars
    ("#" + "\u00e9" * 600 + "\n# @generated\n", "x.py"),
    ("#" + "\u00e9" * 500 + "\n# @generated\n", "x.py"),
    ("// " + "\U0001F389" * 200 + "\n// @generated\n", "x.go"),
    # Python str whitespace (str.strip) in the header scanner
    ("\u00a0# @generated\n", "x.py"),
    ("\u2028# @generated\n", "x.py"),
    ("#\u00a0@generated\n", "x.py"),
    ("\x1c# @generated\n", "x.py"),
]


def test_is_auto_generated_file_name_matches_reference():
    is_ag = ref("kimi_cli.tools.file.auto_generated").is_auto_generated_file_name
    for name in _AG_NAMES:
        assert FILE.is_auto_generated_file_name(name) == is_ag(name), name


def test_detect_auto_generated_marker_matches_reference():
    detect = ref("kimi_cli.tools.file.auto_generated").detect_auto_generated_marker
    for content, path in _AG_MARKERS:
        expected = detect(content, path)
        got = FILE.detect_auto_generated_marker(content, path)
        assert got == expected, (
            f"content={content[:60]!r} path={path!r}\n"
            f"  python: {expected!r}\n  native: {got!r}"
        )


def test_detect_auto_generated_marker_ascii_fuzz_matches_reference():
    detect = ref("kimi_cli.tools.file.auto_generated").detect_auto_generated_marker
    pieces = [
        "#", "//", "/*", "*/", "<!--", "-->", "--", "@generated", "@GENERATED",
        "@generatedx", "@generated_", "code", "generated", "by", "this", "file",
        "was", "automatically", "protoc", "protoc-gen-go", "sqlc", "buf", "swagger",
        "swagger-codegen", "openapi", "openapi-generator", "grpc-gateway", "mockery",
        "stringer", "deepcopy-gen", "napi-rs", "lister-gen", "sqlcard", "\n", " ",
        "\t", "\r\n", "x", "!", ".", "-", "_", "#!/bin/sh", ":", "1", "\x0b",
        "\x0c", "\x1c",
    ]
    paths = [
        "x.py", "x.go", "x.c", "x.html", "x.sql", "x.unknown", "Makefile", "x.ts",
        "zz_generated.go",
    ]
    rnd = random.Random(31337)
    for _ in range(3000):
        body = "".join(rnd.choice(pieces) for _ in range(rnd.randint(1, 20)))
        path = rnd.choice(paths)
        expected = detect(body, path)
        got = FILE.detect_auto_generated_marker(body, path)
        assert got == expected, (
            f"content={body!r} path={path!r}\n"
            f"  python: {expected!r}\n  native: {got!r}"
        )


def test_detect_auto_generated_marker_known_non_ascii_gap():
    """Documented deviation: the scanners use ASCII \\b/\\w.

    ``regex`` treats U+00E9 as a word character, so ``@generated\\b`` does NOT
    match ``"<!--@generated\\u00e9"``; the ASCII scanner does.  Pinned here so a
    future Unicode-aware rewrite has to update this test on purpose.
    """
    detect = ref("kimi_cli.tools.file.auto_generated").detect_auto_generated_marker
    content = "<!--@generated\u00e9"
    assert detect(content, "x.unknown") is None
    assert FILE.detect_auto_generated_marker(content, "x.unknown") == "@generated"
