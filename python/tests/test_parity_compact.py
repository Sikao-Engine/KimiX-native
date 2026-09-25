"""Differential parity tests for the compact builtin tool (kimix-base <-> kimi-agent).

``src/builtin_tools/compact_tool.cpp`` ports the pure decision / prompt-assembly
kernels of the kimi-agent compaction driver.  This module compares every kernel
that ``runtime_py.builtin_tools.web`` exposes for compact against the *original*
implementation in the kimi-agent checkout (``C:/dev/kimi-agent``, override with
``KIMI_AGENT_ROOT``):

* ``should_auto_compact`` -- ``kimi_cli/soul/compaction.py`` (FOCUS 1).  The
  reference suite (``kimi-cli/tests/core/test_simple_compaction.py``,
  ``test_context_pending_tokens.py``) is replayed verbatim, plus a boundary sweep
  and a seeded fuzz, plus the ``max_context_size`` 0 / negative windows.
* prompt assembly -- ``SimpleCompaction._build_prompt_text`` +
  ``_detect_cascade_depth`` (FOCUS 3): ``build_compaction_prompt`` /
  ``build_compact_message_text`` byte-for-byte over the real
  ``kimi_cli.prompts.COMPACT`` / ``COMPACT_CASCADE`` bodies, all four modes,
  ``avoid_cascade``, the decision section and custom instructions.
* slicing -- ``prepare_compaction_input`` vs Python list slicing, plus the
  flattened-message / prompt text against the reference's own assembly
  (FOCUS 2), and a byte-exact comparison against ``SimpleCompaction.prepare``
  wherever the reference split is contiguous.
* message helpers -- ``Message.extract_text`` / ThinkPart detection / role test.
* session-level integration -- repeated compactions over the histories from
  ``kimi-agent/tests/test_integration_compaction.py`` (20-turn session, cascade
  session), plus ``test_reference_split_shapes`` which pins the only two tail
  shapes ``prepare`` emits (a plain cut, or ``[history[0]] + history[k:]``).

Ownership boundary (a *documented* contract, not a bug):
``prepare_compaction_input`` keeps the narrow contract it was specified with --
it receives an already-balanced, contiguous ``preserve_start_index`` and never
re-inserts ``messages[0]``.
The *soul-level* boundary that ``SimpleCompaction.prepare`` computes -- the
balanced tool-pairing search (``kimi_cli/soul/tool_pairing.py``) plus the Phase-6
first-message re-insertion, which makes the reference split non-contiguous
(``[messages[0]] + messages[k:]``) -- is now ported as well:
``compact::resolve_preserve_split`` expresses both shapes through
``preserve_start_index`` + ``keep_first_message``, and
``python/tests/test_parity_tool_pairing.py`` drives it against the reference over
the golden corpus and a fresh fuzz.  ``test_prepare_phase6_reinsertion_is_python_owned``
keeps pinning the *tool* function's narrower contract on purpose;
``test_prepare_matches_reference_prepare_when_contiguous``
compares byte-for-byte on every input where the reference *is* a plain cut (the
re-cut branch), which is where the C++ contract is well defined.

Kernels that are not exposed to Python (``adaptive_preserve_depth``,
``detect_cascade_depth`` (reachable indirectly through ``cascade_depth``),
``compute_surface_fingerprint``, ``estimate_text_tokens`` /
``estimate_message_tokens``) are covered by the golden vectors in
``tests/unit/builtin_tools/test_compact_tool.cpp`` (target
``test_builtin_compact``); ``test_unexposed_kernel_goldens_are_reference_derived``
below re-derives every one of those vectors from the reference so the pasted
goldens cannot drift silently.

Import order is load-bearing: ``kimi_cli.native_loader`` inserts
``<kimi-agent>/bin`` (a *stale* released ``runtime_py.pyd``) first on
``sys.path``, so ``_parity_ref.native()`` imports this checkout's build first and
verifies ``runtime_py.__file__``.
"""

from __future__ import annotations

import os
import random
import sys

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


def _compact():
    return pr.ref("kimi_cli.soul.compaction")


def _prompts():
    return pr.ref("kimi_cli.prompts")


def _kosong_message():
    return pr.ref("kosong.message")


def _wire():
    return pr.ref("kimi_cli.wire.types")


# ---------------------------------------------------------------------------
# Message construction / canonicalisation
# ---------------------------------------------------------------------------

OTHER = "other"


def text(t):
    return ("text", t)


def think(t):
    return ("think", t)


def other():
    return ("image_url", None)


def msg(role, *parts, tool_calls=()):
    """A ``(spec, kosong.Message)`` pair; ``spec`` is (role, parts) only."""
    wire = _wire()
    km = _kosong_message()
    content = []
    for kind, value in parts:
        if kind == "text":
            content.append(wire.TextPart(text=value))
        elif kind == "think":
            content.append(wire.ThinkPart(think=value))
        else:
            content.append(
                wire.ImageURLPart(image_url=wire.ImageURLPart.ImageURL(url="http://x/y.png"))
            )
    calls = [
        km.ToolCall(id=cid, function=km.ToolCall.FunctionBody(name="bash", arguments="{}"))
        for cid in tool_calls
    ]
    spec = (role, tuple((k, "" if v is None else v) for k, v in parts))
    return spec, km.Message(role=role, content=content, tool_calls=calls or None)


def spec_to_dict(spec):
    """Serialise a spec the way the binding's ``parse_message`` reads it.

    The binding keeps an unknown part type verbatim (only the C++ ``Compact``
    Tool-class path collapses non-text/think parts to "other"); both are
    ignored by the assembly kernels, which is what these tests verify.
    """
    role, parts = spec
    content = []
    for kind, value in parts:
        if kind in ("text", "think"):
            content.append({"type": kind, "text": value})
        else:
            content.append({"type": kind})
    return {"role": role, "content": content}


def specs_to_dicts(specs):
    return [spec_to_dict(s) for s in specs]


def canon_cpp(message):
    """Canonicalise a message dict emitted by the C++ binding."""
    return (
        message["role"],
        tuple((p["type"], p.get("text", "")) for p in message["content"]),
    )


