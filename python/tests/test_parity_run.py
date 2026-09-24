"""Differential parity tests for the ``Run`` builtin tool (kimix-base <-> kimi-agent).

``src/builtin_tools/run_tool.cpp`` ports kimi-agent's ``kimix/tools/file/run.py``
(plus the kernels Run reuses from ``common.py`` / ``output_enhance.py``).  This
module proves, live, that the committed golden vectors in
``tests/unit/builtin_tools/run_goldens.inc`` really come from the Python
reference, and compares every kernel that ``runtime_py`` exposes against the
same reference code:

* ``shlex.split`` / ``shlex.quote`` / ``shlex.join`` (the command-decomposition
  and display-command kernels) -- fuzzed against the host CPython.
* ``run.py _cd_prefix`` (the shell-delegation prefix).
* ``common.py _dedup_output`` + ``_truncate_lines`` (the portable stages of
  ``_token_filter_output``, which ``run_tool.cpp`` ports as ``shape_output``)
  and ``_find_error_line_index``.
* ``output_enhance.py interpret_exit_code`` / ``is_expected_exit`` /
  ``annotate_failure`` (exit-code classification, the "expected exit" cases
  grep/diff/SIGPIPE and the failure hint).

Provenance rules (same hazards as ``test_parity_bash.py``):

* ``kimi_cli.native_loader`` inserts ``<kimi-agent>/bin`` (a staged, older
  ``runtime_py.pyd``) at ``sys.path[0]``, so the kimix-base extension is
  imported *first* and its ``__file__`` asserted.
* ``common.py`` is loaded by path and its native gates (``_NATIVE_STREAM`` /
  ``_native_use_native``) are disabled, so the ground truth is the pure-Python
  body -- ``kimi_native``'s ``_compat`` mirrors are never the reference.
* The corpora live in ``scripts/gen_run_data.py`` (the generator that produced
  the .inc), so the test and the goldens can never drift apart.

Known, documented deviations pinned at the bottom of this file:
* ``micro_compress`` / the ``rich`` ANSI parser are not ported (Run's C++
  pipeline starts at the dedup stage).
* the C++ ``annotate_failure`` used by Run (``runtime/tools/shell_safety.cpp``)
  is exact for ASCII only; the bash-owned wrapper adds an ASCII gate that the
  pure Python reference does not have.
"""

from __future__ import annotations

import asyncio
import importlib.util
import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

_KIMIX_BASE_ROOT = Path(__file__).resolve().parents[2]
_TESTS_DIR = Path(__file__).resolve().parent
_GOLDENS = _KIMIX_BASE_ROOT / "tests" / "unit" / "builtin_tools" / "run_goldens.inc"
_GENERATOR = _KIMIX_BASE_ROOT / "scripts" / "gen_run_data.py"


def _kimix_base_bin_dir():
    """The kimix-base build directory holding ``runtime_py`` (conftest's order)."""
    for mode in ("release", "releasedbg", "debug", "check"):
        cand = _KIMIX_BASE_ROOT / "bin" / mode
        if cand.is_dir() and any(cand.glob("runtime_py.*")):
            return cand
    for cand in sorted((_KIMIX_BASE_ROOT / "bin").glob("*")):
        if cand.is_dir() and any(cand.glob("runtime_py.*")):
            return cand
    return None


# IMPORT ORDER IS LOAD-BEARING: the freshly built kimix-base extension must be
# imported before anything pulls in kimi_cli (see the module docstring).
_BIN_DIR = _kimix_base_bin_dir()
if _BIN_DIR is not None:
    sys.path.insert(0, str(_BIN_DIR))
import runtime_py  # noqa: E402

if _BIN_DIR is None:  # pragma: no cover - defensive
    pytest.skip("no kimix-base runtime_py build found under bin/",
                allow_module_level=True)

