"""Differential parity for the line-hash kernel (kimix-base <-> kimi-agent).

``src/runtime/tools/line_hash.cpp`` is the native port of kimi-agent's
``kimi_cli/tools/file/hash_line.py`` (the hashline / edit-anchor kernel).  It is
exposed to Python as ``runtime_py.tools.line_hash`` / ``line_hashes`` and reached
through the ``kimix_native.tools`` shim, so this module compares it against the
*real* reference from the kimi-agent checkout (``C:/dev/kimi-agent``, override
with ``KIMI_AGENT_ROOT``) rather than against the shim's own ``_compat`` mirror:

* ``compute_line_hash(line_num, line, prev_hash)`` (line 54) - the per-line
  recipe: strip one trailing ``\\r``, drop Python-whitespace characters, seed =
  nibble-decoded previous hash, else ``HASH_SEED`` when the line has an
  alphanumeric character, else the 1-based line number, then ``xxh32 & 0xFF``;
* ``_cumulative_hashes(file_lines)`` (line 89) - the chained scan, forced onto
  its pure-Python body with ``_parity_ref.pure_python`` (the reference would
  otherwise short-circuit into *its* native library, which proves nothing).

Two table bugs found by this module (both fixed; the failing cases are kept as
regression tests below):

1. ``kAlnumRanges`` was corrupt from U+066F on (91 values had been dropped, so
   every later ``(start, end)`` pair was shifted and the binary search matched
   huge fake ranges - the emoji planes counted as alphanumeric).  That flipped
   ``has_significant`` and therefore the seed of a line, e.g.
   ``compute_line_hashes("\\U0001f600")``: reference ``['PV']``, port ``['XJ']``.
2. ``is_py_space_cp`` / the ASCII fast path omitted U+001C-U+001F, which *are*
   ``str.isspace()`` in Python (they are not Unicode Zs, so they are easy to
   miss).  ``line_hash("\\x1c", 1)``: reference 146, port 33.

Table provenance: both tables are generated from the running CPython's own
Unicode database by ``scripts/gen_line_hash_tables.py`` (``--check`` guards
against a stale check-in, ``--verify`` re-proves them over every code point);
the tests at the bottom of this module run both, so a hand-edit cannot survive.
"""

from __future__ import annotations

import functools
import os
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------------------
# Import the freshly built extension BEFORE anything imports kimi_cli.
#
# ``kimi_cli.native_loader`` stages ITS OWN native library: it inserts
# ``<kimi-agent>/bin`` at ``sys.path[0]``, where a *released* ``runtime_py.pyd``
# lives.  Any ``import runtime_py`` after that resolves to that foreign library
# and silently compares the port against an older build of itself.  Same guard
# as python/tests/test_parity_bash.py.
# ---------------------------------------------------------------------------
_REPO_ROOT = Path(__file__).resolve().parents[2]
_BIN_DIR = None
for _mode in ("release", "releasedbg", "debug", "check"):
    _cand = _REPO_ROOT / "bin" / _mode
    if (_cand / "runtime_py.pyd").is_file() or (_cand / "runtime_py.so").is_file():
        _BIN_DIR = _cand
        break
if _BIN_DIR is not None:
    _bin_str = str(_BIN_DIR)
    while _bin_str in sys.path:
        sys.path.remove(_bin_str)
    sys.path.insert(0, _bin_str)

import runtime_py  # noqa: E402

if _BIN_DIR is None:  # pragma: no cover - defensive
    pytest.skip("no kimix-base runtime_py build found under bin/",
                allow_module_level=True)
_NATIVE_PATH = Path(runtime_py.__file__).resolve()
assert _BIN_DIR.resolve() in _NATIVE_PATH.parents, (
    f"runtime_py came from {_NATIVE_PATH}, not the kimix-base build {_BIN_DIR} "
    f"- a staged copy shadowed it; parity results would be bogus")

from _parity_ref import KIMI_AGENT_ROOT, pure_python, ref, ref_available  # noqa: E402

pytestmark = pytest.mark.skipif(
    not ref_available(), reason=f"kimi-agent checkout not found at {KIMI_AGENT_ROOT}"
)

TOOLS = runtime_py.tools
HASH_SEED = 0


@functools.lru_cache(maxsize=1)
def reference():
    """kimi-agent's ``hash_line`` module (imported once, on first use)."""
    return ref("kimi_cli.tools.file.hash_line")