def canon_ref(message):
    """Canonicalise a kosong Message the same way (part ``type`` + text)."""
    wire = _wire()
    parts = []
    for part in message.content:
        if isinstance(part, wire.TextPart):
            parts.append(("text", part.text))
        elif isinstance(part, wire.ThinkPart):
            parts.append(("think", part.think))
        else:
            parts.append((getattr(part, "type", OTHER), ""))
    return (message.role, tuple(parts))


def spec_from_message(message):
    """Inverse of :func:`canon_ref`, back into the ``(role, parts)`` spec shape."""
    wire = _wire()
    parts = []
    for part in message.content:
        if isinstance(part, wire.TextPart):
            parts.append(("text", part.text))
        elif isinstance(part, wire.ThinkPart):
            parts.append(("think", part.think))
        else:
            parts.append((getattr(part, "type", OTHER), None))
    return (message.role, tuple(parts))


def canon_spec(spec):
    return (spec[0], tuple(spec[1]))


def ref_prompt_text(compact_message):
    """The instruction text ``prepare`` appended as the last TextPart."""
    wire = _wire()
    tail = [p for p in compact_message.content if isinstance(p, wire.TextPart)]
    return tail[-1].text


def flatten_compact_message(compact_message):
    """Byte-exact flattened text the legacy transport sends (all TextParts)."""
    wire = _wire()
    return "".join(p.text for p in compact_message.content if isinstance(p, wire.TextPart))


# ---------------------------------------------------------------------------
# Corpora
# ---------------------------------------------------------------------------


#: Histories taken from / inspired by kimi-agent's own compaction tests
#: (``kimi-cli/tests/core/test_simple_compaction.py``,
#: ``test_adaptive_preserve.py``) plus tool call/result shapes that exercise the
#: balanced-cut and Phase-6 branches.
HISTORIES = {
    "ref_not_enough_messages": [msg("user", text("Only one message"))],
    "ref_only_preserved": [
        msg("user", text("Latest question")),
        msg("assistant", text("Latest reply")),
    ],
    "ref_builds_compact_message": [
        msg("system", text("System note")),
        msg("user", text("Old question"), think("Hidden thoughts")),
        msg("assistant", text("Old answer")),
        msg("user", text("Latest question")),
        msg("assistant", text("Latest answer")),
    ],
    "ref_custom_instruction": [
        msg("user", text("Old question")),
        msg("assistant", text("Old answer")),
        msg("user", text("Latest question")),
        msg("assistant", text("Latest answer")),
    ],
    "adaptive_callable_depth": [
        msg("user", text("Old")),
        msg("assistant", text("Old reply")),
        msg("user", text("New")),
        msg("assistant", text("New reply")),
    ],
    "plain_qa6": [
        msg("user", text("u1")),
        msg("assistant", text("a1")),
        msg("user", text("u2")),
        msg("assistant", text("a2")),
        msg("user", text("u3")),
        msg("assistant", text("a3")),
    ],
    "tool_pair_middle": [
        msg("user", text("u1")),
        msg("assistant", text("thinking"), tool_calls=["c1"]),
        msg("tool", text("result1")),
        msg("user", text("u2")),
        msg("assistant", text("a2")),
    ],
    "tool_pair_last": [
        msg("user", text("u1")),
        msg("assistant", text("a1")),
        msg("user", text("u2")),
        msg("assistant", text("calling"), tool_calls=["c1"]),
        msg("tool", text("r1")),
    ],
    "two_tool_calls": [
        msg("user", text("u1")),
        msg("assistant", text("calls"), tool_calls=["c1", "c2"]),
        msg("tool", text("r1")),
        msg("tool", text("r2")),
        msg("user", text("u2")),
        msg("assistant", text("a2")),
    ],
    "with_image_part": [
        msg("user", text("u1"), other()),
        msg("assistant", text("a1")),
        msg("user", text("u2"), other()),
        msg("assistant", text("a2")),
    ],
    "unicode": [
        msg("user", text("中文问题")),
        msg("assistant", text("回答")),
        msg("user", text("q2")),
        msg("assistant", text("a2")),
    ],
    "think_tail": [
        msg("user", text("u1")),
        msg("assistant", text("a1")),
        msg("user", text("u2")),
        msg("assistant", think("reasoning only")),
    ],
    "only_system": [msg("system", text("s")), msg("system", text("s2"))],
    "tool_pairs_two_rounds": [
        msg("user", text("u1")),
        msg("assistant", text("a1")),
        msg("user", text("u2")),
        msg("assistant", text("call"), tool_calls=["c1"]),
        msg("tool", text("r1")),
        msg("user", text("u3")),
        msg("assistant", text("call2"), tool_calls=["c2"]),
        msg("tool", text("r2")),
    ],
    "tool_pairs_then_qa": [
        msg("user", text("u1")),
        msg("assistant", text("call"), tool_calls=["c1"]),
        msg("tool", text("r1")),
        msg("user", text("u2")),
        msg("assistant", text("a2")),
        msg("user", text("u3")),
        msg("assistant", text("a3")),
    ],
    "multi_tool_call_block": [
        msg("user", text("u1")),
        msg("assistant", text("calls"), tool_calls=["c1", "c2", "c3"]),
        msg("tool", text("r1")),
        msg("tool", text("r2")),
        msg("tool", text("r3")),
        msg("user", text("u2")),
        msg("assistant", text("a2")),
    ],
    "empty_history": [],
    "leading_system": [
        msg("system", text("sys")),
        msg("user", text("u1")),
        msg("assistant", text("a1")),
        msg("user", text("u2")),
        msg("assistant", text("a2")),
    ],
}


def _history(name):
    return [m for _, m in HISTORIES[name]]


def _specs(name):
    return [s for s, _ in HISTORIES[name]]


CASCADE_MARKER = "Previous context has been compacted"

PROMPT_CORPUS = {
    "empty": [],
    "plain": [msg("user", text("hello"))],
    "one_marker": [msg("assistant", text(CASCADE_MARKER))],
    "two_markers": [
        msg("assistant", text(CASCADE_MARKER)),
        msg("user", text(CASCADE_MARKER)),
    ],
    "three_markers": [msg("assistant", text(CASCADE_MARKER))] * 3,
    "marker_double_in_one_part": [msg("assistant", text(CASCADE_MARKER + " and " + CASCADE_MARKER))],
    "marker_in_think_only": [msg("assistant", think(CASCADE_MARKER))],
    "mixed_roles": [
        msg("system", text("sys")),
        msg("user", text("u"), think("t")),
        msg("assistant", text(CASCADE_MARKER)),
        msg("tool", text("result")),
    ],
    "unicode": [msg("user", text("保留中文 custom")), msg("assistant", text("reply"))],
    "image_part": [
        msg("user", text("look at this"), other()),
        msg("assistant", text("got it")),
        msg("user", other(), text("and this")),
    ],
}

