"""Differential parity tests for the python builtin tool (kimix-base <-> kimi-agent).

``src/builtin_tools/python_tool.{h,cpp}`` is a port of kimi-agent's python tool
and of the shared helpers it calls (``kimix/tools/py/__init__.py``,
``kimix/tools/common.py``, ``kimix/tools/security.py``,
``kimix/tools/background/utils.py``).  This module compares the C++ kernels --
reachable through ``runtime_py.builtin_tools.python`` -- against the *original*
implementation imported straight from the kimi-agent checkout (never from
``kimix-base/python/kimix_native``, whose ``_compat`` mirrors were written
alongside the port and would mask a shared mistake):

* ``plan_script_path``            <- ``common.py`` ``_create_script_file``
* ``resolve_python_exe``          <- ``py/__init__.py`` ``_resolve_python_uncached``
* ``scrub_child_env``             <- ``security.py`` ``scrub_child_env``
* ``prepare_python_env``          <- ``py/__init__.py`` ``_build_env``
* ``module_not_found_hint``       <- ``py/__init__.py`` ``_module_not_found_hint``
* ``build_session_output_block``  <- ``common.py`` ``_build_session_output_block``
* ``extract_export_path``         <- ``common.py`` ``_extract_export_path``
* ``classify/match_wait_pattern`` <- ``background/utils.py`` ``pattern.search``

The remaining kernels (the tool's own ``Python`` Tool class, which spawns the
child process, and everything that needs CPython's ``compile()`` or an LLM
call) are not exposed to Python; the tool's own goldens
(``scripts/gen_python_goldens.py`` -> ``tests/unit/builtin_tools/python_goldens.inc``,
replayed by ``test_builtin_python``) carry that half, and
``test_goldens_inc_is_current`` below keeps the two halves in sync.

Provenance rules (learned the hard way, same as the sibling bash/grep suites):

* Import the freshly built kimix-base extension *before* touching the kimi-agent
  reference: importing kimi-agent runs ``kimi_cli.native_loader._setup()``, which
  puts ``<kimi-agent>/bin`` -- a staged, older ``runtime_py.pyd`` -- first on
  ``sys.path``, and a later ``import runtime_py`` would quietly compare against
  that stale binary.
* ``kimix.tools.security`` / ``kimix.tools.common`` short-circuit to
  ``kimix_native`` when available; every use here goes through
  ``_parity_ref.pure_python`` so the *original Python body* is what the port is
  compared against.

ASCII gate: ``scrub_child_env``'s native path is only taken by the reference
itself when every env name is ASCII (``security.py``), and the C++ kernels are
defined for ASCII input; the corpora below stay inside that gate.
"""

from __future__ import annotations

import importlib.util
import os
import random
import subprocess
import sys
from contextlib import contextmanager
from pathlib import Path

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

_KIMIX_BASE_ROOT = Path(__file__).resolve().parents[2]


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


# IMPORT ORDER IS LOAD-BEARING: see the module docstring.
_BIN_DIR = _kimix_base_bin_dir()
if _BIN_DIR is not None:
    sys.path.insert(0, str(_BIN_DIR))
import runtime_py  # noqa: E402

if _BIN_DIR is None:  # pragma: no cover - defensive
    pytest.skip(
        "no kimix-base runtime_py build found under bin/", allow_module_level=True
    )

_NATIVE_PATH = Path(runtime_py.__file__).resolve()
assert _BIN_DIR.resolve() in _NATIVE_PATH.parents, (
    f"runtime_py came from {_NATIVE_PATH}, not the kimix-base build {_BIN_DIR} "
    f"-- a staged copy shadowed it; parity results would be bogus"
)

if not hasattr(runtime_py.builtin_tools, "python"):  # pragma: no cover - defensive
    pytest.skip(
        "runtime_py.builtin_tools.python is missing - rebuild runtime_py "
        "(python scripts/build_locked.py -- xmake build runtime_py)",
        allow_module_level=True,
    )

from _parity_ref import pure_python, ref, ref_available  # noqa: E402

pytestmark = pytest.mark.skipif(
    not ref_available(), reason="kimi-agent checkout not available"
)

PY = runtime_py.builtin_tools.python

# Reference modules (imported from the kimi-agent checkout only).
PY_REF = ref("kimix.tools.py")
COMMON_REF = ref("kimix.tools.common")
SECURITY_REF = ref("kimix.tools.security")