@functools.lru_cache(maxsize=1)
def shim():
    """This checkout's ``kimix_native.tools`` shim."""
    import kimix_native.tools as tools

    return tools


def nibble_lookup(module) -> list[str]:
    return module._NIBBLE_LOOKUP


def py_hash_value(module, line_num: int, line: str, prev_hash: str | None) -> int:
    """The reference's hash as the raw 0..255 integer (lookup index)."""
    return module._NIBBLE_LOOKUP.index(module.compute_line_hash(line_num, line, prev_hash))


def py_seed(line: str, line_num: int, prev_hash: str | None, seed: int = HASH_SEED) -> int:
    """The reference's seed rule, re-derived from the reference's own predicates."""
    if prev_hash is not None:
        acc = 0
        for c in prev_hash:
            acc = ((acc * 256) + ord(c)) & 0xFFFFFFFF
        return acc
    has_significant = any((not c.isspace()) and c.isalnum() for c in line.rstrip("\r"))
    return seed if has_significant else line_num


def kernel_lines(content: str) -> list[str]:
    """The line stream the kernel sees: split on ``"\\n"``, a trailing ``"\\n"``
    ends the file.

    This is the kernel's documented contract (``compute_line_hashes`` header)
    and how the reference feeds it: ``_cumulative_hashes`` calls
    ``_NATIVE_TOOLS.compute_line_hashes("\\n".join(file_lines))``.  It is *not*
    ``str.splitlines()``, which also breaks on U+000B/U+000C/U+001C-U+001F/
    U+0085/U+2028/U+2029.
    """
    if not content:
        return []
    lines = content.split("\n")
    if content.endswith("\n"):
        lines.pop()
    return lines


def content_of(lines: list[str]) -> str:
    """File text whose :func:`kernel_lines` is exactly *lines*."""
    if not lines:
        return ""
    text = "\n".join(lines)
    if lines[-1] == "":
        # "\n".join(["a", ""]) == "a\n" already terminates the file, so the
        # trailing empty line needs its own terminator.
        text += "\n"
    assert kernel_lines(text) == list(lines), (lines, text)
    return text


def accumulative_hashes(module, lines: list[str]) -> list[str]:
    """Reference ``_cumulative_hashes`` forced onto its pure-Python body.

    Without ``pure_python`` the reference short-circuits into *its* native
    library, and the comparison would only prove that two native builds agree.
    """
    with pure_python(module):
        return module._cumulative_hashes(lines)


# ---------------------------------------------------------------------------
# corpus: ASCII, CJK, astral, emoji, combining marks, whitespace-only, control
# characters (including the C1 block and U+001C-U+001F), NEL, mixed text.
# ---------------------------------------------------------------------------
LINE_CORPUS = [
    pytest.param("hello world", id="ascii-lower"),
    pytest.param("HELLO", id="ascii-upper"),
    pytest.param("12345", id="digits"),
    pytest.param("!!!", id="punctuation-only"),
    pytest.param("\u4e2d\u6587\u6d4b\u8bd5", id="cjk"),
    pytest.param("\u4e2d \u6587", id="cjk-with-space"),
    pytest.param("\U0001f600", id="emoji"),
    pytest.param("a\U0001f600b", id="emoji-mixed"),
    pytest.param("\U0001f468\u200d\U0001f469\u200d\U0001f467", id="emoji-zwj-family"),
    pytest.param("\U0001d400\U0001d401", id="astral-math-letters"),
    pytest.param("e\u0301", id="combining-e-acute"),
    pytest.param("\u0301\u0301", id="combining-marks-only"),
    pytest.param("\u4e2d\u0301", id="combining-on-cjk"),
    pytest.param("   ", id="spaces-only"),
    pytest.param("\t\t", id="tabs-only"),
    pytest.param("\u00a0", id="nbsp"),
    pytest.param("\u3000", id="ideographic-space"),
    pytest.param("\u0085", id="nel-85"),
    pytest.param("\x1c", id="info-separator-1c"),
    pytest.param("\x1d", id="info-separator-1d"),
    pytest.param("\x1e", id="info-separator-1e"),
    pytest.param("\x1f", id="info-separator-1f"),
    pytest.param("\x01", id="ctrl-01"),
    pytest.param("\x7f", id="ctrl-7f"),
    pytest.param("\x80", id="c1-80"),
    pytest.param("\x9f", id="c1-9f"),
    pytest.param("abc\x1cdef", id="ctrl-inside-text"),
    pytest.param("\u0627\u0628\u062a", id="arabic"),
    pytest.param("\u066f", id="arabic-letter-066f"),
    pytest.param("\u066f\u0670", id="arabic-066f-0670"),
    pytest.param("\u06d3\u06d4", id="arabic-06d3-06d4"),
    pytest.param("\u0915\u0916", id="devanagari"),
    pytest.param("\u0e50\u0e51", id="thai-digits"),
    pytest.param("\u2160\u2161", id="roman-numerals"),
    pytest.param("\u00b2\u00b3", id="superscript-digits"),
    pytest.param("\uff21\uff22", id="fullwidth-latin"),
    pytest.param("\uff66\uff67", id="halfwidth-kana"),
    pytest.param("\ud55c\uae00", id="hangul"),
    pytest.param("\u2500\u2502", id="box-drawing"),
    pytest.param("\u2192", id="arrow"),
    pytest.param("\u20ac", id="currency-sign"),
    pytest.param("\U00020000", id="cjk-ext-b"),
    pytest.param("\ufe0f", id="variation-selector"),
    pytest.param("\u200d", id="zwj"),
    pytest.param("\u200b", id="zero-width-space-200b"),
    pytest.param("abc\r", id="trailing-cr"),
    pytest.param("", id="empty"),
    pytest.param("\U0001f600\u4e2d\x1c\U0001d400", id="kitchen-sink"),
]

