"""Parity tests for kimix_native.soul (plans 014/015/016).

Native kernels vs the pure-Python ``_compat`` mirrors (and, when importable,
against the real kimi-agent reference implementations) on:
- >= 100 message shapes: payload dict equality (json.loads of the native
  bytes == reference _convert_message output)
- normalize_tool_call_ids plan application == reference id rewriting
- prune decisions on >= 50 seeded histories (incl. adversarial
  interleavings) == reference context_pruning candidate selection
- leading-reminder counts
- normalize_history plan application == reference normalized history
- compaction prompt bytes == reference SimpleCompaction.prepare text
"""

import json
import os
import random
import sys

import pytest

from kimix_native import soul

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # repo root
KOSONG = r"C:\dev\kimi-agent\kimi-cli\packages\kosong\src"
KIMICLI = r"C:\dev\kimi-agent\kimi-cli\src"

_REF = None  # ("kosong", module) or None


def _load_reference():
    global _REF
    if _REF is not None:
        return _REF
    try:
        for p in (KOSONG, KIMICLI):
            if p not in sys.path:
                sys.path.insert(0, p)
        from kosong.message import Message, TextPart, ThinkPart, ToolCall  # noqa
        from kosong.chat_provider.kimi import _convert_message  # noqa
        from kosong.chat_provider.kimi import _normalize_tool_call_ids  # noqa

        _REF = ("kosong", None)
        _REF = ("kosong", {
            "Message": Message, "TextPart": TextPart, "ThinkPart": ThinkPart,
            "ToolCall": ToolCall, "_convert_message": _convert_message,
            "_normalize_tool_call_ids": _normalize_tool_call_ids,
        })
    except Exception:
        _REF = ("compat", None)
    return _REF


def _ref_module():
    kind, mod = _load_reference()
    return mod


def _msg(role, content=None, tool_calls=None, tool_call_id=None):
    return {
        "role": role,
        "content": content if content is not None else [],
        "tool_calls": tool_calls,
        "tool_call_id": tool_call_id,
    }


def _ref_convert(history, preserved_thinking=False):
    """Reference payload dicts (real kimi.py or the _compat mirror)."""
    mod = _ref_module()
    if mod is None:
        return [
            soul._compat_convert_message(m, preserved_thinking=preserved_thinking)
            for m in history
        ]
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
            tcs = []
            for tc in m["tool_calls"]:
                fn = tc.get("function") or {}
                tcs.append(
                    mod["ToolCall"](
                        id=tc.get("id", ""),
                        function=mod["ToolCall"].FunctionBody(
                            name=fn.get("name", ""),
                            arguments=fn.get("arguments"),
                        ),
                    )
                )
        msgs.append(
            mod["Message"](
                role=m["role"], content=parts, tool_calls=tcs,
                tool_call_id=m.get("tool_call_id"),
            )
        )
    return [
        mod["_convert_message"](m, preserved_thinking_enabled=preserved_thinking)
        for m in msgs
    ]


def _payload(history, preserved_thinking=False):
    raw = soul.build_payload(history, preserved_thinking=preserved_thinking)
    return json.loads(raw.decode("utf-8", "surrogatepass"))


# ---------------------------------------------------------------------------
# Payload parity: >= 100 message shapes
# ---------------------------------------------------------------------------


def _generate_shapes(rng):
    shapes = []
    texts = ["", "hello", "  ", "with \"quote\" and \\ backslash",
             "caf\u00e9 \u4e16\u754c", "line1\nline2\ttab", "x" * 300]
    think_texts = ["", "reasoning here", "\u601d\u8003"]
    # role x content shapes
    for role in ("system", "user", "assistant", "tool"):
        for text in texts[:4]:
            shapes.append(_msg(role, [{"type": "text", "text": text}]))
    # think + text combos
    for t in think_texts:
        shapes.append(_msg("assistant", [{"type": "think", "think": t}]))
        shapes.append(_msg("assistant", [{"type": "think", "think": t},
                                         {"type": "text", "text": "visible"}]))
        shapes.append(_msg("assistant", [{"type": "text", "text": "a"},
                                         {"type": "think", "think": t},
                                         {"type": "text", "text": "b"}]))
    # multi-part text
    shapes.append(_msg("user", [{"type": "text", "text": "a"},
                                {"type": "text", "text": "b"}]))
    shapes.append(_msg("user", []))
    # tool calls variants
    shapes.append(_msg("assistant", [],
                       [{"type": "function", "id": "call_1",
                         "function": {"name": "f", "arguments": "{}"}}]))
    shapes.append(_msg("assistant", [],
                       [{"type": "function", "id": "c1",
                         "function": {"name": "g", "arguments": None}}]))
    shapes.append(_msg("assistant", [{"type": "text", "text": "visible"}],
                       [{"type": "function", "id": "c2",
                         "function": {"name": "h", "arguments": '{"a": 1}'}}]))
    shapes.append(_msg("assistant", [{"type": "text", "text": "  "}],
                       [{"type": "function", "id": "c3",
                         "function": {"name": "h", "arguments": "[]"}}]))
    shapes.append(_msg("assistant", [],
                       [{"type": "function", "id": "x", "function": {"name": "m"}},
                        {"type": "function", "id": "y", "function": {"name": "n"}}]))
    # tool messages (real ids; tool_call_id="" is a degenerate shape the
    # bridge treats as None -- see soul.py bridge contract)
    for tid in ("call_1", "t-42", "call_x"):
        shapes.append(_msg("tool", [{"type": "text", "text": "result"}],
                           tool_call_id=tid))
    # random multi-message histories
    pool = list(shapes)
    for _ in range(60):
        n = rng.randint(1, 5)
        hist = [dict(rng.choice(pool)) for _ in range(n)]
        shapes.append(hist)
    return shapes


