"""Shared helpers for the kimix-base <-> kimi-agent differential parity tests.

The C++ kernels under ``src/builtin_tools/`` are byte-exact ports of the
Python implementations living in the ``kimi-agent`` checkout.  The pure-Python
ground truth for every parity test is imported *directly from that checkout*
(never from ``kimix_native``'s own ``_compat`` mirrors, which would mask a
shared mistake).

Set ``KIMI_AGENT_ROOT`` to override the checkout location.  A test module that
cannot import a reference symbol must ``pytest.skip`` with a clear reason.

Two sys.path hazards make an unguarded ``import`` silently compare the wrong
thing — both are real, both were reproduced, so this module defends against
them instead of trusting import order:

1. ``kimi_cli.native_loader`` inserts ``<kimi-agent>/bin`` at ``sys.path[0]``,
   which holds a *released* ``runtime_py.pyd``.  Any later ``import runtime_py``
   then loads that stale extension and the port is compared against itself.
   ``native()`` therefore verifies ``runtime_py.__file__``; note that a
   *foreign* extension cannot be un-imported (CPython caches single-phase init
   extensions per module name), which is why ``python/tests/conftest.py``
   imports this checkout's ``kimix_native`` -- and with it this extension --
   before any ``kimi_cli`` module can run its loader.
2. ``<kimi-agent>/kimi-cli/src`` contains a ``kimix`` shim package that shadows
   ``<kimi-agent>/src/kimix``.  ``ref()`` verifies the resolved ``__file__`` is
   under the expected root and re-imports from a corrected ``sys.path`` when it
   is not.

Per-import defence is not enough for a *whole directory* run: pytest imports
every test module at collection time (no fixture runs between two module
imports), and each module is free to re-insert ``<kimi-agent>/kimi-cli/src`` at
``sys.path[0]``.  ``normalize_import_state()`` is the shared, idempotent repair
for that: it is called from ``python/tests/conftest.py`` before every module
collection and before every test, and it restores the one resolution order the
suite depends on:

* **this** checkout's ``bin/<mode>`` (``runtime_py``) and ``python/`` (the
  ``kimix_native`` shim) win over ``<kimi-agent>/bin`` (stale ``runtime_py.pyd``
  + kimi-agent's own shim), which is demoted to the end of ``sys.path``;
* the reference roots stay importable, ``<kimi-agent>/src`` *before*
  ``<kimi-agent>/kimi-cli/src``, so a fresh ``import kimix`` gets the real
  package and not kimi-cli's shim;
* modules already cached from the wrong root are purged so the next import
  re-resolves (a module object in ``sys.modules`` beats every ``sys.path``
  entry — that is what broke ``import kimix.tools.file`` after
  ``test_history_index``).
"""

from __future__ import annotations

import importlib
import os
import sys
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TESTS_DIR.parent.parent

KIMI_AGENT_ROOT = Path(os.environ.get("KIMI_AGENT_ROOT", r"C:/dev/kimi-agent"))
KIMI_CLI_SRC = KIMI_AGENT_ROOT / "kimi-cli" / "src"
KIMIX_SRC = KIMI_AGENT_ROOT / "src"

#: sys.path roots searched for the reference modules, in priority order.
_REF_ROOTS = (KIMIX_SRC, KIMI_CLI_SRC)


def _extension_of(bin_dir: Path) -> Path | None:
    """The ``runtime_py`` extension file inside *bin_dir* (or None if absent)."""
    for name in ("runtime_py.pyd", "runtime_py.so", "runtime_py.dylib"):
        cand = bin_dir / name
        if cand.is_file():
            return cand
    return None