MODES = ["balanced", "aggressive", "retentive", "technical"]


# ---------------------------------------------------------------------------
# Basic wiring
# ---------------------------------------------------------------------------


def test_native_is_this_checkout_build():
    """The loaded extension is this checkout's build, in any build mode.

    ``pr.native()`` already pinned ``runtime_py`` to ``pr.BIN_DIR``; assert the
    location again here *without* naming a mode (``bin/debug`` was a worktree
    assumption; the release build is what ``bootstrap.py`` produces by default)
    and without naming the artifact (``.pyd`` on Windows, ``.so`` elsewhere).
    """
    assert pr.BIN_DIR is not None and pr.BIN_DIR.parent.name == "bin", pr.BIN_DIR
    assert os.path.normcase(str(pr.BIN_DIR)) == os.path.normcase(
        os.path.dirname(runtime_py.__file__)
    ), runtime_py.__file__


# ---------------------------------------------------------------------------
# FOCUS 1 -- should_auto_compact
# ---------------------------------------------------------------------------


#: (token_count, max_context_size, kwargs) -- the reference suite's own vectors
#: (kimi-cli/tests/core/test_simple_compaction.py::TestShouldAutoCompact,
#: tests/core/test_context_pending_tokens.py).
REFERENCE_TRIGGER_CASES = [
    (150_000, 200_000, dict(trigger_ratio=0.85, reserved_context_size=50_000), True),
    (149_999, 200_000, dict(trigger_ratio=0.85, reserved_context_size=50_000), False),
    (170_000, 200_000, dict(trigger_ratio=0.85, reserved_context_size=50_000), True),
    (140_000, 200_000, dict(trigger_ratio=0.85, reserved_context_size=50_000), False),
    (850_000, 1_000_000, dict(trigger_ratio=0.85, reserved_context_size=50_000), True),
    (840_000, 1_000_000, dict(trigger_ratio=0.85, reserved_context_size=50_000), False),
    (140_000, 200_000, dict(trigger_ratio=0.7, reserved_context_size=50_000), True),
    (139_999, 200_000, dict(trigger_ratio=0.7, reserved_context_size=50_000), False),
    (0, 200_000, dict(trigger_ratio=0.85, reserved_context_size=50_000), False),
    # kimi-agent tests/test_integration_compaction.py
    (100_000, 1_000_000, dict(trigger_ratio=0.85, reserved_context_size=50_000), False),
    (125_000, 200_000, dict(trigger_ratio=0.99, reserved_context_size=75_000,
                            safety_margin_tokens=4096), True),
    (125_001, 200_000, dict(trigger_ratio=0.99, reserved_context_size=75_000,
                            safety_margin_tokens=4096), True),
    (124_999, 200_000, dict(trigger_ratio=0.99, reserved_context_size=75_000,
                            safety_margin_tokens=4096), False),
    (660_480, 1_048_576, dict(trigger_ratio=0.85, reserved_context_size=75_000,
                              max_tokens=384_000, safety_margin_tokens=4096), True),
    (660_481, 1_048_576, dict(trigger_ratio=0.85, reserved_context_size=75_000,
                              max_tokens=384_000, safety_margin_tokens=4096), True),
    (660_479, 1_048_576, dict(trigger_ratio=0.85, reserved_context_size=75_000,
                              max_tokens=384_000, safety_margin_tokens=4096), False),
    (100_000, 200_000, dict(trigger_ratio=0.85, reserved_context_size=75_000, max_tokens=50_000,
                            tool_call_buffer_tokens=100_000, safety_margin_tokens=4096), True),
    (99_999, 200_000, dict(trigger_ratio=0.85, reserved_context_size=75_000, max_tokens=50_000,
                           tool_call_buffer_tokens=100_000, safety_margin_tokens=4096), False),
]


def _should_auto_compact_cpp(token_count, max_context_size, kwargs):
    cfg = dict(kwargs)
    cfg["max_context_size"] = max_context_size
    return WEB.should_auto_compact(token_count, cfg)


def _should_auto_compact_ref(token_count, max_context_size, kwargs):
    return _compact().should_auto_compact(token_count, max_context_size, **kwargs)


@pytest.mark.parametrize("token_count,max_context_size,kwargs,expected",
                         REFERENCE_TRIGGER_CASES,
                         ids=[f"{c[0]}-{c[1]}" for c in REFERENCE_TRIGGER_CASES])
def test_should_auto_compact_reference_suite(token_count, max_context_size, kwargs, expected):
    ref = _should_auto_compact_ref(token_count, max_context_size, kwargs)
    assert ref is expected, "reference vector changed"
    assert _should_auto_compact_cpp(token_count, max_context_size, kwargs) == ref


def test_should_auto_compact_zero_or_negative_window():
    """A zero/negative ``max_context_size`` has no early-out in the reference.

    ``token_count >= max_context_size * trigger_ratio`` is trivially true for a
    non-negative count (regression: the port used to return False).
    """
    for max_context_size in (0, -1, -100_000):
        for token_count in (-5, -1, 0, 1, 1000, 10**6):
            kwargs = dict(trigger_ratio=0.75, reserved_context_size=8192)
            ref = _should_auto_compact_ref(token_count, max_context_size, kwargs)
            assert _should_auto_compact_cpp(token_count, max_context_size, kwargs) == ref, (
                token_count, max_context_size)


def test_should_auto_compact_none_max_tokens_is_zero():
    """``max_tokens=None`` is treated as 0 (and the binding must accept it)."""
    kwargs = dict(trigger_ratio=0.85, reserved_context_size=50_000, max_tokens=None)
    for token_count in (139_999, 150_000, 200_000):
        ref = _should_auto_compact_ref(token_count, 200_000, kwargs)
        assert _should_auto_compact_cpp(token_count, 200_000, kwargs) == ref


