"""Differential parity tests for the job_output builtin tool (kimix-base <-> kimi-agent).

``src/builtin_tools/job_output_tool.cpp`` is a port of kimi-agent's
``kimix.tools.background`` ``TaskOutput`` tool.  Most of that tool is a
decision/formatting layer over a live process registry and is covered by the
golden replay in ``tests/unit/builtin_tools/job_output_goldens.inc`` +
``test_job_output_tool.cpp`` (the tool is not exposed through ``runtime_py``).

One kernel *is* reachable from Python and is checked here live:

* ``bounded_append`` -- ``src/runtime/tools/security.cpp`` (exposed as
  ``runtime_py.tools.bounded_append``) is the native half of
  ``kimix/tools/background/utils.py::bounded_append`` (the output cap of every
  background job, ``BACKGROUND_MAX_OUTPUT_CHARS``).  The reference calls it
  through a tell-guard

      if buf.tell() + len(text) > cap:
          content, truncated = _NATIVE_TOOLS.bounded_append(buf.getvalue(), text, cap)

  so this module checks three things:

  1. the kernel agrees byte-for-byte (code-point-for-code-point) with the
     *verbatim Python body* of ``utils.py::bounded_append`` (the ground truth --
     driven with the reference module's native gate forced off);
  2. the same body, driven through the reference's own tell-guard with
     ``_NATIVE_TOOLS`` pointed at *our* extension, produces the identical
     buffer -- i.e. the guard arithmetic and the kernel compose exactly like
     the reference;
  3. the ``_compat`` mirror shipped in kimi-agent's ``bin/kimix_native`` (a
     second, independent expression of the same (content, text, cap) contract)
     agrees as well.

Cap semantics under test: ``int(cap * 0.4)`` head / ``cap - head`` tail with the
``\\n[... (output truncated, keeping first N and last M chars)]\\n`` marker, the
returned ``(new_content, truncated)`` pair, the ``truncated`` flag boundary
(exactly ``cap`` is NOT truncated, ``cap + 1`` is), caps of 0/1/2/3 and negative
caps, and Unicode counted in *code points* -- ``str`` lengths and slices in the
reference, never UTF-8 bytes.

Everything imports the kimix-base extension *before* the kimi-agent checkout:
``kimi_cli.native_loader`` puts ``<kimi-agent>/bin`` (a released
``runtime_py.pyd``) first on ``sys.path``, so a later ``import runtime_py`` would
compare the port against itself.  ``_parity_ref.native()`` additionally verifies
``runtime_py.__file__`` and re-imports from ``<repo>/bin/<mode>`` when a foreign
copy won (the same hazard ``test_parity_bash.py`` documents).
"""

from __future__ import annotations

import importlib
import importlib.util
import io
import os
import random
import sys
from pathlib import Path

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from _parity_ref import (  # noqa: E402
    BIN_DIR,
    KIMI_AGENT_ROOT,
    native,
    pure_python,
    ref,
)

pytestmark = pytest.mark.skipif(
    BIN_DIR is None,
    reason="no runtime_py build found under bin/ - build it first",
)

try:  # IMPORT ORDER IS LOAD-BEARING (see the module docstring).
    runtime_py = native()
except RuntimeError as _exc:  # pragma: no cover - defensive
    pytest.skip(str(_exc), allow_module_level=True)

TOOLS = runtime_py.tools

#: The reference module (kimi-agent's own ``background/utils.py``), never a
#: ``kimix_native`` mirror: a mirror that shares a mistake would prove nothing.
try:
    background_utils = ref("kimix.tools.background.utils")
except Exception as _exc:  # pragma: no cover - defensive
    pytest.skip(f"kimi-agent reference unavailable: {_exc}", allow_module_level=True)

bounded_append_ref = background_utils.bounded_append

assert str(BIN_DIR) in str(Path(runtime_py.__file__).resolve()), (
    f"runtime_py came from {runtime_py.__file__}, not {BIN_DIR} -- a staged copy "
    "shadowed it; parity results would be bogus"
)


