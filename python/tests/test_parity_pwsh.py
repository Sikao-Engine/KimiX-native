"""Differential parity tests for the pwsh builtin tool (kimix-base <-> kimi-agent).

``src/builtin_tools/pwsh_tool.cpp`` (plus ``src/runtime/parse/shell_scanner.*``,
dialects ``PWSH_FIX`` / ``PWSH_TRANSFORM``) is a port of kimi-agent's PowerShell
tool kernels.  The ground truth is always the kimi-agent checkout
(``C:/dev/kimi-agent``, override with ``KIMI_AGENT_ROOT``) -- never the
``kimix_native`` mirror in this repo, and never kimi-agent's own native gate.

Kernels compared here (all reachable through ``runtime_py.builtin_tools.shell``):

* ``fix_pwsh_command`` -- ``_shell_compat.py::fix_pwsh_command`` (re-exported by
  ``pwsh_fix.py``, whose native fast path is bypassed on purpose).  The repaired
  ``command``, the ``warning`` text and the interleaving of the two null-device
  warning slots are all part of the contract; the binding returns
  ``(valid, changed, command, warning)`` where ``valid is False`` is the
  encoding of the reference's ``None``.
* ``pwsh_transform`` -- ``_shell_compat.py::pwsh_transform`` (re-exported by
  ``process_pwsh.py``): the PS7 -> PS5.1 rewrite *and* its ``Line N: ...``
  warnings.  Dropping a command word here changes the command that executes.
* ``pwsh_command_detection_variants`` / ``pwsh_check_hardline_blocked`` /
  ``pwsh_maybe_rewrite_with_rtk`` -- the pwsh mirrors of the shared bash kernels
  (``safety.py`` / ``common.py``); covered here at binding level because the
  pwsh tool calls these names.

Kernels that are *not* reachable from Python (no binding exists) are covered by
generated golden vectors consumed by the C++ test ``test_builtin_pwsh``:

* ``detect_self_kill`` / ``self_kill_hint`` (``safety.py`` 457-665) -- the
  self-kill guard.  ``scripts/gen_pwsh_goldens.py`` runs the reference over an
  adversarial kill-target corpus and writes
  ``tests/unit/builtin_tools/pwsh_goldens.inc``;
  ``test_goldens_are_in_sync_with_reference`` below re-derives those vectors and
  fails when the checked-in file drifts from the reference.

ASCII gate: every kernel under test gates non-ASCII input to a sentinel
(``valid=False`` / empty command) so the shim can route the call to the Python
mirror.  The corpora are ASCII-only by construction and
``test_ascii_gate_is_documented`` pins the sentinel values.
"""

from __future__ import annotations

import importlib.util
import itertools
import os
import random
import subprocess
import sys
from pathlib import Path
from unittest import mock

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

_KIMIX_BASE_ROOT = Path(__file__).resolve().parents[2]


def _kimix_base_bin_dir():
    """The kimix-base build directory holding ``runtime_py``."""
    for mode in ("release", "releasedbg", "debug", "check"):
        cand = _KIMIX_BASE_ROOT / "bin" / mode
        if cand.is_dir() and any(cand.glob("runtime_py.*")):
            return cand
    for cand in sorted((_KIMIX_BASE_ROOT / "bin").glob("*")):
        if cand.is_dir() and any(cand.glob("runtime_py.*")):
            return cand
    return None


# IMPORT ORDER IS LOAD-BEARING: the freshly built kimix-base extension must be
# imported *before* anything from the kimi-agent checkout, because importing
# kimi_cli runs ``native_loader._setup()`` which puts ``<kimi-agent>/bin`` -- a
# staged, older ``runtime_py.pyd`` -- first on ``sys.path``.  A later
# ``import runtime_py`` would then quietly compare the port against itself.
_BIN_DIR = _kimix_base_bin_dir()
if _BIN_DIR is not None:
    sys.path.insert(0, str(_BIN_DIR))
import runtime_py  # noqa: E402

if _BIN_DIR is None:  # pragma: no cover - defensive
    pytest.skip("no kimix-base runtime_py build found under bin/",
                allow_module_level=True)

_NATIVE_PATH = Path(runtime_py.__file__).resolve()
assert _BIN_DIR.resolve() in _NATIVE_PATH.parents, (
    f"runtime_py came from {_NATIVE_PATH}, not the kimix-base build "
    f"{_BIN_DIR} -- a staged copy shadowed it; parity results would be bogus")

