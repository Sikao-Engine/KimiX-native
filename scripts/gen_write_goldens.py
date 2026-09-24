#!/usr/bin/env python3
"""Regenerate the `write` tool goldens (tests/unit/builtin_tools/write_goldens.inc).

Every expected value is produced by running the *real* Python implementation
from the kimi-agent checkout (never a kimix_native mirror, never a hand-typed
constant):

  * kimi_cli.tools.file.conflict_detect - match_marker / is_separator /
        scan_conflict_lines / find_dangling_openers / splice_conflict /
        expand_content_tokens / render_conflict_region / conflict_regions_equal /
        conflict_region_present / format_conflict_summary / parse_conflict_uri /
        parse_bulk_directives
  * kimi_cli.tools.file.write          - WriteFile._conflict_guard /
        WriteFile._conflict_markers_error (the mode-aware write guard)
  * kimi_cli.tools.file.write          - decide_parent_dir / expected_write_size
        semantics (write.py 246-261 / 388-395) re-derived from the reference
  * CPython's utf-8 strict decoder   - utf8_decode_error reasons

Usage:
    python scripts/gen_write_goldens.py            # write the .inc
    python scripts/gen_write_goldens.py --check    # fail when stale
    python scripts/gen_write_goldens.py --reference DIR

The generated file is pure ASCII (every byte outside the printable ASCII range
is a 3-digit octal escape, which is never ambiguous with the following
character). Records are packed into single strings so the C++ side only needs
one formatter per kernel; the packing is documented in the file header:

  FLD = \\x01 between the fields of one conflict block
  LIN = \\x02 between the lines of one line list
  BLK = \\x03 between blocks
"""

from __future__ import annotations

import argparse
import asyncio
import os
import random
import sys
from pathlib import Path

DEFAULT_REFERENCE = os.environ.get("KIMI_AGENT_ROOT", r"C:/dev/kimi-agent")
DEFAULT_OUT = Path("tests/unit/builtin_tools/write_goldens.inc")

FLD = "\x01"
LIN = "\x02"
BLK = "\x03"


# ---------------------------------------------------------------------------
# reference import
# ---------------------------------------------------------------------------
def import_reference(root: Path):
    for p in (str(root / "kimi-cli" / "src"), str(root / "src")):
        if p not in sys.path:
            sys.path.insert(0, p)
    import importlib

    return {
        "conflict": importlib.import_module("kimi_cli.tools.file.conflict_detect"),
        "write": importlib.import_module("kimi_cli.tools.file.write"),
        "auto": importlib.import_module("kimi_cli.tools.file.auto_generated"),
    }


# ---------------------------------------------------------------------------
# C++ literal helpers
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
    """A C++ string literal for *data* (str or bytes), chunked and ASCII-only.

    Surrogates are encoded with ``surrogatepass`` so a lone surrogate in a
    corpus string becomes the byte sequence CPython's strict utf-8 decoder
    rejects (which is what the kernel sees).
    """
    if isinstance(data, str):
        data = data.encode("utf-8", "surrogatepass")
    elif not isinstance(data, (bytes, bytearray)):
        data = str(data).encode("utf-8", "surrogatepass")
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


def enc_lines(lines) -> str:
    return LIN.join(lines or ())


def enc_block(b) -> str:
    base = b.base_line if b.base_line is not None else -1
    return FLD.join(
        [
            str(b.start_line),
            str(b.separator_line),
            str(b.end_line),
            str(base),
            b.ours_label or "-",
            b.base_label or "-",
            b.theirs_label or "-",
            enc_lines(b.ours_lines),
            enc_lines(b.base_lines),
            enc_lines(b.theirs_lines),
        ]
    )


def enc_blocks(blocks) -> str:
    return BLK.join(enc_block(b) for b in blocks)


# ---------------------------------------------------------------------------
# corpora
# ---------------------------------------------------------------------------
CONFLICT_TEXT = "a\nb\n<<<<<<< HEAD\nours\n=======\ntheirs\n>>>>>>> t\nc\nd\n"

TWO_WAY = "<<<<<<< HEAD\nours line\n=======\ntheirs line\n>>>>>>> feature/x\n"
DIFF3 = "<<<<<<< HEAD\nours\n||||||| base\nbase line\n=======\ntheirs\n>>>>>>> other\n"

