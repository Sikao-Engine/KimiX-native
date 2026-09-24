#!/usr/bin/env python3
"""Regenerate tests/unit/builtin_tools/python_goldens.inc.

Every vector in that file is produced by running the *real* Python
implementation of the ``python`` built-in tool and the shared helpers it uses,
imported straight from the kimi-agent checkout (never from
``kimix-base/python/kimix_native``, whose ``_compat`` mirrors were written
alongside the port and would mask a shared mistake):

  src/kimix/tools/py/__init__.py     _resolve_python_uncached / _build_env /
                                     _module_not_found_hint
  src/kimix/tools/common.py          _create_script_file /
                                     _extract_export_path /
                                     _build_session_output_block
  src/kimix/tools/security.py        scrub_child_env
  src/kimix/tools/background/utils.py wait_for_output pattern step
                                     (``pattern.search(output)`` over a
                                     ``regex``-compiled wait_for_pattern)

The C++ port (src/builtin_tools/python_tool.cpp) is replayed over the same
inputs by tests/unit/builtin_tools/test_python_tool.cpp and must reproduce
every byte.

Two helper tricks keep the vectors deterministic and machine-independent:

* ``pathlib.Path.is_file`` is replaced by an allow-list lookup, so the
  interpreter-resolution and env-building kernels are driven by an injected
  fake filesystem instead of the host's real one (the C++ kernels take the same
  probe as an injected ``kimix::function<bool(kimix::string_view)>``).
* script-path vectors are derived from ``_create_script_file`` but recorded as
  the *un-resolved* ``<base><sep><index><ext>`` join the C++ kernel computes
  (``Path.resolve()`` only normalises the existing directory's case on
  Windows); the generator asserts that the two agree case-insensitively, so a
  real divergence still fails generation.

Usage::

    python scripts/gen_python_goldens.py            # rewrite the .inc file
    python scripts/gen_python_goldens.py --check    # fail if it is out of date

``--python`` re-executes this script with another interpreter (the kimi-agent
checkout needs ``kimi_cli`` / ``kimi_agent_sdk`` importable).
"""

from __future__ import annotations

import argparse
import os
import random
import subprocess
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
KIMI_AGENT_ROOT = Path(os.environ.get("KIMI_AGENT_ROOT", r"C:/dev/kimi-agent"))
DEFAULT_AGENT_PYTHON = KIMI_AGENT_ROOT / ".venv" / "Scripts" / "python.exe"
OUT_PATH = PROJECT_ROOT / "tests" / "unit" / "builtin_tools" / "python_goldens.inc"

# Fixed scratch directory used for the script-path vectors (the generator
# creates it; the recorded paths are plain string arithmetic and do not depend
# on the machine that generated them).
SCRIPT_DIR = Path("C:/Temp/kimix_py_goldens/tmp")


# ---------------------------------------------------------------------------
# C string literal escaping
# ---------------------------------------------------------------------------


def c_lit(text: str) -> str:
    """Encode *text* as a C string literal (byte-exact, ASCII-source safe)."""
    raw = text.encode("utf-8")
    out: list[str] = ['"']
    prev_hex_escape = False
    for b in raw:
        ch = chr(b)
        escape: str | None = None
        if ch == "\\":
            escape = "\\\\"
        elif ch == '"':
            escape = '\\"'
        elif ch == "\n":
            escape = "\\n"
        elif ch == "\r":
            escape = "\\r"
        elif ch == "\t":
            escape = "\\t"
        elif 0x20 <= b < 0x7F:
            escape = None
        else:
            escape = "\\x%02X" % b
        if escape is None:
            if prev_hex_escape and ch in "0123456789abcdefABCDEF":
                out.append('""')  # split: "\x0B" "C" instead of "\x0BC"
            out.append(ch)
            prev_hex_escape = False
        else:
            out.append(escape)
            # A hex escape swallows following hex digits, so remember it.
            prev_hex_escape = escape.startswith("\\x")
    out.append('"')
    return "".join(out)


def c_rows(rows: list[str]) -> str:
    return "".join("    {" + row + "},\n" for row in rows)


# ---------------------------------------------------------------------------
# Reference import (kimi-agent checkout only)
# ---------------------------------------------------------------------------


def _setup_reference():
    """Import the kimi-agent reference modules with explicit provenance.

    ``<kimi-agent>/bin`` must come first so ``kimix_native`` resolves there
    (the kimix-base mirror under ``python/`` is deliberately not importable
    here), and ``<kimi-agent>/src`` provides ``kimix.tools.*``.  The
    ``kimi-cli/src`` shim package is never added to ``sys.path``.
    """
    for p in (KIMI_AGENT_ROOT / "bin", KIMI_AGENT_ROOT / "src"):
        s = str(p)
        if not p.is_dir():
            raise SystemExit(f"reference path missing: {s}")
        while s in sys.path:
            sys.path.remove(s)
    sys.path.insert(0, str(KIMI_AGENT_ROOT / "src"))
    sys.path.insert(0, str(KIMI_AGENT_ROOT / "bin"))

    import kimix.tools.common as common  # noqa: PLC0415
    import kimix.tools.py as py_mod  # noqa: PLC0415
    import kimix.tools.security as security  # noqa: PLC0415

    for mod in (common, py_mod, security):
        path = Path(mod.__file__).resolve()
        if KIMI_AGENT_ROOT.resolve() not in path.parents:
            raise SystemExit(f"reference module resolved outside the checkout: {path}")
    return common, py_mod, security


