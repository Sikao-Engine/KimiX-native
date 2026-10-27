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


# ---------------------------------------------------------------------------
# src/llm/bash_parser/fallback_data.inc (the native bash_parser kernel)
# ---------------------------------------------------------------------------

BASH_PARSER_DATA_INC = (
    PROJECT_ROOT / "src" / "llm" / "bash_parser" / "fallback_data.inc"
)


def render_bash_parser_data(shell) -> str:
    """Complete fallback tables for the native kimix::llm::bash_parser module.

    Reads the authoritative reference facade (`_shell_compat.py`): every
    `_FALLBACK_BODIES` entry (including the g* GNU aliases and the netcat/htop
    body shares, materialized at reference import time), the runtime-guarded
    `_FALLBACKS` definitions, `_UNSUPPORTED_BODIES` reasons and
    `_FALLBACK_COMMAND_WRAPPERS` kinds.  Nothing is hand-transcribed.
    """
    bodies = shell._FALLBACK_BODIES
    fallbacks = shell._FALLBACKS
    unsupported = getattr(shell, "_UNSUPPORTED_BODIES", {})
    wrappers = getattr(shell, "_FALLBACK_COMMAND_WRAPPERS", {})
    lines = [
        "// GENERATED by scripts/gen_bash_fix_data.py --bash-parser-data from",
        "// kimi-agent bin/kimix_native/_shell_compat.py - DO NOT EDIT BY HAND.",
        "//",
        "// Regenerate: python scripts/gen_bash_fix_data.py --bash-parser-data \\",
        "//     --reference <kimi-agent>/bin/kimix_native/_shell_compat.py",
        "//",
        f"// {len(bodies)} fallback bodies / definitions, "
        f"{len(unsupported)} unsupported reasons, "
        f"{len(wrappers)} fallback command wrappers.",
        "// Included from bp_fallback.cpp only.",
        "",
        "namespace src_llm_bash_parser_fallback_data {",
        "",
        "struct bp_fallback_entry {",
        "    const char *name;",
        "    const char *body;       // _FALLBACK_BODIES[name]",
        "    const char *definition; // _FALLBACKS[name] (guarded definition)",
        "};",
        "",
        "// Reference insertion order (dict order) preserved: the prelude and",
        "// prefix generators iterate names in this order.",
        "const bp_fallback_entry k_bp_fallback_entries[] = {",
    ]
    for name in bodies:
        lines.append("    {{{}, {}, {}}},".format(
            c_string(name),
            c_string(bodies[name]),
            c_string(fallbacks[name]),
        ))
    lines.append("};")
    lines.append("")
    lines.append("struct bp_unsupported_entry {")
    lines.append("    const char *name;")
    lines.append("    const char *reason;")
    lines.append("};")
    lines.append("")
    lines.append("const bp_unsupported_entry k_bp_unsupported_entries[] = {")
    for name in unsupported:
        lines.append("    {{{}, {}}},".format(
            c_string(name), c_string(unsupported[name])))
    lines.append("};")
    lines.append("")
    lines.append("struct bp_wrapper_kind_entry {")
    lines.append("    const char *name;")
    lines.append("    const char *kind;")
    lines.append("};")
    lines.append("")
    lines.append("const bp_wrapper_kind_entry k_bp_fallback_command_wrappers[] = {")
    for name, kind in wrappers.items():
        lines.append("    {{{}, {}}},".format(c_string(name), c_string(kind)))
    lines.append("};")
    lines.append("")
    lines.append("} // namespace src_llm_bash_parser_fallback_data")
    lines.append("using namespace src_llm_bash_parser_fallback_data;")
    lines.append("")
    return "\n".join(lines)


def write_bash_parser_data(shell) -> None:
    write_text_lf(BASH_PARSER_DATA_INC, render_bash_parser_data(shell))
    print(f"bash_parser data: {BASH_PARSER_DATA_INC} "
          f"({BASH_PARSER_DATA_INC.stat().st_size} bytes)")


# ---------------------------------------------------------------------------
# tests/unit/llm/bash_parser_goldens.inc (differential goldens vs. the
# authoritative reference kernel)
# ---------------------------------------------------------------------------

BASH_PARSER_GOLDENS_INC = (
    PROJECT_ROOT / "tests" / "unit" / "llm" / "bash_parser_goldens.inc"
)