def _bin_dir() -> Path | None:
    """Directory holding this repo's freshly built ``runtime_py`` extension.

    ``KIMIX_PARITY_BIN`` wins when set: ``python/tests/conftest.py`` pins the
    directory it selected there, so the extension ``conftest`` put first on
    ``sys.path`` and the one :func:`native` insists on can never disagree (two
    different ``runtime_py`` module objects in one run would make every
    ``isinstance`` comparison across modules bogus).
    """
    override = os.environ.get("KIMIX_PARITY_BIN")
    if override and (Path(override) / "runtime_py.pyd").is_file():
        return Path(override).resolve()
    if override and (Path(override) / "runtime_py.so").is_file():
        return Path(override).resolve()
    # No pin (imported outside the pytest conftest): walk the SAME mode order
    # ``python/tests/conftest.py`` uses -- it pins the directory it selected
    # through ``KIMIX_PARITY_BIN``, and any disagreement between the two would
    # make ``native()`` reject the very extension conftest put on ``sys.path``.
    for mode in ("release", "releasedbg", "debug", "check"):
        cand = REPO_ROOT / "bin" / mode
        if _extension_of(cand) is not None:
            return cand
    return None


BIN_DIR = _bin_dir()

#: This checkout's ``kimix_native`` shim (``python/kimix_native``): the copy
#: ``import kimix_native`` must resolve to.
PY_SHIM_DIR = REPO_ROOT / "python"

#: ``<kimi-agent>/bin``: holds a *released* ``runtime_py.pyd`` **and**
#: kimi-agent's own ``kimix_native`` shim.  ``kimi_cli.native_loader`` inserts
#: it at ``sys.path[0]``, so it is demoted to the end of ``sys.path`` again
#: before every test.
AGENT_BIN_DIR = KIMI_AGENT_ROOT / "bin"


def _purge(prefix: str) -> None:
    for name in [n for n in sys.modules if n == prefix or n.startswith(prefix + ".")]:
        del sys.modules[name]


def native():
    """Return the ``runtime_py`` extension built from *this* checkout.

    Raises ``RuntimeError`` when the extension cannot be found or loaded, so a
    mis-set environment fails loudly instead of comparing the port with itself.
    """
    if BIN_DIR is None:
        raise RuntimeError(
            f"no runtime_py extension under {REPO_ROOT / 'bin'} - build it first "
            "(python scripts/build_locked.py -- xmake build runtime_py)"
        )
    ext = _extension_of(BIN_DIR)
    if ext is None:  # pragma: no cover - BIN_DIR only names a dir that has one
        raise RuntimeError(f"no runtime_py extension in {BIN_DIR}")
    expected = ext.resolve()
    # Purge first (so a module that was re-imported under this name after a
    # failed load cannot linger), then import with our bin dir first on
    # sys.path.
    #
    # NOTE: this purge can NOT recover a *foreign* extension.  CPython caches
    # single-phase init extension modules per module NAME (import.c
    # ``_PyImport_FixupExtensionObject`` -> ``interp->extensions``), so once
    # ``runtime_py`` has been imported from ``<kimi-agent>/bin`` every later
    # ``import runtime_py`` hands back that very object, whatever sys.path says
    # -- verified: ``importlib.util.find_spec("runtime_py")`` reports THIS
    # checkout's ``bin/<mode>/runtime_py.pyd`` while the import still returns
    # kimi-agent's released one.  The only defence is import *order*:
    # ``python/tests/conftest.py`` imports this checkout's ``kimix_native`` (and
    # with it this extension) before any ``kimi_cli`` module can pull in
    # ``<kimi-agent>/bin``.  The check below then turns a poisoned session into
    # a loud failure instead of a silent port-vs-itself comparison.
    _purge("runtime_py")
    sys.path.insert(0, str(BIN_DIR))
    import runtime_py  # noqa: PLC0415

    got = Path(runtime_py.__file__).resolve()
    if got != expected:
        # Something re-inserted a foreign bin dir ahead of ours; retry once.
        _purge("runtime_py")
        sys.path.insert(0, str(BIN_DIR))
        import runtime_py  # noqa: PLC0415, F811

        got = Path(runtime_py.__file__).resolve()
        if got != expected:
            raise RuntimeError(
                "imported runtime_py is not this checkout's build:\n"
                f"  expected: {expected}\n  got:      {got}\n"
                "kimi-agent's bin directory is shadowing it."
            )
    return runtime_py


