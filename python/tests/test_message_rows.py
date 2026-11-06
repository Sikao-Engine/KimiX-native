"""index.context_rows / parse_rows — history persistence rows (report B2).

Three layers:

* the native kernels (``runtime_py.index.context_rows`` / ``parse_rows``);
* the shim's ``_compat`` mirrors (``KIMIX_NATIVE_INDEX=0``);
* the reference serialisation (``Message.model_dump_json(exclude_none=True)`` +
  ``extract_text_from_content``) from the kimi-agent checkout, when importable.

The row blob is ``u32 count | per row {u32 json_len, u32 fts_len, json, fts}``;
the row JSON is ``{"role", "content"|[…], "tool_calls", "tool_call_id"}`` with a
lone TextPart collapsed to a string.
"""

from __future__ import annotations

import importlib.util
import json
import os
import sys

import orjson
import pytest

from kimix_native import _structure, index, soul

KOSONG = r"C:\dev\kimi-agent\kimi-cli\src"


def _reference():
    """(Message, extract_text_from_content) from the sibling checkout, or None."""
    if not os.path.isdir(KOSONG):
        return None
    try:
        if KOSONG not in sys.path:
            sys.path.insert(0, KOSONG)
        from kosong.message import Message, TextPart, ThinkPart, ToolCall  # noqa
        from kimi_cli.soul.fts5_search import extract_text_from_content  # noqa

        return {
            "Message": Message,
            "TextPart": TextPart,
            "ThinkPart": ThinkPart,
            "ToolCall": ToolCall,
            "fts": extract_text_from_content,
        }
    except Exception:
        return None


HISTORY = [
    {"role": "user", "content": [{"type": "text", "text": "hello"}]},
    {"role": "assistant", "content": [
        {"type": "text", "text": "reading"},
        {"type": "think", "think": "deliberating"},
    ], "tool_calls": [
        {"id": "call_1", "function": {"name": "read", "arguments": "{\"path\":\"a\"}"}},
    ]},
    {"role": "tool", "content": [{"type": "text", "text": "line one"},
                                 {"type": "text", "text": "line two"}],
     "tool_call_id": "call_1"},
    {"role": "user", "content": [{"type": "text", "text": ""}]},
]


def decode_rows(blob: bytes) -> list[tuple[dict, bytes]]:
    import struct

    (count,) = struct.unpack_from("<I", blob, 0)
    offset = 4
    out = []
    for _ in range(count):
        json_len, fts_len = struct.unpack_from("<II", blob, offset)
        offset += 8
        row = orjson.loads(blob[offset:offset + json_len])
        offset += json_len
        fts = blob[offset:offset + fts_len]
        offset += fts_len
        out.append((row, fts))
    return out


def test_context_rows_native_matches_compat():
    native = index.context_rows(HISTORY)
    compat = index._compat_context_rows(HISTORY)
    assert native == compat
    rows = decode_rows(native)
    assert len(rows) == len(HISTORY)
    # A lone TextPart collapses to a plain string.
    assert rows[0][0] == {"role": "user", "content": "hello"}
    assert rows[0][1] == b"hello"
    # Multi-part content keeps the part list; think keeps `encrypted: null`.
    assert rows[1][0]["content"][0] == {"type": "text", "text": "reading"}
    assert rows[1][0]["content"][1] == {
        "type": "think", "think": "deliberating", "encrypted": None,
    }
    assert rows[1][0]["tool_calls"][0]["function"]["arguments"] == "{\"path\":\"a\"}"
    # Two text parts -> a content list + a newline-joined FTS text.
    assert rows[2][0]["content"] == [
        {"type": "text", "text": "line one"}, {"type": "text", "text": "line two"},
    ]
    assert rows[2][1] == b"line one\nline two"
    assert rows[2][0]["tool_call_id"] == "call_1"


