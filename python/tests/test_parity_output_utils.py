"""Differential parity harness for the SHARED builtin-tool infrastructure.

``src/builtin_tools/tool_types.*`` + ``utf8_util.*`` are the foundation every
built-in tool kernel stands on, so their behaviour is pinned two ways:

1. **Live differential** (this file).  The only halves of the shared code that
   ``runtime_py`` exposes are ``common.is_ascii`` and
   ``common.utf8_code_point_count`` (``src/runtime/common/utf8.cpp`` -- a
   *parallel* implementation of the same UTF-8 primitives that
   ``builtin_tools::utf8_util`` compiles into kimix-llm).  They are compared
   against CPython over a generated corpus; every other kernel is verified
   through the golden replay below, because no binding reaches it.
2. **Golden replay** (``tests/unit/builtin_tools/test_tool_types.cpp`` and
   ``test_tool.cpp`` over ``tool_types_goldens.inc``).  This module additionally
   re-derives *every* golden vector from the live reference implementation
   (kimi-agent's ``kimi_cli.tools.file.output_utils`` / ``grep_local`` and
   CPython's own codec / json module) and byte-compares the result with the
   committed file, so a frozen golden can never drift away from its reference.

Provenance rules (same as the other parity tests): the extension is imported
FIRST and its ``__file__`` is verified to be this checkout's build --
``kimi_cli.native_loader`` would otherwise put kimi-agent's released
``runtime_py.pyd`` on ``sys.path`` and the port would be compared against
itself.  Never compare against ``python/kimix_native/*``.
"""

from __future__ import annotations

import importlib.util
import json
import os
import re
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import _parity_ref  # noqa: E402
from _parity_ref import (  # noqa: E402
    BIN_DIR,
    KIMI_AGENT_ROOT,
    REPO_ROOT,
    native,
    ref,
    ref_available,
)

if BIN_DIR is None:
    pytest.skip(
        "runtime_py extension not built - run "
        "'python scripts/build_locked.py -- xmake build runtime_py'",
        allow_module_level=True,
    )

# IMPORT ORDER IS LOAD-BEARING (see _parity_ref.native): claim the extension
# before anything imports kimi_cli and its native_loader.
runtime_py = native()
COMMON = runtime_py.common

GOLDENS = REPO_ROOT / "tests" / "unit" / "builtin_tools" / "tool_types_goldens.inc"
GENERATOR = REPO_ROOT / "scripts" / "gen_tool_types_goldens.py"

pytestmark = pytest.mark.skipif(
    not ref_available(), reason=f"kimi-agent checkout not found at {KIMI_AGENT_ROOT}"
)


def _load_generator():
    spec = importlib.util.spec_from_file_location("gen_tool_types_goldens", GENERATOR)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _cpp_lit(text) -> str:
    """The literal encoding used by the generator (mirrored here on purpose:
    if one side changes, the presence checks below fail loudly)."""
    raw = text.encode("utf-8") if isinstance(text, str) else bytes(text)
    out = ['"']
    for b in raw:
        if b == 0x22:
            out.append('\\"')
        elif b == 0x5C:
            out.append("\\\\")
        elif 0x20 <= b < 0x7F:
            out.append(chr(b))
        else:
            out.append("\\%03o" % b)
    return "".join(out) + '"'


# ---------------------------------------------------------------------------
# provenance
# ---------------------------------------------------------------------------


def test_native_extension_is_this_checkouts_build():
    """The harness must exercise THIS checkout's own freshly built pyd.

    Provenance is asserted by *location*: the imported extension must be the
    file in the build directory ``conftest.py`` pinned (``KIMIX_PARITY_BIN`` ->
    ``BIN_DIR``), that directory must be inside this repo, and it must not be
    kimi-agent's staged release.  Neither the repo directory's name nor the
    build mode is part of the contract (``release`` is the ``bootstrap.py``
    default, but a worktree may legitimately be configured for ``debug``).
    """
    resolved = Path(runtime_py.__file__).resolve()
    assert resolved.parent == BIN_DIR.resolve(), (
        f"runtime_py came from {resolved}, expected {BIN_DIR}"
    )
    assert REPO_ROOT.resolve() in resolved.parents, "extension is outside this repo"
    assert "kimi-agent" not in str(resolved), "kimi-agent's released pyd must not win"