#: Extra hand-picked differential vectors (robustness / malformed / nesting)
#: appended to the AST-extracted corpus; each row goes through the same
#: reference kernel.  Written with explicit escapes (this file is exec'd as
#: Python source, so the escapes are real control characters).
BASH_PARSER_EXTRA_CASES = [
    "", " ", "\n", "echo \\", "rev '", 'rev "', "echo $(rev", "echo `rev",
    "cat <<EOF\nunterminated", "\x00rev\x00", "if rev; then", "case x in rev)",
    '"', "'", "`", "$(", "${", "((", ";;", "&&&", "|||",
    "echo hi > nul", "echo hi > NUL", "echo hi > \\nul", "echo hi > n\\ul",
    "echo hi > $'nul'", "echo hi > nul''", "echo hi > 'nul'", 'echo hi > "nul"',
    "echo hi < nul", "echo hi 2> nul", "echo hi &> nul", "echo hi >> nul",
    "echo hi > $(echo nul)",
    "cd D:\\x", "cd \\\\server\\share", "cd /c/dev", "cd /tmp/x", "cd ~\\Desktop",
    "cd .\\build\\dist", "ls D:\\*.txt", "grep foo D:\\x\\f.txt",
    "cd /tmp/my dir", "echo /c/", "cat /tmpdir/x",
    "rev", "tree", "htop", "journalctl", "sudo rev", "watch -n1 rev",
    "env bash rev", "timeout 5 rev", "xargs rev", "eval 'rev'", "trap 'rev' EXIT",
    "bash -c 'rev'", "sh -c 'rev'", "bash cd /c/x && grep foo",
    "find . -exec sh -c 'rev' \\;", "nohup rev &",
    "@echo off", "@echo on", "@dir", "@@x", "@echo.", "goto x", ":label",
    "call x.bat", "call :label", "exit /b",
    "if exist D:\\x (echo a)", "if not exist x (a) else (b)",
    "if defined V (echo a)", "if errorlevel 1 (echo a)",
    "if a==b (echo x)", "if 5 geq 3 (echo x)", "if /i A==a (echo x)",
    "for %i in (a b) do echo %i", "for %%i in (a,b) do echo %%i",
    "for %i in (D:\\x) do echo %i",
    "cat <<E\nhi\nE", "cat <<E\nhi\nE\n&& echo x\n", "cat <<-E\n\thi\n\tE",
    "echo " + "$(echo " * 250 + "$(rev)" + ")" * 250,
    "echo " + "$(" * 2000 + "pwd" + ")" * 2000,
    "cd " + "$(cd " * 50 + "D:\\x" + ")" * 50,
    "; ".join(["rev"] * 2000),
    "printf x " + "argument " * 2000,
    'eval "rev $HOME"', 'bash -c "rev $HOME"', 'watch "rev <<< $x"',
    "diff <(rev a) <(rev b)", "echo $((1+2))", "echo ${x:-y}",
    'echo "$(rev) x"', "echo `rev` ; wget",
]


def _extract_reference_corpus(reference_root: Path) -> list:
    """AST-extract every string literal passed to fix_bash_command /
    _process_unquoted / pytest.mark.parametrize in the reference Bash test
    suite (the plan's "every applicable reference test vector")."""
    import ast

    files = [
        "tests/test_bash.py",
        "tests/unit/tools/test_bash_paths_ir.py",
        "tests/unit/tools/test_bash_nul_leak.py",
        "tests/unit/tools/test_bash_wrappers_ir.py",
        "tests/unit/tools/test_bash_unquoted_ir.py",
        "tests/unit/tools/test_bash_cmd_at_prefix.py",
        "tests/unit/tools/test_bash_cmd_conditional.py",
        "tests/unit/tools/test_bash_cmd_control_flow_refusals.py",
        "tests/unit/tools/test_bash_cmd_for.py",
        "tests/unit/tools/test_bash_cmd_if_variants.py",
        "tests/unit/tools/test_bash_lexer.py",
        "tests/unit/tools/test_bash_shell_ir.py",
    ]
    cmds: list = []
    seen = set()

    def add(value) -> None:
        if not isinstance(value, str) or value in seen:
            return
        if len(value) <= 4000 and (not value or any(ch.isalpha() for ch in value)):
            seen.add(value)
            cmds.append(value)

    for rel in files:
        path = reference_root / rel
        if not path.is_file():
            continue
        try:
            tree = ast.parse(path.read_text(encoding="utf-8"))
        except SyntaxError:
            continue
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call):
                continue
            fn = node.func
            name = getattr(fn, "id", None) or getattr(fn, "attr", None)
            if name in ("fix_bash_command", "_fix_for_windows",
                        "_process_unquoted", "process_unquoted"):
                for arg in node.args:
                    if isinstance(arg, ast.Constant):
                        add(arg.value)
            elif name == "parametrize" and len(node.args) >= 2:
                for c in ast.walk(node.args[1]):
                    if isinstance(c, ast.Constant):
                        add(c.value)
    return cmds


