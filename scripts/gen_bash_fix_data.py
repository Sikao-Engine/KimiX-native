"""Generate the native BashFix data tables and the byte-exact golden vectors.

The C++ port of the Windows Git Bash compatibility fix (bash_fix.py /
``_shell_compat.py`` in C:/dev/kimi-agent) is verified against the canonical
pure-Python reference implementation.  This script is the single source of the
derived data, so nothing is transcribed by hand:

* ``--tables``   rewrites the generated region of ``src/builtin_tools/bash_tool.cpp``
                 (between the ``GENERATED:BASH-FIX-DATA`` markers) with

                   - ``_FALLBACK_BODIES`` (names + bodies, reference order),
                   - ``_UNSUPPORTED_BODIES`` (name + reason).

* ``--tables-runtime`` (alias ``--parse-tables``) rewrites the generated region
                 of ``src/runtime/parse/shell_scanner.cpp`` (between the
                 ``GENERATED:BASH-FIX-PARSE-DATA`` markers) with the BASH_FIX
                 scanner's *name* tables -- ``_FALLBACK_BODIES`` keys,
                 ``_FALLBACK_COMMAND_WRAPPERS`` (name -> wrapper kind) and
                 ``_UNSUPPORTED_BODIES`` keys -- so the runtime kernel can never
                 drift from the reference again, and writes the name coverage
                 vectors of ``tests/unit/native/shell_scanner_names_goldens.inc``.

* ``--goldens``  writes ``tests/unit/builtin_tools/bash_fix_goldens.inc``
                 (compact expectations, one row per input) and
                 ``tests/unit/builtin_tools/bash_fix_prefix_goldens.inc``
                 (full expected command / prelude for a subset), by running the
                 reference implementation over

                   - every string literal used by the ``TestBashFix*`` classes
                     of C:/dev/kimi-agent/tests/test_bash.py,
                   - a curated feature corpus,
                   - a deterministic fuzz corpus.

* ``--rtk``      writes ``tests/unit/builtin_tools/bash_rtk_goldens.inc``: the
                 RTK rewrite scanner (``_split_shell_segments``,
                 ``_rewrite_shell_segment``, ``_is_known_rtk_command``,
                 ``_maybe_rewrite_shell_command_with_rtk``) of
                 ``<kimi-agent>/src/kimix/tools/common.py`` over the commands
                 kimi-agent's own tests use, an adversarial corpus and a
                 deterministic fuzz corpus (4 rewrite profiles each).

Usage (from the project root, with the reference checkout available):

    python scripts/gen_bash_fix_data.py --all

The reference module is located via ``--reference`` (default:
``C:/dev/kimi-agent/bin/kimix_native/_shell_compat.py``); the golden vectors use
a fixed Windows temp directory (``C:/Temp``) so they do not depend on the host
that generated them.  ``--reference-common`` points at the RTK scanner's home
(default ``C:/dev/kimi-agent/src/kimix/tools/common.py``).
"""

from __future__ import annotations

import argparse
import ast
import importlib.util
import os
import random
import sys
from pathlib import Path
from unittest import mock

PROJECT_ROOT = Path(__file__).resolve().parent.parent
BASH_TOOL_CPP = PROJECT_ROOT / "src" / "builtin_tools" / "bash_tool.cpp"
SHELL_SCANNER_CPP = PROJECT_ROOT / "src" / "runtime" / "parse" / "shell_scanner.cpp"
GOLDENS_INC = PROJECT_ROOT / "tests" / "unit" / "builtin_tools" / "bash_fix_goldens.inc"
PREFIX_GOLDENS_INC = (
    PROJECT_ROOT / "tests" / "unit" / "builtin_tools" / "bash_fix_prefix_goldens.inc"
)
RTK_GOLDENS_INC = (
    PROJECT_ROOT / "tests" / "unit" / "builtin_tools" / "bash_rtk_goldens.inc"
)
RUNTIME_NAMES_INC = (
    PROJECT_ROOT / "tests" / "unit" / "native" / "shell_scanner_names_goldens.inc"
)
DEFAULT_REFERENCE = Path("C:/dev/kimi-agent/bin/kimix_native/_shell_compat.py")
DEFAULT_REFERENCE_COMMON = Path("C:/dev/kimi-agent/src/kimix/tools/common.py")
DEFAULT_REFERENCE_TESTS = Path("C:/dev/kimi-agent/tests/test_bash.py")
#: kimi-agent checkout layout (``--reference-common`` lives under ``<root>/src``).
KIMI_AGENT_ROOT = Path("C:/dev/kimi-agent")
KIMI_CLI_SRC = KIMI_AGENT_ROOT / "kimi-cli" / "src"
KIMIX_SRC = KIMI_AGENT_ROOT / "src"

DATA_BEGIN = "// >>> GENERATED:BASH-FIX-DATA >>>"
DATA_END = "// <<< GENERATED:BASH-FIX-DATA <<<"

# The runtime PARSE kernel (src/runtime/parse/shell_scanner.cpp) keeps its own
# copy of the same reference data (the fallback names, the fallback command
# wrappers and the unsupported names) because it is a pure C++ kernel that must
# not include the builtin tool's tables.  It is generated from the same
# reference file, so the two can never drift apart again.
RUNTIME_DATA_BEGIN = "// >>> GENERATED:BASH-FIX-PARSE-DATA >>>"
RUNTIME_DATA_END = "// <<< GENERATED:BASH-FIX-PARSE-DATA <<<"

# Fixed Windows temp directory used for the golden vectors (the reference test
# suite monkeypatches ``_windows_temp_dir`` the same way).
GOLDEN_TEMP_DIR = "C:/Temp"

# Unit separator: joins list fields inside one golden row.
SEP = "\x1f"