def _pure_python(*modules) -> list[tuple[object, str, object]]:
    """Disable the ``kimix_native`` short-circuits of *modules* (in place).

    Returns the saved (module, attribute, value) triples for restoration.
    """
    saved: list[tuple[object, str, object]] = []
    for mod in modules:
        for attr in ("_native_use_native", "_use_native"):
            if hasattr(mod, attr):
                saved.append((mod, attr, getattr(mod, attr)))
                setattr(mod, attr, lambda *_a, **_k: False)
        for attr in ("_NATIVE_TOOLS", "_NATIVE_STREAM", "_NATIVE_GLOB", "_NATIVE"):
            if hasattr(mod, attr):
                saved.append((mod, attr, getattr(mod, attr)))
                setattr(mod, attr, None)
    return saved


def _restore(saved: list[tuple[object, str, object]]) -> None:
    for mod, attr, value in reversed(saved):
        setattr(mod, attr, value)


# ---------------------------------------------------------------------------
# 1. script path planning (common.py _create_script_file)
# ---------------------------------------------------------------------------


def gen_paths(common) -> list[str]:
    SCRIPT_DIR.mkdir(parents=True, exist_ok=True)
    saved_folder = common._temp_folder
    saved_idx = common._temp_idx
    rows: list[str] = []
    cases: list[tuple[str, Path, int, str]] = [
        ("first_script", SCRIPT_DIR, 0, ".py"),
        ("second_script", SCRIPT_DIR, 1, ".py"),
        ("index_42", SCRIPT_DIR, 42, ".py"),
        ("txt_extension", SCRIPT_DIR, 7, ".txt"),
        ("nested_base", SCRIPT_DIR / "sub" / "deep", 3, ".py"),
        ("high_index", SCRIPT_DIR, 999999, ".py"),
    ]
    try:
        for name, base, start, ext in cases:
            base.mkdir(parents=True, exist_ok=True)
            common._temp_folder = base
            common._temp_idx = start
            ref = common._create_script_file("payload", ext)
            expected = str(base / (str(start) + ext))
            if os.path.normcase(ref) != os.path.normcase(expected):
                raise SystemExit(
                    f"script path vector {name!r} disagrees with the kernel "
                    f"arithmetic: reference={ref!r} join={expected!r}"
                )
            rows.append(
                ", ".join(
                    [
                        c_lit(name),
                        c_lit(str(base)),
                        str(start),
                        c_lit(ext),
                        c_lit(expected),
                    ]
                )
            )
    finally:
        common._temp_folder = saved_folder
        common._temp_idx = saved_idx
    return rows


# ---------------------------------------------------------------------------
# 2. interpreter resolution (py/__init__.py _resolve_python_uncached)
# ---------------------------------------------------------------------------


class _SessionStub:
    def __init__(self, directory: str) -> None:
        self.dir = directory


def _fake_is_file(allowed: set[str]):
    def _is_file(self: Path) -> bool:  # noqa: ANN001
        return str(self) in allowed

    return _is_file