def render_bash_parser_goldens(shell, corpus: list) -> str:
    """Golden rows: every metadata field of the authoritative
    bash_core.fix_bash_command (fixed temp dir) + the _process_unquoted
    transform for each corpus command."""
    from kimix_native.shell_compat.unquoted import _process_unquoted

    shell._windows_temp_dir = lambda: GOLDEN_TEMP_DIR
    # paths.py resolves the temp dir through
    # bash_core._windows_temp_dir_from_namespace, which prefers the facade
    # module dict ONLY when the facade is registered under its canonical
    # name; otherwise it calls bash_core._windows_temp_dir directly.  Patch
    # the implementation module too so every row is deterministic.
    from kimix_native.shell_compat import bash_core as _bash_core_mod
    _bash_core_mod._windows_temp_dir = lambda: GOLDEN_TEMP_DIR
    rows = []
    for command in corpus:
        result = shell.fix_bash_command(command)
        rows.append((
            command,
            result.command,
            tuple(result.replacements),
            tuple(result.path_changes),
            tuple(result.shell_wrappers),
            tuple(result.nul_fixes),
            tuple(result.unsupported),
            tuple(result.cmd_conditional_fixes),
            bool(result.changed),
            result.warning,
            _process_unquoted(command),
        ))

    lines = [
        "// GENERATED by scripts/gen_bash_fix_data.py --bash-parser-goldens",
        "// from the authoritative reference kernel (kimi-agent",
        "// bin/kimix_native/shell_compat/bash_core.py::fix_bash_command with",
        "// _windows_temp_dir monkeypatched to " + repr(GOLDEN_TEMP_DIR) +
        ") - DO NOT EDIT.",
        "//",
        "// One row per corpus command: the complete BashFix tuple (command,",
        "// all six metadata lists, changed, warning) plus the reference",
        "// _process_unquoted output.  List fields join with \\x1f (SEP).",
        "// " + str(len(rows)) + " rows.",
        "",
        "struct bash_parser_golden {",
        "    const char *command;",
        "    const char *fixed;",
        "    const char *replacements;      // \\x1f-joined",
        "    const char *path_changes;      // \\x1f-joined",
        "    const char *shell_wrappers;    // \\x1f-joined",
        "    const char *nul_fixes;         // \\x1f-joined",
        "    const char *unsupported;       // \\x1f-joined",
        "    const char *cmd_conditional;   // \\x1f-joined",
        "    bool changed;",
        "    const char *warning;",
        "    const char *unquoted;          // process_unquoted(command)",
        "};",
        "",
        "constexpr size_t k_bash_parser_golden_count = " + str(len(rows)) + ";",
        "",
        "const bash_parser_golden k_bash_parser_goldens[] = {",
    ]
    for row in rows:
        lines.append("    {")
        lines.append("        " + c_string(row[0]) + ",")
        lines.append("        " + c_string(row[1]) + ",")
        for field in row[2:8]:
            lines.append("        " + c_string(SEP.join(field)) + ",")
        lines.append("        " + ("true" if row[8] else "false") + ",")
        lines.append("        " + c_string(row[9]) + ",")
        lines.append("        " + c_string(row[10]) + ",")
        lines.append("    },")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def write_bash_parser_goldens(shell, reference_root: Path) -> None:
    corpus = _extract_reference_corpus(reference_root)
    seen = set(corpus)
    for extra in BASH_PARSER_EXTRA_CASES:
        if extra not in seen:
            seen.add(extra)
            corpus.append(extra)
    write_text_lf(BASH_PARSER_GOLDENS_INC,
                  render_bash_parser_goldens(shell, corpus))
    print("bash_parser goldens: " + str(BASH_PARSER_GOLDENS_INC) +
          " (" + str(len(corpus)) + " rows, " +
          str(BASH_PARSER_GOLDENS_INC.stat().st_size) + " bytes)")


# ---------------------------------------------------------------------------
# tests/unit/llm/bash_parser_path_goldens.inc (pure path/nul primitives)
# ---------------------------------------------------------------------------

