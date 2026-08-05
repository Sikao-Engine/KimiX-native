"""Parity tests for kimix_native.codec (plans 007/008/009).

Compares the native kernels against the pure-Python ``_compat`` mirrors on:
- envelope serialize/deserialize round-trip for a registry of type names
  (10 representative wire type names) with nested / unicode / null payloads
- canonicalize_payload recursive key sort 3 levels deep
- WireMergeBuffer append/flush semantics; ArgsBuffer delta watermark
- JSON-RPC frame + jsonl record bytes
- RecvBuffer: random chunk splits (byte equality vs the _compat reference
  loop); 64 MB copy-bound sanity (no O(n^2))
- SSE frame bytes (multi-line, id, event, empty)
- the KIMIX_NATIVE_CODEC=0 fallback toggle
"""

import os
import random
import struct

import pytest

from kimix_native import codec

# 10 representative wire type names (subset of the 40+ wire registry).
TYPE_NAMES = [
    "TurnBegin", "StepBegin", "StepRetry", "HookTriggered", "StatusUpdate",
    "Notification", "TextPart", "ThinkPart", "ToolCallPart", "ToolResult",
]

PAYLOADS = [
    b'{"user_input": "hello"}',
    b'{"n": 1, "next_attempt": 2, "wait_s": 1.5}',
    b'{"event": "PreToolUse", "target": "", "hook_count": 3}',
    b'{"type": "text", "text": "caf\\u00e9 \\u4e16\\u754c"}',
    b'{"id": null, "category": "sys", "payload": {"a": [1, 2, 3]}}',
    b'{"request_id": "r1", "response": "approve", "feedback": ""}',
    b'{"tools": [{"name": "a", "parameters": {}}]}',
    b'{"loading": true, "connected": 0, "total": 3, "servers": []}',
    b'{"parent_tool_call_id": null, "agent_id": "ag-1"}',
    b'{"emoji": "\\ud83d\\ude00", "nested": {"deep": {"x": [1, 2]}}}',
]


def test_envelope_roundtrip_type_registry():
    native = [codec.serialize_envelope(tn, pl) for tn, pl in
              zip(TYPE_NAMES, PAYLOADS)]
    compat = [codec._compat_serialize_envelope(tn, pl) for tn, pl in
              zip(TYPE_NAMES, PAYLOADS)]
    for tn, nf, cf in zip(TYPE_NAMES, native, compat):
        # Semantic equality: same envelope JSON (yyjson vs json.dumps).
        assert nf == cf, tn
        # Deserialize both ways.
        dn = codec.deserialize_envelope(nf)
        dc = codec._compat_deserialize_envelope(cf)
        assert dn is not None and dc is not None, tn
        assert dn[0] == dc[0] == tn
        assert dn[1] == dc[1], tn
        # Round-trip: re-serialize the deserialized envelope is stable.
        again = codec.serialize_envelope(dn[0], dn[1])
        assert codec.deserialize_envelope(again) == dn


def test_envelope_deserialize_malformed():
    for impl in (codec.deserialize_envelope, codec._compat_deserialize_envelope):
        assert impl(b"garbage") is None
        assert impl(b"{}") is None
        assert impl(b'{"type":1,"payload":{}}') is None
        assert impl(b"[1,2]") is None
    # Invalid payload text -> escaped-string fallback (still parseable).
    frame = codec.serialize_envelope("T", b"not json")
    out = codec.deserialize_envelope(frame)
    assert out is not None and out[0] == "T"


def test_canonicalize_key_sort_three_levels():
    payload = b'{"l3": {"l2": {"l1": {"z": 1, "y": 2}, "b": 3}, "a": 4}, "m": 5}'
    native = codec.canonicalize_payload(payload)
    compat = codec._compat_canonicalize_payload(payload)
    assert native == compat == (
        b'{"l3":{"a":4,"l2":{"b":3,"l1":{"y":2,"z":1}}},"m":5}'
    )


def test_canonicalize_parity_random():
    rng = random.Random(1234)
    scalars = [1, -2, 3.5, True, False, None, "s", "\\u4e16"]
    for _ in range(60):
        def gen(depth=0):
            if depth > 2 or rng.random() < 0.4:
                return rng.choice(scalars)
            if rng.random() < 0.5:
                return {f"k{rng.randrange(6)}": gen(depth + 1)
                        for _ in range(rng.randrange(4))}
            return [gen(depth + 1) for _ in range(rng.randrange(4))]
        obj = gen()
        blob = codec._compact(obj)
        native = codec.canonicalize_payload(blob)
        compat = codec._compat_canonicalize_payload(blob)
        assert native == compat, blob
    assert codec.canonicalize_payload(b"{") is None
    assert codec._compat_canonicalize_payload(b"{") is None


def test_wire_merge_buffer_semantics():
    native = codec.WireMergeBuffer()
    compat = codec._CompatWireMergeBuffer()
    for buf in (native, compat):
        assert buf.append("text", b"Hello ")
        assert buf.append("text", b"World")
        assert buf.snapshot() == b"Hello World"
        assert not buf.append("args", b"{}")  # kind change -> flush
        assert buf.snapshot() == b"Hello World"
        buf.reset()
        assert buf.empty()
        assert buf.append("args", b"{")
        assert buf.append("args", b"}")
        assert buf.snapshot() == b"{}"
        assert not buf.append("think", b"x")  # unknown kind never merges
        assert buf.snapshot() == b"{}"