def gen_resolve(py_mod) -> list[str]:
    rows: list[str] = []
    tool = py_mod.python(session=_SessionStub(r"C:\proj\sub\deep"))
    orig_is_file = Path.is_file
    orig_cwd = Path.cwd
    saved_env = {k: os.environ.get(k) for k in ("KIMIX_PYTHON_EXECUTABLE", "VIRTUAL_ENV")}
    try:
        Path.cwd = classmethod(lambda cls: Path(r"C:\work"))
        cases: list[tuple[str, str, str, str, str, str, set[str]]] = [
            # name, override, session_dir, cwd, virtual_env, fallback, existing
            (
                "override_beats_everything",
                r"C:\custom\python.exe",
                r"C:\proj\sub\deep",
                r"C:\work",
                r"V:\env",
                r"C:\sys\python.exe",
                {
                    r"C:\custom\python.exe",
                    r"C:\proj\sub\deep\.venv\Scripts\python.exe",
                    r"V:\env\Scripts\python.exe",
                },
            ),
            (
                "override_missing_is_skipped",
                r"C:\gone\python.exe",
                r"C:\proj\sub\deep",
                r"C:\work",
                "",
                r"C:\sys\python.exe",
                {r"C:\proj\.venv\Scripts\python.exe"},
            ),
            (
                "venv_at_session_dir",
                "",
                r"C:\proj\sub\deep",
                r"C:\work",
                "",
                r"C:\sys\python.exe",
                {r"C:\proj\sub\deep\.venv\Scripts\python.exe"},
            ),
            (
                "venv_two_levels_up",
                "",
                r"C:\proj\sub\deep",
                r"C:\work",
                "",
                r"C:\sys\python.exe",
                {r"C:\proj\.venv\Scripts\python.exe"},
            ),
            (
                "venv_posix_candidate_at_root",
                "",
                r"C:\proj\sub\deep",
                r"C:\work",
                "",
                r"C:\sys\python.exe",
                {r"C:\.venv\bin\python"},
            ),
            (
                "venv_from_cwd_walk",
                "",
                r"C:\proj",
                r"C:\work\pkg",
                "",
                r"C:\sys\python.exe",
                {r"C:\work\.venv\bin\python"},
            ),
            (
                "scripts_candidate_beats_bin_candidate",
                "",
                r"C:\proj",
                r"C:\work",
                "",
                r"C:\sys\python.exe",
                {r"C:\proj\.venv\Scripts\python.exe", r"C:\proj\.venv\bin\python"},
            ),
            (
                "virtual_env_scripts",
                "",
                r"C:\proj",
                r"C:\work",
                r"V:\env",
                r"C:\sys\python.exe",
                {r"V:\env\Scripts\python.exe"},
            ),
            (
                "virtual_env_bin",
                "",
                r"C:\proj",
                r"C:\work",
                r"V:\env",
                r"C:\sys\python.exe",
                {r"V:\env\bin\python"},
            ),
            (
                "fallback_when_nothing_exists",
                "",
                r"C:\proj",
                r"C:\work",
                r"V:\env",
                r"C:\sys\python.exe",
                set(),
            ),
            (
                "no_fallback_is_none",
                "",
                r"C:\proj",
                r"C:\work",
                r"V:\env",
                "",
                set(),
            ),
            (
                "session_dir_base_wins_over_cwd_base",
                "",
                r"C:\A",
                r"C:\B",
                "",
                r"C:\sys\python.exe",
                {r"C:\A\.venv\Scripts\python.exe", r"C:\B\.venv\Scripts\python.exe"},
            ),
            (
                "nearest_parent_wins_over_scripts_vs_bin",
                "",
                r"C:\proj\sub",
                r"C:\work",
                "",
                r"C:\sys\python.exe",
                {r"C:\proj\.venv\bin\python", r"C:\proj\sub\.venv\Scripts\python.exe"},
            ),
            (
                "venv_walk_beats_virtual_env",
                "",
                r"C:\proj",
                r"C:\work",
                r"V:\env",
                r"C:\sys\python.exe",
                {r"C:\proj\.venv\Scripts\python.exe", r"V:\env\Scripts\python.exe"},
            ),
            (
                "virtual_env_beats_fallback",
                "",
                r"C:\proj",
                r"C:\work",
                r"V:\env",
                r"C:\sys\python.exe",
                {r"V:\env\Scripts\python.exe"},
            ),
        ]
        for name, override, session_dir, cwd, venv, fallback, existing in cases:
            allowed = {str(Path(p)) for p in existing}
            Path.is_file = _fake_is_file(allowed)
            Path.cwd = classmethod(lambda cls, c=cwd: Path(c))
            if override:
                os.environ["KIMIX_PYTHON_EXECUTABLE"] = override
            else:
                os.environ.pop("KIMIX_PYTHON_EXECUTABLE", None)
            if venv:
                os.environ["VIRTUAL_ENV"] = venv
            else:
                os.environ.pop("VIRTUAL_ENV", None)
            tool._session = _SessionStub(str(Path(session_dir)))
            tool._resolved_python = None
            got = tool._resolve_python_uncached()
            # The reference always falls back to ``sys.executable``, which the
            # C++ kernel receives as an injected parameter.  A result equal to
            # sys.executable therefore means "no candidate matched": the golden
            # records the kernel's fallback argument ("" == the kernel's
            # std::nullopt, its documented stand-in for "no interpreter").
            norm = {os.path.normcase(p) for p in allowed}
            if got is not None and os.path.normcase(str(got)) == os.path.normcase(
                sys.executable
            ) and os.path.normcase(str(got)) not in norm:
                expected = fallback
            else:
                expected = "" if got is None else str(got)
            rows.append(
                ", ".join(
                    [
                        c_lit(name),
                        c_lit(override),
                        c_lit(str(Path(session_dir))),
                        c_lit(str(Path(cwd))),
                        c_lit(venv),
                        c_lit(fallback),
                        c_lit("\n".join(sorted(allowed))),
                        c_lit(expected),
                    ]
                )
            )
    finally:
        Path.is_file = orig_is_file
        Path.cwd = orig_cwd
        for k, v in saved_env.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
    return rows


# ---------------------------------------------------------------------------
# 3. environment scrubbing (security.py scrub_child_env)
# ---------------------------------------------------------------------------


