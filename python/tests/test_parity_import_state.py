"""Integration regressions for the differential parity suite itself.

The per-tool ``test_parity_<tool>.py`` modules each pass on their own, yet the
whole directory used to fail depending on *collection order*: pytest imports
every test module before any fixture runs, and several modules re-insert
``<kimi-agent>/kimi-cli/src`` at ``sys.path[0]``.  That made three separate
imports resolve to the wrong file for every later test:

1. ``kimi_cli.native_loader`` puts ``<kimi-agent>/bin`` (a *released*
   ``runtime_py.pyd``) at ``sys.path[0]``, so a later ``import runtime_py``
   compared the port against itself;
2. ``<kimi-agent>/kimi-cli/src/kimix`` is a shim *package*: once it is cached in
   ``sys.modules`` nothing else can win, and ``import kimix.tools.file`` (used by
   the reference ``bash_fix``/``common`` modules) raises ``ModuleNotFoundError``;
3. ``python/kimix_native/_shell_compat.py`` (the vendored mirror) was stale, so
   kimi-agent's ``bash_fix.py:110`` raised ``AttributeError`` on
   ``_UNSUPPORTED_BODIES`` as soon as this checkout's shim won ``import
   kimix_native``.

``python/tests/conftest.py`` + ``_parity_ref.normalize_import_state()`` repair
all three before every module collection and every test.  Each test below
reproduces the hazard in a *fresh interpreter* (so the repair is the only thing
under test) and then asserts the repaired state; the hand-off to the
subprocess makes them independent of whatever ran before in this process.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import _parity_ref as pr  # noqa: E402

REPO_ROOT = pr.REPO_ROOT
AGENT_ROOT = pr.KIMI_AGENT_ROOT
AGENT_BIN = AGENT_ROOT / "bin"
AGENT_SHIM = AGENT_ROOT / "kimi-cli" / "src" / "kimix"

pytestmark = pytest.mark.skipif(
    pr.BIN_DIR is None, reason="no kimix-base runtime_py build found under bin/"
)

_HAS_AGENT_NATIVE = (AGENT_BIN / "runtime_py.pyd").is_file() or (
    AGENT_BIN / "runtime_py.so"
).is_file()
_HAS_AGENT_SHIM = (AGENT_SHIM / "__init__.py").is_file()


def _run_snippet(body: str, timeout: int = 300) -> dict[str, str]:
    """Run *body* in a fresh interpreter and return its ``KEY=value`` lines."""
    preamble = (
        "import sys\n"
        "from pathlib import Path\n"
        f"_BASE = Path(r'{REPO_ROOT}')\n"
        f"_AGENT = Path(r'{AGENT_ROOT}')\n"
        "sys.path.insert(0, str(_BASE / 'python'))\n"
        f"sys.path.insert(0, r'{pr.BIN_DIR}')\n"
    )
    proc = subprocess.run(
        [sys.executable, "-u", "-c", preamble + body],
        capture_output=True,
        text=True,
        timeout=timeout,
        env=dict(os.environ),
    )
    assert proc.returncode == 0, f"subprocess failed:\n{proc.stdout}\n{proc.stderr}"
    out: dict[str, str] = {}
    for line in proc.stdout.splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            out[key.strip()] = value.strip()
    return out


@pytest.mark.skipif(not _HAS_AGENT_NATIVE, reason="kimi-agent has no staged runtime_py")
def test_loader_cannot_steal_runtime_py_or_the_shim():
    """Symptom 1: after ``kimi_cli`` was imported, ``import runtime_py`` and
    ``import kimix_native`` must still resolve to THIS checkout.

    The bootstrap below is exactly what ``conftest.py`` does (pin ``bin/<mode>``
    + ``python/`` and import the shim), then it does what ``test_history_index``
    does (``sys.path.insert(0, <kimi-agent>/kimi-cli/src)``) and imports the
    loader, which inserts ``<kimi-agent>/bin`` at ``sys.path[0]``.  Before the
    fix the later ``import runtime_py`` / ``import kimix_native`` picked up
    kimi-agent's released pair.
    """
    out = _run_snippet(
        "sys.path.insert(0, str(_BASE / 'python' / 'tests'))\n"
        "import _parity_ref\n"
        "_parity_ref.normalize_import_state()\n"
        "import kimix_native as shim\n"
        "sys.path.insert(0, str(_AGENT / 'kimi-cli' / 'src'))\n"
        "import kimi_cli.native_loader as nl\n"
        "import runtime_py\n"
        "print('loader_path=' + str(nl.NATIVE_PATH))\n"
        "print('runtime=' + str(Path(runtime_py.__file__).resolve().parent))\n"
        "print('shim=' + str(Path(shim.__file__).resolve().parent))\n"
        "print('shim_native_same=' + str(shim._native is runtime_py))\n"
        "print('path_len=' + str(len(sys.path)))\n"
        "_parity_ref.normalize_import_state()\n"
        f"print('bin_index=' + str(sys.path.index(r'{pr.BIN_DIR}')))\n"
        f"print('shim_index=' + str(sys.path.index(str(_BASE / 'python'))))\n"
        "print('agent_bin_index_after=' + str(sys.path.index(str(_AGENT / 'bin'))))\n"
    )
    # The loader still reports kimi-agent's dir as NATIVE_PATH (it resolved that
    # before we demoted it) -- harmless, because the import it did was ours.
    assert Path(out["loader_path"]) == AGENT_BIN, out
    assert Path(out["runtime"]) == Path(pr.BIN_DIR).resolve(), out
    assert Path(out["shim"]) == (REPO_ROOT / "python" / "kimix_native"), out
    assert out["shim_native_same"] == "True", out
    # This checkout's extension + shim are first, kimi-agent's bin/ is behind
    # them, so it can no longer shadow anything the tests import.
    assert int(out["bin_index"]) == 0, out
    assert int(out["shim_index"]) == 1, out
    assert int(out["agent_bin_index_after"]) > int(out["shim_index"]), out


@pytest.mark.skipif(not _HAS_AGENT_NATIVE, reason="kimi-agent has no staged runtime_py")
def test_purging_sys_modules_cannot_recover_a_foreign_extension():
    """Why *ordering* (not purging) is the fix for symptom 1.

    Characterises CPython: single-phase init extension modules are cached per
    module *name*, so deleting ``sys.modules['runtime_py']`` and putting the
    right directory first still returns the foreign object -- while
    ``importlib.util.find_spec`` happily reports the right file.  This is the
    evidence behind ``conftest.py``'s pre-import and its loud poisoned-session
    check; if a future CPython changes this behaviour, this test is the canary.
    """
    out = _run_snippet(
        "sys.path.insert(0, str(_AGENT / 'kimi-cli' / 'src'))\n"
        "import kimi_cli.native_loader\n"
        "import runtime_py\n"
        "print('first=' + str(Path(runtime_py.__file__).resolve().parent))\n"
        "del sys.modules['runtime_py']\n"
        f"sys.path.insert(0, r'{pr.BIN_DIR}')\n"
        "import importlib.util\n"
        "spec = importlib.util.find_spec('runtime_py')\n"
        "print('find_spec=' + str(Path(spec.origin).resolve().parent))\n"
        "import runtime_py\n"
        "print('second=' + str(Path(runtime_py.__file__).resolve().parent))\n"
    )
    assert Path(out["first"]) == AGENT_BIN, out
    # find_spec says ours ...
    assert Path(out["find_spec"]) == Path(pr.BIN_DIR).resolve(), out
    # ... but the import still returns kimi-agent's already-initialised module.
    assert Path(out["second"]) == AGENT_BIN, out


@pytest.mark.skipif(not _HAS_AGENT_SHIM, reason="kimi-agent has no kimix shim package")
def test_foreign_kimix_shim_is_purged_by_normalize():
    """Symptom 2: kimi-cli's ``kimix`` shim must not shadow the real package."""
    out = _run_snippet(
        "_AGENT_SRC = _AGENT / 'src'\n"
        "sys.path.insert(0, str(_AGENT_SRC))\n"
        "sys.path.insert(0, str(_AGENT / 'kimi-cli' / 'src'))\n"
        "import kimix\n"
        "print('shim_won=' + str('kimi-cli' in str(Path(kimix.__file__).resolve())))\n"
        "try:\n"
        "    import kimix.tools.file\n"
        "    print('hazard_tools=importable')\n"
        "except ModuleNotFoundError as exc:\n"
        "    print('hazard_tools=' + str(exc))\n"
        "sys.path.insert(0, str(_BASE / 'python' / 'tests'))\n"
        "import _parity_ref\n"
        "_parity_ref.normalize_import_state()\n"
        "import kimix, kimix.tools.file\n"
        "from kimix.retrieval import InvertedIndex, NgramTokenizer  # noqa: F401\n"
        "print('fixed_kimix=' + str(Path(kimix.__file__).resolve().parent))\n"
        "print('fixed_tools=' + str(Path(kimix.tools.file.__file__).resolve().parent))\n"
        "print('fixed_retrieval=ok')\n"
    )
    assert out["shim_won"] == "True", out
    # The shim has no `tools/file` subpackage: the later import must have failed.
    assert out["hazard_tools"].startswith("No module named"), out
    assert Path(out["fixed_kimix"]) == (AGENT_ROOT / "src" / "kimix"), out
    assert Path(out["fixed_tools"]) == (AGENT_ROOT / "src" / "kimix" / "tools" / "file"), out
    assert out["fixed_retrieval"] == "ok", out