def test_reference_modules_resolve_to_the_kimi_agent_checkout():
    output_utils = ref("kimi_cli.tools.file.output_utils")
    grep_local = ref("kimi_cli.tools.file.grep_local")
    for mod in (output_utils, grep_local):
        assert Path(mod.__file__).resolve().is_relative_to(KIMI_AGENT_ROOT.resolve())


# ---------------------------------------------------------------------------
# live differential: the UTF-8 primitives reachable from Python
# ---------------------------------------------------------------------------


def _byte_corpus() -> list:
    import random

    rng = random.Random(0x0F7F_2026)
    interesting = [
        0x00, 0x09, 0x20, 0x41, 0x7A, 0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF,
        0xC0, 0xC1, 0xC2, 0xDF, 0xE0, 0xE1, 0xEC, 0xED, 0xEE, 0xEF, 0xF0, 0xF1,
        0xF3, 0xF4, 0xF5, 0xFE, 0xFF,
    ]
    corpus = [b"", b"ok", b"\x80", b"\xff", b"\xc3\xa9", "\u2192".encode()]
    for b in range(256):
        corpus.append(bytes([b]))
    for lead in interesting:
        for second in range(0, 256, 7):
            corpus.append(bytes([lead, second]))
    for _ in range(4000):
        n = rng.randint(0, 8)
        corpus.append(bytes(rng.choice(interesting) for _ in range(n)))
    for _ in range(2000):
        n = rng.randint(0, 8)
        corpus.append(bytes(rng.randrange(256) for _ in range(n)))
    # valid text, including astral code points
    for _ in range(1000):
        parts = []
        for _ in range(rng.randint(0, 6)):
            while True:
                cp = rng.randrange(0, 0x110000)
                if not (0xD800 <= cp <= 0xDFFF):
                    break
            parts.append(chr(cp))
        corpus.append("".join(parts).encode("utf-8"))
    return corpus


def test_common_is_ascii_matches_cpython():
    for data in _byte_corpus():
        want = all(b < 0x80 for b in data)
        assert COMMON.is_ascii(data) is want, data


def test_common_utf8_code_point_count_matches_cpython_for_valid_utf8():
    for data in _byte_corpus():
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            continue  # CPython has no code-point count for invalid bytes
        assert COMMON.utf8_code_point_count(data) == len(text), data


def test_common_utf8_code_point_count_on_invalid_bytes_is_documented():
    """Invalid input is outside CPython's contract, but the native walk is still
    deterministic: it advances one byte per byte it cannot fold into a code
    point.  ``src/runtime/common/utf8.cpp`` rejects overlongs and >U+10FFFF but
    *accepts* a CESU surrogate (its ``min_cp`` check cannot see one), the
    opposite policy from ``builtin_tools::utf8_util`` (which rejects surrogates
    and accepts overlongs).  Both are only ever fed valid UTF-8 by their
    callers; the values are pinned here so a change has to be deliberate."""
    assert COMMON.utf8_code_point_count(b"\xe0\x80\x80") == 3  # overlong rejected
    assert COMMON.utf8_code_point_count(b"\xe1\x80") == 2  # truncated lead + stray byte
    assert COMMON.utf8_code_point_count(b"\xed\xa0\x80") == 1  # CESU surrogate accepted
    assert COMMON.utf8_code_point_count(b"\xf4\x90\x80\x80") == 4  # beyond U+10FFFF
    # CPython's replacement count agrees on the overlong case and differs on the
    # surrogate one - neither answer is "wrong" for malformed input.
    assert len(b"\xe0\x80\x80".decode("utf-8", "replace")) == 3
    assert len(b"\xed\xa0\x80".decode("utf-8", "replace")) == 3
    assert COMMON.utf8_code_point_count(b"abc") == 3


# ---------------------------------------------------------------------------
# golden freshness: the committed .inc must equal a fresh render
# ---------------------------------------------------------------------------


def test_golden_file_is_reference_derived():
    gen = _load_generator()
    grep_local, output_utils = gen.load_reference(str(KIMI_AGENT_ROOT))
    fresh = gen.render(grep_local, output_utils)
    committed = GOLDENS.read_text(encoding="utf-8")
    if fresh != committed:
        fresh_lines = fresh.splitlines()
        committed_lines = committed.splitlines()
        for i, (a, b) in enumerate(zip(fresh_lines, committed_lines)):
            if a != b:
                pytest.fail(
                    f"{GOLDENS.name} is stale at line {i + 1}:\n"
                    f"  reference: {a[:200]}\n  committed: {b[:200]}\n"
                    "re-run: python scripts/gen_tool_types_goldens.py"
                )
        pytest.fail(
            f"{GOLDENS.name} is stale (line counts differ: reference "
            f"{len(fresh_lines)} vs committed {len(committed_lines)})"
        )