def gen_scrub(security) -> list[str]:
    rows: list[str] = []
    cases: list[tuple[str, list[tuple[str, str]]]] = [
        (
            "safe_prefixes_kept",
            [
                ("PATH", r"C:\bin"),
                ("HOME", r"C:\Users\x"),
                ("USER", "x"),
                ("LANG", "en_US.UTF-8"),
                ("LC_ALL", "C"),
                ("TERM", "xterm"),
                ("TMP", r"C:\tmp"),
                ("TEMP", r"C:\tmp"),
                ("SHELL", "/bin/bash"),
                ("LOGNAME", "x"),
                ("XDG_CONFIG_HOME", "/x"),
                ("PYTHONPATH", "/p"),
                ("VIRTUAL_ENV", "/v"),
                ("CONDA_PREFIX", "/c"),
                ("KIMIX_HOME", "/k"),
                ("PROCESSOR_ARCHITECTURE", "AMD64"),
                ("PROGRAMFILES", r"C:\Program Files"),
                ("APPDATA", r"C:\a"),
                ("LOCALAPPDATA", r"C:\l"),
                ("HOMEDRIVE", "C:"),
                ("HOMEPATH", "\\Users\\x"),
                ("SYSTEMROOT", r"C:\Windows"),
                ("WINDIR", r"C:\Windows"),
                ("COMSPEC", r"C:\Windows\system32\cmd.exe"),
                ("PATHEXT", ".COM;.EXE"),
                ("NUMBER_OF_PROCESSORS", "16"),
                ("OS", "Windows_NT"),
                ("COMPUTERNAME", "PC"),
                ("USERPROFILE", r"C:\Users\x"),
                ("TZ", "UTC"),
                ("PWD", "/pwd"),
                ("SHLVL", "1"),
                ("SSH_AUTH_SOCK", "/s"),
                ("GIT_SSH_COMMAND", "ssh"),
                ("UV_CACHE_DIR", "/u"),
                ("PIP_INDEX_URL", "https://x"),
            ],
        ),
        (
            "secret_names_dropped",
            [
                ("AWS_SECRET_ACCESS_KEY", "x"),
                ("AWS_ACCESS_KEY_ID", "x"),
                ("MY_TOKEN", "x"),
                ("GITHUB_TOKEN", "x"),
                ("DATABASE_PASSWORD", "x"),
                ("DB_PASSWD", "x"),
                ("GOOGLE_APPLICATION_CREDENTIALS", "/x.json"),
                ("AUTHORIZATION", "Bearer x"),
                ("PG_DSN", "postgres://x"),
                ("SLACK_WEBHOOK", "https://x"),
                ("MY_CREDS", "x"),
                ("BEARER_TOKEN", "x"),
                ("SOME_APIKEY", "x"),
                ("aws_secret_access_key", "x"),
                ("token", "x"),
                ("ApiKey", "x"),
                ("SECRET", "x"),
            ],
        ),
        (
            "no_secret_name_kept",
            [
                ("DATABASE_URL", "postgres://u:p@h/db"),
                ("EDITOR", "vim"),
                ("PAGER", "less"),
                ("FOO", "bar"),
                ("", "empty-name"),
                ("WITH_SPACE", "a b"),
                ("KEYISH", "x"),  # contains KEY -> dropped
                ("MONKEY", "x"),  # contains KEY -> dropped
                ("HOCKEY", "x"),  # contains KEY -> dropped
                ("TURKEY", "x"),  # contains KEY -> dropped
            ],
        ),
        (
            "safe_prefix_wins_over_secret_substring",
            [
                ("PATH_TOKEN", "x"),
                ("KIMIX_SECRET", "x"),
                ("PYTHON_API_KEY", "x"),
                ("GIT_CREDENTIALS", "x"),
                ("SSH_PASSWORD", "x"),
                ("UV_TOKEN", "x"),
                ("PIP_TOKEN", "x"),
                ("PYTHON_KEY", "x"),
                ("VIRTUAL_ENV_NAME", "x"),
            ],
        ),
        (
            "order_preserved",
            [
                ("zzz", "1"),
                ("TOKEN", "2"),
                ("aaa", "3"),
                ("SECRET", "4"),
                ("mmm", "5"),
                ("PATH", "6"),
            ],
        ),
        ("empty_env", []),
        (
            "case_folding_is_ascii_upper",
            [
                ("path", "1"),
                ("Path", "2"),
                ("pAtH", "3"),
                ("kImIx_Home", "4"),
            ],
        ),
    ]
    for name, env in cases:
        ref = security.scrub_child_env(dict(env))
        rows.append(
            ", ".join(
                [
                    c_lit(name),
                    c_lit("\n".join(f"{k}\t{v}" for k, v in env)),
                    c_lit("\n".join(ref.keys())),
                ]
            )
        )
    return rows


# ---------------------------------------------------------------------------
# 4. child environment assembly (py/__init__.py _build_env)
# ---------------------------------------------------------------------------


def gen_env(py_mod) -> list[str]:
    rows: list[str] = []
    share = Path(r"C:\share")
    share_bin = r"C:\share\bin"
    saved_share = py_mod.get_share_dir
    orig_is_file = Path.is_file
    saved_path = os.environ.get("PATH")
    saved_venv = os.environ.get("VIRTUAL_ENV")
    try:
        py_mod.get_share_dir = lambda: share
        for key in ("VIRTUAL_ENV",):
            os.environ.pop(key, None)
        cases: list[tuple[str, str, str, str, bool]] = [
            # name, python_exe, current_path, parent_virtual_env, pyvenv_cfg
            ("nonvenv_prepends_share_bin", r"C:\sys\python.exe", r"C:\a;C:\b", "", False),
            (
                "nonvenv_share_bin_already_first",
                r"C:\sys\python.exe",
                r"C:\share\bin;C:\a",
                "",
                False,
            ),
            ("nonvenv_path_is_share_bin_only", r"C:\sys\python.exe", share_bin, "", False),
            (
                "nonvenv_share_bin_later_is_deduped",
                r"C:\sys\python.exe",
                r"C:\a;C:\share\bin;C:\b",
                "",
                False,
            ),
            ("nonvenv_empty_path", r"C:\sys\python.exe", "", "", False),
            (
                "nonvenv_empty_entries_removed",
                r"C:\sys\python.exe",
                r"C:\a;;C:\share\bin;;C:\b;",
                "",
                False,
            ),
            (
                "nonvenv_case_sensitive_entry_compare",
                r"C:\sys\python.exe",
                r"C:\A;C:\SHARE\BIN",
                "",
                False,
            ),
            (
                "venv_scripts_sets_virtual_env",
                r"C:\proj\.venv\Scripts\python.exe",
                r"C:\a;C:\b",
                "",
                True,
            ),
            (
                "venv_bin_posix_layout",
                r"C:\proj\.venv\bin\python",
                r"C:\a",
                "",
                True,
            ),
            (
                "venv_share_bin_already_first_still_rebuilt",
                r"C:\proj\.venv\Scripts\python.exe",
                r"C:\share\bin;C:\a",
                "",
                True,
            ),
            (
                "scripts_without_pyvenv_cfg_is_not_venv",
                r"C:\proj\weird\Scripts\python.exe",
                r"C:\a",
                "",
                False,
            ),
            (
                "nonvenv_keeps_parent_virtual_env",
                r"C:\sys\python.exe",
                r"C:\a",
                r"C:\parent\venv",
                False,
            ),
            (
                "venv_dedups_share_bin_from_path",
                r"C:\proj\.venv\Scripts\python.exe",
                r"C:\share\bin;C:\a;C:\share\bin;C:\b",
                "",
                True,
            ),
            (
                "trailing_separator_path",
                r"C:\sys\python.exe",
                r"C:\a;",
                "",
                False,
            ),
            (
                "nested_venv_dirname",
                r"C:\proj\.venv\Scripts\python.exe",
                "",
                "",
                True,
            ),
        ]
        for name, python_exe, current_path, parent_venv, pyvenv in cases:
            allowed: set[str] = set()
            if pyvenv:
                allowed.add(str(Path(python_exe).parent.parent / "pyvenv.cfg"))
            Path.is_file = _fake_is_file(allowed)
            os.environ["PATH"] = current_path
            if parent_venv:
                os.environ["VIRTUAL_ENV"] = parent_venv
            else:
                os.environ.pop("VIRTUAL_ENV", None)
            env = py_mod.python._build_env(python_exe, scrub_env=False)
            if env is None:
                rows.append(
                    ", ".join(
                        [
                            c_lit(name),
                            c_lit(python_exe),
                            c_lit(share_bin),
                            c_lit(current_path),
                            c_lit(parent_venv),
                            "true" if pyvenv else "false",
                            "true",  # expect_fast_path
                            "false",  # expect_virtual_env_change
                            c_lit(""),
                            c_lit(""),
                        ]
                    )
                )
                continue
            got_path = env.get("PATH", "")
            got_venv = env.get("VIRTUAL_ENV", "")
            expect_venv_change = got_venv != (parent_venv if parent_venv else "")
            rows.append(
                ", ".join(
                    [
                        c_lit(name),
                        c_lit(python_exe),
                        c_lit(share_bin),
                        c_lit(current_path),
                        c_lit(parent_venv),
                        "true" if pyvenv else "false",
                        "false",
                        "true" if expect_venv_change else "false",
                        c_lit(got_path),
                        c_lit(got_venv if expect_venv_change else ""),
                    ]
                )
            )
    finally:
        py_mod.get_share_dir = saved_share
        Path.is_file = orig_is_file
        if saved_path is None:
            os.environ.pop("PATH", None)
        else:
            os.environ["PATH"] = saved_path
        if saved_venv is None:
            os.environ.pop("VIRTUAL_ENV", None)
        else:
            os.environ["VIRTUAL_ENV"] = saved_venv
    return rows


