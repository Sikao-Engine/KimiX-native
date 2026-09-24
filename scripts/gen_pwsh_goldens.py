#!/usr/bin/env python3
"""Regenerate the pwsh golden vectors consumed by ``test_builtin_pwsh``.

Every expectation in ``tests/unit/builtin_tools/pwsh_goldens.inc`` is produced
by running the *original Python* implementation in the kimi-agent checkout, so
nothing is transcribed by hand:

* ``pwsh_transform`` -- ``bin/kimix_native/_shell_compat.py::pwsh_transform``
  (the canonical pure-Python PS7 -> PS5.1 transformer that ``process_pwsh.py``
  re-exports).
* ``fix_pwsh_command`` -- ``bin/kimix_native/_shell_compat.py::fix_pwsh_command``
  (``pwsh_fix.py`` re-exports it; the native fast path in ``pwsh_fix.py`` is
  skipped on purpose so the goldens describe the *reference* behaviour).
* ``command_detection_variants`` / ``detect_self_kill`` / ``self_kill_hint`` --
  ``src/kimix/tools/file/bash/safety.py`` with its ``_compat_*`` helpers pinned
  to the kimi-agent ``kimix_native`` shim (``bin/kimix_native/tools.py``).

The self-kill entries use a *fixed* agent identity (protected PIDs, image names
and command line) so the vectors do not depend on the machine that generated
them; ``self_kill_hint`` additionally stores the ``agent_pid`` it embedded in
the message, because the reference interpolates ``os.getpid()`` into the text.

Usage:
    python scripts/gen_pwsh_goldens.py            # rewrite the .inc
    python scripts/gen_pwsh_goldens.py --check    # verify it is up to date
    python scripts/gen_pwsh_goldens.py --stdout   # print instead of writing
"""

from __future__ import annotations

import argparse
import ast
import importlib.util
import os
import random
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
KIMI_AGENT_ROOT = Path(os.environ.get("KIMI_AGENT_ROOT", r"C:/dev/kimi-agent"))
KIMIX_SRC = KIMI_AGENT_ROOT / "src"
SHIM_DIR = KIMI_AGENT_ROOT / "bin" / "kimix_native"
OUT_PATH = REPO_ROOT / "tests" / "unit" / "builtin_tools" / "pwsh_goldens.inc"

#: Fixed agent identity shared by every self-kill vector (C++ side uses the same
#: literals in tests/unit/builtin_tools/test_pwsh_tool.cpp).
PROTECTED_PIDS = (4100, 5000)
AGENT_IMAGE_NAMES = ("python.exe", "python", "kimi")
AGENT_CMDLINE = "/usr/bin/python3 /home/u/kimi-cli/bin/kimi --agent worker"

#: The reference interpolates ``os.getpid()`` into the self-kill hint, so the
#: generator pins it: without a fixed pid the generated file would change on
#: every run (and the parity suite's in-sync check could never pass).
AGENT_PID = 4100

#: Longest corpus entry kept (keeps the generated C++ literals sane; the
#: pathological deep-nesting inputs from the reference suite are length-gated
#: out on purpose).
MAX_LEN = 400

#: safety.py detector 4's pkill word scanner (``\b(?:pkill|killall)(?:\.exe)?\b``).
_PKILL_RE = re.compile(r"\bpkill(?:\.exe)?\b")


# ---------------------------------------------------------------------------
# reference loaders
# ---------------------------------------------------------------------------


