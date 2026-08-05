"""Parity tests for kimix_native.tools.build_export_markdown (plan 016).

Native kernel vs the pure-Python ``_compat`` mirror, and (when the real
kimi-agent export module is importable) against the reference
build_export_markdown bytes:
- metadata header + Overview + turns on seeded histories
- tool-call blocks (hint extraction, orjson OPT_INDENT_2 args JSON)
- tool-result collapsible blocks, thinking details
- internal user messages skipped
"""

import os
import random
import sys

import pytest

from kimix_native import tools

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KOSONG = r"C:\dev\kimi-agent\kimi-cli\packages\kosong\src"
KIMICLI = r"C:\dev\kimi-agent\kimi-cli\src"

_REF = None


def _ref_module():
    global _REF
    if _REF is not None:
        return _REF
    try:
        for p in (KOSONG, KIMICLI):
            if p not in sys.path:
                sys.path.insert(0, p)
        from kimi_cli.utils.export import build_export_markdown as ref  # noqa
        from kosong.message import Message, TextPart, ThinkPart, ToolCall  # noqa
        import pendulum  # noqa

        _REF = {"build": ref, "Message": Message, "TextPart": TextPart,
                "ThinkPart": ThinkPart, "ToolCall": ToolCall, "pendulum": pendulum}
    except Exception:
        _REF = None
    return _REF


def _msg(role, content=None, tool_calls=None, tool_call_id=None):
    return {
        "role": role,
        "content": content if content is not None else [],
        "tool_calls": tool_calls,
        "tool_call_id": tool_call_id,
    }


def _ref_markdown(history, opts):
    mod = _ref_module()
    if mod is None:
        return None, None
    msgs = []
    for m in history:
        parts = []
        for p in m.get("content") or []:
            if p.get("type") == "text":
                parts.append(mod["TextPart"](text=p.get("text", "")))
            elif p.get("type") == "think":
                parts.append(mod["ThinkPart"](think=p.get("think", "")))
            else:
                raise ValueError(f"unsupported part {p}")
        tcs = None
        if m.get("tool_calls"):
            tcs = [
                mod["ToolCall"](
                    id=tc.get("id", ""),
                    function=mod["ToolCall"].FunctionBody(
                        name=(tc.get("function") or {}).get("name", ""),
                        arguments=(tc.get("function") or {}).get("arguments"),
                    ),
                )
                for tc in m["tool_calls"]
            ]
        msgs.append(mod["Message"](role=m["role"], content=parts, tool_calls=tcs,
                                   tool_call_id=m.get("tool_call_id")))
    now = mod["pendulum"].datetime(2024, 5, 1, 12, 0, 0)
    exported_at = now.isoformat(timespec="seconds")
    return mod["build"](opts["session_id"], opts["work_dir"], msgs,
                        opts["token_count"], now).encode("utf-8"), exported_at


def _seed_history(rng):
    hist = []
    n = rng.randint(2, 8)
    for i in range(n):
        kind = rng.random()
        if kind < 0.15:
            hist.append(_msg("user", [{"type": "text",
                                       "text": "<system-reminder>\nr\n</system-reminder>"}]))
        elif kind < 0.25:
            hist.append(_msg("user", [{"type": "text",
                                       "text": '<notification id="n" category="task">t</notification>'}]))
        elif kind < 0.55:
            text = rng.choice(["hello", "what is 2+2?", "read file.txt please",
                               "   ", "caf\u00e9 \u4e16\u754c"])
            hist.append(_msg("user", [{"type": "text", "text": text}]))
        elif kind < 0.8:
            parts = []
            if rng.random() < 0.4:
                parts.append({"type": "think", "think": "thinking about it"})
            if rng.random() < 0.7:
                parts.append({"type": "text", "text": rng.choice(["ok", "done", "42"])})
            hist.append(_msg("assistant", parts))
            if rng.random() < 0.6:
                tid = f"call_{i}"
                args = rng.choice(['{"path": "a.txt"}', '{"query": "x"}', "{}",
                                   '{"name": "tool", "pattern": "*.py"}', "not json{"])
                hist[-1] = _msg("assistant", parts, [
                    {"type": "function", "id": tid,
                     "function": {"name": rng.choice(["read_file", "search"]),
                                  "arguments": args}}])
                hist.append(_msg("tool", [{"type": "text",
                                           "text": rng.choice(["content", "nothing found"])}],
                                 tool_call_id=tid))
        else:
            hist.append(_msg("assistant", [{"type": "text", "text": "final"}]))
    return hist