# ---------------------------------------------------------------------------
# 5. module-not-found hint (py/__init__.py _module_not_found_hint)
# ---------------------------------------------------------------------------


def gen_hint(py_mod) -> list[str]:
    exe = r"C:\proj\.venv\Scripts\python.exe"
    outputs: list[tuple[str, str]] = [
        (
            "single_quotes",
            "Traceback (most recent call last):\n"
            '  File "x.py", line 1, in <module>\n'
            "ModuleNotFoundError: No module named 'requests'\n",
        ),
        ("double_quotes", 'ModuleNotFoundError: No module named "numpy.core"\n'),
        ("dotted_and_underscored", "ModuleNotFoundError: No module named 'a.b_c.d1'"),
        ("no_match_other_error", "ValueError: bad\n"),
        ("no_match_empty", ""),
        ("marker_without_quotes", "ModuleNotFoundError: No module named requests\n"),
        ("marker_at_end", "ModuleNotFoundError: No module named "),
        ("unterminated_single_quote", "ModuleNotFoundError: No module named 'requests\n"),
        ("empty_module_name", "ModuleNotFoundError: No module named ''\n"),
        (
            "first_empty_then_valid",
            "ModuleNotFoundError: No module named ''\n"
            "ModuleNotFoundError: No module named 'second'\n",
        ),
        ("mismatched_quotes", "ModuleNotFoundError: No module named 'weird\"\n"),
        ("newline_inside_name", "ModuleNotFoundError: No module named 'a\nb'\n"),
        ("space_inside_name", "ModuleNotFoundError: No module named 'my pkg'\n"),
        ("lowercase_error_name", "modulenotfounderror: No module named 'x'\n"),
        ("marker_inside_word", "XModuleNotFoundError: No module named 'x'\n"),
        ("two_valid_occurrences_uses_first", "ModuleNotFoundError: No module named 'one'\nModuleNotFoundError: No module named 'two'\n"),
        ("crlf_line_endings", "ModuleNotFoundError: No module named 'x'\r\n"),
        ("unicode_module_name", "ModuleNotFoundError: No module named 'paketü'\n"),
    ]
    rows: list[str] = []
    for name, output in outputs:
        hint = py_mod.python._module_not_found_hint(output, exe)
        rows.append(
            ", ".join([c_lit(name), c_lit(output), c_lit(exe), c_lit(hint)])
        )
    return rows


# ---------------------------------------------------------------------------
# 6. session output block (common.py _build_session_output_block)
# ---------------------------------------------------------------------------