CURATED_TEXTS = [
    ("two_way_labels", TWO_WAY),
    ("two_way_bare", "<<<<<<<\nours\n=======\ntheirs\n>>>>>>>\n"),
    ("two_way_no_headers", "<<<<<<< HEAD\na\n=======\nb\n>>>>>>> t"),
    ("diff3", DIFF3),
    ("diff3_bare_base", "<<<<<<<\na\n|||||||\nb\n=======\nc\n>>>>>>>\n"),
    ("with_context", CONFLICT_TEXT),
    ("two_blocks", TWO_WAY + "middle\n" + DIFF3),
    ("adjacent_blocks", TWO_WAY + TWO_WAY),
    ("empty_sections", "<<<<<<<\n=======\n>>>>>>>\n"),
    ("empty_ours", "<<<<<<< H\n=======\nt\n>>>>>>> T\n"),
    ("empty_theirs", "<<<<<<< H\no\n=======\n>>>>>>> T\n"),
    ("unclosed", "<<<<<<< HEAD\nours\n=======\ntheirs\n"),
    ("unclosed_ours_only", "text\n<<<<<<< HEAD\nours\n"),
    ("stray_separator", "=======\n>>>>>>> x\n"),
    ("stray_base", "||||||| stray\n<<<<<<< HEAD\na\n=======\nb\n>>>>>>> t\n"),
    ("stray_theirs", ">>>>>>> t\n<<<<<<< H\na\n=======\nb\n>>>>>>> t\n"),
    ("nested_opener", "<<<<<<< first\n<<<<<<< second\na\n=======\nb\n>>>>>>> t\n"),
    ("nested_after_ours", "<<<<<<< first\na\n<<<<<<< second\nb\n=======\nc\n>>>>>>> t\n"),
    ("nested_in_theirs", "<<<<<<< first\na\n=======\nb\n<<<<<<< second\nc\n=======\nd\n>>>>>>> t\n"),
    ("double_separator", "<<<<<<< H\na\n=======\n=======\nb\n>>>>>>> t\n"),
    ("base_after_separator", "<<<<<<< H\na\n=======\n||||||| b\nx\n>>>>>>> t\n"),
    ("base_twice", "<<<<<<< H\na\n|||||||\nb\n|||||||\nc\n=======\nd\n>>>>>>> t\n"),
    ("theirs_then_more", "<<<<<<< H\na\n=======\nb\n>>>>>>> t\n>>>>>>> u\n"),
    ("label_two_spaces", "<<<<<<<  two\na\n=======\nb\n>>>>>>>  three\n"),
    ("label_trailing_space", "<<<<<<< \na\n=======\nb\n>>>>>>> \n"),
    ("indented_markers", " <<<<<<< H\na\n=======\nb\n>>>>>>> t\n"),
    ("eight_arrows", "<<<<<<<< H\na\n========\nb\n>>>>>>>> t\n"),
    ("crlf", TWO_WAY.replace("\n", "\r\n")),
    ("crlf_diff3", DIFF3.replace("\n", "\r\n")),
    ("cr_only", "<<<<<<< H\ra\r=======\rb\r>>>>>>> t\r"),
    ("mixed_crlf_lf", "<<<<<<< H\r\na\n=======\r\nb\n>>>>>>> t\n"),
    ("empty", ""),
    ("plain", "no markers here\nx = 1\n"),
    ("non_ascii", "<<<<<<< héad\noursé\n=======\ntheirs\n>>>>>>> 日本\n"),
    ("emoji", "<<<<<<< 🎉\na\n=======\nb\n>>>>>>> 🎉\n"),
    ("tabs", "<<<<<<<\ta\n=======\nb\n>>>>>>>\tc\n"),
    ("long_lines", "<<<<<<< H\n" + "x" * 300 + "\n=======\n" + "y" * 300 + "\n>>>>>>> t\n"),
    ("blank_lines_inside", "<<<<<<< H\n\na\n\n=======\n\nb\n\n>>>>>>> t\n"),
    ("marker_like_in_body", "<<<<<<< H\n<<<<<<<\n=======\n>>>>>>>\n>>>>>>> t\n"),
    ("ctr_chars", "<<<<<<< H\n\x01\x02\n=======\n\x03\n>>>>>>> t\n"),
    ("only_openers", "<<<<<<<\n<<<<<<<\n<<<<<<<\n"),
    ("trailing_newline_missing", "<<<<<<< H\na\n=======\nb\n>>>>>>> t"),
    ("block_at_eof_no_newline", "x\n<<<<<<< H\na\n=======\nb\n>>>>>>> t"),
    ("many_blocks", (TWO_WAY + "sep\n") * 3),
    ("malformed_reset_reprocess", "<<<<<<< A\na\n||||||| B\n<<<<<<< C\nc\n=======\nd\n>>>>>>> t\n"),
]

# ---------------------------------------------------------------------------
# per-kernel corpora
# ---------------------------------------------------------------------------
SPLICE_REPLACEMENTS = [
    "",
    "resolved",
    "one\ntwo",
    "b\nours\nc\nd",
    "b\nours",
    "ours\nc",
    "c\nd",
    "a\nb\nours\nc\nd\ne",
    "b",
    "c",
    "f(",
    "f(x)",
    "b\nf(",
    "b\nf(x)",
    "@ours",
    "\n",
    "\n\n",
    "trailing\n",
    "ours\n=======\ntheirs",
    ">>>>>>> t",
    "<<<<<<< HEAD",
]

