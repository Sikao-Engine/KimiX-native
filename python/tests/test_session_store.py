"""session.Store: the serve-mode part ring + session table (report A3 + C3).

The snapshot JSON is an opencode-shaped public contract, so it is pinned two
ways:

* the native writer is compared byte-for-byte with the shim's ``_CompatStore``
  (and with the C++ golden in ``tests/unit/native/test_session_store.cpp``);
* ``snapshot(since_seq)`` and ``part_json`` are compared too, because the SSE
  resume path uses both.
"""

from __future__ import annotations

import orjson
import pytest

from kimix_native import session

# The five-part fixture whose JSON is pinned by the C++ suite.
GOLDEN = (
    b'{"sessionID":"ses_1","status":0,"seq":5,"parts":['
    b'{"id":"p1","sessionID":"ses_1","messageID":"msg_1","type":"text",'
    b'"text":"hello","time":{"start":1,"end":2}},'
    b'{"id":"p2","sessionID":"ses_1","messageID":"msg_1","type":"tool",'
    b'"callID":"call_1","tool":"read","state":{"status":"completed"}},'
    b'{"id":"p3","sessionID":"ses_1","messageID":"msg_1","type":"step-finish",'
    b'"reason":"stop","snapshot":"snap","cost":0.5,"prompt":{"a":1},"tokens":{"in":2}},'
    b'{"id":"p4","sessionID":"ses_1","messageID":"msg_1","type":"reasoning",'
    b'"text":"thinking"},'
    b'{"id":"p5","sessionID":"ses_1","messageID":"msg_1","type":"step-start",'
    b'"snapshot":"s0"}]}'
)


def fill(store, handle):
    store.append(handle, "text", "assistant", session_id=b"ses_1",
                 message_id=b"msg_1", part_id=b"p1", text=b"hello",
                 time_json=b'{"start":1,"end":2}')
    store.append(handle, "tool", "assistant", session_id=b"ses_1",
                 message_id=b"msg_1", part_id=b"p2", call_id=b"call_1",
                 tool=b"read", state_json=b'{"status":"completed"}')
    store.append(handle, "step-finish", "assistant", session_id=b"ses_1",
                 message_id=b"msg_1", part_id=b"p3", reason=b"stop",
                 snapshot=b"snap", cost=0.5, prompt_json=b'{"a":1}',
                 tokens_json=b'{"in":2}')
    store.append(handle, "reasoning", "assistant", session_id=b"ses_1",
                 message_id=b"msg_1", part_id=b"p4", text=b"thinking")
    store.append(handle, "step-start", "assistant", session_id=b"ses_1",
                 message_id=b"msg_1", part_id=b"p5", snapshot=b"s0")


def test_snapshot_golden():
    store = session.Store(4096, 65536)
    handle = store.create("ses_1")
    fill(store, handle)
    assert store.snapshot(handle) == GOLDEN
    assert store.part_count(handle) == 5
    assert store.sessions() == 1
    assert store.generation(handle) == 5
    # The golden is valid JSON with the opencode MessageWithParts shape.
    payload = orjson.loads(GOLDEN)
    assert payload["sessionID"] == "ses_1"
    assert [part["type"] for part in payload["parts"]] == [
        "text", "tool", "step-finish", "reasoning", "step-start",
    ]


def test_compat_matches_native():
    native = session.Store(4096, 65536)
    compat = session._CompatStore(4096, 65536)
    native_handle = native.create("ses_1")
    compat_handle = compat.create("ses_1")
    fill(native, native_handle)
    fill(compat, compat_handle)
    assert native.snapshot(native_handle) == compat.snapshot(compat_handle)
    assert native.snapshot(native_handle, 4) == compat.snapshot(compat_handle, 4)
    assert native.part_json(native_handle, b"p2") == compat.part_json(
        compat_handle, b"p2"
    )
    assert native.part_json(native_handle, b"nope") is None
    assert compat.part_json(compat_handle, b"nope") is None


def test_snapshot_since_seq():
    store = session.Store(4096, 65536)
    handle = store.create("ses_1")
    fill(store, handle)
    payload = orjson.loads(store.snapshot(handle, 4))
    assert [part["id"] for part in payload["parts"]] == ["p4", "p5"]
    assert payload["seq"] == 5
    with pytest.raises(ValueError):
        store.snapshot(999)


def test_ring_budget_drops_oldest():
    store = session.Store(120, 65536)
    handle = store.create("ses_1")
    fill(store, handle)
    assert store.part_count(handle) < 5
    assert store.dropped(handle) >= 1
    payload = orjson.loads(store.snapshot(handle))
    assert "p1" not in [part["id"] for part in payload["parts"]]
    assert "p5" in [part["id"] for part in payload["parts"]]
    assert store.bytes_live() <= 120


def test_oversized_part_rejected():
    store = session.Store(64, 65536)
    handle = store.create("ses_1")
    assert store.append(handle, "text", "assistant", part_id=b"p", text=b"x" * 4096) is False


def test_lru_eviction():
    store = session.Store(4096, 200)
    first = store.create("ses_a")
    fill(store, first)
    second = store.create("ses_b")
    fill(store, second)
    assert store.sessions() == 1
    with pytest.raises(ValueError):
        store.snapshot(first)
    payload = orjson.loads(store.snapshot(second))
    assert payload["sessionID"] == "ses_b"
    assert store.bytes_live() <= 200


def test_status_and_lifecycle():
    store = session.Store()
    handle = store.create("ses_1")
    assert store.create("ses_1") == handle  # idempotent
    assert store.status(handle) == session.STATUS_IDLE
    store.set_status(handle, session.STATUS_BUSY)
    assert store.status(handle) == session.STATUS_BUSY
    assert b'"status":1' in store.snapshot(handle)
    assert store.close(handle) is True
    assert store.close(handle) is False
    assert store.sessions() == 0
    store.clear()


def test_unknown_kind_rejected():
    store = session.Store()
    handle = store.create("ses_1")
    with pytest.raises(ValueError):
        store.append(handle, "bogus", "assistant", part_id=b"p")


def test_fallback_switch(monkeypatch):
    monkeypatch.setenv("KIMIX_NATIVE_SESSION", "0")
    store = session.new_store(4096, 65536)
    assert store._s is None
    handle = store.create("ses_1")
    fill(store, handle)
    assert store.snapshot(handle) == GOLDEN
    assert store.bytes_owned() == 0


def test_utf8_ids_and_text():
    store = session.Store(4096, 65536)
    handle = store.create("ses_世界")
    assert store.append(handle, "text", "assistant", session_id="ses_世界".encode(),
                        message_id=b"m", part_id=b"p", text="text 世界".encode())
    payload = orjson.loads(store.snapshot(handle))
    assert payload["sessionID"] == "ses_世界"
    assert payload["parts"][0]["text"] == "text 世界"