def load_reference(path: Path):
    spec = importlib.util.spec_from_file_location("_kimix_bash_fix_reference", path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"cannot load reference module: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    module._windows_temp_dir = lambda: GOLDEN_TEMP_DIR
    return module


# ---------------------------------------------------------------------------
# tables
# ---------------------------------------------------------------------------

def c_string(value: str, chunk: int = 4000) -> str:
    """Render *value* as a C++ string literal (UTF-8 source escapes).

    Non-printable bytes become three-digit octal escapes: a ``\\xNN`` escape is
    greedy, so a following hex digit would extend it (``"\\x1Fde"`` would be
    parsed as one out-of-range escape).  Values longer than *chunk* bytes are
    emitted as adjacent literals because MSVC rejects a single literal over
    16380 bytes (C2026).
    """
    data = value.encode("utf-8")
    pieces = []
    for start in range(0, max(len(data), 1), chunk):
        out = ['"']
        for byte in data[start : start + chunk]:
            if byte == 0x22:
                out.append('\\"')
            elif byte == 0x5C:
                out.append("\\\\")
            elif 0x20 <= byte < 0x7F:
                out.append(chr(byte))
            else:
                out.append(f"\\{byte:03o}")
        out.append('"')
        pieces.append("".join(out))
    return "\n".join(pieces) if len(pieces) > 1 else pieces[0]


def render_tables(shell) -> str:
    lines = [
        "// GENERATED by scripts/gen_bash_fix_data.py from kimi-agent's",
        "// bin/kimix_native/_shell_compat.py - DO NOT EDIT BY HAND.",
        "//",
        "// _FALLBACK_BODIES (88 entries: reference insertion order is preserved",
        "// because _FALLBACKS - and therefore the exported definitions prefix and",
        "// bash_compatibility_prelude() - iterate in that order) and",
        "// _UNSUPPORTED_BODIES (commands with no faithful Git Bash equivalent).",
        "const bash_fix_fallback_body k_bash_fix_fallback_bodies[] = {",
    ]
    for name, body in shell._FALLBACK_BODIES.items():
        lines.append(f"    {{{c_string(name)}, {c_string(body)}}},")
    lines.append("};")
    lines.append("")
    reasons = getattr(shell, "_UNSUPPORTED_BODIES", {})
    if reasons:
        lines.append("const bash_fix_unsupported_reason k_bash_fix_unsupported_reasons[] = {")
        for name, reason in reasons.items():
            lines.append(f"    {{{c_string(name)}, {c_string(reason)}}},")
        lines.append("};")
    else:
        lines.append("const bash_fix_unsupported_reason k_bash_fix_unsupported_reasons[] = {};")
    return "\n".join(lines)


def write_text_lf(path: Path, text: str) -> None:
    """Write *text* with LF line endings.

    ``Path.write_text`` opens in text mode and translates ``\\n`` to the
    platform separator, so on Windows a single regeneration rewrote every line
    of ``bash_tool.cpp`` and of the ``.inc`` files as CRLF (a whole-file diff
    against the LF blobs in git).  ``newline=""`` disables the translation.
    """
    with open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)


def write_tables(shell) -> None:
    text = BASH_TOOL_CPP.read_text(encoding="utf-8")
    begin = text.index(DATA_BEGIN) + len(DATA_BEGIN)
    end = text.index(DATA_END)
    block = "\n" + render_tables(shell) + "\n"
    updated = text[:begin] + block + text[end:]
    write_text_lf(BASH_TOOL_CPP, updated)
    print(f"tables: {BASH_TOOL_CPP} ({len(shell._FALLBACK_BODIES)} fallbacks, "
          f"{len(getattr(shell, '_UNSUPPORTED_BODIES', {}))} unsupported)")


# ---------------------------------------------------------------------------
# runtime PARSE kernel tables (src/runtime/parse/shell_scanner.cpp)
# ---------------------------------------------------------------------------

def render_runtime_tables(shell) -> str:
    """The BASH_FIX scanner's name tables (fallbacks / wrappers / unsupported).

    The runtime kernel is pure C++ (it cannot include the builtin tool's
    generated tables), so the same reference data is spliced into its own
    marked region from the same source file.
    """
    fallbacks = shell._FALLBACK_BODIES
    wrappers = getattr(shell, "_FALLBACK_COMMAND_WRAPPERS", {})
    unsupported = getattr(shell, "_UNSUPPORTED_BODIES", {})
    lines = [
        "// GENERATED by scripts/gen_bash_fix_data.py from kimi-agent's",
        "// bin/kimix_native/_shell_compat.py - DO NOT EDIT BY HAND.",
        "//",
        "// The BASH_FIX scanner's name tables, keyed by the reference's own data:",
        "//",
        "//   kFallbackNames    _FALLBACK_BODIES keys (" + str(len(fallbacks)) +
        " names, reference order).",
        "//                     A word at an executable command position whose",
        "//                     quote-removal value is one of these has its fallback",
        "//                     definition recorded (and is swapped for the standalone",
        "//                     runner when an exec-ing wrapper consumes it as its",
        "//                     command operand).",
        "//   kFallbackWrapperNames  _FALLBACK_COMMAND_WRAPPERS: names that are",
        "//                     fallback names AND command wrappers.  `kind` is the",
        "//                     reference wrapper kind; this kernel implements the",
        "//                     \"sudo\" kind (WrapperKind::SUDO) and only records the",
        "//                     name for the others - the \"timeout\"/\"watch\" wrapper",
        "//                     semantics (option tables, the mandatory DURATION",
        "//                     operand, quoted `watch` scripts) are not ported here,",
        "//                     and the Python shim routes those commands to the",
        "//                     pure-Python reference.",
        "//   kUnsupportedNames _UNSUPPORTED_BODIES keys (" + str(len(unsupported)) +
        " name(s)): commands with no",
        "//                     faithful Windows Git Bash equivalent.  The scanner",
        "//                     records them in its `unsupported` output and leaves the",
        "//                     command text byte-for-byte, so the caller can refuse to",
        "//                     run the command with the reason instead of letting Bash",
        "//                     fail with \"command not found\".",
        "struct fallback_wrapper_entry {",
        "    const char *name;",
        "    const char *kind;",
        "};",
        "",
        "const char* const kFallbackNames[] = {",
    ]
    row: list[str] = []
    for name in fallbacks:
        row.append(c_string(name))
        if len(row) == 8:
            lines.append("    " + ", ".join(row) + ",")
            row = []
    if row:
        lines.append("    " + ", ".join(row) + ",")
    lines.append("};")
    lines.append("")
    lines.append("const fallback_wrapper_entry kFallbackWrapperNames[] = {")
    for name, kind in wrappers.items():
        lines.append(f"    {{{c_string(name)}, {c_string(kind)}}},")
    lines.append("};")
    lines.append("")
    lines.append("const char* const kUnsupportedNames[] = {")
    if unsupported:
        for name in unsupported:
            lines.append(f"    {c_string(name)},")
    else:
        # An empty initializer list is not valid C++; the reference always
        # carries at least one unsupported name, and the empty string can never
        # equal a scanned word (read_word never yields an empty word).
        lines.append(f"    {c_string('')},")
    lines.append("};")
    lines.append("")
    # Counts are derived from the arrays themselves: a table that drifts from
    # its count would otherwise be an out-of-bounds read at run time.
    lines.append("constexpr size_t kFallbackNamesCount =")
    lines.append("    sizeof(kFallbackNames) / sizeof(kFallbackNames[0]);")
    lines.append("constexpr size_t kUnsupportedNamesCount =")
    lines.append("    sizeof(kUnsupportedNames) / sizeof(kUnsupportedNames[0]);")
    return "\n".join(lines)