# ---------------------------------------------------------------------------
# reference helpers
# ---------------------------------------------------------------------------


class _SessionStub:
    """Minimal stand-in for kimi_cli Session (only ``.dir`` is read)."""

    def __init__(self, directory: str) -> None:
        self.dir = directory


@contextmanager
def fake_filesystem(allowed: set[str], cwd: str | None = None):
    """Drive ``Path.is_file`` / ``Path.cwd`` from an allow-list.

    The C++ kernels take the same information as an injected probe
    (``kimix::function<bool(kimix::string_view)>``), so both sides see the same
    fake filesystem without touching the real one.
    """
    orig_is_file = Path.is_file
    orig_cwd = Path.cwd
    Path.is_file = lambda self: str(self) in allowed  # noqa: ARG005
    if cwd is not None:
        Path.cwd = classmethod(lambda cls, c=cwd: Path(c))
    try:
        yield
    finally:
        Path.is_file = orig_is_file
        Path.cwd = orig_cwd


@contextmanager
def environ(**values: str | None):
    """Set/clear environment variables, restoring the previous state."""
    saved = {k: os.environ.get(k) for k in values}
    try:
        for key, value in values.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        yield
    finally:
        for key, value in saved.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


def _load_generator():
    """Load scripts/gen_python_goldens.py by path (it is not a package)."""
    path = _KIMIX_BASE_ROOT / "scripts" / "gen_python_goldens.py"
    if not path.is_file():
        return None
    spec = importlib.util.spec_from_file_location("_gen_python_goldens", path)
    if spec is None or spec.loader is None:  # pragma: no cover - defensive
        return None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


REGEX_META = set(".*?+{()[^$|\\")


def needs_regex_engine(pattern: str) -> bool:
    """True when a literal substring search cannot reproduce the reference."""
    return bool(pattern) and (bool(set(pattern) & REGEX_META) or not pattern.isascii())


# ---------------------------------------------------------------------------
# 1. script path planning (common.py _create_script_file)
# ---------------------------------------------------------------------------


def test_plan_script_path_matches_reference(tmp_path: Path) -> None:
    saved_folder = COMMON_REF._temp_folder
    saved_idx = COMMON_REF._temp_idx
    try:
        base = tmp_path / "scripts"
        base.mkdir(parents=True, exist_ok=True)
        COMMON_REF._temp_folder = base
        for index, ext in ((0, ".py"), (1, ".py"), (42, ".py"), (7, ".txt"), (999999, ".py")):
            COMMON_REF._temp_idx = index
            ref_path = COMMON_REF._create_script_file("payload", ext)
            assert Path(ref_path).name == f"{index}{ext}"
            # The kernel performs the plain join; pathlib's resolve() only adds
            # the absolute/case normalisation of the base it was given.
            expected = str(base / f"{index}{ext}")
            assert os.path.normcase(ref_path) == os.path.normcase(expected)
            assert PY.plan_script_path(str(base), index, ext) == expected
    finally:
        COMMON_REF._temp_folder = saved_folder
        COMMON_REF._temp_idx = saved_idx


def test_plan_script_path_nested_base(tmp_path: Path) -> None:
    base = tmp_path / "a" / "b"
    base.mkdir(parents=True)
    assert PY.plan_script_path(str(base), 3, ".py") == str(base / "3.py")


# ---------------------------------------------------------------------------
# 2. interpreter resolution (py/__init__.py _resolve_python_uncached)
# ---------------------------------------------------------------------------