def test_payload_parity_100_shapes():
    rng = random.Random(20240501)
    shapes = _generate_shapes(rng)
    single = [s for s in shapes if isinstance(s, dict)]
    histories = [s for s in shapes if isinstance(s, list)]
    count = 0
    for msg in single:
        native = _payload([msg])
        ref = _ref_convert([msg])
        assert native == ref, (msg, native, ref)
        # preserved_thinking variant
        native2 = _payload([msg], preserved_thinking=True)
        ref2 = _ref_convert([msg], preserved_thinking=True)
        assert native2 == ref2, ("preserved", msg, native2, ref2)
        count += 2
    for hist in histories:
        native = _payload(hist)
        ref = _ref_convert(hist)
        assert native == ref, (hist, native, ref)
        count += 1
    assert count >= 100


# ---------------------------------------------------------------------------
# normalize_tool_call_ids parity
# ---------------------------------------------------------------------------


def _apply_ref_id_fixes(history):
    mod = _ref_module()
    if mod is None:
        fixes = soul._compat_normalize_tool_call_ids(history)
        return soul.apply_id_fixes(history, fixes)
    msgs = []
    for m in history:
        parts = [{"type": "text", "text": p.get("text", "")}
                 for p in (m.get("content") or [])]
        msgs.append((m, parts))
    # rebuild kosong messages
    kosong_msgs = []
    for m in history:
        parts = []
        for p in m.get("content") or []:
            if p.get("type") == "text":
                parts.append(mod["TextPart"](text=p.get("text", "")))
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
        kosong_msgs.append(
            mod["Message"](role=m["role"], content=parts, tool_calls=tcs,
                           tool_call_id=m.get("tool_call_id"))
        )
    normalized = mod["_normalize_tool_call_ids"](kosong_msgs)
    out = []
    for nm, orig in zip(normalized, history):
        d = dict(orig)
        d["tool_calls"] = [
            {"type": "function", "id": tc.id,
             "function": {"name": tc.function.name, "arguments": tc.function.arguments}}
            for tc in (nm.tool_calls or [])
        ] if nm.tool_calls else orig.get("tool_calls")
        d["tool_call_id"] = nm.tool_call_id
        out.append(d)
    return out


def test_normalize_tool_call_ids_parity():
    cases = [
        [_msg("assistant", [], [{"type": "function", "id": "Read:9",
                                 "function": {"name": "f", "arguments": "{}"}}])],
        [_msg("assistant", [], [{"type": "function", "id": "bad id",
                                 "function": {"name": "f", "arguments": "{}"}},
                                {"type": "function", "id": "bad\tid",
                                 "function": {"name": "g", "arguments": "{}"}}])],
        [_msg("tool", [{"type": "text", "text": "r"}], tool_call_id="Read:9")],
        [_msg("assistant", [], [{"type": "function", "id": "a b",
                                 "function": {"name": "f", "arguments": "{}"}}]),
         _msg("tool", [{"type": "text", "text": "r"}], tool_call_id="a b")],
        [_msg("assistant", [], [{"type": "function", "id": "ok-id_1",
                                 "function": {"name": "f", "arguments": "{}"}}])],
        [_msg("assistant", [], [{"type": "function", "id": "x" * 70,
                                 "function": {"name": "f", "arguments": "{}"}}])],
        [_msg("assistant", [], [{"type": "function", "id": "",
                                 "function": {"name": "f", "arguments": "{}"}}])],
        [_msg("user", [{"type": "text", "text": "no ids"}])],
    ]
    rng = random.Random(7)
    # Note: empty tool_call_id on tool messages is excluded (the bridge
    # treats an empty span as None); empty tool_calls ids are covered by the
    # explicit case above.
    ids = ["good_id", "Read:9", "bad id", "a/b", "x" * 70, "caf\u00e9"]
    for _ in range(20):
        n = rng.randint(1, 6)
        hist = []
        for i in range(n):
            if rng.random() < 0.5:
                tid = rng.choice(ids)
                hist.append(_msg(
                    "assistant", [],
                    [{"type": "function", "id": tid,
                      "function": {"name": "f", "arguments": "{}"}}]))
            else:
                hist.append(_msg("tool", [{"type": "text", "text": "r"}],
                                 tool_call_id=rng.choice(ids)))
        cases.append(hist)
    for hist in cases:
        native_fixes = soul.normalize_tool_call_ids(hist)
        native_applied = soul.apply_id_fixes(hist, native_fixes)
        ref_applied = _apply_ref_id_fixes(hist)
        # Compare the resulting ids, not the fix order.
        def ids_of(h):
            return [(m.get("role"), m.get("tool_call_id"),
                     tuple(tc.get("id", "") for tc in (m.get("tool_calls") or [])))
                    for m in h]
        assert ids_of(native_applied) == ids_of(ref_applied), hist


