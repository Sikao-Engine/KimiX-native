"""Case-sensitivity parity tests for kimix_native.glob.

Plan: the Glob tool must be case-insensitive on Windows (filesystem semantics)
while staying case-sensitive on POSIX.  The native kernels and the pure-Python
``_compat`` mirrors both implement ``fnmatch.fnmatch`` platform semantics
(case-insensitive on Windows via os.path.normcase, case-sensitive on POSIX)
with an explicit ``case_insensitive`` override.

Coverage:
- parity: native is_ignored / filter_paths vs the _compat mirrors for
  case_insensitive=None/True/False across a case matrix (wildcard, anchored
  **, bracket, negation, dir-only, **-suffix rules)
- explicit-flag unit tests on _compat_gitignore_match / _match
- platform default: _match(None) is case-insensitive on Windows and
  case-sensitive on POSIX
"""

from __future__ import annotations

import os

import pytest

from kimix_native import glob as G

RULES = {
    "ext": G.parse_gitignore(b"*.PY\n", ""),  # unanchored wildcard
    "anchored": G.parse_gitignore(b"SRC/**/*.CPP\n", ""),  # ** anchored
    "bracket": G.parse_gitignore(b"[A-Z]a?\n", ""),  # character class
    "negation": G.parse_gitignore(b"*.LOG\n!IMPORTANT.LOG\n", ""),  # negation
    "dir_only": G.parse_gitignore(b"BUILD/\n", ""),  # dir-only descendants
    "ds_suffix": G.parse_gitignore(b"**/NODE_MODULES\n", ""),  # **/ suffix
}

# (rule-name, rel_path, is_dir) — expected values below are for the
# case-insensitive flag; the parity assertion only requires native == compat.
CASES = [
    ("ext", "a.py", False),
    ("ext", "A.PY", False),
    ("ext", "a.PY", False),
    ("ext", "b.txt", False),
    ("anchored", "src/a.cpp", False),
    ("anchored", "src/sub/deep.cpp", False),
    ("anchored", "SRC/a.CPP", False),
    ("anchored", "other/a.cpp", False),
    ("bracket", "Aa1", False),
    ("bracket", "aa1", False),
    ("bracket", "za1", False),
    ("negation", "debug.log", False),
    ("negation", "important.log", False),
    ("negation", "IMPORTANT.LOG", False),
    ("dir_only", "build", True),
    ("dir_only", "build/x.py", False),
    ("ds_suffix", "node_modules", True),
    ("ds_suffix", "src/node_modules", True),
    ("ds_suffix", "node_modules_summary.txt", False),
]


def _case_insensitive_default() -> bool:
    return os.name == "nt"


# ---------------------------------------------------------------------------
# _match helper
# ---------------------------------------------------------------------------


def test_match_helper_explicit_flags():
    assert G._match("*.PY", "a.py", True)
    assert not G._match("*.PY", "a.py", False)
    assert G._match("*.PY", "A.PY", False)  # exact-case match still works
    assert G._match("A?.PY", "Ab.py", True)
    assert not G._match("A?.PY", "Ab.py", False)
    assert G._match("[A-Z]*", "abc.py", True)
    assert not G._match("[A-Z]*", "abc.py", False)


def test_match_helper_platform_default():
    assert G._match("*.PY", "a.py", None) is _case_insensitive_default()
    # Explicit flag overrides the platform default in both directions.
    assert G._match("*.PY", "a.py", True)
    assert not G._match("*.PY", "a.py", False)


# ---------------------------------------------------------------------------
# _compat mirror: explicit flag + platform default
# ---------------------------------------------------------------------------


def test_compat_case_insensitive_flag():
    rules = G._compat_parse_gitignore(b"*.PYC\n", "")
    assert G._compat_is_ignored("a.pyc", False, rules, True)
    assert G._compat_is_ignored("src/a.pyc", False, rules, True)
    assert not G._compat_is_ignored("a.pyc", False, rules, False)

    rules = G._compat_parse_gitignore(b"SRC/*.CPP\n", "")
    assert G._compat_is_ignored("src/a.cpp", False, rules, True)
    assert not G._compat_is_ignored("src/a.cpp", False, rules, False)

    rules = G._compat_parse_gitignore(b"[A-Z]*\n", "")
    assert G._compat_is_ignored("abc.py", False, rules, True)
    assert not G._compat_is_ignored("abc.py", False, rules, False)


def test_compat_platform_default():
    rules = G._compat_parse_gitignore(b"*.PY\n", "")
    assert G._compat_is_ignored("a.py", False, rules, None) is _case_insensitive_default()