_RESOLVE_CASES = [
    # override, session_dir, cwd, virtual_env, fallback, existing, expected
    (
        r"C:\custom\python.exe",
        r"C:\proj\sub",
        r"C:\work",
        r"V:\env",
        r"C:\sys\python.exe",
        {r"C:\custom\python.exe", r"C:\proj\.venv\Scripts\python.exe"},
        r"C:\custom\python.exe",
    ),
    (
        r"C:\gone\python.exe",
        r"C:\proj\sub\deep",
        r"C:\work",
        "",
        r"C:\sys\python.exe",
        {r"C:\proj\.venv\Scripts\python.exe"},
        r"C:\proj\.venv\Scripts\python.exe",
    ),
    (
        "",
        r"C:\proj\sub\deep",
        r"C:\work",
        "",
        r"C:\sys\python.exe",
        {r"C:\proj\sub\deep\.venv\bin\python"},
        r"C:\proj\sub\deep\.venv\bin\python",
    ),
    (
        "",
        r"C:\proj",
        r"C:\work\pkg",
        "",
        r"C:\sys\python.exe",
        {r"C:\work\.venv\Scripts\python.exe"},
        r"C:\work\.venv\Scripts\python.exe",
    ),
    (
        "",
        r"C:\proj",
        r"C:\work",
        r"V:\env",
        r"C:\sys\python.exe",
        {r"V:\env\bin\python"},
        r"V:\env\bin\python",
    ),
    (
        "",
        r"C:\proj",
        r"C:\work",
        r"V:\env",
        r"C:\sys\python.exe",
        set(),
        r"C:\sys\python.exe",
    ),
    (
        "",
        r"C:\A",
        r"C:\B",
        "",
        r"C:\sys\python.exe",
        {r"C:\A\.venv\Scripts\python.exe", r"C:\B\.venv\Scripts\python.exe"},
        r"C:\A\.venv\Scripts\python.exe",
    ),
    (
        "",
        r"C:\proj",
        r"C:\work",
        r"V:\env",
        r"C:\sys\python.exe",
        {r"C:\proj\.venv\bin\python", r"V:\env\Scripts\python.exe"},
        r"C:\proj\.venv\bin\python",
    ),
]


@pytest.mark.parametrize(
    "override,session_dir,cwd,virtual_env,fallback,existing,expected", _RESOLVE_CASES
)
def test_resolve_python_exe_matches_reference(
    override, session_dir, cwd, virtual_env, fallback, existing, expected
) -> None:
    tool = PY_REF.python(session=_SessionStub(session_dir))
    with fake_filesystem({str(Path(p)) for p in existing}, cwd=str(Path(cwd))), environ(
        KIMIX_PYTHON_EXECUTABLE=override or None, VIRTUAL_ENV=virtual_env or None
    ):
        got = tool._resolve_python_uncached()
    # The reference falls back to sys.executable, which is the kernel's
    # injected `fallback` argument; when no candidate matched, the kernel takes
    # that fallback.
    if str(got) == sys.executable and str(got) not in existing:
        got = fallback
    cpp = PY.resolve_python_exe(
        override, [session_dir, cwd], virtual_env, fallback, sorted(existing)
    )
    assert cpp == (got or None)


def test_resolve_python_exe_fuzz_fake_filesystem() -> None:
    rng = random.Random(4242)
    tool = PY_REF.python(session=_SessionStub(r"C:\proj\sub"))
    parts = ["C:\\proj", "C:\\proj\\sub", "C:\\work", r"V:\env"]
    exts = [".venv\\Scripts\\python.exe", ".venv\\bin\\python", "Scripts\\python.exe"]
    override_exe = r"C:\ovr\python.exe"
    for _ in range(40):
        existing = set()
        for _ in range(rng.randint(0, 3)):
            existing.add(os.path.join(rng.choice(parts), rng.choice(exts)))
        override = rng.choice(["", override_exe])
        venv = rng.choice(["", r"V:\env"])
        with fake_filesystem(existing, cwd=r"C:\work"), environ(
            KIMIX_PYTHON_EXECUTABLE=override or None, VIRTUAL_ENV=venv or None
        ):
            got = tool._resolve_python_uncached()
        # The reference falls back to sys.executable, which is the kernel's
        # injected `fallback` argument (override_exe here).
        if str(got) == sys.executable and str(got) not in existing:
            got = override_exe
        assert PY.resolve_python_exe(
            override, [r"C:\proj\sub", r"C:\work"], venv, override_exe, sorted(existing)
        ) == (got or None)


# ---------------------------------------------------------------------------
# 3. child environment scrubbing (security.py scrub_child_env)
# ---------------------------------------------------------------------------

_SCRUB_ENVS = [
    {"PATH": "/bin", "HOME": "/h", "AWS_SECRET_ACCESS_KEY": "x", "DATABASE_URL": "d"},
    {"MY_TOKEN": "x", "KIMIX_TOKEN": "x", "PYTHONPATH": "/p", "GIT_TOKEN": "x"},
    {"ssh_auth_sock": "/s", "lowercase_key": "x", "MONKEY": "x", "TURKEY": "x"},
    {"": "empty", "WITH SPACE": "x", "KEYISH": "x", "AUTHORIZATION": "b"},
    {"PIP_INDEX_URL": "u", "UV_CACHE_DIR": "u", "PATH_TOKEN": "x", "SSH_PASSWORD": "x"},
    {},
]


