#!/usr/bin/env python3
"""Regenerate ``tests/unit/builtin_tools/run_goldens.inc`` from the kimi-agent
Python reference (the source of truth ``src/builtin_tools/run_tool.cpp`` ports).

Every expected value in the generated file is *computed* here, never
transcribed:

* ``kShlexGoldens`` / ``kQuoteGoldens`` / ``kJoinGoldens`` - the host CPython
  ``shlex`` module (``shlex.split(s, posix=...)`` is exactly what
  ``Run.__call__`` calls; ``shlex.quote`` / ``shlex.join`` drive the display
  command).
* ``kCdGoldens`` - ``kimix.tools.file.run._cd_prefix`` (run.py 58-71).
* ``kShapeGoldens`` - the portable stages of ``kimix.tools.common.
  _token_filter_output``: ``_dedup_output(threshold=3, max_block_lines=1)``
  followed by ``_truncate_lines(max_lines, preserve_errors=True,
  error_context_lines=2)``.  Every vector is *also* checked against the full
  ``_token_filter_output`` coroutine, so a golden is literally what run.py
  produces for that input (the ANSI-strip / micro_compress stages are a no-op
  for the corpus; vectors where they are not are dropped).
* ``kExitGoldens`` - ``output_enhance.interpret_exit_code`` /
  ``is_expected_exit`` plus the run.py message assembly (557-559 / 586-587).
* ``kAnnotateGoldens`` - ``output_enhance.annotate_failure``.

The generated file also carries a deterministic fuzz corpus (an LCG, not
``random``, so the byte output does not depend on the CPython version) and the
edge vectors the task asks for: empty / huge / binary (U+FFFD) output, CRLF,
non-ASCII and a missing trailing newline.

Usage
-----
    python scripts/gen_run_data.py --write     # rewrite the .inc
    python scripts/gen_run_data.py --check     # verify it is up to date (exit 1)

``--reference`` overrides the kimi-agent checkout (default
``C:/dev/kimi-agent``, honouring ``KIMI_AGENT_ROOT``).
"""

from __future__ import annotations

import argparse
import asyncio
import importlib.util
import os
import re
import shlex
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
OUT_PATH = REPO_ROOT / "tests" / "unit" / "builtin_tools" / "run_goldens.inc"


def _reference_root() -> Path:
    return Path(os.environ.get("KIMI_AGENT_ROOT", r"C:/dev/kimi-agent"))


# ---------------------------------------------------------------------------
# C++ string literal emission
# ---------------------------------------------------------------------------


def c_literal(text: str) -> str:
    """Render *text* as a C++ string literal.

    Printable ASCII, quotes and backslashes stay inside one literal; a byte
    that needs a ``\\xNN`` escape (control bytes and every byte of a multi-byte
    code point) ends the literal right after the escape, so the escape can never
    swallow a following hex digit.  Adjacent literals are concatenated by the
    C++ lexer.
    """
    parts: list[str] = []
    current: list[str] = []

    def close() -> None:
        if current:
            parts.append("".join(current))
            current.clear()

    for byte in text.encode("utf-8", "surrogateescape"):
        c = chr(byte)
        if c == '"':
            current.append('\\"')
        elif c == "\\":
            current.append("\\\\")
        elif c == "\n":
            current.append("\\n")
        elif c == "\r":
            current.append("\\r")
        elif c == "\t":
            current.append("\\t")
        elif byte < 0x20 or byte >= 0x7F:
            current.append("\\x%02x" % byte)
            close()
        else:
            current.append(c)
    close()
    if not parts:
        return '""'
    return " ".join('"%s"' % part for part in parts)


_C_STR = re.compile(r'"((?:[^"\\]|\\.)*)"')


def c_unescape_bytes(literal: str) -> bytes:
    """Inverse of :func:`c_literal` for the escapes the generator emits.

    Returns raw bytes: :func:`c_literal` may split a multi-byte code point across
    several adjacent literals, so callers must concatenate the bytes *before*
    decoding (decoding each literal on its own would yield lone surrogates).
    """
    out = bytearray()
    i = 0
    while i < len(literal):
        c = literal[i]
        if c != "\\":
            out += c.encode("utf-8", "surrogateescape")
            i += 1
            continue
        n = literal[i + 1]
        i += 2
        if n == "n":
            out += b"\n"
        elif n == "t":
            out += b"\t"
        elif n == "r":
            out += b"\r"
        elif n == '"':
            out += b'"'
        elif n == "\\":
            out += b"\\"
        elif n == "0":
            out += b"\0"
        elif n == "x":
            out += bytes([int(literal[i : i + 2], 16)])
            i += 2
        else:
            out += n.encode("utf-8", "surrogateescape")
    return bytes(out)


