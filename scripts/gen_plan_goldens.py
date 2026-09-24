#!/usr/bin/env python3
"""Regenerate the plan/note tool goldens (tests/unit/builtin_tools/plan_goldens.inc).

`src/builtin_tools/plan_tool.cpp` is the C++ port of kimi-agent's plan-file tools
(`C:/dev/kimi-agent/src/kimix/tools/note/__init__.py`: WritePlan / ReadPlan /
EditPlan).  The port is not exposed through `runtime_py`, so it is verified by a
golden-replay Boost.UT test (`tests/unit/builtin_tools/test_plan_tool.cpp`)
whose expectations are produced *here* by running the real Python objects -- not
a kimix_native mirror, not a hand-typed constant:

  * `kimix.tools.note` is imported straight from the kimi-agent checkout
    (defensive: a `kimix` package cached from kimi-cli's shim is purged).
  * every tool call goes through `CallableTool2.call(json)` -- the same entry
    point the agent's toolset uses, so pydantic validation, the alias/repair
    pass and the tool body all run before the result is recorded.
  * plan files are real files in a temporary directory; the directory is
    replaced by the `@PLAN@` token in every recorded string so the goldens do
    not depend on the machine that generated them (the test substitutes its own
    temporary directory back in).
  * ReadPlan's render bookkeeping (start_line / total_lines / max_lines_reached
    / max_bytes_reached / truncated line numbers) is not part of the reference's
    ToolReturnValue, so those expectations are *parsed out of the reference's
    own message* and the parse is validated by rebuilding that message.

Usage:
    python scripts/gen_plan_goldens.py            # write the .inc
    python scripts/gen_plan_goldens.py --check    # fail if the .inc is stale
    python scripts/gen_plan_goldens.py --schemas  # rewrite the LLM-facing tool
                                                  # descriptions/schemas in
                                                  # src/builtin_tools/plan_tool.cpp
    python scripts/gen_plan_goldens.py --all      # goldens + schemas
    python scripts/gen_plan_goldens.py --reference DIR

The generated file is pure ASCII (every byte outside the safe printable set is
emitted as a 3-digit octal escape -- including spaces, so no tool that folds
whitespace can corrupt the checked-in values) and every literal is chunked below
MSVC's 16 KiB string-literal limit.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import re
import shutil
import sys
import tempfile
from pathlib import Path

DEFAULT_REFERENCE = os.environ.get("KIMI_AGENT_ROOT", r"C:/dev/kimi-agent")
DEFAULT_OUT = Path("tests/unit/builtin_tools/plan_goldens.inc")
DEFAULT_CPP = Path("src/builtin_tools/plan_tool.cpp")

CPP_META_BEGIN = "// >>> BEGIN GENERATED:PLAN-TOOL-META >>>"
CPP_META_END = "// <<< END GENERATED:PLAN-TOOL-META <<<"

#: Placeholder for the generated temp directory holding the plan files.
PLAN_TOKEN = "@PLAN@"
#: Separator inside a `text` recipe spec (see make_text_spec).
SPEC_SEP = "\x1f"
SPEC_MARK = "\x02"

MAX_LINES = 1000
MAX_BYTES = 100 * 1024

#: The event loop every reference coroutine runs on (set in main()).
_LOOP: asyncio.AbstractEventLoop | None = None


def run(coro):
    assert _LOOP is not None, "event loop not initialised"
    return _LOOP.run_until_complete(coro)


# ---------------------------------------------------------------------------
# C++ literal emission
# ---------------------------------------------------------------------------

def _esc(byte: int) -> str:
    """One C++ escape sequence for *byte*.

    Spaces are escaped too: the goldens must survive any pipeline that folds or
    strips whitespace between this generator and the checked-in file.
    """
    if byte == 0x22:
        return '\\"'
    if byte == 0x5C:
        return "\\\\"
    if 0x21 <= byte <= 0x7E:
        return chr(byte)
    return "\\%03o" % byte


def lit(data, indent: str = " ") -> str:
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


def lit_readable(data) -> str:
    """A C++ string literal with literal spaces (for the .cpp, where an escaped
    space only hurts readability)."""
    if isinstance(data, str):
        data = data.encode("utf-8")
    out = '"'
    for byte in data:
        if byte == 0x22:
            out += '\\"'
        elif byte == 0x5C:
            out += "\\\\"
        elif 0x20 <= byte <= 0x7E:
            out += chr(byte)
        else:
            out += "\\%03o" % byte
    out += '"'
    return out


def canon_json(obj) -> str:
    """Canonical compact JSON (sorted keys, ASCII) used both in the .inc and in
    the generated KIMIX_REGISTER_TOOL literals."""
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def make_text_spec(name: str, data: bytes):
    """Return (spec, expand) for *data*.

    Large corpora would otherwise be stored twice (input + rendered output), so
    a repeated unit is described by a tiny recipe that the C++ test expands and
    length-checks: SPEC_MARK + "R" + SPEC_SEP + count + SPEC_SEP + unit.
    """
    if len(data) >= 512:
        for unit_len in range(1, 4097):
            if len(data) % unit_len:
                continue
            unit = data[:unit_len]
            count = len(data) // unit_len
            if count < 4 or unit * count != data:
                continue
            if SPEC_SEP.encode() in unit or SPEC_MARK.encode() in unit:
                continue
            return (SPEC_MARK.encode() + b"R" + SPEC_SEP.encode()
                    + str(count).encode() + SPEC_SEP.encode() + unit), True
    return data, False


def line_count(text: str) -> int:
    """The number of "\\n"-terminated (plus one trailing partial) lines, i.e. the
    count the C++ test computes. Deliberately NOT str.splitlines(), which also
    breaks on \\x0b / \\x0c / U+0085 / U+2028 - those bytes are ordinary line
    content for the renderer."""
    if not text:
        return 0
    return text.count("\n") + (0 if text.endswith("\n") else 1)


def lf_to_crlf(text: str) -> bytes:
    """The CPython Windows text-mode write form of *text*."""
    return text.replace("\n", "\r\n").encode("utf-8")


# ---------------------------------------------------------------------------
# reference import (defensive: two sys.path traps documented in
# python/tests/_parity_ref.py -- kimi-cli/src holds a shadowing `kimix` shim)
# ---------------------------------------------------------------------------

def import_reference(root: Path):
    kimix_src = root / "src"
    kimi_cli_src = root / "kimi-cli" / "src"
    if not (kimix_src / "kimix" / "tools" / "note").is_dir():
        raise SystemExit("not a kimi-agent checkout: %s" % root)

    def _under(path, parent: Path) -> bool:
        if not path:
            return False
        try:
            return parent.resolve() in Path(path).resolve().parents
        except (OSError, ValueError):
            return False

    def _purge_shadow():
        for mod_name in [n for n in sys.modules
                         if n == "kimix" or n.startswith("kimix.")]:
            mod = sys.modules[mod_name]
            if not _under(getattr(mod, "__file__", None), kimix_src):
                del sys.modules[mod_name]

    for p in (kimi_cli_src, kimix_src):
        s = str(p)
        while s in sys.path:
            sys.path.remove(s)
    sys.path.insert(0, str(kimi_cli_src))
    sys.path.insert(0, str(kimix_src))

    _purge_shadow()
    import kimix.tools.note as note  # noqa: PLC0415

    if not _under(note.__file__, kimix_src):
        raise SystemExit("kimix.tools.note resolved outside %s: %s"
                         % (kimix_src, note.__file__))
    return note


# ---------------------------------------------------------------------------
# message parsing (the reference's own text is the ground truth for the
# C++-only bookkeeping fields of the ReadPlan envelope)
# ---------------------------------------------------------------------------

_RE_RENDERED = re.compile(r"^(\d+) lines read from plan starting from line (\d+)\.$")
_RE_TOTAL = re.compile(r"^Total lines in file: (\d+)\.$")
_RE_MAX_LINES = re.compile(r"^Max (\d+) lines reached\.$")
_RE_MAX_BYTES = re.compile(r"^Max (\d+) bytes reached\.$")
_RE_TRUNCATED = re.compile(r"^Lines \[([\d, ]*)\] were truncated\.$")


def parse_render_message(message: str) -> dict:
    """Split a ReadPlan message into the render bookkeeping it reports."""
    out = {
        "rendered": -1, "start_line": -1, "total_lines": -1,
        "max_lines_reached": 0, "max_bytes_reached": 0, "truncated": "",
    }
    for raw in message.split(". "):
        seg = raw.strip()
        if not seg.endswith("."):
            seg += "."
        m = _RE_RENDERED.match(seg)
        if m:
            out["rendered"] = int(m.group(1))
            out["start_line"] = int(m.group(2))
            continue
        if seg == "No lines read from plan.":
            out["rendered"] = 0
            continue
        m = _RE_TOTAL.match(seg)
        if m:
            out["total_lines"] = int(m.group(1))
            continue
        if _RE_MAX_LINES.match(seg):
            out["max_lines_reached"] = 1
            continue
        if _RE_MAX_BYTES.match(seg):
            out["max_bytes_reached"] = 1
            continue
        m = _RE_TRUNCATED.match(seg)
        if m:
            out["truncated"] = m.group(1).replace(",", "")
            continue
        raise AssertionError("unparsed ReadPlan message segment: %r" % seg)
    # The parse must reconstruct the reference message exactly.
    parts = []
    if out["rendered"] == 0:
        parts.append("No lines read from plan.")
    else:
        parts.append("%d lines read from plan starting from line %d."
                     % (out["rendered"], out["start_line"]))
    if out["total_lines"] >= 0:
        parts.append("Total lines in file: %d." % out["total_lines"])
    if out["max_lines_reached"]:
        parts.append("Max %d lines reached." % MAX_LINES)
    elif out["max_bytes_reached"]:
        parts.append("Max %d bytes reached." % MAX_BYTES)
    if out["truncated"]:
        parts.append("Lines [%s] were truncated."
                     % ", ".join(out["truncated"].split()))
    assert " ".join(parts) == message, (message, " ".join(parts))
    return out


def cpp_status(tool: str, ret) -> str:
    """Map a reference ToolReturnValue onto the C++ envelope status.

    The mapping is the port's own contract (`plan_tool.h`); the *is_error* flag
    is the Python-side truth and is asserted separately.
    """
    if not ret.is_error:
        return "ok"
    msg = ret.message
    if msg.startswith("No replacements were made."):
        return "no_change"
    if msg.startswith("Plan file `") and msg.endswith("` does not exist."):
        return "not_found"
    if msg.endswith(" is not a file."):
        return "invalid_input"
    if msg.endswith(" tool invalid: no plan_writing_path set."):
        return "invalid_input"
    return "external_library"


# ---------------------------------------------------------------------------
# the reference driver
# ---------------------------------------------------------------------------

class _FakeSession:
    """Minimal stand-in for kimi_cli Session: the plan tools only touch
    `custom_data`."""

    def __init__(self):
        self.custom_data: dict = {}


class Driver:
    def __init__(self, note, root: Path):
        self.note = note
        self.root = root
        note._set_enable_plan(True)

    # -- helpers ----------------------------------------------------------
    def case_dir(self, name: str) -> Path:
        """One directory per case, named after the case (names are unique), so
        the recorded paths - and therefore the generated file - are stable."""
        d = self.root / name
        d.mkdir(parents=True, exist_ok=True)
        return d

    def tokenize(self, text: str) -> str:
        """Replace the temp root with the @PLAN@ token (path separator kept)."""
        return text.replace(str(self.root), PLAN_TOKEN)

    def setup(self, d: Path, rel_path: str, kind: str, initial: bytes) -> Path:
        target = d / rel_path
        if kind == "dir":
            target.mkdir(parents=True, exist_ok=True)
        elif kind == "parent_is_file":
            target.parent.parent.mkdir(parents=True, exist_ok=True)
            target.parent.write_bytes(b"i am a file\n")
        elif kind == "file":
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(initial)
        return target

    def tool_class(self, name: str):
        note = self.note
        return {"WritePlan": note.WritePlan, "ReadPlan": note.ReadPlan,
                "EditPlan": note.EditPlan}[name]

    async def run_tool_case(self, name: str, tool_name: str, params_json: str,
                            rel_path: str = "plan.md", kind: str = "file",
                            initial: bytes = b"", enabled: bool = True,
                            set_plan_path: bool = True, logical_write: str | None = None,
                            append: bool = False, wrote: bool | None = None):
        d = self.case_dir(name)
        note = self.note
        cls = self.tool_class(tool_name)
        note._set_enable_plan(enabled)
        target = self.setup(d, rel_path, kind, initial)
        session = _FakeSession()
        if set_plan_path:
            session.custom_data["plan_writing_path"] = target
        if not enabled:
            raised = None
            try:
                cls(session=session)
            except Exception as exc:  # noqa: BLE001
                raised = exc
            note._set_enable_plan(True)
            return {"skipped": raised}
        tool = cls(session=session)
        ret = await tool.call(json.loads(params_json))
        note._set_enable_plan(True)
        rec = {
            "name": name,
            "tool": tool_name,
            "params_json": params_json,
            # relative to the temp root, so the test can rebuild the same path
            "rel_path": ("%s/%s" % (d.name, rel_path)) if set_plan_path else "",
            "kind": kind,
            "initial": initial,
            "is_error": 1 if ret.is_error else 0,
            "status": cpp_status(tool_name, ret),
            "message": self.tokenize(ret.message),
            "output": self.tokenize(ret.output if isinstance(ret.output, str) else ""),
            "brief": self.tokenize(ret.brief),
        }
        if tool_name == "ReadPlan" and not ret.is_error:
            rec.update(parse_render_message(ret.message))
        raw = target.read_bytes() if target.is_file() else None
        rec["exists_after"] = 1 if raw is not None else 0
        rec["python_bytes"] = raw if raw is not None else b""
        rec["append"] = 1 if append else 0
        if raw is None:
            rec["wrote"] = 0
            rec["write_logical"] = ""
            return rec
        # The text the reference handed to the file write. WritePlan cases pass
        # it explicitly (its content may contain bare CR, whose round trip
        # through CPython's universal newlines is lossy); edit cases recover it
        # from the on-disk bytes (a text-mode read is the exact inverse of a
        # text-mode write for content that came from a universal-newline read).
        if wrote is None:
            wrote = (tool_name == "WritePlan"
                     or (tool_name == "EditPlan" and not ret.is_error))
        rec["wrote"] = 1 if wrote else 0
        if not rec["wrote"]:
            rec["write_logical"] = ""
            assert raw == initial, (name, raw, initial)
            return rec
        if logical_write is None:
            assert tool_name == "EditPlan", (name, tool_name)
            logical_write = raw.decode("utf-8").replace("\r\n", "\n")
        base = initial if append else b""
        rec["write_logical"] = logical_write
        # Cross-check the claimed logical write against what Python left on
        # disk (each LF became CRLF on Windows, nothing else changed).
        if os.name == "nt":
            expect = base + logical_write.replace("\n", "\r\n").encode("utf-8")
        else:
            expect = base + logical_write.encode("utf-8")
        assert raw == expect, (name, raw, expect)
        return rec

    async def render_case(self, name: str, data: bytes, line_offset: int,
                          n_lines: int):
        """Call ReadPlan._read_forward / _read_tail directly (no char window)."""
        d = self.case_dir(name)
        target = d / "plan.md"
        target.write_bytes(data)
        tool = self.note.ReadPlan(session=_FakeSession())
        params = self.note.ReadPlanParams(line_offset=line_offset, n_lines=n_lines)
        if line_offset < 0:
            ret = await tool._read_tail(target, params)
        else:
            ret = await tool._read_forward(target, params)
        return {
            "name": name,
            "text": data,
            "line_offset": line_offset,
            "n_lines": n_lines,
            "output": ret.output,
            "message": ret.message,
        }

    async def window_case(self, name: str, data: bytes, line_offset: int,
                          n_lines: int, char_offset: int, max_char: int):
        rec = await self.run_tool_case(
            name, "ReadPlan",
            canon_json({"line_offset": line_offset, "n_lines": n_lines,
                        "char_offset": char_offset, "max_char": max_char}),
            initial=data)
        rec["text"] = data
        rec["char_offset"] = char_offset
        rec["max_char"] = max_char
        return rec

    async def param_case(self, name: str, tool_name: str, params_json: str):
        note = self.note
        note._set_enable_plan(True)
        from kosong.tooling.error import ToolValidateError  # noqa: PLC0415
        tool = self.tool_class(tool_name)(session=_FakeSession())
        ret = await tool.call(json.loads(params_json))
        return {
            "name": name,
            "tool": tool_name,
            "params_json": params_json,
            "accepted": 0 if isinstance(ret, ToolValidateError) else 1,
        }


# ---------------------------------------------------------------------------
# corpora
# ---------------------------------------------------------------------------

LONG = b"p" * 2000 + b"\n"


def read_texts():
    return [
        ("simple", b"a\nb\nc\n"),
        ("no_trailing_newline", b"a\nb\nc"),
        ("empty", b""),
        ("one_line", b"only\n"),
        ("crlf", b"a\r\nb\r\nc\r\n"),
        ("lone_cr", b"a\rb\rc\r"),
        ("mixed_endings", b"a\r\nb\rc\nd"),
        ("blank_lines", b"a\n\n\nb\n"),
        ("unicode", "h\u00e9llo\n\u4e2d\u6587\nend\n".encode("utf-8")),
        ("tabs_and_spaces", b"  indented\n\ttabbed\n"),
        ("vertical_tab", b"a\x0bb\x0cc\n"),
        ("nul_byte", b"a\x00b\n"),
        ("invalid_utf8", b"ok\nbad \xff\xfe end\n"),
        ("long_1999", b"x" * 1999 + b"\nshort\n"),
        ("long_2000", b"y" * 2000 + b"\n"),
        ("long_2001", b"z" * 2001 + b"\nshort\n"),
        ("crlf_long", b"w" * 2000 + b"\r\n"),
        ("trailing_ws", b"a  \nb\t\n"),
        ("bom", b"\xef\xbb\xbfplan\n"),
        ("max_lines_plus_one", b"x\n" * 1001),
        ("numbered_1200", b"".join(b"l%d\n" % i for i in range(1200))),
        ("bytes_budget_forward", LONG * 60),
        ("bytes_budget_tail", (b"q" * 1900 + b"\n") * 60),
    ]


READ_PARAMS = {
    "simple": [(1, 1000), (2, 1000), (9, 1000), (-1, 1000), (-2, 1000), (-3, 2)],
    "no_trailing_newline": [(1, 1000), (-1, 1000)],
    "empty": [(1, 1000), (-5, 1000)],
    "one_line": [(1, 1000), (-1, 1000)],
    "crlf": [(1, 1000), (-2, 1000)],
    "lone_cr": [(1, 1000), (-2, 1000)],
    "mixed_endings": [(1, 1000), (-3, 1000)],
    "blank_lines": [(1, 1000), (-2, 1000)],
    "unicode": [(1, 1000), (-1, 1000)],
    "tabs_and_spaces": [(1, 1000)],
    "vertical_tab": [(1, 1000)],
    "nul_byte": [(1, 1000)],
    "invalid_utf8": [(1, 1000)],
    "long_1999": [(1, 1000)],
    "long_2000": [(1, 1000)],
    "long_2001": [(1, 1000)],
    "crlf_long": [(1, 1000)],
    "trailing_ws": [(1, 1000)],
    "bom": [(1, 1000)],
    "max_lines_plus_one": [(1, 1000), (1, 5), (-1000, 1000)],
    "numbered_1200": [(-1000, 1000), (-999, 1000), (-1, 1000)],
    "bytes_budget_forward": [(1, 1000), (1, 2)],
    "bytes_budget_tail": [(-60, 1000), (-60, 3)],
}

CHAR_WINDOWS = [
    ("win_head", 0, 8),
    ("win_middle", 5, 20),
    ("win_inverted", 20, 5),
    ("win_zero_len", 0, 0),
    ("win_past_end", 100000, 100008),
    ("win_default", 0, 65536),
]

EDIT_CASES = [
    ("exact", b"hello world\n", {"edit": {"old": "hello", "new": "hi"}}),
    ("exact_multi_line", b"line1\nline2\nline3\n",
     {"edit": [{"old": "line1", "new": "first"}, {"old": "line3", "new": "third"}]}),
    ("aliased_edits", b"hello world", {"edits": {"old_string": "hello", "new_string": "hi"}}),
    ("replace_all", b"foo bar foo", {"edit": {"old": "foo", "new": "baz", "replace_all": True}}),
    ("replace_all_no_hit", b"foo bar foo",
     {"edit": {"old": "nope", "new": "baz", "replace_all": True}}),
    ("no_match", b"hello world", {"edit": {"old": "zzzz_nonexistent", "new": "abc"}}),
    ("no_match_close", b"hello world\n", {"edit": {"old": "hello worl", "new": "x"}}),
    ("strip_match", b"  hello world  \n", {"edit": {"old": "  hello  ", "new": "hi"}}),
    ("strip_match_keeps_newline", b"the hello world\n", {"edit": {"old": "hello", "new": "hi"}}),
    ("fuzzy_ratio_90", b"hellp world\n", {"edit": {"old": "hello world", "new": "HI"}}),
    ("fuzzy_multi_line", b"alpha\nbeta gamma\ndelta\n",
     {"edit": {"old": "beta gama", "new": "BETA"}}),
    ("noop_old_equals_new", b"hello\n", {"edit": {"old": "x", "new": "x"}}),
    ("noop_empty_old", b"hello\n", {"edit": {"old": "", "new": "y"}}),
    ("empty_edit_list", b"hello\n", {"edits": []}),
    ("mixed_noop_and_hit", b"alpha\nbeta\ngamma\n",
     {"edit": [{"old": "x", "new": "x"}, {"old": "alpha", "new": "A"},
               {"old": "zzz", "new": "Z"}]}),
    ("crlf_file", b"hello\r\nworld\r\n", {"edit": {"old": "hello", "new": "hi"}}),
    ("crlf_old_string", b"a\r\nb\r\n", {"edit": {"old": "a\r\nb", "new": "z"}}),
    ("unicode", "caf\u00e9\n".encode("utf-8"), {"edit": {"old": "caf\u00e9", "new": "tea"}}),
    ("tabs", b"\talpha\n\tbeta\n", {"edit": {"old": "alpha", "new": "A"}}),
    ("multiline_exact", b"a\nb\nc\n", {"edit": {"old": "a\nb", "new": "AB"}}),
    ("json_string_edit", b"hello world", {"edit": '{"old": "hello", "new": "hi"}'}),
    ("json_string_edits", b"hello world", {"edits": '[{"old": "hello", "new": "hi"}]'}),
    ("suggestion_after_change", b"alpha beta\n",
     {"edit": [{"old": "alpha", "new": "A"}, {"old": "btea", "new": "B"}]}),
]

WRITE_CASES = [
    ("overwrite", "file", b"old content\n", {"content": "new\n"}),
    ("overwrite_text_alias", "file", b"", {"text": "aliased\n"}),
    ("aliases_both", "file", b"", {"content": "canonical", "text": "alias"}),
    ("append", "file", b"line1\n", {"content": "line2\n", "mode": "append"}),
    ("append_creates", "absent", b"", {"content": "created\n", "mode": "append"}),
    ("nested_dirs", "file", b"", {"content": "deep\n"}, "nested/dirs/plan.md"),
    ("empty_content", "file", b"stale\n", {"content": ""}),
    ("unicode", "file", b"", {"content": "h\u00e9llo \u4e2d\u6587\n"}),
    ("crlf_content", "file", b"", {"content": "l1\r\nl2\n"}),
    ("overwrite_replaces", "file", b"a\nb\n", {"content": "z\n"}),
]

PARAM_CASES = [
    ("write_defaults", "WritePlan", {"content": "x"}),
    ("write_text_alias", "WritePlan", {"text": "x"}),
    ("write_both_aliases", "WritePlan", {"content": "c", "text": "t"}),
    ("write_mode_append", "WritePlan", {"content": "x", "mode": "append"}),
    ("write_mode_null", "WritePlan", {"content": "x", "mode": None}),
    ("write_mode_bad", "WritePlan", {"content": "x", "mode": "sideways"}),
    ("write_mode_int", "WritePlan", {"content": "x", "mode": 1}),
    ("write_content_null", "WritePlan", {"content": None}),
    ("write_content_int", "WritePlan", {"content": 123}),
    ("write_missing", "WritePlan", {}),
    ("write_extra_key", "WritePlan", {"text": "x", "zzz": 1}),
    ("write_fuzzy_key", "WritePlan", {"body": "x"}),
    ("read_defaults", "ReadPlan", {}),
    ("read_line_offset_null", "ReadPlan", {"line_offset": None}),
    ("read_line_offset_str", "ReadPlan", {"line_offset": "3"}),
    ("read_line_offset_float", "ReadPlan", {"line_offset": 2.0}),
    ("read_line_offset_frac", "ReadPlan", {"line_offset": 1.5}),
    ("read_line_offset_zero", "ReadPlan", {"line_offset": 0}),
    ("read_line_offset_too_neg", "ReadPlan", {"line_offset": -1001}),
    ("read_line_offset_min", "ReadPlan", {"line_offset": -1000}),
    ("read_n_lines_zero", "ReadPlan", {"n_lines": 0}),
    ("read_n_lines_big", "ReadPlan", {"n_lines": 100000}),
    ("read_max_char_neg", "ReadPlan", {"max_char": -1}),
    ("read_char_offset_neg", "ReadPlan", {"char_offset": -1}),
    ("read_fuzzy_key", "ReadPlan", {"start_line": 2, "num_lines": 3}),
    ("edit_single", "EditPlan", {"edit": {"old": "a", "new": "b"}}),
    ("edit_aliases", "EditPlan", {"edits": {"old_string": "a", "new_string": "b"}}),
    ("edit_both_keys", "EditPlan",
     {"edit": {"old": "a", "new": "b"}, "edits": [{"old": "c", "new": "d"}]}),
    ("edit_inner_both", "EditPlan",
     {"edit": {"old": "a", "old_string": "b", "new": "c", "new_string": "d"}}),
    ("edit_inner_replace_all_str", "EditPlan",
     {"edit": {"old": "a", "new": "b", "replace_all": "yes"}}),
    ("edit_inner_null_old", "EditPlan", {"edit": {"old": None, "new": "b"}}),
    ("edit_inner_missing_new", "EditPlan", {"edit": {"old": "a"}}),
    ("edit_missing", "EditPlan", {}),
    ("edit_null", "EditPlan", {"edit": None}),
    ("edit_int", "EditPlan", {"edit": 5}),
    ("edit_list_empty", "EditPlan", {"edits": []}),
    ("edit_list_item_not_object", "EditPlan", {"edits": ["nope"]}),
    ("edit_json_string", "EditPlan", {"edit": '{"old": "a", "new": "b"}'}),
    ("edit_json_string_list", "EditPlan", {"edits": '[{"old": "a", "new": "b"}]'}),
]

OS_ERROR_CASES = [
    ("write_path_is_dir", "WritePlan", {"content": "x"}, "plan_dir", "dir"),
    ("edit_path_is_dir", "EditPlan", {"edit": {"old": "a", "new": "b"}}, "plan_dir", "dir"),
    ("write_parent_is_file", "WritePlan", {"content": "x"}, "afile/plan.md",
     "parent_is_file"),
]

KINDS = {"absent": 0, "file": 1, "dir": 2, "parent_is_file": 3}

#: Parameter cases where the port deliberately does NOT follow the reference.
#: kimi-agent runs kosong's repair pass (`_repair_dict_for_model`) *before* the
#: tool body: it coerces scalars (int->str, str->int, "yes"->bool) and clamps
#: Field constraints (ge/le) so a value the pydantic model rejects outright is
#: silently fixed up. `src/builtin_tools/*` ports the *pydantic* contract only
#: (see e.g. `read::validate_int_option`, which returns the byte-exact pydantic
#: ValueError message) and has no repair layer, so these rows are strict
#: `invalid_input` in C++ while Python accepts them. Each reason is asserted by
#: the test: the row must be rejected by the port.
PARAM_DEVIATIONS = {
    "write_content_int": "pydantic lax str coercion (123 -> \"123\")",
    "read_line_offset_str": "pydantic lax int coercion (\"3\" -> 3)",
    "read_n_lines_zero": "repair clamps n_lines to the ge=1 bound",
    "read_max_char_neg": "repair clamps max_char to the ge=0 bound",
    "read_char_offset_neg": "repair clamps char_offset to the ge=0 bound",
    "edit_inner_replace_all_str": "pydantic lax bool coercion (\"yes\" -> true)",
}


# ---------------------------------------------------------------------------
# generation
# ---------------------------------------------------------------------------

def generate(note, root: Path) -> str:
    driver = Driver(note, root)
    out: list[str] = []

    def w(s: str) -> None:
        out.append(s)

    w("// GENERATED by scripts/gen_plan_goldens.py - DO NOT EDIT BY HAND.\n")
    w("// Every expectation comes from running the real Python implementation\n")
    w("// (kimi-agent src/kimix/tools/note/__init__.py: WritePlan / ReadPlan /\n")
    w("// EditPlan, driven through CallableTool2.call exactly like the agent\n")
    w("// toolset) over real files in a temporary directory. Paths are recorded\n")
    w("// with the @PLAN@ token; the test substitutes its own temp directory.\n")
    w("// Regenerate with: python scripts/gen_plan_goldens.py\n")
    w("//                 python scripts/gen_plan_goldens.py --check\n")
    w("// Conventions:\n")
    w("// * every byte outside [!-~] minus '\\\\' and '\"' is a 3-digit octal\n")
    w("//   escape, so the file is pure ASCII and whitespace-safe\n")
    w("// * a `text` field starting with \\002 is a recipe:"
      " \\002R<us>COUNT<us>UNIT\n")
    w("//   expands to UNIT repeated COUNT times (text_len is the expansion size)\n")
    w("// * kind: 0 absent, 1 regular file, 2 directory, 3 parent path is a file\n")
    w("// * -1 in a long long field means 'not reported by Python / not asserted'\n")
    w("\n")

    # ---- tool meta -----------------------------------------------------
    tools = [("WritePlan", note.WritePlan), ("ReadPlan", note.ReadPlan),
             ("EditPlan", note.EditPlan)]
    w("// LLM-facing tool metadata: name + description + pydantic parameter\n")
    w("// schema (by_alias, $defs dereferenced -- exactly what kosong hands the\n")
    w("// model). The C++ KIMIX_REGISTER_TOOL literal must match this.\n")
    w("struct plan_meta_golden {\n")
    w("    const char *tool;\n    const char *description;\n"
      "    const char *params_json;\n};\n\n")
    w("const plan_meta_golden kPlanMeta[] = {\n")
    for name, cls in tools:
        obj = _instance(cls)
        assert obj.name == name, (obj.name, name)
        w("    {%s, %s,\n     %s},\n" % (lit(name), lit(obj.description),
                                        lit(canon_json(obj.base.parameters))))
    w("};\n\n")

    # ---- parameter validation matrix -----------------------------------
    w("// Parameter models: `accepted` == pydantic (plus kosong's repair pass)\n")
    w("// let the call through to the tool body. A rejected row must fail the\n")
    w("// C++ parse_*_params with invalid_input; `deviation` is non-empty for the\n")
    w("// rows where the port is deliberately stricter than the reference (no\n")
    w("// repair pass) and must therefore reject the row.\n")
    w("struct plan_param_golden {\n")
    w("    const char *name;\n    const char *tool;\n    const char *params_json;\n"
      "    int accepted;\n    const char *deviation;\n};\n\n")
    w("const plan_param_golden kPlanParamGoldens[] = {\n")
    for name, tool, params in PARAM_CASES:
        rec = run(driver.param_case(name, tool, canon_json(params)))
        dev = PARAM_DEVIATIONS.get(name, "")
        assert not dev or rec["accepted"] == 1, (name, rec)
        w("    {%s, %s, %s,\n     %d, %s},\n"
          % (lit(rec["name"]), lit(rec["tool"]), lit(rec["params_json"]),
             rec["accepted"], lit(dev)))
    w("};\n\n")

    # ---- render kernels ------------------------------------------------
    texts = dict(read_texts())
    forward, tail = [], []
    for key, _ in read_texts():
        data = texts[key]
        for line_offset, n_lines in READ_PARAMS[key]:
            name = "%s_lo%d_n%d" % (key, line_offset, n_lines)
            rec = run(driver.render_case(name, data, line_offset, n_lines))
            rec["key"] = key
            rec["text_len"] = len(data)
            (forward if line_offset > 0 else tail).append(rec)

    def emit_renders(name: str, rows, with_struct: bool) -> None:
        w("// ReadPlan._read_forward / _read_tail, called directly on a real file\n")
        w("// (no char window applied).\n")
        if with_struct:
            w("struct plan_render_golden {\n")
            w("    const char *name;\n    const char *text;\n    long long text_len;\n"
              "    long long line_offset;\n    long long n_lines;\n")
            w("    const char *output;\n    long long output_len;\n"
              "    long long output_lines;\n    int assert_output;\n")
            w("    const char *message;\n};\n\n")
        w("const plan_render_golden %s[] = {\n" % name)
        for rec in rows:
            spec, expand = make_text_spec(rec["key"], rec["text"])
            omit = len(rec["output"]) > 20000
            w("    {%s, %s, %d, %d, %d,\n     %s,\n     %d, %d, %d,\n     %s},\n"
              % (lit(rec["name"]), lit(spec), rec["text_len"],
                 rec["line_offset"], rec["n_lines"],
                 lit("" if omit else rec["output"]),
                 len(rec["output"].encode("utf-8")),
                 line_count(rec["output"]),
                 0 if omit else 1, lit(rec["message"])))
        w("};\n\n")

    emit_renders("kForwardGoldens", forward, with_struct=True)
    emit_renders("kTailGoldens", tail, with_struct=False)

    # ---- character window ----------------------------------------------
    window_rows = []
    for key in ("simple", "unicode", "crlf"):
        data = texts[key]
        for wname, char_offset, max_char in CHAR_WINDOWS:
            window_rows.append(run(driver.window_case(
                "%s_%s" % (key, wname), data, 1, 1000, char_offset, max_char)))
    w("// ReadPlan.__call__ `output[char_offset:max_char]` (max_char is the slice\n")
    w("// END, not a length) plus the un-windowed message.\n")
    w("struct plan_window_golden {\n")
    w("    const char *name;\n    const char *text;\n"
      "    long long char_offset;\n    long long max_char;\n")
    w("    const char *output;\n    const char *message;\n};\n\n")
    w("const plan_window_golden kPlanWindowGoldens[] = {\n")
    for rec in window_rows:
        w("    {%s, %s, %d, %d,\n     %s,\n     %s},\n"
          % (lit(rec["name"]), lit(rec["text"]), rec["char_offset"],
             rec["max_char"], lit(rec["output"]), lit(rec["message"])))
    w("};\n\n")

    # ---- tool replay ---------------------------------------------------
    tool_rows = []
    # Keys whose *rendered output* is large (byte/line budgets) are covered by
    # the render goldens above; replaying them through the tool would store the
    # same corpus twice for no extra coverage.
    big = {"numbered_1200", "bytes_budget_forward", "bytes_budget_tail"}
    for key, _ in read_texts():
        data = texts[key]
        if key in big:
            continue
        for line_offset, n_lines in READ_PARAMS[key][:2]:
            tool_rows.append(run(driver.run_tool_case(
                "read_%s_lo%d_n%d" % (key, line_offset, n_lines), "ReadPlan",
                canon_json({"line_offset": line_offset, "n_lines": n_lines}),
                initial=data)))
        if len(data) < 64:
            tool_rows.append(run(driver.run_tool_case(
                "read_%s_window" % key, "ReadPlan",
                canon_json({"char_offset": 3, "max_char": 12}), initial=data)))
    for name, initial, params in EDIT_CASES:
        tool_rows.append(run(driver.run_tool_case(
            "edit_" + name, "EditPlan", canon_json(params), initial=initial)))
    for case in WRITE_CASES:
        name, kind, raw_init, params = case[0], case[1], case[2], case[3]
        rel = case[4] if len(case) > 4 else "plan.md"
        # The logical text a successful WritePlan hands to the file write is
        # exactly the resolved `content` parameter (written verbatim). pydantic
        # prefers the declared alias (`text`) when both spellings are present.
        logical = params["text"] if "text" in params else params.get("content", "")
        tool_rows.append(run(driver.run_tool_case(
            "write_" + name, "WritePlan", canon_json(params), rel_path=rel,
            kind=kind, initial=raw_init, logical_write=logical,
            append=params.get("mode") == "append")))
    tool_rows.append(run(driver.run_tool_case(
        "read_missing", "ReadPlan", "{}", rel_path="gone.md", kind="absent")))
    tool_rows.append(run(driver.run_tool_case(
        "edit_missing", "EditPlan", canon_json({"edit": {"old": "a", "new": "b"}}),
        rel_path="gone.md", kind="absent")))
    tool_rows.append(run(driver.run_tool_case(
        "read_path_is_dir", "ReadPlan", "{}", rel_path="plan_dir", kind="dir")))
    no_path_row = run(driver.run_tool_case(
        "write_no_plan_path", "WritePlan", canon_json({"content": "x"}),
        rel_path="plan.md", set_plan_path=False, wrote=False))
    no_path_row["rel_path"] = ""  # the session has no plan_writing_path at all
    tool_rows.append(no_path_row)

    w("// End-to-end replay: the arguments go through CallableTool2.call, the\n")
    w("// tool works on a real file, and the recorded expectations are the\n")
    w("// reference's own status/output/message/brief plus the on-disk result.\n")
    w("// The file after a successful call must hold exactly\n")
    w("//     (append ? initial : \"\") + write_logical      (UTF-8, LF verbatim)\n")
    w("// and `initial` itself when `wrote` is 0. `python_bytes` is what CPython\n")
    w("// actually left on disk: a text-mode write turns every LF into CRLF on\n")
    w("// Windows -- the one documented platform deviation, asserted to be the\n")
    w("// only difference. `initial`/`write_logical`/`python_bytes`/`output` are\n")
    w("// arbitrary bytes, so their lengths are recorded too (an embedded NUL\n")
    w("// would otherwise truncate a const char* comparison).\n")
    w("struct plan_tool_golden {\n")
    w("    const char *name;\n    const char *tool;\n    const char *params_json;\n")
    w("    const char *rel_path; // \"\" == no plan_writing_path configured\n")
    w("    int kind; // see KINDS\n")
    w("    const char *initial;\n    long long initial_len;\n")
    w("    int exists_after;\n    int wrote;\n    int append;\n"
      "    const char *write_logical;\n    long long write_logical_len;\n"
      "    const char *python_bytes;\n    long long python_bytes_len;\n")
    w("    int is_error;\n    const char *status;\n")
    w("    const char *message;\n    const char *output;\n    long long output_len;\n")
    w("    const char *brief;\n")
    w("    long long start_line;\n    long long total_lines;\n"
      "    int max_lines_reached;\n    int max_bytes_reached;\n")
    w("    const char *truncated;\n};\n\n")
    w("const plan_tool_golden kPlanToolGoldens[] = {\n")
    for rec in tool_rows:
        w("    {%s, %s, %s,\n     %s, %d, %s, %d,\n     %d, %d, %d, %s, %d, %s, %d,\n"
          "     %d, %s,\n     %s,\n     %s, %d,\n     %s,\n"
          "     %d, %d, %d, %d,\n     %s},\n"
          % (lit(rec["name"]), lit(rec["tool"]), lit(rec["params_json"]),
             lit(rec["rel_path"]), KINDS[rec["kind"]], lit(rec["initial"]),
             len(rec["initial"]),
             rec["exists_after"], rec["wrote"], rec["append"],
             lit(rec["write_logical"]), len(rec["write_logical"].encode("utf-8")),
             lit(rec["python_bytes"]), len(rec["python_bytes"]),
             rec["is_error"], lit(rec["status"]),
             lit(rec["message"]), lit(rec["output"]),
             len(rec["output"].encode("utf-8")),
             lit(rec["brief"]),
             rec.get("start_line", -1), rec.get("total_lines", -1),
             rec.get("max_lines_reached", -1), rec.get("max_bytes_reached", -1),
             lit(rec.get("truncated", ""))))
    w("};\n\n")

    # ---- os-level failures ---------------------------------------------
    w("// OS-level failures. CPython's message is the raw OSError text\n")
    w("// (\"[Errno 13] Permission denied: '<path>'\" on Windows), which the port\n")
    w("// cannot reproduce byte-exactly; the golden pins the Python-independent\n")
    w("// parts plus the prefix the reference's own wrapper produces (\"\" for\n")
    w("// WritePlan, whose `except` block reports str(exc) verbatim).\n")
    w("struct plan_os_error_golden {\n")
    w("    const char *name;\n    const char *tool;\n    const char *params_json;\n")
    w("    const char *rel_path;\n    int kind;\n    int is_error;\n")
    w("    const char *message_prefix;\n    const char *output;\n"
      "    const char *brief;\n};\n\n")
    w("const plan_os_error_golden kPlanOsErrorGoldens[] = {\n")
    for name, tool, params, rel, kind in OS_ERROR_CASES:
        rec = run(driver.run_tool_case("os_" + name, tool, canon_json(params),
                                       rel_path=rel, kind=kind))
        prefix = ""
        if tool == "EditPlan":
            assert rec["message"].startswith("Failed to edit plan. Error: "), rec
            prefix = "Failed to edit plan. Error: "
        else:
            assert not rec["message"].startswith("Failed to write plan. Error: "), rec
        w("    {%s, %s, %s,\n     %s, %d, %d,\n"
          "     %s, %s, %s},\n"
          % (lit(name), lit(tool), lit(canon_json(params)),
             lit(rec["rel_path"]), KINDS[kind],
             rec["is_error"], lit(prefix), lit(rec["output"]), lit(rec["brief"])))
    w("};\n\n")

    # ---- the _enable_plan gate -----------------------------------------
    w("// `_enable_plan` gate: False makes the reference constructor raise\n")
    w("// SkipThisTool, so the tool is never offered to the model. The C++\n")
    w("// mirror is Session::plan_enabled == false -> the invocation answers\n")
    w("// tool_status::unsupported instead of running.\n")
    w("struct plan_gate_golden {\n    const char *tool;\n    int python_skips;\n};\n\n")
    w("const plan_gate_golden kPlanGateGoldens[] = {\n")
    gate_params = {
        "WritePlan": {"content": "x"},
        "ReadPlan": {},
        "EditPlan": {"edit": {"old": "a", "new": "b"}},
    }
    for name, _ in tools:
        rec = run(driver.run_tool_case(
            "gate_" + name, name, canon_json(gate_params[name]), enabled=False))
        assert rec["skipped"] is not None, name
        assert type(rec["skipped"]).__name__ == "SkipThisTool", rec["skipped"]
        w("    {%s, 1},\n" % lit(name))
    w("};\n")

    text = "".join(out)
    assert text.isascii(), "generated goldens must be ASCII-only"
    return text


def _instance(cls):
    from kimi_agent_sdk import CallableTool2  # noqa: PLC0415
    obj = cls.__new__(cls)
    CallableTool2.__init__(obj)
    obj._session = _FakeSession()
    return obj


# ---------------------------------------------------------------------------
# the generated KIMIX_REGISTER_TOOL block
# ---------------------------------------------------------------------------

def schemas_block(note) -> str:
    lines = []
    for name, cls in (("WritePlan", note.WritePlan), ("ReadPlan", note.ReadPlan),
                      ("EditPlan", note.EditPlan)):
        obj = _instance(cls)
        schema = canon_json(obj.base.parameters)
        assert '"' not in obj.description.replace('\\"', "")
        lines.append("KIMIX_REGISTER_TOOL(\n")
        lines.append("    %s,\n" % name)
        lines.append('    %s,\n' % lit_readable(obj.description))
        lines.append('    R"JSON(%s)JSON");\n' % schema)
    return "".join(lines)


def write_schemas(cpp_path: Path, note) -> int:
    src = cpp_path.read_text(encoding="utf-8")
    begin = src.find(CPP_META_BEGIN)
    end = src.find(CPP_META_END)
    if begin < 0 or end < 0 or end < begin:
        raise SystemExit("PLAN-TOOL-META markers not found in %s" % cpp_path)
    new = (src[:begin + len(CPP_META_BEGIN)] + "\n" + schemas_block(note)
           + src[end:])
    cpp_path.write_text(new, encoding="utf-8", newline="\n")
    return len(new) - len(src)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    global _LOOP
    parser = argparse.ArgumentParser(description="regenerate plan tool goldens")
    parser.add_argument("--reference", default=DEFAULT_REFERENCE,
                        help="kimi-agent checkout root")
    parser.add_argument("--out", default=str(DEFAULT_OUT),
                        help="output .inc path")
    parser.add_argument("--cpp", default=str(DEFAULT_CPP),
                        help="plan_tool.cpp (for --schemas)")
    parser.add_argument("--check", action="store_true",
                        help="exit 1 when the checked-in file is stale")
    parser.add_argument("--schemas", action="store_true",
                        help="rewrite the LLM-facing tool meta in plan_tool.cpp")
    parser.add_argument("--all", action="store_true",
                        help="--schemas + goldens")
    args = parser.parse_args(argv)

    root = Path(args.reference)
    if not (root / "kimi-cli" / "src" / "kimi_cli").is_dir():
        parser.error("not a kimi-agent checkout: %s" % root)
    note = import_reference(root)

    _LOOP = asyncio.new_event_loop()
    asyncio.set_event_loop(_LOOP)
    tmp = Path(tempfile.mkdtemp(prefix="plan_goldens_"))
    try:
        text = generate(note, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    out_path = Path(args.out)
    if args.check:
        if not out_path.is_file() or out_path.read_text(encoding="utf-8") != text:
            print("STALE: %s" % out_path, file=sys.stderr)
            return 1
        print("up to date: %s" % out_path)
        return 0
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("wrote %s (%d bytes)" % (out_path, len(text.encode("utf-8"))))

    if args.schemas or args.all:
        delta = write_schemas(Path(args.cpp), note)
        print("rewrote tool meta in %s (%+d bytes)" % (args.cpp, delta))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