@pytest.mark.parametrize("env", _SCRUB_ENVS)
def test_scrub_child_env_matches_reference(env) -> None:
    with pure_python(SECURITY_REF, COMMON_REF, PY_REF):
        expected = SECURITY_REF.scrub_child_env(dict(env))
    got = PY.scrub_child_env(dict(env))
    assert got == expected
    assert list(got) == list(expected)  # insertion order is part of the contract


def test_scrub_child_env_fuzz() -> None:
    rng = random.Random(7)
    names = [
        "PATH", "HOME", "PYTHONPATH", "VIRTUAL_ENV", "AUTH_TOKEN", "SECRET_PASSWORD",
        "CREDENTIALS", "APIKEY", "BEARER_X", "WEBHOOK_URL", "PG_DSN", "UV_CACHE",
        "KIMIX_HOME", "os", "COMPUTERNAME", "userprofile", "mixedCaseKEY",
    ]
    with pure_python(SECURITY_REF, COMMON_REF, PY_REF):
        for _ in range(60):
            env = {rng.choice(names): rng.choice(["", "v", "/x", "a b"])
                   for _ in range(rng.randint(0, 6))}
            got = PY.scrub_child_env(dict(env))
            expected = SECURITY_REF.scrub_child_env(dict(env))
            assert got == expected
            assert list(got) == list(expected)


# ---------------------------------------------------------------------------
# 4. child environment assembly (py/__init__.py _build_env)
# ---------------------------------------------------------------------------

SHARE_BIN = r"C:\share\bin"


def _assert_env_case(
    python_exe: str, current_path: str, parent_venv: str, pyvenv_cfg: bool
) -> None:
    """Compare the reference _build_env with the C++ env delta.

    ``pyvenv_cfg`` decides whether ``<exe parent>/../pyvenv.cfg`` exists in the
    injected fake filesystem (the venv test the reference performs).
    """
    venv_cfg = str(Path(python_exe).parent.parent / "pyvenv.cfg")
    existing = [venv_cfg] if pyvenv_cfg else []
    cpp = PY.prepare_python_env(
        python_exe, SHARE_BIN, current_path, os.pathsep, existing
    )
    # Reference: _build_env returns the complete env snapshot (or None).
    orig_share = PY_REF.get_share_dir
    try:
        PY_REF.get_share_dir = lambda: Path(r"C:\share")
        with fake_filesystem(set(existing)), environ(
            PATH=current_path, VIRTUAL_ENV=(parent_venv or None)
        ):
            env = PY_REF.python._build_env(python_exe, scrub_env=False)
    finally:
        PY_REF.get_share_dir = orig_share

    if env is None:
        assert cpp is None, f"reference took the zero-copy path: {python_exe!r}"
        return
    assert cpp is not None, f"reference built an env: {python_exe!r}"
    assert cpp["PATH"] == env["PATH"]
    expected_venv = env.get("VIRTUAL_ENV", "")
    if expected_venv == (parent_venv or ""):
        assert "VIRTUAL_ENV" not in cpp  # the delta must not touch it
    else:
        assert cpp["VIRTUAL_ENV"] == expected_venv


_ENV_CASES = [
    # python_exe, PATH, parent VIRTUAL_ENV, pyvenv.cfg exists
    (r"C:\sys\python.exe", r"C:\a;C:\b", "", False),
    (r"C:\sys\python.exe", SHARE_BIN + r";C:\a", "", False),
    (r"C:\sys\python.exe", SHARE_BIN, "", False),
    (r"C:\sys\python.exe", r"C:\a;" + SHARE_BIN + r";C:\b", "", False),
    (r"C:\sys\python.exe", "", "", False),
    (r"C:\sys\python.exe", r"C:\a;;" + SHARE_BIN + r";;C:\b;", "", False),
    (r"C:\proj\.venv\Scripts\python.exe", r"C:\a;C:\b", "", True),
    (r"C:\proj\.venv\bin\python", r"C:\a", "", True),
    (r"C:\proj\.venv\Scripts\python.exe", SHARE_BIN + r";C:\a", "", True),
    (r"C:\proj\weird\Scripts\python.exe", r"C:\a", "", False),
    (r"C:\sys\python.exe", r"C:\a", r"C:\parent\venv", False),
    (r"C:\proj\.venv\Scripts\python.exe", SHARE_BIN + r";C:\a;" + SHARE_BIN, "", True),
    (r"C:\proj\.venv\bin\python", r"C:\a;C:\b", r"C:\other", True),
]


