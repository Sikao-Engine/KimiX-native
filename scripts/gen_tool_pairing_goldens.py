#!/usr/bin/env python3
"""Regenerate tests/unit/builtin_tools/tool_pairing_goldens.inc.

The golden vectors come from running the *real* Python implementation of the
compaction preserve-boundary algorithm in the kimi-agent checkout:

* ``kimi_cli.soul.tool_pairing`` -- ``message_tool_call_delta`` /
  ``balanced_cut_indices`` / ``nearest_balanced_cut_before``
* ``kimi_cli.soul.compaction.SimpleCompaction.prepare`` -- the boundary the
  reference actually uses (preserve-depth walk + balanced-cut snap + the Phase-6
  primacy re-insertion and its re-cut), plus
  ``kimi_cli.soul.compaction.adaptive_preserve_depth`` for the adaptive variant.

The C++ port (``src/builtin_tools/compact_tool.cpp``:
``message_tool_call_delta`` / ``balanced_cut_indices`` /
``nearest_balanced_cut_before`` / ``resolve_preserve_split``) is replayed over the
same histories by ``tests/unit/builtin_tools/test_compact_tool.cpp`` and must
reproduce every vector.  ``python/tests/test_parity_tool_pairing.py`` cross-checks
the same kernels live through the pybind11 bindings.

For each history we record:

* the balanced-cut set (or the unbalanced marker, where the reference raises
  ``ValueError``), and
* ``nearest_balanced_cut_before(history, i)`` for every ``i`` in ``0..len``, and
* for each preserve depth (and for the adaptive depth) the reference's
  ``prepare`` result, reduced to the shape the C++ ``preserve_split`` expresses:
  ``compact`` (``compact_message is not None``), the contiguous
  ``preserve_start_index`` and ``keep_first_message``.

``keep_first_message`` is recovered from the reference's own ``to_preserve`` list:
``[messages[0]] + messages[k:]`` means the Phase-6 re-insertion happened, a plain
``messages[k:]`` means it did not (the re-cut branch reassigns ``to_preserve``
from the history, discarding the primacy copy).

Usage (any interpreter that can import ``kimi_cli``)::

    python scripts/gen_tool_pairing_goldens.py            # rewrite the .inc
    python scripts/gen_tool_pairing_goldens.py --check    # fail if out of date

``--python`` lets the caller point at the kimi-agent virtualenv interpreter when
the current interpreter cannot import ``kimi_cli``.
"""
from __future__ import annotations

import argparse
import os
import random
import re
import subprocess
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
KIMI_AGENT_ROOT = Path(os.environ.get("KIMI_AGENT_ROOT", r"C:/dev/kimi-agent"))
DEFAULT_AGENT_PYTHON = KIMI_AGENT_ROOT / ".venv" / "Scripts" / "python.exe"
OUT_PATH = PROJECT_ROOT / "tests" / "unit" / "builtin_tools" / "tool_pairing_goldens.inc"

# Preserve depths exercised for every history. The adaptive variant always uses
# adaptive_preserve_depth(msgs, min_preserved=1, max_preserved=2) (kimi_cli's
# LoopControl defaults), so `depth` is only the fixed-depth max_preserved_messages.
DEPTHS = [1, 2, 3, 5, 10]


def _ensure_reference_on_path() -> None:
    for rel in ("kimi-cli/src", "src"):
        p = KIMI_AGENT_ROOT / rel
        if p.is_dir() and str(p) not in sys.path:
            sys.path.insert(0, str(p))


def _import_reference():
    _ensure_reference_on_path()
    from kosong.message import Message, TextPart, ThinkPart, ToolCall  # noqa: PLC0415
    from kimi_cli.soul.compaction import (  # noqa: PLC0415
        SimpleCompaction,
        adaptive_preserve_depth,
    )
    from kimi_cli.soul import tool_pairing  # noqa: PLC0415

    return (Message, TextPart, ThinkPart, ToolCall, SimpleCompaction,
            adaptive_preserve_depth, tool_pairing)