_NATIVE_PATH = Path(runtime_py.__file__).resolve()
assert _BIN_DIR.resolve() in _NATIVE_PATH.parents, (
    f"runtime_py came from {_NATIVE_PATH}, not the kimix-base build {_BIN_DIR} "
    "-- a staged copy shadowed it; parity results would be bogus")

from _parity_ref import KIMI_AGENT_ROOT, ref_available  # noqa: E402

pytestmark = pytest.mark.skipif(
    not ref_available(), reason="kimi-agent checkout not available"
)

SHELL = runtime_py.builtin_tools.shell

_GEN = importlib.util.spec_from_file_location("_parity_run_gen", _GENERATOR)
assert _GEN is not None and _GEN.loader is not None
gen = importlib.util.module_from_spec(_GEN)
sys.modules["_parity_run_gen"] = gen
_GEN.loader.exec_module(gen)
c_unescape_bytes = gen.c_unescape_bytes


def _reference():
    if not hasattr(_reference, "_cached"):
        _reference._cached = gen.Reference(KIMI_AGENT_ROOT)
    return _reference._cached


# ---------------------------------------------------------------------------
# .inc parsing (the committed golden file, not the generator's output)
# ---------------------------------------------------------------------------


def _rows(text: str, start: str, end: str) -> list[str]:
    body = text.split(start, 1)[1].split(end, 1)[0]
    return [ln.strip() for ln in body.splitlines() if ln.strip().startswith("{")]


def _split_fields(row: str) -> list[str]:
    """Split one ``{a, b, {...}}`` row into top-level fields."""
    body = row.strip()
    assert body.startswith("{") and body.endswith("},"), row
    body = body[1:-2]
    fields: list[str] = []
    current: list[str] = []
    i = 0
    depth = 0
    in_string = False
    while i < len(body):
        c = body[i]
        if in_string:
            current.append(c)
            if c == "\\":
                current.append(body[i + 1])
                i += 2
                continue
            if c == '"':
                in_string = False
            i += 1
            continue
        if c == '"':
            in_string = True
            current.append(c)
        elif c == "{":
            depth += 1
            current.append(c)
        elif c == "}":
            depth -= 1
            current.append(c)
        elif c == "," and depth == 0:
            fields.append("".join(current).strip())
            current = []
        else:
            current.append(c)
        i += 1
    fields.append("".join(current).strip())
    return fields


def _text(field: str) -> str:
    """Unescape a (possibly concatenated) C++ literal into a Python str.

    Multi-byte code points are split across adjacent literals by the generator,
    so the bytes are joined before decoding.
    """
    raw = b"".join(c_unescape_bytes(m.group(1)) for m in gen._C_STR.finditer(field))
    return raw.decode("utf-8", "surrogateescape")


def _int(field: str) -> int:
    return int(field)


def _flag(field: str) -> bool:
    return field.strip() == "true"


def _array(field: str, name: str) -> list[str]:
    inner = field.strip()
    assert inner.startswith("{") and inner.endswith("}"), field
    inner = inner[1:-1].strip()
    if not inner:
        return []
    return [_text(part) for part in _split_fields("{" + inner + "},") if part != "nullptr"]


@pytest.fixture(scope="module")
def goldens() -> str:
    return _GOLDENS.read_text(encoding="utf-8", newline="")


# ---------------------------------------------------------------------------
# 1. the committed goldens are exactly what the Python reference produces
# ---------------------------------------------------------------------------


