"""Differential parity tests for the runtime command scanners.

``src/runtime/parse/shell_scanner.cpp`` owns the BASH_FIX dialect behind
``runtime_py.parse.shell_scan``, which is the kernel
``kimix_native.parse.fix_bash_command`` uses on the Python-visible path.  The
ground truth is kimi-agent's ``bin/kimix_native/_shell_compat.py``, loaded *by
path* from the checkout (the same file ``scripts/gen_bash_fix_data.py``
generates the kernel's tables from), never ``kimix_native``'s own mirror.

What is pinned here:

* every ``_FALLBACK_BODIES`` name (88) at a command position is recorded by the
  kernel and the reconstructed fix is byte-identical to the reference;
* every ``_UNSUPPORTED_BODIES`` name (``journalctl``) is reported through the
  ``scan_shell`` ``unsupported`` channel with the command text left untouched;
* ``_FALLBACK_COMMAND_WRAPPERS`` (``gtimeout``/``watch``/``sudo``): ``sudo``'s
  fallback name is recorded *and* its wrapper semantics entered; the other two
  record the name only - the ``timeout``/``watch`` wrapper semantics are not
  ported into the kernel, and ``kimix_native.parse`` routes those commands to
  the reference (``_OPERAND_WRAPPER_WORDS``);
* ``kimix_native.parse`` no longer diverts the ten newest names
  (``free``/``htop``/``ip``/``journalctl``/``man``/``ss``/``sudo``/
  ``systemctl``/``top``/``uptime``) to the mirror with a
  ``_POST_KERNEL_FALLBACKS`` bridge, and its result (including ``unsupported``)
  equals the reference over the whole committed corpus
  (``tests/unit/builtin_tools/bash_fix_goldens.inc``, 2946 commands).

Known kernel gaps (documented in ``shell_scanner.h``, routed to the reference by
the shim, deliberately out of scope here): the ``timeout``/``stdbuf``/``nice``/
``xargs`` operand wrappers, redundant ``bash``/``sh`` unwrapping, Git Bash
virtual absolute paths and the ``timeout``/``watch`` fallback wrapper kinds.
"""

from __future__ import annotations

import importlib.util
import os
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from _parity_ref import BIN_DIR, KIMI_AGENT_ROOT, ref_available  # noqa: E402

pytestmark = pytest.mark.skipif(
    not ref_available(), reason="kimi-agent checkout not available"
)

#: The canonical reference module (kimi-agent's own copy, not the mirror).
SHELL_COMPAT_PATH = KIMI_AGENT_ROOT / "bin" / "kimix_native" / "_shell_compat.py"

REPO_ROOT = Path(__file__).resolve().parents[2]
GOLDENS_INC = (
    REPO_ROOT / "tests" / "unit" / "builtin_tools" / "bash_fix_goldens.inc"
)