def render_runtime_names_inc(shell) -> str:
    """Name coverage vectors for tests/unit/native/test_shell_scanner.cpp."""
    lines = [
        "// GENERATED by scripts/gen_bash_fix_data.py - DO NOT EDIT BY HAND.",
        "//",
        "// Every command name the reference's BASH_FIX scanner knows about, with",
        "// the verdict the kernel must produce for it:",
        "//   name         the word (quoted-literal forms are covered by the",
        "//                reference corpus sweep, not by this table)",
        "//   wrapper_kind _FALLBACK_COMMAND_WRAPPERS membership (\"\" = plain",
        "//                fallback name); only the kinds this kernel implements",
        "//                (see kFallbackWrapperNames) also get wrapper semantics",
        "//   unsupported  _UNSUPPORTED_BODIES membership: the name goes to the",
        "//                scan_shell `unsupported` output and the command text is",
        "//                left byte-for-byte (no fallback name, no replacement)",
        "struct shell_scanner_name_golden {",
        "    const char *name;",
        "    const char *wrapper_kind;",
        "    bool unsupported;",
        "};",
        "",
    ]
    wrappers = getattr(shell, "_FALLBACK_COMMAND_WRAPPERS", {})
    unsupported = getattr(shell, "_UNSUPPORTED_BODIES", {})
    names = list(shell._FALLBACK_BODIES) + [
        n for n in unsupported if n not in shell._FALLBACK_BODIES
    ]
    lines.append(f"// {len(names)} name vectors "
                 f"({len(shell._FALLBACK_BODIES)} fallbacks, "
                 f"{len(unsupported)} unsupported)")
    lines.append("const shell_scanner_name_golden k_shell_scanner_name_goldens[] = {")
    for name in names:
        lines.append("    {{{}, {}, {}}},".format(
            c_string(name),
            c_string(wrappers.get(name, "")),
            "true" if name in unsupported else "false",
        ))
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def write_runtime_tables(shell) -> None:
    text = SHELL_SCANNER_CPP.read_text(encoding="utf-8")
    begin = text.index(RUNTIME_DATA_BEGIN) + len(RUNTIME_DATA_BEGIN)
    end = text.index(RUNTIME_DATA_END)
    block = "\n" + render_runtime_tables(shell) + "\n"
    updated = text[:begin] + block + text[end:]
    write_text_lf(SHELL_SCANNER_CPP, updated)
    write_text_lf(RUNTIME_NAMES_INC, render_runtime_names_inc(shell))
    print(f"runtime tables: {SHELL_SCANNER_CPP} "
          f"({len(shell._FALLBACK_BODIES)} fallbacks, "
          f"{len(getattr(shell, '_FALLBACK_COMMAND_WRAPPERS', {}))} fallback "
          f"wrappers, "
          f"{len(getattr(shell, '_UNSUPPORTED_BODIES', {}))} unsupported) + "
          f"{RUNTIME_NAMES_INC}")


# ---------------------------------------------------------------------------
# corpus
# ---------------------------------------------------------------------------

