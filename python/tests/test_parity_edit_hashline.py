"""Differential parity for the ``edit`` tool's hashline hash tables.

``src/builtin_tools/edit_tool.cpp`` carries its **own** copy of the Unicode
tables behind ``hash_line.py``'s two per-character predicates (``str.isalnum()``
-> ``has_significant`` -> the seed of a line, and ``str.isspace()`` -> the byte
filter), because ``builtin_tools`` must not link the ``runtime`` kernel.  The
``read`` tool embeds a third copy (``rd_alnum_ranges``).  The edit copy had been
corrupted exactly like the others: 91 values missing from U+066F onward, which
shifted every later ``(start, end)`` pair so the binary search matched huge fake
ranges (the emoji planes and U+E0100-U+E01EF variation selectors counted as
alphanumeric), plus a whitespace predicate missing Python's ASCII information
separators U+001C-U+001F.  Effect: ``edit`` rejected the ``LINE#HASH`` anchors
``read`` had just printed for any file whose first line was non-ASCII
("N lines have changed since last read", with nothing changed).

The C++ test suite (``tests/unit/builtin_tools/test_edit_tool.cpp``, target
``test_builtin_edit``) pins the fix; this module proves the *pinned vectors are
the Python reference's own values* and that no copy of the tables is stale:

* ``test_edit_hash_golden_matches_the_python_reference`` - every pinned
  ``(line_num, line) -> hash`` vector from the C++ suite is recomputed with the
  real ``kimi_cli.tools.file.hash_line.compute_line_hash`` (kimi-agent checkout,
  never ``kimix_native``'s ``_compat`` mirror);
* ``test_edit_hash_golden_matches_the_built_line_hash_kernel`` - the same
  vectors through the built ``runtime_py.tools.line_hash`` kernel (the recipe the
  edit tool re-implements);
* ``test_every_cpp_table_verifies_over_all_code_points`` - runs
  ``scripts/gen_line_hash_tables.py --verify``, which re-parses each checked-in
  table and compares it with ``str.isalnum`` / ``str.isspace`` over all 1 114 112
  code points;
* ``test_no_target_has_a_stale_generated_table`` / ``..._scan_finds_no_corrupt_table``
  - the ``--check`` / ``--scan`` guards that make a hand-edit impossible.
"""

from __future__ import annotations

import functools
import os
import re
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
# as python/tests/test_parity_line_hash.py.
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

from _parity_ref import KIMI_AGENT_ROOT, ref, ref_available  # noqa: E402

pytestmark = pytest.mark.skipif(
    not ref_available(), reason=f"kimi-agent checkout not found at {KIMI_AGENT_ROOT}"
)

TOOLS = runtime_py.tools
CXX_TEST = _REPO_ROOT / "tests" / "unit" / "builtin_tools" / "test_edit_tool.cpp"
GENERATOR = _REPO_ROOT / "scripts" / "gen_line_hash_tables.py"

#: files holding a copy of the hashline Unicode tables (generator TARGETS)
TABLE_FILES = (
    "src/runtime/tools/line_hash.cpp",
    "src/builtin_tools/edit_tool.cpp",
    "src/builtin_tools/read_tool.cpp",
)


@functools.lru_cache(maxsize=1)
def reference():
    """kimi-agent's ``hash_line`` module (imported once, on first use)."""
    return ref("kimi_cli.tools.file.hash_line")


# ---------------------------------------------------------------------------
# the pinned vectors, parsed straight out of the C++ suite
# ---------------------------------------------------------------------------
def _cxx_string(expr: str) -> str:
    """Decode a C++ string literal sequence (``"a\\x1c" "def"``) to text.

    The test file is ASCII-only, with non-ASCII/control bytes written as
    ``\\xNN`` escapes.  Adjacent literals are concatenated first (a hex digit
    right after ``\\xNN`` would otherwise be swallowed by the escape).
    """
    pieces = re.findall(r'"([^"]*)"', expr)
    assert pieces, f"no string literal in {expr!r}"
    raw = "".join(pieces)
    return raw.encode("ascii").decode("unicode_escape").encode("latin-1").decode("utf-8")


def pinned_goldens() -> list[tuple[int, str, str, str]]:
    """``(line_num, line, hash, label)`` for every entry of the C++ table."""
    text = CXX_TEST.read_bytes().decode("utf-8")
    block = re.search(r"const hash_golden goldens\[\] = \{(.*?)\n\s*\};", text, re.S)
    assert block is not None, (
        "the pinned golden table moved/renamed in tests/unit/builtin_tools/"
        "test_edit_tool.cpp - update this parser")
    body = block.group(1)
    entries = []
    for line in body.split("\n"):
        if "{" not in line:
            continue
        match = re.match(r'\s*\{(\d+),\s*(.+),\s*"([^"]*)",\s*"([^"]*)"\},?\s*$', line)
        assert match is not None, f"unparsed golden entry: {line!r}"
        entries.append((int(match.group(1)), _cxx_string(match.group(2)),
                        match.group(3), match.group(4)))
    # every line that opens an entry must have been parsed
    assert len(entries) == sum(1 for l in body.split("\n") if "{" in l)
    assert len(entries) >= 55, f"only {len(entries)} goldens parsed"
    return entries