def _load_by_path(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:  # pragma: no cover - defensive
        raise SystemExit(f"cannot load reference module {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def _load_shim_package(pkg_dir: Path, alias: str):
    """Load ``<kimi-agent>/bin/kimix_native`` under *alias* (relative imports)."""
    spec = importlib.util.spec_from_file_location(
        alias, pkg_dir / "__init__.py",
        submodule_search_locations=[str(pkg_dir)])
    if spec is None or spec.loader is None:  # pragma: no cover - defensive
        raise SystemExit(f"cannot load reference package {pkg_dir}")
    package = importlib.util.module_from_spec(spec)
    sys.modules[alias] = package
    spec.loader.exec_module(package)
    return importlib.import_module(f"{alias}.tools")


def load_reference():
    """Return ``(shell_compat, safety)`` from the kimi-agent checkout."""
    for path in (str(KIMIX_SRC), str(KIMI_AGENT_ROOT / "kimi-cli" / "src")):
        if os.path.isdir(path) and path not in sys.path:
            sys.path.insert(0, path)
    shell_compat = _load_by_path(SHIM_DIR / "_shell_compat.py",
                                 "_goldens_shell_compat")
    tools_shim = _load_shim_package(SHIM_DIR, "_goldens_kimix_native")
    safety = _load_by_path(
        KIMIX_SRC / "kimix" / "tools" / "file" / "bash" / "safety.py",
        "_goldens_safety")
    # Pin the reference's shared helpers to the kimi-agent shim (never the
    # kimix-base mirror, whose tables differ).
    safety._COMPAT_TOOLS = tools_shim
    safety._NATIVE_TOOLS = None
    safety._native_use_native = lambda *_a, **_k: False
    safety._agent_pids = lambda: set(PROTECTED_PIDS)
    safety._agent_image_names = lambda: set(AGENT_IMAGE_NAMES)
    safety._agent_cmdline = lambda: AGENT_CMDLINE
    # Pinned pid: ``self_kill_hint`` embeds ``os.getpid()`` in the message.
    safety.os = _FixedOs()
    # Pinned image-name tie-break (see _make_deterministic_name_kill_hit).
    safety._name_kill_hit = _make_deterministic_name_kill_hit(safety)
    return shell_compat, safety


class _FixedOs:
    """Stand-in for the ``os`` module inside the reference (pinned pid)."""

    name = os.name
    sep = os.sep
    environ = os.environ

    @staticmethod
    def getpid() -> int:
        return AGENT_PID


def _make_deterministic_name_kill_hit(safety):
    """``safety._name_kill_hit`` with a pinned (ascending) set order.

    The reference iterates ``image_names`` -- an unordered ``set`` -- and returns
    the *first* entry matching either the token's basename or its suffix-less
    stem.  When both spellings are in the set (``python`` / ``python.exe``) the
    returned name -- and therefore the description text -- depends on the
    interpreter's hash seed: the same ``taskkill /IM python.exe`` answers
    ``python.exe`` under one seed and ``python`` under another.  The port sorts
    the names (ascending) so the result is deterministic; the goldens pin that
    same order, and
    ``python/tests/test_parity_pwsh.py::test_image_name_tie_break_is_nondeterministic_in_the_reference``
    demonstrates the instability.
    """

    def hit(token, image_names):
        base, stem = safety._split_image_name(token)
        if not base or not safety.re.search(r"[a-z0-9]", base, safety.re.IGNORECASE):
            return None
        ordered = sorted(image_names)
        if base.endswith("*"):
            prefix = base[:-1]
            if len(prefix) < 3:
                return None
            for name in ordered:
                if name.startswith(prefix):
                    return name
            return None
        for name in ordered:
            if base == name or stem == name:
                return name
        return None

    return hit


# ---------------------------------------------------------------------------
# corpora
# ---------------------------------------------------------------------------

#: PowerShell 7 -> 5.1 transform: hand-written adversarial cases (everything the
#: reference test suite exercises per feature, plus command-prefix / region
#: interactions).
TRANSFORM_CURATED = [
    # ternary
    '$x = $cond ? "a" : "b"', '$cond ? "yes" : "no"', '$a -gt 5 ? $a : 0',
    '$x = Test-Path $p ? (Get-Item $p) : $null', '$? ? "yes" : "no"',
    'Write-Output $a ? $b : $c', 'return $cond ? $a : $b',
    'Write-Output $a $b $c ? "yes" : "no"', 'exit $cond ? 0 : 1',
    'continue $cond ? 1 : 0', '$x = 5 ? 6 : 7 ? 8 : 9',
    'Write-Output ($cond ? "a" : "b")', '$x = $c ? 1 : 2# tail',
    # null coalescing
    '$x = $a ?? "default"', '$a ?? "fallback"', '$x = $a ?? $b ?? "c"',
    'Write-Output $a ?? "default"', 'Write-Output 123 ?? 0',
    'Write-Output @(1,2) ?? 0', 'Write-Output @{a=1} ?? "fallback"',
    '$path = $env:HOME ?? \'C:\\Users\\Default\'', '$x = 1 `\n?? $y',
    '$a = $b ?? $c', 'Write-Output $a $b $c ?? "default"',
    # null-coalescing assignment
    '$x ??= 5', '$a = $b ??= $c', 'Write-Output $a ??= 1',
    '${foo} ??= 1', '$arr[0] ??= 1', '$obj.Name ??= 1',
    # null-conditional
    '$obj?.Prop', '$arr?[0]', '$obj?.A?.B', 'Write-Output $a?.Name',
    '$a?."b-c"?."d-e"', '$arr[0][1]?.Name', 'Write-Output $a $b $c?.Name',
    '$obj?.Method(1,2)', '$o?[0]?[1]', '$x = $obj?.P',
    # pipeline chains
    'a && b', 'a || b', 'a && b || c', 'cmd /c "echo a && b"',
    'git status && cargo test', 'if (a) { b } && c',
    # regions
    '"keep ?? inside"', "'keep ? : inside'", '# comment ??\n$a ?? $b',
    '@\'\nhere ??\n\'@\n$a ?? $b', '<# block ?? #>\n$a ?? $b',
    'cmd /c echo --% "hello', 'Write-Output "a$( "b" )c"',
    'Write-Output $($x ? 1 : 2)', 'echo "$($x ? 1 : 2)"',
    '"a`"b ?? c"', "a#b ?? c", 'a #comment ??\n$b = 1 ?? 2',
    '@"\n$not ? transformed\n"@\n$a ?? $b',
    # misc / negative
    'echo hi', 'Get-Item -Path "C:\\x"', 'if ($x) { 1 } else { 2 }',
    '$x = @{a=1}',
    'foreach ($pid in 4100,5000) { Stop-Process -Id $pid }',
    'Start-Process foo -ArgumentList "--x --y"',
    'echo `#not a comment', '$a = 1; $b = $a ? 2 : 3',
    'Write-Output $a $b $c?.Name', 'exit $cond ? 0 : 1',
]

#: fix_pwsh_command: hand-written adversarial cases.  Every warning kind, every
#: nul-redirection spelling, the interleavings of both, and the wrapper-safety
#: newline repairs.
FIX_CURATED = [
    # empty / irreparable
    '', ' ', '\t\n ', '`', 'Write-Output `', 'Get-ChildItem `', '--%',
    '--% foo', '\n', '  \t ',
    # valid, left unchanged
    'Get-Location', '1 + 2', 'Write-Output "hi"', 'git status; cargo test',
    'Write-Output "a""b"', 'Write-Output "a$( "b" )c"', '$x = @"\nhi\n"@',
    'Write-Output foo#c "x"', '<# one <# two #> three #>', 'a@"\nhello\n"@',
    '$x = 5 # trailing comment\nWrite-Output $x', '   Get-Date   ',
    'Write-Output (5)#c "x"\nWrite-Output ok', 'cmd /c echo --% "a" b',
    # repairs
    'Write-Output "hello', 'Write-Output "a" "b', 'Write-Output "" "',
    "Write-Output 'hello", "'it''s", '"a\'b', 'Write-Output "a`', '"a``',
    '@"\nunclosed here-string', "@'\nunclosed here-string",
    'Write-Output ok <# unclosed comment', '# just a comment',
    '<# just a comment', 'Write-Output ok # done', 'cmd /c echo --% foo',
    'Write-Output `\n', 'Get-ChildItem `\n-Filter *.ps1',
    'Write-Output `\n"hello', '"', "'", '"""', '""""""', '"\'"', "''",
    "'''", '@\'@\'@', '>"@', '<#', '#>', '"$(', "'$( \"x\" )", '@"@"',
    '"a`"', "'a`b'", 'Write-Output "a$( "b', '"a\'b"c', "'it''", '@"', "@'",
    '<# c #>', 'cmd /c echo --% `', 'Write-Output `"', '"a` `"',
    '# comment `', 'Write-Output "a"; "b', "Write-Output 'a' 'b",
    '"a$( "b )c"', '`" `" `"', '@"\r\n"@\r\n"@', '$x = a@"\nhello\n"@',
    'Write-Output "a`\nb `"c"', 'Write-Output ("a" + \'b"\')',
    '[pscustomobject]@{name = \'has " quote\'} | Out-String',
    'Write-Output "`""', '$s = "a" -replace \'"\', \'""\'; Write-Output $s',
    'Write-Output "a"; # comment " b\nWrite-Output c',
    # nul redirection
    'Write-Output hi > nul', 'Write-Output hi >NUL', 'Write-Output hi >> nul',
    'Write-Output hi >nul', 'Write-Output hi 2> nul', 'Write-Output hi 2>> nul',
    'Write-Output hi *> nul', "Write-Output 'nul' > nul",
    "Write-Output hi > 'nul'", 'Write-Output hi > "nul"',
    'cmd /c echo --% > nul\nWrite-Output after', 'cmd /c echo --% > nul',
    'Write-Output "hi > nul"', "Write-Output 'text > nul'",
    '# comment > nul\nWrite-Output hi', 'Write-Output nul',
    'Write-Output hi > nul.txt', 'Write-Output hi > $null',
    'Write-Output a > nul; Write-Output b >> nul; Write-Output c 2> nul',
    'echo hi > nul', 'echo hi 2>nul', 'echo >nul', 'echo > nul',
    # nul + repair interleavings (warning ORDER is part of the contract)
    'echo > nul "x', 'Write-Host "x" > nul # c', 'echo > nul `\n',
    'echo `\n> nul', 'echo >nul"a', 'echo "a> nul', 'echo > nul `x',
    'echo >>nul`', 'echo > nul #c', 'echo >nul"',
    'echo > nul; echo "x', 'echo "x; echo > nul',
    'echo > nul <# c', 'echo <# c#> > nul',
    # misc
    'echo hi', 'echo "x', "echo 'x", 'echo `', '#comment',
    '$a = @"\nx\n"@\n$b = @\'\ny\n\'@\nWrite-Output $a$b',
    'foreach ($pid in 4100,5000) { Stop-Process -Id $pid }',
    'echo hi > nul\n', 'echo hi\n> nul',
]

#: Self-kill guard: adversarial kill-target corpus (FOCUS 2).  A false negative
#: here means the agent kills its own process, so every detector, PID spelling,
#: quoting trick and loop form the reference implements is represented.
SELF_KILL_CURATED = [
    # 1. kill / tskill
    'kill 4100', 'kill 5000', 'kill -9 4100', 'kill -TERM 4100', 'kill 9999',
    'kill 41000', 'kill 410', 'kill 4100abc', 'kill -4100', 'kill -- 4100',
    'kill "4100"', "kill '4100'", 'kill (4100)', 'kill 4100)', 'kill 4100.',
    'kill 4100,5000', 'kill 5000,4100', 'kill ', 'kill', 'killed 4100',
    'kill.exe 4100', 'KILL 4100', 'Kill 4100', 'kill\t4100', 'kill\n4100',
    'kill 4100 ; kill 9999', 'kill 9999 ; kill 4100', 'echo x; kill 4100',
    'kill 4100 && echo ok', 'x | kill 4100', 'kill 9999 | kill 4100',
    'tskill 4100', 'tskill.exe 4100', 'tskill 4100 /a', 'tskill 9999',
    # container/VCS kills must not be treated as host PIDs
    'docker kill 4100', 'docker kill 9999', 'podman kill 4100',
    'kubectl kill 4100', 'compose kill 4100', 'sudo docker kill 4100',
    'docker compose kill 4100', 'xdocker kill 4100', 'dockerx kill 4100',
    # bash self-PID spellings the reference does NOT resolve (documented gap)
    'kill $$', 'kill $PPID', 'kill ${PPID}', 'kill $!',
    # 2. taskkill
    'taskkill /PID 4100', 'taskkill /pid 4100 /F', 'taskkill /F /PID 4100',
    'taskkill /PID 9999', 'taskkill /PID "4100"', "taskkill /PID '4100'",
    'taskkill /FI "PID eq 4100"', 'taskkill /FI "PID eq 9999"',
    'taskkill /FI "pid eq 4100" /F', 'taskkill /IM python.exe',
    'taskkill /IM python.exe /F', 'taskkill /IM python', 'taskkill /IM python*',
    'taskkill /IM PYTHON.EXE', 'taskkill /IM "python.exe"',
    'taskkill /IM C:\\Python312\\python.exe', 'taskkill /IM kimi',
    'taskkill /IM node.exe', 'taskkill /IM py', 'taskkill /IM py*',
    'taskkill /IM 123', 'taskkill /IM .py', 'taskkill.exe /IM python.exe',
    'taskkill /PID 9999 /IM python.exe', 'taskkill /IM python.exe /PID 9999',
    'TASKKILL /IM python.exe', 'taskkill /IM python.exe; kill 9999',
    'kill 9999; taskkill /IM python.exe', 'taskkill /im python.exe',
    # 3. Stop-Process / Get-Process
    'Stop-Process -Id 4100', 'stop-process -id 4100 -Force',
    'Stop-Process 4100', 'Stop-Process -Id 9999', 'Stop-Process -Id 4100,5000',
    'Stop-Process -Name python', 'Stop-Process -Name python.exe',
    'Stop-Process -Name py*', 'Stop-Process -Name node', 'Stop-Process -Name py',
    'Stop-Process -Name "python.exe"', 'Stop-Process -Id (Get-Process x).Id',
    'Get-Process -Id 4100 | Stop-Process', 'Get-Process python | Stop-Process',
    'Get-Process -Name python | Stop-Process -Force',
    'Get-Process -Id 9999 | Kill', '(Get-Process -Id 4100).Kill()',
    '(Get-Process -Id 9999).Kill()', 'Get-Process -Name python | kill',
    'Get-Process python', 'Stop-Process -Id 9999 | Kill 4100',
    'Get-Process | Stop-Process', 'Stop-Process -Name *python*',
    # 4. pkill / killall
    'pkill python', 'pkill -f python', 'pkill -9 python', 'pkill -9f python',
    'pkill --full python', 'pkill -f "bin/kimi"', 'pkill -f kimi-cli',
    "pkill -f '/usr/bin/python3'", 'pkill -f ^python$', 'pkill py.*',
    'pkill node', 'pkill -x python', 'pkill.exe python', 'pkill -F python',
    'pkill -f 4100', 'pkill -f "python3 /home/u/kimi-cli/bin/kimi"',
    'pkill -f "not in cmdline"', 'killall python', 'killall python.exe',
    'killall -9 python', 'killall node', 'killall py', 'killall.exe python',
    'pkill -f /usr/bin/python3', 'pkill -f worker', 'pkill -f --agent',
    'pkill ()', 'pkill $@', 'pkill python && echo ok',
    # 5. wmic
    'wmic process where ProcessId=4100 delete',
    'wmic process where ProcessId=4100 call terminate',
    'wmic process where processid=4100 delete',
    'wmic process where ProcessId=9999 delete',
    'wmic process where ProcessId=4100 get Name',
    'wmic process get ProcessId', 'wmic process where Name="python.exe" delete',
    'wmic.exe process where ProcessId=4100 delete',
    'wmic process where "ProcessId=4100" delete',
    # loops / variables
    'for pid in 4100 5000; do taskkill /PID $pid; done',
    'for pid in 9999 8888; do taskkill /PID $pid; done',
    'for pid in 4100 5000; do kill $pid; done',
    'for pid in 5000; do kill $pid; done',
    'for pid in $(pgrep python); do kill $pid; done',
    'for p in 4100; do kill $p; done',
    'for pid in 4100; do kill ${pid}; done',
    'for pid in 4100; do kill "$pid"; done',
    'for pid in 4100; do taskkill /PID "$pid"; done',
    'for pid in 4100; do wmic process where ProcessId=$pid delete; done',
    'for pid in 9999; do wmic process where ProcessId=$pid delete; done',
    'foreach ($pid in 4100,5000) { Stop-Process -Id $pid }',
    'foreach ($pid in 9999,8888) { Stop-Process -Id $pid }',
    'foreach ($p in 4100) { Stop-Process -Id $p }',
    'foreach ($pid in 4100,5000) { taskkill /PID $pid }',
    'foreach ($pid in @(4100)) { Stop-Process -Id $pid }',
    'foreach ($pid in $list) { Stop-Process -Id $pid }',
    'for pid in 5000 4100; do kill -9 $pid; done',
    # obfuscation / whitespace
    '  kill   4100  ', 'kill\n\t4100', 'KILL 4100 /f',
    'echo "kill 4100"', "echo 'kill 4100'", '# kill 4100',
    'kill 0x1004', 'kill 4_100',
    # safe / negative controls
    '', '   ', 'echo hello', 'Get-Date', 'git status', 'ls -la',
    'kill 1', 'taskkill /IM explorer.exe', 'pkill vim', 'killall vim',
    'Stop-Process -Name notepad', 'wmic process get Name',
    'docker kill 9999', 'print("kill 4100")', 'echo 4100',
]


def _iter_string_constants(path: Path) -> list[str]:
    """Every string literal in *path* (inputs and expected values alike)."""
    try:
        tree = ast.parse(path.read_text(encoding="utf-8", errors="replace"))
    except (OSError, SyntaxError):  # pragma: no cover - defensive
        return []
    out: list[str] = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            out.append(node.value)
    return out


def _iter_call_args(path: Path, names: set[str]) -> list[str]:
    """First string argument of every ``name(...)`` call in *path*."""
    try:
        tree = ast.parse(path.read_text(encoding="utf-8", errors="replace"))
    except (OSError, SyntaxError):  # pragma: no cover - defensive
        return []
    out: list[str] = []

    def callee(node: ast.Call) -> str | None:
        f = node.func
        if isinstance(f, ast.Name):
            return f.id
        if isinstance(f, ast.Attribute):
            return f.attr
        return None

    for node in ast.walk(tree):
        if isinstance(node, ast.Call) and node.args:
            if callee(node) in names:
                first = node.args[0]
                if isinstance(first, ast.Constant) and isinstance(first.value, str):
                    out.append(first.value)
    return out


def _seeded_fuzz(count: int, seed: int, alphabet: list[str], max_tokens: int = 12
                 ) -> list[str]:
    rng = random.Random(seed)
    return ["".join(rng.choice(alphabet) for _ in range(rng.randint(1, max_tokens)))
            for _ in range(count)]


def _clean(corpus: list[str]) -> list[str]:
    """ASCII, length-bounded, order-preserving dedupe."""
    seen: set[str] = set()
    out: list[str] = []
    for item in corpus:
        if not isinstance(item, str) or not item.isascii() or len(item) > MAX_LEN:
            continue
        if item in seen:
            continue
        seen.add(item)
        out.append(item)
    return out


_PS_ALPHABET = list('ab $?^:=(){}[]|&;,\'"`#<>@*\n\t\\/.0123-+_nulNUL>') + [
    '??', '??=', '?.', '?[', '--%', '@"', "@'", '"@', "'@", '<#', '#>',
    'echo', 'Write-Output', 'foreach', 'in', '#>', 'nul', 'NUL',
]
_KILL_ALPHABET = [
    'kill', 'taskkill', 'tskill', 'pkill', 'killall', 'Stop-Process',
    'Get-Process', 'wmic', 'delete', 'terminate', '4100', '5000', '9999',
    '/PID', '/IM', '/FI', '-Id', '-Name', '-f', '--full', '$pid', '${pid}',
    '$PID', '$$', 'for', 'in', 'do', 'done', 'foreach', 'python', 'python.exe',
    'bash', '-9', '/F', '|', ';', '&&', ' ', '\t', '\n', '"', "'", '(', ')',
    '.', ',', 'docker', 'process', 'where', 'ProcessId=',
]
_FIX_ALPHABET = list('ab "?$`#@\'()<>*-0123\n\t\\/.nulNUL>%') + [
    'nul', 'NUL', '--%', '@"', "@'", '"@', "'@", '<#', '#>', 'Write-Output',
    'echo', '>', '>>', '2>', '*>',
]


def build_corpora() -> dict[str, list[str]]:
    """The five golden corpora, in generated-file order."""
    kimi_tests = KIMI_AGENT_ROOT / "tests"
    transform = _clean(
        _iter_call_args(kimi_tests / "test_process_pwsh.py", {"pwsh_transform"})
        + TRANSFORM_CURATED
        + _seeded_fuzz(600, 20240921, _PS_ALPHABET))
    fix = _clean(
        _iter_string_constants(kimi_tests / "test_pwsh_fix.py")
        + FIX_CURATED
        + _seeded_fuzz(600, 20240922, _FIX_ALPHABET))
    self_kill = _clean(
        SELF_KILL_CURATED
        + _seeded_fuzz(400, 20240923, _KILL_ALPHABET))
    variants = _clean(
        SELF_KILL_CURATED
        + TRANSFORM_CURATED
        + FIX_CURATED
        + _seeded_fuzz(200, 20240924, _KILL_ALPHABET))
    hint = _clean(
        [c for c in SELF_KILL_CURATED if c.strip()]
        + _seeded_fuzz(120, 20240925, _KILL_ALPHABET))
    return {
        "transform": transform,
        "fix": fix,
        "self_kill": self_kill,
        "variants": variants,
        "hint": hint,
    }


# ---------------------------------------------------------------------------
# C++ literal emission
# ---------------------------------------------------------------------------

_ESC = {
    "\\": "\\\\",
    '"': '\\"',
    "\n": "\\n",
    "\r": "\\r",
    "\t": "\\t",
    "\v": "\\v",
    "\f": "\\f",
    "?": "\\?",
}


def cstr(text: str | None) -> str:
    """A C++ string literal; ``None`` becomes ``nullptr``.

    Non-printable bytes are emitted as fixed-width octal escapes so a following
    hex digit can never be swallowed by a greedy ``\\x`` escape.
    """
    if text is None:
        return "nullptr"
    out = ['"']
    for ch in text:
        if ch in _ESC:
            out.append(_ESC[ch])
        elif 0x20 <= ord(ch) < 0x7F:
            out.append(ch)
        else:
            out.append("\\%03o" % ord(ch))
    out.append('"')
    return "".join(out)


SEP = "\x1f"

#: Regex metacharacters that make a pkill pattern escape the native substring
#: subset (mirrors the routing gate documented in pwsh_tool.h; replicated here
#: so a row the kernel may legitimately report as ``unsupported`` is distinct
#: from a row it must answer natively).
_GATE_METACHARS = frozenset("[](){}+?|^$\\.")


def _gate_needs_python(safety, command: str) -> bool:
    """True when the pkill pattern gate must route *command* to Python."""
    text = " ".join(command.split()).lower()
    for match in _PKILL_RE.finditer(text):
        for token in safety._segment_tokens(text, match.end()):
            if safety._looks_like_flag(token):
                continue
            pattern = token.strip().strip("\"'")
            if any(ch in _GATE_METACHARS for ch in pattern):
                return True
    return False


def _native_expected(safety, command: str) -> bool:
    """True when the kernel must answer *command* itself (status ok)."""
    return not _gate_needs_python(safety, command)


def _native_expected_hint(safety, command: str) -> bool:
    """True when self_kill_hint may be answered natively for every variant."""
    return all(_native_expected(safety, variant)
               for variant in safety.command_detection_variants(command))


def build_goldens() -> str:
    """Render the whole ``pwsh_goldens.inc`` translation unit."""
    shell_compat, safety = load_reference()
    corpora = build_corpora()

    lines: list[str] = []
    add = lines.append
    add("// GENERATED by scripts/gen_pwsh_goldens.py - DO NOT EDIT BY HAND.")
    add("//")
    add("// Byte-exact expectations harvested from the kimi-agent reference:")
    add("//   * bin/kimix_native/_shell_compat.py  (pwsh_transform, fix_pwsh_command)")
    add("//   * src/kimix/tools/file/bash/safety.py (detect_self_kill, variants,")
    add("//     self_kill_hint) with the _compat_* helpers pinned to")
    add("//     bin/kimix_native/tools.py and a FIXED agent identity.")
    add("//")
    add(f"// Self-kill identity: protected_pids={{" +
        ", ".join(str(p) for p in PROTECTED_PIDS) + "},")
    add("// image_names={" + ", ".join(AGENT_IMAGE_NAMES) + "},")
    add("// cmdline=" + cstr(AGENT_CMDLINE) + ".")
    add("// Image-name ties (python vs python.exe) use ascending set order: the")
    add("// reference iterates an unordered set there, so its answer depends on")
    add("// the interpreter's hash seed (documented deviation, pinned here).")
    add("// List fields are joined with \\037 (unit separator).")
    add("")
    add("struct pwsh_transform_golden {")
    add("    const char *code;")
    add("    const char *expected_command;")
    add("    const char *expected_warnings; // joined with \\037")
    add("};")
    add("")
    add("struct pwsh_fix_golden {")
    add("    const char *command;")
    add("    bool valid;    // false == the reference returned None")
    add("    bool changed;")
    add("    const char *expected_command;")
    add("    const char *expected_warning;")
    add("};")
    add("")
    add("struct pwsh_self_kill_golden {")
    add("    const char *command;")
    add("    bool native; // true == the kernel must answer (status ok)")
    add("    const char *expected_description; // nullptr == safe")
    add("};")
    add("")
    add("struct pwsh_variants_golden {")
    add("    const char *command;")
    add("    const char *expected_variants; // joined with \\037")
    add("};")
    add("")
    add("struct pwsh_hint_golden {")
    add("    const char *command;")
    add("    int64_t agent_pid;             // pid embedded in the message")
    add("    bool native;                   // true == the kernel must answer")
    add("    const char *expected_hint;     // nullptr == safe")
    add("};")
    add("")

    transform_rows = []
    for code in corpora["transform"]:
        command, warnings = shell_compat.pwsh_transform(code)
        transform_rows.append((code, command, SEP.join(warnings)))
    add(f"// {len(transform_rows)} transform vectors")
    add("const pwsh_transform_golden k_pwsh_transform_goldens[] = {")
    for code, command, warnings in transform_rows:
        add(f"    {{{cstr(code)}, {cstr(command)}, {cstr(warnings)}}},")
    add("};")
    add("")

    fix_rows = []
    for command in corpora["fix"]:
        result = shell_compat.fix_pwsh_command(command)
        if result is None:
            fix_rows.append((command, False, False, "", ""))
        else:
            fix_rows.append((command, True, bool(result.warning), result.command,
                             result.warning))
    add(f"// {len(fix_rows)} fixer vectors")
    add("const pwsh_fix_golden k_pwsh_fix_goldens[] = {")
    for command, valid, changed, expected, warning in fix_rows:
        add(f"    {{{cstr(command)}, {'true' if valid else 'false'}, "
            f"{'true' if changed else 'false'}, {cstr(expected)}, "
            f"{cstr(warning)}}},")
    add("};")
    add("")

    self_kill_rows = []
    for command in corpora["self_kill"]:
        desc = safety.detect_self_kill(
            command, protected_pids=set(PROTECTED_PIDS),
            image_names=set(AGENT_IMAGE_NAMES), cmdline=AGENT_CMDLINE)
        self_kill_rows.append((command, _native_expected(safety, command), desc))
    add(f"// {len(self_kill_rows)} self-kill vectors")
    add("const pwsh_self_kill_golden k_pwsh_self_kill_goldens[] = {")
    for command, native, desc in self_kill_rows:
        add(f"    {{{cstr(command)}, {'true' if native else 'false'}, "
            f"{cstr(desc)}}},")
    add("};")
    add("")

    variant_rows = []
    for command in corpora["variants"]:
        variants = safety.command_detection_variants(command)
        variant_rows.append((command, SEP.join(variants)))
    add(f"// {len(variant_rows)} deobfuscation-variant vectors")
    add("const pwsh_variants_golden k_pwsh_variants_goldens[] = {")
    for command, variants in variant_rows:
        add(f"    {{{cstr(command)}, {cstr(variants)}}},")
    add("};")
    add("")

    agent_pid = AGENT_PID
    hint_rows = []
    for command in corpora["hint"]:
        hint_rows.append((command, _native_expected_hint(safety, command),
                          safety.self_kill_hint(command)))
    add(f"// {len(hint_rows)} self-kill-hint vectors (agent_pid={agent_pid})")
    add("const pwsh_hint_golden k_pwsh_hint_goldens[] = {")
    for command, native, hint in hint_rows:
        add(f"    {{{cstr(command)}, {agent_pid}, {'true' if native else 'false'}, "
            f"{cstr(hint)}}},")
    add("};")
    add("")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="fail when the checked-in file is out of date")
    parser.add_argument("--stdout", action="store_true",
                        help="print the generated text instead of writing it")
    parser.add_argument("--out", default=str(OUT_PATH),
                        help="output path (default: the checked-in .inc)")
    args = parser.parse_args()

    text = build_goldens()
    if args.stdout:
        sys.stdout.write(text)
        return 0
    out = Path(args.out)
    if args.check:
        current = out.read_text(encoding="utf-8") if out.is_file() else ""
        if current != text:
            print(f"OUT OF DATE: {out}", file=sys.stderr)
            return 1
        print(f"up to date: {out}")
        return 0
    out.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {out} ({len(text.splitlines())} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