def test_shell_compat_mirror_has_the_reference_tables():
    """Symptom 4: the vendored mirror must expose what ``bash_fix.py`` re-exports.

    kimi-agent's ``src/kimix/tools/file/bash/bash_fix.py`` binds
    ``_UNSUPPORTED_BODIES`` and the newer ``free``/``uptime``/``top``/``ss``
    fallback bodies straight off the shim, so a stale mirror makes the reference
    module raise ``AttributeError`` at import time (``bash_fix.py:110``).
    """
    pr.ref_available() or pytest.skip("kimi-agent reference not importable")
    bash_fix = pr.ref("kimix.tools.file.bash.bash_fix")
    assert bash_fix._UNSUPPORTED_BODIES, "reference exposes no unsupported table"
    for name in ("free", "uptime", "top", "ss"):
        assert name in bash_fix._FALLBACK_BODIES, name
    # The reference resolves those tables through whichever `kimix_native`
    # wins, which must be the mirror that ships with THIS checkout.
    import kimix_native._shell_compat as shell_mod

    assert Path(shell_mod.__file__).resolve() == (
        REPO_ROOT / "python" / "kimix_native" / "_shell_compat.py"
    ).resolve()


def _golden_commands() -> list[str]:
    """The reference corpus: every command in ``bash_fix_goldens.inc``."""
    path = REPO_ROOT / "tests" / "unit" / "builtin_tools" / "bash_fix_goldens.inc"
    if not path.is_file():
        return []
    text = path.read_text(encoding="utf-8")
    raws = re.findall(r'^\s*\{"((?:[^"\\]|\\.)*)"', text, re.M)
    return [
        raw.encode("utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")
        if "\\" in raw
        else raw
        for raw in raws
    ]


    def test_native_parse_prefix_matches_the_reference_corpus():
        """``kimix_native.parse`` must agree with the vendored reference everywhere.

        ``parse.py``'s native path takes the fallback *names* from the compiled
        ``parse.shell_scan`` kernel and the bodies from the vendored
        ``_shell_compat`` tables.  The kernel's BASH_FIX name tables (fallback
        names, fallback command wrappers and the unsupported set) are generated
        from the reference by ``scripts/gen_bash_fix_data.py --tables-runtime``,
        so the newest fallback names (``free``/``uptime``/``top``/``ss``/``ip``/
        ``man``/``systemctl``/``htop``/``sudo``) and ``journalctl``'s
        unsupported verdict are the kernel's own - there is no longer a
        ``_POST_KERNEL_FALLBACKS`` bridge diverting them to the mirror.  This
        sweeps the committed golden corpus - generated from the reference - so a
        stale mirror or a drifted kernel table shows up as a real command diff
        instead of silently passing on both sides.
        """
    import kimix_native.parse as P
    import kimix_native._shell_compat as SC

    commands = _golden_commands()
    if not commands:
        pytest.skip("bash_fix_goldens.inc not present")
    assert len(commands) > 1000, len(commands)
    bad = []
    for cmd in commands:
        n = P.fix_bash_command(cmd)
        c = SC.fix_bash_command(cmd)
        if (n.command, n.replacements, n.path_changes, n.nul_fixes) != (
            c.command,
            c.replacements,
            c.path_changes,
            c.nul_fixes,
        ):
            bad.append((cmd, n.replacements, c.replacements))
    assert not bad, f"{len(bad)}/{len(commands)} commands differ: {bad[:5]}"