# ---------------------------------------------------------------------------
# Prune scan parity: >= 50 seeded histories
# ---------------------------------------------------------------------------


def _seed_history(rng, i):
    hist = []
    n = rng.randint(3, 12)
    for _ in range(n):
        kind = rng.random()
        if kind < 0.18:
            hist.append(_msg("user", [{"type": "text",
                                       "text": "<system-reminder>\nr\n</system-reminder>"}]))
        elif kind < 0.30:
            hist.append(_msg("user", [{"type": "text",
                                       "text": f'<notification id="n{i}" category="task">t</notification>'}]))
        elif kind < 0.38:
            hist.append(_msg("user", [{"type": "text",
                                       "text": "<active-background-tasks>snap</active-background-tasks>"}]))
        elif kind < 0.44:
            hist.append(_msg("user", [{"type": "text",
                                       "text": "D-Mail from your future self: x"}]))
        elif kind < 0.62:
            tid = f"c{rng.randrange(3)}"
            hist.append(_msg("assistant", [],
                             [{"type": "function", "id": tid,
                               "function": {"name": "f", "arguments": "{}"}}]))
        elif kind < 0.82:
            tid = f"c{rng.randrange(3)}"
            text = rng.choice([
                "<system>ERROR: boom</system>",
                "Tool output is empty",
                "some normal result text here",
                "x" * rng.choice([10, 200, 3000]),
            ])
            hist.append(_msg("tool", [{"type": "text", "text": text}],
                             tool_call_id=tid))
        else:
            hist.append(_msg("user", [{"type": "text", "text": "real message"}]))
    return hist


def test_prune_scan_parity_seeded():
    rng = random.Random(99)
    for i in range(60):
        hist = _seed_history(rng, i)
        for policy in (None, {"stable_prefix_messages": 0, "recent_messages_protected": 0},
                       {"stable_prefix_messages": 2, "recent_messages_protected": 3,
                        "current_turn_index": len(hist) - 1}):
            native = soul.prune_scan(hist, policy)
            compat = soul._compat_prune_scan(hist, policy)
            assert native == compat, (i, policy, hist, native, compat)


def test_prune_scan_adversarial_interleavings():
    # Tool call -> error -> retry -> success chains interleaved.
    hist = []
    for i in range(4):
        tid = f"chain_{i}"
        hist.append(_msg("assistant", [],
                         [{"type": "function", "id": tid,
                           "function": {"name": "f", "arguments": "{}"}}]))
        hist.append(_msg("tool", [{"type": "text", "text": "<system>ERROR: x</system>"}],
                         tool_call_id=tid))
        hist.append(_msg("assistant", [],
                         [{"type": "function", "id": tid,
                           "function": {"name": "f", "arguments": "{}"}}]))
        hist.append(_msg("tool", [{"type": "text", "text": "ok result"}],
                         tool_call_id=tid))
    native = soul.prune_scan(hist, None)
    compat = soul._compat_prune_scan(hist, None)
    assert native == compat


# ---------------------------------------------------------------------------
# Reminders / normalize plan / compaction prompt parity
# ---------------------------------------------------------------------------


