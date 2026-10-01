"""Generate the runtime BASH_FIX scanner tables and the bash RTK goldens.

The Windows Git Bash compatibility fix is no longer ported into
``src/builtin_tools/bash_tool.cpp`` (the command reaches Git Bash exactly as
written; only the ``export MSYSTEM=; `` neutralization survives), so what is
derived here is the data the remaining kernels still compile in.  Everything
comes straight from the canonical pure-Python reference (``_shell_compat.py`` /
``common.py`` in C:/dev/kimi-agent), so nothing is transcribed by hand:

* ``--tables-runtime`` (alias ``--parse-tables``) rewrites the generated region
                 of ``src/runtime/parse/shell_scanner.cpp`` (between the
                 ``GENERATED:BASH-FIX-PARSE-DATA`` markers) with the BASH_FIX
                 scanner's *name* tables -- ``_FALLBACK_BODIES`` keys,
                 ``_FALLBACK_COMMAND_WRAPPERS`` (name -> wrapper kind) and
                 ``_UNSUPPORTED_BODIES`` keys -- so the runtime kernel can never
                 drift from the reference again, and writes the name coverage
                 vectors of ``tests/unit/native/shell_scanner_names_goldens.inc``.

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
import importlib.util
import os
import random
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
SHELL_SCANNER_CPP = PROJECT_ROOT / "src" / "runtime" / "parse" / "shell_scanner.cpp"
RTK_GOLDENS_INC = (
    PROJECT_ROOT / "tests" / "unit" / "builtin_tools" / "bash_rtk_goldens.inc"
)
RUNTIME_NAMES_INC = (
    PROJECT_ROOT / "tests" / "unit" / "native" / "shell_scanner_names_goldens.inc"
)
DEFAULT_REFERENCE = Path("C:/dev/kimi-agent/bin/kimix_native/_shell_compat.py")
DEFAULT_REFERENCE_COMMON = Path("C:/dev/kimi-agent/src/kimix/tools/common.py")
#: kimi-agent checkout layout (``--reference-common`` lives under ``<root>/src``).
KIMI_AGENT_ROOT = Path("C:/dev/kimi-agent")
KIMI_CLI_SRC = KIMI_AGENT_ROOT / "kimi-cli" / "src"
KIMIX_SRC = KIMI_AGENT_ROOT / "src"


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
# C++ literal rendering helpers
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



def write_text_lf(path: Path, text: str) -> None:
    """Write *text* with LF line endings.

    ``Path.write_text`` opens in text mode and translates ``\\n`` to the
    platform separator, so on Windows a single regeneration rewrote every line
    of ``bash_tool.cpp`` and of the ``.inc`` files as CRLF (a whole-file diff
    against the LF blobs in git).  ``newline=""`` disables the translation.
    """
    with open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)



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
    parser.add_argument("--tables-runtime", "--parse-tables", action="store_true",
                        dest="tables_runtime",
                        help="rewrite the GENERATED:BASH-FIX-PARSE-DATA region "
                             "of src/runtime/parse/shell_scanner.cpp and "
                             "tests/unit/native/shell_scanner_names_goldens.inc")
    parser.add_argument("--rtk", action="store_true",
                        help="rewrite bash_rtk_goldens.inc (RTK scanner vectors)")
    parser.add_argument("--all", action="store_true")
    args = parser.parse_args()
    if not (args.tables_runtime or args.rtk or args.all):
        parser.error("nothing to do: pass --tables-runtime, --rtk or --all")
    shell = load_reference(args.reference)
    if args.tables_runtime or args.all:
        write_runtime_tables(shell)
    if args.rtk or args.all:
        write_rtk_goldens(shell, args.reference_common)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