#: Contents (not line lists) for the chained kernel.
CONTENT_CORPUS = [
    pytest.param("alpha\nbeta\ngamma", id="ascii"),
    pytest.param("\u4e2d\u6587\n\u6d4b\u8bd5", id="cjk"),
    pytest.param("\U0001f600\n\U0001f600", id="emoji"),
    pytest.param("\x1c\n\x1d\n\x1e\n\x1f", id="info-separators"),
    pytest.param("a\n\u4e2d\u6587\n\U0001f600\n\x1c\n\n  ", id="mixed-with-blank"),
    pytest.param("e\u0301\n\u0301", id="combining"),
    pytest.param("\U0001d400\n\U0001f600", id="astral"),
    pytest.param("\n\n", id="empty-lines"),
    pytest.param("a\n\n", id="trailing-empty-line"),
    pytest.param("", id="empty-content"),
    pytest.param("\n", id="single-newline"),
    pytest.param("line1\r\nline2\r\n", id="crlf"),
    pytest.param("no trailing newline", id="no-trailing-newline"),
    pytest.param("\u3000\n\u00a0\n\u0085", id="whitespace-only-lines"),
]

#: kimi-agent's own parametrisation of
#: ``test_cumulative_hashes_equivalence`` (kimi-cli/tests/native/
#: test_diff_glob_tools_equivalence.py), including the trailing-empty-line cases
#: that only ever reach the reference's *pure-Python* branch.
REFERENCE_CUMULATIVE_CASES = [
    pytest.param([], id="empty-list"),
    pytest.param([""], id="one-empty"),
    pytest.param(["", ""], id="two-empty"),
    pytest.param(["a"], id="one"),
    pytest.param(["a", "b"], id="two"),
    pytest.param(["a", ""], id="a-empty"),
    pytest.param(["a", "b", ""], id="a-b-empty"),
    pytest.param(["", "abc"], id="empty-abc"),
    pytest.param(["abc", "  ", "def"], id="abc-spaces-def"),
    pytest.param([" ", "\t", ""], id="ws-ws-empty"),
    pytest.param(["a\r", "b"], id="carriage-return"),
    pytest.param(["line1", "line2", "line3"], id="numbered"),
    pytest.param(["h\u00e9llo", "\u4e16\u754c", "\U0001f389"], id="unicode"),
    pytest.param(["", "a", "", "b", ""], id="interleaved-empty"),
    pytest.param(["\t", "x"], id="tab-x"),
    pytest.param(["one"], id="one-word"),
    pytest.param(["a", "b", "c", "d", "e"], id="five"),
    pytest.param(["\x1c", "\x1d"], id="info-separators"),
    pytest.param(["\U0001d400", "\U0001f600"], id="astral"),
]


# ---------------------------------------------------------------------------
# per-line kernel
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("line", LINE_CORPUS)
@pytest.mark.parametrize("line_num", [1, 7, 255])
def test_line_hash_matches_reference(line, line_num):
    """``runtime_py.tools.line_hash`` vs ``hash_line.compute_line_hash``.

    The reference's first-line seed depends on ``str.isalnum`` (through
    ``has_significant``) and on ``str.isspace`` (through the filter) - the two
    tables that were corrupt.
    """
    module = reference()
    seed = py_seed(line, line_num, None)
    want = py_hash_value(module, line_num, line, None)
    got = TOOLS.line_hash(line.encode("utf-8"), seed)
    assert got == want, (
        f"line_hash({line!r}, seed={seed}) = {got}, reference "
        f"compute_line_hash({line_num}, {line!r}, None) = {want}"
    )