def test_parse_rows_roundtrip_into_history():
    blob = index.context_rows(HISTORY)
    handle = soul.new_history()
    added = index.parse_rows(blob, handle)
    assert added == len(HISTORY)
    assert len(handle) == len(HISTORY)
    decoded = handle.to_messages()
    assert decoded == [_structure.normalize_message(m) for m in HISTORY]


def test_parse_rows_list_mode():
    blob = index.context_rows(HISTORY)
    rows = index.parse_rows(blob)
    assert len(rows) == len(HISTORY)
    assert rows[0]["content"][0]["text"] == "hello"


def test_parse_rows_rejects_malformed():
    blob = index.context_rows(HISTORY)
    handle = soul.new_history()
    with pytest.raises(ValueError):
        index.parse_rows(blob[: len(blob) - 4], handle)
    broken = bytearray(blob)
    broken[12:16] = b"xxxx"
    with pytest.raises(ValueError):
        index.parse_rows(bytes(broken), handle)


def test_rows_match_reference_serialisation():
    ref = _reference()
    if ref is None:
        pytest.skip("kimi-agent checkout not importable")
    Message, TextPart, ThinkPart, ToolCall = (
        ref["Message"], ref["TextPart"], ref["ThinkPart"], ref["ToolCall"]
    )
    messages = [
        Message(role="user", content=[TextPart(text="hello")]),
        Message(
            role="assistant",
            content=[TextPart(text="reading"), ThinkPart(think="deliberating")],
            tool_calls=[ToolCall(id="call_1",
                                 function=ToolCall.FunctionBody(name="read",
                                                                arguments="{\"path\":\"a\"}"))],
        ),
        Message(role="tool", content=[TextPart(text="line one"), TextPart(text="line two")],
                tool_call_id="call_1"),
        Message(role="user", content=[TextPart(text="")]),
    ]
    rows = decode_rows(index.context_rows(HISTORY))
    for message, (row, fts) in zip(messages, rows):
        reference_json = orjson.loads(message.model_dump_json(exclude_none=True))
        assert row == reference_json
        assert fts.decode() == ref["fts"](message.content)


def test_durable_index_binding(tmp_path):
    path = str(tmp_path / "history.db")
    store = index.SqliteHistoryIndex(path)
    ok, error = store.open()
    assert ok, error
    assert store.is_open
    appended = store.append_turns([
        {"turn_id": 0, "timestamp": 1000.0, "role": "user", "text": "alpha beta"},
        {"turn_id": 1, "timestamp": 1001.0, "role": "assistant", "text": "gamma delta"},
    ])
    assert appended == 2
    assert store.turn_count() == 2
    hits = store.search("alpha", 5)
    assert [hit["text"] for hit in hits] == ["alpha beta"]
    assert hits[0]["role"] == "user"
    assert store.get_by_id(0) == "alpha beta"
    assert store.get_by_id(99) is None
    assert store.non_compacted_turn_ids() == [0, 1]
    store.mark_compacted()
    assert store.non_compacted_turn_ids() == []
    reserved = store.reserve_turn_id()
    store.insert_turn_with_id(reserved, 1002.0, "user", "prune payload")
    assert store.get_by_id(reserved) == "prune payload"
    assert store.fts_stale is False
    store.close()
    assert store.is_open is False


def test_durable_index_fallback_mirror(tmp_path, monkeypatch):
    monkeypatch.setenv("KIMIX_NATIVE_INDEX", "0")
    store = index.SqliteHistoryIndex(str(tmp_path / "fallback.db"))
    assert store.open()[0] is True
    assert store.append_turns([{"turn_id": 0, "timestamp": 1.0, "role": "user",
                               "text": "alpha beta"}]) == 1
    assert store.turn_count() == 1
    assert [hit["text"] for hit in store.search("alpha", 3)] == ["alpha beta"]
    assert store.get_by_id(0) == "alpha beta"
    store.clear()
    assert store.turn_count() == 0