def _pin_front(paths) -> None:
    """Move *paths* to the front of ``sys.path``, in the order given."""
    strs = [str(p) for p in paths if p and os.path.isdir(str(p))]
    for s in strs:
        while s in sys.path:
            sys.path.remove(s)
    for s in reversed(strs):
        sys.path.insert(0, s)


def _demote(paths) -> None:
    """Move *paths* to the end of ``sys.path`` (keep them importable last)."""
    for p in paths:
        s = str(p)
        if not os.path.isdir(s):
            continue
        while s in sys.path:
            sys.path.remove(s)
        sys.path.append(s)


def _ensure_paths() -> None:
    """Put the reference roots first, in priority order (KIMIX_SRC wins)."""
    for p in reversed(_REF_ROOTS):
        s = str(p)
        if not os.path.isdir(s):
            continue
        while s in sys.path:
            sys.path.remove(s)
        sys.path.insert(0, s)


def _purge_foreign(module: str, root: Path | None) -> bool:
    """Drop the cached *module* when it came from outside *root*.

    Returns True when something was purged.  A module without ``__file__``
    (a synthetic stub a test installed into ``sys.modules``) is left alone: its
    origin cannot be proven, and purging it would break the test that made it.
    """
    mod = sys.modules.get(module)
    if mod is None:
        return False
    path = getattr(mod, "__file__", None)
    if not path:
        return False
    if root is not None and _under(path, root):
        return False
    _purge(module)
    return True


def _purge_incomplete_kimix_stub() -> bool:
    """Drop a hand-built ``kimix`` stub that cannot serve submodules.

    ``test_parse.py`` installs a synthetic ``kimix`` module with
    ``__path__ = []`` so it can load kimi-agent's parser files by path without
    importing the heavy real package (``base.py`` imports ``kimi_cli`` at module
    level).  That stub is a session-wide poison: ``kimix.__path__`` empty means
    *every* later ``import kimix.tools.<...>`` fails with
    ``No module named 'kimix.tools'``, which is how
    ``test_parity_pwsh.py`` (loading kimi-agent's ``bash/safety.py``, which does
    ``from kimix.tools.security import validate_workdir``) broke when it was
    collected *after* ``test_parse``.  ``test_parse``'s own tests only use the
    parser classes it captured at import time, so dropping the stub later is
    safe.  A real (or namespace) ``kimix`` package always has a non-empty
    ``__path__`` and is left alone.
    """
    mod = sys.modules.get("kimix")
    if mod is None or getattr(mod, "__file__", None):
        return False
    path = getattr(mod, "__path__", None)
    if path is None or len(path) > 0:
        return False
    _purge("kimix")
    return True


def normalize_import_state() -> None:
    """Restore the one import order this suite is written against.

    Idempotent and cheap (a handful of list edits plus dict lookups), so it is
    safe to call before every module collection and every test.  Without it the
    suite is order-dependent: whichever test imported ``kimi_cli`` first decides
    where ``runtime_py``, ``kimix_native`` and ``kimix`` come from for every
    later module, and ``test_history_index`` (which puts
    ``<kimi-agent>/kimi-cli/src`` at ``sys.path[0]``) reliably poisons them.
    """
    # 1. This checkout's extension + shim first, then the reference roots with
    #    <kimi-agent>/src ahead of kimi-cli (whose `kimix` is only a shim).
    _pin_front((BIN_DIR, PY_SHIM_DIR, KIMIX_SRC, KIMI_CLI_SRC))
    # 2. <kimi-agent>/bin last: its runtime_py.pyd is an OLD release.
    _demote((AGENT_BIN_DIR,))
    # 3. Purge anything cached from the wrong root so the next import
    #    re-resolves.  ``kimix_native`` is an ordinary package, so purging it is
    #    enough; a foreign ``runtime_py`` is deliberately left alone (it cannot
    #    be un-cached -- see native()), and purging the shim underneath it would
    #    only make this checkout's shim bind the foreign extension.
    _purge_foreign("kimix_native", PY_SHIM_DIR)
    _purge_foreign("kimix", KIMIX_SRC)
    _purge_incomplete_kimix_stub()
    _purge_foreign("kimi_cli", KIMI_CLI_SRC)