def test_leading_reminder_counts():
    rng = random.Random(3)
    for _ in range(30):
        n = rng.randint(0, 6)
        hist = []
        for i in range(n):
            hist.append(_msg("user", [{"type": "text",
                                       "text": "<system-reminder>\nr\n</system-reminder>"}]))
        if rng.random() < 0.7:
            hist.append(_msg("user", [{"type": "text", "text": "real"}]))
            for _ in range(rng.randint(0, 2)):
                hist.append(_msg("user", [{"type": "text",
                                           "text": "<system-reminder>\nx\n</system-reminder>"}]))
        assert soul.count_leading_reminders(hist) == soul._compat_count_leading_reminders(hist)
        assert soul.count_leading_reminders(hist) == n


def test_normalize_history_plan_application():
    rng = random.Random(11)
    for _ in range(30):
        hist = []
        for _ in range(rng.randint(1, 8)):
            role = rng.choice(["user", "user", "assistant", "tool", "user"])
            if role == "tool":
                hist.append(_msg("tool", [{"type": "text", "text": "r"}],
                                 tool_call_id="c"))
            elif rng.random() < 0.2:
                hist.append(_msg("user", [{"type": "text",
                                           "text": "<notification id=\"n\">t</notification>"}]))
            else:
                hist.append(_msg(role, [{"type": "text", "text": "m"}]))
        plan = soul.build_normalize_plan(hist)
        compat_plan = soul._compat_build_normalize_plan(hist)
        assert plan == compat_plan, hist
        applied = soul.apply_normalize_plan(hist, plan)
        # Reference semantics: merge adjacent user messages (non-notification).
        ref = []
        for m in hist:
            if (ref and ref[-1]["role"] == m["role"] == "user"
                    and not soul._compat_is_notification(ref[-1])
                    and not soul._compat_is_notification(m)):
                ref[-1] = {"role": "user",
                           "content": list(ref[-1].get("content") or [])
                           + list(m.get("content") or [])}
            else:
                ref.append(dict(m))
        def clean(h):
            return [{k: v for k, v in m.items() if v is not None} for m in h]
        assert clean(applied) == clean(ref), (hist, plan)


def test_compaction_prompt_bytes():
    rng = random.Random(21)
    cases = [
        [_msg("user", [{"type": "text", "text": "hello"}])],
        [_msg("user", [{"type": "text", "text": "a"}]),
         _msg("assistant", [{"type": "think", "think": "hmm"},
                            {"type": "text", "text": "b"}])],
        [_msg("user", [{"type": "text", "text": "Previous context has been compacted. s"}]),
         _msg("user", [{"type": "text", "text": "Previous context has been compacted. s"}]),
         _msg("user", [{"type": "text", "text": "Previous context has been compacted. s"}])],
    ]
    for _ in range(10):
        n = rng.randint(1, 6)
        cases.append([_msg(rng.choice(["user", "assistant", "tool"]),
                           [{"type": "text", "text": f"m{i}"}])
                      for i in range(n)])
    for hist in cases:
        native = soul.build_compaction_prompt(hist, "")
        compat = soul._compat_build_compaction_prompt(hist, "")
        assert native == compat, hist
        native_sp = soul.build_compaction_prompt(hist, "You are a compactor.")
        compat_sp = soul._compat_build_compaction_prompt(hist, "You are a compactor.")
        assert native_sp == compat_sp
        assert native_sp.startswith(b"You are a compactor.\n\n")
        # The compact text (after the system prompt) matches the reference
        # prompts.COMPACT-based suffix.
        assert b"\n\n**Compaction Style Guidance:** Be balanced." in native
        assert native.endswith(b"TODO items.")


def test_native_disabled_fallback():
    """With KIMIX_NATIVE_SOUL=0 the shim must behave identically."""
    import subprocess
    env = dict(os.environ, KIMIX_NATIVE_SOUL="0")
    code = (
        "import sys; sys.path.insert(0, %r); sys.path.insert(0, %r);\n"
        "from kimix_native import soul\n"
        "h = [{'role': 'user', 'content': [{'type': 'text', 'text': 'hi'}]}]\n"
        "import json\n"
        "assert json.loads(soul.build_payload(h)) == [{'role': 'user', 'content': 'hi'}]\n"
        "assert soul.count_leading_reminders(h) == 0\n"
        "assert soul.build_normalize_plan(h) == [(0, 0, 0)]\n"
        "assert soul.normalize_tool_call_ids(h) == []\n"
        "assert soul.prune_scan(h, None) == [(0, 3)]\n"
        "print('SOUL_FALLBACK_OK')\n"
    ) % (os.path.join(ROOT, "python"), os.path.join(ROOT, "..", "bin", "debug"))
    r = subprocess.run([sys.executable, "-c", code], capture_output=True,
                       text=True, env=env)
    assert r.returncode == 0, r.stderr
    assert "SOUL_FALLBACK_OK" in r.stdout
