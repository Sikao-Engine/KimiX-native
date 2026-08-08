"""Parity tests for kimix_native.tools grep-pattern kernels (plan: commit
0582e09 "Study from hermes").

Compares the native kernels against the verbatim _compat mirrors over
adversarial patterns (odd/even backslash runs, real newlines, CRLF) and
non-ASCII patterns (compat path, equal by construction).  Gate toggled
in-process via KIMIX_NATIVE_TOOLS=0.

Coverage:
- pattern_has_regex_newline: literal newline + odd/even backslash runs
- multiline_pattern: CRLF normalization, regex-escape rewrite, real-newline
  rewrite (order-safe), identity passthrough
"""

import pytest

from kimix_native import tools as T


def _run_both(fn, *args, monkeypatch):
    monkeypatch.delenv("KIMIX_NATIVE_TOOLS", raising=False)
    native = fn(*args)
    monkeypatch.setenv("KIMIX_NATIVE_TOOLS", "0")
    compat = fn(*args)
    assert native == compat, f"native != compat for {args!r}:\n{native!r}\n{compat!r}"
    return native, compat


PATTERN_CASES = [
    "abc",
    "a\nb",
    "a\\nb",
    "a\\\\nb",
    "a\\\\\\nb",
    "a\nb\\nc",
    "\\n",
    "\\\\n",
    "\\\\\\n",
    "x\\ny",
    "a\r\nb",
    "a\r\nb\\nc",
    "\\\\\\n\\n",
    "a\\\\\\\\nb",  # 4 backslashes + n: even
    "a\\\\\\\\\\nb",  # 5 backslashes + n: odd
    "\\n\\n\\n",
    "\u00e9\\n",  # non-ASCII routes to compat
    "\u00e9\nx",
    "",
]


@pytest.mark.parametrize("pattern", PATTERN_CASES)
def test_has_regex_newline_parity(pattern, monkeypatch):
    native, compat = _run_both(T.pattern_has_regex_newline, pattern, monkeypatch=monkeypatch)
    assert compat == T._compat_pattern_has_regex_newline(pattern)


@pytest.mark.parametrize("pattern", PATTERN_CASES)
def test_multiline_parity(pattern, monkeypatch):
    native, compat = _run_both(T.multiline_pattern, pattern, monkeypatch=monkeypatch)
    assert compat == T._compat_multiline_pattern(pattern)


@pytest.mark.parametrize(
    "pattern,expected",
    [
        ("abc", False),
        ("a\nb", True),
        ("a\\nb", True),
        ("a\\\\nb", False),
        ("a\\\\\\nb", True),
        ("\\\\n", False),
        ("\\\\\\n", True),
        ("", False),
    ],
)
def test_has_regex_newline_goldens(pattern, expected, monkeypatch):
    monkeypatch.delenv("KIMIX_NATIVE_TOOLS", raising=False)
    assert T.pattern_has_regex_newline(pattern) is expected


@pytest.mark.parametrize(
    "pattern,expected",
    [
        ("abc", "abc"),
        ("a\nb", "a\\r?\\nb"),
        ("a\r\nb", "a\\r?\\nb"),
        ("a\\nb", "a\\r?\\nb"),
        ("a\\\\nb", "a\\\\nb"),  # even backslashes: identity
        ("a\\\\\\nb", "a\\r?\\nb"),
        ("a\nb\\nc", "a\\r?\\nb\\r?\\nc"),
        ("", ""),
    ],
)
def test_multiline_goldens(pattern, expected, monkeypatch):
    monkeypatch.delenv("KIMIX_NATIVE_TOOLS", raising=False)
    assert T.multiline_pattern(pattern) == expected