# ---------------------------------------------------------------------------
# reference loaders
# ---------------------------------------------------------------------------
def _load_shim_package(pkg_dir: Path, alias: str):
    """Load ``<kimi-agent>/bin/kimix_native`` under *alias* as a real package.

    ``tools.py`` uses relative imports (``from . import _native, use_native``),
    so it cannot be loaded standalone; a private alias keeps the kimi-agent copy
    authoritative even if a kimix-base shim owns the ``kimix_native`` entry.
    """
    spec = importlib.util.spec_from_file_location(
        alias, pkg_dir / "__init__.py",
        submodule_search_locations=[str(pkg_dir)])
    if spec is None or spec.loader is None:  # pragma: no cover - defensive
        pytest.skip(f"cannot load reference package {pkg_dir}")
    package = importlib.util.module_from_spec(spec)
    sys.modules[alias] = package
    spec.loader.exec_module(package)
    return importlib.import_module(f"{alias}.tools")


_SHIM = _load_shim_package(
    KIMI_AGENT_ROOT / "bin" / "kimix_native", "_parity_job_output_native")
COMPAT_APPEND = _SHIM._compat_bounded_append  # (content, text, cap) -> (str, bool)


# ---------------------------------------------------------------------------
# harnesses
# ---------------------------------------------------------------------------
def ref_append(content: str, text: str, cap: int) -> tuple[str, bool]:
    """Drive the reference *pure-Python body* over (content, text, cap).

    ``pure_python`` turns ``_native_use_native`` into ``lambda: False`` and
    clears ``_NATIVE_TOOLS``, so ``bounded_append`` runs its verbatim Python
    body (and, on the truncation branch, the module-global ``io.StringIO``
    slicing) instead of short-circuiting into a native library.
    """
    buf = io.StringIO()
    buf.write(content)
    with pure_python(background_utils):
        truncated = bounded_append_ref(buf, text, cap)
    return buf.getvalue(), truncated


def ref_append_via_native_guard(content: str, text: str, cap: int) -> tuple[str, bool]:
    """Drive the reference *through its own tell-guard* with our kernel behind it.

    This is the reference's real call path (``utils.py`` 66-79): the guard
    ``buf.tell() + len(text) > cap`` decides whether the native kernel runs at
    all, and the kernel result replaces the buffer.  Pointing ``_NATIVE_TOOLS``
    at ``runtime_py.tools`` makes the comparison end-to-end.
    """
    class _Shim:  # the shape background/utils.py expects of kimix_native.tools
        bounded_append = staticmethod(TOOLS.bounded_append)

    saved_use = background_utils._native_use_native
    saved_tools = background_utils._NATIVE_TOOLS
    background_utils._native_use_native = lambda *_a, **_k: True
    background_utils._NATIVE_TOOLS = _Shim()
    try:
        buf = io.StringIO()
        buf.write(content)
        truncated = bounded_append_ref(buf, text, cap)
        return buf.getvalue(), truncated
    finally:
        background_utils._native_use_native = saved_use
        background_utils._NATIVE_TOOLS = saved_tools


def compat_append(content: str, text: str, cap: int) -> tuple[str, bool]:
    """kimi-agent's shipped ``_compat_bounded_append`` (independent mirror)."""
    return COMPAT_APPEND(content, text, cap)


def cpp_append(content: str, text: str, cap: int) -> tuple[str, bool]:
    content_out, truncated = TOOLS.bounded_append(content, text, cap)
    return content_out, truncated


def assert_parity(content: str, text: str, cap: int) -> tuple[str, bool]:
    """The C++ kernel must equal the reference body AND the guarded call path."""
    expected = ref_append(content, text, cap)
    got = cpp_append(content, text, cap)
    assert got == expected, (
        f"bounded_append mismatch for cap={cap!r}, content[{len(content)}]="
        f"{content[:40]!r}, text[{len(text)}]={text[:40]!r}\n"
        f"  python: {expected[1]!r} {expected[0][:160]!r}\n"
        f"  c++   : {got[1]!r} {got[0][:160]!r}")
    assert ref_append_via_native_guard(content, text, cap) == expected, (
        "the reference's tell-guard + our kernel disagrees with the Python body "
        f"for cap={cap!r}, content={content[:40]!r}, text={text[:40]!r}")
    assert compat_append(content, text, cap) == expected, (
        "kimi-agent's _compat_bounded_append disagrees with its own Python body "
        f"for cap={cap!r}, content={content[:40]!r}, text={text[:40]!r}")
    return got