CURATED = [
    # fallbacks / command positions
    "rev", "'rev' <<< abc", '"rev" <<< abc', r"\rev <<< abc", 'r""ev <<< abc',
    "rev first.txt second.txt", "printf x | rev", "gtimeout 1 true",
    "gtimeout 1 true; printf x | rev", "xdg-open .", "open README.md",
    "printf text | pbcopy", "pbpaste", "wget https://example.com/f.zip",
    "wget -O out.zip https://example.com/f.zip", "wget -q -c -T 5 https://x/y",
    "xclip -selection clipboard", "xsel -bo", "gsed -n 1p file", "gawk '{print}'",
    "zip -r out.zip dir", "zip -q -9 out.zip a b", "nc -z example.com 80",
    "netcat -z -v -w 3 example.com 80", "pgrep bash", "pgrep -lf python",
    "pkill -f python", "tree -L 1 dir", "tree -a -d --noreport", "say hello",
    "wl-copy < file", "wl-paste -n", "python3 --version", "pip3 install x",
    "traceroute -n -m 5 example.com", "column -t -s , file",
    "killall python", "pidof python", "watch -n 1 rev", "watch 'rev <<< abc'",
    "watch -n1 'cd D:\\x && rev'", "htop", "free -h", "free", "uptime -s",
    "top -b -n 1", "ss -s", "ss -tlnp", "ip addr", "ip route", "man ls",
    "systemctl status sshd", "journalctl -u svc -f", "sudo ls",
    # cmd.exe style / misc
    "copy a b", "move a b", "del file.txt", "ren a b", "md dir", "rd dir",
    "cls", "xcopy src dst", "mklink link target", "findstr foo file",
    "fc a b", "where git", "tasklist", "taskkill /F /IM python.exe",
    "systeminfo",
    # wrappers / operands
    "timeout 5 rev <<< abc", "timeout 5s rev <<< abc",
    "timeout --foreground 5 rev <<< abc", "timeout -s KILL 5 rev <<< abc",
    "timeout --signal KILL 5 rev <<< abc", "timeout --kill-after 1 5 rev <<< abc",
    "timeout -- 5 rev <<< abc", "stdbuf -oL rev <<< abc",
    "stdbuf --output=L rev <<< abc", "stdbuf -o L -e L rev <<< abc",
    "nice -n 5 rev <<< abc", "nice --adjustment 5 rev <<< abc",
    "nice rev <<< abc", "xargs rev", "xargs -0 rev", "xargs -n 2 rev",
    "xargs -I {} rev", "printf x | xargs rev", "time timeout 5 rev <<< abc",
    "nohup timeout 5 rev <<< abc", "env timeout 5 rev <<< abc",
    "timeout 5 python3 --version", "gtimeout 5 rev <<< abc",
    "env bash rev <<< abc", "nohup bash rev <<< abc", "env bash -c 'rev <<< abc'",
    "sudo bash -c 'rev <<< abc'", "timeout 5 bash -c 'rev <<< abc'",
    "xargs -a C:\\in.txt rev", "xargs --arg-file=C:\\in.txt rev",
    "env -C C:\\x rev", "env --chdir=C:\\x rev", "time -o C:\\out.txt rev",
    "sudo -D C:\\x rev", "watch -t -d -n 1 rev <<< abc",
    "timeout 3 watch -n 1 rev <<< abc",
    # shell wrappers
    "bash cd /c/dev/x && echo ok", "sh cd /c/dev/x && echo ok",
    "bash grep -rn kimix src tests --include=*.h | head -40",
    "bash -c 'rev'", 'bash -c "rev"', "bash -lc 'rev'", "bash -cl 'rev'",
    "bash -l -c 'rev'", "sh -c 'rev'", "dash -c 'rev'", "ash -c 'rev'",
    "bash -c 'rev' && echo done", "echo $(bash -c 'rev')",
    "bash -c 'echo $HOME'", "bash -c 'cd C:\\x && rev'", "bash cd /c/dev/x && rev",
    "'bash' cd /c/dev/x && echo ok", '"bash" cd /c/dev/x && echo ok',
    r"\bash cd /c/dev/x && echo ok",
    "bash script.sh", "bash ./script.sh", "bash ../tools/run", "bash scripts/deploy.sh",
    "sh build.sh --release", "bash -c 'echo hi' arg1", 'bash -e -c "rev"',
    'bash -ec "rev"', 'bash -x "rev"', "bash -s", "bash --", "bash", "echo bash",
    "ls sh", "bash -c", "FOO=1 bash -c 'printf %s \"$FOO\"'",
    "env FOO=1 bash -c 'printf %s \"$FOO\"'", "timeout 5 bash -c 'rev <<< abc && rev <<< xyz'",
    # nul redirection
    "echo hi > nul", "echo hi >NUL", "echo hi 2> nul", "echo hi &> nul",
    "echo hi >> nul", "echo hi 2>> nul", "echo hi>nul",
    "echo hi > nul; echo bye > nul", "echo 'nul' > nul",
    "echo a > nul; echo b > NUL; echo c >nul", "echo hi > 'nul'",
    'echo hi > "nul"', "echo hi < nul", "echo nul", "echo /dev/null > nul.txt",
    "echo hi > null", "echo hi > /dev/null",
    # Windows paths
    r"cd D:\repo\src", r"C:\Windows\System32\where.exe git", r"d:\tools\run.exe --help",
    r"\\server\share\tool.exe arg", r".\build\tool.exe arg", r"..\scripts\run.sh",
    r"~\bin\tool.exe --help", r"\Users\me\tool.exe", r"build\dist\tool.exe arg",
    r"echo a && C:\x\tool.exe", r"echo a; C:\x\tool.exe | cat", r"(C:\x\tool.exe)",
    r"{ C:\x\tool.exe; }", r"if C:\x\probe.exe; then echo ok; fi",
    r"while C:\x\poll.exe; do :; done", r"command C:\x\tool.exe",
    r"env FOO=1 D:\x\tool.exe", r"nohup D:\x\tool.exe &", r"D:\x\*.exe",
    r"D:\Program\ Files\x.exe", r"x=$(C:\x\tool.exe)", r"echo `C:\x\tool.exe`",
    r"echo \033\015", r"a\nb arg", r"\a\b", r"x\n\t", r"foo\bar arg",
    r"cd /d D:\x", "cd /d", "cd /d && echo x", "cd /d # comment",
    r"cd /d D:\x && echo /d && cd /d/foo", r"cd D:\x", r"echo a > C:\x\y.txt",
    r"env FOO=D:\x true", r"cat D:\x\y.txt", r"declare -a arr=(D:\x\y.txt)",
    # Git Bash virtual paths
    "echo /tmp/x.txt", "cat > /tmp/out.txt", "cd /tmp", "rm -f /tmp/a /tmp/b",
    "echo /c/dev/file.cpp", "echo /C/Dev/file.cpp", "cd /d/foo",
    "/c/Windows/System32/where.exe cmd",
    "cd /c/dev/RoboCute && cat > /tmp/test_util.cpp <<'EOF'\nx\nEOF",
    "echo $(cat /tmp/x)", "env --chdir=/tmp cmd", "arr=(/tmp/a.txt /c/b.txt)",
    "echo '/tmp/x'", 'echo "/tmp/x"', "cat <<'EOF'\n/tmp/x\nEOF",
    "cat <<< /tmp/x", "echo /tmpfile", "echo /d", "echo /c", "x=/tmp/x",
    "alias cd=/c/x", "echo ok # cd /tmp/x", "chdir /tmp",
    # arrays
    "arr=(D:\\x\\y.txt D:\\a\\b.txt)", "arr+=(D:\\x\\y.txt)",
    "arr=(C:\\data\\*.csv)", "declare -a arr=(D:\\x\\y.txt)",
    "declare -a arr=(D:\\x\\y.txt plain)", "local arr=(D:\\x\\y.txt)",
    "arr=(D:\\Program\\ Files\\x.txt)", "arr=(D:\\x\\y.txt); echo ${arr[0]}",
    "arr=('D:\\x\\y.txt')", 'arr=("D:\\x\\y.txt")', "array=(rev open pbcopy)",
    "declare -a x=(rev)", "declare -a x=(wget xclip xsel)", "arr=([k]=D:\\x)",
    "arr=(a\\nb)", "arr=($(rev <<< abc))",
    # heredocs / operators / robustness
    "cat <<EOF\nhi\nEOF", "cat <<EOF\nhi\nEOF\n&& echo done",
    "cat <<EOF\nhi\nEOF\n&& rev", "cat <<-EOF\n\thi\n\tEOF\n&& echo x",
    "cat <<'EOF'\n$(rev)\nEOF", "cat <<EOF\n$(rev)\nEOF",
    "cat <<EOF\nD:\\x\nEOF", "echo a && rev <<< abc",
    "echo x; rev; echo y", "printf x | rev | cat", "rev | rev",
    "rev || rev", "rev & rev", "rev && (rev)", "if rev; then rev; fi",
    "for f in a b; do rev; done", "while rev; do :; done", "case x in rev) rev;; esac",
    "case $f in D:\\x) echo ok;; esac", "case $(rev) in *) :;; esac",
    "function rev { :; }", "rev() { :; }", "f() { rev; }", "declare -f rev",
    "{ rev; }", "! rev", "time rev", "command -v rev", "command -p rev",
    "exec -a name rev", "coproc NAME { rev; }", "coproc rev",
    # quotes / escapes / substitutions
    "echo \"a ; b\"", "echo 'a ; b'", "echo `rev`", "echo $(rev)",
    "echo \"$(rev)\"", "echo \"${rev}\"", "echo ${rev:-x}", "echo $((1+2))",
    "echo [[", "[[ -f x ]] && rev", "(( x = 1 ))", "rev <(cat x)", "rev > >(cat)",
    "echo '", 'echo "', "echo `", "echo $(", "echo ${", "((", "cat <<EOF\nunterminated",
    "echo \\", "rev '", 'rev "', "echo $(rev", "echo `rev", "if rev; then",
    "case x in rev)", "\x01rev\x01", "rev\r\n rev", "rev\t&&\trev",
    "sudo rev", "nohup rev &", "echo rev > /tmp/x", "rev; echo done",
    "env PATH=/x rev", "env -u PATH rev", "env -S 'rev' x",
    "xargs -n1 -I{} rev", "time -o C:\\out.txt rev", "stdbuf -i0 -o0 rev",
    "nice --adjustment=5 rev", "timeout -k 1 5 rev", "timeout 5 /bin/rev",
    "watch -n 5 'date && rev'", "watch --interval=1 rev", "watch -q rev",
    "cd /c/dev/x; rev; cd /tmp", "echo /c/a /tmp/b /d/c",
    "printf '%s\\n' rev", "echo \"rev\", echo 'rev'", "alias rev='rev'",
    "run=timeout", "echo watch date", "echo 'watch rev'", "echo xargs rev",
    "case x in timeout) echo no;; esac", "alias watch='tail -f log'",
    "function timeout { :; }", "timeout() { :; }", "echo hi # timeout 5 rev",
    "echo timeout 5 rev",
]