def test_should_auto_compact_boundary_sweep():
    """Sweep +-2 tokens around both thresholds for a grid of configurations."""
    mismatches = []
    for max_context_size in (8192, 32_768, 64_000, 128_000, 200_000, 1_000_000, 1_048_576):
        for trigger_ratio in (0.0, 0.5, 0.7, 0.75, 0.85, 0.99, 1.0):
            for reserved in (0, 1024, 8192, 50_000, 75_000):
                for max_tokens in (0, 50_000, 384_000):
                    for tool_buffer in (0, 20_000, 100_000):
                        kwargs = dict(trigger_ratio=trigger_ratio,
                                      reserved_context_size=reserved,
                                      max_tokens=max_tokens,
                                      tool_call_buffer_tokens=tool_buffer,
                                      safety_margin_tokens=1024)
                        output_size = max_tokens + 1024
                        reservation = max(tool_buffer, reserved, output_size)
                        effective = min(reservation, max(0, max_context_size - reserved))
                        boundary = max_context_size - effective
                        base = max_context_size * trigger_ratio
                        for token_count in {int(base) - 2, int(base) - 1, int(base),
                                            int(base) + 1, int(base) + 2,
                                            boundary - 2, boundary - 1, boundary,
                                            boundary + 1, boundary + 2,
                                            0, max_context_size}:
                            if token_count < 0:
                                continue
                            ref = _should_auto_compact_ref(token_count, max_context_size, kwargs)
                            got = _should_auto_compact_cpp(token_count, max_context_size, kwargs)
                            if ref != got:
                                mismatches.append((token_count, max_context_size, kwargs, got, ref))
    assert not mismatches, mismatches[:5]


def test_should_auto_compact_fuzz():
    rnd = random.Random(0xC0FFEE)
    mismatches = []
    for _ in range(20_000):
        max_context_size = rnd.choice([1, 2, 3, 4096, 32_768, 128_000, 200_000, 1_048_576,
                                       0, -1, rnd.randint(1, 500_000)])
        token_count = rnd.choice([0, 1, max_context_size, max_context_size - 1,
                                  max_context_size + 1, rnd.randint(-10, max_context_size + 1000),
                                  rnd.randint(-10, 2 * max(1, max_context_size))])
        kwargs = dict(
            trigger_ratio=rnd.choice([0.0, 0.5, 0.75, 0.85, 0.99, 1.0, 1.5, -0.5, rnd.random()]),
            reserved_context_size=rnd.choice([0, 1, 8192, 50_000, 75_000,
                                              rnd.randint(0, max(1, max_context_size))]),
            max_tokens=rnd.choice([0, 1, 50_000, 384_000, rnd.randint(0, max(1, max_context_size))]),
            tool_call_buffer_tokens=rnd.choice([0, 1, 20_000, 100_000,
                                                rnd.randint(0, max(1, max_context_size))]),
            safety_margin_tokens=rnd.choice([0, 1, 1024, 4096, rnd.randint(0, 10_000)]),
        )
        ref = _should_auto_compact_ref(token_count, max_context_size, kwargs)
        got = _should_auto_compact_cpp(token_count, max_context_size, kwargs)
        if ref != got:
            mismatches.append((token_count, max_context_size, kwargs, got, ref))
    assert not mismatches, mismatches[:5]


def test_should_auto_compact_default_cfg_matches_reference_defaults():
    """No cfg keys at all: the binding defaults must equal the reference defaults."""
    for token_count in (0, 95_999, 96_000, 128_000):
        ref = _compact().should_auto_compact(
            token_count, 128_000, trigger_ratio=0.75, reserved_context_size=8192)
        assert WEB.should_auto_compact(token_count, {}) == ref


# ---------------------------------------------------------------------------
# Message helpers
# ---------------------------------------------------------------------------


def test_extract_text_parity():
    cases = [
        [text("a"), text("b")],
        [text("a"), think("T"), text("b")],
        [think("only")],
        [],
        [text(""), text("x")],
        [text("中文"), text("ascii")],
    ]
    for parts in cases:
        for sep in ("", " ", "\n", "|"):
            spec = ("user", tuple(parts))
            ref = _kosong_message().Message(role="user", content=[
                _wire().TextPart(text=v) if k == "text" else _wire().ThinkPart(think=v)
                for k, v in parts
            ]).extract_text(sep)
            assert WEB.extract_text(spec_to_dict(spec), sep) == ref, (parts, sep)


def test_has_think_part_parity():
    cases = [[text("a")], [think("t")], [text("a"), think("t")], [], [other()]]
    for parts in cases:
        spec = ("assistant", tuple(parts))
        assert WEB.has_think_part(spec_to_dict(spec)) is any(k == "think" for k, _ in parts)


def test_is_user_or_assistant_parity():
    for role in ("user", "assistant", "system", "tool", "", "User"):
        spec = (role, (text("x"),))
        assert WEB.is_user_or_assistant(spec_to_dict(spec)) is (role in {"user", "assistant"})


# ---------------------------------------------------------------------------
# FOCUS 3 -- prompt assembly
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("corpus_name", sorted(PROMPT_CORPUS))
@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("avoid_cascade", [False, True])
@pytest.mark.parametrize("decision_section", [False, True])
def test_build_compaction_prompt_parity(corpus_name, mode, avoid_cascade, decision_section):
    prompts = _prompts()
    comp = _compact()
    specs = [s for s, _ in PROMPT_CORPUS[corpus_name]]
    ref_messages = [m for _, m in PROMPT_CORPUS[corpus_name]]
    options = {"avoid_cascade": avoid_cascade, "mode": mode,
               "decision_section_enabled": decision_section}
    for custom in ("", "Keep every file path.", "line1\nline2", "保留中文"):
        out = WEB.build_compaction_prompt(specs_to_dicts(specs), options, custom,
                                          prompts.COMPACT, prompts.COMPACT_CASCADE)
        compactor = comp.SimpleCompaction(decision_section_enabled=decision_section)
        expected_text, expected_depth = compactor._build_prompt_text(
            ref_messages,
            comp.CompactionOptions(avoid_cascade=avoid_cascade, mode=comp.CompactMode(mode)),
            custom,
        )
        assert out["prompt_text"] == expected_text
        assert out["cascade_depth"] == expected_depth