# ---------------------------------------------------------------------------
# History model
# ---------------------------------------------------------------------------


class Msg:
    """One history entry in the generator's own terms.

    ``calls`` is the number of persisted tool calls.  Every message always uses
    the canonical call identity ``(id="c<k>", function=T("T", "{}"))`` so the
    reference's ``Message.__eq__`` (used by the Phase-6 check) agrees with the C++
    message equality, which only compares the tool-call *count*.
    """

    __slots__ = ("role", "text", "calls", "think")

    def __init__(self, role: str, text: str, calls: int = 0, think: str = "") -> None:
        self.role = role
        self.text = text
        self.calls = calls
        self.think = think


def to_kosong(msg: Msg, Message, TextPart, ThinkPart, ToolCall):
    """Build the reference Message for `msg`.

    A streamed ``ToolCallPart`` cannot be placed in ``Message.content`` in this
    kosong version (only the registered ContentPart subclasses validate), so the
    ``ToolCallPart`` branch of ``message_tool_call_delta`` is unreachable from a
    real Message and cannot be covered by a Python-derived golden.  The C++
    ``is_tool_call_part`` port of that branch is covered by hand-written cases in
    ``tests/unit/builtin_tools/test_compact_tool.cpp`` instead.
    """
    content = []
    if msg.think:
        content.append(ThinkPart(think=msg.think))
    if msg.text:
        content.append(TextPart(text=msg.text))
    tool_calls = [
        ToolCall(id=f"c{k}", function=ToolCall.FunctionBody(name="T", arguments="{}"))
        for k in range(msg.calls)
    ]
    return Message(role=msg.role, content=content, tool_calls=tool_calls or None)


# ---------------------------------------------------------------------------
# Corpus
# ---------------------------------------------------------------------------


def _triples(rounds: int, *, text: str = "body") -> list[Msg]:
    out: list[Msg] = []
    for i in range(rounds):
        out.append(Msg("user", f"u{i}{text}"))
        out.append(Msg("assistant", f"a{i}{text}", calls=1))
        out.append(Msg("tool", f"t{i}{text}"))
    return out