def test_shlex_goldens_come_from_cpython_shlex(goldens):
    rows = _rows(goldens, "const shlex_golden kShlexGoldens[] = {", "\n};")
    assert len(rows) > 200, f"expected a large shlex corpus, got {len(rows)}"
    import shlex

    for row in rows:
        fields = _split_fields(row)
        text = _text(fields[0])
        posix = _flag(fields[1])
        expect_ok = _flag(fields[2])
        error = _text(fields[3])
        tokens = _array(fields[4], "tokens")
        count = _int(fields[5])
        try:
            got = shlex.split(text, posix=posix)
        except ValueError as exc:
            assert not expect_ok, f"{text!r} posix={posix} raised {exc}"
            assert str(exc) == error, f"{text!r}: {exc} != {error}"
            continue
        assert expect_ok, f"{text!r} posix={posix} unexpectedly parsed"
        assert error == ""
        assert len(got) == count, f"{text!r} posix={posix}: {len(got)} != {count}"
        assert got[:8] == tokens, f"{text!r} posix={posix}"


def test_quote_and_join_goldens_come_from_cpython_shlex(goldens):
    import shlex

    for row in _rows(goldens, "const quote_golden kQuoteGoldens[] = {", "\n};"):
        fields = _split_fields(row)
        assert shlex.quote(_text(fields[0])) == _text(fields[1]), row
    for row in _rows(goldens, "const join_golden kJoinGoldens[] = {", "\n};"):
        fields = _split_fields(row)
        argv = _array(fields[0], "argv")
        assert _int(fields[1]) == len(argv), row
        assert shlex.join(argv) == _text(fields[2]), row


def test_cd_goldens_come_from_run_py(goldens):
    run = _reference().run
    rows = _rows(goldens, "const cd_golden kCdGoldens[] = {", "\n};")
    assert len(rows) >= 15
    for row in rows:
        fields = _split_fields(row)
        cwd = _text(fields[0])
        shell = _text(fields[1])
        assert run._cd_prefix(cwd or None, shell) == _text(fields[2]), row


def test_shape_goldens_come_from_the_reference_pipeline_stages(goldens):
    reference = _reference()
    rows = _rows(goldens, "const shape_golden kShapeGoldens[] = {", "\n};")
    assert len(rows) >= 60, f"expected a broad shape corpus, got {len(rows)}"
    for row in rows:
        fields = _split_fields(row)
        text = _text(fields[0])
        max_lines = _int(fields[1])
        expected = _text(fields[2])
        changed = _flag(fields[3])
        got, changed_got = reference.shape(text, None if max_lines < 0 else max_lines)
        assert got == expected, f"shape golden mismatch for {text[:60]!r}"
        assert changed_got == changed, f"changed flag mismatch for {text[:60]!r}"


def test_exit_goldens_come_from_output_enhance(goldens):
    reference = _reference()
    oe = reference.output_enhance
    rows = _rows(goldens, "const exit_golden kExitGoldens[] = {", "\n};")
    assert len(rows) >= 20
    for row in rows:
        fields = _split_fields(row)
        command = _text(fields[0])
        code = _int(fields[2]) if _flag(fields[1]) else None
        meaning_field = fields[3]
        expected = _flag(fields[4])
        meaning = oe.interpret_exit_code(command, code)
        if meaning_field == "nullptr":
            assert meaning is None, f"{command} exit={code}: {meaning!r}"
        else:
            assert meaning == _text(meaning_field), command
        assert oe.is_expected_exit(command, code) is expected, f"{command} {code}"
        success = code == 0
        ok_message = ("success" if success
                      else (meaning if meaning is not None else "expected non-zero exit"))
        assert _text(fields[5]) == ok_message, row
        assert _text(fields[6]) == "failed", row


def test_annotate_goldens_come_from_output_enhance(goldens):
    oe = _reference().output_enhance
    rows = _rows(goldens, "const annotate_golden kAnnotateGoldens[] = {", "\n};")
    assert len(rows) >= 10
    for row in rows:
        fields = _split_fields(row)
        output = _text(fields[0])
        command = _text(fields[1])
        code = _int(fields[3]) if _flag(fields[2]) else None
        hint = oe.annotate_failure(output, command, code)
        if fields[4] == "nullptr":
            assert hint is None, f"{output!r}: {hint!r}"
        else:
            assert hint == _text(fields[4]), f"{output!r}"