def test_build_compaction_prompt_cascade_selection():
    """avoid_cascade wins over the >= 3 cascade depth threshold."""
    prompts = _prompts()
    specs = specs_to_dicts([s for s, _ in PROMPT_CORPUS["three_markers"]])
    without = WEB.build_compaction_prompt(specs, {"avoid_cascade": False},
                                          "", prompts.COMPACT, prompts.COMPACT_CASCADE)
    with_avoid = WEB.build_compaction_prompt(specs, {"avoid_cascade": True},
                                             "", prompts.COMPACT, prompts.COMPACT_CASCADE)
    assert without["cascade_depth"] == 3
    assert without["prompt_text"].startswith("\n" + prompts.COMPACT_CASCADE)
    assert with_avoid["prompt_text"].startswith("\n" + prompts.COMPACT)


@pytest.mark.parametrize("corpus_name", sorted(PROMPT_CORPUS))
def test_build_compact_message_text_parity(corpus_name):
    """The flattened legacy transport text, byte-for-byte."""
    comp = _compact()
    prompts = _prompts()
    specs = [s for s, _ in PROMPT_CORPUS[corpus_name]]
    ref_messages = [m for _, m in PROMPT_CORPUS[corpus_name]]
    compactor = comp.SimpleCompaction()
    region = list(ref_messages)
    expected_flat, expected_prompt, _ = _reference_assembly(
        compactor, region, comp.CompactionOptions(), "keep it")
    assert WEB.build_compact_message_text(specs_to_dicts(specs), expected_prompt) == expected_flat
    # ... and through the prepare entry point.
    if region:
        out = WEB.prepare_compaction_input(specs_to_dicts(specs), len(region),
                                           {"mode": "balanced"}, "keep it",
                                           prompts.COMPACT, prompts.COMPACT_CASCADE)
        assert out["compact_message_text"] == expected_flat
        assert out["prompt_text"] == expected_prompt


def _reference_assembly(compactor, region, options, custom_instruction):
    """Mirror of ``SimpleCompaction.prepare`` lines 775-786 for a given region."""
    wire = _wire()
    parts = []
    for i, message in enumerate(region):
        parts.append(f"## Message {i + 1}\nRole: {message.role}\nContent:\n")
        parts.extend(p.text for p in message.content if isinstance(p, wire.TextPart))
    prompt_text, cascade_depth = compactor._build_prompt_text(region, options, custom_instruction)
    parts.append(prompt_text)
    return "".join(parts), prompt_text, cascade_depth


# ---------------------------------------------------------------------------
# FOCUS 2 -- prepare_compaction_input (slicing + assembly)
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("corpus_name", sorted(HISTORIES))
def test_prepare_compaction_input_slice_parity(corpus_name):
    """The C++ contract: a contiguous cut at an already-balanced index.

    ``preserve_start_index`` is clamped to ``[0, len]`` exactly like Python list
    slicing (the reference's ``history[:i] / history[i:]``); index 0 yields the
    "nothing to compact" signal.
    """
    specs = _specs(corpus_name)
    dicts = specs_to_dicts(specs)
    prompts = _prompts()
    for index in range(0, len(specs) + 3):
        clamped = min(index, len(specs))
        if clamped == 0:
            with pytest.raises(RuntimeError):
                WEB.prepare_compaction_input(dicts, index)
            continue
        out = WEB.prepare_compaction_input(dicts, index, {}, "", prompts.COMPACT,
                                           prompts.COMPACT_CASCADE)
        assert [canon_cpp(m) for m in out["to_compact"]] == [
            canon_spec(s) for s in specs[:clamped]], (corpus_name, index)
        assert [canon_cpp(m) for m in out["to_preserve"]] == [
            canon_spec(s) for s in specs[clamped:]], (corpus_name, index)


@pytest.mark.parametrize("corpus_name", sorted(HISTORIES))
@pytest.mark.parametrize("mode", MODES)
def test_prepare_compaction_input_text_assembly_parity(corpus_name, mode):
    """Legacy flattened text + prompt for the region the C++ was given."""
    prompts = _prompts()
    comp = _compact()
    specs = _specs(corpus_name)
    dicts = specs_to_dicts(specs)
    ref_messages = _history(corpus_name)
    compactor = comp.SimpleCompaction(decision_section_enabled=True)
    options_dict = {"mode": mode, "decision_section_enabled": True}
    ref_options = comp.CompactionOptions(mode=comp.CompactMode(mode))
    for index in range(1, len(specs) + 3):
        clamped = min(index, len(specs))
        if clamped == 0:
            continue
        custom = "keep the rust code" if clamped % 2 else ""
        expected_flat, expected_prompt, expected_depth = _reference_assembly(
            compactor, ref_messages[:clamped], ref_options, custom)
        out = WEB.prepare_compaction_input(dicts, index, options_dict, custom,
                                           prompts.COMPACT, prompts.COMPACT_CASCADE)
        assert out["compact_message_text"] == expected_flat, (corpus_name, index)
        assert out["prompt_text"] == expected_prompt, (corpus_name, index)
        assert out["cascade_depth"] == expected_depth, (corpus_name, index)


#: Constructors covering the reference's own prepare() variants.  The special
#: label "adaptive" installs ``preserve_depth=adaptive_preserve_depth`` (the
#: callable form kimi-agent's KimiSoul configures).
_COMPACTORS = [
    ("max1", dict(max_preserved_messages=1)),
    ("max2", dict(max_preserved_messages=2)),
    ("max3", dict(max_preserved_messages=3)),
    ("depth1", dict(max_preserved_messages=2, preserve_depth=1)),
    ("depth2", dict(max_preserved_messages=1, preserve_depth=2)),
    ("depth3", dict(max_preserved_messages=1, preserve_depth=3)),
    ("decision", dict(max_preserved_messages=2, decision_section_enabled=True)),
    ("unbalanced", dict(max_preserved_messages=2, balanced_cuts=False)),
    ("adaptive", dict(max_preserved_messages=2)),
]


def _options_dict(kwargs, mode):
    """The binding's ``options`` dict for the reference compactor's configuration."""
    return {
        "mode": mode,
        "decision_section_enabled": bool(kwargs.get("decision_section_enabled", False)),
    }