def corpus() -> list[tuple[str, list[Msg]]]:
    cases: list[tuple[str, list[Msg]]] = []
    cases.append(("empty", []))
    cases.append(("single_user", [Msg("user", "hello")]))
    cases.append(("plain_qa_pair", [Msg("user", "q0"), Msg("assistant", "a0")]))
    cases.append(("plain_qa6", [m for i in range(6) for m in
                                (Msg("user", f"q{i}"), Msg("assistant", f"a{i}"))]))
    cases.append(("tool_triples2", _triples(2)))
    cases.append(("tool_triples8", _triples(8)))

    # Two tool calls answered by two tool results (one assistant -> two tools).
    cases.append(("two_calls_one_assistant", [
        Msg("user", "u0"),
        Msg("assistant", "a0", calls=2),
        Msg("tool", "t0"),
        Msg("tool", "t1"),
        Msg("user", "u1"),
        Msg("assistant", "a1"),
    ]))

    # Tolerated dangling assistant tool call (in_progress stays positive).
    cases.append(("dangling_assistant_call", [
        Msg("user", "u0"),
        Msg("assistant", "a0", calls=1),
    ]))

    # Unbalanced: a tool result with no matching call (in_progress goes negative).
    cases.append(("orphan_tool", [Msg("user", "u0"), Msg("tool", "t0"), Msg("user", "u1")]))
    cases.append(("orphan_tool_first", [Msg("tool", "t0"), Msg("user", "u0")]))

    # Phase-6 primacy duplication: a later message equals the first one.
    cases.append(("primacy_duplicate", [
        Msg("user", "dup"),
        Msg("assistant", "a0"),
        Msg("user", "other"),
        Msg("assistant", "a1"),
        Msg("user", "dup"),
    ]))

    # Primacy re-cut candidates: the first message's own answer may stay in the
    # compacted region, depending on the depth.
    cases.append(("primacy_recut", [
        Msg("user", "u0"),
        Msg("assistant", "a0", calls=1),
        Msg("tool", "t0"),
        Msg("user", "u1"),
        Msg("assistant", "a1", calls=1),
        Msg("tool", "t1"),
        Msg("user", "u2"),
        Msg("assistant", "a2", calls=1),
        Msg("tool", "t2"),
    ]))

    # No user/assistant messages at all (preserve walk finds nothing).
    cases.append(("tool_only", [Msg("tool", "t0"), Msg("tool", "t1")]))

    # Re-cut triggers: an assistant tool call that follows a tool result makes the
    # walk land on an assistant whose predecessor is a tool message, so the
    # Phase-6 primacy removal (preserve point - 1) leaves the boundary off a
    # balanced cut and the reference re-cuts and drops the primacy copy.
    cases.append(("chained_recut1", [
        Msg("user", "u0"),
        Msg("assistant", "a0", calls=1),
        Msg("tool", "t0"),
        Msg("assistant", "a1", calls=1),
        Msg("tool", "t1"),
        Msg("user", "u1"),
    ]))
    cases.append(("chained_recut2", [
        Msg("user", "u0"),
        Msg("assistant", "a0", calls=1),
        Msg("tool", "t0"),
        Msg("assistant", "a1", calls=1),
        Msg("tool", "t1"),
        Msg("user", "u2"),
        Msg("assistant", "a2", calls=1),
        Msg("tool", "t2"),
        Msg("user", "u3"),
    ]))
    cases.append(("chained_recut3", [
        Msg("user", "u0"),
        Msg("assistant", "a0", calls=2),
        Msg("tool", "t0"),
        Msg("tool", "t1"),
        Msg("assistant", "a1", calls=1),
        Msg("tool", "t2"),
        Msg("assistant", "a2", calls=1),
        Msg("tool", "t3"),
        Msg("user", "u1"),
        Msg("assistant", "a3", calls=1),
        Msg("tool", "t4"),
        Msg("user", "u2"),
    ]))
    cases.append(("chained_plain_between", [
        Msg("user", "u0"),
        Msg("assistant", "a0", calls=1),
        Msg("tool", "t0"),
        Msg("assistant", "a1"),
        Msg("user", "u1"),
        Msg("assistant", "a2", calls=1),
        Msg("tool", "t1"),
        Msg("user", "u2"),
    ]))

    # Adaptive-preserve signals on the most recent user/assistant message.
    base = _triples(4)
    cases.append(("adaptive_error", base + [Msg("user", "u9"), Msg("assistant", "boom: error raised")]))
    cases.append(("adaptive_think", base + [Msg("user", "u9"), Msg("assistant", "plain", think="hmm")]))
    cases.append(("adaptive_file_refs", base + [Msg("user", "u9"),
                                                Msg("assistant", "file: a.py file: b.md file: c.txt")]))
    cases.append(("adaptive_quiet", base + [Msg("user", "u9"), Msg("assistant", "all good")]))

    # Realistic 24-message session (mirrors the soul unit test).
    cases.append(("session24", _triples(8, text=" padded to grow the estimate")))

    # Deterministic fuzz over role/call shapes.
    rng = random.Random(20260923)
    roles = ("user", "assistant", "tool")
    for n in range(0, 13):
        for variant in range(3):
            hist: list[Msg] = []
            for i in range(n):
                role = roles[rng.randrange(len(roles))]
                calls = rng.randrange(0, 3) if role == "assistant" else 0
                hist.append(Msg(role, f"m{i}", calls=calls))
            cases.append((f"fuzz_n{n}_v{variant}", hist))
    return cases