BASH_PARSER_PATH_GOLDENS_INC = (
    PROJECT_ROOT / "tests" / "unit" / "llm" / "bash_parser_path_goldens.inc"
)

#: Word battery exercising the pure path/null-device primitives
#: (paths.windows_path_replacement / git_bash_abs_path_replacement /
#: path_replacement / normalize_windows_path / quote_path_word /
#: decode_unquoted_word / plausible_path_segments / is_effective_nul_word /
#: quote_virtual_path / decode_virtual_path_span).  Backslashes are built with
#: chr(92) so this file stays a valid Python literal.
BASH_PARSER_PATH_WORDS = None  # built in render_bash_parser_path_goldens


def _bash_parser_path_words() -> list:
    BS = chr(92)
    return [
        "D:" + BS + "x", "D:" + BS + "kimi-agent" + BS + "kimi-cli",
        "d:" + BS + "tools" + BS + "build", "C:" + BS,
        BS + BS + "server" + BS + "share" + BS + "dir",
        BS + "Users" + BS + "foo", "~" + BS + "Desktop" + BS + "file.txt",
        "." + BS + "build" + BS + "dist", ".." + BS + "src" + BS + "lib",
        ".." + BS + ".." + BS + "repo", "build" + BS + "dist" + BS + "assets",
        "foo" + BS + BS + "bar", "D:" + BS + "temp" + BS + "cache" + BS,
        "ls", "D:" + BS + "*.txt",
        "/tmp/x", "/tmp", "/tmpdir/x", "/c/dev", "/C/x", "/c/", "/", "//x",
        "/tmp/my dir", "plain", "", "a",
        BS + "Program" + BS + " Files" + BS + "x",
        "D:" + BS + "a b" + BS + "c", "/c/Program Files/x",
        "~" + BS + "x", ".." + BS + "x", "." + BS + "x",
        BS + BS + "h" + BS + "s", "e:" + BS,
        "nul", "NUL", BS + "nul", "n" + BS + "ul", "nu" + BS + "l",
        "$'nul'", "nul''", "''nul", 'n""ul', "'nul'", '"nul"',
        "$(echo nul)", "x", BS + "156ul",
        "/tmp/a" + BS + "b", "D:" + BS + "x" + BS + "*.txt",
    ]


def render_bash_parser_path_goldens(shell) -> str:
    """Pure path/nul primitive vectors from the reference (fixed temp dir)."""
    from kimix_native.shell_compat import paths as paths_mod

    shell._windows_temp_dir = lambda: GOLDEN_TEMP_DIR
    paths_mod._windows_temp_dir_from_namespace = lambda: GOLDEN_TEMP_DIR
    shell._windows_temp_dir_from_namespace = lambda: GOLDEN_TEMP_DIR

    # None is encoded as the  sentinel (never a real path value); an
    # empty STRING result stays "" so the two are distinguishable.
    def opt(value) -> str:
        return "" if value is None else value

    words = _bash_parser_path_words()
    rows = []
    for w in words:
        rows.append((
            w,
            opt(paths_mod.windows_path_replacement(w)),
            opt(paths_mod.git_bash_abs_path_replacement(w)),
            opt(paths_mod.path_replacement(w)),
            paths_mod.normalize_windows_path(w),
            paths_mod.quote_path_word(w),
            paths_mod.decode_unquoted_word(w),
            bool(paths_mod.plausible_path_segments(w)),
            bool(paths_mod.is_effective_nul_word(w)),
            opt(paths_mod.quote_virtual_path(w)),
            opt(paths_mod.decode_virtual_path_span(w)),
        ))

    lines = [
        "// GENERATED by scripts/gen_bash_fix_data.py --bash-parser-path-goldens",
        "// from the authoritative reference (kimi-agent shell_compat/paths.py,",
        "// _windows_temp_dir pinned to " + repr(GOLDEN_TEMP_DIR) + ") - DO NOT EDIT.",
        "//",
        "// Pure path/null-device primitive vectors.  An empty string field",
        "//  sentinel = the reference returned None; "" = an empty string result.",
        "// " + str(len(rows)) + " rows.",
        "",
        "struct bash_parser_path_golden {",
        "    const char *word;",
        "    const char *windows_path;      // windows_path_replacement",
        "    const char *git_bash_abs;      // git_bash_abs_path_replacement",
        "    const char *path_replacement;  // path_replacement",
        "    const char *normalize;         // normalize_windows_path",
        "    const char *quote_path_word;   // quote_path_word",
        "    const char *decode_unquoted;   // decode_unquoted_word",
        "    bool plausible_segments;       // plausible_path_segments",
        "    bool effective_nul;            // is_effective_nul_word",
        "    const char *quote_virtual;     // quote_virtual_path",
        "    const char *decode_virtual;    // decode_virtual_path_span",
        "};",
        "",
        "constexpr size_t k_bash_parser_path_golden_count = " + str(len(rows)) + ";",
        "",
        "const bash_parser_path_golden k_bash_parser_path_goldens[] = {",
    ]
    for row in rows:
        lines.append("    {")
        lines.append("        " + c_string(row[0]) + ",")
        lines.append("        " + c_string(row[1]) + ",")
        lines.append("        " + c_string(row[2]) + ",")
        lines.append("        " + c_string(row[3]) + ",")
        lines.append("        " + c_string(row[4]) + ",")
        lines.append("        " + c_string(row[5]) + ",")
        lines.append("        " + c_string(row[6]) + ",")
        lines.append("        " + ("true" if row[7] else "false") + ",")
        lines.append("        " + ("true" if row[8] else "false") + ",")
        lines.append("        " + c_string(row[9]) + ",")
        lines.append("        " + c_string(row[10]) + ",")
        lines.append("    },")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def write_bash_parser_path_goldens(shell) -> None:
    write_text_lf(BASH_PARSER_PATH_GOLDENS_INC,
                  render_bash_parser_path_goldens(shell))
    print("bash_parser path goldens: " + str(BASH_PARSER_PATH_GOLDENS_INC) +
          " (" + str(BASH_PARSER_PATH_GOLDENS_INC.stat().st_size) + " bytes)")