def test_generator_check_mode_reports_up_to_date():
    import subprocess

    proc = subprocess.run(
        [sys.executable, str(_GENERATOR), "--check"],
        cwd=str(_KIMIX_BASE_ROOT), capture_output=True, text=True,
    )
    assert proc.returncode == 0, proc.stderr or proc.stdout
    assert "is up to date" in proc.stdout


def test_shape_goldens_equal_the_full_reference_pipeline(goldens):
    """For every vector the ported stages own, the full Run pipeline agrees."""
    reference = _reference()
    rows = _rows(goldens, "const shape_golden kShapeGoldens[] = {", "\n};")
    checked = 0
    for row in rows:
        fields = _split_fields(row)
        text = _text(fields[0])
        max_lines = _int(fields[1])
        max_lines = None if max_lines < 0 else max_lines
        portable, _changed = reference.shape(text, max_lines)
        pipelined = asyncio.run(reference.pipeline(text, max_lines))
        if pipelined != portable:
            continue  # micro_compress territory - pinned by the next test
        assert portable == _text(fields[2])
        checked += 1
    assert checked >= 50, f"only {checked} vectors matched the full pipeline"


def test_micro_compress_divergence_is_documented():
    """The one stage family the C++ pipeline does not port, pinned explicitly.

    ``_token_filter_output`` strips ANSI via ``rich`` and then runs
    ``micro_compress`` (banner drop / prefix fold / whitespace-only-line
    removal).  The C++ kernel starts at the dedup stage, so for these inputs the
    reference output is the *compressed* one while the C++ keeps the text.
    """
    reference = _reference()

    crlf = "a\r\nb\r\n"
    portable, _ = reference.shape(crlf, None)
    assert portable == "a\nb"  # what shape_output()/dedup_output() produces
    assert asyncio.run(reference.pipeline(crlf, None)) == ""  # micro_compress

    spaced = "trailing spaces   \ntrailing spaces   \n"
    assert reference.shape(spaced, None)[0] == "trailing spaces   \ntrailing spaces   "
    # micro_compress strips the trailing blanks and folds the repeated line.
    assert asyncio.run(reference.pipeline(spaced, None)) == "trailing spaces\ntrailing spaces"


# ---------------------------------------------------------------------------
# 2. kernels exposed through runtime_py.builtin_tools.shell
# ---------------------------------------------------------------------------


def _py_truncate(reference, text: str, max_lines: int) -> str:
    return reference.common._truncate_lines(
        text, max_lines, preserve_errors=True, error_context_lines=2
    )


def test_cpp_truncate_lines_matches_reference(goldens):
    reference = _reference()
    cases: list[tuple[str, int]] = []
    for row in _rows(goldens, "const shape_golden kShapeGoldens[] = {", "\n};"):
        fields = _split_fields(row)
        max_lines = _int(fields[1])
        if max_lines >= 0:
            cases.append((_text(fields[0]), max_lines))
    for text, max_lines in gen.SHAPE_CASES:
        if max_lines is not None:
            cases.append((text, max_lines))
    # deterministic fuzz: repeated blocks + a diagnostic line inside the fold
    for n in (4, 5, 9, 12, 40, 5000):
        for max_lines in (3, 4, 5, 10, 33):
            cases.append(("\n".join(f"line_{i}" for i in range(n)), max_lines))
            cases.append(("\n".join(f"ERROR {i % 3}" for i in range(n)), max_lines))
    assert len(cases) > 80
    for text, max_lines in cases:
        expected = _py_truncate(reference, text, max_lines)
        got = SHELL.truncate_lines(text, max_lines, True, 2)
        assert got == expected, f"truncate_lines mismatch (max_lines={max_lines})\n{text[:80]!r}"