# ---------------------------------------------------------------------------
# Reference evaluation
# ---------------------------------------------------------------------------


def _delta(m: Msg) -> int:
    """message_tool_call_delta for one message: +N for an assistant tool call,
    -1 for a tool result, 0 otherwise (tool_pairing.py:13-27)."""
    if m.role == "assistant":
        return m.calls
    if m.role == "tool":
        return -1
    return 0


def _reference_fold(messages: list[Msg]) -> tuple[list[int], bool, int]:
    """Python's fold, replayed locally so the unbalanced index can be recorded."""
    cuts = [0]
    in_progress = 0
    for i, msg in enumerate(messages, start=1):
        in_progress += _delta(msg)
        if in_progress < 0:
            return cuts, True, i - 1
        if in_progress == 0:
            cuts.append(i)
    if cuts[-1] != len(messages):
        cuts.append(len(messages))
    return cuts, False, -1


_INDEX_RE = re.compile(r"at index (\d+)\s*$")


def _split_shape(ref_history, to_preserve) -> tuple[int, bool]:
    """Reduce the reference's ``to_preserve`` list to ``(k, keep_first)``.

    Both shapes the reference can emit are ``messages[k:]`` (no primacy copy) and
    ``[messages[0]] + messages[k:]`` (Phase-6 re-insertion).
    """
    n = len(ref_history)
    tail = list(to_preserve)
    keep_first = bool(tail) and tail[0] is ref_history[0] and len(tail) != n
    if keep_first:
        tail = tail[1:]
    for k in range(n + 1):
        candidate = list(ref_history[k:])
        if len(candidate) != len(tail):
            continue
        if all(a is b for a, b in zip(candidate, tail)):
            return k, keep_first
    raise AssertionError(f"to_preserve is not a suffix: {len(to_preserve)} of {n}")


def _msg_key(m: Msg):
    """Key matching the C++ message equality (role, tool-call count, content)."""
    return (m.role, m.calls, m.think, m.text)


def _mirror_prepare(spec: list[Msg], cuts: list[int], unbalanced: bool,
                    depth: int) -> dict:
    """Replay SimpleCompaction.prepare's boundary math (compaction.py:711-772).

    Used only to annotate the ``recut`` / ``recut_fallback`` columns and to
    cross-check the (compact, preserve_start, keep_first) triple that is derived
    from the reference's own ``to_preserve`` list.
    """
    out = {"compact": False, "p": 0, "keep_first": False,
           "recut": False, "recut_fallback": False}
    if not spec or depth <= 0 or unbalanced:
        return out
    n = len(spec)
    p = n
    n_preserved = 0
    for i in range(n - 1, -1, -1):
        if spec[i].role in ("user", "assistant"):
            n_preserved += 1
            if n_preserved == depth:
                p = i
                break
    if n_preserved < depth:
        return out
    p = max(c for c in cuts if c <= p)
    keep_first = not any(_msg_key(m) == _msg_key(spec[0]) for m in spec[p:])
    to_compact_len = p - 1 if (keep_first and p > 0) else p
    if to_compact_len not in cuts:
        smaller = [c for c in cuts if c < p]
        if smaller:
            p = max(smaller)
        else:
            p = 1
            out["recut_fallback"] = True
        to_compact_len = p
        out["recut"] = True
        keep_first = False
    if to_compact_len == 0:
        return out
    out["compact"] = True
    out["p"] = p
    out["keep_first"] = keep_first
    return out