TOKEN_CONTENTS = [
    "@ours",
    "@theirs",
    "@base",
    "@both",
    "before\n@ours\nafter",
    "  @ours  ",
    "@ours\n@ours",
    "@both\n@theirs",
    "@OURS",
    "@oursx",
    "x@ours",
    "keep\nme",
    "",
    "\n",
    "@base\n@both",
    "@theirs\n\n@ours",
]

RENDER_SCOPES = [None, "ours", "theirs", "base", "bogus"]

URI_CASES = [
    "conflict://1",
    "conflict://12",
    "conflict://*",
    "conflict://1/ours",
    "conflict://3/theirs",
    "conflict://3/base",
    "conflict://1/bogus",
    "conflict://*/ours",
    "src/x.py:conflict://4",
    "a/b/c.py:conflict://7",
    "conflict://",
    "conflict://abc",
    "conflict://0",
    "conflict://-1",
    "conflict://+5",
    "conflict:// 5",
    "conflict://5 ",
    "conflict://1/",
    "conflict://1/ours/x",
    "conflict://*x",
    "x:conflict://",
    "x:conflict://1",
    ":conflict://1",
    "conflict://1:",
    "src/x.py",
    "",
    "whatever",
    "CONFLICT://1",
    " conflict://1",
    "conflict://1 ",
    "dir/conflict://2",
    "://conflict://3",
    "a:conflict://b",
    "conflict://1/ours/",
    "\u00e9:conflict://9",
]

BULK_CASES = [
    "1: @ours",
    "1: @ours\n2: @theirs",
    "1: @ours\n\n2: @theirs\n",
    "1: @both\n",
    "1:@ours",
    " 1 : @ours ",
    "1: @ours\nplain line",
    "",
    "\n\n",
    " \n",
    "1: @ours\n1: @theirs",
    "0: @ours",
    "1: @bogus",
    "1: @ours extra",
    "x: @ours",
    "1 @ours",
    "1: @ours\n2: @base",
    "\t1:\t@ours\t",
    "1: @OURS",
    "1: @ours\n",
    "1: @ours\n\n",
]


def guard_cases():
    """(name, display_path, old_text, new_content, append, file_existed, allow)"""
    out = []
    texts = [("empty", ""), ("clean", "a\nb\n"), ("two_way", TWO_WAY),
             ("diff3", DIFF3), ("unclosed", "x\n<<<<<<< H\nours\n"),
             ("unclosed_mid", "x\n<<<<<<< H\nours\n>>>>>>> t\n"),
             ("crlf", TWO_WAY.replace("\n", "\r\n")),
             ("crlf_unclosed", "x\r\n<<<<<<< H\r\nours\r\n")]
    contents = ["clean text\n", "a\nb\n", "more\n", "", TWO_WAY, DIFF3,
                "<<<<<<< H\nours\n=======\ntheirs\n>>>>>>> t\n", "x\n<<<<<<< H\nours\n",
                "@ours\n", TWO_WAY.replace("\n", "\r\n")]
    for tname, t in texts:
        for cname, c in zip(["clean", "ab", "more", "empty", "tw", "d3", "tw2", "unclosed2",
                             "token", "crlf_tw"], contents):
            for append in (True, False):
                for existed in (True, False):
                    for allow in (True, False):
                        out.append((f"{tname}_{cname}_append{int(append)}_exists{int(existed)}"
                                    f"_allow{int(allow)}",
                                    "/tmp/f.py", t, c, append, existed, allow))
    return out


PARENT_CASES = [
    ("exists", True, True, "a/b.py", "a", False, ""),
    ("exists_nomkdir", True, False, "a/b.py", "a", False, ""),
    ("missing_mkdir_ok", False, True, "a/b.py", "a", False, ""),
    ("missing_mkdir_fail", False, True, "a/b.py", "a", True, "Permission denied"),
    ("missing_nomkdir", False, False, "a/b.py", "a", False, ""),
    ("missing_nomkdir_err", False, False, "a/b.py", "/root/a", False, ""),
    ("missing_mkdir_oserror", False, True, "/o/b.py", "/o", True,
     "[Errno 13] Permission denied: '/o'"),
    ("empty_parent", False, True, "b.py", "", True, "boom"),
]