def gen_block(common) -> list[str]:
    cases: list[dict] = [
        {
            "name": "minimal",
            "task_id": "python",
            "status": "completed",
            "output": "hello",
        },
        {
            "name": "empty_output",
            "task_id": "python",
            "status": "completed",
            "output": "",
        },
        {
            "name": "all_fields",
            "task_id": "task_7",
            "status": "completed",
            "output": "line1\nline2",
            "exit_code": 0,
            "exit_code_meaning": "success",
            "failure_hint": "hint",
            "wait_matched": True,
            "elapsed_seconds": 1.25,
            "output_path": "/tmp/out.txt",
            "output_truncated": True,
            "original_path": "/tmp/orig.txt",
        },
        {
            "name": "empty_optional_strings_render_null",
            "task_id": "t",
            "status": "running",
            "output": "x",
            "exit_code_meaning": "",
            "failure_hint": "",
            "output_path": "",
            "original_path": "",
            "elapsed_seconds": 0.0,
        },
        {
            "name": "wait_matched_false_and_none",
            "task_id": "t",
            "status": "running",
            "output": "x",
            "wait_matched": False,
        },
        {
            "name": "wait_matched_none",
            "task_id": "t",
            "status": "running",
            "output": "x",
            "wait_matched": None,
        },
        {
            "name": "trailing_newlines_stripped",
            "task_id": "t",
            "status": "completed",
            "output": "a\nb\n\n\n",
        },
        {
            "name": "blank_and_whitespace_lines_not_indented",
            "task_id": "t",
            "status": "completed",
            "output": "a\n\n   \n\t\nb",
        },
        {
            "name": "crlf_and_cr_and_vertical_tab",
            "task_id": "t",
            "status": "completed",
            "output": "a\r\nb\rc\x0bd\x0ce",
        },
        {
            "name": "output_with_unicode",
            "task_id": "t",
            "status": "completed",
            "output": "caf\u00e9 \u4f60\u597d",
        },
        {
            "name": "output_lines_with_leading_spaces",
            "task_id": "t",
            "status": "completed",
            "output": "  indented\nnot",
        },
        {
            "name": "negative_exit_code",
            "task_id": "t",
            "status": "completed",
            "output": "x",
            "exit_code": -1,
            "exit_code_meaning": "killed",
        },
        {
            "name": "elapsed_two_decimals",
            "task_id": "t",
            "status": "completed",
            "output": "x",
            "elapsed_seconds": 59.999,
        },
        {
            "name": "elapsed_large",
            "task_id": "t",
            "status": "completed",
            "output": "x",
            "elapsed_seconds": 12345.678,
        },
        {
            "name": "elapsed_rounding_tie",
            "task_id": "t",
            "status": "completed",
            "output": "x",
            "elapsed_seconds": 2.675,
        },
        {
            "name": "elapsed_rounding_half",
            "task_id": "t",
            "status": "completed",
            "output": "x",
            "elapsed_seconds": 0.005,
        },
        {
            "name": "empty_task_id_and_status",
            "task_id": "",
            "status": "",
            "output": "x",
        },
        {
            "name": "only_newlines_output",
            "task_id": "t",
            "status": "completed",
            "output": "\n\n",
        },
    ]
    rows: list[str] = []
    opt_str = ("exit_code_meaning", "failure_hint", "output_path", "original_path")
    rows: list[str] = []
    for case in cases:
        kwargs = {
            "task_id": case.get("task_id", "t"),
            "status": case.get("status", "completed"),
            "output": case.get("output", ""),
            "wait_matched": case.get("wait_matched", None),
            "elapsed_seconds": case.get("elapsed_seconds", None),
            "exit_code": case.get("exit_code", None),
            "exit_code_meaning": case.get("exit_code_meaning", None),
            "failure_hint": case.get("failure_hint", None),
            "output_path": case.get("output_path", None),
            "output_truncated": case.get("output_truncated", False),
            "original_path": case.get("original_path", None),
        }
        expected = common._build_session_output_block(**kwargs)

        def opt_str(key: str) -> list[str]:
            value = kwargs[key]
            return [
                "true" if value is not None else "false",
                c_lit(value if value is not None else ""),
            ]

        # Field order must match struct py_block_golden exactly.
        fields = [
            c_lit(case["name"]),
            c_lit(kwargs["task_id"]),
            c_lit(kwargs["status"]),
            c_lit(kwargs["output"]),
            "true" if kwargs["exit_code"] is not None else "false",
            str(kwargs["exit_code"]) if kwargs["exit_code"] is not None else "0",
        ]
        fields += opt_str("exit_code_meaning")
        fields += opt_str("failure_hint")
        fields += [
            "true" if kwargs["wait_matched"] is not None else "false",
            "true" if kwargs["wait_matched"] else "false",
        ]
        fields += [
            "true" if kwargs["elapsed_seconds"] is not None else "false",
            repr(float(kwargs["elapsed_seconds"]))
            if kwargs["elapsed_seconds"] is not None
            else "0.0",
        ]
        fields += opt_str("output_path")
        fields.append("true" if kwargs["output_truncated"] else "false")
        fields += opt_str("original_path")
        fields.append(c_lit(expected))
        rows.append(", ".join(fields))
    return rows

# ---------------------------------------------------------------------------
# 7. export path extraction (common.py _extract_export_path)
# ---------------------------------------------------------------------------


def gen_export(common) -> list[str]:
    outputs = [
        "exported to file `C:/t/0.txt`",
        "added to file `C:/t/1.txt`",
        "added to file: C:/t/2.txt",
        "output exported to: C:/t/3.txt",
        "output exported to file: C:/t/4.txt",
        "see added to file: C:/t/5.txt]",
        "exported to file `C:/t/6.txt`]`",
        "prefix exported to file `C:/t/7.txt` suffix",
        "exported to file `C:/t/8.txt",
        "exported to file `C:/t/9.txt``]]",
        "exported to file C:/t/10.txt",
        "exported to file: ",
        "exported to file `",
        "added to file: ",
        "nothing here",
        "",
        "exported to file `caf\u00e9/1.txt`",
        "output exported to file: C:/t/11.txt and also added to file `C:/t/12.txt`",
        "added to file `first` then exported to file: second",
        "]exported to file: tail]",
        "exported to file: C:/spaces are kept.txt  ",
    ]
    rows: list[str] = []
    for idx, output in enumerate(outputs):
        got = common._extract_export_path(output)
        rows.append(
            ", ".join(
                [
                    c_lit(f"case_{idx}"),
                    c_lit(output),
                    "true" if got is not None else "false",
                    c_lit(got if got is not None else ""),
                ]
            )
        )
    return rows