def evaluate(Message, TextPart, ThinkPart, ToolCall, SimpleCompaction,
             adaptive_preserve_depth, tool_pairing):
    hist_cases: list[dict] = []
    nearest_cases: list[dict] = []
    split_cases: list[dict] = []

    for name, spec in corpus():
        ref_history = [to_kosong(m, Message, TextPart, ThinkPart, ToolCall) for m in spec]
        cuts, unbalanced, bad_index = _reference_fold(spec)

        # Cross-check the local fold against the reference implementation.
        if not unbalanced:
            assert set(cuts) == tool_pairing.balanced_cut_indices(ref_history), name
        else:
            try:
                tool_pairing.balanced_cut_indices(ref_history)
                raise AssertionError(f"{name}: reference did not raise")
            except ValueError as exc:
                m = _INDEX_RE.search(str(exc))
                assert m and int(m.group(1)) == bad_index, (name, str(exc), bad_index)

        hist_cases.append({
            "name": name,
            "count": len(spec),
            "roles": "|".join(m.role for m in spec),
            "calls": "|".join(str(_delta(m)) for m in spec),
            "texts": "|".join(m.text for m in spec),
            "thinks": "|".join("1" if m.think else "0" for m in spec),
            "think_texts": "|".join(m.think for m in spec),
            "cuts": "|".join(str(c) for c in cuts) if not unbalanced else "",
            "unbalanced": unbalanced,
            "unbalanced_index": bad_index,
        })

        # Include out-of-range indices: the reference clamps them *before* the
        # fold, so a negative index returns 0 and an index past the end returns
        # len(messages) even on an unbalanced history (no ValueError).
        indices = list(range(len(spec) + 1)) + [-1, -7, len(spec) + 1, len(spec) + 9]
        for index in indices:
            try:
                expect = tool_pairing.nearest_balanced_cut_before(ref_history, index)
            except ValueError:
                # Only reachable for an unbalanced history with an index the
                # clamp does not short-circuit.
                assert unbalanced, (name, index)
                nearest_cases.append({"name": name, "index": index, "expect": -1,
                                      "unbalanced": True})
                continue
            if unbalanced:
                # The reference clamps *before* the fold, so an out-of-range
                # index never raises even on an unbalanced history.
                assert index < 0 or index > len(spec), (name, index)
                nearest_cases.append({"name": name, "index": index, "expect": expect,
                                      "unbalanced": False})
                continue
            assert expect in cuts, (name, index)
            nearest_cases.append({"name": name, "index": index, "expect": expect,
                                  "unbalanced": False})

        for depth in DEPTHS:
            for adaptive in (False, True):
                if adaptive:
                    compactor = SimpleCompaction(
                        max_preserved_messages=2,
                        preserve_depth=lambda msgs: adaptive_preserve_depth(
                            msgs, min_preserved=1, max_preserved=2),
                    )
                    effective = adaptive_preserve_depth(
                        ref_history, min_preserved=1, max_preserved=2)
                else:
                    compactor = SimpleCompaction(max_preserved_messages=depth)
                    effective = depth

                row = {
                    "name": name, "depth": depth, "adaptive": adaptive,
                    "compact": False, "preserve_start": 0,
                    "keep_first": False, "unbalanced": False,
                    "recut": False, "recut_fallback": False,
                }
                # An unbalanced history only reaches the balanced-cut fold when
                # the preserve walk found `depth` user/assistant messages: with a
                # larger depth `prepare` returns before touching the pairing and
                # never raises.  Record which of the two happened.
                result = None
                try:
                    result = compactor.prepare(ref_history)
                except ValueError:
                    row["unbalanced"] = True
                if result is not None and result.compact_message is not None:
                    k, keep_first = _split_shape(ref_history, list(result.to_preserve))
                    row["compact"] = True
                    row["preserve_start"] = k
                    row["keep_first"] = keep_first

                mirror = _mirror_prepare(spec, cuts, unbalanced, effective)
                # The mirror must reproduce the reference-derived shape on every
                # case, otherwise the recut annotation is meaningless.
                if row["unbalanced"]:
                    assert not mirror["compact"], (name, depth, adaptive)
                else:
                    assert mirror["compact"] == row["compact"], (name, depth, adaptive)
                    assert mirror["p"] == row["preserve_start"], (name, depth, adaptive)
                    assert mirror["keep_first"] == row["keep_first"], (name, depth, adaptive)
                row["recut"] = mirror["recut"]
                row["recut_fallback"] = mirror["recut_fallback"]
                split_cases.append(row)
    return hist_cases, nearest_cases, split_cases


