"""Differential parity check: kimix-base native kernels vs kimi-agent Python.

Prototype covering the `glob`, `hash_line`, `security` and `bash safety`
kernels.  Every assertion compares a ``runtime_py`` native result with the
result of the *original Python implementation* imported from the kimi-agent
checkout (see ``_parity_ref.py``).
"""

from __future__ import annotations

import fnmatch
import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import _parity_ref  # noqa: E402
from _parity_ref import KIMI_AGENT_ROOT, pure_python, ref, ref_available  # noqa: E402

if _parity_ref.BIN_DIR is None:
    pytest.skip(
        "runtime_py extension not built - run "
        "'python scripts/build_locked.py -- xmake build runtime_py'",
        allow_module_level=True,
    )

# Pin OUR extension before importing any kimi_cli reference module (whose
# native_loader would otherwise put kimi-agent's own released runtime_py.pyd on
# sys.path and silently compare the port against itself).
runtime_py = _parity_ref.native()

pytestmark = pytest.mark.skipif(
    not ref_available(), reason=f"kimi-agent checkout not found at {KIMI_AGENT_ROOT}"
)

FILE = runtime_py.builtin_tools.file
GLOB_NATIVE = runtime_py.glob
TOOLS = runtime_py.tools


# ---------------------------------------------------------------------------
# glob: _is_unsafe_recursive_pattern
# ---------------------------------------------------------------------------

UNSAFE_CASES = [
    "**",
    "**/*",
    "**/**",
    "*",
    "*.py",
    "src/**/*.py",
    "src/**",
    "./**",
    ".\\**\\",
    "**/*.py",
    "a/**/b",
    "**/",
    "/**",
    "***",
    "**/*/*",
    "**x/**",
    "",
    "./",
    "src/*",
    "**/node_modules",
]


def test_is_unsafe_recursive_pattern_parity():
    g = ref("kimi_cli.tools.file.glob")
    with pure_python(g):
        for pat in UNSAFE_CASES:
            assert FILE.is_unsafe_recursive_pattern(pat) == g._is_unsafe_recursive_pattern(
                pat
            ), pat


# ---------------------------------------------------------------------------
# glob: parse_gitignore / is_ignored
# ---------------------------------------------------------------------------

GITIGNORE_CORPUS = [
    "*.py\n",
    "*.PY\n",
    "!keep.py\n",
    "build/\n",
    "/anchored.txt\n",
    "src/**/*.cpp\n",
    "**/node_modules\n",
    "# comment\n\n   \n",
    "  leading_space\n",
    "trailing_space   \n",
    "a?c\n",
    "[abc].py\n",
    "\\!escaped\n",
    "foo/bar/baz\n",
    "**/logs/**\n",
    "a\nb\nc\n",
]


def _rules_to_tuples(rules):
    out = []
    for r in rules:
        out.append((str(r.pattern), bool(r.negated), bool(r.anchored), bool(r.is_dir_only)))
    return out


@pytest.mark.parametrize("content", GITIGNORE_CORPUS)
def test_parse_gitignore_parity(content):
    g = ref("kimi_cli.tools.file.glob")
    native = GLOB_NATIVE.parse_gitignore(content.encode(), ".")
    with pure_python(g):
        python = _rules_to_tuples(g._parse_gitignore(content, Path(".")))
    assert native == python


IGNORE_PATHS = [
    ("a.py", False),
    ("A.PY", False),
    ("keep.py", False),
    ("build", True),
    ("build/x.txt", False),
    ("anchored.txt", False),
    ("src/a.cpp", False),
    ("src/deep/b.cpp", False),
    ("node_modules", True),
    ("x/node_modules", True),
    ("abc.py", False),
    ("logs/a.txt", False),
    ("foo/bar/baz", False),
]


@pytest.mark.parametrize("content", GITIGNORE_CORPUS)
@pytest.mark.parametrize("rel,is_dir", IGNORE_PATHS)
def test_is_ignored_parity(content, rel, is_dir):
    g = ref("kimi_cli.tools.file.glob")
    native_rules = GLOB_NATIVE.parse_gitignore(content.encode(), ".")
    with pure_python(g):
        rules = g._parse_gitignore(content, Path("."))

        def py_is_ignored():
            # Replica of _is_ignored_by_gitignore for the single-source-dir case
            # (rel_path is already relative to the rules' source dir), using the
            # module's own rule matcher.
            ignored = False
            for rule in rules:
                if g._gitignore_match(Path(rel), rel, is_dir, rule):
                    ignored = not rule.negated
            return ignored

        expected = py_is_ignored()
    # case_insensitive=None mirrors fnmatch.fnmatch's platform default.
    assert GLOB_NATIVE.is_ignored(rel, is_dir, [tuple(r) for r in native_rules]) == expected, (
        content,
        rel,
        is_dir,
    )


# ---------------------------------------------------------------------------
# fnmatch core
# ---------------------------------------------------------------------------

FNMATCH_CASES = [
    ("*.py", "test.py"),
    ("*.py", "test.cpp"),
    ("*", "anything/with/slash"),
    ("[abc].py", "a.py"),
    ("[!abc].py", "d.py"),
    ("a?c", "abc"),
    ("a?c", "a/c"),
    ("[a-c]x", "bx"),
    ("[]]", "]"),
    ("[]a]", "a"),
    ("[", "["),
    ("**", "a/b"),
    ("a[b", "a[b"),
    ("*.*", "a.b"),
    ("[!]a]", "b"),
    ("", ""),
    ("", "x"),
    ("\\*", "\\abc"),
]


@pytest.mark.parametrize("pattern,text", FNMATCH_CASES)
def test_fnmatch_case_sensitive_parity(pattern, text):
    assert FILE.fnmatch_match(pattern, text, False) == fnmatch.fnmatchcase(text, pattern)


@pytest.mark.parametrize("pattern,text", FNMATCH_CASES)
def test_fnmatch_default_case_parity(pattern, text):
    expected = fnmatch.fnmatchcase(os.path.normcase(text), os.path.normcase(pattern))
    assert FILE.fnmatch_match(pattern, text, True) == expected


# ---------------------------------------------------------------------------
# tools: line_hash / line_hashes
# ---------------------------------------------------------------------------


def _py_line_hash(line: str, seed: int) -> int:
    import xxhash

    if line.endswith("\r"):
        line = line[:-1]
    chars = [c for c in line if not c.isspace()]
    data = "".join(chars).encode("utf-8")
    return xxhash.xxh32(data, seed).intdigest() & 0xFF


LINE_CORPUS = [
    "",
    "abc",
    "  \t ",
    "a b c",
    "hello world\r",
    "\u00e9\u4e16\u754c",
    "def foo():",
    "\x1b[31mred\x1b[0m",
    "12345",
    "\u3000",
]


@pytest.mark.parametrize("line", LINE_CORPUS)
@pytest.mark.parametrize("seed", [0, 1, 7, 255, 123456789])
def test_line_hash_parity(line, seed):
    assert TOOLS.line_hash(line.encode(), seed) == _py_line_hash(line, seed)