# ---------------------------------------------------------------------------
# 8. wait_for_pattern matching (background/utils.py pattern.search)
# ---------------------------------------------------------------------------

REGEX_META = set(".*?+{()[^$|\\")


def _reference_match(pattern: str, buffer: str) -> tuple[bool, bool]:
    """Return (matched, invalid_regex) for the reference ``pattern.search``."""
    import regex as re  # noqa: PLC0415

    if pattern == "":
        return True, False
    try:
        compiled = re.compile(pattern)
    except Exception:  # noqa: BLE001 - re.error (regex.error) subclasses Exception
        return False, True
    return bool(compiled.search(buffer)), False


def gen_patterns() -> list[str]:
    cases: list[tuple[str, str, str]] = [
        ("literal_match", "ready", "all ready to go"),
        ("literal_no_match", "ready", "nope"),
        ("literal_with_space", "hello world", "say hello world now"),
        ("literal_with_closing_bracket", "a]b", "x a]b y"),
        ("literal_with_closing_brace", "a}b", "x a}b y"),
        ("literal_with_inert_quantifier_text", "a{}b", "x a{}b y"),
        ("literal_with_dash_and_comma", "a-b,c", "a-b,c"),
        ("empty_pattern", "", "buffer"),
        ("regex_dot", "a.b", "axb"),
        ("regex_caret", "^ready", "ready now"),
        ("regex_dollar", "now$", "ready now"),
        ("regex_plus", "ab+c", "abbbc"),
        ("regex_repeat_braces", "a{2}", "aa"),
        ("regex_backslash_d", r"\d+", "x42"),
        ("regex_alternation", "cat|dog", "a dog"),
        ("regex_group", "(ab)+", "abab"),
        # The four glob metacharacters: the reference treats them as *regex*,
        # so a native glob engine silently disagrees (this is the corpus that
        # pins the corrected contract).
        ("regex_star_quantifier", "ready*", "read done"),
        ("regex_star_quantifier_matches", "ready*", "ready now"),
        ("regex_invalid_star_prefix", "*done*", "step done here"),
        ("regex_question_mark", "ab?c", "ac"),
        ("regex_char_class", "err: [0-9]", "err: 7"),
        ("regex_char_class_negated_regex_style", "a[^b]c", "axc"),
        ("regex_char_class_negated_glob_style", "a[!b]c", "axc"),
        ("regex_class_range", "a[b-d]e", "ace"),
        ("regex_unterminated_class", "a[", "a["),
        ("regex_empty_class", "[]", "x"),
        ("regex_unclosed_group", "(unclosed", "x"),
        ("regex_trailing_backslash", "abc\\", "abc\\"),
        ("non_ascii_pattern", "caf\u00e9", "un caf\u00e9 ici"),
        ("non_ascii_buffer_literal_pattern", "cafe", "un caf\u00e9 ici"),
        ("literal_newline", "a\nb", "x a\nb y"),
        ("literal_long", "x" * 200, "y" * 100 + "x" * 200 + "z" * 100),
    ]
    # Deterministic fuzz over the metacharacter alphabet: every pattern is
    # classified by whether it needs the regex engine, and the reference result
    # is recorded so the C++ literal path can be compared byte-exactly.
    rng = random.Random(20240924)
    alphabet = list("ab.*?+{}()[]^$|\\ 01") + ["\n"]
    for i in range(48):
        pattern = "".join(rng.choice(alphabet) for _ in range(rng.randint(1, 6)))
        buffer = "".join(rng.choice(list("abc012 .[]*")) for _ in range(rng.randint(0, 12)))
        cases.append((f"fuzz_{i:02d}", pattern, buffer))

    rows: list[str] = []
    for name, pattern, buffer in cases:
        matched, invalid = _reference_match(pattern, buffer)
        needs_engine = bool(set(pattern) & REGEX_META) or not pattern.isascii()
        if pattern == "":
            needs_engine = True  # degenerate input: kernel refuses it
        rows.append(
            ", ".join(
                [
                    c_lit(name),
                    c_lit(pattern),
                    c_lit(buffer),
                    "true" if matched else "false",
                    "true" if invalid else "false",
                    "true" if needs_engine else "false",
                ]
            )
        )
    return rows


# ---------------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------------