SIZE_CASES = [
    ("overwrite_ascii", False, "", "", "abc"),
    ("overwrite_utf8", False, "", "", "abc\u2192"),
    ("overwrite_emoji", False, "", "", "\U0001F389"),
    ("overwrite_empty", False, "", "", ""),
    ("overwrite_newline", False, "", "", "a\n"),
    ("append_ascii", True, "ab", "cd", ""),
    ("append_utf8", True, "a\u2192", "\U0001F389", ""),
    ("append_empty", True, "", "", ""),
    ("invalid_overwrite", False, "", "", "a\udc80"),
    ("invalid_append_old", True, "a\udc80", "b", ""),
    ("invalid_append_content", True, "a", "b\udc80", ""),
]

UTF8_CASES = [
    ("empty", b""),
    ("ascii", b"hello"),
    ("multi", "h\u00e9llo \u2192 \U0001F389".encode("utf-8")),
    ("lone_continuation", b"\x80"),
    ("lone_continuation_mid", b"ab\x80cd"),
    ("truncated_2", b"\xc3"),
    ("truncated_3", b"\xe2\x86"),
    ("truncated_4", b"\xf0\x9f\x8e"),
    ("overlong2", b"\xc0\xaf"),
    ("overlong3", b"\xe0\x80\x80"),
    ("overlong4", b"\xf0\x80\x80\x80"),
    ("surrogate", b"\xed\xa0\x80"),
    ("above_max", b"\xf4\x90\x80\x80"),
    ("above_max2", b"\xf5\x80\x80\x80"),
    ("bad_cont", b"\xe2\x28\xa1"),
    ("ff", b"\xff"),
    ("fe", b"\xfe"),
    ("c0", b"\xc0"),
    ("f8", b"\xf8\x88\x80\x80\x80"),
    ("mixed", b"abc\xe2\x86\x92\xff"),
    ("nul", b"a\x00b"),
]