@pytest.mark.parametrize("python_exe,current_path,parent_venv,pyvenv_cfg", _ENV_CASES)
def test_prepare_python_env_matches_reference(
    python_exe, current_path, parent_venv, pyvenv_cfg
) -> None:
    _assert_env_case(python_exe, current_path, parent_venv, pyvenv_cfg)


def test_prepare_python_env_fuzz() -> None:
    rng = random.Random(99)
    for _ in range(40):
        entries = [
            rng.choice(["C:\\a", "C:\\b", SHARE_BIN, "", "C:\\share"])
            for _ in range(rng.randint(0, 5))
        ]
        current_path = os.pathsep.join(entries)
        python_exe, default_venv = rng.choice(
            [
                (r"C:\sys\python.exe", False),
                (r"C:\proj\.venv\Scripts\python.exe", True),
                (r"C:\proj\.venv\bin\python", True),
                (r"C:\proj\other\python.exe", False),
            ]
        )
        pyvenv_cfg = default_venv and rng.choice([True, False])
        parent_venv = rng.choice(["", r"C:\parent\venv"])
        _assert_env_case(python_exe, current_path, parent_venv, pyvenv_cfg)


# ---------------------------------------------------------------------------
# 5. module-not-found hint (py/__init__.py _module_not_found_hint)
# ---------------------------------------------------------------------------

_HINT_OUTPUTS = [
    "ModuleNotFoundError: No module named 'requests'\n",
    'ModuleNotFoundError: No module named "numpy.core"\n',
    "ModuleNotFoundError: No module named 'a.b_c.d1'",
    "ValueError: bad\n",
    "",
    "ModuleNotFoundError: No module named requests\n",
    "ModuleNotFoundError: No module named ",
    "ModuleNotFoundError: No module named 'requests\n",
    "ModuleNotFoundError: No module named ''\n",
    "ModuleNotFoundError: No module named ''\nModuleNotFoundError: No module named 'second'\n",
    "ModuleNotFoundError: No module named 'weird\"\n",
    "ModuleNotFoundError: No module named 'a\nb'\n",
    "modulenotfounderror: No module named 'x'\n",
    "XModuleNotFoundError: No module named 'x'\n",
]


@pytest.mark.parametrize("output", _HINT_OUTPUTS)
def test_module_not_found_hint_matches_reference(output) -> None:
    exe = r"C:\proj\.venv\Scripts\python.exe"
    assert PY.module_not_found_hint(output, exe) == PY_REF.python._module_not_found_hint(
        output, exe
    )


def test_module_not_found_hint_fuzz() -> None:
    rng = random.Random(31337)
    exe = "python"
    alphabet = list("abc '\"\n. _-\t()") + ["ModuleNotFoundError: No module named "]
    for _ in range(120):
        output = "".join(rng.choice(alphabet) for _ in range(rng.randint(0, 12)))
        assert PY.module_not_found_hint(
            output, exe
        ) == PY_REF.python._module_not_found_hint(output, exe)


# ---------------------------------------------------------------------------
# 6. session output block (common.py _build_session_output_block)
# ---------------------------------------------------------------------------