def test_cpp_find_error_line_index_matches_reference(goldens):
    reference = _reference()
    cases = [""]
    for row in _rows(goldens, "const shape_golden kShapeGoldens[] = {", "\n};"):
        cases.append(_text(_split_fields(row)[0]))
    cases += [
        "ok\nall good\n",
        "a\nerror: boom\nb\n",
        "Traceback (most recent call last):\n  File 'x', line 1\nValueError\n",
        "line1\nAssertionError: x\nline3\n",
        "error\n" * 50,
        "caf\u00e9 error\n",
        "no error here\n" + "x" * 5000 + "\nnot found\n",
    ]
    for text in cases:
        expected = reference.common._find_error_line_index(text)
        got = SHELL.find_error_line_index(text)
        if expected is None:
            assert got is None, f"{text[:60]!r}: {got} != None"
        else:
            assert got == expected, f"{text[:60]!r}: {got} != {expected}"


def test_cpp_exit_code_kernels_match_reference(goldens):
    reference = _reference()
    oe = reference.output_enhance
    cases: list[tuple[str, int | None]] = []
    for row in _rows(goldens, "const exit_golden kExitGoldens[] = {", "\n};"):
        fields = _split_fields(row)
        cases.append((_text(fields[0]), _int(fields[2]) if _flag(fields[1]) else None))
    cases += [
        ("grep -n x f", 1),
        ("grep x f | head -3", 141),
        ("diff a b", 1),
        ("sh -c 'exit 1'", 1),
        ("cmd /c exit 9009", 9009),
        ("badcmd", 127),
        ("", 0),
        ("git status", None),
        ("curl -s url", 7),
        ("curl -s url", 28),
        ("pytest -q", 5),
    ]
    for command, code in cases:
        expected_meaning = oe.interpret_exit_code(command, code)
        got_meaning = SHELL.interpret_exit_code(command, code)
        assert got_meaning == expected_meaning, f"meaning mismatch for {command!r} exit={code}"
        assert SHELL.is_expected_exit(command, code) == oe.is_expected_exit(command, code), (
            f"is_expected_exit mismatch for {command!r} exit={code}")


def test_grep_no_match_and_sigpipe_are_expected_exits():
    """The FOCUS-3 cases, spelled out: grep no-match, diff differs, SIGPIPE."""
    assert SHELL.is_expected_exit("grep -n pattern file", 1) is True
    assert SHELL.interpret_exit_code("grep -n pattern file", 1) == "No matches found (not an error)"
    assert SHELL.is_expected_exit("diff a.txt b.txt", 1) is True
    assert SHELL.interpret_exit_code("diff a.txt b.txt", 1) == "Files differ (expected, not an error)"
    assert SHELL.is_expected_exit("yes | head -1", 141) is True
    assert SHELL.interpret_exit_code("yes | head -1", 141) == (
        "SIGPIPE: an upstream pipeline stage was truncated (expected when piping to head/tail)"
    )
    # A SIGPIPE without a top-level pipe is not expected.
    assert SHELL.is_expected_exit("sh -c 'exit 141'", 141) is False
    assert SHELL.is_expected_exit("cat missing.txt", 1) is False


def test_cpp_annotate_failure_matches_reference_for_ascii(goldens):
    reference = _reference()
    rows = _rows(goldens, "const annotate_golden kAnnotateGoldens[] = {", "\n};")
    # The bash-owned binding wraps the kernel with an ASCII gate; the reference
    # has no such gate, so the corpus compared here is ASCII-only.
    for row in rows:
        fields = _split_fields(row)
        output = _text(fields[0])
        if not output.isascii():
            continue
        command = _text(fields[1])
        code = _int(fields[3]) if _flag(fields[2]) else None
        expected = reference.output_enhance.annotate_failure(output, command, code)
        assert SHELL.annotate_failure(output, command, code) == expected, output