# ---------------------------------------------------------------------------
# corpora
# ---------------------------------------------------------------------------
#: Cases taken verbatim from kimi-agent's own suite:
#: tests/test_background_utils.py::test_bounded_append_under_cap_no_op /
#: test_bounded_append_over_cap_head_tail_marker /
#: test_bounded_append_accumulates_and_truncates (cap 1000).
KIMI_AGENT_CASES = [
    ("", "hello world", 1000),
    ("", "x" * 3000, 1000),
    ("a" * 600, "b" * 600, 1000),
    ("a" * 600, "", 1000),
    ("", "", 1000),
    # Exactly at the cap is NOT a truncation; one character more is.
    ("", "y" * 1000, 1000),
    ("", "y" * 1001, 1000),
    ("z" * 999, "w", 1000),
    ("z" * 1000, "w", 1000),
    # A full job's worth of chunks (test_get_output_bounded_with_truncation_flag).
    ("y" * 100 * 10, "y" * 100, 1000),
]

#: Degenerate caps: the marker line and both slices must still be well defined.
DEGENERATE_CAPS = [0, 1, 2, 3, 4, 5, 7, 9, 10, 17, -1, -2, -5, -100, -1000]

#: Character mix for the code-point (never byte) checks.
UNICODE_SAMPLES = [
    "日" * 5,          # 3 UTF-8 bytes / 1 code point
    "🙂" * 4,          # 4 UTF-8 bytes / 1 code point (astral)
    "a日b🙂c",
    "e\u0301" * 3,     # combining acute: 2 code points each
    "Ǆx",
    "😀" * 1000,
    "日" * 700,
]


# ---------------------------------------------------------------------------
# 1. kimi-agent's own corpus
# ---------------------------------------------------------------------------
@pytest.mark.parametrize(("content", "text", "cap"), KIMI_AGENT_CASES)
def test_kimi_agent_corpus(content, text, cap):
    value, truncated = assert_parity(content, text, cap)
    # The reference's own assertions (test_bounded_append_over_cap_head_tail_marker).
    total = content + text
    assert truncated is (len(total) > cap)
    if truncated:
        head_len = int(cap * 0.4)
        tail_len = cap - head_len
        assert value.startswith(total[:head_len])
        assert value.endswith(total[len(total) - tail_len:])
        assert f"keeping first {head_len} and last {tail_len} chars" in value
        assert "(output truncated" in value
        assert len(value) < cap + 200


def test_head_tail_marker_bytes():
    """Byte-exact marker line, from the reference's f-string."""
    value, truncated = assert_parity("", "x" * 3000, 1000)
    assert truncated is True
    assert value == (
        "x" * 400
        + "\n[... (output truncated, keeping first 400 and last 600 chars)]\n"
        + "x" * 600
    )


def test_accumulates_then_truncates_like_the_reference():
    """First chunk under the cap is a no-op; the second rewrites to head+tail."""
    first, first_truncated = assert_parity("", "a" * 600, 1000)
    assert first_truncated is False and first == "a" * 600
    second, second_truncated = assert_parity(first, "b" * 600, 1000)
    assert second_truncated is True
    assert second.startswith("a" * 400)
    assert second.endswith("b" * 600)
    assert len(second) < 1200