def test_golden_render_is_deterministic():
    gen = _load_generator()
    grep_local, output_utils = gen.load_reference(str(KIMI_AGENT_ROOT))
    assert gen.render(grep_local, output_utils) == gen.render(grep_local, output_utils)


# ---------------------------------------------------------------------------
# the regression vectors, checked against the LIVE reference
# ---------------------------------------------------------------------------


def _inc_text() -> str:
    return GOLDENS.read_text(encoding="utf-8")


def _esc(text) -> str:
    """The literal *body* (no surrounding quotes) the generator emits, so a
    marker that is embedded inside a longer literal can be located."""
    return _cpp_lit(text)[1:-1]


def test_utf8_truncated_invalid_continuation_is_pinned():
    """CPython reports "invalid continuation byte" (not "unexpected end of
    data") when a truncated sequence already carries an illegal continuation
    byte -- the port checked truncation first and reported the wrong reason for
    3020 of the 19235 golden vectors."""
    for data in (b"\xe0\x00", b"\xed\xa0", b"\xf0\x90\x41", b"\xe0\x80", b"\xf4\x90"):
        with pytest.raises(UnicodeDecodeError) as exc:
            data.decode("utf-8")
        assert exc.value.reason == "invalid continuation byte", data
        assert exc.value.start == 0, data
        # ... and the committed golden carries exactly that answer.
        row = "{tt_cstr(%s), false, 0, tt_cstr(%s)}," % (
            _cpp_lit(data),
            _cpp_lit("invalid continuation byte"),
        )
        assert row in _inc_text(), f"missing golden row for {data!r}: {row}"
    # a legal-but-short prefix keeps the other reason
    for data in (b"\xe0\xa0", b"\xc3"):
        with pytest.raises(UnicodeDecodeError) as exc:
            data.decode("utf-8")
        assert exc.value.reason == "unexpected end of data", data


def test_utf8_error_vectors_cover_every_reason():
    inc = _inc_text()
    m = re.search(r"k_tt_utf8_error_golden\[\] = \{(.*?)\n\};", inc, re.S)
    assert m is not None, "the utf8 error array is missing"
    reasons = re.findall(r'false, \d+, tt_cstr\("([^"]*)"\)', m.group(1))
    assert set(reasons) == {
        "invalid start byte",
        "invalid continuation byte",
        "unexpected end of data",
    }, sorted(set(reasons))
    # "surrogates not allowed" is an *encode*-side wording: it can never be a
    # decoding reason, so the port must not emit it anywhere.
    assert "surrogates not allowed" not in inc


def test_join_with_byte_limit_keeps_the_crossing_line():
    """The shared helper used to stop *before* the line that reaches the
    budget; the Python reference (grep_local._join_with_byte_limit, and glob.py
    631-637 which inlines the same loop) keeps it."""
    grep_local = ref("kimi_cli.tools.file.grep_local")
    lines = ["aaaa", "bbbb", "cccc", "dddd"]
    want, truncated = grep_local._join_with_byte_limit(list(lines), 10)
    assert want == "aaaa\nbbbb\ncccc" and truncated is True
    # the old native answer (stop before the crossing line) is NOT the golden
    assert want != "aaaa\nbbbb"
    row = "{tt_cstr(%s), 10, tt_cstr(%s), true, 1}," % (
        _cpp_lit("aaaa\x1fbbbb\x1fcccc\x1fdddd\x1f"),
        _cpp_lit(want),
    )
    assert row in _inc_text(), f"missing golden row: {row}"
    # glob.py's inline loop (glob.py 631-637) agrees with the function
    acc, n_bytes, glob_truncated = [], 0, False
    for line in lines:
        acc.append(line)
        n_bytes += (1 if len(acc) > 1 else 0) + len(line.encode("utf-8"))
        if n_bytes >= 10:
            glob_truncated = True
            break
    assert ("\n".join(acc), glob_truncated) == (want, truncated)