def _comparison_plan():
    """Yield (corpus, compactor name/kwargs, options, custom) reference plans.

    Returns ``None`` for entries whose reference split is *not* a plain
    contiguous cut (Phase-6 first-message re-insertion); see the module
    docstring.
    """
    comp = _compact()
    for corpus_name in sorted(HISTORIES):
        for label, kwargs in _COMPACTORS:
            for mode in MODES:
                for custom in ("", "keep paths"):
                    yield corpus_name, label, kwargs, mode, custom


def test_prepare_matches_reference_prepare_when_contiguous():
    """Byte-exact vs ``SimpleCompaction.prepare`` wherever the split is a cut."""
    prompts = _prompts()
    comp = _compact()
    compared = 0
    non_contiguous = 0
    skipped_no_compaction = 0
    for corpus_name, label, kwargs, mode, custom in _comparison_plan():
        specs = _specs(corpus_name)
        ref_messages = _history(corpus_name)
        options = comp.CompactionOptions(mode=comp.CompactMode(mode))
        if label == "adaptive":
            compactor = comp.SimpleCompaction(preserve_depth=comp.adaptive_preserve_depth, **kwargs)
        else:
            compactor = comp.SimpleCompaction(**kwargs)
        result = compactor.prepare(ref_messages, custom_instruction=custom,
                                   options=options, aligned_system_prompt="SYS")
        if result.compact_message is None:
            skipped_no_compaction += 1
            with pytest.raises(RuntimeError):
                WEB.prepare_compaction_input(specs_to_dicts(specs), 0, _options_dict(kwargs, mode),
                                             custom, prompts.COMPACT, prompts.COMPACT_CASCADE)
            continue
        region = list(result.summarization_input.messages)
        tail = list(result.to_preserve)
        joined = [canon_ref(m) for m in region + tail]
        if joined != [canon_ref(m) for m in ref_messages]:
            # Phase-6 re-insertion: to_preserve == [messages[0]] + messages[k:].
            non_contiguous += 1
            continue
        index = len(region)
        out = WEB.prepare_compaction_input(specs_to_dicts(specs), index, _options_dict(kwargs, mode),
                                           custom, prompts.COMPACT, prompts.COMPACT_CASCADE)
        assert [canon_cpp(m) for m in out["to_compact"]] == [canon_ref(m) for m in region]
        assert [canon_cpp(m) for m in out["to_preserve"]] == [canon_ref(m) for m in tail]
        assert out["compact_message_text"] == flatten_compact_message(result.compact_message)
        assert out["prompt_text"] == ref_prompt_text(result.compact_message)
        assert out["cascade_depth"] == result.cascade_depth
        compared += 1
    assert compared > 0, "no contiguous reference plans were exercised"
    assert non_contiguous > 0, "Phase-6 branch never hit - corpus lost its coverage"
    assert skipped_no_compaction > 0
    print(f"compared={compared} phase6_non_contiguous={non_contiguous} "
          f"no_compaction={skipped_no_compaction}")


def test_prepare_phase6_reinsertion_is_python_owned():
    """Pin the documented ownership boundary (compact_tool.h lines 32-35).

    ``SimpleCompaction.prepare`` always keeps ``messages[0]`` verbatim, so for
    every non-degenerate input the reference ``to_preserve`` is
    ``[messages[0]] + messages[k:]`` -- not a contiguous cut, hence not
    expressible through the C++ single-index API.  The C++ kernel is asserted to
    implement the contiguous cut that Python is expected to derive from it.
    """
    comp = _compact()
    prompts = _prompts()
    name = "ref_builds_compact_message"
    ref_messages = _history(name)
    specs = _specs(name)
    compactor = comp.SimpleCompaction(max_preserved_messages=2)
    result = compactor.prepare(ref_messages, aligned_system_prompt="SYS")
    region = list(result.summarization_input.messages)
    tail = list(result.to_preserve)
    assert canon_ref(tail[0]) == canon_ref(ref_messages[0]), "expected Phase-6 re-insertion"
    assert canon_ref(region[0]) != canon_ref(ref_messages[0])
    # C++ gets the pre-Phase-6 balanced index (3 for this history) and returns the
    # plain cut; Python's extra step is the first-message re-insertion.
    out = WEB.prepare_compaction_input(specs_to_dicts(specs), 3, {}, "",
                                       prompts.COMPACT, prompts.COMPACT_CASCADE)
    assert [canon_cpp(m) for m in out["to_preserve"]] == [canon_spec(s) for s in specs[3:]]
    assert [canon_cpp(m) for m in out["to_compact"]] == [canon_spec(s) for s in specs[:3]]
    assert [canon_ref(m) for m in tail] != [canon_cpp(m) for m in out["to_preserve"]]


def test_prepare_balanced_cut_helpers_are_reference_side():
    """``balanced_cut_indices`` / ``nearest_balanced_cut_before`` stay in Python.

    The C++ slice must never be handed an index that splits a tool call from its
    result; this test derives the safe indices from the reference helper and
    checks the C++ honours every one of them (i.e. the tail never starts with an
    orphan tool result for the reference-derived index).
    """
    pairing = pr.ref("kimi_cli.soul.tool_pairing")
    prompts = _prompts()
    for name in ("tool_pair_middle", "tool_pair_last", "two_tool_calls",
                 "tool_pairs_two_rounds", "tool_pairs_then_qa", "multi_tool_call_block"):
        ref_messages = _history(name)
        specs = _specs(name)
        cuts = sorted(pairing.balanced_cut_indices(ref_messages))
        assert cuts, name
        for cut in cuts:
            if cut == 0:
                with pytest.raises(RuntimeError):
                    WEB.prepare_compaction_input(specs_to_dicts(specs), cut)
                continue
            out = WEB.prepare_compaction_input(specs_to_dicts(specs), cut, {}, "",
                                               prompts.COMPACT, prompts.COMPACT_CASCADE)
            assert [canon_cpp(m) for m in out["to_compact"]] == [
                canon_spec(s) for s in specs[:cut]]
            # A balanced cut never leaves an orphan tool result at the head of
            # the preserved tail.
            if out["to_preserve"]:
                assert out["to_preserve"][0]["role"] != "tool", (name, cut)