def literals_from_reference_tests(path: Path) -> list[str]:
    """Every string literal used inside the TestBashFix* classes."""
    if not path.exists():
        return []
    tree = ast.parse(path.read_text(encoding="utf-8"))
    found: list[str] = []
    for node in ast.walk(tree):
        if isinstance(node, ast.ClassDef) and node.name.startswith("TestBashFix"):
            for sub in ast.walk(node):
                if isinstance(sub, ast.Constant) and isinstance(sub.value, str):
                    found.append(sub.value)
    return found


FUZZ_TOKENS = [
    "rev", "gtimeout", "watch", "sudo", "timeout", "stdbuf", "nice", "xargs",
    "env", "nohup", "time", "command", "exec", "coproc", "bash", "sh", "dash",
    "ash", "free", "uptime", "top", "ss", "ip", "man", "systemctl", "journalctl",
    "zip", "tree", "wget", "nc", "pgrep", "pkill", "killall", "pidof", "column",
    "traceroute", "say", "python3", "pip3", "copy", "move", "del", "md", "ren",
    "where", "tasklist", "taskkill", "systeminfo", "findstr", "fc", "cls",
    "grep", "ls", "cat", "echo", "cd", "printf", "declare", "local", "export",
    "if", "then", "else", "fi", "for", "do", "done", "while", "case", "esac",
    "in", "function", "return", "true", "xx", "-n", "-r", "-f", "--", "-c",
    "-lc", "-cl", "-l", "-e", "-o", "5", "1", "2", "abc", "EOF",
    "D:\\x\\y.txt", "C:\\Program Files\\a", "\\\\srv\\share", ".\\build", "..\\x",
    "~\\b", "\\a\\b", "foo\\bar", "/tmp/x", "/c/dev", "/d", "/tmp",
    ">", ">>", "2>", "&>", "<", "<<<", "|", "||", "&&", ";", "&", "(", ")",
    "{", "}", "[[", "]]", "((", "))", "`", "'", '"', "\\", "$(", "${", "$((1))",
    "#c", "\n", "\t", " ", "=", "a=", "nul", "NUL",
]


def fuzz_corpus(count: int, seed: int = 20240923) -> list[str]:
    rng = random.Random(seed)
    out: list[str] = []
    for _ in range(count):
        parts = []
        for _ in range(rng.randint(1, 9)):
            parts.append(rng.choice(FUZZ_TOKENS))
            if rng.random() < 0.7:
                parts.append(" ")
        command = "".join(parts)
        if "\n" in command and rng.random() < 0.5:
            command = command.replace("\n", " <<EOF\nEOF\n")
        out.append(command)
    return out


def build_corpus(reference_tests: Path) -> list[str]:
    candidates: list[str] = []
    candidates.extend(literals_from_reference_tests(reference_tests))
    candidates.extend(CURATED)
    candidates.extend(fuzz_corpus(1800))
    seen: set[str] = set()
    corpus: list[str] = []
    for command in candidates:
        if not command or len(command) > 1200:
            continue
        if not command.isascii():
            continue
        if command in seen:
            continue
        seen.add(command)
        corpus.append(command)
    return corpus


# ---------------------------------------------------------------------------
# goldens
# ---------------------------------------------------------------------------

def reference_prefix(result, shell) -> str:
    """The prefix the reference scanner prepends (definitions + exports)."""
    unique = list(dict.fromkeys(result.replacements))
    if not unique:
        return ""
    definitions = "\n".join(shell._FALLBACKS[name] for name in unique)
    exports = "\n".join(
        f"if declare -F {name} >/dev/null; then export -f {name}; fi" for name in unique
    )
    return definitions + "\n" + exports + "\n"


def golden_row(shell, command: str) -> tuple[str, ...]:
    result = shell.fix_bash_command(command)
    prefix = reference_prefix(result, shell)
    assert result.command.startswith(prefix), command
    source = result.command[len(prefix):]
    return (
        command,
        SEP.join(result.replacements),
        SEP.join(result.path_changes),
        SEP.join(result.shell_wrappers),
        SEP.join(result.nul_fixes),
        SEP.join(getattr(result, "unsupported", ())),
        source,
        result.warning,
    )


def render_goldens(shell, corpus: list[str]) -> str:
    lines = [
        "// GENERATED by scripts/gen_bash_fix_data.py - DO NOT EDIT BY HAND.",
        "//",
        "// Byte-exact expectations from the reference implementation",
        "// (kimi-agent bin/kimix_native/_shell_compat.py) for a Windows Git Bash",
        "// host with the temp directory fixed to C:/Temp.  Fields per row:",
        "//   command, replacements, path_changes, shell_wrappers, nul_fixes,",
        "//   unsupported, expected_source, expected_warning",
        "// List fields are joined with \\x1f (unit separator).",
        "// The expected command is prefix + expected_source; the prefix",
        "// composition (fallback definitions + conditional exports) is covered by",
        "// bash_fix_prefix_goldens.inc.",
        "struct bash_fix_golden {",
        "    const char *command;",
        "    const char *replacements;",
        "    const char *path_changes;",
        "    const char *shell_wrappers;",
        "    const char *nul_fixes;",
        "    const char *unsupported;",
        "    const char *expected_source;",
        "    const char *expected_warning;",
        "};",
        "",
        f"// {len(corpus)} golden vectors",
        "const bash_fix_golden k_bash_fix_goldens[] = {",
    ]
    for command in corpus:
        row = golden_row(shell, command)
        lines.append("    {" + ", ".join(c_string(f) for f in row) + "},")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def render_prefix_goldens(shell) -> str:
    lines = [
        "// GENERATED by scripts/gen_bash_fix_data.py - DO NOT EDIT BY HAND.",
        "//",
        "// Full expected commands (fallback definitions + conditional exports +",
        "// rewritten source) and the interactive compatibility prelude, taken",
        "// verbatim from the reference implementation.",
        "struct bash_fix_prefix_golden {",
        "    const char *command;",
        "    const char *expected_command;",
        "};",
        "",
        "const bash_fix_prefix_golden k_bash_fix_prefix_goldens[] = {",
    ]
    cases: list[str] = [name for name in shell._FALLBACK_BODIES]
    cases.extend(
        [
            "rev; rev",
            "gtimeout 1 true; printf x | rev",
            "rev && tree",
            "timeout 5 rev <<< abc",
            "xargs rev",
            "env bash rev <<< abc",
            "bash -c 'rev && tree'",
            "watch 'rev'",
            "cd /c/dev/x && rev",
            "journalctl -u svc",
            "rev <<< abc",
            "cat <<EOF\nhi\nEOF\n&& rev",
        ]
    )
    for command in cases:
        result = shell.fix_bash_command(command)
        lines.append(f"    {{{c_string(command)}, {c_string(result.command)}}},")
    lines.append("};")
    lines.append("")
    # bash_compatibility_prelude() is gated on sys.platform in the reference.
    with mock.patch.object(shell.sys, "platform", "win32"):
        prelude = shell.bash_compatibility_prelude()
    lines.append("// bash_compatibility_prelude() on win32.")
    lines.append(f"const char k_bash_fix_prelude[] = {c_string(prelude)};")
    lines.append("")
    return "\n".join(lines)


