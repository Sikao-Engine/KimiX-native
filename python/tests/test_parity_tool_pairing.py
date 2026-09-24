"""Live differential parity tests for the compaction preserve boundary.

``src/builtin_tools/compact_tool.cpp`` ports two things ``compact_tool.h`` used
to declare Python-owned:

* ``kimi_cli/soul/tool_pairing.py`` -- ``message_tool_call_delta`` /
  ``balanced_cut_indices`` / ``nearest_balanced_cut_before``, and
* the boundary math of ``SimpleCompaction.prepare``
  (``kimi_cli/soul/compaction.py``:711-772) including the Phase-6 primacy
  re-insertion and the re-cut that follows it.

Both are now implemented in C++ and exposed through
``runtime_py.builtin_tools.web``; this module drives them over the same corpus
the golden generator uses
(``scripts/gen_tool_pairing_goldens.py``) *and* over a fresh seeded fuzz, and
compares against the real reference implementation in the kimi-agent checkout.

What the C++ ``preserve_split`` expresses, and how it is derived from the
reference:

* ``compact``             -- ``prepare(...).compact_message is not None``
* ``preserve_start_index``-- the contiguous cut ``k`` with
  ``to_preserve == messages[k:]`` (or ``[messages[0]] + messages[k:]``)
* ``keep_first_message``  -- ``to_preserve[0] is messages[0]`` and the tail is
  not simply the whole history (Phase-6 re-insertion survived)
* ``unbalanced``          -- the reference raised ``ValueError`` out of
  ``prepare`` (an orphan tool result reached the balanced-cut fold).  The port
  refuses to compact instead of raising, because this build has exceptions
  disabled.
"""
from __future__ import annotations

import importlib.util
import os
import random
import sys
from pathlib import Path

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import _parity_ref as pr  # noqa: E402

runtime_py = pr.native()
WEB = runtime_py.builtin_tools.web

_REF_READY = pr.ref_available()

pytestmark = pytest.mark.skipif(
    not _REF_READY,
    reason="kimi-agent reference checkout not importable (set KIMI_AGENT_ROOT)",
)

_PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent


def _load_generator():
    """Import scripts/gen_tool_pairing_goldens.py by path (it is not a package)."""
    path = _PROJECT_ROOT / "scripts" / "gen_tool_pairing_goldens.py"
    spec = importlib.util.spec_from_file_location("_gen_tool_pairing_goldens", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


GEN = _load_generator()


def _tool_pairing():
    return pr.ref("kimi_cli.soul.tool_pairing")


def _compaction():
    return pr.ref("kimi_cli.soul.compaction")


def _kosong_message():
    return pr.ref("kosong.message")


def _dict(msg):
    """The C++ binding's message-dict shape for one generator ``Msg``."""
    content = []
    if msg.think:
        content.append({"type": "think", "text": msg.think})
    if msg.text:
        content.append({"type": "text", "text": msg.text})
    out = {"role": msg.role, "content": content}
    if msg.calls:
        out["tool_calls"] = [
            {"type": "function", "id": f"c{k}", "function": {"name": "T", "arguments": "{}"}}
            for k in range(msg.calls)
        ]
    return out


def _reference_history(spec):
    km = _kosong_message()
    return [GEN.to_kosong(m, km.Message, km.TextPart, km.ThinkPart, km.ToolCall) for m in spec]


def _reference_split(history, depth, *, adaptive=False):
    """``SimpleCompaction.prepare`` reduced to the C++ preserve_split shape.

    Returns ``(compact, preserve_start, keep_first, unbalanced)``.
    """
    comp = _compaction()
    if adaptive:
        compactor = comp.SimpleCompaction(
            max_preserved_messages=2,
            preserve_depth=lambda msgs: comp.adaptive_preserve_depth(
                msgs, min_preserved=1, max_preserved=2
            ),
        )
    else:
        compactor = comp.SimpleCompaction(max_preserved_messages=depth)
    try:
        result = compactor.prepare(history)
    except ValueError:
        return (False, 0, False, True)
    if result.compact_message is None:
        return (False, 0, False, False)
    keep = list(result.to_preserve)
    keep_first = bool(keep) and keep[0] is history[0] and len(keep) != len(history)
    if keep_first:
        keep = keep[1:]
    for k in range(len(history) + 1):
        if len(history) - k != len(keep):
            continue
        if all(a is b for a, b in zip(history[k:], keep)):
            return (True, k, keep_first, False)
    raise AssertionError("reference to_preserve is not one of the two shapes")


def _corpus():
    return GEN.corpus()


def _all_specs():
    specs = list(_corpus())
    # A fresh seeded fuzz so this module does not only re-check the golden corpus.
    rng = random.Random(0xC0FFEE)
    roles = ("user", "assistant", "tool")
    for variant in range(160):
        n = rng.randrange(0, 16)
        spec = []
        for i in range(n):
            role = roles[rng.randrange(3)]
            calls = rng.randrange(0, 3) if role == "assistant" else 0
            spec.append(GEN.Msg(role, f"m{i}", calls=calls))
        specs.append((f"fuzz_n{n}_v{variant}", spec))
    return specs


# ---------------------------------------------------------------------------
# message_tool_call_delta
# ---------------------------------------------------------------------------


def test_message_tool_call_delta_matches_reference():
    tp = _tool_pairing()
    checked = 0
    for name, spec in _all_specs():
        history = _reference_history(spec)
        for i, ref_msg in enumerate(history):
            want = tp.message_tool_call_delta(ref_msg)
            got = WEB.message_tool_call_delta(_dict(spec[i]))
            assert got == want, (name, i, want, got)
            checked += 1
    assert checked > 500


# ---------------------------------------------------------------------------
# balanced_cut_indices / nearest_balanced_cut_before
# ---------------------------------------------------------------------------


def test_balanced_cut_indices_matches_reference():
    tp = _tool_pairing()
    unbalanced_cases = 0
    for name, spec in _all_specs():
        history = _reference_history(spec)
        out = WEB.balanced_cut_indices([_dict(m) for m in spec])
        try:
            want = sorted(tp.balanced_cut_indices(history))
        except ValueError as exc:
            unbalanced_cases += 1
            assert out["unbalanced"] is True, name
            bad = int(str(exc).rsplit("at index", 1)[1].strip())
            assert out["unbalanced_index"] == bad, (name, bad)
            continue
        assert out["unbalanced"] is False, name
        assert list(out["cuts"]) == want, name
    assert unbalanced_cases > 0, "the corpus must contain unbalanced histories"


def test_nearest_balanced_cut_before_matches_reference():
    tp = _tool_pairing()
    for name, spec in _all_specs():
        history = _reference_history(spec)
        dicts = [_dict(m) for m in spec]
        for index in range(len(spec) + 1):
            out = WEB.nearest_balanced_cut_before(dicts, index)
            try:
                want = tp.nearest_balanced_cut_before(history, index)
            except ValueError:
                assert out["unbalanced"] is True, (name, index)
                continue
            assert out["unbalanced"] is False, (name, index)
            assert out["cut"] == want, (name, index, want, out["cut"])
        # Out-of-range indices are clamped exactly like the reference.
        for index in (-5, -1, len(spec) + 1, len(spec) + 99):
            out = WEB.nearest_balanced_cut_before(dicts, index)
            try:
                want = tp.nearest_balanced_cut_before(history, index)
            except ValueError:
                assert out["unbalanced"] is True, (name, index)
                continue
            assert out["unbalanced"] is False, (name, index)
            assert out["cut"] == want, (name, index, want, out["cut"])


# ---------------------------------------------------------------------------
# resolve_preserve_split
# ---------------------------------------------------------------------------

_DEPTHS = [1, 2, 3, 5, 10]


def test_resolve_preserve_split_matches_reference():
    typed = _compaction()
    for name, spec in _all_specs():
        history = _reference_history(spec)
        dicts = [_dict(m) for m in spec]
        for depth in _DEPTHS:
            for adaptive in (False, True):
                if adaptive:
                    effective = typed.adaptive_preserve_depth(
                        history, min_preserved=1, max_preserved=2
                    )
                else:
                    effective = depth
                want = _reference_split(history, effective, adaptive=adaptive)
                got = WEB.resolve_preserve_split(dicts, effective, True)
                assert got["unbalanced"] is want[3], (name, depth, adaptive)
                if want[3]:
                    assert got["compact"] is False, (name, depth, adaptive)
                    continue
                assert got["compact"] is want[0], (name, depth, adaptive)
                if want[0]:
                    assert got["preserve_start_index"] == want[1], (name, depth, adaptive)
                    assert got["keep_first_message"] is want[2], (name, depth, adaptive)


def test_resolve_preserve_split_balanced_cuts_flag_matches_reference():
    """``balanced_cuts=False`` is the raw preserve walk (no snap, no re-cut)."""
    comp = _compaction()
    typed = _compaction()
    checked = 0
    for name, spec in _all_specs():
        history = _reference_history(spec)
        dicts = [_dict(m) for m in spec]
        depth = typed.adaptive_preserve_depth(history, min_preserved=1, max_preserved=2)
        compactor = comp.SimpleCompaction(
            max_preserved_messages=2,
            preserve_depth=lambda msgs: typed.adaptive_preserve_depth(
                msgs, min_preserved=1, max_preserved=2
            ),
            balanced_cuts=False,
        )
        try:
            result = compactor.prepare(history)
        except ValueError:
            continue
        got = WEB.resolve_preserve_split(dicts, depth, False)
        if result.compact_message is None:
            assert got["compact"] is False, (name, depth)
            continue
        keep = list(result.to_preserve)
        keep_first = bool(keep) and keep[0] is history[0] and len(keep) != len(history)
        if keep_first:
            keep = keep[1:]
        k = next(
            k for k in range(len(history) + 1)
            if len(history) - k == len(keep) and all(a is b for a, b in zip(history[k:], keep))
        )
        assert got["compact"] is True, (name, depth)
        assert got["preserve_start_index"] == k, (name, depth, k)
        assert got["keep_first_message"] is keep_first, (name, depth)
        checked += 1
    assert checked > 50


def test_resolve_preserve_split_refuses_unbalanced_instead_of_raising():
    """The port cannot raise (exceptions are disabled), so it reports the refusal."""
    comp = _compaction()
    km = _kosong_message()
    history = [
        km.Message(role="user", content=[km.TextPart(text="u0")]),
        km.Message(role="tool", content=[km.TextPart(text="t0")], tool_call_id="x"),
        km.Message(role="user", content=[km.TextPart(text="u1")]),
    ]
    with pytest.raises(ValueError):
        comp.SimpleCompaction(max_preserved_messages=1).prepare(history)
    dicts = [
        {"role": "user", "content": [{"type": "text", "text": "u0"}]},
        {"role": "tool", "content": [{"type": "text", "text": "t0"}]},
        {"role": "user", "content": [{"type": "text", "text": "u1"}]},
    ]
    out = WEB.resolve_preserve_split(dicts, 1, True)
    assert out["unbalanced"] is True
    assert out["compact"] is False
    # With a depth the preserve walk cannot satisfy, the reference returns before
    # the pairing fold and never raises -- and neither does the port.
    ref_early = comp.SimpleCompaction(max_preserved_messages=9).prepare(history)
    assert ref_early.compact_message is None
    out2 = WEB.resolve_preserve_split(dicts, 9, True)
    assert out2["unbalanced"] is False
    assert out2["compact"] is False


def test_tool_call_parts_are_counted_from_content():
    """tool_pairing also counts streamed ToolCallPart entries in content.

    kosong no longer lets a ToolCallPart live in a real Message, so the reference
    cannot be driven through it; the port keeps the branch and counts a C++
    content part of type "tool_call" (the shape the golden generator documents).
    """
    tp = _tool_pairing()
    km = _kosong_message()
    # Sanity: the reference's own input shape is the persisted list.
    ref = km.Message(role="assistant", content=[km.TextPart(text="x")],
                     tool_calls=[km.ToolCall(id="c0",
                                             function=km.ToolCall.FunctionBody(name="T",
                                                                               arguments="{}"))])
    assert tp.message_tool_call_delta(ref) == 1
    got = WEB.message_tool_call_delta(
        {"role": "assistant",
         "content": [{"type": "tool_call", "text": ""}, {"type": "tool_call", "text": ""}],
         "tool_calls": [{"type": "function", "id": "c0",
                         "function": {"name": "T", "arguments": "{}"}}]})
    assert got == 3


def test_golden_file_is_in_sync_with_the_reference():
    """The committed .inc must equal what the generator produces right now."""
    out = GEN.OUT_PATH
    assert out.exists()
    hist, nearest, splits = GEN.evaluate(*GEN._import_reference())
    assert GEN._emit(hist, nearest, splits) == out.read_text(encoding="utf-8")