_BLOCK_CASES = [
    dict(task_id="python", status="completed", output="hello"),
    dict(task_id="python", status="completed", output=""),
    dict(
        task_id="task_7",
        status="completed",
        output="line1\nline2",
        exit_code=0,
        exit_code_meaning="success",
        failure_hint="hint",
        wait_matched=True,
        elapsed_seconds=1.25,
        output_path="/tmp/out.txt",
        output_truncated=True,
        original_path="/tmp/orig.txt",
    ),
    dict(task_id="t", status="running", output="x", exit_code_meaning="", failure_hint=""),
    dict(task_id="t", status="running", output="x", wait_matched=False),
    dict(task_id="t", status="completed", output="a\nb\n\n\n"),
    dict(task_id="t", status="completed", output="a\n\n   \n\t\nb"),
    dict(task_id="t", status="completed", output="a\r\nb\rc\x0bd\x0ce"),
    dict(task_id="t", status="completed", output="caf\u00e9 \u4f60\u597d"),
    dict(task_id="t", status="completed", output="  indented\nnot"),
    dict(task_id="t", status="completed", output="x", exit_code=-1, exit_code_meaning="killed"),
    dict(task_id="t", status="completed", output="x", elapsed_seconds=0.005),
    dict(task_id="t", status="completed", output="x", elapsed_seconds=2.675),
    dict(task_id="t", status="completed", output="x", elapsed_seconds=12345.678),
    dict(task_id="", status="", output="x"),
    dict(task_id="t", status="completed", output="\n\n"),
]


@pytest.mark.parametrize("kwargs", _BLOCK_CASES)
def test_build_session_output_block_matches_reference(kwargs) -> None:
    expected = COMMON_REF._build_session_output_block(**kwargs)
    assert PY.build_session_output_block(**kwargs) == expected


def test_build_session_output_block_fuzz() -> None:
    rng = random.Random(2718)
    for _ in range(60):
        kwargs = {
            "task_id": rng.choice(["", "python", "task_12"]),
            "status": rng.choice(["completed", "running", "timeout"]),
            "output": "".join(
                rng.choice(list("ab \t\n\r\x0b\x0c\u00e9"))
                for _ in range(rng.randint(0, 24))
            ),
            "exit_code": rng.choice([None, 0, 1, -1, 127]),
            "exit_code_meaning": rng.choice([None, "", "success"]),
            "failure_hint": rng.choice([None, "", "hint"]),
            "wait_matched": rng.choice([None, True, False]),
            "elapsed_seconds": rng.choice([None, 0.0, 1.005, 59.999, 3600.5]),
            "output_path": rng.choice([None, "", "/tmp/o"]),
            "output_truncated": rng.choice([True, False]),
            "original_path": rng.choice([None, "", "/tmp/g"]),
        }
        assert PY.build_session_output_block(**kwargs) == (
            COMMON_REF._build_session_output_block(**kwargs)
        )


# ---------------------------------------------------------------------------
# 7. export-path extraction (common.py _extract_export_path)
# ---------------------------------------------------------------------------

_EXPORT_INPUTS = [
    "exported to file `C:/t/0.txt`",
    "added to file `C:/t/1.txt`",
    "added to file: C:/t/2.txt",
    "output exported to: C:/t/3.txt",
    "[Output too large, exported to file: C:/t/4.txt]",
    "[Output too large, added to file: C:/t/5.txt]",
    "see added to file: C:/t/6.txt]",
    "exported to file `C:/t/7.txt`]`",
    "prefix exported to file `C:/t/8.txt` suffix",
    "exported to file `C:/t/9.txt",
    "exported to file C:/t/10.txt",
    "exported to file: ",
    "exported to file `",
    "added to file: ",
    "nothing here",
    "",
    "]exported to file: tail]",
]


@pytest.mark.parametrize("output", _EXPORT_INPUTS)
def test_extract_export_path_matches_reference(output) -> None:
    assert PY.extract_export_path(output) == COMMON_REF._extract_export_path(output)


def test_extract_export_path_fuzz() -> None:
    rng = random.Random(1234)
    fragments = [
        "exported to file `",
        "added to file `",
        "exported to file: ",
        "added to file: ",
        "exported to file ",
        "added to file ",
        "C:/t/x.txt",
        "]",
        "`",
        "",
        "text ",
    ]
    for _ in range(150):
        output = "".join(rng.choice(fragments) for _ in range(rng.randint(0, 4)))
        assert PY.extract_export_path(output) == COMMON_REF._extract_export_path(output)


# ---------------------------------------------------------------------------
# 8. wait_for_pattern (background/utils.py pattern.search -> `regex`)
# ---------------------------------------------------------------------------