@pytest.mark.parametrize("line", LINE_CORPUS)
@pytest.mark.parametrize("prev_hash", ["ZP", "HH", "00"])
def test_line_hash_with_prev_hash_seed(line, prev_hash):
    """The nibble-decoded previous-hash seed path (``prev_hash is not None``)."""
    module = reference()
    seed = py_seed(line, 3, prev_hash)
    want = py_hash_value(module, 3, line, prev_hash)
    got = TOOLS.line_hash(line.encode("utf-8"), seed)
    assert got == want, (
        f"line_hash({line!r}, seed from {prev_hash!r} = {seed}) = {got}, "
        f"reference compute_line_hash(3, {line!r}, {prev_hash!r}) = {want}"
    )


@pytest.mark.parametrize("line", LINE_CORPUS)
def test_single_line_chain_uses_the_reference_seed(line):
    """The seed choice itself (has_significant) must be the reference's.

    ``compute_line_hashes`` derives the first line's seed internally; the
    reference's seed is either ``HASH_SEED`` or the 1-based line number.  This
    is the purest probe of ``kAlnumRanges``: an emoji line is *not*
    alphanumeric, so it must hash with seed 1, not 0.
    """
    module = reference()
    content = content_of([line])
    want = py_hash_value(module, 1, line, None)
    got = list(TOOLS.line_hashes(content.encode("utf-8"), HASH_SEED))
    assert got == [want], (
        f"line_hashes({content!r}) = {got}, reference "
        f"_cumulative_hashes([{line!r}]) = {[want]}"
    )


# ---------------------------------------------------------------------------
# chained kernel
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("content", CONTENT_CORPUS)
def test_chained_line_hashes_match_reference(content):
    module = reference()
    lines = kernel_lines(content)
    want = [module._NIBBLE_LOOKUP.index(h) for h in accumulative_hashes(module, lines)]
    got = list(TOOLS.line_hashes(content.encode("utf-8"), HASH_SEED))
    assert got == want, (
        f"line_hashes({content!r}) = {got}, reference "
        f"_cumulative_hashes({lines!r}) = {want}"
    )


@pytest.mark.parametrize("lines", REFERENCE_CUMULATIVE_CASES)
def test_kimi_agent_own_cumulative_hash_cases(lines):
    """kimi-agent's ``CUMULATIVE_HASH_CASES``, run against our kernel."""
    module = reference()
    content = content_of(lines)
    assert kernel_lines(content) == list(lines)
    want = [module._NIBBLE_LOOKUP.index(h) for h in accumulative_hashes(module, lines)]
    got = list(TOOLS.line_hashes(content.encode("utf-8"), HASH_SEED))
    assert got == want, (
        f"for file_lines={lines!r} (content={content!r}): kernel={got}, "
        f"reference={want}"
    )


@pytest.mark.parametrize("content", CONTENT_CORPUS)
def test_compute_line_hashes_shim_matches_reference(content):
    """The reference-shaped entry point (2-char nibble strings), through the shim."""
    module = reference()
    lines = kernel_lines(content)
    want = accumulative_hashes(module, lines)
    got = shim().compute_line_hashes(content)
    assert got == want, (
        f"kimix_native.tools.compute_line_hashes({content!r}) = {got}, "
        f"reference _cumulative_hashes({lines!r}) = {want}"
    )


def test_line_hash_accepts_bytes_and_the_kernel_strips_one_cr():
    module = reference()
    for raw in (b"abc\r", b"abc\r\n", b"abc", "abc\r".encode()):
        seed = py_seed("abc\r", 5, None)
        want = py_hash_value(module, 5, "abc\r", None)
        assert TOOLS.line_hash(raw, seed) == want


# ---------------------------------------------------------------------------
# exhaustive table coverage (the two tables are generated; this is the guard
# that closes the class of bug that caused the corruption)
# ---------------------------------------------------------------------------
def _reference_range_edges() -> list[int]:
    """Every code point where ``str.isalnum()`` or ``str.isspace()`` changes."""
    edges = set()
    for predicate in (str.isalnum, str.isspace):
        for cp in range(0x110000):
            if 0xD800 <= cp <= 0xDFFF:
                continue
            if predicate(chr(cp)):
                edges.add(cp)
                edges.add(cp + 1)     # the first code point that is *not* in
    return sorted(edges)