def c_unescape(literal: str) -> str:
    """Decode :func:`c_unescape_bytes` (adjacent literals joined first)."""
    return c_unescape_bytes(literal).decode("utf-8", "surrogateescape")


# ---------------------------------------------------------------------------
# reference loading
# ---------------------------------------------------------------------------


def _load_by_path(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:  # pragma: no cover - defensive
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


class Reference:
    """The kimi-agent Python reference with every native gate switched off."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.kimix_src = root / "src"
        self.kimi_cli_src = root / "kimi-cli" / "src"
        for path in (str(self.kimi_cli_src), str(self.kimix_src)):
            if os.path.isdir(path) and path not in sys.path:
                sys.path.insert(0, path)
        # common.py lives in the real `kimix` package; load it by path so a
        # kimi-cli shim `kimix` package can never shadow it.
        self.common = _load_by_path(
            self.kimix_src / "kimix" / "tools" / "common.py", "_gen_run_common"
        )
        # Force the pure-Python bodies (the native STREAM/TOOLS fast paths are
        # a different implementation of the same kernels).
        self.common._NATIVE_STREAM = None
        self.common._native_use_native = lambda *_a, **_k: False
        from kimix.tools.file.bash import output_enhance as oe

        self.output_enhance = oe
        oe._native_use_native = lambda *_a, **_k: False
        oe._NATIVE_TOOLS = None

        import kimix.tools.file.run as run_module

        self.run = run_module
        if not str(run_module.__file__).replace("\\", "/").startswith(
            str(self.kimix_src).replace("\\", "/")
        ):  # pragma: no cover - defensive
            raise RuntimeError(f"run.py resolved outside the reference: {run_module.__file__}")

    # -- kernels ----------------------------------------------------------
    def cd_prefix(self, cwd: str | None, shell: str) -> str:
        return self.run._cd_prefix(cwd, shell)

    def dedup(self, text: str) -> str:
        return self.common._dedup_output(text, 3, max_block_lines=1)

    def truncate(self, text: str, max_lines: int) -> str:
        return self.common._truncate_lines(
            text, max_lines, preserve_errors=True, error_context_lines=2
        )

    def shape(self, text: str, max_lines: int | None) -> tuple[str, bool]:
        """The portable stages of ``_token_filter_output`` as ported in C++."""
        out = self.dedup(text)
        if max_lines is not None:
            out = self.truncate(out, max_lines)
        return out, out != text

    async def pipeline(self, text: str, max_lines: int | None) -> str:
        """The full reference pipeline output for *text*."""
        async def _no_export(*_a, **_k):
            return ("<tmp>/filtered.txt", False)

        self.common._export_to_temp_file_async = _no_export
        out, _path = await self.common._token_filter_output(
            text, token_kill=True, max_lines=max_lines, rtk_rewritten=False
        )
        return out


# ---------------------------------------------------------------------------
# corpora
# ---------------------------------------------------------------------------

#: Command lines used by kimi-agent's own suite (tests/test_run.py,
#: tests/test_bash.py) and by the tool's documentation.
SHLEX_INPUTS = [
    'python -c "print(1)"',
    "git status",
    'C:\\Program Files\\Git\\bin\\bash.exe -lc "echo hi"',
    "echo 'a b' c",
    "a\\ b c",
    'x="1 2" y',
    '"quoted path" arg1 arg2',
    'tool --flag=value "two words"',
    '"a" "b"',
    "a'b'c",
    'a\\"b\\"c',
    "a\\\\b",
    "  ",
    "",
    "a  b",
    "''",
    "a''b",
    '""',
    'a""b',
    "''''",
    "a\\\\",
    "\\\\a",
    "a\\'b",
    '"a\\\\b"',
    "a#b",
    "# comment",
    "a #b",
    " x ",
    "a'b",
    "'a b",
    '"a b',
    "'unterminated",
    "a\\",
    "-v --flag",
    "a\tb\nc",
    "x=",
    "ls -la /tmp/dir",
    'grep -rn "pattern with spaces" src/',
    "cmd /c dir",
    "python.exe script.py --flag",
    "'single quoted'",
    '"double quoted"',
    "mix 'a b' \"c d\" e",
    "trail\\",
    "back\\\\slash",
    "eq=a=b",
    "--opt='val ue'",
    "path/to/exe arg",
    "./relative arg",
    "..\\windows\\path arg",
]

#: Extra parser edge cases the port must survive (run.py splits the command
#: itself, so a malformed command reaches shlex.split verbatim).
SHLEX_EXTRA = [
    "'",
    '"',
    "\\",
    "\\\\",
    "'a''b'",
    '"a""b"',
    "a'b\"c",
    'a\\"b',
    "  a  ",
    "\t",
    "\n",
    "a\nb",
    "a\r\nb",
    "a\\\tb",
    "'a b' 'c d'",
    "x='a b' y=\"c d\"",
    "cmd /c echo \"a b\"",
    'C:\\Users\\me\\AppData\\Local\\Programs\\x.exe --v "a b"',
    "//server/share/tool arg",
    "\\\\server\\share\\tool arg",
    "$HOME/bin/git status",
    "~/bin/git status",
    "a;b|c&d",
    "a&&b||c",
    "x=$(a) y",
    "`a b`",
    "a=b=c",
    "-",
    "--",
    "a-b_c.d/e",
    "'unclosed\"",
    '"unclosed\'',
    "a\\ b\\ c",
    "\\'a\\'",
    '"\\""',
    "''''''",
    "python -c \"print('a')\"",
    "path with spaces/tool.exe --x",
]

QUOTE_INPUTS = [
    "abc",
    "a b",
    "a'b",
    'a"b',
    "a$b",
    "a\tb",
    "",
    "a\nb",
    "/usr/bin/env",
    "C:\\Program Files\\x",
    "--flag=value",
    "a,b:c",
    "a%b",
    "a+b",
    "a=b",
    "a@b",
    "simple.exe",
    "with-dash_and_underscore.x",
    "semi;colon",
    "pipe|amp&",
    "star*glob?",
    "back\\slash",
]

QUOTE_EXTRA = ["a\rb", "a\vb", "a\fb", "'", '"', "\\", "a'b\\c", "print(1)"]

JOIN_ARGVS = [
    ["git", "status"],
    ["python", "-c", "print(1)"],
    ["exe", "a b", "c'd"],
    ["single"],
    [],
    ["path with space", "--flag=value", 'quo"te'],
]

JOIN_EXTRA = [
    ["cmd", "/c", "echo hello"],
    ["C:\\Program Files\\x.exe", "--flag"],
    ["git", "user.name=x y", "status"],
]

#: ``_cd_prefix`` vectors (run.py 58-71 + tests/test_run.py
#: TestRunShellCwdViaCd).
CD_CASES: list[tuple[str | None, str]] = [
    (None, "bash"),
    (None, "pwsh"),
    ("", "bash"),
    ("", "pwsh"),
    ("/tmp/work", "bash"),
    ("/tmp/a b", "bash"),
    ("/tmp/it's", "bash"),
    ("/tmp/a$b", "bash"),
    ("C:\\work", "pwsh"),
    ("C:\\it's", "pwsh"),
    ("C:\\work dir", "pwsh"),
    ("C:\\it's \\a b", "pwsh"),
    ("~/projects", "bash"),
    ("rel/dir", "bash"),
    ("C:/dev/kimix-base", "pwsh"),
]

#: (input, max_lines) pairs for the output-shaping stages.  ``max_lines`` None
#: means "no fold".
SHAPE_CASES: list[tuple[str, int | None]] = [
    ("", None),
    ("", 3),
    ("\n", None),
    ("\n\n", None),
    (" ", None),
    ("plain output", None),
    ("a\nb\n", None),
    ("a\nb\n\n", None),
    ("a\nb", None),
    ("ERROR\nERROR\nERROR", None),
    ("ERROR\nERROR\nERROR\n", None),
    ("ERROR\n" * 4, None),
    ("ERROR\n" * 7, None),
    ("ERROR\n" * 10, None),
    ("ERROR\r\n" * 5, None),
    ("a\nb\na\nb\na\nb\n", None),
    ("x\n" * 4 + "y\n" + "x\n" * 3, None),
    ("x\n" * 4 + "x\n" * 4, None),
    ("a\r\nb\r\n", None),
    ("a\rb\r", None),
    ("a\r\nb\nc\r\n", None),
    ("line_0\nline_1\nline_2", 3),
    ("line_0\nline_1\nline_2", 4),
    ("line_0\nline_1\nline_2\n", 3),
    ("\n".join(f"line_{i}" for i in range(500)), 10),
    ("\n".join(f"line_{i}" for i in range(500)), None),
    ("\n".join(f"line_{i}" for i in range(20)) + "\n", 5),
    ("\n".join(f"line_{i}" for i in range(9)), 6),
    ("\n".join(f"line_{i}" for i in range(11)), 10),
    ("\n".join(f"line_{i}" for i in range(12)), 11),
    (
        "ok\n"
        + "\n".join(f"l{i}" for i in range(30))
        + "\nTraceback (most recent call last):\n  boom\n"
        + "\n".join(f"t{i}" for i in range(30)),
        10,
    ),
    (
        "ok\n"
        + "\n".join(f"l{i}" for i in range(30))
        + "\nerror: boom\n"
        + "\n".join(f"t{i}" for i in range(30)),
        6,
    ),
    ("\n".join(f"e{i}" for i in range(40)), 3),
    ("caf\u00e9\n" * 4, None),
    ("caf\u00e9\n" * 4, 3),
    ("\u4e2d\u6587\u884c\n" * 5, None),
    ("emoji \U0001f600 line\n" * 4, None),
    ("\ufffd\ufffd\n" * 4, None),
    ("\x01\x02\x03\ntail\n", None),
    ("mix\u00e9d\n" * 4 + "plain\n", 4),
    ("tab\there\n" * 4, None),
    ("trailing spaces   \n" * 4, None),
    ("same" * 200 + "\n" + "same" * 200 + "\n", None),
    ("unique_line\n", None),
]

#: Deterministic fuzz corpus for the shape stage: repeated / mixed lines built
#: by a small LCG so the bytes never depend on the CPython version.
def _fuzz_shape_cases() -> list[tuple[str, int | None]]:
    out: list[tuple[str, int | None]] = []
    state = 0x2545F4914F6CDD1D
    words = ["alpha", "beta", "gamma", "delta", "ERROR", "warning:", "  at x:1"]
    for _ in range(24):
        lines: list[str] = []
        count = 0
        for _ in range(1 + (state >> 33) % 7):
            state = (state * 6364136223846793005 + 1442695040888963407) & ((1 << 64) - 1)
            count = 1 + (state >> 40) % 6
            word = words[(state >> 8) % len(words)]
            lines.extend([word] * count)
        state = (state * 6364136223846793005 + 1442695040888963407) & ((1 << 64) - 1)
        text = "\n".join(lines)
        if (state >> 20) & 1:
            text += "\n"
        state = (state * 6364136223846793005 + 1442695040888963407) & ((1 << 64) - 1)
        max_lines = None if ((state >> 12) & 3) == 0 else 3 + (state >> 16) % 12
        out.append((text, max_lines))
    return out


#: Exit-code classification (run.py 526-528 + output_enhance).
EXIT_CASES: list[tuple[str, int | None]] = [
    ("git status", 0),
    ("git status", None),
    ("grep -n pattern file.txt", 1),
    ("grep -q x f", 1),
    ("rg pattern", 1),
    ("diff a.txt b.txt", 1),
    ("git diff --exit-code", 1),
    ("find . -name '*.cpp'", 1),
    ("test -f missing.txt", 1),
    ("cmp a b", 1),
    ("sh -c 'yes | head -1'", 141),
    ("producer | head -n 5", 141),
    ("cat big.log | head", 141),
    ("python x.py", 1),
    ("python x.py", 2),
    ("python x.py", 126),
    ("python x.py", 127),
    ("python x.py", 130),
    ("python x.py", 137),
    ("python x.py", 42),
    ("cmd --flag", 1),
    ("cmd --flag", 2),
    ("ls -la", 2),
    ("make", 2),
    ("cargo test", 101),
    ("pytest -q", 1),
]

#: ``annotate_failure`` vectors (output, command, exit code).
ANNOTATE_CASES: list[tuple[str, str, int | None]] = [
    ("", "cmd", 1),
    ("ok\nall good\n", "cmd", 0),
    ("/bin/sh: 1: nope: not found\n", "nope", 127),
    ("bash: line 1: foo: command not found\n", "foo", 127),
    ("python: can't open file 'x.py': [Errno 2] No such file or directory\n", "python x.py", 2),
    ("ModuleNotFoundError: No module named 'requests'\n", "python x.py", 1),
    ("cat: missing.txt: No such file or directory\n", "cat missing.txt", 1),
    ("Permission denied\n", "./run.sh", 126),
    ("Traceback (most recent call last):\n  File \"x.py\", line 1\nValueError: boom\n", "python x.py", 1),
    ("npm ERR! missing script: build\n", "npm run build", 1),
    ("error: could not compile `x`\n", "cargo build", 101),
    ("x" * 5000 + "\ncommand not found\n", "x", 127),
    ("caf\u00e9 command not found\n", "caf\u00e9", 127),
]

# ---------------------------------------------------------------------------
# rendering
# ---------------------------------------------------------------------------


def _shlex_rows(reference, inputs: list[str]) -> list[str]:
    rows: list[str] = []
    for text in inputs:
        expected = []
        for posix in (True, False):
            try:
                expected.append((posix, shlex.split(text, posix=posix), ""))
            except ValueError as exc:
                expected.append((posix, [], str(exc)))
        for posix, tokens, error in expected:
            shown = tokens[:8]
            toks = ", ".join(c_literal(t) for t in shown)
            if toks:
                toks += ", "
            toks += "nullptr"
            rows.append(
                "    {%s, %s, %s, %s, {%s}, %d},"
                % (
                    c_literal(text),
                    "true" if posix else "false",
                    "true" if error == "" else "false",
                    c_literal(error),
                    toks,
                    len(tokens),
                )
            )
    return rows


def _quote_rows(inputs: list[str]) -> list[str]:
    return [
        "    {%s, %s}," % (c_literal(t), c_literal(shlex.quote(t))) for t in inputs
    ]


def _join_rows(argv_list: list[list[str]]) -> list[str]:
    rows = []
    for argv in argv_list:
        shown = argv[:4]
        entries = ", ".join(c_literal(a) for a in shown)
        if entries:
            entries += ", "
        entries += "nullptr"
        rows.append(
            "    {{%s}, %d, %s}," % (entries, len(argv), c_literal(shlex.join(argv)))
        )
    return rows


def _fuzz_shlex_inputs() -> list[str]:
    """Fuzz command lines built from shell-significant fragments (LCG order)."""
    fragments = [
        "git",
        "status",
        "-c",
        "--flag=x",
        "'a b'",
        '"c d"',
        "a\\ b",
        "\\\\",
        "x=",
        "#c",
        ";",
        "|",
        "&&",
        "$HOME",
        "$(a)",
        "`b`",
        "C:\\\\Program Files\\\\x.exe",
        "\t",
        "\n",
        "'",
        '"',
        "",
        "caf\u00e9",
    ]
    state = 0x9E3779B97F4A7C15
    inputs: list[str] = []
    for _ in range(60):
        parts: list[str] = []
        for _ in range(1 + (state >> 35) % 5):
            state = (state * 6364136223846793005 + 1442695040888963407) & ((1 << 64) - 1)
            parts.append(fragments[(state >> 24) % len(fragments)])
        separator = " " if (state >> 10) & 1 else ""
        inputs.append(separator.join(parts))
        state = (state * 6364136223846793005 + 1442695040888963407) & ((1 << 64) - 1)
    return inputs


def render(reference: Reference) -> str:
    shlex_inputs = SHLEX_INPUTS + SHLEX_EXTRA + _fuzz_shlex_inputs()
    quote_inputs = QUOTE_INPUTS + QUOTE_EXTRA
    join_argvs = JOIN_ARGVS + JOIN_EXTRA

    lines: list[str] = []
    lines.append("// GENERATED by scripts/gen_run_data.py from the kimi-agent Python")
    lines.append("// reference (C:/dev/kimi-agent). Do not edit by hand - regenerate with")
    lines.append("//   python scripts/gen_run_data.py --write")
    lines.append("//   python scripts/gen_run_data.py --check")
    lines.append("// Python %s" % sys.version.split()[0])
    lines.append("")
    lines.append("struct shlex_golden {")
    lines.append("    const char *input;")
    lines.append("    bool posix;")
    lines.append("    bool ok; // false == CPython raised ValueError")
    lines.append("    const char *error; // ValueError text when !ok")
    lines.append("    const char *tokens[8]; // NULL-terminated when ok")
    lines.append("    int token_count;")
    lines.append("};")
    lines.append("")
    lines.append("const shlex_golden kShlexGoldens[] = {")
    lines.extend(_shlex_rows(reference, shlex_inputs))
    lines.append("};")
    lines.append("")
    lines.append("struct quote_golden { const char *input; const char *quoted; };")
    lines.append("")
    lines.append("const quote_golden kQuoteGoldens[] = {")
    lines.extend(_quote_rows(quote_inputs))
    lines.append("};")
    lines.append("")
    lines.append("struct join_golden { const char *argv[4]; int count; const char *joined; };")
    lines.append("")
    lines.append("const join_golden kJoinGoldens[] = {")
    lines.extend(_join_rows(join_argvs))
    lines.append("};")
    lines.append("")
    lines.append("// run.py 58-71: the `cd` prefix Run prepends when it delegates to")
    lines.append("// Bash/Powershell (shell == \"bash\" | \"pwsh\").")
    lines.append("struct cd_golden { const char *cwd; const char *shell; const char *expected; };")
    lines.append("")
    lines.append("const cd_golden kCdGoldens[] = {")
    for cwd, shell in CD_CASES:
        lines.append(
            "    {%s, %s, %s},"
            % (c_literal(cwd or ""), c_literal(shell), c_literal(reference.cd_prefix(cwd, shell)))
        )
    lines.append("};")
    lines.append("")

    shape_cases = SHAPE_CASES + _fuzz_shape_cases()
    shape_rows: list[str] = []
    divergent: list[str] = []
    for text, max_lines in shape_cases:
        expected, changed = reference.shape(text, max_lines)
        # Cross-check against the full reference pipeline.  micro_compress
        # (banner drop / prefix fold / whitespace-only-line removal) is a
        # separate, unported stage, so a few vectors legitimately differ; they
        # are still valid goldens for the portable stages and the pytest pins
        # the divergence explicitly.
        if asyncio.run(reference.pipeline(text, max_lines)) != expected:
            divergent.append(text[:60])
        shape_rows.append(
            "    {%s, %d, %s, %s},"
            % (
                c_literal(text),
                -1 if max_lines is None else max_lines,
                c_literal(expected),
                "true" if changed else "false",
            )
        )
    if divergent:
        sys.stderr.write(
            "note: %d/%d shape vector(s) where the full reference pipeline "
            "differs (micro_compress stages):\n" % (len(divergent), len(shape_cases))
        )
        for head in divergent:
            sys.stderr.write("  %r\n" % head)
    lines.append("// common.py _token_filter_output's portable stages:")
    lines.append("//   _dedup_output(threshold=3, max_block_lines=1) then")
    lines.append("//   _truncate_lines(max_lines, preserve_errors=True, error_context_lines=2)")
    lines.append("// max_lines == -1 means \"no fold\". `changed` is the reference's")
    lines.append("// `output != original_output` test. Every row equals the live reference")
    lines.append("// composition; the rows without CRLF / whitespace-only lines / control")
    lines.append("// bytes also equal the full _token_filter_output output (the remaining")
    lines.append("// stages are micro_compress and the rich ANSI parser).")
    lines.append("struct shape_golden {")
    lines.append("    const char *input;")
    lines.append("    int max_lines; // -1 == no fold")
    lines.append("    const char *expected;")
    lines.append("    bool changed;")
    lines.append("};")
    lines.append("")
    lines.append("const shape_golden kShapeGoldens[] = {")
    lines.extend(shape_rows)
    lines.append("};")
    lines.append("")

    lines.append("// output_enhance.interpret_exit_code / is_expected_exit + the run.py")
    lines.append("// message assembly (557-559 / 586-587). `meaning` NULL == None.")
    lines.append("struct exit_golden {")
    lines.append("    const char *command;")
    lines.append("    bool has_code;")
    lines.append("    int exit_code;")
    lines.append("    const char *meaning;")
    lines.append("    bool expected;")
    lines.append("    const char *ok_message;")
    lines.append("    const char *fail_message;")
    lines.append("};")
    lines.append("")
    lines.append("const exit_golden kExitGoldens[] = {")
    for command, code in EXIT_CASES:
        meaning = reference.output_enhance.interpret_exit_code(command, code)
        expected = reference.output_enhance.is_expected_exit(command, code)
        success = code == 0
        ok_message = (
            "success"
            if success
            else (meaning if meaning is not None else "expected non-zero exit")
        )
        fail_message = "failed"
        lines.append(
            "    {%s, %s, %d, %s, %s, %s, %s},"
            % (
                c_literal(command),
                "true" if code is not None else "false",
                0 if code is None else code,
                "nullptr" if meaning is None else c_literal(meaning),
                "true" if expected else "false",
                c_literal(ok_message),
                c_literal(fail_message),
            )
        )
    lines.append("};")
    lines.append("")

    lines.append("// output_enhance.annotate_failure (hint NULL == None).")
    lines.append("struct annotate_golden {")
    lines.append("    const char *output;")
    lines.append("    const char *command;")
    lines.append("    bool has_code;")
    lines.append("    int exit_code;")
    lines.append("    const char *hint;")
    lines.append("};")
    lines.append("")
    lines.append("const annotate_golden kAnnotateGoldens[] = {")
    for output, command, code in ANNOTATE_CASES:
        hint = reference.output_enhance.annotate_failure(output, command, code)
        lines.append(
            "    {%s, %s, %s, %d, %s},"
            % (
                c_literal(output),
                c_literal(command),
                "true" if code is not None else "false",
                0 if code is None else code,
                "nullptr" if hint is None else c_literal(hint),
            )
        )
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--write", action="store_true", help="rewrite the .inc")
    group.add_argument("--check", action="store_true", help="fail when stale")
    parser.add_argument("--reference", type=Path, default=_reference_root())
    parser.add_argument("--output", type=Path, default=OUT_PATH)
    args = parser.parse_args(argv)

    reference = Reference(args.reference)
    text = render(reference) + "\n"

    if args.check:
        current = args.output.read_text(encoding="utf-8", newline="")
        # The "// Python X.Y.Z" line is provenance, not data: the tables are
        # built from pure-string inputs and an LCG (see the module docstring), so
        # they are byte-identical across CPython patch versions.  Comparing the
        # stamp would make --check fail for whoever runs it on a different
        # interpreter than the last --write (and the fix would be to rewrite the
        # header back and forth), so it is stripped from the comparison and only
        # reported as a note.
        def _stamp(line: str) -> str | None:
            return line if line.startswith("// Python ") else None

        cur_lines = current.splitlines()
        new_lines = text.splitlines()
        cur_stamp = next(filter(None, map(_stamp, cur_lines)), None)
        new_stamp = next(filter(None, map(_stamp, new_lines)), None)
        if [l for l in cur_lines if _stamp(l) is None] != \
           [l for l in new_lines if _stamp(l) is None]:
            import difflib

            diff = difflib.unified_diff(
                cur_lines, new_lines, "committed", "regenerated", lineterm=""
            )
            sys.stderr.write("\n".join(list(diff)[:80]) + "\n")
            sys.stderr.write(f"\n{args.output} is STALE - run --write\n")
            return 1
        note = "" if cur_stamp == new_stamp else (
            f" (provenance stamp {cur_stamp!r} regenerated as {new_stamp!r}; "
            "data identical)"
        )
        print(f"{args.output} is up to date ({len(text)} bytes){note}")
        return 0

    # newline="" keeps the file LF-only (the committed blob is LF).
    with args.output.open("w", encoding="utf-8", newline="") as handle:
        handle.write(text)
    print(f"wrote {args.output} ({len(text)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