def make_cases(ref):
    """Build every golden list (all expected values come from the reference)."""
    cd = ref["conflict"]
    write = ref["write"]

    def scan(text):
        return cd.scan_conflict_lines(text.split("\n"), 1)

    def entry_for(text, index=0, id_=1):
        blocks = scan(text)
        b = blocks[index]
        return cd.ConflictEntry(
            start_line=b.start_line,
            separator_line=b.separator_line,
            end_line=b.end_line,
            base_line=b.base_line,
            ours_label=b.ours_label,
            base_label=b.base_label,
            theirs_label=b.theirs_label,
            ours_lines=b.ours_lines,
            base_lines=b.base_lines,
            theirs_lines=b.theirs_lines,
            id=id_,
            absolute_path="/tmp/x.py",
            display_path="/tmp/x.py",
        )

    # ---- scan goldens (curated + fuzz) -----------------------------------
    texts = list(CURATED_TEXTS)
    rnd = random.Random(20240607)
    marker_lines = [
        "<<<<<<<", "<<<<<<< HEAD", "<<<<<<<  two", "<<<<<<< ",
        "=======", "======= x", "|||||||", "||||||| base",
        ">>>>>>>", ">>>>>>> t", ">>>>>>>  two",
        "plain", "", "a", "// x", "# y", "\t", " ", "<<<<<<<HEAD",
        " <<<<<<<", ">>>>>>> t\r", "<<<<<<< H\r", "=======\r",
    ]
    for i in range(140):
        n = rnd.randint(0, 12)
        body = "\n".join(rnd.choice(marker_lines) for _ in range(n))
        if rnd.random() < 0.5:
            body += "\n"
        texts.append((f"fuzz{i}", body))
    for i in range(20):
        n = rnd.randint(1, 6)
        body = "\n".join(rnd.choice(marker_lines) for _ in range(n))
        texts.append((f"fuzz_crlf{i}", body.replace("\n", "\r\n")))

    scan_goldens = [(name, text, enc_blocks(scan(text))) for name, text in texts]

    # ---- dangling openers -------------------------------------------------
    dangling_goldens = []
    for name, text in texts:
        lines = text.replace("\r\n", "\n").splitlines() if text else []
        got = cd.find_dangling_openers(lines)
        dangling_goldens.append((name, text,
                                 LIN.join(f"{n}{FLD}{t}" for n, t in got)))

    # ---- splice -----------------------------------------------------------
    splice_goldens = []

    def py_entry(recorded, idx, path="/tmp/x.py"):
        b = scan(recorded)[idx]
        return cd.ConflictEntry(
            start_line=b.start_line, separator_line=b.separator_line,
            end_line=b.end_line, base_line=b.base_line, ours_label=b.ours_label,
            base_label=b.base_label, theirs_label=b.theirs_label,
            ours_lines=b.ours_lines, base_lines=b.base_lines,
            theirs_lines=b.theirs_lines, id=1, absolute_path=path, display_path=path)

    def add_splice(name, recorded, current, idx, repl):
        e = py_entry(recorded, idx)
        try:
            sp = cd.splice_conflict(current, e, repl)
        except cd.ConflictError as exc:
            splice_goldens.append((name, recorded, current, idx, repl, 0, "", 0, 0,
                                   exc.message))
            return
        splice_goldens.append((name, recorded, current, idx, repl, 1, sp.text,
                               sp.trimmed_leading, sp.trimmed_trailing, ""))

    splice_sources = [
        ("context", CONFLICT_TEXT, 0),
        ("two_way", TWO_WAY, 0),
        ("diff3", DIFF3, 0),
        ("diff3_bare", "<<<<<<<\na\n|||||||\nb\n=======\nc\n>>>>>>>\n", 0),
        ("empty_sections", "<<<<<<<\n=======\n>>>>>>>\n", 0),
        ("crlf", TWO_WAY.replace("\n", "\r\n"), 0),
        ("crlf_context", CONFLICT_TEXT.replace("\n", "\r\n"), 0),
        ("two_blocks", TWO_WAY + "mid\n" + DIFF3, 1),
        ("no_trailing_nl", "x\n" + TWO_WAY.rstrip("\n"), 0),
        ("labels", "<<<<<<< H\nours\n=======\ntheirs\n>>>>>>> T\n", 0),
    ]
    for sname, recorded, idx in splice_sources:
        for r_i, repl in enumerate(SPLICE_REPLACEMENTS):
            add_splice(f"{sname}_r{r_i}", recorded, recorded, idx, repl)
    # shifted / altered / missing current texts
    recorded = CONFLICT_TEXT
    for cname, current in [
        ("shifted", "NEW1\nNEW2\n" + CONFLICT_TEXT),
        ("shifted_far", "x\n" * 20 + CONFLICT_TEXT),
        ("missing", "no conflicts here\n"),
        ("altered_ours", CONFLICT_TEXT.replace("ours", "OURS")),
        ("reordered", "a\nb\n=======\ntheirs\n<<<<<<< HEAD\nours\n>>>>>>> t\nc\nd\n"),
        ("duplicated", CONFLICT_TEXT + CONFLICT_TEXT),
        ("crlf_current", CONFLICT_TEXT.replace("\n", "\r\n")),
        ("same_shift", CONFLICT_TEXT),
    ]:
        for r_i, repl in enumerate(["resolved", "", "b\nours\nc\nd", "ours"]):
            add_splice(f"{cname}_r{r_i}", recorded, current, 0, repl)

    # ---- tokens -----------------------------------------------------------
    token_goldens = []
    for sname, src, idx in [("context", CONFLICT_TEXT, 0), ("diff3", DIFF3, 0),
                            ("bare", "<<<<<<<\na\n=======\nb\n>>>>>>>\n", 0)]:
        for c_i, content in enumerate(TOKEN_CONTENTS):
            e = py_entry(src, idx)
            try:
                out = cd.expand_content_tokens(content, e)
            except cd.ConflictError as exc:
                token_goldens.append((f"{sname}_c{c_i}", src, idx, content, 0, "",
                                      exc.message))
                continue
            token_goldens.append((f"{sname}_c{c_i}", src, idx, content, 1, out, ""))

    # ---- render -----------------------------------------------------------
    render_goldens = []
    for sname, src, idx in [("context", CONFLICT_TEXT, 0), ("diff3", DIFF3, 0),
                            ("empty_sections", "<<<<<<<\n=======\n>>>>>>>\n", 0),
                            ("multi", TWO_WAY + "mid\n" + DIFF3, 0)]:
        for scope in RENDER_SCOPES:
            e = py_entry(src, idx)
            label = "none" if scope is None else scope
            try:
                lines, start = cd.render_conflict_region(e, scope)
            except cd.ConflictError as exc:
                render_goldens.append((f"{sname}_{label}", src, idx,
                                       "" if scope is None else scope, 0, 0, "",
                                       exc.message))
                continue
            render_goldens.append((f"{sname}_{label}", src, idx,
                                   "" if scope is None else scope, 1, start,
                                   LIN.join(lines), ""))

        # ---- region equality / presence ---------------------------------------
        region_sources = [
            ("context", CONFLICT_TEXT),
            ("two_way", TWO_WAY),
            ("diff3", DIFF3),
            ("empty_sections", "<<<<<<<\n=======\n>>>>>>>\n"),
            ("multi", TWO_WAY + "mid\n" + DIFF3),
            ("crlf", TWO_WAY.replace("\n", "\r\n")),
            ("labels", "<<<<<<< H\nours\n=======\ntheirs\n>>>>>>> T\n"),
            ("bare", "<<<<<<<\na\n=======\nb\n>>>>>>>\n"),
        ]
        region_equal_goldens = []
        for an, asrc in region_sources:
            for bn, bsrc in region_sources:
                for ai in range(len(scan(asrc))):
                    for bi in range(len(scan(bsrc))):
                        ea = py_entry(asrc, ai)
                        eb = py_entry(bsrc, bi)
                        region_equal_goldens.append(
                            (f"{an}{ai}_{bn}{bi}", asrc, ai, bsrc, bi,
                             1 if cd.conflict_regions_equal(ea, eb) else 0))
        region_present_goldens = []
        probe_contents = [
            CONFLICT_TEXT, TWO_WAY, DIFF3, "", "clean\n",
            TWO_WAY.replace("\n", "\r\n"), CONFLICT_TEXT.replace("\n", "\r\n"),
            "prefix\n" + TWO_WAY + "suffix\n",
        ]
        for pname, content in enumerate(probe_contents):
            for sn, src in region_sources:
                for i in range(len(scan(src))):
                    e = py_entry(src, i)
                    region_present_goldens.append(
                        (f"p{pname}_{sn}{i}", content, src, i,
                         1 if cd.conflict_region_present(content, e) else 0))

    # ---- summary ----------------------------------------------------------
    summary_goldens = []
    for sname, src in [("none", "plain\n"), ("one", CONFLICT_TEXT), ("two", TWO_WAY + DIFF3),
                       ("diff3", DIFF3), ("empty_sections", "<<<<<<<\n=======\n>>>>>>>\n")]:
        for path in ["/tmp/x.py", "", "dir/f.py"]:
            for trunc in (False, True):
                blocks = scan(src)
                entries = [
                    cd.ConflictEntry(
                        start_line=b.start_line, separator_line=b.separator_line,
                        end_line=b.end_line, base_line=b.base_line,
                        ours_label=b.ours_label, base_label=b.base_label,
                        theirs_label=b.theirs_label, ours_lines=b.ours_lines,
                        base_lines=b.base_lines, theirs_lines=b.theirs_lines,
                        id=i + 1, absolute_path=path, display_path=path)
                    for i, b in enumerate(blocks)
                ]
                out = cd.format_conflict_summary(entries, display_path=path,
                                                 scan_truncated=trunc)
                summary_goldens.append((f"{sname}_{path or 'nopath'}_{int(trunc)}", src,
                                        path, 1 if trunc else 0, out))

    # ---- uri --------------------------------------------------------------
    uri_goldens = []
    for raw in URI_CASES:
        try:
            parsed = cd.parse_conflict_uri(raw)
        except cd.ConflictError as exc:
            uri_goldens.append((raw, raw, 3, "0", "", "", exc.message))
            continue
        if parsed is None:
            uri_goldens.append((raw, raw, 0, "0", "", "", ""))
        elif parsed.id == "*":
            uri_goldens.append((raw, raw, 1, "0", parsed.scope or "", "", ""))
        else:
            uri_goldens.append((raw, raw, 2, str(int(parsed.id)), parsed.scope or "",
                                parsed.recovered_prefix or "", ""))

    # ---- uri deviations ---------------------------------------------------
    # Python int() accepts underscores and non-ASCII decimal digits and has
    # unbounded precision; the port documents a strict ASCII/int64
    # approximation (port report deviation 4). Record the C++ behaviour of
    # those inputs explicitly instead of asserting equality with Python.
    uri_dev_goldens = []
    for raw in ["conflict://1_0", "conflict://1__0", "conflict://٣",
                "conflict://１", "conflict://999999999999999999999999",
                "conflict://-999999999999999999999999"]:
        try:
            parsed = cd.parse_conflict_uri(raw)
            py_repr = "None" if parsed is None else str(parsed.id)
        except cd.ConflictError as exc:
            py_repr = "ConflictError: " + exc.message
        id_part = raw[len("conflict://"):]
        message = (
            f"Invalid conflict id '{id_part}' in '{raw}'. Expected conflict://<N> "
            "or conflict://<N>/<ours|theirs|base>."
        )
        uri_dev_goldens.append((raw, raw, py_repr, message))

    # ---- bulk -------------------------------------------------------------
    bulk_goldens = []
    for i, content in enumerate(BULK_CASES):
        got = cd.parse_bulk_directives(content)
        expect = "NONE" if got is None else ";".join(
            f"{k}:{v}" for k, v in got.items())
        bulk_goldens.append((f"bulk{i}", content, expect))

    # ---- bulk deviations --------------------------------------------------
    # Python's int() is unbounded; the port keeps the ids inside int32 (the
    # ConflictEntry id is an int32 in the C++ struct) and reports "not every
    # line is a directive" for anything larger (port report deviation 4).
    bulk_dev_goldens = []
    for content in ["99999999999999999999: @ours", "2147483648: @ours",
                    "2147483647: @ours", "1_0: @ours", "٣: @ours"]:
        got = cd.parse_bulk_directives(content)
        py_repr = "NONE" if got is None else ";".join(f"{k}:{v}" for k, v in got.items())
        bulk_dev_goldens.append((content, content, py_repr))

    # ---- guard ------------------------------------------------------------
    host = type("_GuardHost", (), {})
    host._conflict_markers_error = write.WriteFile._conflict_markers_error

    def run_guard(display_path, old_text, new_content, append, file_existed, allow):
        return asyncio.run(
            write.WriteFile._conflict_guard(
                host(), display_path, old_text, new_content,
                "append" if append else "overwrite",
                file_existed=file_existed, allow_conflicts=allow,
            )
        )

    guard_goldens = []
    for name, path, old, new, append, existed, allow in guard_cases():
        err, note, old_had = run_guard(path, old, new, append, existed, allow)
        guard_goldens.append((name, path, old, new, 1 if append else 0,
                              1 if existed else 0, 1 if allow else 0,
                              "" if err is None else err.message, note,
                              1 if old_had else 0))

    # ---- parent dir -------------------------------------------------------
    parent_goldens = []
    for name, exists, mkdir, display, parent, has_err, err in PARENT_CASES:
        # write.py 246-261 semantics (the kernel takes the decision as data)
        if exists:
            status, message = "ok", ""
        elif not mkdir:
            status = "not_found"
            message = (f"Parent directory does not exist: {parent}. "
                       "Set mkdir=True to create it.")
        elif has_err:
            status = "invalid_input"
            message = (f"Failed to create parent directory for {display}: {err}")
        else:
            status, message = "ok", ""
        parent_goldens.append((name, 1 if exists else 0, 1 if mkdir else 0, display,
                               parent, 1 if has_err else 0, err,
                               status, message))

    # ---- expected size ----------------------------------------------------
    size_goldens = []
    for name, append, old, content, new_text in SIZE_CASES:
        try:
            if append:
                size = len(old.encode("utf-8")) + len(content.encode("utf-8"))
            else:
                size = len(new_text.encode("utf-8"))
            has = 1
        except UnicodeEncodeError:
            size, has = 0, 0
        size_goldens.append((name, 1 if append else 0, old, content, new_text,
                             has, size))

    # ---- utf8 -------------------------------------------------------------
    utf8_goldens = []
    for name, data in UTF8_CASES:
        try:
            data.decode("utf-8", "strict")
            utf8_goldens.append((name, data, len(data), 0, ""))
        except UnicodeDecodeError as exc:
            utf8_goldens.append((name, data, len(data), 1, exc.reason))

    return {
        "scan": scan_goldens,
        "dangling": dangling_goldens,
        "splice": splice_goldens,
        "token": token_goldens,
        "render": render_goldens,
        "region_equal": region_equal_goldens,
        "region_present": region_present_goldens,
        "summary": summary_goldens,
        "uri": uri_goldens,
        "uri_dev": uri_dev_goldens,
        "bulk": bulk_goldens,
        "bulk_dev": bulk_dev_goldens,
        "guard": guard_goldens,
        "parent": parent_goldens,
        "size": size_goldens,
        "utf8": utf8_goldens,
    }