def write_goldens(shell, reference_tests: Path) -> None:
    corpus = build_corpus(reference_tests)
    write_text_lf(GOLDENS_INC, render_goldens(shell, corpus))
    write_text_lf(PREFIX_GOLDENS_INC, render_prefix_goldens(shell))
    print(f"goldens: {GOLDENS_INC} ({len(corpus)} vectors, "
          f"{GOLDENS_INC.stat().st_size} bytes)")
    print(f"prefix goldens: {PREFIX_GOLDENS_INC} "
          f"({PREFIX_GOLDENS_INC.stat().st_size} bytes)")


# ---------------------------------------------------------------------------
# RTK rewrite goldens (common.py)
# ---------------------------------------------------------------------------
# The RTK scanner (_split_shell_segments / _read_shell_word /
# _rewrite_shell_segment / _is_known_rtk_command /
# _maybe_rewrite_shell_command_with_rtk) lives in kimi-agent's
# ``src/kimix/tools/common.py`` -- pure Python there, no native gate.  It
# decides whether the command the agent actually RUNS gets an ``rtk`` prefix, so
# a silent divergence changes behaviour; the vectors below pin it byte-for-byte.

#: str(Path(...)) form of the fixed rtk binary the reference reports through
#: ``_rtk_binary_path()``: exercises the absolute-path fast path without
#: depending on the host.
GOLDEN_RTK_BINARY = "C:\\Temp\\rtk.exe"

#: (token_kill, exclude_read, pwsh, rtk_binary_path) rewrite profiles.
RTK_PROFILES: list[tuple[bool, bool, bool, str]] = [
    (True, False, False, ""),
    (True, True, False, ""),
    (True, False, True, ""),
    (True, False, False, GOLDEN_RTK_BINARY),
]

#: Commands used by kimi-agent's own suite (tests/test_token_filter.py
#: ``rtk_available`` fixture, tests/test_bash.py ``TestBashRtkRewrite``).
RTK_AGENT_CASES = [
    "find . -name '*.cpp' -not -path './build/*'",
    "git status", "git status && cargo test", "git status; cargo test",
    "git status || cargo test", "rtk git status",
    "RTK_DISABLED=1 git status", "unknown-cmd arg", 'echo "git status"',
    "read var", "git status | grep x", "VAR=value git status",
    'echo "$(git status)"', "echo `git status`", "git log | head",
    "& rtk git status",
]

#: Adversarial corpus: quoting, separators, env prefixes, subshells, comments,
#: escaped separators and path-shaped executable tokens.  The near-duplicate
#: pairs (``dir/git/`` vs ``dir/git``, ``\\\\git`` vs ``\\\\srv\\share\\git``,
#: ``ls\\`` vs ``ls``) exist because the ``Path(...).stem`` step used to be
#: approximated by "text after the last separator", which disagrees with
#: pathlib on all of them.
RTK_CURATED = RTK_AGENT_CASES + [
    # bare commands / leftmost-command detection
    "git", "git status --short", "ls -la", "npm run build", "Git status",
    "GIT status", "git.exe status", "GIT.EXE status", "read x", "READ x",
    "find .", "find.exe .", "gradlew build", "mvn -q", "rg foo",
    "grep -n x f", "oc get pods", "echo hello", "cat x",
    # quotes / substitutions / subshells
    "'git status'", '"git status"', "git 'status'", 'git "status"',
    "echo 'git status'", "echo $(git status)", "echo `git status`",
    'echo "$(git status)"', "echo ${git}", "(git status)", "$(git status)",
    "x=$(git status) git log", "(cd /tmp && git status)",
    "echo $(cd x; git status)", "git log $(git status)",
    "git log `git status`", "echo $(git status", "echo `git status",
    "echo 'git status", 'echo "git status', "echo a\\;b", "echo \"a ; b\"",
    "echo 'a;b'", "printf '%s' git", "echo $'git'", "eval git", "echo `a`git",
    # separators / pipelines / background
    "git log | head", "git log | head | tail", "git log |head", "git log| head",
    "git status||cargo test", "git status;cargo test", "git status;;git status",
    "git log&&&git status", "git log|||git status", "git log &",
    "git log&", "git log & git status", "git log&git status",
    "cd / &&git status", "x;git status", ";git status", "&&git status",
    "|git status", "||git status", "git log \\; git status",
    "git log \\| git status", "git log \\&& git status",
    "git log \\\\ git status", "git status && (cargo test)",
    "if git status; then cargo test; fi", "for f in a b; do git status; done",
    "git status\ncargo test", "git status\t&&\tcargo test", "  git status  ",
    # comments (the reference has no comment awareness)
    "git log # git status", "# git status", "  # git status",
    "git log; # git status", "git log && # git status", "git log # x && ls",
    # env prefixes / assignments / modifiers
    "FOO=1 git status", "FOO=1 BAR=2 git log", "FOO= git log", "_x=1 git log",
    "9x=1 git log", "A_1= git log", "FOO.bar=1 git log", "FOO=1 sudo git log",
    "sudo git log", "time git log", "nohup git log", "nice git log",
    "nice -n5 git log", "time sudo git log", "sudo time nice git log",
    "RTK_DISABLED=1", "RTK_DISABLED=1 sudo git log", "RTK_DISABLED=2 git log",
    "RTK_DISABLED=1x git log", "x=RTK_DISABLED=1 git log", "sudo=true git log",
    "sudoer git log", "timex git log", "nohupx git log",
    # already-rewritten / rtk spellings
    "rtk", "rtk ", "rtk\tgit log", "rtk.exe", "rtk.exe git log", "& rtk",
    "& rtk git log", "&  rtk git log", "RTK git log", "RTK.EXE git log",
    "rtk.exe.git log", "rtkx git log",
    # path-shaped executables (WindowsPath.stem semantics)
    "/usr/bin/git status", "./git status", "dir/git status", "dir/git/",
    "dir/git/.", "git/.", "//git", "\\\\git", "\\\\srv\\share\\git status",
    "C:\\bin\\git status", "C:\\bin\\git.exe status",
    "\"C:\\Program Files\\git.exe\" status", "'/usr/bin/git' status",
    "$HOME/bin/git status", "~/bin/git status", "../git status",
    ".../git status", "git.", ".git", "a.b", "x.exe.exe", "git.exe.git",
    "git.git", "read/", "read.", "node_modules/.bin/vite", "ls\\", "ls\\\\",
    "npm\\", "gradlew\\\\", "go&$HOME/bin/git\\", "git\\\\", "git\\x",
    "git/..", "./git/", "C:git", "C:", "C:/", "C:\\", "\\\\?\\C:\\bin\\git",
    "\\\\?\\UNC\\srv\\share\\git", "\\\\\\git", "dir//git", "dir/./git",
    # whitespace / empties
    "\tgit status", "\ngit status", " git status", "git\tstatus",
    "git \t status", "\rgit status", "git\rstatus", "   ", "", "\t", "\n",
]