def test_annotate_failure_ascii_gate_is_documented():
    """Pinned deviation (owned by the bash tool, inherited by the binding).

    ``bash::annotate_failure`` returns no hint for non-ASCII output while the
    pure Python reference still matches its (ASCII) signatures.  ``Run`` itself
    calls ``kimix::runtime::tools::annotate_failure``, which is *not* gated -
    covered by ``test_annotate_failure_goldens`` in
    ``tests/unit/builtin_tools/test_run_tool.cpp`` (the non-ASCII vector there
    asserts the reference answer).
    """
    output = "caf\u00e9 command not found\n"
    reference = _reference()
    assert reference.output_enhance.annotate_failure(output, "caf\u00e9", 127) == (
        "The command was not found. Check it is installed and on PATH "
        "(use `which <cmd>` / `Get-Command <cmd>`)."
    )
    assert SHELL.annotate_failure(output, "caf\u00e9", 127) is None


def test_cpp_cd_prefix_matches_reference(goldens):
    run = _reference().run
    rows = _rows(goldens, "const cd_golden kCdGoldens[] = {", "\n};")
    assert len(rows) >= 15
    for row in rows:
        fields = _split_fields(row)
        cwd = _text(fields[0])
        shell = _text(fields[1])
        assert run._cd_prefix(cwd or None, shell) == _text(fields[2]), row


# ---------------------------------------------------------------------------
# 3. parameter coercion / clamping (FOCUS 1)
# ---------------------------------------------------------------------------


def test_param_coercion_table_is_the_reference_behaviour():
    """Anchor the C++ coercion/clamping tests to the live reference.

    ``tests/unit/builtin_tools/test_run_tool.cpp`` (the
    ``run_params_timeout_clamps_and_coerces`` / ``run_params_bool_coercion``
    tables) mirrors these expectations.  In kimi-agent the parameters go
    through ``CallableTool2.call`` -> ``_repair_dict_for_model`` (lax coercion +
    ``_clamp_numeric_value``) before ``RunParams`` validation, which is what is
    replayed here.
    """
    from kosong.tooling import _repair_dict_for_model

    run_params = _reference().run.RunParams

    def repaired(**kwargs):
        args = {"command": "x", **kwargs}
        try:
            return run_params.model_validate(_repair_dict_for_model(args, run_params))
        except Exception:
            return None

    # timeout: numeric values are CLAMPED into [1, 900]; coerced values are
    # validated strictly (the reference's clamp pass runs first).
    for value, expected in [(0, 1), (-5, 1), (1, 1), (900, 900), (901, 900),
                            (1000, 900), (8.0, 8), (1e9, 900), (True, 1)]:
        params = repaired(timeout=value)
        assert params is not None, f"timeout={value!r} should be accepted"
        assert params.timeout == expected, f"timeout={value!r} -> {params.timeout}"
    for value in (7.9, False, "0", "abc"):
        assert repaired(timeout=value) is None, f"timeout={value!r} should be rejected"
    for value, expected in [("45", 45), (" 45 ", 45), ("+45", 45), ("1_0", 10)]:
        assert repaired(timeout=value).timeout == expected

    # max_lines: ge=3 clamping, no upper bound.
    for value, expected in [(2, 3), (0, 3), (-1, 3), (3, 3), ("7", 7), (" 5 ", 5)]:
        assert repaired(max_lines=value).max_lines == expected
    assert repaired(max_lines=4.5) is None

    # bools: lax coercion by truthiness + the pydantic string table.
    for value, expected in [(True, True), (False, False), (1, True), (0, False),
                            (2, True), (1.0, True), (0.0, False),
                            ("true", True), ("TRUE", True), ("yes", True),
                            ("on", True), ("1", True), ("no", False),
                            ("off", False), ("0", False)]:
        assert repaired(run_in_background=value).run_in_background is expected
    assert repaired(run_in_background="maybe") is None

    # The default timeout is 30 and the range is [1, 900].
    assert run_params(command="x").timeout == 30
    assert repaired().timeout == 30