from _parity_ref import KIMI_AGENT_ROOT, KIMI_CLI_SRC, KIMIX_SRC, ref_available  # noqa: E402

pytestmark = pytest.mark.skipif(
    not ref_available(), reason="kimi-agent checkout not available"
)

REPO_ROOT = _KIMIX_BASE_ROOT
SHELL = runtime_py.builtin_tools.shell
GOLDENS_PATH = REPO_ROOT / "tests" / "unit" / "builtin_tools" / "pwsh_goldens.inc"


# ---------------------------------------------------------------------------
# reference modules (loaded with explicit provenance)
# ---------------------------------------------------------------------------


def _load_by_path(path: Path, name: str):
    """Load a standalone reference module by file path (no package context)."""
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:  # pragma: no cover - defensive
        pytest.skip(f"cannot load reference module {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def _load_shim_package(pkg_dir: Path, alias: str):
    """Load ``<kimi-agent>/bin/kimix_native`` under *alias* as a real package.

    ``tools.py`` uses relative imports, so it cannot be loaded standalone;
    importing it under a private alias keeps the kimi-agent copy authoritative
    without touching the ``kimix_native`` entry (a kimix-base shim may
    legitimately own that name in this session).
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


_SHIM_DIR = KIMI_AGENT_ROOT / "bin" / "kimix_native"
KIMI_NATIVE_TOOLS = _load_shim_package(_SHIM_DIR, "_parity_pwsh_native")

#: The canonical pure-Python pwsh scanner/transformer (``pwsh_fix.py`` and
#: ``process_pwsh.py`` only re-export it, so this *is* the reference body).
KIMI_SHELL_COMPAT = _load_by_path(_SHIM_DIR / "_shell_compat.py",
                                  "_parity_pwsh_shell_compat")

for _path in (str(KIMIX_SRC), str(KIMI_CLI_SRC)):
    if os.path.isdir(_path) and _path not in sys.path:
        sys.path.insert(0, _path)

#: ``safety.py`` holds the real ``detect_self_kill`` body; its ``_compat_*``
#: helpers are pinned to the kimi-agent shim, and the native gate is disabled.
SAFETY = _load_by_path(
    KIMIX_SRC / "kimix" / "tools" / "file" / "bash" / "safety.py",
    "_parity_pwsh_safety")
SAFETY._COMPAT_TOOLS = KIMI_NATIVE_TOOLS
SAFETY._NATIVE_TOOLS = None
SAFETY._native_use_native = lambda *_a, **_k: False

#: Generator for the C++ golden vectors; reused here for its corpora and to
#: prove the checked-in ``.inc`` still matches the reference.
GENERATOR = _load_by_path(REPO_ROOT / "scripts" / "gen_pwsh_goldens.py",
                          "_parity_pwsh_generator")

#: Windows path shape kimi-agent's ``_rtk_binary_path()`` returns.
RTK_BINARY = str(Path("C:/Temp/rtk.exe"))

_REF_TESTS = KIMI_AGENT_ROOT / "tests"


# ---------------------------------------------------------------------------
# reference wrappers
# ---------------------------------------------------------------------------


def py_fix(command):
    """Reference ``fix_pwsh_command`` as the binding's 4-tuple (None -> all off)."""
    result = KIMI_SHELL_COMPAT.fix_pwsh_command(command)
    if result is None:
        return (False, False, "", "")
    return (True, bool(result.warning), result.command, result.warning)


def cpp_fix(command):
    valid, changed, command_out, warning = SHELL.fix_pwsh_command(command)
    return (bool(valid), bool(changed), command_out, warning)


def py_transform(code):
    command, warnings = KIMI_SHELL_COMPAT.pwsh_transform(code)
    return (command, list(warnings))


def cpp_transform(code):
    command, warnings = SHELL.pwsh_transform(code)
    return (command, list(warnings))


# ``kimix.tools.common`` (RTK rewrite) is loaded by path: ``import
# kimix.tools.common`` resolves the ``kimix`` package from whatever is first on
# ``sys.path``/``sys.modules`` and the kimi-agent workspace has two candidates
# (``src/kimix`` and the ``kimi-cli/src/kimix`` shim).
COMMON = _load_by_path(KIMIX_SRC / "kimix" / "tools" / "common.py",
                       "_parity_pwsh_common")
if hasattr(COMMON, "_COMPAT_SHELL"):
    COMMON._COMPAT_SHELL = KIMI_SHELL_COMPAT


def py_rtk(command, token_kill, exclude_read=False, *, available=True,
           rtk_path=RTK_BINARY):
    with mock.patch.object(COMMON, "_rtk_available", return_value=available), \
            mock.patch.object(
                COMMON, "_rtk_binary_path",
                return_value=None if rtk_path is None else Path(rtk_path)):
        return COMMON._maybe_rewrite_shell_command_with_rtk(
            command, token_kill, exclude_read, True)


def cpp_rtk(command, token_kill, exclude_read=False, *, available=True,
            rtk_path=RTK_BINARY):
    return tuple(SHELL.pwsh_maybe_rewrite_with_rtk(
        command, token_kill, available, "" if rtk_path is None else rtk_path,
        exclude_read))


# ---------------------------------------------------------------------------
# corpora
# ---------------------------------------------------------------------------

#: PowerShell 7 -> 5.1 transform inputs taken from kimi-agent's own suite
#: (``tests/test_process_pwsh.py``) plus the generator's curated/fuzz corpus.
_TRANSFORM_FROM_SUITE = GENERATOR._iter_call_args(
    _REF_TESTS / "test_process_pwsh.py", {"pwsh_transform"})

TRANSFORM_CORPUS = GENERATOR._clean(
    _TRANSFORM_FROM_SUITE + GENERATOR.TRANSFORM_CURATED)

#: Every string literal in kimi-agent's ``tests/test_pwsh_fix.py`` (inputs and
#: expected values alike -- both are valid fixer inputs) plus the generator's
#: curated/fuzz corpus.
_FIX_FROM_SUITE = GENERATOR._iter_string_constants(_REF_TESTS / "test_pwsh_fix.py")

FIX_CORPUS = GENERATOR._clean(_FIX_FROM_SUITE + GENERATOR.FIX_CURATED)

#: The self-kill corpus the C++ goldens are generated from (used here for the
#: variant/hardline comparisons and to keep the corpora in one place).
SELF_KILL_CORPUS = GENERATOR._clean(GENERATOR.SELF_KILL_CURATED)

_PS_ALPHABET = list('ab $?^:=(){}[]|&;,\'"`#<>@*\n\t\\/.0123-+_nulNUL>%') + [
    '??', '??=', '?.', '?[', '--%', '@"', "@'", '"@', "'@", '<#', '#>',
    'Write-Output', 'echo', 'nul', 'NUL', '>',
]
_RTK_ALPHABET = [
    "git", "status", "ls", "read", "find", "grep", "echo", "cat", "rtk",
    "rtk.exe", "RTK", "/usr/bin/git", "C:\\bin\\git", "-la", "x", "&&", ";",
    "|", "&", "(", ")", '"', "'", "`", "$(", "#", "\t", " ", "FOO=1",
    "RTK_DISABLED=1",
]
_HARDLINE_ALPHABET = [
    "rm", "-rf", "-r", "-f", "/", "~", "C:\\", "del", "/s", "/q", "format",
    "dd", "of=/dev/sda", "mkfs", "shutdown", "Stop-Computer", "kill", "1",
    "4100", "sudo", "x", ";", "&&", "|", '"', "'", "\\", "$(", " ", "\t",
]


def _fuzz(alphabet, count, seed, max_tokens=12):
    rng = random.Random(seed)
    return ["".join(rng.choice(alphabet)
                    for _ in range(rng.randint(1, max_tokens)))
            for _ in range(count)]


#: Grammar corpus: a prefix (which is where the command-word bug lived), one
#: PS7-only construct and one region/continuation decoration, in every
#: combination -- the shapes a purely random fuzz reaches too rarely.
_TRANSFORM_PREFIXES = [
    "", "Write-Output ", "Get-Item ", "echo ", "return ", "exit ",
    "continue ", "x ", "Write-Host ", "if ", "foreach ", "cmd /c ",
    "'", '"', "(", "@", "-", "1 ", "$y = ", "$y.Name = ",
]
_TRANSFORM_EXPRS = [
    "$a ?? $b", "$a ? $b : $c", "$o?.p", "$a ??= 1", "a && b", "a || b",
    "$x", "1", '"s"', "@(1,2)", "$o?.A?.B", "$arr?[0]", "$o ? 1 : 2",
    "$a ?? $b ?? $c",
]
_TRANSFORM_SUFFIXES = [
    "", " # c", " #", " ; echo done", " | Out-String", '"', "'", " `",
    " `\n", "\n", "\n$b ?? 1", " && echo x", " --% rest", " <# c",
    "\n# c", ' "$(x)"', " ?? ", "@'", '"@', "'@",
]


def _grammar_transform_corpus():
    out = [pre + expr + suf
           for pre, expr, suf in itertools.product(_TRANSFORM_PREFIXES,
                                                   _TRANSFORM_EXPRS,
                                                   _TRANSFORM_SUFFIXES)]
    for pre, expr in itertools.product(_TRANSFORM_PREFIXES, _TRANSFORM_EXPRS):
        out.append(pre + expr + "\n" + pre + expr)
        out.append("@'\n" + pre + expr + "\n'@\n" + pre + expr)
        out.append('"x ' + pre + expr + '"')
        out.append('$z = "' + pre + expr + '"')
    return GENERATOR._clean(out)


_FIX_PREFIXES = ["", "echo ", "Write-Output ", "cmd /c echo "]
_FIX_NULS = ["", " > nul", " >>nul", " 2> nul", " >nul", " *> nul",
             " > 'nul'", " > nul.txt"]
_FIX_TAILS = [' "a', " 'a", " `", " `\n", " # c", " <# c", "\n", "",
             ' "', " '", " > nul", ' @"', " @'", " #>", " --%",
             " --% > nul", " (", ' "$(' , ' "a""b']


def _grammar_fix_corpus():
    out = []
    for pre, mid, tail in itertools.product(_FIX_PREFIXES, _FIX_NULS,
                                            _FIX_TAILS):
        out.append(pre + mid + tail)
        out.append(pre + tail + mid)
        out.append(pre + mid + pre + tail)
    return GENERATOR._clean(out)


# ---------------------------------------------------------------------------
# fix_pwsh_command
# ---------------------------------------------------------------------------

#: The bug this suite was written for: a scanner repair must not be reported as
#: a null-device rewrite, and the two warning slots must keep the reference's
#: order (nul-first, scanner, nul-after).  Values below are the reference's.
_NUL_WARNING = (
    "Rewrote Windows-style null-device redirection target(s) `nul` to `$null` "
    "so PowerShell discards the output instead of creating a file named `nul`."
)
_UNCLOSED_DQ = (
    "The command has an unclosed double-quoted string; "
    'appended a closing `"` at the end to make it a legal PowerShell command.'
)
_TRAILING_COMMENT = (
    "The command ends with a line comment; appended a newline so the trailing "
    "comment does not swallow the try/catch wrapper used to execute the command."
)
_UNCLOSED_BLOCK = (
    "The command has an unclosed block comment `<#`; "
    "appended `#>` at the end to close it."
)
_TRAILING_CONT = (
    "The command ends with a backtick line-continuation; appended a newline so "
    "the continuation does not join with the try/catch wrapper used to execute "
    "the command."
)


def test_no_spurious_nul_warning_on_scanner_repair():
    """A repair is not a ``nul`` rewrite.

    ``echo "x`` needs a closing quote, nothing else: the reference warning is
    exactly the unclosed-string note.  The port used to append the null-device
    warning whenever the *scanner* changed the text.
    """
    command = 'echo "x'
    assert cpp_fix(command) == py_fix(command)
    assert cpp_fix(command) == (True, True, 'echo "x"', _UNCLOSED_DQ)


def test_nul_warning_precedes_the_scanner_warning():
    """Both warning slots fire -> reference order is nul, scanner, nul.

    ``fix_pwsh_command`` in ``_shell_compat.py`` builds
    ``[nul_warning, scanner_warning, nul_warning_after]``; the port emitted the
    scanner note first.
    """
    for command, expected in [
        ('echo > nul "x', (True, True, 'echo > $null "x"',
                           _NUL_WARNING + "\n" + _UNCLOSED_DQ)),
        ('Write-Host "x" > nul # c',
         (True, True, 'Write-Host "x" > $null # c\n',
          _NUL_WARNING + "\n" + _TRAILING_COMMENT)),
        ('echo > nul `\n', (True, True, 'echo > $null `\n\n',
                            _NUL_WARNING + "\n" + _TRAILING_CONT)),
        ('echo > nul <# c', (True, True, 'echo > $null <# c#>',
                             _NUL_WARNING + "\n" + _UNCLOSED_BLOCK)),
    ]:
        assert py_fix(command) == expected, command
        assert cpp_fix(command) == expected, command


def test_fix_matches_reference_over_corpus():
    mismatches = []
    for command in FIX_CORPUS:
        expected = py_fix(command)
        got = cpp_fix(command)
        if got != expected:
            mismatches.append((command, expected, got))
    assert not mismatches, "\n".join(
        f"fix_pwsh_command({c!r})\n  reference={e!r}\n  port     ={g!r}"
        for c, e, g in mismatches[:20]) + (
        f"\n({len(mismatches)} of {len(FIX_CORPUS)} mismatched)")


def test_fix_matches_reference_fuzz():
    corpus = (GENERATOR._clean(_fuzz(_PS_ALPHABET, 4000, seed=424242))
              + _grammar_fix_corpus())
    mismatches = [(c, py_fix(c), cpp_fix(c)) for c in corpus
                  if py_fix(c) != cpp_fix(c)]
    assert not mismatches, "\n".join(
        f"fix_pwsh_command({c!r})\n  reference={e!r}\n  port     ={g!r}"
        for c, e, g in mismatches[:20]) + (
        f"\n({len(mismatches)} of {len(corpus)} mismatched)")


def test_fix_none_encoding_is_valid_false():
    """The reference returns ``None`` exactly where the binding says invalid."""
    for command in ("", " ", "\t\n ", "`", "Write-Output `", "Get-ChildItem `",
                    "--%", "--% foo"):
        assert KIMI_SHELL_COMPAT.fix_pwsh_command(command) is None, repr(command)
        assert cpp_fix(command) == (False, False, "", ""), repr(command)


def test_fix_changed_flag_tracks_the_warning():
    """``PwshFix.changed`` is ``bool(warning)`` in the reference."""
    for command in FIX_CORPUS + _fuzz(_PS_ALPHABET, 500, seed=7):
        expected = py_fix(command)
        got = cpp_fix(command)
        assert got[1] == bool(expected[3]), repr(command)
        if not got[0]:  # unrepairable: the reference returns None
            assert got == (False, False, "", ""), repr(command)
            continue
        assert got[1] == (got[2] != command or bool(got[3])), repr(command)


# ---------------------------------------------------------------------------
# pwsh_transform
# ---------------------------------------------------------------------------


def test_transform_matches_reference_over_suite_corpus():
    """Every input kimi-agent's own transform suite uses, byte-exact."""
    assert len(TRANSFORM_CORPUS) > 1000, (
        f"expected kimi-agent's transform corpus; got {len(TRANSFORM_CORPUS)}",
    )
    mismatches = []
    for code in TRANSFORM_CORPUS:
        expected = py_transform(code)
        got = cpp_transform(code)
        if got != expected:
            mismatches.append((code, expected, got))
    assert not mismatches, "\n".join(
        f"pwsh_transform({c!r})\n  reference={e!r}\n  port     ={g!r}"
        for c, e, g in mismatches[:20]) + (
        f"\n({len(mismatches)} of {len(TRANSFORM_CORPUS)} mismatched)")


def test_transform_matches_reference_fuzz():
    corpus = (GENERATOR._clean(_fuzz(_PS_ALPHABET, 5000, seed=20240921))
              + _grammar_transform_corpus())
    mismatches = []
    for code in corpus:
        expected = py_transform(code)
        got = cpp_transform(code)
        if got != expected:
            mismatches.append((code, expected, got))
    assert not mismatches, "\n".join(
        f"pwsh_transform({c!r})\n  reference={e!r}\n  port     ={g!r}"
        for c, e, g in mismatches[:20]) + (
        f"\n({len(mismatches)} of {len(corpus)} mismatched)")


def test_transform_keeps_the_command_word_prefix():
    """Regression: a rewritten expression must keep its command word.

    ``_strip_command_prefix`` only *locates* the expression inside the line; it
    returns ``start + len(command_word)`` and the reference rebuilds the line as
    ``line[:start] + inner + rest``.  The port used the *un*adjusted start, so
    ``Write-Output $a ? $b : $c`` silently became ``if (...) {...}`` -- a
    different command.  kimi-agent's own suite asserts the prefix survives
    (``test_null_conditional_after_command_prefix``,
    ``test_cmd_prefix_with_variable_prop``,
    ``test_command_followed_by_ternary_without_parens``).
    """
    cases = [
        'Write-Output $a ? $b : $c',
        'Write-Output $a?.Name',
        'Write-Output $a ?? "default"',
        'Write-Output 123 ?? 0',
        'Write-Output $a ?? 0',
        'Write-Output @(1,2) ?? 0',
        'Write-Output @{a=1} ?? "fallback"',
        'return $cond ? $a : $b',
        'exit $cond ? 0 : 1',
        'continue $cond ? 1 : 0',
        'Write-Output $a $b $c ? "yes" : "no"',
        'Write-Output $a $b $c ?? "default"',
        'Write-Output $a $b $c?.Name',
    ]
    for code in cases:
        expected = py_transform(code)
        got = cpp_transform(code)
        assert got == expected, (code, expected, got)
    # Spot-check the literal reference outputs (no reliance on the port).
    assert py_transform('Write-Output $a?.Name')[0] == \
        'Write-Output $(if ($null -ne $a) { $a.Name })'
    assert cpp_transform('Write-Output $a?.Name')[0] == \
        'Write-Output $(if ($null -ne $a) { $a.Name })'
    assert py_transform('Write-Output $a ? $b : $c')[0] == \
        'Write-Output if ($a) { $b } else { $c }'
    assert cpp_transform('Write-Output $a ? $b : $c')[0] == \
        'Write-Output if ($a) { $b } else { $c }'
    # A keyword prefix is never stripped (check_keywords).
    for code in ('if ($x) { 1 } else { 2 }', 'foreach ($x in $y) { $z ?? 1 }'):
        assert cpp_transform(code) == py_transform(code), code


def test_transform_assignments_keep_their_prefix():
    """The assignment form is preserved verbatim (``$x = <rewritten>``)."""
    for code in ('$x = $a ?? $b', '$status = $count -eq 0 ? "e" : "ne"',
                 '$a = $b ??= $c', '$obj.Name ??= 1', '$arr[0] ??= 1'):
        assert cpp_transform(code) == py_transform(code), code


def test_transform_warning_line_numbers():
    code = '# comment ??\n$a ?? $b'
    assert py_transform(code) == (
        '# comment ??\nif ($null -ne $a) { $a } else { $b }',
        ["Line 2: ?? operator `$a ?? $b` rewritten to "
         "`if ($null -ne $a) { $a } else { $b }`"])
    assert cpp_transform(code) == py_transform(code)
    code = "@'\nhere ??\n'@\n$a ?? $b"
    assert cpp_transform(code) == py_transform(code)
    assert cpp_transform(code)[1][0].startswith("Line 4: ")


# ---------------------------------------------------------------------------
# pwsh mirrors of the shared kernels
# ---------------------------------------------------------------------------


def test_pwsh_command_detection_variants_matches_reference():
    for command in SELF_KILL_CORPUS + _fuzz(_HARDLINE_ALPHABET, 400, seed=11):
        expected = SAFETY.command_detection_variants(command)
        got = list(SHELL.pwsh_command_detection_variants(command))
        assert got == expected, repr(command)


def test_pwsh_check_hardline_blocked_matches_reference():
    corpus = [
        "rm -rf /", "rm -rf ~", "sudo rm -rf /", "del /f /s /q C:\\*",
        "format C:", "dd if=x of=/dev/sda", "mkfs.ext4 /dev/sda1",
        "shutdown /s", "Stop-Computer", "kill 1", "kill -9 1",
        "Remove-Item -Recurse -Force C:\\", "echo hi", "Get-ChildItem",
        "git status", "taskkill /IM chrome.exe", "r\\\\m -rf /",
    ] + _fuzz(_HARDLINE_ALPHABET, 800, seed=12)
    for command in corpus:
        expected = tuple(KIMI_NATIVE_TOOLS._compat_check_hardline_blocked(command))
        got = tuple(SHELL.pwsh_check_hardline_blocked(command))
        assert got == expected, repr(command)


def test_pwsh_maybe_rewrite_with_rtk_matches_reference():
    corpus = [
        "git status", "git status | grep x", "VAR=value git status",
        "git status && cargo test", "git status; cargo test", "rtk git status",
        "RTK_DISABLED=1 git status", "unknown-cmd arg", 'echo "git status"',
        "echo $(git status)", "echo `git status`", "read var", "ls -la",
        "find . -name x", "git log", "& rtk git status", "&  rtk git status",
        "C:\\Temp\\rtk.exe git status", "./git status", "/usr/bin/git status",
        "", " ", "  git status  ", "|", "&&",
    ] + _fuzz(_RTK_ALPHABET, 600, seed=13)
    for command in corpus:
        for token_kill in (True, False):
            for exclude_read in (False, True):
                expected = py_rtk(command, token_kill, exclude_read)
                got = cpp_rtk(command, token_kill, exclude_read)
                assert got == expected, (command, token_kill, exclude_read)
    # The pwsh variant is selected through the flag, not a separate scanner.
    assert cpp_rtk("git status", True) == ("& rtk git status", True)
    assert cpp_rtk("git status", True, available=False) == ("git status", False)


# ---------------------------------------------------------------------------
# ASCII gate (documented sentinel values)
# ---------------------------------------------------------------------------


def test_reference_provenance():
    """The compared reference objects really come from the kimi-agent checkout."""
    agent_root = str(KIMI_AGENT_ROOT).replace("\\", "/").lower()
    for module in (KIMI_SHELL_COMPAT, KIMI_NATIVE_TOOLS, SAFETY):
        path = str(getattr(module, "__file__", "")).replace("\\", "/").lower()
        assert path.startswith(agent_root), (module.__name__, path)
    # ``pwsh_fix.py`` / ``process_pwsh.py`` only re-export the canonical scanner
    # body from ``_shell_compat.py`` (checked on the source so the claim holds
    # regardless of which ``kimix_native`` a session already imported).
    bash_dir = KIMIX_SRC / "kimix" / "tools" / "file" / "bash"
    fix_src = (bash_dir / "pwsh_fix.py").read_text(encoding="utf-8")
    assert "_shell.fix_pwsh_command(cmd)" in fix_src
    assert "PwshFix = _shell.PwshFix" in fix_src
    transform_src = (bash_dir / "process_pwsh.py").read_text(encoding="utf-8")
    assert "pwsh_transform = _shell.pwsh_transform" in transform_src
    # ``safety.py``'s detect_self_kill body is its own; only the shared helpers
    # come from the shim, which is exactly what SAFETY is pinned to.
    assert SAFETY._compat_tools() is KIMI_NATIVE_TOOLS
    for name in ("_compat_segment_tokens", "_compat_looks_like_flag",
                 "_compat_command_detection_variants",
                 "_compat_check_hardline_blocked"):
        assert callable(getattr(KIMI_NATIVE_TOOLS, name)), name


@pytest.mark.parametrize("command", [
    "$x ?? caf\u00e9", 'Write-Output "\u65e5\u672c\u8a9e"',
    "\u4e2d\u6587 command", "echo \U0001f600 ?? 1",
])
def test_ascii_gate_is_documented(command):
    """Non-ASCII input never reaches the kernels: the shim routes it to Python.

    The sentinel is ``valid=False`` for ``fix_pwsh_command`` and an empty
    command for ``pwsh_transform``; ``pwsh_transform`` reports the same sentinel
    for whitespace-only input, which is why the shim checks the input type
    first.  Pinning this keeps a "fix" from silently comparing transformed
    text against nothing.
    """
    assert cpp_fix(command) == (False, False, "", "")
    assert cpp_transform(command) == ("", [])
    # The reference, of course, still answers.
    assert py_fix(command)[0] is True
    assert py_transform(command)[0] != ""


def test_ascii_corpus_is_ascii():
    """The differential corpora must not silently contain non-ASCII input."""
    for name, corpus in (("fix", FIX_CORPUS), ("transform", TRANSFORM_CORPUS),
                         ("self_kill", SELF_KILL_CORPUS)):
        bad = [c for c in corpus if not c.isascii()]
        assert not bad, (name, bad[:5])


# ---------------------------------------------------------------------------
# generated C++ goldens (kernels with no Python binding: detect_self_kill)
# ---------------------------------------------------------------------------


@pytest.mark.skipif(not GOLDENS_PATH.is_file(),
                    reason="pwsh_goldens.inc not generated yet")
def test_goldens_are_in_sync_with_reference():
    """Re-derive ``pwsh_goldens.inc`` from the reference and compare.

    The self-kill guard (``detect_self_kill`` / ``self_kill_hint``) is not
    exposed to Python, so the C++ test ``test_builtin_pwsh`` verifies it against
    generated vectors instead.  This test is what keeps those vectors honest:
    the file must be byte-identical to a fresh run of the generator, i.e. to the
    kimi-agent reference.
    """
    expected = GENERATOR.build_goldens()
    current = GOLDENS_PATH.read_text(encoding="utf-8")
    if current != expected:
        exp_lines = expected.splitlines()
        cur_lines = current.splitlines()
        diff = [(i, a, b) for i, (a, b) in enumerate(zip(cur_lines, exp_lines))
                if a != b][:10]
        detail = "\n".join(f"  line {i + 1}:\n    file={a!r}\n    ref ={b!r}"
                           for i, a, b in diff)
        pytest.fail(f"{GOLDENS_PATH} is out of date "
                    f"({len(cur_lines)} vs {len(exp_lines)} lines); "
                    f"run `python scripts/gen_pwsh_goldens.py`\n{detail}")


def test_image_name_tie_break_is_nondeterministic_in_the_reference():
    """Why the goldens pin an order the reference does not have.

    ``safety._name_kill_hit`` returns the first entry of ``image_names`` (an
    unordered ``set``) whose basename *or* stem matches the token.  With both
    ``python`` and ``python.exe`` in the set, ``taskkill /IM python.exe``
    answers ``python.exe`` under some hash seeds and ``python`` under others, so
    the reference's description text is unstable across processes.  The port
    sorts the names ascending (documented deviation) and
    ``scripts/gen_pwsh_goldens.py`` pins the same order, otherwise the generated
    vectors could never be reproduced.  This test proves the instability from
    the outside (fresh interpreters, different hash seeds).
    """
    code = (
        "names={'python.exe','python','kimi'};"
        "print([n for n in names if n in ('python.exe','python')][0])"
    )
    seen = set()
    for seed in range(12):
        env = dict(os.environ, PYTHONHASHSEED=str(seed))
        proc = subprocess.run([sys.executable, "-c", code], env=env,
                              capture_output=True, text=True)
        assert proc.returncode == 0, proc.stderr
        seen.add(proc.stdout.strip())
    assert seen == {"python", "python.exe"}, (
        "expected the unordered-set tie-break to flip between hash seeds; "
        f"got {seen}")
    # ... and the reference really does route the tie through that set.
    src = (KIMIX_SRC / "kimix" / "tools" / "file" / "bash" / "safety.py")
    assert "for name in image_names:" in src.read_text(encoding="utf-8")
    # The goldens must carry the deterministic (ascending) spelling instead.
    assert '"kills by image name `python` via `taskkill /IM`' in \
        GOLDENS_PATH.read_text(encoding="utf-8")


def test_self_kill_goldens_cover_the_guard():
    """The generated corpus must actually exercise the self-kill guard.

    A corpus that only contained safe commands would make the C++ golden test
    vacuous, so the *reference* is checked to hit, per detector family, and the
    known false negative is pinned explicitly.
    """
    text = GOLDENS_PATH.read_text(encoding="utf-8", errors="replace")
    for needle in ("targets PID 4100 via `kill`", "via `taskkill`",
                   "via `Stop-Process`", "via `pkill`", "via `killall`",
                   "via `wmic`", "through loop variable", "kills by image name"):
        assert needle in text, f"self-kill goldens lost coverage: {needle}"

    def reference_hit(command):
        return SAFETY.detect_self_kill(
            command, protected_pids={4100, 5000},
            image_names={"python.exe", "python", "kimi"},
            cmdline=GENERATOR.AGENT_CMDLINE)

    # Documented gap inherited from the reference: bash's own PID spellings
    # (``$$`` / ``$PPID`` / ``$!``) are not resolved to the agent PID, so a
    # ``kill $$`` slips through.  The port must reproduce that exactly (it is a
    # parity suite, and the shim routes to Python for the same input).
    for command in ("kill $$", "kill $PPID", "kill ${PPID}", "kill $!"):
        assert reference_hit(command) is None, command
        assert f'{{"{command}", true, nullptr}}' in text, command
    # ... while the loop-variable form IS resolved on both sides.
    assert reference_hit("for pid in 4100 5000; do kill $pid; done")
    assert reference_hit("foreach ($pid in 4100,5000) { Stop-Process -Id $pid }")