#: Names checked against _is_known_rtk_command (the reference table plus every
#: near miss that has ever been confused with a table entry).
RTK_KNOWN_NAMES = [
    "ls", "tree", "read", "smart", "grep", "rg", "diff", "wc", "json", "log",
    "env", "deps", "git", "cargo", "vitest", "jest", "tsc", "lint", "prettier",
    "format", "next", "prisma", "playwright", "npm", "npx", "pnpm", "pytest",
    "ruff", "mypy", "pip", "uv", "go", "golangci-lint", "rspec", "rubocop",
    "rake", "dotnet", "docker", "kubectl", "oc", "aws", "gh", "glab", "gt",
    "curl", "wget", "psql", "php", "phpunit", "phpstan", "pest", "paratest",
    "ecs", "pint", "gradlew", "mvn",
    "find", "find.exe", "echo", "cat", "rtk", "rtk.exe", "", " ", "LS ", " ls",
    "git ", "g", "gi", "gitt", "git--", "git.exe.exe", "smart.exe", "Smart",
    "SMART", "GOLANGCI-LINT", "golangci_lint", "go.exe", "npm.cmd", "npm.exe",
    "ls.exe.exe", "readme", "reading", "formatter", "nextjs", "cargo.lock",
    "docker-compose", "docker.exe", "kubectl.exe", "glab-cli", "psql.exe",
]

_RTK_FUZZ_WORDS = [
    "git", "ls", "read", "find", "grep", "rg", "cargo", "npm", "npx", "pnpm",
    "pytest", "ruff", "uv", "go", "docker", "kubectl", "aws", "gh", "curl",
    "wget", "psql", "php", "mvn", "gradlew", "rustup", "echo", "cat", "true",
    "sudo", "time", "nohup", "nice", "env", "command", "exec", "xargs",
    "timeout", "stdbuf", "rtk", "rtk.exe", "RTK", "Git", "GIT", "git.exe",
    "read.exe", "readme", "/usr/bin/git", "./git", "../git", "dir/git",
    "dir/git.exe", "C:\\bin\\git", "C:\\bin\\git.exe", "~/bin/git",
    "$HOME/bin/git", "a.b", "git.", ".git", "x.exe.exe", "FOO=1", "_x=1",
    "9x=1", "RTK_DISABLED=1", "RTK_DISABLED=1x", "'git'", '"git"',
    "'git status'", "$'git'", "${VAR}", "$HOME", "`git status`",
    "$(git status)", "'a;b'", '"a|b"', "status", "-la", "--short", "--", "x",
    "\\\\git", "//git", "dir/git/", "git/.", "ls\\", "C:git", "C:\\bin\\git\\",
]
_RTK_FUZZ_SEPARATORS = [" ", "  ", "\t", "", " ; ", " && ", " || ", " | ",
                        " & ", ";", "&&", "||", "|", "&", " \\; ", " \\| "]
_RTK_FUZZ_PUNCT = ["'", '"', "`", "\\\\", "\\", "$(", "(", ")", "#", "\n",
                   "\r", "!"]


def rtk_fuzz_corpus(count: int, seed: int = 20240923) -> list[str]:
    """Deterministic shell-shaped corpus for the RTK scanner."""
    rng = random.Random(seed)
    out: list[str] = []
    for _ in range(count):
        parts: list[str] = []
        n = rng.randint(1, 8)
        for i in range(n):
            parts.append(rng.choice(_RTK_FUZZ_WORDS))
            if i != n - 1:
                parts.append(rng.choice(_RTK_FUZZ_SEPARATORS))
            if rng.random() < 0.1:
                parts.append(rng.choice(_RTK_FUZZ_PUNCT))
        if rng.random() < 0.15:
            parts.insert(0, rng.choice(["# ", " ", "\t", "\n"]))
        if rng.random() < 0.1:
            parts.append(rng.choice(["'", '"', "`", "$(", "\\"]))
        out.append("".join(parts))
    return out


def build_rtk_corpus(fuzz_count: int = 700) -> list[str]:
    """Curated corpus + kimi-agent's cases + the reference table names."""
    candidates = list(RTK_CURATED)
    candidates.extend(RTK_KNOWN_NAMES)
    candidates.extend(rtk_fuzz_corpus(fuzz_count))
    seen: set[str] = set()
    corpus: list[str] = []
    for command in candidates:
        if command is None or len(command) > 400:
            continue
        if not command.isascii():
            continue
        # \x1e / \x1f encode the golden rows (segment separator / field
        # separator); other control characters are noise for this scanner.
        if any(ord(c) < 0x20 and c not in "\t\n\r" for c in command):
            continue
        if command in seen:
            continue
        seen.add(command)
        corpus.append(command)
    return corpus


