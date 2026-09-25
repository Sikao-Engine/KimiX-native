"""pytest fixture: make the compiled extension and the shim importable.

The compiled extension module ``runtime_py.pyd`` lives in the xmake targetdir
(bin/<mode>, default ``bin/debug`` for debug builds). The shim package lives in
``python/kimix_native``.
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # repo root


def _prepend(path):
    if path and path not in sys.path:
        sys.path.insert(0, path)


_EXTENSION_SUFFIXES = (".pyd", ".so", ".dylib")


def _has_extension(directory):
    """True when ``directory`` holds a built runtime_py extension."""
    try:
        entries = os.listdir(directory)
    except OSError:
        return False
    return any(
        name.startswith("runtime_py.")
        and name.endswith(_EXTENSION_SUFFIXES)
        and os.path.isfile(os.path.join(directory, name))
        for name in entries
    )


# Locate the xmake targetdir: the first bin/<mode> that actually holds the
# compiled extension.  A mode directory only counts when it actually contains
# the extension: building a single target in a new mode creates bin/<mode> with
# libraries but no runtime_py, and selecting that directory would silently
# import the shim without its native kernels (surfacing as confusing
# `'NoneType' has no attribute ...`).
#
# The mode ORDER is a contract, not a preference: the parity modules resolve the
# same directory themselves ("conftest's order" in their docstrings), so a
# directory that holds no extension must simply be skipped rather than
# re-ranked by mtime -- a freshness rule here would make this pin disagree with
# those modules and trip their provenance asserts.
BIN = None
# Prefer the release build when it exists; stale debug artifacts would
# otherwise shadow the freshly-built release extension.
for mode in ("release", "releasedbg", "debug", "check"):
    cand = os.path.join(ROOT, "bin", mode)
    if os.path.isdir(cand) and _has_extension(cand):
        BIN = cand
        break
if BIN is None:
    bin_root = os.path.join(ROOT, "bin")
    if os.path.isdir(bin_root):
        for entry in sorted(os.listdir(bin_root)):
            cand = os.path.join(bin_root, entry)
            if os.path.isdir(cand) and _has_extension(cand):
                BIN = cand
                break
if BIN:
    _prepend(BIN)  # runtime_py.pyd

_prepend(os.path.join(ROOT, "python"))  # kimix_native shim

# ---------------------------------------------------------------------------
# Deterministic import order for a whole-directory run
#
# sys.path alone is not enough: pytest imports EVERY test module at collection
# time (no fixture runs between two module imports) and several modules
# re-insert `<kimi-agent>/kimi-cli/src` at sys.path[0] on import.  That makes
# the resolution of `runtime_py`, `kimix_native` and `kimix` depend on which
# module happened to be imported first:
#   * `kimi_cli.native_loader` puts `<kimi-agent>/bin` (an OLD released
#     runtime_py.pyd + kimi-agent's own kimix_native shim) at sys.path[0], so a
#     later `import runtime_py` / `import kimix_native` silently loads the stale
#     pair;
#   * `<kimi-agent>/kimi-cli/src/kimix` is a shim *package* that wins over
#     `<kimi-agent>/src/kimix` once kimi-cli is imported, and a module object
#     already in sys.modules beats every sys.path entry, so
#     `import kimix.tools.file` then raises ModuleNotFoundError everywhere.
#
# `_parity_ref.normalize_import_state()` repairs all of it (pins this
# checkout's bin/ + python/ first, demotes <kimi-agent>/bin, purges modules
# cached from the wrong root) and is re-run before every module collection and
# before every test.  Importing the shim HERE, before anything can import
# kimi_cli, also means kimi_cli's loader finds this checkout's kimix_native
# instead of stageing kimi-agent's.
# ---------------------------------------------------------------------------
import pytest  # noqa: E402

_TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
if _TESTS_DIR not in sys.path:
    sys.path.insert(0, _TESTS_DIR)

if BIN:
    # Keep _parity_ref.BIN_DIR identical to the extension we put on sys.path:
    # two different runtime_py module objects in one process would make every
    # cross-module isinstance check bogus.
    os.environ["KIMIX_PARITY_BIN"] = BIN

import _parity_ref  # noqa: E402

_parity_ref.normalize_import_state()
try:  # pin THIS checkout's shim before kimi_cli.native_loader can pick kimi-agent's
    import kimix_native  # noqa: E402,F401
except ImportError:  # pragma: no cover - no build/shim staged
    pass
_parity_ref.normalize_import_state()

# Every parity module asserts `runtime_py.__file__` is this checkout's build.
# That can only be guaranteed by *ordering*: CPython caches single-phase init
# extension modules per module name, so if anything imported kimi_cli (and thus
# kimi-agent's staged runtime_py) before this conftest ran, every later
# `import runtime_py` would return that foreign object -- and every comparison
# would silently be the port against itself.  Fail loudly instead.
_EXT = sys.modules.get("runtime_py")
if _EXT is not None and BIN:
    _ext_file = os.path.abspath(getattr(_EXT, "__file__", "") or "")
    if os.path.dirname(_ext_file).lower() != os.path.abspath(BIN).lower():
        raise RuntimeError(
            "runtime_py was already imported from\n  "
            f"{_ext_file}\ninstead of this checkout's build in\n  {BIN}\n"
            "Something imported kimi_cli (kimi_cli.native_loader inserts "
            "kimi-agent's staged bin/) before python/tests/conftest.py ran, and "
            "CPython's extension cache makes that irreversible in this process. "
            "Re-run pytest without preloading kimi_cli (a plugin/plugin entry "
            "point or -p option is the usual cause)."
        )


def pytest_collectstart(collector):
    """Restore the import order before a module is imported for collection."""
    _parity_ref.normalize_import_state()


@pytest.fixture(autouse=True)
def _deterministic_import_state():
    """Restore the import order before every test (module-level code may have
    wrecked it, e.g. test_history_index's ``sys.path.insert(0, ...)``)."""
    _parity_ref.normalize_import_state()
    yield