def _kernel_probe_points() -> list[int]:
    """Code points the kernel sweep checks.

    Exhaustive coverage of *both* tables over all 1 114 112 code points is the
    generator's job (``test_generated_tables_verify_over_all_code_points`` reads
    the checked-in constants and compares them with ``str.isalnum`` /
    ``str.isspace`` for every code point, in ~1 s).  This test drives the same
    predicates through the *built* kernel instead, which is far more expensive
    per call, so it concentrates on

    * every code point adjacent to a predicate transition (a shifted, dropped or
      duplicated range entry always breaks at one of these), and
    * a fixed stride sample (every 997th code point, plus the plane boundaries)
      so an interior-only error cannot hide.
    """
    points = set(_reference_range_edges())
    points.update(range(0, 0x110000, 997))
    points.update(range(0, 0x110000, 0x10000))     # plane starts
    points.update(range(0x10FFC0, 0x110000))       # top of the last plane
    return sorted(cp for cp in points if not 0xD800 <= cp <= 0xDFFF)


def test_kernel_matches_python_predicates_at_every_table_edge():
    """``is_alnum_cp`` / ``is_py_space_cp`` (as observed through the kernel) vs
    ``str.isalnum()`` / ``str.isspace()`` at every transition and a stride sample.

    ``has_significant`` (the only observable use of ``kAlnumRanges``) decides the
    seed of a *first* line: seed 0 when the line has an alphanumeric character,
    else the 1-based line number.  A single ``line_hashes(chr(cp))`` call
    therefore reveals both whether the whitespace filter dropped the character
    and whether the alnum table claimed it, so the sweep proves both tables
    against the reference predicates.

    Surrogates are skipped: ``str.encode("utf-8")`` cannot represent them, so the
    reference's own ``compute_line_hash`` raises for them too.
    """
    import xxhash

    points = _kernel_probe_points()
    bad = []
    for cp in points:
        char = chr(cp)
        is_space = char.isspace()
        has_significant = (not is_space) and char.isalnum()
        data = ("" if is_space else char).encode("utf-8")
        want = xxhash.xxh32(data, HASH_SEED if has_significant else 1).intdigest() & 0xFF
        got = TOOLS.line_hashes(char.encode("utf-8"), HASH_SEED)
        if got != [want]:
            bad.append((cp, is_space, has_significant, got, [want]))
    assert not bad, (
        f"{len(bad)} of {len(points)} probed code points hash differently from the "
        "reference, first 5: "
        + ", ".join(
            f"U+{cp:04X} isspace={sp} isalnum={al}: got {g} want {w}"
            for cp, sp, al, g, w in bad[:5])
    )


def test_generated_tables_are_not_stale():
    """The checked-in tables must equal what the generator derives right now."""
    script = _REPO_ROOT / "scripts" / "gen_line_hash_tables.py"
    proc = subprocess.run([sys.executable, str(script), "--check"],
                          cwd=str(_REPO_ROOT), capture_output=True, text=True)
    assert proc.returncode == 0, (
        "the generated Unicode tables in src/runtime/tools/line_hash.cpp are "
        "stale - run `python scripts/gen_line_hash_tables.py`\n"
        f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
    )


def test_generated_tables_verify_over_all_code_points():
    """``--verify`` re-parses the C++ file: sorted, non-overlapping, exact."""
    script = _REPO_ROOT / "scripts" / "gen_line_hash_tables.py"
    proc = subprocess.run([sys.executable, str(script), "--verify"],
                          cwd=str(_REPO_ROOT), capture_output=True, text=True)
    assert proc.returncode == 0, (
        f"table verification failed:\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")
    assert "isalnum mismatches over U+0000..U+10FFFF (1114112 code points): 0" in proc.stdout
    assert "isspace mismatches over U+0000..U+10FFFF (1114112 code points): 0" in proc.stdout


def test_reference_module_is_the_kimi_agent_checkout():
    """Guard against a shadowed reference (the shim's own `_compat` mirror)."""
    module = reference()
    assert "kimi_cli" in module.__file__.replace("\\", "/")
    assert "hash_line.py" in module.__file__.replace("\\", "/")
    assert KIMI_AGENT_ROOT.as_posix() in Path(module.__file__).as_posix()