# ---------------------------------------------------------------------------
# tests/unit/llm/bash_parser_rule_goldens.inc (pure wrapper + cmd rule vectors)
# ---------------------------------------------------------------------------

BASH_PARSER_RULE_GOLDENS_INC = (
    PROJECT_ROOT / "tests" / "unit" / "llm" / "bash_parser_rule_goldens.inc"
)

# Sentinel for a reference None; SEP joins multi-value fields (both also used
# by the bash_parser goldens).
_RULE_NONE = "\x1e"


def render_bash_parser_rule_goldens(shell) -> str:
    """Pure wrapper/cmd rule vectors from the authoritative reference:
    wrappers.wrapper_step / classify_pattern_option / plausible_script_file and
    cmd_translator.cmd_cmp_side / build_cmd_cmp / split_top_eq /
    cmd_if_branch_ok / cmd_for_item_ok / cmd_body_has_bare_ampersand /
    at_boundary_ok.  Each row is (kind, aux, aux2, input, expected)."""
    from kimix_native.shell_compat import cmd_translator as ct
    from kimix_native.shell_compat import wrappers as wrappers_mod
    from kimix_native.shell_compat.wrappers import WrapperState, wrapper_step

    decode = shell._BashFixScanner._literal_word_value

    step_seqs = [
        ("timeout", ["5", "rev"], 1),
        ("timeout", ["-s", "KILL", "5s", "rev"], 1),
        ("env", ["-S", "FOO=1", "rev"], 0),
        ("env", ["--split-string=x", "rev"], 0),
        ("env", ["-u", "FOO", "rev"], 0),
        ("env", ["FOO=1", "rev"], 0),
        ("command", ["-v", "rev"], 0),
        ("command", ["-p", "rev"], 0),
        ("command", ["-pv", "rev"], 0),
        ("sudo", ["-u", "root", "rev"], 0),
        ("sudo", ["--", "rev"], 0),
        ("xargs", ["-I{}", "rev"], 0),
        ("stdbuf", ["-oL", "rev"], 0),
        ("nice", ["-n", "5", "rev"], 0),
        ("watch", ["-n1", "rev"], 0),
        ("exec", ["-a", "name", "rev"], 0),
        ("nohup", ["rev"], 0),
        ("time", ["-o", "f", "rev"], 0),
        ("builtin", ["rev"], 0),
        ("coproc", ["rev"], 0),
    ]
    pattern_opts = [
        ("grep", "-e"), ("grep", "--regexp"), ("grep", "-f"), ("grep", "--file"),
        ("grep", "--regexp=x"), ("grep", "--file=x"), ("grep", "--include=g"),
        ("grep", "--exclude=g"), ("grep", "-A"), ("grep", "-m"), ("grep", "-x"),
        ("rg", "-e"), ("rg", "-f"),
        ("sed", "-e"), ("sed", "--expression"), ("sed", "-f"), ("sed", "--file"),
        ("sed", "--expression=x"), ("sed", "--file=x"), ("sed", "-n"),
        ("awk", "-v"), ("awk", "-F"), ("awk", "--assign"), ("awk", "-f"),
        ("awk", "--file"), ("awk", "--assign=x"), ("awk", "--file=x"), ("awk", "-W"),
    ]
    script_files = [
        "deploy.sh", "./x", "../x", ".\\x", "..\\x", "/x", "a/b", "rev", "x.bash",
        "x.zsh", "x.ksh", "x.dash", "x.ash", "x.bats", "X.SH", "x.txt", "-", "",
    ]
    cmp_sides = [
        ("%FOO%", False, False), ("%FOO%", True, False), ("%FOO%", False, True),
        ("%errorlevel%", False, False), ("%ERRORLEVEL%", False, False),
        ('"abc"', False, False), ('"abc"', True, False), ("abc", False, False),
        ("5", False, True), ("-5", False, True), ("x", False, True),
        ("%~ni", False, False), ("%partial%x", False, False), ("", False, False),
        ('a"b', False, False), ("a$b", False, False), ('"a""b"', False, False),
    ]
    build_cmps = [
        ("a", "b", "==", False, False), ("5", "3", "geq", False, True),
        ("%X%", "%Y%", "==", False, False), ("%X%", "%Y%", "==", True, False),
        ('"a"', "b", "==", False, False), ("a", '"b"', "==", False, False),
        ('"a"', '"b"', "==", False, False), ("x", "y", "neq", False, True),
    ]
    split_eqs = ["a==b", "a==b==c", '"a==b"==c', "noequals", "a=", "==b", 'x"a==b"y==z']
    branch_ok = ["echo a", "echo a/nb", "", "   ", "echo # c", "echo '#' c",
                 "a 'b#c' d", 'a "b#c" d', "rev <<< x"]
    for_items = ["a", "a,b", '"a"', "a'", "'a", "a$b", "a`b", "a!b", "a%b",
                 "a^b", "*", "a*b", "D:x"]
    bare_amp = ["a & b", "a && b", "a || b", "a 'x & y' b", "a\\ & b", "&",
                "&&", "a|b"]
    at_boundary = [("@echo off", 9, 9), ("@echo off && x", 9, 14),
                   ("@echo off & x", 9, 13), ("@echo off; x", 9, 12),
                   ("@echo off | x", 9, 13), ("@echo offline", 9, 13),
                   ("@echo off\n", 9, 10)]

    def opt(value):
        return _RULE_NONE if value is None else value

    rows = []
    for kind, words, operands in step_seqs:
        st = WrapperState(kind=kind, operands=operands)
        fields = []
        for w in words:
            a = wrapper_step(st, w, decode)
            fields.append("%s,%d,%d,%d,%d" % (
                a, int(st.skip_next), int(st.opaque), int(st.path_value),
                st.operands))
        rows.append(("step", kind, str(operands), SEP.join(words),
                     SEP.join(fields)))
    for fam, w in pattern_opts:
        rows.append(("pattern", fam, "", w,
                     opt(wrappers_mod.classify_pattern_option(fam, w))))
    for f in script_files:
        rows.append(("script", "", "", f,
                     "1" if wrappers_mod.plausible_script_file(f) else "0"))
    for raw, folded, numeric in cmp_sides:
        r = ct.cmd_cmp_side(raw, folded=folded, numeric=numeric)
        enc = _RULE_NONE if r is None else "%s,%d" % (r[0], int(r[1]))
        rows.append(("cmp_side", str(int(folded)), str(int(numeric)), raw, enc))
    for lhs, rhs, op, folded, numeric in build_cmps:
        r = ct.build_cmd_cmp(lhs, rhs, op, folded=folded, numeric=numeric)
        rows.append(("build_cmp", "%d,%d" % (int(folded), int(numeric)), op,
                     lhs + SEP + rhs, opt(r)))
    for s in split_eqs:
        r = ct.split_top_eq(s)
        enc = _RULE_NONE if r is None else r[0] + SEP + r[1]
        rows.append(("split_eq", "", "", s, enc))
    for b in branch_ok:
        rows.append(("branch_ok", "", "", b,
                     "1" if ct.cmd_if_branch_ok(b, 0, len(b)) else "0"))
    for it in for_items:
        rows.append(("for_item", "", "", it,
                     "1" if ct.cmd_for_item_ok(it) else "0"))
    for bd in bare_amp:
        rows.append(("bare_amp", "", "", bd,
                     "1" if ct.cmd_body_has_bare_ampersand(bd) else "0"))
    for src_s, pos, end in at_boundary:
        rows.append(("at_boundary", str(pos), str(end), src_s,
                     "1" if ct.at_boundary_ok(src_s, pos, end) else "0"))

    lines = [
        "// GENERATED by scripts/gen_bash_fix_data.py --bash-parser-rule-goldens",
        "// from the authoritative reference (kimi-agent shell_compat",
        "// wrappers.py + cmd_translator.py pure functions) - DO NOT EDIT.",
        "//",
        "// kind selects the rule; aux/aux2 carry per-kind scalars; input is",
        "// the word(s) (\\x1f-joined for multi-word rows); expected is the",
        "// reference result (\\x1e = None).",
        "// " + str(len(rows)) + " rows.",
        "",
        "struct bash_parser_rule_golden {",
        "    const char *kind;",
        "    const char *aux;",
        "    const char *aux2;",
        "    const char *input;",
        "    const char *expected;",
        "};",
        "",
        "constexpr size_t k_bash_parser_rule_golden_count = " + str(len(rows)) + ";",
        "",
        "const bash_parser_rule_golden k_bash_parser_rule_goldens[] = {",
    ]
    for row in rows:
        lines.append("    {" + ", ".join(c_string(f) for f in row) + "},")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def write_bash_parser_rule_goldens(shell) -> None:
    write_text_lf(BASH_PARSER_RULE_GOLDENS_INC,
                  render_bash_parser_rule_goldens(shell))
    print("bash_parser rule goldens: " + str(BASH_PARSER_RULE_GOLDENS_INC) +
          " (" + str(BASH_PARSER_RULE_GOLDENS_INC.stat().st_size) + " bytes)")