def _expected_root(module: str) -> Path | None:
    if module == "kimix" or module.startswith("kimix."):
        return KIMIX_SRC
    if module == "kimi_cli" or module.startswith("kimi_cli."):
        return KIMI_CLI_SRC
    return None


def _under(path, root: Path) -> bool:
    """True when *path* is a resolved file inside *root*."""
    if not path:
        return False
    try:
        return root.resolve() in Path(path).resolve().parents
    except (OSError, ValueError):
        return False


def ref(module: str):
    """Import a reference module from the kimi-agent checkout.

    Defensive about two shadowing traps that silently produce a wrong "ground
    truth" (both reproduced in a full-suite run):

    * ``kimi-cli/src/kimix`` is a shim *package* whose ``__init__.py`` makes it
      win over ``<kimi-agent>/src/kimix`` (which has no ``tools/__init__.py``),
      so ``import kimix.tools.…`` raises ``ModuleNotFoundError`` once anything
      imported the shim first;
    * a module cached under the wrong root (from an earlier test's sys.path
      manipulation) keeps winning even after sys.path is corrected.

    Both are handled by purging the offending top-level package and retrying
    once with the reference roots pinned at the front of ``sys.path``.
    """
    _ensure_paths()
    root = _expected_root(module)
    if root is None:
        return importlib.import_module(module)
    top = module.split(".")[0]

    if top in sys.modules and not _under(getattr(sys.modules[top], "__file__", None), root):
        _purge(top)  # e.g. kimi-cli's shim 'kimix' package

    last: Exception | None = None
    for attempt in range(2):
        try:
            mod = importlib.import_module(module)
        except Exception as exc:  # shadowed package: purge and retry once
            last = exc
            if attempt == 0:
                _purge(top)
                _ensure_paths()
                continue
            raise
        if _under(getattr(mod, "__file__", None), root):
            return mod
        last = RuntimeError(
            f"reference module {module!r} resolved outside {root}: "
            f"{getattr(mod, '__file__', None)}"
        )
        if attempt == 0:
            _purge(top)
            _ensure_paths()
            continue
    raise last  # pragma: no cover - both attempts exhausted


def ref_available() -> bool:
    """True when the kimi-agent checkout and its reference modules import."""
    if not (KIMIX_SRC / "kimix").is_dir():
        return False
    try:
        ref("kimi_cli.tools.file.output_utils")
    except Exception:
        return False
    return True


def is_windows() -> bool:
    return os.name == "nt"


class pure_python:
    """Context manager forcing kimi-agent reference modules off their native path.

    kimi-agent's own modules short-circuit to ``kimix_native`` when available.
    Comparing our kernels against *that* would only prove two native libraries
    agree; the parity target is the original Python body, so every use of this
    helper disables the reference module's native gate for the duration.
    """

    def __init__(self, *modules):
        self._modules = modules
        self._saved = []

    def __enter__(self):
        for mod in self._modules:
            for attr in ("_native_use_native", "_use_native"):
                if hasattr(mod, attr):
                    self._saved.append((mod, attr, getattr(mod, attr)))
                    setattr(mod, attr, lambda *_a, **_k: False)
            for attr in ("_NATIVE_GLOB", "_NATIVE_TOOLS", "_NATIVE", "_native"):
                if hasattr(mod, attr):
                    self._saved.append((mod, attr, getattr(mod, attr)))
                    setattr(mod, attr, None)
        return self

    def __exit__(self, *exc):
        for mod, attr, value in reversed(self._saved):
            setattr(mod, attr, value)
        self._saved = []
        return False