def test_compat_dir_only_case_flag():
    rules = G._compat_parse_gitignore(b"BUILD/\n", "")
    assert G._compat_is_ignored("build/x.py", False, rules, True)
    assert not G._compat_is_ignored("build/x.py", False, rules, False)
    assert G._compat_is_ignored("BUILD", True, rules, False)  # exact case


def test_compat_double_star_case_flag():
    rules = G._compat_parse_gitignore(b"SRC/**/*.CPP\n", "")
    # Suffix glob segments are case-folded; the anchored prefix stays literal
    # (the **-pattern structure mirrors the reference implementation exactly,
    # and native + compat agree bit-for-bit on this).
    assert G._compat_is_ignored("SRC/sub/deep.cpp", False, rules, True)
    assert G._compat_is_ignored("SRC/sub/deep.CPP", False, rules, False)
    assert not G._compat_is_ignored("SRC/sub/deep.cpp", False, rules, False)
    assert not G._compat_is_ignored("src/sub/deep.cpp", False, rules, True)

    rules = G._compat_parse_gitignore(b"**/NODE_MODULES\n", "")
    assert G._compat_is_ignored("a/node_modules", True, rules, True)
    assert not G._compat_is_ignored("a/node_modules", True, rules, False)
    assert not G._compat_is_ignored("node_modules_summary.txt", False, rules, True)


def test_compat_negation_case_flag():
    rules = G._compat_parse_gitignore(b"*.LOG\n!IMPORTANT.LOG\n", "")
    assert G._compat_is_ignored("debug.log", False, rules, True)
    assert not G._compat_is_ignored("important.log", False, rules, True)
    assert not G._compat_is_ignored("important.log", False, rules, False)
    assert not G._compat_is_ignored("IMPORTANT.LOG", False, rules, False)


def test_compat_filter_paths_flag():
    rules = G._compat_parse_gitignore(b"*.pyc\n", "")
    paths = ["a.pyc", "a.PYC", "keep.py"]
    dirs = [False, False, False]
    assert G._compat_filter_paths(paths, dirs, rules, True) == [True, True, False]
    assert G._compat_filter_paths(paths, dirs, rules, False) == [True, False, False]


# ---------------------------------------------------------------------------
# Public wrappers: explicit flag + platform default
# ---------------------------------------------------------------------------


def test_is_ignored_explicit_flags():
    rules = RULES["ext"]  # *.PY
    assert G.is_ignored("a.py", False, rules, True)
    assert G.is_ignored("a.py", False, rules, None) is _case_insensitive_default()
    assert not G.is_ignored("a.py", False, rules, False)
    assert G.is_ignored("A.PY", False, rules, False)


def test_filter_paths_explicit_flags():
    rules = RULES["ext"]
    paths = ["a.py", "A.PY", "b.txt", "src/a.PY"]
    dirs = [False, False, False, False]
    ci = G.filter_paths(paths, dirs, rules, True)
    cs = G.filter_paths(paths, dirs, rules, False)
    assert ci == [True, True, False, True]
    assert cs == [False, True, False, True]
    # Platform default matches the platform-appropriate explicit flag.
    assert G.filter_paths(paths, dirs, rules, None) == (
        ci if _case_insensitive_default() else cs
    )


# ---------------------------------------------------------------------------
# Native vs _compat parity (only when the native kernel is active)
# ---------------------------------------------------------------------------


@pytest.mark.skipif(not G.use_native("GLOB"), reason="native glob not active")
def test_parity_case_matrix():
    for name, rel, is_dir in CASES:
        rules = RULES[name]
        for ci in (None, True, False):
            native = G.is_ignored(rel, is_dir, rules, ci)
            compat = G._compat_is_ignored(rel, is_dir, rules, ci)
            assert native == compat, (
                f"{name} {rel!r} is_dir={is_dir} ci={ci}: "
                f"native={native} compat={compat}"
            )


@pytest.mark.skipif(not G.use_native("GLOB"), reason="native glob not active")
def test_parity_filter_paths():
    rules = RULES["ext"]
    paths = ["a.py", "A.PY", "b.txt", "src/a.PY", "src/deep.BIN"]
    dirs = [False, False, False, False, False]
    for ci in (None, True, False):
        native = G.filter_paths(paths, dirs, rules, ci)
        compat = G._compat_filter_paths(paths, dirs, rules, ci)
        assert native == compat, f"ci={ci}: native={native} compat={compat}"


@pytest.mark.skipif(not G.use_native("GLOB"), reason="native glob not active")
def test_parity_platform_default_matches_native_default():
    # Calling the wrapper without the flag must equal calling it with None
    # (native C++ default when omitted, platform default via the shim).
    rules = RULES["ext"]
    for rel, is_dir in [("a.py", False), ("A.PY", False), ("b.txt", False)]:
        assert G.is_ignored(rel, is_dir, rules) == G.is_ignored(rel, is_dir, rules, None)