def render_inc(data) -> str:
    out = []
    out.append(
        "// GENERATED by scripts/gen_write_goldens.py - DO NOT EDIT BY HAND.\n"
        "// Every expected value comes from running the real Python implementation\n"
        "// in the kimi-agent checkout (kimi_cli.tools.file.conflict_detect,\n"
        "// kimi_cli.tools.file.write, CPython's utf-8 strict decoder).\n"
        "// Regenerate with:\n"
        "//   python scripts/gen_write_goldens.py\n"
        "// Conventions:\n"
        "// * conflict blocks are packed as\n"
        "//     start \\x01 sep \\x01 end \\x01 base(-1 = 2-way) \\x01 ours_label(- = none)\n"
        "//     \\x01 base_label \\x01 theirs_label \\x01 ours_lines \\x01 base_lines\n"
        "//     \\x01 theirs_lines\n"
        "//   where line lists join their lines with \\x02 and '\\\\x01' is the literal\n"
        "//   byte 0x01 (the C++ writer emits 0x01/0x02/0x03 through kimix::string,\n"
        "//   never as text).\n"
        "// * several blocks in one expectation are joined with \\x03\n"
        "// * all bytes outside printable ASCII use 3-digit octal escapes, so the\n"
        "//   file is pure ASCII (no BOM/encoding surprises on MSVC)\n"
        "\n"
    )

    def emit_struct(name, fields, rows):
        out.append(f"struct {name} {{\n")
        for fname, ftype in fields:
            out.append(f"    {ftype} {fname};\n")
        out.append("};\n")
        out.append(f"static const {name} {name}_goldens[] = {{\n")
        for row in rows:
            parts = []
            for value, (_, ftype) in zip(row, fields):
                if ftype == "const char *":
                    parts.append(lit(value))
                else:
                    parts.append(str(value))
            out.append("    {" + ", ".join(parts) + "},\n")
        out.append("};\n\n")

    emit_struct(
        "wr_scan_g",
        [("name", "const char *"), ("content", "const char *"), ("expect", "const char *")],
        data["scan"],
    )
    emit_struct(
        "wr_dangling_g",
        [("name", "const char *"), ("content", "const char *"), ("expect", "const char *")],
        data["dangling"],
    )
    emit_struct(
        "wr_splice_g",
        [("name", "const char *"), ("recorded", "const char *"),
         ("current", "const char *"), ("block_index", "int"),
         ("replacement", "const char *"), ("ok", "int"), ("text", "const char *"),
         ("trimmed_leading", "int"), ("trimmed_trailing", "int"),
         ("error", "const char *")],
        data["splice"],
    )
    emit_struct(
        "wr_token_g",
        [("name", "const char *"), ("source", "const char *"), ("block_index", "int"),
         ("content", "const char *"), ("ok", "int"), ("text", "const char *"),
         ("error", "const char *")],
        data["token"],
    )
    emit_struct(
        "wr_render_g",
        [("name", "const char *"), ("source", "const char *"), ("block_index", "int"),
         ("scope", "const char *"), ("ok", "int"), ("start_line", "int"),
         ("lines", "const char *"), ("error", "const char *")],
        data["render"],
    )
    emit_struct(
        "wr_summary_g",
        [("name", "const char *"), ("source", "const char *"), ("display_path", "const char *"),
         ("truncated", "int"), ("expect", "const char *")],
        data["summary"],
    )
    emit_struct(
        "wr_region_equal_g",
        [("name", "const char *"), ("source_a", "const char *"), ("index_a", "int"),
         ("source_b", "const char *"), ("index_b", "int"), ("expect", "int")],
        data["region_equal"],
    )
    emit_struct(
        "wr_region_present_g",
        [("name", "const char *"), ("content", "const char *"),
         ("source", "const char *"), ("index", "int"), ("expect", "int")],
        data["region_present"],
    )
    emit_struct(
        "wr_uri_g",
        [("name", "const char *"), ("raw", "const char *"), ("kind", "int"),
         ("id", "const char *"), ("scope", "const char *"), ("prefix", "const char *"),
         ("error", "const char *")],
        data["uri"],
    )
    emit_struct(
        "wr_uri_dev_g",
        [("name", "const char *"), ("raw", "const char *"), ("py_repr", "const char *"),
         ("cpp_error", "const char *")],
        data["uri_dev"],
    )
    emit_struct(
        "wr_bulk_g",
        [("name", "const char *"), ("content", "const char *"), ("expect", "const char *")],
        data["bulk"],
    )
    emit_struct(
        "wr_bulk_dev_g",
        [("name", "const char *"), ("content", "const char *"),
         ("py_repr", "const char *")],
        data["bulk_dev"],
    )
    emit_struct(
        "wr_guard_g",
        [("name", "const char *"), ("display_path", "const char *"),
         ("old_text", "const char *"), ("new_content", "const char *"),
         ("append", "int"), ("file_existed", "int"), ("allow_conflicts", "int"),
         ("error", "const char *"), ("note", "const char *"), ("old_had_blocks", "int")],
        data["guard"],
    )
    emit_struct(
        "wr_parent_g",
        [("name", "const char *"), ("parent_exists", "int"), ("mkdir", "int"),
         ("display_path", "const char *"), ("parent_path", "const char *"),
         ("has_create_error", "int"), ("create_error", "const char *"),
         ("status", "const char *"), ("message", "const char *")],
        data["parent"],
    )
    emit_struct(
        "wr_size_g",
        [("name", "const char *"), ("append", "int"), ("old_text", "const char *"),
         ("content", "const char *"), ("new_text", "const char *"),
         ("has_value", "int"), ("size", "unsigned long long")],
        data["size"],
    )
    emit_struct(
        "wr_utf8_g",
        [("name", "const char *"), ("bytes", "const char *"), ("len", "unsigned long long"),
         ("has_error", "int"), ("reason", "const char *")],
        data["utf8"],
    )
    return "".join(out)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--reference", default=DEFAULT_REFERENCE)
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args(argv)

    ref = import_reference(Path(args.reference))
    data = make_cases(ref)
    text = render_inc(data)
    out_path = Path(args.out)
    if args.check:
        current = out_path.read_text(encoding="ascii") if out_path.exists() else ""
        if current != text:
            print(f"STALE: {out_path} differs from the reference output")
            return 1
        print(f"{out_path} is up to date")
        return 0
    out_path.write_text(text, encoding="ascii", newline="\n")
    counts = ", ".join(f"{k}={len(v)}" for k, v in data.items())
    print(f"wrote {out_path} ({counts})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