def test_args_buffer_delta_watermark():
    native = codec.ArgsBuffer()
    compat = codec._CompatArgsBuffer()
    parts = [b'{"a":', b'1,', b'"b": [1,2]}']
    for impl in (native, compat):
        deltas = []
        for p in parts:
            impl.append(p)
            deltas.append(impl.delta_since())
        assert b"".join(deltas) == b'{"a":1,"b": [1,2]}'
        assert impl.snapshot() == b'{"a":1,"b": [1,2]}'
        assert impl.delta_since() == b""  # nothing new
        impl.reset()
        assert impl.snapshot() == b""


def test_frame_writer_and_jsonl_bytes():
    native_w = codec.JsonRpcFrameWriter()
    native_r = codec.JsonlRecorder()
    payload = b'{"jsonrpc":"2.0","id":1,"method":"event"}'
    assert native_w.write(payload) == payload + b"\n"
    assert native_r.record(payload) == payload + b"\n"
    assert native_w.write(b"") == b"\n"
    assert native_r.record(b"") == b"\n"


def test_recv_buffer_random_chunk_splits():
    rng = random.Random(99)
    frames = []
    for i in range(50):
        body = (f"frame-{i}-" + "x" * rng.randrange(0, 300)).encode()
        frames.append(struct.pack(">I", len(body)) + body)

    stream = b"".join(frames)
    # Random split points.
    cuts = sorted(rng.randrange(0, len(stream) + 1) for _ in range(40))
    chunks = []
    prev = 0
    for c in cuts:
        if c > prev:
            chunks.append(stream[prev:c])
        prev = c
    if prev < len(stream):
        chunks.append(stream[prev:])

    native = codec.RecvBuffer()
    compat = codec._CompatRecvBuffer()
    for chunk in chunks:
        native.append(chunk)
        compat.append(chunk)
        while True:
            nf = native.take_frame_length_prefixed()
            cf = compat.take_frame_length_prefixed()
            assert nf == cf, (len(chunks), nf, cf)
            if nf is None:
                break
            assert nf.startswith(b"frame-")
    assert native.size() == compat.size()
    assert native.peek() == compat.peek()


def test_recv_buffer_copy_bound_64mb():
    # 64 MB in 4 KB frames: native must not exhibit O(n^2) byte concat.
    payload = b"z" * (4 * 1024)
    framed = struct.pack(">I", len(payload)) + payload
    native = codec.RecvBuffer()
    total = 0
    for _ in range((64 * 1024 * 1024) // len(payload)):
        native.append(framed)
        total += len(payload)
        while native.take_frame_length_prefixed() is not None:
            pass
    assert native.size() == 0


def test_recv_buffer_delimiter_parity():
    rng = random.Random(7)
    lines = [f"line-{i}".encode() for i in range(30)]
    stream = b"\n".join(lines) + b"\n"
    native = codec.RecvBuffer()
    compat = codec._CompatRecvBuffer()
    pos = 0
    while pos < len(stream):
        n = rng.randrange(1, 9)
        native.append(stream[pos:pos + n])
        compat.append(stream[pos:pos + n])
        pos += n
        while True:
            nf = native.take_frame_delimiter(b"\n")
            cf = compat.take_frame_delimiter(b"\n")
            assert nf == cf
            if nf is None:
                break


def test_sse_frame_bytes():
    cases = [
        ("message", b'{"id":"evt_1","type":"text","properties":{}}', 1,
         b"event: message\nid: 1\ndata: {\"id\":\"evt_1\",\"type\":\"text\",\"properties\":{}}\n\n"),
        ("message", b"line1\nline2", 7,
         b"event: message\nid: 7\ndata: line1\ndata: line2\n\n"),
        ("", b"{\"x\":1}", 5, b"id: 5\ndata: {\"x\":1}\n\n"),
        ("message", b"data", 0, b"event: message\ndata: data\n\n"),
        ("message", b"", 0, b"event: message\ndata: \n\n"),
    ]
    for name, data, eid, expected in cases:
        assert codec.build_sse_frame(name, data, eid) == expected
        assert codec._compat_build_sse_frame(name, data, eid) == expected


def test_native_disabled_fallback():
    """With KIMIX_NATIVE_CODEC=0 the shim must behave identically (the
    _compat mirrors are used)."""
    env = dict(os.environ, KIMIX_NATIVE_CODEC="0")
    import subprocess
    import sys
    code = (
        "import sys; sys.path.insert(0, %r); sys.path.insert(0, %r);\n"
        "from kimix_native import codec\n"
        "assert codec._USE is False\n"
        "env = codec.serialize_envelope('StepBegin', b'{\"n\": 1}')\n"
        "out = codec.deserialize_envelope(env)\n"
        "assert out == ('StepBegin', b'{\"n\":1}'), out\n"
        "assert codec.canonicalize_payload(b'{\"b\":2,\"a\":1}') == b'{\"a\":1,\"b\":2}'\n"
        "mb = codec.WireMergeBuffer(); mb.append('text', b'a'); mb.append('text', b'b')\n"
        "assert mb.snapshot() == b'ab'\n"
        "rb = codec.RecvBuffer(); rb.append((2).to_bytes(4, 'big') + b'hi')\n"
        "assert rb.take_frame_length_prefixed() == b'hi'\n"
        "assert codec.build_sse_frame('m', b'x', 3) == b'event: m\\nid: 3\\ndata: x\\n\\n'\n"
        "print('FALLBACK_OK')\n"
    ) % (os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
         os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "..", "bin", "debug"))
    r = subprocess.run([sys.executable, "-c", code], capture_output=True,
                       text=True, env=env)
    assert r.returncode == 0, r.stderr
    assert "FALLBACK_OK" in r.stdout