def _load_by_path(path: Path, name: str):
    """Load a standalone reference module by file path (no package context)."""
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:  # pragma: no cover - defensive
        pytest.skip(f"cannot load reference module {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


REF = _load_by_path(SHELL_COMPAT_PATH, "_parity_shell_scanner_ref")

# Import the extension *after* the reference is loaded by path, and prove it is
# this checkout's build (a staged runtime_py.pyd under kimi-agent/bin would make
# every comparison below a port-versus-itself run).
sys.path.insert(0, str(BIN_DIR))
import runtime_py  # noqa: E402

assert Path(runtime_py.__file__).resolve().parent == Path(BIN_DIR).resolve(), (
    f"runtime_py came from {runtime_py.__file__}, not this checkout's build "
    f"{BIN_DIR}"
)

import kimix_native.parse as SHIM  # noqa: E402
import kimix_native._shell_compat as SHIM_MIRROR  # noqa: E402

assert Path(SHIM.__file__).resolve() == (
    REPO_ROOT / "python" / "kimix_native" / "parse.py"
).resolve(), f"kimix_native.parse came from {SHIM.__file__}"

# ---------------------------------------------------------------------------
# kernel-level helpers
# ---------------------------------------------------------------------------

FIELDS = (
    "command",
    "replacements",
    "path_changes",
    "nul_fixes",
    "unsupported",
)


def kernel_fix(cmd: str) -> dict:
    """Reconstruct a BashFix-shaped result from ``parse.shell_scan``.

    Mirrors what ``kimix_native.parse.fix_bash_command`` does with the kernel's
    output (marker expansion + fallback-definition prefix from the vendored
    mirror), so the comparison is against the *kernel's* data, not the shim's
    routing decisions.
    """
    data = cmd.encode("utf-8", "surrogatepass")
    scan = runtime_py.parse.shell_scan("bash_fix", data)
    assert len(scan) == 5, "kernel does not expose the unsupported channel"
    edits, names_bytes, notes_bytes, nul_bytes, unsupported_bytes = scan
    assert not nul_bytes, f"unexpected nul fixes for {cmd!r}"
    pieces, previous = [], 0
    for start, end, replacement in sorted(edits):
        if replacement.startswith(b"\x01") and replacement.endswith(b"\x01"):
            name = replacement[1:-1].decode("utf-8", "surrogatepass")
            replacement = SHIM_MIRROR._wrapper_runner(name).encode(
                "utf-8", "surrogatepass"
            )
        pieces.append(data[previous:start])
        pieces.append(replacement)
        previous = end
    pieces.append(data[previous:])
    source = b"".join(pieces).decode("utf-8", "surrogatepass")
    names = [n.decode("utf-8", "surrogatepass") for n in names_bytes]
    unique = list(dict.fromkeys(names))
    definitions = "\n".join(SHIM_MIRROR._FALLBACKS[n] for n in unique)
    exports = "\n".join(
        f"if declare -F {n} >/dev/null; then export -f {n}; fi" for n in unique
    )
    prefix = definitions + "\n" + exports + "\n" if definitions else ""
    return {
        "command": prefix + source,
        "replacements": names,
        "path_changes": [n.decode("utf-8", "surrogatepass") for n in notes_bytes],
        "nul_fixes": [],
        "unsupported": [
            n.decode("utf-8", "surrogatepass") for n in unsupported_bytes
        ],
    }


def reference_fix(cmd: str) -> dict:
    res = REF.fix_bash_command(cmd)
    return {field: list(getattr(res, field)) if field != "command"
            else res.command for field in FIELDS}


def shim_fix(cmd: str) -> dict:
    res = SHIM.fix_bash_command(cmd)
    return {field: list(getattr(res, field)) if field != "command"
            else res.command for field in FIELDS}


def first_difference(got: dict, expected: dict):
    for field in FIELDS:
        if got[field] != expected[field]:
            return field
    return None


def name_probe_commands(name: str) -> list[str]:
    """Command-position variants for one name (all kernel-supported)."""
    return [
        name,
        f"{name} -h",
        f"{name} arg1 arg2",
        f"echo x | {name}",
        f"{name} && echo ok",
    ]


# ---------------------------------------------------------------------------
# kernel: the reference's name tables
# ---------------------------------------------------------------------------


def test_kernel_records_every_reference_fallback_name():
    """All 88 ``_FALLBACK_BODIES`` names must be kernel-recognised, exactly."""
    names = list(REF._FALLBACK_BODIES)
    assert len(names) == 88, f"reference carries {len(names)} fallback names"
    bad = []
    for name in names:
        for cmd in name_probe_commands(name):
            got, expected = kernel_fix(cmd), reference_fix(cmd)
            field = first_difference(got, expected)
            if field is not None:
                bad.append((cmd, field, got[field], expected[field]))
    assert not bad, f"{len(bad)} kernel/reference differences: {bad[:5]}"


def test_kernel_records_the_nine_previously_missing_names():
    """The nine names the hand-maintained kernel table was missing."""
    missing = ["free", "htop", "ip", "man", "ss", "sudo", "systemctl", "top",
               "uptime"]
    for name in missing:
        assert name in REF._FALLBACK_BODIES, name
        for cmd in name_probe_commands(name):
            got = kernel_fix(cmd)
            assert got["replacements"] == reference_fix(cmd)["replacements"], cmd
            assert name in got["replacements"], (cmd, got["replacements"])


def test_kernel_reports_unsupported_names():
    """``_UNSUPPORTED_BODIES`` must travel in the ``unsupported`` channel."""
    unsupported = list(REF._UNSUPPORTED_BODIES)
    assert unsupported, "reference exposes no unsupported table"
    for name in unsupported:
        for cmd in [name, f"{name} -u svc -f", f"echo x | {name}",
                    f"{name} && echo ok", f"nohup {name} -f",
                    f"{name}; {name} -f", r"journalctl C:\log\app.log"]:
            got, expected = kernel_fix(cmd), reference_fix(cmd)
            field = first_difference(got, expected)
            assert field is None, (
                f"{cmd!r}: kernel {field}={got[field]!r} != "
                f"reference {expected[field]!r}"
            )
            assert got["unsupported"] == [name], (cmd, got["unsupported"])
            # No fallback definition and no edit: the text runs untouched.
            assert got["replacements"] == [], (cmd, got["replacements"])


def test_kernel_fallback_command_wrappers():
    """``_FALLBACK_COMMAND_WRAPPERS``: name recording + ``sudo``'s wrapper."""
    cases = [
        "sudo ls", "sudo rev", "env sudo rev", "sudo -u root rev",
        r"sudo -D C:\x rev", "sudo journalctl -f", "journalctl -u svc -f",
        "env journalctl -f", "nohup journalctl -f", "systemctl restart x",
        "man ls", "ss -tlnp", "top -b -n 1", "uptime -s", "ip addr", "htop",
        "mytop topfree freebsd", "journalctl -u svc; free -h", "echo free",
        "'free'", r"\free", "x=$(free)", "nohup free", "free -h | head",
    ]
    for cmd in cases:
        got, expected = kernel_fix(cmd), reference_fix(cmd)
        field = first_difference(got, expected)
        assert field is None, (
            f"{cmd!r}: kernel {field}={got[field]!r} != "
            f"reference {expected[field]!r}"
        )
    # sudo is the one _FALLBACK_COMMAND_WRAPPERS kind this kernel implements.
    assert kernel_fix("sudo rev")["replacements"] == ["sudo", "rev"]


def test_kernel_does_not_report_a_marker_for_unsupported_names():
    """An unsupported word is never swapped for a fallback runner."""
    data = b"env journalctl -f"
    edits, names, notes, nul, unsupported = runtime_py.parse.shell_scan(
        "bash_fix", data
    )
    assert edits == []
    assert names == []
    assert nul == []
    assert unsupported == [b"journalctl"]


# ---------------------------------------------------------------------------
# shim: the bridge is gone and the whole committed corpus still matches
# ---------------------------------------------------------------------------


def test_shim_has_no_post_kernel_fallback_bridge():
    """The ten newest names must not be diverted to the mirror again."""
    assert not hasattr(SHIM, "_POST_KERNEL_FALLBACKS"), (
        "kimix_native.parse re-introduced the post-kernel name bridge; the "
        "kernel's generated tables are authoritative"
    )
    assert not hasattr(SHIM, "_POST_KERNEL_RE")


def test_shim_routes_kernel_supported_names_to_the_kernel():
    """Every name of the ten must actually reach the kernel (not the mirror)."""
    names = ["free", "htop", "ip", "journalctl", "man", "ss", "sudo",
             "systemctl", "top", "uptime"]
    for name in names:
        for cmd in (name, f"{name} -h", f"echo x | {name}"):
            assert not SHIM._WRAPPER_RE.search(cmd), cmd
            assert not SHIM._GIT_BASH_ABS_PATH_RE.search(cmd), cmd
            assert not SHIM._NUL_REDIRECT_RE.search(cmd), cmd
            # the mirror's own result must equal the shim's: the shim used the
            # kernel, and the kernel agrees with the reference
            assert shim_fix(cmd) == reference_fix(cmd), cmd


def test_regression_ten_previously_missing_names():
    """Regression vectors for the parity-review bug (shim level, full command)."""
    cases = [
        "free", "free -h", "free -m arg", "uptime", "uptime -s", "top",
        "top -b -n 1", "ss", "ss -tlnp", "ip addr", "ip route", "man ls",
        "systemctl status sshd", "htop", "htop -u root", "sudo ls",
        "sudo rev", "env sudo rev", "journalctl", "journalctl -u svc -f",
        "sudo journalctl -f", "systemctl restart x && free -h",
    ]
    for cmd in cases:
        got, expected = shim_fix(cmd), reference_fix(cmd)
        field = first_difference(got, expected)
        assert field is None, (
            f"{cmd!r}: shim {field}={got[field]!r} != reference "
            f"{expected[field]!r}"
        )
    # The unsupported verdict carries the reference's reason text.
    res = SHIM.fix_bash_command("journalctl -u svc -f")
    assert res.unsupported == ("journalctl",)
    assert res.command == "journalctl -u svc -f"
    assert res.warning == REF.fix_bash_command("journalctl -u svc -f").warning


def _c_literals(text: str) -> list[str]:
    """Decode every C string literal of one generated golden row."""
    out, i, n = [], 0, len(text)
    while i < n:
        if text[i] != '"':
            i += 1
            continue
        i += 1
        buf: list[str] = []
        while i < n and text[i] != '"':
            ch = text[i]
            if ch == "\\" and i + 1 < n:
                nxt = text[i + 1]
                if nxt in "01234567":
                    j = i + 1
                    digits = ""
                    while j < n and len(digits) < 3 and text[j] in "01234567":
                        digits += text[j]
                        j += 1
                    buf.append(chr(int(digits, 8)))
                    i = j
                    continue
                buf.append({"n": "\n", "t": "\t", "r": "\r", "0": "\0"}.get(
                    nxt, nxt))
                i += 2
                continue
            buf.append(ch)
            i += 1
        i += 1
        out.append("".join(buf))
    return out


def golden_rows() -> list[list[str]]:
    """The committed corpus: 8 fields per ``bash_fix_goldens.inc`` row."""
    if not GOLDENS_INC.is_file():  # pragma: no cover - defensive
        pytest.skip(f"{GOLDENS_INC} not present")
    text = GOLDENS_INC.read_text(encoding="utf-8")
    body = text[text.index("const bash_fix_golden k_bash_fix_goldens[] = {"):]
    rows = []
    for line in body.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        fields = _c_literals(line)
        if len(fields) == 8:
            rows.append(fields)
    return rows


def test_committed_corpus_contains_the_new_names():
    """Guard: the corpus sweep below really exercises the ten names."""
    commands = [row[0] for row in golden_rows()]
    assert len(commands) > 2000, len(commands)
    pattern = re.compile(
        r"(?:^|[\s;|&(){}!\n])"
        r"(?:free|htop|ip|journalctl|man|ss|sudo|systemctl|top|uptime)"
        r"(?=[\s;|&(){}<>\n]|$)"
    )
    hits = [c for c in commands if pattern.search(c)]
    assert len(hits) > 10, len(hits)


def test_shim_matches_reference_over_the_committed_corpus():
    """2946 commands: ``kimix_native.parse`` vs the kimi-agent reference.

    Commands the shim deliberately routes to the mirror (shell wrappers,
    operand wrappers, Git Bash virtual paths, ``nul`` redirections) compare the
    reference with itself; everything else - including every command using one
    of the ten newest names, which used to be diverted by the
    ``_POST_KERNEL_FALLBACKS`` bridge - compares the *kernel's* output.
    """
    rows = golden_rows()
    assert len(rows) > 2000, len(rows)
    bad = []
    for row in rows:
        cmd = row[0]
        got, expected = shim_fix(cmd), reference_fix(cmd)
        field = first_difference(got, expected)
        if field is not None:
            bad.append((cmd, field, got[field], expected[field]))
    assert not bad, f"{len(bad)}/{len(rows)} commands differ: {bad[:5]}"