def test_marker_texts_are_pinned():
    """The fold / dedup markers are user-visible strings: pin their exact text
    against the reference and against the committed goldens."""
    _, output_utils = _load_generator().load_reference(str(KIMI_AGENT_ROOT))
    folded, omitted = output_utils.fold_lines([str(i) for i in range(1, 11)], 3)
    assert folded[1] == "\u2026 (7 lines omitted) \u2026" and omitted == 7
    assert folded[0] == "1" and folded[-2:] == ["9", "10"]
    # the marker is UTF-8, so it appears octal-escaped in the generated literal
    inc = _inc_text()
    assert _esc("\u2026 (7 lines omitted) \u2026") in inc
    collapsed, saved = output_utils.dedup_lines(
        ["a", "b", "b", "b", "b", "c", "d", "d", "e"], min_repeats=3
    )
    assert collapsed == ["a", "b  (3 repeats)", "c", "d", "d", "e"] and saved == 3
    assert _esc("b  (3 repeats)") in inc, "the dedup suffix text must be a golden"
    # the suffix shape is f"{line}  ({n} repeats)" - two spaces, always plural
    assert collapsed[1].index("  (") == 1
    truncated = output_utils.truncate_line("x" * 60, 20)
    assert truncated.endswith("\u2026 [+40 chars]") and len(truncated) == 20
    assert _esc("\u2026 [+40 chars]") in inc


def test_json_golden_buckets_are_classified_by_the_generator():
    """The JSON vectors are bucketed by *policy*, not by hand: everything
    CPython accepts that the native value model cannot hold must be in a
    documented-divergence bucket, everything else in the parity bucket."""
    gen = _load_generator()
    buckets = gen.classify_json_texts()
    parity = dict(buckets["parity"])
    assert '{"a":1}' in parity and parity['{"a":1}'] == "{k:a=i:1}"
    assert '{"a":1.0}' in parity and parity['{"a":1.0}'] == "{k:a=f:3ff0000000000000}"
    assert '{"a":-0.0}' in parity and parity['{"a":-0.0}'] == "{k:a=f:8000000000000000}"
    assert '{"a":18446744073709551615}' in parity  # uint64 max stays exact
    assert '{"a":1,"a":2}' in parity and parity['{"a":1,"a":2}'] == "{k:a=i:2}"

    unrep = [t for t, _ in buckets["unrepresentable"]]
    assert '{"a":NaN}' in unrep and '{"a":1e999}' in unrep
    assert '{"a":"\\ud800"}' in unrep  # lone surrogate is not UTF-8

    lossy = [t for t, _, _ in buckets["lossy"]]
    assert '{"a":123456789012345678901234567890}' in lossy
    assert '{"a":-9223372036854775809}' in lossy

    nonobj = [t for t, _ in buckets["nonobject"]]
    assert "[1,2,3]" in nonobj and '"str"' in nonobj and "42" in nonobj

    reject = buckets["reject"]
    assert "" in reject and '{"a":1,}' in reject and '{"a":1} x' in reject

    # every parity row's canon really is the canonical rendering of that text
    for text, canon in buckets["parity"]:
        assert canon == gen.json_canon(json.loads(text)), text
        assert canon

    # the doubles really are compared bit for bit
    one = json.loads('{"a":3.14}')["a"]
    assert "f:" + struct.pack(">d", one).hex() in parity['{"a":3.14}']

    # and the committed file carries those rows
    inc = _inc_text()
    assert "{tt_cstr(%s), tt_cstr(%s)}," % (
        _cpp_lit('{"a":NaN}'),
        _cpp_lit(ascii(repr({"a": float("nan")}))),
    ) in inc
    assert _cpp_lit("{k:a=i:123456789012345678901234567890}") in inc


def test_json_parity_corpus_is_large_enough_to_be_meaningful():
    gen = _load_generator()
    buckets = gen.classify_json_texts()
    assert len(buckets["parity"]) >= 60
    assert len(buckets["reject"]) >= 38
    assert len(buckets["unrepresentable"]) >= 15
    assert len(buckets["lossy"]) >= 6
    assert len(buckets["nonobject"]) >= 6


def test_utf8_corpus_is_large_enough_to_be_meaningful():
    gen = _load_generator()
    corpus = gen.utf8_corpus()
    assert len(corpus) >= 19000
    # exhaustive over the single bytes and the representative two-byte leads
    assert {bytes([b]) for b in range(256)} <= set(corpus)
    assert len(gen.utf8_valid_corpus()) >= 700