# ---------------------------------------------------------------------------
# Session-level integration (kimi-agent tests/test_integration_compaction.py)
# ---------------------------------------------------------------------------


def _session_messages(n_turns):
    """Mirror of kimi-agent tests/test_integration_compaction.py::_make_messages."""
    out = [msg("user", text("Original task: refactor auth module"))]
    for i in range(1, n_turns):
        if i % 2 == 1:
            out.append(msg("assistant", text(f"Step {i} done")))
        else:
            out.append(msg("user", text(f"Request {i}")))
    return out


def _cascade_session_messages():
    """The integration test's cascade shape: 5 summaries followed by 10 turns."""
    out = [msg("user", text(f"Previous context has been compacted. {letter}")) for letter in "ABCDE"]
    out.extend(_session_messages(10))
    return out


SESSION_HISTORIES = {
    "session_20_turns": _session_messages(20),
    "session_10_turns": _session_messages(10),
    "cascade_session": _cascade_session_messages(),
}


def _split_shape(history, to_preserve):
    """Classify the reference tail against the two shapes ``prepare`` can emit.

    Returns ``(kind, k)`` with kind in {"cut", "first_plus_tail"} and
    ``k = len(history) - len(to_compact)``; "other" means the reference did
    something the C++ single-index contract cannot express.
    """
    keep = [canon_ref(m) for m in to_preserve]
    whole = [canon_ref(m) for m in history]
    n = len(keep)
    if n == 0:
        return "cut", len(whole)
    if keep == whole[len(whole) - n:]:
        return "cut", len(whole) - n
    if keep[0] == whole[0] and keep[1:] == whole[len(whole) - (n - 1):]:
        return "first_plus_tail", len(whole) - (n - 1)
    return "other", None


def _prepare_for(comp, history, label, kwargs, options, custom=""):
    if label == "adaptive":
        compactor = comp.SimpleCompaction(preserve_depth=comp.adaptive_preserve_depth, **kwargs)
    else:
        compactor = comp.SimpleCompaction(**kwargs)
    return compactor, compactor.prepare(history, custom_instruction=custom, options=options,
                                        aligned_system_prompt="SYS")


def test_reference_split_shapes():
    """Pin the two tail shapes ``prepare`` produces (the Phase-6 rules).

    * ``cut`` -- a plain contiguous split ``history[:k] / history[k:]`` (the
      shape the C++ single-index API models; reached whenever the Phase-6
      first-message re-insertion is undone by the balanced re-cut).
    * ``first_plus_tail`` -- ``[history[0]] + history[k:]`` with
      ``to_compact = history[1:k]`` (the un-ported Phase-6 step: the very first
      message is always preserved verbatim, which is *not* contiguous).
    """
    comp = _compact()
    shapes = {"cut": 0, "first_plus_tail": 0, "no_compaction": 0}
    for name, pairs in {**HISTORIES, **SESSION_HISTORIES}.items():
        history = [m for _, m in pairs]
        for label, kwargs in _COMPACTORS:
            for mode in MODES:
                options = comp.CompactionOptions(mode=comp.CompactMode(mode))
                compactor, result = _prepare_for(comp, history, label, kwargs, options)
                if result.compact_message is None:
                    shapes["no_compaction"] += 1
                    continue
                kind, k = _split_shape(history, list(result.to_preserve))
                assert kind != "other", (name, label, mode)
                region = [canon_ref(m) for m in list(result.summarization_input.messages)]
                if kind == "cut":
                    assert region == [canon_ref(m) for m in history[:k]], (name, label, mode)
                else:
                    assert region == [canon_ref(m) for m in history[1:k]], (name, label, mode)
                shapes[kind] += 1
    assert shapes["cut"] > 0, shapes
    assert shapes["first_plus_tail"] > 0, shapes
    assert shapes["no_compaction"] > 0, shapes
    print(f"split shapes: {shapes}")


def test_session_simulation_assembly_parity():
    """Repeated compactions over simulated sessions, compared to the reference.

    At every round the C++ slice must equal Python slicing, and the assembled
    flattened text + prompt must equal the reference's own assembly for the same
    region (``history[:k]``).  The history is then advanced exactly the way
    ``SimpleCompaction.compact`` does it (summary message + preserved tail) so
    the later rounds see realistic cascade shapes.
    """
    comp = _compact()
    prompts = _prompts()
    km = _kosong_message()
    wire = _wire()
    rounds_total = 0
    for name, pairs in SESSION_HISTORIES.items():
        history = [m for _, m in pairs]
        for _ in range(4):
            specs = [spec_from_message(m) for m in history]
            dicts = specs_to_dicts(specs)
            compactor, result = _prepare_for(comp, history, "max2",
                                             dict(max_preserved_messages=2),
                                             comp.CompactionOptions())
            if result.compact_message is None:
                break
            kind, k = _split_shape(history, list(result.to_preserve))
            assert kind != "other", (name, k)
            assert k >= 1
            out = WEB.prepare_compaction_input(dicts, k, {}, "", prompts.COMPACT,
                                               prompts.COMPACT_CASCADE)
            assert [canon_cpp(m) for m in out["to_compact"]] == [canon_ref(m) for m in history[:k]]
            assert [canon_cpp(m) for m in out["to_preserve"]] == [canon_ref(m) for m in history[k:]]
            expected_flat, expected_prompt, expected_depth = _reference_assembly(
                compactor, history[:k], comp.CompactionOptions(), "")
            assert out["compact_message_text"] == expected_flat, (name, k)
            assert out["prompt_text"] == expected_prompt, (name, k)
            assert out["cascade_depth"] == expected_depth, (name, k)
            rounds_total += 1
            summary = km.Message(role="user", content=[wire.TextPart(
                text="Previous context has been compacted. Here is the compaction output:\n\n"
                     "<summary of earlier turns>")])
            history = [summary] + list(result.to_preserve)
    assert rounds_total >= 6, rounds_total


# ---------------------------------------------------------------------------
# Error surface (FOCUS 3)
# ---------------------------------------------------------------------------