# ---------------------------------------------------------------------------
# src/llm/bash_parser/unicode_class.inc (Python str.isalpha/isalnum ranges)
# ---------------------------------------------------------------------------

UNICODE_CLASS_INC = (
    PROJECT_ROOT / "src" / "llm" / "bash_parser" / "unicode_class.inc"
)


def _unicode_ranges(pred) -> list:
    cps = [c for c in range(0x110000) if pred(chr(c))]
    out = []
    if not cps:
        return out
    start = prev = cps[0]
    for c in cps[1:]:
        if c == prev + 1:
            prev = c
        else:
            out.append((start, prev))
            start = prev = c
    out.append((start, prev))
    return out


def render_unicode_class() -> str:
    """Generated Python str.isalpha / str.isalnum codepoint ranges.

    The path rules (paths.py) consult Python `isalpha`/`isalnum` on decoded
    characters; a byte-oriented C++ port must reproduce the SAME Unicode
    classification for full parity (non-ASCII path words).  These tables are
    the authoritative ranges, so the native kernel never falls back to
    locale-dependent C isalpha.  Emitted as sorted (lo, hi) inclusive uint32
    ranges for binary search.
    """
    alpha = _unicode_ranges(lambda ch: ch.isalpha())
    alnum = _unicode_ranges(lambda ch: ch.isalnum())
    lines = [
        "// GENERATED by scripts/gen_bash_fix_data.py --bash-parser-unicode",
        "// Python str.isalpha / str.isalnum codepoint ranges - DO NOT EDIT.",
        "// Sorted inclusive [lo, hi] uint32 ranges for binary search.",
        "// Included from bp_paths.cpp only.",
        "",
        "namespace src_llm_bash_parser_unicode_class {",
        "",
        "struct bp_uc_range { uint32_t lo; uint32_t hi; };",
        "",
        "// str.isalpha: " + str(len(alpha)) + " ranges.",
        "const bp_uc_range k_bp_uc_alpha[] = {",
    ]
    for lo, hi in alpha:
        lines.append("    {0x%X, 0x%X}," % (lo, hi))
    lines.append("};")
    lines.append("")
    lines.append("// str.isalnum: " + str(len(alnum)) + " ranges (a superset of isalpha).")
    lines.append("const bp_uc_range k_bp_uc_alnum[] = {")
    for lo, hi in alnum:
        lines.append("    {0x%X, 0x%X}," % (lo, hi))
    lines.append("};")
    lines.append("")
    lines.append("constexpr size_t k_bp_uc_alpha_count = sizeof(k_bp_uc_alpha) / sizeof(k_bp_uc_alpha[0]);")
    lines.append("constexpr size_t k_bp_uc_alnum_count = sizeof(k_bp_uc_alnum) / sizeof(k_bp_uc_alnum[0]);")
    lines.append("")
    # Python str.upper() deltas for alpha codepoints (the /c/ drive-letter
    # rewrite uses raw[1].upper()): every alpha codepoint whose upper()
    # differs, mapped to the UTF-8 bytes of its upper string (multi-codepoint
    # expansions included, e.g. U+00DF -> "SS").
    upper_entries = [
        (c, chr(c).upper()) for c in range(0x110000)
        if chr(c).isalpha() and chr(c).upper() != chr(c)
    ]
    lines.append("// str.upper() deltas for alpha codepoints (the /c/ drive-letter")
    lines.append("// rewrite): " + str(len(upper_entries)) + " entries, sorted by codepoint.")
    lines.append("struct bp_uc_upper { uint32_t cp; const char *upper_utf8; };")
    lines.append("")
    lines.append("const bp_uc_upper k_bp_uc_upper[] = {")
    for c, u in upper_entries:
        lines.append("    {0x%X, " % c + c_string(u) + "},")
    lines.append("};")
    lines.append("")
    lines.append("constexpr size_t k_bp_uc_upper_count = sizeof(k_bp_uc_upper) / sizeof(k_bp_uc_upper[0]);")
    lines.append("")
    lines.append("} // namespace src_llm_bash_parser_unicode_class")
    lines.append("using namespace src_llm_bash_parser_unicode_class;")
    lines.append("")
    return "\n".join(lines)