def render() -> str:
    common, py_mod, security = _setup_reference()
    saved = _pure_python(common, py_mod, security)
    try:
        path_rows = gen_paths(common)
        resolve_rows = gen_resolve(py_mod)
        scrub_rows = gen_scrub(security)
        env_rows = gen_env(py_mod)
        hint_rows = gen_hint(py_mod)
        block_rows = gen_block(common)
        export_rows = gen_export(common)
        pattern_rows = gen_patterns()
    finally:
        _restore(saved)

    parts: list[str] = []
    parts.append(
        "// GENERATED by scripts/gen_python_goldens.py from the kimi-agent Python\n"
        "// reference implementation (src/kimix/tools/py/__init__.py,\n"
        "// src/kimix/tools/common.py, src/kimix/tools/security.py,\n"
        "// src/kimix/tools/background/utils.py). Do not edit by hand -\n"
        "// regenerate with: python scripts/gen_python_goldens.py\n"
        "//\n"
        "// Every row is one reference call; test_python_tool.cpp replays it through\n"
        "// the C++ kernels and must reproduce every byte.\n"
    )
    parts.append(
        """
// 1. ScriptFileWriter / plan_script_path  (common.py _create_script_file)
struct py_path_golden {
    const char *name;
    const char *base_dir;
    unsigned long long index;
    const char *ext;
    const char *expected;
};

static const py_path_golden k_python_path_goldens[] = {
"""
    )
    parts.append(c_rows(path_rows))
    parts.append(
        """};

// 2. resolve_python_exe  (py/__init__.py _resolve_python_uncached)
// `existing` is a '\\n'-separated allow-list probed by the injected exists().
// `expected` == "" means the reference returned None (no interpreter at all).
struct py_resolve_golden {
    const char *name;
    const char *override_exe;
    const char *session_dir;
    const char *cwd;
    const char *virtual_env;
    const char *fallback;
    const char *existing;
    const char *expected;
};

static const py_resolve_golden k_python_resolve_goldens[] = {
"""
    )
    parts.append(c_rows(resolve_rows))
    parts.append(
        """};

// 3. scrub_child_env  (security.py scrub_child_env)
// `env` is "NAME\\tVALUE" lines; `expected` the surviving NAME lines.
struct py_scrub_golden {
    const char *name;
    const char *env;
    const char *expected;
};

static const py_scrub_golden k_python_scrub_goldens[] = {
"""
    )
    parts.append(c_rows(scrub_rows))
    parts.append(
        """};

// 4. prepare_python_env  (py/__init__.py _build_env)
// `expect_fast_path` == true means the reference returned None (zero-copy path).
// Otherwise the delta must set PATH to `expected_path` and, when
// `expect_virtual_env_change` is true, VIRTUAL_ENV to `expected_virtual_env`.
struct py_env_golden {
    const char *name;
    const char *python_exe;
    const char *share_bin_dir;
    const char *current_path;
    const char *parent_virtual_env;
    bool pyvenv_cfg_exists;
    bool expect_fast_path;
    bool expect_virtual_env_change;
    const char *expected_path;
    const char *expected_virtual_env;
};

static const py_env_golden k_python_env_goldens[] = {
"""
    )
    parts.append(c_rows(env_rows))
    parts.append(
        """};

// 5. module_not_found_hint  (py/__init__.py _module_not_found_hint)
struct py_hint_golden {
    const char *name;
    const char *output;
    const char *python_exe;
    const char *expected;
};

static const py_hint_golden k_python_hint_goldens[] = {
"""
    )
    parts.append(c_rows(hint_rows))
    parts.append(
        """};

// 6. build_session_output_block  (common.py _build_session_output_block)
struct py_block_golden {
    const char *name;
    const char *task_id;
    const char *status;
    const char *output;
    bool has_exit_code;
    int exit_code;
    bool has_exit_code_meaning;
    const char *exit_code_meaning;
    bool has_failure_hint;
    const char *failure_hint;
    bool has_wait_matched;
    bool wait_matched;
    bool has_elapsed_seconds;
    double elapsed_seconds;
    bool has_output_path;
    const char *output_path;
    bool output_truncated;
    bool has_original_path;
    const char *original_path;
    const char *expected;
};

static const py_block_golden k_python_block_goldens[] = {
"""
    )
    parts.append(c_rows(block_rows))
    parts.append(
        """};

// 7. extract_export_path  (common.py _extract_export_path)
// `expected_present` is false when the reference returned None (an *empty*
// string result is a distinct, valid outcome).
struct py_export_golden {
    const char *name;
    const char *output;
    bool expected_present;
    const char *expected;
};

static const py_export_golden k_python_export_goldens[] = {
"""
    )
    parts.append(c_rows(export_rows))
    parts.append(
        """};

// 8. wait_for_pattern  (background/utils.py pattern.search -> `regex`)
// The reference compiles wait_for_pattern as a *regex* and calls
// pattern.search(buffer). `needs_regex_engine` is true when the pattern uses
// any regex metacharacter (or is non-ASCII / empty), i.e. when a literal
// substring search cannot reproduce the reference result; the native kernel
// must answer tool_status::unsupported for those (the caller routes them to
// the Python regex engine). For `needs_regex_engine == false` the kernel's
// literal match must equal `reference_matched` byte-exactly.
struct py_pattern_golden {
    const char *name;
    const char *pattern;
    const char *buffer;
    bool reference_matched;
    bool reference_invalid_regex;
    bool needs_regex_engine;
};

static const py_pattern_golden k_python_pattern_goldens[] = {
"""
    )
    parts.append(c_rows(pattern_rows))
    parts.append("};\n")
    return "".join(parts)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true", help="fail when out of date")
    ap.add_argument("--python", default=None, help="interpreter used to import the reference")
    args = ap.parse_args()

    if args.python is not None:
        exe = Path(args.python)
        if not exe.is_file():
            raise SystemExit(f"--python interpreter not found: {exe}")
        if Path(sys.executable).resolve() != exe.resolve():
            proc = subprocess.run(
                [str(exe), str(Path(__file__).resolve()), *filter(None, ["--check"] if args.check else [])],
                check=False,
            )
            return proc.returncode

    text = render()
    if args.check:
        current = OUT_PATH.read_text(encoding="utf-8") if OUT_PATH.is_file() else ""
        if current != text:
            print(f"{OUT_PATH} is out of date; run scripts/gen_python_goldens.py")
            return 1
        print(f"{OUT_PATH} is up to date")
        return 0
    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUT_PATH.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUT_PATH} ({len(text)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