_PATTERN_CASES = [
    ("ready", "all ready to go"),
    ("ready", "nope"),
    ("hello world", "say hello world now"),
    ("a]b", "x a]b y"),
    ("a}b", "x a}b y"),
    ("a{}b", "x a{}b y"),
    ("", "buffer"),
    ("a.b", "axb"),
    ("^ready", "ready now"),
    ("^-multiline", "x\nready"),
    ("now$", "ready now"),
    ("ab+c", "abbbc"),
    ("a{2}", "aa"),
    (r"\d+", "x42"),
    ("cat|dog", "a dog"),
    ("(ab)+", "abab"),
    # glob metacharacters are REGEX in the reference: "ready*" matches
    # "read done" (the '*' quantifies the preceding 'y'), which fnmatch cannot.
    ("ready*", "read done"),
    ("ready*", "ready now"),
    ("*done*", "step done here"),
    ("ab?c", "ac"),
    ("err: [0-9]", "err: 7"),
    ("a[!b]c", "axc"),
    ("a[^b]c", "axc"),
    ("a[b-d]e", "ace"),
    ("a[", "a["),
    ("[]", "x"),
    ("(unclosed", "x"),
    ("abc\\", "abc\\"),
    ("caf\u00e9", "un caf\u00e9 ici"),
    ("a\nb", "x a\nb y"),
]


@pytest.mark.parametrize("pattern,buffer", _PATTERN_CASES)
def test_classify_and_match_wait_pattern_matches_reference(pattern, buffer) -> None:
    import regex as re

    if pattern == "":
        # Documented deviation: the reference compiles "" (it matches every
        # buffer); the kernel refuses the degenerate input.
        assert PY.match_wait_pattern(pattern, buffer) == ("invalid_input", False)
        return
    if needs_regex_engine(pattern):
        # The reference compiles a full regex; the native kernel must refuse it
        # rather than answer with a different (literal/fnmatch) semantics.
        assert PY.classify_wait_pattern(pattern) == "unsupported"
        assert PY.match_wait_pattern(pattern, buffer) == ("unsupported", False)
    else:
        assert PY.classify_wait_pattern(pattern) == "literal"
        status, matched = PY.match_wait_pattern(pattern, buffer)
        assert status == "ok"
        assert matched == bool(re.search(pattern, buffer))


def test_glob_metacharacter_is_not_native() -> None:
    """Regression pin: fnmatch semantics disagree with the reference regex.

    The reference compiles ``ready*`` as a regex, so it matches ``read done``
    (``y*`` is a quantifier); a fnmatch engine requires the literal ``ready``
    and answers false.  The kernel used to take the fnmatch path and silently
    returned the wrong answer; it now refuses the pattern.
    """
    import regex as re

    assert bool(re.search("ready*", "read done")) is True
    assert ("ready*" in "read done") is False
    assert PY.classify_wait_pattern("ready*") == "unsupported"
    assert PY.match_wait_pattern("ready*", "read done") == ("unsupported", False)
    # ... and a pattern without metacharacters stays native and exact.
    assert PY.classify_wait_pattern("ready") == "literal"
    assert PY.match_wait_pattern("ready", "read done") == ("ok", False)
    assert PY.match_wait_pattern("read", "read done") == ("ok", True)


def test_wait_pattern_fuzz_matches_reference() -> None:
    import regex as re

    rng = random.Random(20240924)
    alphabet = list("ab.*?+{}()[]^$|\\ 01") + ["\n"]
    for _ in range(300):
        pattern = "".join(rng.choice(alphabet) for _ in range(rng.randint(1, 6)))
        buffer = "".join(rng.choice(list("abc012 .[]*")) for _ in range(rng.randint(0, 12)))
        status, matched = PY.match_wait_pattern(pattern, buffer)
        if needs_regex_engine(pattern):
            assert status == "unsupported"
            assert matched is False
            continue
        assert status == "ok"
        assert matched == bool(re.search(pattern, buffer)), (pattern, buffer)


# ---------------------------------------------------------------------------
# 9. keep the C++ goldens in sync with the reference
# ---------------------------------------------------------------------------


def test_goldens_inc_is_current() -> None:
    """python_goldens.inc (consumed by test_builtin_python) is up to date."""
    generator = _load_generator()
    if generator is None:  # pragma: no cover - defensive
        pytest.skip("scripts/gen_python_goldens.py not found")
    out_path = generator.OUT_PATH
    if not out_path.is_file():  # pragma: no cover - defensive
        pytest.skip(f"{out_path} has not been generated yet")
    assert generator.render() == out_path.read_text(encoding="utf-8"), (
        "python_goldens.inc is stale: rerun scripts/gen_python_goldens.py"
    )