def load_common_reference(path: Path, shell):
    """Import kimi-agent's ``kimix.tools.common`` with fixed RTK gates.

    ``common.py`` is pure Python for the RTK kernels, but its four quote helpers
    delegate to ``_shell_compat.py``; the module is pinned to the canonical copy
    loaded by :func:`load_reference` so the goldens never depend on whichever
    ``kimix_native`` happens to be importable.
    """
    for entry in (str(KIMI_CLI_SRC), str(KIMIX_SRC)):
        if os.path.isdir(entry) and entry not in sys.path:
            sys.path.insert(0, entry)
    common = importlib.import_module("kimix.tools.common")
    common._COMPAT_SHELL = shell
    # Deterministic gates: rtk is "available" and reports a fixed binary path.
    common._rtk_available = lambda: True
    common._rtk_binary_path = lambda: Path(GOLDEN_RTK_BINARY)
    return common


def render_rtk_goldens(shell, common, corpus: list[str]) -> str:
    lines = [
        "// GENERATED by scripts/gen_bash_fix_data.py - DO NOT EDIT BY HAND.",
        "//",
        "// Byte-exact expectations from the reference RTK scanner",
        "// (kimi-agent src/kimix/tools/common.py: _split_shell_segments,",
        "// _rewrite_shell_segment, _is_known_rtk_command,",
        "// _maybe_rewrite_shell_command_with_rtk) for a host where rtk is",
        "// available and _rtk_binary_path() reports " + GOLDEN_RTK_BINARY + ".",
        "//",
        "// - k_bash_rtk_rewrite_goldens: one row per (command, profile).",
        "//   rtk_binary_path == \"\" means the reference reported None (no",
        "//   absolute-path fast path); rtk_available is always true in these rows.",
        "// - k_bash_rtk_segment_goldens: _rewrite_shell_segment per segment.",
        "// - k_bash_rtk_split_goldens: _split_shell_segments; the segments field",
        "//   joins (text, \\x1f, sep) with \\x1e (an empty command encodes as",
        "//   \"\\x1f\").",
        "// - k_bash_rtk_known_goldens: _is_known_rtk_command per name.",
        "struct bash_rtk_rewrite_golden {",
        "    const char *command;",
        "    bool token_kill;",
        "    bool exclude_read;",
        "    bool pwsh;",
        "    const char *rtk_binary_path;",
        "    const char *rewritten;",
        "    bool changed;",
        "};",
        "",
        "struct bash_rtk_segment_golden {",
        "    const char *segment;",
        "    bool exclude_read;",
        "    bool pwsh;",
        "    const char *rewritten;",
        "    bool changed;",
        "};",
        "",
        "struct bash_rtk_split_golden {",
        "    const char *command;",
        "    const char *segments;",
        "};",
        "",
        "struct bash_rtk_known_golden {",
        "    const char *name;",
        "    bool known;",
        "};",
        "",
    ]
    rows = 0
    lines.append(f"// {len(corpus) * len(RTK_PROFILES)} rewrite vectors")
    lines.append("const bash_rtk_rewrite_golden k_bash_rtk_rewrite_goldens[] = {")
    for command in corpus:
        for token_kill, exclude_read, pwsh, rtk_path in RTK_PROFILES:
            rewritten, changed = common._maybe_rewrite_shell_command_with_rtk(
                command, token_kill, exclude_read, pwsh
            )
            lines.append(
                "    {"
                + ", ".join([
                    c_string(command),
                    "true" if token_kill else "false",
                    "true" if exclude_read else "false",
                    "true" if pwsh else "false",
                    c_string(rtk_path),
                    c_string(rewritten),
                    "true" if changed else "false",
                ])
                + "},"
            )
            rows += 1
    lines.append("};")
    lines.append("")

    lines.append(f"// {len(corpus) * 3} segment vectors")
    lines.append("const bash_rtk_segment_golden k_bash_rtk_segment_goldens[] = {")
    for command in corpus:
        for exclude_read, pwsh in ((False, False), (True, False), (False, True)):
            rewritten, changed = common._rewrite_shell_segment(
                command, exclude_read, pwsh
            )
            lines.append(
                "    {"
                + ", ".join([
                    c_string(command),
                    "true" if exclude_read else "false",
                    "true" if pwsh else "false",
                    c_string(rewritten),
                    "true" if changed else "false",
                ])
                + "},"
            )
            rows += 1
    lines.append("};")
    lines.append("")

    lines.append(f"// {len(corpus)} split vectors")
    lines.append("const bash_rtk_split_golden k_bash_rtk_split_goldens[] = {")
    for command in corpus:
        segments = common._split_shell_segments(command)
        encoded = "\x1e".join(f"{text}\x1f{sep}" for text, sep in segments)
        lines.append(f"    {{{c_string(command)}, {c_string(encoded)}}},")
        rows += 1
    lines.append("};")
    lines.append("")

    lines.append(f"// {len(RTK_KNOWN_NAMES)} known-command vectors")
    lines.append("const bash_rtk_known_golden k_bash_rtk_known_goldens[] = {")
    for name in RTK_KNOWN_NAMES:
        known = common._is_known_rtk_command(name)
        lines.append(f"    {{{c_string(name)}, {'true' if known else 'false'}}},")
        rows += 1
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def write_rtk_goldens(shell, reference: Path) -> None:
    common = load_common_reference(reference, shell)
    corpus = build_rtk_corpus()
    write_text_lf(RTK_GOLDENS_INC, render_rtk_goldens(shell, common, corpus))
    print(f"rtk goldens: {RTK_GOLDENS_INC} ({len(corpus)} commands, "
          f"{RTK_GOLDENS_INC.stat().st_size} bytes)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, default=DEFAULT_REFERENCE)
    parser.add_argument("--reference-common", type=Path,
                        default=DEFAULT_REFERENCE_COMMON)
    parser.add_argument("--reference-tests", type=Path, default=DEFAULT_REFERENCE_TESTS)
    parser.add_argument("--tables", action="store_true")
    parser.add_argument("--tables-runtime", "--parse-tables", action="store_true",
                        dest="tables_runtime",
                        help="rewrite the GENERATED:BASH-FIX-PARSE-DATA region "
                             "of src/runtime/parse/shell_scanner.cpp and "
                             "tests/unit/native/shell_scanner_names_goldens.inc")
    parser.add_argument("--goldens", action="store_true")
    parser.add_argument("--rtk", action="store_true",
                        help="rewrite bash_rtk_goldens.inc (RTK scanner vectors)")
    parser.add_argument("--all", action="store_true")
    args = parser.parse_args()
    if not (args.tables or args.tables_runtime or args.goldens or args.rtk or args.all):
        parser.error("nothing to do: pass --tables, --tables-runtime, --goldens, "
                     "--rtk or --all")
    shell = load_reference(args.reference)
    if args.tables or args.all:
        write_tables(shell)
    if args.tables_runtime or args.all:
        write_runtime_tables(shell)
    if args.goldens or args.all:
        write_goldens(shell, args.reference_tests)
    if args.rtk or args.all:
        write_rtk_goldens(shell, args.reference_common)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