# ---------------------------------------------------------------------------
# Emit
# ---------------------------------------------------------------------------


def c_literal(text: str, chunk: int = 1000) -> str:
    data = text.encode("utf-8")
    chunks: list[list[str]] = [[]]
    size = 0
    pending_hex = False
    for byte in data:
        ch = chr(byte)
        if byte == 0x22:
            token = '\\"'
            pending_hex = False
        elif byte == 0x5C:
            token = "\\\\"
            pending_hex = False
        elif 0x20 <= byte < 0x7F:
            token = ('""' + ch) if (pending_hex and ch in "0123456789abcdefABCDEF") else ch
            pending_hex = False
        else:
            token = "\\x%02x" % byte
            pending_hex = True
        if size >= chunk and not pending_hex:
            chunks.append([])
            size = 0
        chunks[-1].append(token)
        size += len(token)
    return '\n '.join('"' + "".join(part) + '"' for part in chunks)


def _emit(hist_cases, nearest_cases, split_cases) -> str:
    lines: list[str] = []
    lines.append("// GENERATED by scripts/gen_tool_pairing_goldens.py from the Python")
    lines.append("// reference (kimi_cli/soul/tool_pairing.py +")
    lines.append("// kimi_cli/soul/compaction.py SimpleCompaction.prepare). Do not edit by")
    lines.append("// hand - regenerate with: python scripts/gen_tool_pairing_goldens.py")
    lines.append("//")
    lines.append("// One history per case: every column is '|'-separated with one entry per")
    lines.append("// message, in order. `calls` is the golden value of")
    lines.append("// message_tool_call_delta: len(Message.tool_calls) for an assistant, -1 for")
    lines.append("// a tool result, 0 otherwise. Build a C++ message with")
    lines.append("// tool_call_count = max(calls, 0).")
    lines.append("struct tp_history_case {")
    lines.append("    const char *name;")
    lines.append("    int32_t count;")
    lines.append("    const char *roles;    // '|'-separated roles")
    lines.append("    const char *calls;    // '|'-separated message_tool_call_delta values")
    lines.append("    const char *texts;    // '|'-separated text parts (empty when absent)")
    lines.append("    const char *thinks;   // '|'-separated 0/1: carries a think part first")
    lines.append("    const char *think_texts; // '|'-separated think texts (empty when thinks=0)")
    lines.append("    const char *cuts;     // '|'-separated balanced cuts (empty when unbalanced)")
    lines.append("    bool unbalanced;")
    lines.append("    int32_t unbalanced_index;")
    lines.append("};")
    lines.append("")
    lines.append("// nearest_balanced_cut_before(history, index) for every index 0..count.")
    lines.append("// `expect` is -1 when the reference raises ValueError (unbalanced).")
    lines.append("struct tp_nearest_case {")
    lines.append("    const char *name;")
    lines.append("    int32_t index;")
    lines.append("    int32_t expect;")
    lines.append("    bool unbalanced;")
    lines.append("};")
    lines.append("")
    lines.append("// resolve_preserve_split(history, depth, adaptive) reduced to the shape the")
    lines.append("// C++ preserve_split expresses. `recut` marks the reference's re-cut branch")
    lines.append("// (which drops the Phase-6 primacy copy); `recut_fallback` marks the")
    lines.append("// pathological \"no balanced cut below the preserve point\" fallback to 1.")
    lines.append("struct tp_split_case {")
    lines.append("    const char *name;")
    lines.append("    int32_t depth;       // fixed max_preserved_messages when adaptive == false")
    lines.append("    bool adaptive;       // adaptive_preserve_depth(msgs, 1, 2) instead")
    lines.append("    bool compact;")
    lines.append("    int32_t preserve_start;")
    lines.append("    bool keep_first;")
    lines.append("    bool unbalanced;")
    lines.append("    bool recut;")
    lines.append("    bool recut_fallback;")
    lines.append("};")
    lines.append("")
    lines.append("const tp_history_case kToolPairingHistories[] = {")
    for row in hist_cases:
        lines.append('    {%s, %d, %s, %s, %s, %s, %s, %s, %s, %d},' % (
            c_literal(row["name"]), row["count"],
            c_literal(row["roles"]), c_literal(row["calls"]),
            c_literal(row["texts"]), c_literal(row["thinks"]),
            c_literal(row["think_texts"]),
            c_literal(row["cuts"]),
            "true" if row["unbalanced"] else "false", row["unbalanced_index"]))
    lines.append("};")
    lines.append("")
    lines.append("const tp_nearest_case kToolPairingNearest[] = {")
    for row in nearest_cases:
        lines.append('    {%s, %d, %d, %s},' % (
            c_literal(row["name"]), row["index"], row["expect"],
            "true" if row["unbalanced"] else "false"))
    lines.append("};")
    lines.append("")
    lines.append("const tp_split_case kToolPairingSplits[] = {")
    for row in split_cases:
        lines.append('    {%s, %d, %s, %s, %d, %s, %s, %s, %s},' % (
            c_literal(row["name"]), row["depth"],
            "true" if row["adaptive"] else "false",
            "true" if row["compact"] else "false", row["preserve_start"],
            "true" if row["keep_first"] else "false",
            "true" if row["unbalanced"] else "false",
            "true" if row["recut"] else "false",
            "true" if row["recut_fallback"] else "false"))
    lines.append("};")
    lines.append("")
    lines.append("const size_t kToolPairingHistoryCount =")
    lines.append("    sizeof(kToolPairingHistories) / sizeof(kToolPairingHistories[0]);")
    lines.append("const size_t kToolPairingNearestCount =")
    lines.append("    sizeof(kToolPairingNearest) / sizeof(kToolPairingNearest[0]);")
    lines.append("const size_t kToolPairingSplitCount =")
    lines.append("    sizeof(kToolPairingSplits) / sizeof(kToolPairingSplits[0]);")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="fail when the checked-in .inc differs from the generated one")
    parser.add_argument("--python", default=None,
                        help="interpreter able to import kimi_cli (default: kimi-agent venv)")
    parser.add_argument("--out", default=str(OUT_PATH), help="output .inc path")
    args = parser.parse_args()

    try:
        refs = _import_reference()
    except Exception as exc:  # pragma: no cover - environment issue
        fallback = Path(args.python) if args.python else DEFAULT_AGENT_PYTHON
        if fallback.is_file() and Path(sys.executable) != fallback:
            print("re-running under %s (%s)" % (fallback, exc), file=sys.stderr)
            cmd = [str(fallback), str(Path(__file__).resolve()), *sys.argv[1:]]
            return subprocess.call(cmd)
        print("cannot import kimi_cli from %s: %s" % (KIMI_AGENT_ROOT, exc), file=sys.stderr)
        return 2

    hist_cases, nearest_cases, split_cases = evaluate(*refs)
    text = _emit(hist_cases, nearest_cases, split_cases)
    out_path = Path(args.out)
    if args.check:
        existing = out_path.read_text(encoding="utf-8") if out_path.exists() else ""
        if existing != text:
            print("%s is out of date" % out_path, file=sys.stderr)
            return 1
        print("%s is up to date (%d histories, %d nearest, %d splits)" % (
            out_path, len(hist_cases), len(nearest_cases), len(split_cases)))
        return 0
    out_path.write_text(text, encoding="utf-8", newline="\n")
    print("wrote %s (%d histories, %d nearest, %d splits)" % (
        out_path, len(hist_cases), len(nearest_cases), len(split_cases)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