def write_unicode_class() -> None:
    write_text_lf(UNICODE_CLASS_INC, render_unicode_class())
    print("bash_parser unicode: " + str(UNICODE_CLASS_INC) +
          " (" + str(UNICODE_CLASS_INC.stat().st_size) + " bytes)")


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
    parser.add_argument("--bash-parser-data", action="store_true",
                        dest="bash_parser_data",
                        help="rewrite src/llm/bash_parser/fallback_data.inc "
                             "(the native bash_parser kernel's fallback tables)")
    parser.add_argument("--bash-parser-goldens", action="store_true",
                        dest="bash_parser_goldens",
                        help="rewrite tests/unit/llm/bash_parser_goldens.inc "
                             "(differential goldens from the reference kernel)")
    parser.add_argument("--reference-root", type=Path,
                        default=Path("D:/kimi-agent"),
                        help="kimi-agent checkout root (its tests/ corpus is "
                             "AST-mined for the bash_parser goldens)")
    parser.add_argument("--bash-parser-path-goldens", action="store_true",
                        dest="bash_parser_path_goldens",
                        help="rewrite tests/unit/llm/bash_parser_path_goldens.inc "
                             "(pure path/nul primitive vectors)")
    parser.add_argument("--bash-parser-rule-goldens", action="store_true",
                        dest="bash_parser_rule_goldens",
                        help="rewrite tests/unit/llm/bash_parser_rule_goldens.inc "
                             "(pure wrapper/cmd rule vectors)")
    parser.add_argument("--bash-parser-unicode", action="store_true",
                        dest="bash_parser_unicode",
                        help="rewrite src/llm/bash_parser/unicode_class.inc "
                             "(Python isalpha/isalnum codepoint ranges)")
    parser.add_argument("--all", action="store_true")
    args = parser.parse_args()
    if not (args.tables_runtime or args.rtk or args.bash_parser_data
            or args.bash_parser_goldens or args.bash_parser_path_goldens
            or args.bash_parser_rule_goldens or args.bash_parser_unicode
            or args.all):
        parser.error("nothing to do: pass --tables-runtime, --rtk, "
                     "--bash-parser-data, --bash-parser-goldens or --all")
    shell = load_reference(args.reference)
    if args.tables_runtime or args.all:
        write_runtime_tables(shell)
    if args.rtk or args.all:
        write_rtk_goldens(shell, args.reference_common)
    if args.bash_parser_data or args.all:
        write_bash_parser_data(shell)
    if args.bash_parser_goldens or args.all:
        write_bash_parser_goldens(shell, args.reference_root)
    if args.bash_parser_path_goldens or args.all:
        write_bash_parser_path_goldens(shell)
    if args.bash_parser_rule_goldens or args.all:
        write_bash_parser_rule_goldens(shell)
    if args.bash_parser_unicode or args.all:
        write_unicode_class()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