# ---------------------------------------------------------------------------
# 2. boundary lengths
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("cap", [1, 2, 3, 5, 10, 100, 999, 1000, 2_000_000])
def test_exact_cap_boundary(cap):
    # len(content) + len(text) == cap  -> not truncated
    for content_len in {0, max(cap // 3, 0), max(cap - 1, 0), cap}:
        if content_len > cap:
            continue
        content = "a" * content_len
        text = "b" * (cap - content_len)
        value, truncated = assert_parity(content, text, cap)
        assert truncated is False
        assert value == content + text
    # cap + 1 -> truncated
    value, truncated = assert_parity("a" * cap, "b", cap)
    assert truncated is True
    assert "keeping first" in value


@pytest.mark.parametrize("cap", DEGENERATE_CAPS)
def test_degenerate_caps(cap):
    for content, text in [("", ""), ("", "a"), ("a", ""), ("a", "b"), ("a" * 5, "b" * 5)]:
        assert_parity(content, text, cap)


@pytest.mark.parametrize("cap", [0, 1, 2, 3, -1, -5])
def test_tiny_caps_marker_and_empty_tail(cap):
    value, truncated = assert_parity("", "hello", cap)
    head_len = int(cap * 0.4)
    tail_len = cap - head_len
    assert f"keeping first {head_len} and last {tail_len} chars" in value


# ---------------------------------------------------------------------------
# 3. Unicode: code points, never bytes
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("text", UNICODE_SAMPLES)
@pytest.mark.parametrize("cap", [0, 1, 2, 3, 4, 5, 8, 16, 100, 1000])
def test_unicode_caps_are_code_points(text, cap):
    value, truncated = assert_parity("", text, cap)
    # A cap counted in code points: the flag flips exactly at len(text) > cap,
    # and an astral/3-byte character is ONE unit even though its UTF-8 form is
    # 4/3 bytes (a byte counter would truncate far too early).
    assert truncated is (len(text) > cap)


def test_unicode_no_truncation_keeps_every_code_point():
    text = "日" * 8
    value, truncated = assert_parity("", text, 8)
    assert truncated is False and value == text
    assert cpp_append("", text, 8) == (text, False)
    # 8 code points but 24 bytes: a byte-based cap of 8 would have truncated.
    assert len(text.encode("utf-8")) == 24


def test_unicode_truncation_slices_on_code_points():
    text = "🙂" * 10  # 10 code points, 40 bytes
    value, truncated = assert_parity("", text, 4)
    assert truncated is True
    marker = "\n[... (output truncated, keeping first 1 and last 3 chars)]\n"
    assert value == "🙂" + marker + "🙂" * 3


def test_unicode_mixed_boundary():
    head, tail = "a日", "🙂b"
    value, truncated = assert_parity("", head + tail, 4)
    assert truncated is False
    assert value == head + tail
    value, truncated = assert_parity("", head + tail + "c", 4)
    assert truncated is True


# ---------------------------------------------------------------------------
# 4. the returned pair
# ---------------------------------------------------------------------------
def test_returns_str_bool_pair():
    result = TOOLS.bounded_append("a", "b", 1000)
    assert isinstance(result, tuple) and len(result) == 2
    assert isinstance(result[0], str) and isinstance(result[1], bool)


@pytest.mark.parametrize("cap", [0, 1, 5, 100, 1000, 1_000_000])
def test_flags_only_when_the_buffer_exceeds_the_cap(cap):
    for extra in (-1, 0, 1):
        total = cap + extra
        if total < 0:
            continue
        content = "x" * (total // 2)
        text = "y" * (total - total // 2)
        _, truncated = assert_parity(content, text, cap)
        assert truncated is (len(content) + len(text) > cap)


def test_large_cap_and_large_text():
    cap = 2_000_000
    text = "q" * (cap + 5)
    value, truncated = assert_parity("", text, cap)
    assert truncated is True
    assert value.startswith("q" * 800000)
    assert value.endswith("q" * 1200000)
    assert "keeping first 800000 and last 1200000 chars" in value


def test_real_cap_constant_round_trips():
    """BACKGROUND_MAX_OUTPUT_CHARS (2_000_000) is what the tools pass."""
    cap = background_utils.BACKGROUND_MAX_OUTPUT_CHARS
    assert cap == 2_000_000
    small = "ok\n"
    assert assert_parity("", small, cap) == ("ok\n", False)


# ---------------------------------------------------------------------------
# 5. fixed-seed fuzz
# ---------------------------------------------------------------------------
def test_fuzz_fixed_seed():
    alphabet = ["a", "b", "Z", "\n", " ", "日", "🙂", "é", "\u0301", "x" * 7, "0"]
    rng = random.Random(0xB0DDED)
    caps = [0, 1, 2, 3, 4, 5, 6, 9, 10, 17, 31, 64, 100, 999, 1000, -1, -3, -64]
    for _ in range(1500):
        cap = rng.choice(caps)
        content = "".join(rng.choice(alphabet) for _ in range(rng.randrange(0, 30)))
        text = "".join(rng.choice(alphabet) for _ in range(rng.randrange(0, 30)))
        assert_parity(content, text, cap)


def test_fuzz_accumulation_sequence():
    """A whole job's chunk stream against the in-place reference buffer."""
    rng = random.Random(20240924)
    alphabet = ["a", "b", "\n", "日", "🙂", "x" * 13]
    for _ in range(60):
        cap = rng.choice([10, 37, 100, 1000, 4096])
        chunks = ["".join(rng.choice(alphabet) for _ in range(rng.randrange(0, 40)))
                  for _ in range(rng.randrange(1, 12))]
        # Reference: one StringIO written chunk by chunk (the real call pattern).
        buf = io.StringIO()
        expected_flags = []
        with pure_python(background_utils):
            for chunk in chunks:
                expected_flags.append(bounded_append_ref(buf, chunk, cap))
        # C++: the (content, text, cap) contract, threaded by hand.
        content = ""
        for chunk, expected_flag in zip(chunks, expected_flags):
            content, got_flag = cpp_append(content, chunk, cap)
            assert got_flag is expected_flag
        assert content == buf.getvalue()