GOLDENS = pinned_goldens()
GOLDEN_IDS = [f"L{n}-{label}" for n, _line, _h, label in GOLDENS]


# ---------------------------------------------------------------------------
# the vectors themselves
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("line_num,line,want,label", GOLDENS, ids=GOLDEN_IDS)
def test_edit_hash_golden_matches_the_python_reference(line_num, line, want, label):
    """The value ``edit::compute_line_hash`` is pinned to is Python's."""
    got = reference().compute_line_hash(line_num, line, None)
    assert got == want, (
        f"{label}: the C++ suite pins edit::compute_line_hash({line_num}, "
        f"{line!r}, None) == {want!r}, the Python reference says {got!r}")


@pytest.mark.parametrize("line_num,line,want,label", GOLDENS, ids=GOLDEN_IDS)
def test_edit_hash_golden_matches_the_built_line_hash_kernel(line_num, line, want, label):
    """The same vector through the built kernel the edit tool re-implements.

    ``runtime_py.tools.line_hash`` takes the *seed*, so this also proves the
    seed rule the edit tool derives (``HASH_SEED`` when the line has an
    alphanumeric character, else the 1-based line number) - the observable
    effect of the alnum table.
    """
    lookup = reference()._NIBBLE_LOOKUP
    body = line[:-1] if line.endswith("\r") else line
    has_significant = any((not c.isspace()) and c.isalnum() for c in body)
    seed = reference().HASH_SEED if has_significant else line_num
    got = TOOLS.line_hash(line.encode("utf-8"), seed)
    assert got == lookup.index(want), (
        f"{label}: line_hash({line!r}, seed={seed}) = {got}, the pinned C++ "
        f"golden {want!r} is index {lookup.index(want)}")


def test_read_anchors_for_a_non_ascii_file_are_the_reference_hashes():
    """The exact anchors the ``edit`` regression test feeds back into edit.

    ``tests/unit/builtin_tools/test_edit_tool.cpp`` builds its anchors with
    ``read::compute_line_hash_strings``; this pins what the reference says those
    anchors are, so the read side of the round trip is covered too.
    """
    module = reference()
    lines = ["\U0001f600", "\u4e2d\u6587", "plain ascii"]
    with_prev = []
    prev = None
    for i, line in enumerate(lines, 1):
        prev = module.compute_line_hash(i, line, prev)
        with_prev.append(prev)
    assert with_prev == ["PV", "SQ", "KR"], with_prev


# ---------------------------------------------------------------------------
# table provenance / staleness guards (all three copies)
# ---------------------------------------------------------------------------
def _run_generator(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(GENERATOR), *args],
                          cwd=str(_REPO_ROOT), capture_output=True, text=True)


def test_no_target_has_a_stale_generated_table():
    """``--check``: every checked-in table equals what the generator derives."""
    proc = _run_generator("--check")
    assert proc.returncode == 0, (
        "a checked-in hashline Unicode table is stale - run "
        f"`python scripts/gen_line_hash_tables.py`\nstdout:\n{proc.stdout}\n"
        f"stderr:\n{proc.stderr}")
    for path in TABLE_FILES:
        assert f"up to date: {path}" in proc.stdout, proc.stdout


def test_every_cpp_table_verifies_over_all_code_points():
    """``--verify`` re-reads the C++ files: sorted, non-overlapping, exact."""
    proc = _run_generator("--verify")
    assert proc.returncode == 0, (
        f"table verification failed:\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")
    for path in TABLE_FILES:
        assert f"{path} (" in proc.stdout, proc.stdout
    assert proc.stdout.count(
        "isalnum mismatches over U+0000..U+10FFFF (1114112 code points): 0") == 3, (
        proc.stdout)
    assert proc.stdout.count(
        "isspace mismatches over U+0000..U+10FFFF (1114112 code points): 0") == 3, (
        proc.stdout)


def test_scan_finds_no_corrupt_table():
    """``--scan`` catches any *other* stale copy under src/ (the class of bug)."""
    proc = _run_generator("--scan")
    assert proc.returncode == 0, (
        f"corrupt hashline table:\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")
    assert "0 corrupt" in proc.stdout, proc.stdout
    for name in ("ed_alnum_ranges", "ed_py_space_ranges", "rd_alnum_ranges",
                 "kAlnumRanges", "kPySpaceRanges"):
        assert name in proc.stdout, f"{name} was not scanned:\n{proc.stdout}"


def test_reference_module_is_the_kimi_agent_checkout():
    """Guard against a shadowed reference (the shim's own `_compat` mirror)."""
    module = reference()
    assert "kimi_cli" in module.__file__.replace("\\", "/")
    assert "hash_line.py" in module.__file__.replace("\\", "/")
    assert KIMI_AGENT_ROOT.as_posix() in Path(module.__file__).as_posix()