def test_no_change_signal_is_a_translated_runtime_error():
    """The "nothing to compact" tool_error must reach Python as RuntimeError.

    Regression: ``throw_tool_error`` (src/runtime/py/py_builtin_web.cpp) threw a
    bare ``std::runtime_error``, which this build's pybind11 3.0.2 default
    translator does not translate -- Python saw ``SystemError: Exception escaped
    from default exception translator!`` with the message lost.
    """
    specs = specs_to_dicts(_specs("ref_builds_compact_message"))
    with pytest.raises(RuntimeError) as excinfo:
        WEB.prepare_compaction_input(specs, 0)
    assert "no messages to compact" in str(excinfo.value)
    assert type(excinfo.value).__name__ == "RuntimeError"


def test_prepare_out_of_range_index_is_clamped_like_python_slicing():
    specs = specs_to_dicts(_specs("plain_qa6"))
    prompts = _prompts()
    out = WEB.prepare_compaction_input(specs, 99, {}, "", prompts.COMPACT,
                                       prompts.COMPACT_CASCADE)
    assert len(out["to_compact"]) == len(specs)
    assert out["to_preserve"] == []


# ---------------------------------------------------------------------------
# Golden vectors for the kernels that runtime_py does not expose
# ---------------------------------------------------------------------------

#: Kept in sync (same corpus, same order) with the goldens in
#: tests/unit/builtin_tools/test_compact_tool.cpp -- re-derived here from the
#: reference so a reference change breaks this test too.
ESTIMATE_GOLDENS = [
    ("", 0),
    ("a", 1),
    ("abc", 1),
    ("abcd", 1),
    ("abcde", 1),
    ("aaaaaaa", 1),
    ("aaaaaaaa", 2),
    ("a" * 100, 25),
    ("a" * 1000, 250),
    ("hello world, this is a test.", 7),
    ("def f(x):\n    return x + 1\n", 6),
    ("中文", 1),
    ("中文测试", 1),
    ("あいう", 1),
    ("abc中文", 1),
    ("abcdefg中", 2),
    ("ＡＢ", 1),
    ("a" * 19 + "中", 5),
    ("a" * 17 + "中文", 5),
    ("mix 中文 abcdef", 4),
    # kimi-cli/tests/utils/test_tokens.py::TestEstimateCharsTokens
    ("a" * 400, 100),
    ("你" * 300, 100),
    ("def foo():\n return '你好'", 6),
    ("a" * 96 + "你" * 4, 25),
    ("a" * 95 + "你" * 5, 28),
    ("Hello world 世", 3),
    ("Hello世界", 2),
    ("你好世界", 1),
    ("안녕하세요", 1),
    ("こんにちは", 1),
    ("Hello world", 2),
]

ADAPTIVE_GOLDENS = [
    (([], 1, 5), 1),
    (([], 5, 3), 5),
    (([], 0, 0), 0),
    (([("system", (text("error"),))], 5, 3), 5),
    (([("tool", (text("failed"),))], 5, 3), 5),
    (([("user", (text("failed"),))], 5, 3), 3),
    (([("user", (text("failed"),))], 0, 0), 0),
    (([("user", (text("Hello"),)), ("assistant", (text("Hi there"),))], 1, 5), 1),
    (([("user", (text("There was an error in the build"),))], 1, 5), 2),
    (([("assistant", (text("A RuntimeException occurred"),))], 1, 5), 2),
    (([("user", (text("The test failed"),))], 1, 5), 2),
    (([("assistant", (text("Let me think"), think("deep reasoning...")),)], 1, 5), 2),
    (([("assistant", (text("Edited file: foo.py and bar.md and baz.py"),),)], 1, 5), 2),
    (([("assistant", (text("There was an error while editing file: a.py, b.py, c.py"),
                      think("reasoning...")),)], 1, 5), 4),
    (([("assistant", (text("error exception failed file: a.py b.py c.py d.py"),
                      think("think...")),)], 1, 3), 3),
    (([("user", (text("plain chat"),))], 2, 5), 2),
    (([("user", (text("error here"),)), ("assistant", (text("all good"),))], 1, 5), 1),
    (([("system", (text("error here"),)), ("user", (text("all good"),))], 1, 5), 1),
]


def test_unexposed_kernel_goldens_are_reference_derived():
    comp = _compact()
    tokens = pr.ref("kimi_cli.utils.tokens")
    with pr.pure_python(tokens):
        for value, expected in ESTIMATE_GOLDENS:
            assert tokens._estimate_chars_tokens(value) == expected, repr(value)

    km = _kosong_message()
    wire = _wire()

    def build(parts):
        content = []
        for kind, value in parts:
            content.append(wire.TextPart(text=value) if kind == "text"
                           else wire.ThinkPart(think=value))
        return km.Message(role="user", content=content)

    for (specs, lo, hi), expected in ADAPTIVE_GOLDENS:
        messages = [km.Message(role=role, content=build(parts).content) for role, parts in specs]
        assert comp.adaptive_preserve_depth(messages, min_preserved=lo,
                                            max_preserved=hi) == expected, specs


def test_surface_fingerprint_reference_semantics():
    """None only for an empty history -- mirroring the C++ golden test."""
    comp = _compact()
    km = _kosong_message()
    wire = _wire()
    assert comp._surface_fingerprint([]).last_message_text is None
    fp = comp._surface_fingerprint([km.Message(role="user", content=[wire.ThinkPart(think="x")])])
    assert fp.last_message_text == ""
    fp2 = comp._surface_fingerprint([
        km.Message(role="user", content=[wire.TextPart(text="a")]),
        km.Message(role="assistant", content=[]),
    ])
    assert fp2.last_message_text == ""
    assert fp2.token_count == 1


def test_estimate_text_tokens_reference_corpus():
    """Same corpus as the C++ ``estimate_text_tokens_reference_goldens`` test.

    ``count_message_tokens`` sums one estimate per TextPart, never one estimate
    over a concatenated message, so two tiny parts cost more than their join.
    """
    tokens = pr.ref("kimi_cli.utils.tokens")
    with pr.pure_python(tokens):
        for value, expected in ESTIMATE_GOLDENS:
            assert tokens._estimate_chars_tokens(value) == expected, repr(value)
        assert tokens._estimate_chars_tokens("a") + tokens._estimate_chars_tokens("a") == 2
        assert tokens._estimate_chars_tokens("aa") == 1
        assert tokens._estimate_chars_tokens("a" * 400) == 100
        assert tokens._estimate_chars_tokens("你" * 300) == 100