def test_export_markdown_native_compat_parity():
    rng = random.Random(2024)
    opts = {"session_id": "sess-001", "work_dir": "C:/work",
            "exported_at": "2024-05-01T12:00:00", "token_count": 1234567}
    for _ in range(30):
        hist = _seed_history(rng)
        native = tools.build_export_markdown(hist, opts)
        compat = tools._compat_build_export_markdown(hist, opts)
        assert native == compat, (hist, native, compat)


def test_export_markdown_reference_parity():
    mod = _ref_module()
    opts = {"session_id": "sess-001", "work_dir": "C:/work",
            "exported_at": "2024-05-01T12:00:00", "token_count": 1234567}
    rng = random.Random(31)
    for _ in range(10):
        hist = _seed_history(rng)
        ref, exported_at = _ref_markdown(hist, opts)
        if ref is not None:
            opts2 = dict(opts, exported_at=exported_at)
            native = tools.build_export_markdown(hist, opts2)
            assert native == ref, (hist, native.decode(), ref.decode())


def test_export_header_overview_and_turns():
    hist = [
        _msg("user", [{"type": "text", "text": "What is the weather?"}]),
        _msg("assistant", [{"type": "text", "text": "It is sunny."}]),
    ]
    opts = {"session_id": "s1", "work_dir": "wd", "exported_at": "2024-05-01T12:00:00",
            "token_count": 1234567}
    out = tools.build_export_markdown(hist, opts).decode()
    assert "session_id: s1" in out
    assert "message_count: 2" in out
    assert "token_count: 1234567" in out
    assert "- **Topic**: What is the weather?" in out
    assert "1,234,567 tokens" in out
    assert "## Turn 1" in out
    assert "### User" in out and "### Assistant" in out


def test_export_tool_blocks_and_escaping():
    hist = [
        _msg("user", [{"type": "text", "text": "read a.txt"}]),
        _msg("assistant", [],
             [{"type": "function", "id": "call_1",
               "function": {"name": "read_file", "arguments": '{"path": "a.txt"}'}}]),
        _msg("tool", [{"type": "text", "text": "file contents here"}], tool_call_id="call_1"),
    ]
    opts = {"session_id": "s1", "work_dir": "wd", "exported_at": "2024-05-01T12:00:00",
            "token_count": 1}
    out = tools.build_export_markdown(hist, opts).decode()
    assert "#### Tool Call: read_file (`a.txt`)" in out
    assert "```json\n{\n  \"path\": \"a.txt\"\n}\n```" in out
    assert "<!-- call_id: call_1 -->" in out
    assert ("<details><summary>Tool Result: read_file (`a.txt`)</summary>"
            "\n\n<!-- call_id: call_1 -->\nfile contents here\n\n</details>") in out


def test_export_internal_messages_skipped():
    hist = [
        _msg("user", [{"type": "text", "text": "<system-reminder>\nr\n</system-reminder>"}]),
        _msg("user", [{"type": "text", "text": "real"}]),
        _msg("assistant", [{"type": "text", "text": "answer"}]),
    ]
    opts = {"session_id": "s1", "work_dir": "wd", "exported_at": "2024-05-01T12:00:00",
            "token_count": 0}
    out = tools.build_export_markdown(hist, opts).decode()
    assert "message_count: 3" in out
    assert "system-reminder" not in out
    assert "## Turn 1" in out


def test_export_thinking_details():
    hist = [
        _msg("user", [{"type": "text", "text": "q"}]),
        _msg("assistant", [{"type": "think", "think": "hmm"},
                           {"type": "text", "text": "done"}]),
    ]
    opts = {"session_id": "s1", "work_dir": "wd", "exported_at": "2024-05-01T12:00:00",
            "token_count": 0}
    out = tools.build_export_markdown(hist, opts).decode()
    assert "<details><summary>Thinking</summary>\n\nhmm\n\n</details>" in out
