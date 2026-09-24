"""Differential parity tests for the edit builtin tool (kimix-base <-> kimi-agent).

The C++ edit kernels live in ``src/builtin_tools/edit_tool.{h,cpp}`` and are a
byte-exact port of kimi-agent's multi-mode edit tool.  The one kernel that is
reachable from Python is ``runtime_py.builtin_tools.file.apply_edit`` (see the
header comment of ``src/runtime/py/py_builtin_file.cpp``)::

    file.apply_edit(content, old_text, new_text, replace_all=False,
                    max_replacements=None, match_mode="fuzzy")
        -> (new_content: str, replacements: int, suggestion: str | None)

Its ground truth is ``ReplaceModeExecutor._apply_edit`` in
``kimi-cli/src/kimi_cli/tools/file/edit/modes/replace.py`` (the body behind
``EditFile._apply_edit`` / ``kimi_cli.tools.file.edit.__init__``).  That module
is pure Python (rapidfuzz only) and has **no** native short-circuit, so it can
be imported and called directly; ``pure_python(...)`` is still wrapped around
every call as a belt-and-braces guard in case kimi-agent grows one.

Coverage
--------
* the exact vectors from kimi-agent's own edit test suite
  (``kimi-cli/tests/tools/test_edit_file.py``, ``test_edit_file_fuzzy.py``),
  asserting BOTH implementations reproduce the reference's expected values;
* a hand-built corpus: multi-line, CRLF/CR/LF mixtures, tabs vs spaces,
  leading/trailing whitespace, unicode (é/ß/emoji/CJK), repeated blocks,
  overlapping and non-overlapping replace_all, max_replacements, empty inputs,
  no-op edits, exact vs fuzzy mode;
* a deterministic (seeded) fuzz loop over three generators: slice/mutation
  near-misses, line-window multi-line targets, and a unicode code-point soup;
* targeted near-cutoff cases (fuzz scores in [74, 76] and around the *.5
  rounding boundary of the ``{score:.0f}`` suggestion text).

Known deviation
---------------
``apply_edit`` enforces a native input cap (10 000 code points / 25 000 000 DP
cells, report deviation #2) and raises ``ValueError`` above it, whereas
rapidfuzz never fails.  That is documented in
``src/builtin_tools/reports/edit.md``; ``test_documented_deviation_fuzz_cap``
pins it as an expected divergence (xfail(strict=True)).

Ground truth is imported from the kimi-agent checkout only -- never from
``python/kimix_native`` (whose mirrors share ancestry with the port).
"""

from __future__ import annotations

import os
import random
import sys
from pathlib import Path

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------------------
# Import the freshly built extension BEFORE anything imports kimi_cli, and make
# sure it is *this* repo's build (a foreign runtime_py.pyd from the kimi-agent
# checkout would silently compare the port against an older release of itself).
# ---------------------------------------------------------------------------
_REPO_ROOT = Path(__file__).resolve().parents[2]
for _mode in ("release", "releasedbg", "debug", "check"):
    _cand = _REPO_ROOT / "bin" / _mode
    if (_cand / "runtime_py.pyd").is_file() or (_cand / "runtime_py.so").is_file():
        _cand_str = str(_cand)
        while _cand_str in sys.path:
            sys.path.remove(_cand_str)
        sys.path.insert(0, _cand_str)
        break

import runtime_py  # noqa: E402

_NATIVE_FILE = Path(runtime_py.__file__).resolve()
if _REPO_ROOT not in _NATIVE_FILE.parents:
    raise RuntimeError(
        f"runtime_py resolved to a foreign build ({_NATIVE_FILE}); the parity "
        f"result would compare the port against an older release of itself. "
        f"Build it first: python scripts/build_locked.py -- xmake build runtime_py"
    )

from _parity_ref import KIMI_AGENT_ROOT, pure_python, ref, ref_available  # noqa: E402

# ---------------------------------------------------------------------------
# Reference (kimi-agent) side
# ---------------------------------------------------------------------------
_REF_ERROR: Exception | None = None
_ref_replace_mod = None
_ref_params_mod = None
_REF_EXECUTOR = None
_RefEditItem = None
try:
    _ref_replace_mod = ref("kimi_cli.tools.file.edit.modes.replace")
    _ref_params_mod = ref("kimi_cli.tools.file.edit.params")
    _REF_EXECUTOR = _ref_replace_mod.ReplaceModeExecutor()
    _RefEditItem = _ref_params_mod.ReplaceEditItem
except Exception as _exc:  # noqa: BLE001 - reported via the skipif below
    _REF_ERROR = _exc

_REF_MODULES = tuple(m for m in (_ref_replace_mod, _ref_params_mod) if m is not None)

pytestmark = pytest.mark.skipif(
    not ref_available() or _REF_ERROR is not None,
    reason=f"kimi-agent edit reference not importable (checkout={KIMI_AGENT_ROOT}, "
           f"error={_REF_ERROR!r})",
)

FILE = runtime_py.builtin_tools.file


def ref_apply_edit(
    content: str,
    old: str,
    new: str,
    *,
    replace_all: bool = False,
    max_replacements: int | None = None,
    match_mode: str = "fuzzy",
):
    """Call the real Python body: ReplaceModeExecutor._apply_edit."""
    with pure_python(*_REF_MODULES):
        item = _RefEditItem(
            old=old,
            new=new,
            replace_all=replace_all,
            max_replacements=max_replacements,
            match_mode=match_mode,
        )
        return _REF_EXECUTOR._apply_edit(content, item)


def native_apply_edit(
    content: str,
    old: str,
    new: str,
    *,
    replace_all: bool = False,
    max_replacements: int | None = None,
    match_mode: str = "fuzzy",
):
    """Call the C++ kernel through the runtime_py extension."""
    return FILE.apply_edit(content, old, new, replace_all, max_replacements, match_mode)


def _outcome(fn, *args, **kwargs):
    """(kind, payload) so raising and returning can be compared uniformly.

    Only the *kind* (ok/raise) is compared for failures: the reference raises
    pydantic ``ValidationError`` where the extension raises ``ValueError``, and
    the exception text is not part of the tool contract.
    """
    try:
        return ("ok", fn(*args, **kwargs))
    except Exception as exc:  # noqa: BLE001 - the reference raises pydantic errors
        return ("raise", f"{type(exc).__name__}: {exc}")


def compare_cases(cases, limit_reported=8):
    """Run every case through both sides, return the list of mismatches.

    ``cases`` is an iterable of ``(label, content, old, new, kwargs)``.
    """
    mismatches = []
    for label, content, old, new, kwargs in cases:
        expected = _outcome(ref_apply_edit, content, old, new, **kwargs)
        actual = _outcome(native_apply_edit, content, old, new, **kwargs)
        same = expected[0] == actual[0] and (
            expected[0] == "raise" or expected[1] == actual[1]
        )
        if not same:
            mismatches.append((label, content, old, new, kwargs, expected, actual))
    if mismatches:
        lines = [f"{len(mismatches)} mismatching case(s); first {min(limit_reported, len(mismatches))}:"]
        for label, content, old, new, kwargs, expected, actual in mismatches[:limit_reported]:
            lines.append(f"  [{label}] content={content!r} old={old!r} new={new!r} kwargs={kwargs!r}")
            lines.append(f"      python: {expected[0]} {expected[1]!r}")
            lines.append(f"      native: {actual[0]} {actual[1]!r}")
        pytest.fail("\n".join(lines), pytrace=False)
    return mismatches


# ===========================================================================
# 0. The reference really is the pure-Python body
# ===========================================================================


def test_reference_module_is_pure_python():
    """The reference must not short-circuit to a native library.

    If replace.py ever gained a ``use_native`` gate, comparing against its
    native branch would only prove that two builds of our own library agree.
    """
    src = Path(_ref_replace_mod.__file__).read_text(encoding="utf-8")
    assert "kimix_native" not in src
    assert "use_native" not in src
    # The body we call is the executor's own method, not a shim.
    assert type(_REF_EXECUTOR).__name__ == "ReplaceModeExecutor"
    assert type(_REF_EXECUTOR)._apply_edit.__qualname__.startswith("ReplaceModeExecutor.")
    # kimi-agent's own test: default match_mode is fuzzy.
    assert _RefEditItem(old="a", new="b").match_mode == "fuzzy"


# ===========================================================================
# 1. Exact-vector corpus (kimi-agent's own test suite values + hand corpus)
# ===========================================================================


def structured_corpus():
    """(label, content, old, new, kwargs) tuples, deterministic order."""
    c = []

    def add(label, content, old, new, **kwargs):
        c.append((label, content, old, new, kwargs))

    # --- vectors lifted from kimi-cli/tests/tools/test_edit_file*.py ---------
    add("ka_exact_single", "Hello world! This is a test.", "world", "universe")
    add("ka_replace_all", "apple banana apple cherry apple", "apple", "fruit",
        replace_all=True)
    add("ka_multiline", "Line 1\nLine 2\nLine 3\n", "Line 2\nLine 3",
        "Modified line 2\nModified line 3")
    add("ka_unicode", "Hello \u4e16\u754c! caf\u00e9", "\u4e16\u754c", "\u5730\u7403")
    add("ka_no_match", "Hello world!", "notfound", "replacement")
    add("ka_fuzzy_crlf_file_lf_old", "line1\r\nline2\r\nline3", "line2\nline3",
        "replaced")
    add("ka_fuzzy_lf_file_crlf_old", "line1\nline2\nline3", "line2\r\nline3",
        "replaced")
    add("ka_replace_all_crlf", "a\r\nb\r\na\r\nc", "a\n", "X\n", replace_all=True)
    add("ka_default_fuzzy_typo", "helo world\nnext line", "hello world",
        "hi universe")
    add("ka_exact_mode_typo", "helo world\nnext line", "hello world",
        "hi universe", match_mode="exact")
    add("ka_fuzzy_trailing_spaces", "hello world  \nnext line", "hello world",
        "hi universe")
    add("ka_fuzzy_leading_spaces", "  hello world\nnext line", "hello world",
        "hi universe")
    add("ka_fuzzy_wording_drift", "def compute_sum(a, b):\n    return a + b",
        "def compute_sum(a, b):", "def add(a, b):")
    add("ka_fuzzy_no_match_no_suggestion", "hello world\nfoo bar\nbaz qux",
        "xyz123_not_close", "replacement")
    add("ka_replace_all_no_match_suggestion", "hello world\nfoo bar",
        "helo wrld", "replacement", replace_all=True)
    add("ka_replace_all_unrelated", "hello world\nfoo bar", "xyz123_not_close",
        "replacement", replace_all=True)
    add("ka_fuzzy_multiline_whitespace", "start\n  line A\n  line B\nend",
        "line A\nline B", "line X\nline Y")
    add("ka_fuzzy_multiline_close", "class Foo:\n    def bar(self):\n        pass\n"
        "    def baz(self):\n        pass",
        "    def bar(self):\n        pass", "    def qux(self):\n        return 42")
    add("ka_max_replacements", "a b a c a d a", "a", "X", replace_all=True,
        max_replacements=2)
    add("ka_find_similar_typo", "hello world\nfoo bar\nbaz qux", "hello wrld",
        "replacement", match_mode="exact")

    # --- empty / no-op / degenerate ----------------------------------------
    add("empty_all", "", "", "")
    add("empty_content", "", "a", "b")
    add("empty_old", "abc", "", "x")
    add("same_old_new", "abc", "a", "a")
    add("same_whole", "abc", "abc", "abc")
    add("new_empty", "abc", "a", "")
    add("whole_file", "abc", "abc", "")
    add("empty_old_multiline", "", "a\nb", "c")
    add("only_newlines", "\n\n\n", "a", "b")
    add("empty_to_old", "abc", "", "abc")

    # --- repeated blocks / overlaps ----------------------------------------
    add("repeat_first_only", "x\nx\nx\ny\n", "x", "z")
    add("repeat_all", "x\nx\nx\ny\n", "x", "z", replace_all=True)
    add("repeat_limited", "ab\nab\nab\n", "ab", "cd", replace_all=True,
        max_replacements=2)
    add("repeat_limited_over", "ab\nab\nab\n", "ab", "cd", replace_all=True,
        max_replacements=5)
    add("overlap_aa_aaa", "aaa", "aa", "b", replace_all=True)
    add("overlap_aa_aaaa", "aaaa", "aa", "b", replace_all=True)
    add("overlap_aa_aaaa_1", "aaaa", "aa", "b", replace_all=True,
        max_replacements=1)
    add("grow_new_contains_old", "aaa", "a", "aa", replace_all=True,
        max_replacements=3)
    add("overlap_aba", "abab", "aba", "X", replace_all=True)
    add("multiline_repeat_first", "hello\nhello\nhello\n", "hello\nhello", "X")
    add("multiline_repeat_all", "hello\nhello\nhello\n", "hello\nhello", "X",
        replace_all=True)
    add("multiline_identical_lines", "l1\nl2\nl2\nl3\n", "l2\nl2", "Q")
    add("multiline_identical_all", "l1\nl2\nl2\nl3\n", "l2\nl2", "Q",
        replace_all=True)

    # --- whitespace: tabs vs spaces, leading/trailing ----------------------
    add("tab_content", "\tfoo bar\n", "foo bar", "baz")
    add("tab_old", "foo\tbar\n", "foo bar", "baz")
    add("trailing_spaces_content", "foo bar   \n", "foo bar", "baz")
    add("trailing_space_in_old", "line with trailing space \nnext\n",
        "line with trailing space ", "L")
    add("indent_fuzzy", "    if x:\n        y()\n", "if x:\n\ty()",
        "if z:\n\ty()")
    add("whitespace_only_lines", "   \n   \n", "   ", "Q")
    add("strip_match_indent", "x\n foo bar\nz\n", "\tfoo bar", "FOO")
    add("strip_match_crlf", "x\r\n foo \r\ny\r\n", "\tfoo", "FOO")
    add("strip_match_cr", "x\r foo\ry\r", "foo\t", "FOO")
    add("blank_lines", "a\n\n\nb\n", "\n\n", "Q")

    # --- line endings -------------------------------------------------------
    add("crlf_single", "a\r\nb\r\nc\r\n", "b", "B")
    add("cr_single", "a\rb\rc\r", "b", "B")
    add("mixed_eol", "a\r\nb\rc\n", "b", "B")
    add("cr_crlf", "a\r\r\nb\r\r\nc", "b", "B")
    add("crlf_content_lf_old", "line1\r\nline2\r\nline3", "line1\nline2", "X")
    add("crlf_repeat_all", "x\r\nx\r\nx\r\n", "x", "y", replace_all=True)
    add("crlf_repeat_multiline", "x\r\nx\r\nx\r\n", "x\nx", "y", replace_all=True)
    add("crlf_fuzzy_unicode_line", "caf\u00e9 au lait\r\nnext\r\n",
        "cafe au lait", "COFFEE")
    add("no_trailing_newline", "no newline at end", "at end", "AT END")
    add("trailing_newline_only", "\n", "\n", "X")
    add("trailing_nl_in_old", "trailing\n", "\n", "X")

    # --- multi-line windows -------------------------------------------------
    lines5 = "l1\nl2\nl3\nl4\nl5\n"
    add("window_exact", lines5, "l2\nl3", "Q")
    add("window_second", lines5, "l3\nl4", "Q")
    add("window_near", lines5, "l2\nlX", "Q")
    add("window_three", lines5, "l2\nl3\nl4", "Q")
    add("window_whole", lines5, "l1\nl2\nl3\nl4\nl5", "Q")
    add("window_tail", lines5, "l4\nl5", "Q")
    add("window_too_long", "l1\nl2\n", "l1\nl2\nl3\nl4", "Q")

    # --- unicode ------------------------------------------------------------
    add("unicode_exact", "\u00e9\u00e9\u00e9\n", "\u00e9", "e")
    add("unicode_all", "\u00e9\u00e9\u00e9\u00e9\n", "\u00e9\u00e9", "e",
        replace_all=True)
    add("unicode_emoji", "\U0001f600\U0001f600\n", "\U0001f600", ":)")
    add("unicode_cjk", "\u4f60\u597d\u4e16\u754c\n", "\u4f60\u597d", "hi")
    add("unicode_section", "\u00a7\u00a7\u00a7\n", "\u00a7", "S")
    add("unicode_fuzzy_accent", "cafe na\u00efve\n", "caf\u00e9", "th\u00e9")
    add("unicode_fuzzy_cafe", "caf\u00e9 au lait\n", "cafe au lait", "COFFEE")
    add("unicode_mixed_fuzzy", "a\u00e9bcdefg\n", "abcdefgh", "Y")
    add("unicode_combining", "e\u0301cole\n", "ecole", "school")

    # --- max_replacements without replace_all (ignored by the reference) ----
    add("max_without_all", "a a a", "a", "b", max_replacements=1)
    add("max_without_all_miss", "a a a", "zz", "b", max_replacements=1)

    return c


_CORPUS = structured_corpus()


@pytest.mark.parametrize(
    "label,content,old,new,kwargs",
    _CORPUS,
    ids=[case[0] for case in _CORPUS],
)
def test_structured_corpus_matches(label, content, old, new, kwargs):
    compare_cases([(label, content, old, new, kwargs)])


def test_kimi_agent_expected_values_are_reproduced():
    """Pin the values kimi-agent's own suite asserts, on both sides.

    Keeps the corpus honest: if the reference ever changes, this fails before
    the differential test can silently agree on a new answer.
    """
    expectations = [
        (("helo world\nnext line",), dict(old="hello world", new="hi universe"),
         ("hi universe\nnext line", 1, "fuzzy-matched at 95%: 'helo world'")),
        (("hello world  \nnext line",), dict(old="hello world", new="hi universe"),
         ("hi universe  \nnext line", 1, None)),
        (("  hello world\nnext line",), dict(old="hello world", new="hi universe"),
         ("  hi universe\nnext line", 1, None)),
        (("hello world\nfoo bar\nbaz qux",), dict(old="xyz123_not_close", new="replacement"),
         ("hello world\nfoo bar\nbaz qux", 0, None)),
        (("hello world\nfoo bar",), dict(old="helo wrld", new="replacement",
                                         replace_all=True),
         ("hello world\nfoo bar", 0, "hello world")),
        (("start\n  line A\n  line B\nend",), dict(old="line A\nline B", new="line X\nline Y"),
         ("start\nline X\nline Y\nend", 1,
          "fuzzy-matched at 87%: '  line A\n  line B'")),
        (("a b a c a d a",), dict(old="a", new="X", replace_all=True, max_replacements=2),
         ("X b X c a d a", 2, None)),
        (("helo world\nnext line",), dict(old="hello world", new="hi universe",
                                          match_mode="exact"),
         ("helo world\nnext line", 0, "helo world")),
        (("line1\r\nline2\r\nline3",), dict(old="line2\nline3", new="replaced"),
         ("line1\nreplaced", 1, None)),
        (("a\r\nb\r\na\r\nc",), dict(old="a\n", new="X\n", replace_all=True),
         ("X\nb\nX\nc", 2, None)),
    ]
    for args, kwargs, expected in expectations:
        content = args[0]
        ref_out = ref_apply_edit(content, kwargs["old"], kwargs["new"],
                                 **{k: v for k, v in kwargs.items()
                                    if k not in ("old", "new")})
        native_out = native_apply_edit(content, kwargs["old"], kwargs["new"],
                                       **{k: v for k, v in kwargs.items()
                                          if k not in ("old", "new")})
        assert ref_out == expected, f"reference drift for {content!r}: {ref_out!r}"
        assert native_out == expected, f"native drift for {content!r}: {native_out!r}"


# ===========================================================================
# 2. Seeded fuzz generators
# ===========================================================================


def _fuzz_slice_cases(seed=20240611, n=3000):
    """Random small alphabets; `old` is often a mutated slice of the content."""
    rng = random.Random(seed)
    alpha = ["a", "b", "c", " ", "\t", "\n", "\r\n", "x", "y", "hello", "foo",
             "bar", "\u00e9", "\u4e16", "AB", "ab", "", "  ", "1"]

    def text(maxlen=10):
        return "".join(rng.choice(alpha) for _ in range(rng.randint(0, maxlen)))

    cases = []
    for i in range(n):
        content = text(12)
        if rng.random() < 0.5 and content and rng.random() < 0.7:
            s = rng.randrange(len(content))
            e = rng.randrange(s, len(content) + 1)
            old = content[s:e]
            if old and rng.random() < 0.4:
                k = rng.randrange(len(old))
                old = old[:k] + rng.choice(alpha) + old[k + 1:]
        else:
            old = text(6)
        new = text(4)
        kwargs = {}
        if rng.random() < 0.4:
            kwargs["replace_all"] = True
            if rng.random() < 0.4:
                kwargs["max_replacements"] = rng.randint(1, 3)
        if rng.random() < 0.2:
            kwargs["match_mode"] = "exact"
        cases.append((f"fuzz-slice-{i}", content, old, new, kwargs))
    return cases


def _fuzz_lines_cases(seed=987654321, n=3000):
    """Line-structured content with CRLF/CR/LF/no-EOL variants."""
    rng = random.Random(seed)
    words = ["alpha", "beta", "gamma", "  indented", "\tx", "x", "yy", "", "Zz",
             "hello world", "caf\u00e9", "\u4e16\u754c", "\U0001f600", "a b c",
             "def f():", "    pass", "}", "{", "// comment", "  ", "\t\t",
             "trailing  ", " leading", "MiXeD"]
    eols = ["\n", "\r\n", "\r", "", "\n\n"]

    def rand_lines(count, eol):
        return eol.join(rng.choice(words) for _ in range(count))

    cases = []
    for i in range(n):
        eol = rng.choice(eols)
        content = rand_lines(rng.randint(0, 6), eol)
        if rng.random() < 0.3:
            content += rng.choice(["\n", "\r\n", ""])
        kind = rng.random()
        if kind < 0.35:
            if content:
                s = rng.randrange(len(content))
                e = rng.randrange(s, len(content) + 1)
                old = content[s:e]
            else:
                old = ""
            if old and rng.random() < 0.45:
                k = rng.randrange(len(old))
                old = old[:k] + rng.choice(["\t", " ", "x", "", "\n", "\r\n",
                                            "\u4e16"]) + old[k + 1:]
        elif kind < 0.6:
            lines = content.splitlines()
            if lines:
                k = rng.randint(1, 3)
                if len(lines) >= k:
                    s = rng.randrange(0, len(lines) - k + 1)
                    old = rng.choice(["\n", "\r\n", eol]).join(lines[s:s + k])
                    if rng.random() < 0.4:
                        old = old.replace("  ", " ")
                else:
                    old = "\n".join(lines)
            else:
                old = rng.choice(words)
        else:
            old = rand_lines(rng.randint(0, 3), rng.choice(["\n", "\r\n"]))
        new = rand_lines(rng.randint(0, 2), rng.choice(["\n", "\r\n"]))
        kwargs = {}
        if rng.random() < 0.35:
            kwargs["replace_all"] = True
            if rng.random() < 0.35:
                kwargs["max_replacements"] = rng.randint(1, 4)
        if rng.random() < 0.15:
            kwargs["match_mode"] = "exact"
        cases.append((f"fuzz-lines-{i}", content, old, new, kwargs))
    return cases


def _fuzz_unicode_cases(seed=1357911, n=1500):
    """Code-point soup: accents, emoji, CJK, exotic separators, combiners."""
    rng = random.Random(seed)
    soup = ["\u00e9", "\u00df", "\u4e16", "\U0001f600", "\u0301", "a", " ",
            "\u00a7", "\u2502", "\u27ea", "\u2028", "\u0085", "\v", "\f"]
    cases = []
    for i in range(n):
        content = "".join(rng.choice(soup) for _ in range(rng.randint(0, 14)))
        if rng.random() < 0.5:
            old = "".join(rng.choice(soup) for _ in range(rng.randint(0, 6)))
        elif content:
            s = rng.randrange(len(content) + 1)
            old = content[s:s + rng.randint(0, 6)]
        else:
            old = ""
        new = "".join(rng.choice(soup) for _ in range(rng.randint(0, 5)))
        kwargs = {"replace_all": True} if rng.random() < 0.3 else {}
        cases.append((f"fuzz-unicode-{i}", content, old, new, kwargs))
    return cases


def test_fuzz_slice_corpus_matches():
    compare_cases(_fuzz_slice_cases())


def test_fuzz_lines_corpus_matches():
    compare_cases(_fuzz_lines_cases())


def test_fuzz_unicode_corpus_matches():
    compare_cases(_fuzz_unicode_cases())


# ===========================================================================
# 3. Cutoff / score boundary
# ===========================================================================


def test_near_cutoff_scores_match():
    """Scores in [74, 76] and around the *.5 rounding boundary."""
    from rapidfuzz import fuzz as rf_fuzz

    rng = random.Random(555777)
    cases = []
    for i in range(2500):
        base = "".join(rng.choice("abcdefghij ") for _ in range(rng.randint(3, 25)))
        mut = list(base)
        for _ in range(rng.randint(1, 5)):
            if not mut:
                break
            j = rng.randrange(len(mut))
            op = rng.random()
            if op < 0.4:
                mut[j] = rng.choice("abcdefghij ")
            elif op < 0.7:
                mut.insert(j, rng.choice("abcdefghij "))
            else:
                del mut[j]
        target = "".join(mut)
        score = rf_fuzz.ratio(target, base)
        frac = abs(score - round(score))
        if not (74.0 <= score <= 76.0 or abs(frac - 0.5) < 0.02):
            continue
        content = base + "\nother line here\n"
        cases.append((f"near-cutoff-{i}", content, target, "REPL", {}))
    assert len(cases) > 50, "near-cutoff corpus degenerated"
    compare_cases(cases)


def test_exactly_at_cutoff_is_a_match():
    """fuzz.ratio == 75.0 exactly: >= cutoff, so the fuzzy path applies.

    U+1F642 + 'abc' vs U+1F642 + 'abd' scores exactly 75.0 (see the C++
    golden in tests/unit/builtin_tools/test_edit_tool.cpp).
    """
    content = "\U0001f642abd\n"
    old = "\U0001f642abc"
    expected = ("Q\n", 1, "fuzzy-matched at 75%: '\U0001f642abd'")
    assert ref_apply_edit(content, old, "Q") == expected
    assert native_apply_edit(content, old, "Q") == expected


def test_half_point_score_rounding_matches():
    """A score of exactly 87.5 formats as 88% on both sides (round-half-even)."""
    content = "abcdefgh\nnext\n"
    old = "abcdefgX"  # ratio("abcdefgX", "abcdefgh") == 87.5
    expected = ("Y\nnext\n", 1, "fuzzy-matched at 88%: 'abcdefgh'")
    assert ref_apply_edit(content, old, "Y") == expected
    assert native_apply_edit(content, old, "Y") == expected


def test_suggestion_truncates_matched_text_at_80_code_points():
    """`matched_text[:80]` is a code-point slice, not a byte slice."""
    line = ("\u00e9" * 40) + "X" + ("\u00e9" * 40)
    old = ("\u00e9" * 40) + "Y" + ("\u00e9" * 40)
    content = line + "\n"
    ref_out = ref_apply_edit(content, old, "Z")
    native_out = native_apply_edit(content, old, "Z")
    assert ref_out == native_out
    assert ref_out[0] == "Z\n"
    assert ref_out[2].startswith("fuzzy-matched at ")
    assert ref_out[2].endswith("\u00e9'")
    assert len(ref_out[2].split("'")[1]) == 80


# ===========================================================================
# 4. match_mode / argument-shape contract
# ===========================================================================


def test_match_mode_names_accepted_and_rejected():
    """Only 'exact' and 'fuzzy' exist (params.py EditMode literal)."""
    content = "hello world\n"
    for mode in ("exact", "fuzzy"):
        assert native_apply_edit(content, "hello world", "x", match_mode=mode) == \
            ref_apply_edit(content, "hello world", "x", match_mode=mode)
    # "sloppy" is an *edit* mode, never a match_mode: both sides must reject it.
    assert _outcome(ref_apply_edit, content, "hello world", "x",
                    match_mode="sloppy")[0] == "raise"
    assert _outcome(native_apply_edit, content, "hello world", "x",
                    match_mode="sloppy")[0] == "raise"
    assert _outcome(ref_apply_edit, content, "hello world", "x",
                    match_mode="EXACT")[0] == "raise"
    assert _outcome(native_apply_edit, content, "hello world", "x",
                    match_mode="EXACT")[0] == "raise"


def test_replace_all_and_max_replacements_boundaries():
    """max_replacements only applies with replace_all (reference semantics)."""
    cases = []
    for content, old, new in [("a a a", "a", "b"), ("aaaa", "aa", "b"),
                              ("a", "a", "a"), ("abc", "x", "y")]:
        for replace_all in (False, True):
            for max_rep in (None, 1, 2, 5):
                cases.append((f"max-{content!r}-{replace_all}-{max_rep}", content,
                              old, new,
                              {"replace_all": replace_all,
                               "max_replacements": max_rep}))
    compare_cases(cases)


def test_error_contract_no_match_is_data_not_exception():
    """A miss returns (content, 0, suggestion|None); it never raises."""
    out = native_apply_edit("hello world\n", "notfound", "x")
    assert out[0] == "hello world\n"
    assert out[1] == 0


# ===========================================================================
# 5. Regression: the below-cutoff fuzzy leak (fixed in edit_tool.cpp)
# ===========================================================================


def test_regression_below_cutoff_fuzzy_must_not_replace():
    """best_fuzzy_match must not surface a candidate below the 75 cutoff.

    Before the fix, apply_edit() applied the best *scoring* line even when its
    score was far below the cutoff (the C++ kept matched_original set), turning
    "no match" into a bogus replacement with a "fuzzy-matched at 0%" note.
    """
    cases = [
        # (content, old, new) -- the reference says: unchanged, 0, no suggestion
        ("hello world\nfoo bar\nbaz qux", "xyz123_not_close", "replacement"),
        ("Hello world!", "notfound", "replacement"),
        ("cafe na\u00efve\n", "caf\u00e9", "th\u00e9"),
        ("b", "ABbabABc", "b"),
        ("l1\nl2\n", "l9\nl8", "Q"),
    ]
    for content, old, new in cases:
        expected = ref_apply_edit(content, old, new)
        assert expected == (content, 0, None), (
            f"reference changed for {content!r}/{old!r}: {expected!r}")
        actual = native_apply_edit(content, old, new)
        assert actual == expected, (
            f"below-cutoff fuzzy leak for content={content!r} old={old!r}: "
            f"native={actual!r} python={expected!r}")


def test_regression_below_cutoff_replace_all_suggestion_still_works():
    """The below-cutoff fix must not disable the >= cutoff suggestion path."""
    content = "hello world\nfoo bar"
    old = "helo wrld"
    expected = ref_apply_edit(content, old, "replacement", replace_all=True)
    assert expected == (content, 0, "hello world")
    assert native_apply_edit(content, old, "replacement", replace_all=True) == expected


# ===========================================================================
# 6. Documented deviation: the native fuzz length gate
# ===========================================================================


@pytest.mark.xfail(
    strict=True,
    reason="reports/edit.md deviation #2: the native kernel caps fuzz_ratio "
           "inputs (10k code points / 25M cells) and raises ValueError; "
           "rapidfuzz has no such limit. Intended, documented divergence.",
)
def test_documented_deviation_fuzz_cap():
    big_line = "a" * 40000
    other = ("a" * 39999) + "b"
    content = big_line + "\n"
    assert native_apply_edit(content, other, "X") == ref_apply_edit(content, other, "X")


def test_the_cap_is_real_and_reports_a_value_error():
    """Pin the actual deviating behaviour so the xfail above cannot go stale."""
    big_line = "a" * 40000
    other = ("a" * 39999) + "b"
    content = big_line + "\n"
    with pytest.raises(ValueError):
        native_apply_edit(content, other, "X")
    # ... while the reference happily returns a fuzzy match.
    out = ref_apply_edit(content, other, "X")
    assert out[1] == 1


# ===========================================================================
# 7. apply_diff_hunks parity (report deviations #3 and #6)
# ===========================================================================
#
# ``apply_diff_hunks`` is the second kernel the extension exposes.  kimi-agent
# *deleted* its reference module (commit 2e0d464 "remove patch and hashline edit
# mode"), so the ground truth is that file's last revision, fetched from the
# checkout's own git object store (never a copy pasted into this repo).  This
# section therefore also verifies two entries of the port report's deviation
# list:
#   #3 ``_infer_indent_adjustment`` tie break (tab/space deltas),
#   #6 ``match_diff_header`` accepting an optional `,count` on the NEW side.

_DIFF_REF_REV = "2e0d464^"
_DIFF_REF_PATH = "kimi-cli/src/kimi_cli/tools/file/edit/diff.py"
_DIFF_HIST_MODULE = "kimix_parity_hist_diff"
_DIFF_HIST_PATH = f"{_DIFF_REF_PATH}@{_DIFF_REF_REV}"


def _load_historical_module(rel_path, rev, module_name):
    """Import one kimi-agent revision of a module from the git object store.

    Used where the reference file was deleted upstream (patch/hashline modes)
    but its kernels are still present in the port.  Nothing is copied into this
    repo: the source always comes from the checkout's own .git.
    """
    import subprocess
    import types

    if module_name in sys.modules:
        return sys.modules[module_name]
    try:
        proc = subprocess.run(
            ["git", "-C", str(KIMI_AGENT_ROOT), "show", f"{rev}:{rel_path}"],
            capture_output=True, text=True, encoding="utf-8",
        )
    except OSError:
        return None
    if proc.returncode != 0 or not proc.stdout:
        return None
    module = types.ModuleType(module_name)
    module.__file__ = f"<{KIMI_AGENT_ROOT}/{rel_path}@{rev}>"
    sys.modules[module_name] = module
    exec(compile(proc.stdout, f"{rel_path}@{rev}", "exec"), module.__dict__)  # noqa: S102
    return module


def _load_historical_diff_module():
    """Import the last revision of edit/modes' diff.py from the git object store."""
    return _load_historical_module(_DIFF_REF_PATH, _DIFF_REF_REV, _DIFF_HIST_MODULE)


_diff_hist = _load_historical_diff_module()

requires_diff_ref = pytest.mark.skipif(
    _diff_hist is None,
    reason=f"cannot read {_DIFF_HIST_PATH} from the kimi-agent git object store",
)


def ref_apply_diff_hunks(diff, content, allow_fuzzy=True, threshold=0.75):
    """(kind, payload) of the historical pure-Python parse+apply pipeline."""
    try:
        hunks = _diff_hist.parse_diff_hunks(diff)
        return ("ok", _diff_hist.apply_diff_hunks(content, hunks,
                                                  allow_fuzzy=allow_fuzzy,
                                                  threshold=threshold))
    except _diff_hist.ApplyPatchError as exc:
        return ("err", exc.message)
    except Exception as exc:  # noqa: BLE001
        return ("exc", f"{type(exc).__name__}: {exc}")


def native_apply_diff_hunks(diff, content, allow_fuzzy=True, threshold=0.75):
    """(kind, payload) of the extension binding (diff first, then content)."""
    try:
        return ("ok", FILE.apply_diff_hunks(diff, content, allow_fuzzy, threshold))
    except ValueError as exc:
        return ("err", str(exc))
    except Exception as exc:  # noqa: BLE001
        return ("exc", f"{type(exc).__name__}: {exc}")


def compare_diff_cases(cases, limit_reported=6):
    mismatches = []
    for label, diff, content, allow_fuzzy, threshold in cases:
        expected = ref_apply_diff_hunks(diff, content, allow_fuzzy, threshold)
        actual = native_apply_diff_hunks(diff, content, allow_fuzzy, threshold)
        same = expected[0] == actual[0] and (
            expected[0] in ("raise", "exc") or expected[1] == actual[1]
        )
        if not same:
            mismatches.append((label, diff, content, allow_fuzzy, threshold,
                               expected, actual))
    if mismatches:
        lines = [f"{len(mismatches)} mismatching diff case(s); "
                 f"first {min(limit_reported, len(mismatches))}:"]
        for label, diff, content, af, th, expected, actual in mismatches[:limit_reported]:
            lines.append(f"  [{label}] diff={diff!r} content={content!r} "
                         f"allow_fuzzy={af} threshold={th}")
            lines.append(f"      python: {expected[0]} {expected[1]!r}")
            lines.append(f"      native: {actual[0]} {actual[1]!r}")
        pytest.fail("\n".join(lines), pytrace=False)


DIFF_CORPUS = [
    # standard / bare / anchored headers
    ("std", "@@ -1,3 +1,3 @@\n a\n-b\n+c\n", "a\nb\n"),
    ("std_trailing", "@@ -1,3 +1,3 @@\n a\n-b\n+c\n", "a\nb\nx\n"),
    ("bare_no_count", "@@ -1 +1 @@\n-a\n+b\n", "a\n"),
    ("count_old_only", "@@ -1,1 +1 @@\n-a\n+b\n", "a\n"),
    ("count_new_only", "@@ -1 +1,1 @@\n-a\n+b\n", "a\n"),
    ("count_both", "@@ -1,1 +1,1 @@\n-a\n+b\n", "a\n"),
    ("context_suffix", "@@ -1,2 +1,2 @@ function foo\n a\n-b\n+c\n", "a\nb\n"),
    ("trailing_blank", "@@ -1,2 +1,2 @@\n a\n-b\n+c\n\n", "a\nb\n"),
    ("anchor_only", "1\n-a\n+b\n", "a\n"),
    ("bare_at_at", "@@\n-a\n+b\n", "a\n"),
    ("crlf", "@@ -1,3 +1,3 @@\r\n a\r\n-b\r\n+c\r\n", "a\r\nb\r\n"),
    ("file_headers", "--- a/x\n+++ b/x\n@@ -1,2 +1,2 @@\n a\n-b\n+c\n", "a\nb\n"),
    ("git_headers",
     "diff --git a/x b/x\nindex 1..2 100644\n--- a/x\n+++ b/x\n@@ -1,2 +1,2 @@\n a\n-b\n+c\n",
     "a\nb\n"),
    ("no_newline_marker", "@@ -1,3 +1,3 @@\n a\n-b\n+c\n+", "a\nb\n"),
    # matching / ambiguity / miss
    ("ambiguous", "@@ -1,2 +1,2 @@\n a\n-b\n+c\n", "a\nb\nb\n"),
    ("no_match", "@@ -1,2 +1,2 @@\n a\n-b\n+c\n", "no match here\n"),
    ("anchor_offset", "@@ -2,1 +2,1 @@\n-a\n+b\n", "x\na\n"),
    ("multi_line_hunk", "@@ -1,2 +1,2 @@\n context\n-removed\n+added\n",
     "context\nremoved\n"),
    # fuzzy fallback + indentation
    ("fuzzy_indent", "@@ -1,1 +1,1 @@\n-a\n+b\n", "  a\n"),
    ("fuzzy_tab", "@@ -1,1 +1,1 @@\n-\ta\n+\tb\n", "  a\n"),
    ("fuzzy_spaces", "@@ -1,1 +1,1 @@\n-  a\n+  b\n", "\ta\n"),
    # adds / deletes only
    ("delete_only", "@@ -1,1 +1,1 @@\n-a\n", "a\n"),
    ("add_only", "@@ -1,1 +1,1 @@\n+a\n", ""),
    ("create", "@@ -0,0 +1,2 @@\n+a\n+b\n", ""),
    ("empty_diff", "", "a\n"),
    ("blank_diff", "\n\n", "a\n"),
    ("empty_content", "@@ -1,1 +1,1 @@\n-a\n+b\n", ""),
    # malformed content outside a hunk
    ("junk_before_hunk", "garbage line\n@@ -1,1 +1,1 @@\n-a\n+b\n", "a\n"),
    ("junk_after_hunk", "@@ -1,1 +1,1 @@\n-a\n+b\n\ngarbage", "a\n"),
    ("trailing_blank_content", "@@ -1,1 +1,1 @@\n-a\n+b\n", "a\n\n"),
]


@requires_diff_ref
@pytest.mark.parametrize(
    "label,diff,content,allow_fuzzy,threshold",
    [(label, d, c, True, 0.75) for label, d, c in DIFF_CORPUS],
    ids=[case[0] for case in DIFF_CORPUS],
)
def test_diff_structured_corpus_matches(label, diff, content, allow_fuzzy, threshold):
    compare_diff_cases([(label, diff, content, allow_fuzzy, threshold)])


@requires_diff_ref
def test_diff_error_texts_match():
    """Error strings (multiple matches / no match / unexpected content) are data."""
    cases = [
        ("ambiguous", "@@ -1,1 +1,1 @@\n-a\n+b\n", "a\na\n", True, 0.75),
        ("no_match_context", "@@ -1,1 +1,1 @@ ctx here\n-a\n+b\n", "zzz\n", True, 0.75),
        ("no_match_plain", "@@ -1,1 +1,1 @@\n-a\n+b\n", "zzz\n", True, 0.75),
        ("no_match_nofuzzy", "@@ -1,1 +1,1 @@\n-a\n+b\n", "zzz\n", False, 0.75),
        ("junk_line", "@@ -1,1 +1,1 @@\nx y\n", "x y\n", True, 0.75),
    ]
    compare_diff_cases(cases)
    # Spot-check the exact wording once, so a refactor cannot swap the texts.
    assert ref_apply_diff_hunks("@@ -1,1 +1,1 @@\n-a\n+b\n", "zzz\n") == (
        "err", "No match found for hunk anchored at line 1.")
    assert native_apply_diff_hunks("@@ -1,1 +1,1 @@\n-a\n+b\n", "zzz\n") == (
        "err", "No match found for hunk anchored at line 1.")
    unexpected = "garbage line\n@@ -1,1 +1,1 @@\n-a\n+b\n"
    expected_msg = "Unexpected diff content outside a hunk: 'garbage line'"
    assert ref_apply_diff_hunks(unexpected, "a\n") == ("err", expected_msg)
    assert native_apply_diff_hunks(unexpected, "a\n") == ("err", expected_msg)


@requires_diff_ref
def test_diff_multi_byte_truncation_of_unexpected_content():
    """Regression: diff.py slices ``line[:80]`` in CODE POINTS.

    The port sliced bytes (``substr(0, 80)``), which cut multi-byte characters
    in half and reported a much shorter prefix for non-ASCII lines.
    """
    line = "garbage " + ("\u00e9" * 200)
    diff = line + "\n@@ -1,1 +1,1 @@\n-a\n+b\n"
    expected = ref_apply_diff_hunks(diff, "a\n")
    actual = native_apply_diff_hunks(diff, "a\n")
    assert expected == actual, f"\n python={expected[1]!r}\n native={actual[1]!r}"
    # 80 code points: 'garbage ' + 72 accented chars.
    assert expected[1].count("\u00e9") == 72
    # Exactly 80 code points too (no off-by-one at the boundary).  The payload
    # is the quoted repr, not the whole message (whose prefix contains "context"
    # -- and therefore an 'x').
    for n in (79, 80, 81, 100):
        d = ("x" * n) + "\n@@ -1,1 +1,1 @@\n-a\n+b\n"
        exp = ref_apply_diff_hunks(d, "a\n")
        act = native_apply_diff_hunks(d, "a\n")
        assert exp == act, f"n={n}: python={exp[1]!r} native={act[1]!r}"
        payload = exp[1].split("'")[1]
        assert payload.count("x") == min(n, 80)
        assert payload == "x" * min(n, 80)


@requires_diff_ref
def test_diff_header_count_variants_match():
    """Deviation #6: optional `,count` on BOTH sides (the reference regex)."""
    bodies = [
        "@@ -1 +1 @@\n-a\n+b\n",
        "@@ -1,1 +1 @@\n-a\n+b\n",
        "@@ -1 +1,1 @@\n-a\n+b\n",
        "@@ -1,1 +1,1 @@\n-a\n+b\n",
        "@@ -1 +1 @@ ctx\n-a\n+b\n",
        "@@ -1,7 +1,9 @@\n-a\n+b\n",
        "@@ -0,0 +1,1 @@\n+a\n",
    ]
    # Every variant must be accepted by both sides and produce the same text.
    compare_diff_cases(
        [(f"hdr{i}", body, "a\n", True, 0.75) for i, body in enumerate(bodies)]
    )
    for body in bodies:
        assert ref_apply_diff_hunks(body, "a\n")[0] == "ok"
        assert native_apply_diff_hunks(body, "a\n")[0] == "ok"


@requires_diff_ref
def test_diff_fuzz_corpus_matches():
    """Seeded hunk fuzz: context/add/delete bodies over a small line alphabet."""
    rng = random.Random(4242)
    lines = ["a", "b", "c", "  indented", "\ttabbed", "", "x y",
             "hello world", "q"]
    cases = []
    for i in range(1200):
        n = rng.randint(1, 6)
        content_lines = [rng.choice(lines) for _ in range(n)]
        content = "\n".join(content_lines)
        if rng.random() < 0.5:
            content += "\n"
        start = rng.randint(1, n)
        take = rng.randint(1, 3)
        body = []
        for ln in content_lines[start - 1:start - 1 + take]:
            if rng.random() < 0.5:
                body.append(" " + ln)
            else:
                body.append("-" + ln)
                body.append("+" + ln + "X")
        if not body:
            body = ["+new"]
        if rng.random() < 0.15:
            header = "@@ -%d +%d @@" % (start, start)
        else:
            header = "@@ -%d,%d +%d,%d @@" % (start, take, start, take)
        diff = header + "\n" + "\n".join(body) + "\n"
        cases.append((f"fuzz-{i}", diff, content, True, 0.75))
    compare_diff_cases(cases)


@requires_diff_ref
def test_diff_indent_adjustment_tie_breaks_match():
    """Deviation #3: `max(set(deltas), key=deltas.count)` tie break for tabs."""
    rng = random.Random(31337)
    ws_lines = ["\ta", "    a", "  a", "a", "\t  a", " \t a", "        a", " a"]
    cases = []
    for i in range(1000):
        n = rng.randint(1, 5)
        content_lines = [rng.choice(ws_lines) for _ in range(n)]
        for j in range(len(content_lines)):
            if rng.random() < 0.3:
                content_lines[j] += rng.choice(["x", "y", ""])
        content = "\n".join(content_lines) + "\n"
        start = rng.randint(1, n)
        window = content_lines[start - 1:start - 1 + rng.randint(1, 2)]
        body = ["+" + w for w in window]
        header = "@@ -%d,%d +%d,%d @@" % (start, len(window), start, len(window))
        cases.append((f"indent-{i}", header + "\n" + "\n".join(body) + "\n",
                      content, True, 0.75))
    for i in range(1000):
        n = rng.randint(2, 5)
        content_lines = [rng.choice(ws_lines) for _ in range(n)]
        content = "\n".join(content_lines) + "\n"
        start = rng.randint(1, n - 1)
        body = [" " + content_lines[start - 1],
                "-" + content_lines[start],
                "+" + rng.choice(ws_lines)]
        header = "@@ -%d,2 +%d,2 @@" % (start, start)
        cases.append((f"ws-{i}", header + "\n" + "\n".join(body) + "\n",
                      content, True, 0.75))
    compare_diff_cases(cases)


@requires_diff_ref
@pytest.mark.xfail(
    strict=True,
    reason="reports/edit.md deviation #4: py_repr passes non-ASCII code points "
           "through verbatim, so non-printable ones (Cc/Cf/Zs/Zl) are not "
           "escaped as Python repr() does (\\x85, \\xa0, \\u2028, \\ufeff). "
           "Reachable only through the apply_diff_hunks error text; a correct "
           "table exists in src/runtime/tools/security.cpp but that TU is not "
           "linked into kimix-llm (adding it needs src/xmake.lua, off-limits).",
)
def test_documented_deviation_py_repr_non_ascii_escaping():
    for ch in ("\x85", "\xa0", "\u2028", "\ufeff"):
        diff = f"garbage {ch} line\n@@ -1,1 +1,1 @@\n-a\n+b\n"
        assert native_apply_diff_hunks(diff, "a\n") == \
            ref_apply_diff_hunks(diff, "a\n")


@requires_diff_ref
def test_py_repr_non_ascii_deviation_shape_is_pinned():
    """Characterise the deviation above: printable non-ASCII still agrees."""
    # Printable non-ASCII (café, CJK, emoji) is passed through by BOTH sides.
    for ch in ("\u00e9", "\u4e16", "\U0001f600"):
        diff = f"garbage {ch} line\n@@ -1,1 +1,1 @@\n-a\n+b\n"
        assert native_apply_diff_hunks(diff, "a\n") == \
            ref_apply_diff_hunks(diff, "a\n")
    # ... and the non-printable ones differ in exactly the documented way.
    diff = "garbage \x85 line\n@@ -1,1 +1,1 @@\n-a\n+b\n"
    assert ref_apply_diff_hunks(diff, "a\n") == (
        "err", "Unexpected diff content outside a hunk: 'garbage \\x85 line'")
    assert native_apply_diff_hunks(diff, "a\n") == (
        "err", "Unexpected diff content outside a hunk: 'garbage \x85 line'")


# ===========================================================================
# 8. Kernels the extension cannot reach, bridged through their C++ goldens
# ===========================================================================
#
# The sloppy-mode and hashline kernels are pure C++ (no pybind entry point), so
# they cannot be called from Python.  Their expectations live in
# tests/unit/builtin_tools/test_edit_tool.cpp, which was written by transcribing
# values harvested from kimi-agent.  The tests below re-derive *exactly those
# literals* from the reference, so the chain
#     C++ kernel == C++ golden (Boost.UT)  and  C++ golden == Python (here)
# is closed.  They intentionally do not assert anything about the C++ side; that
# is what the Boost.UT suite does.

_SLOPPY_MOD = "kimi_cli.tools.file.edit.modes.sloppy"
_sloppy_ref = None
try:
    _sloppy_ref = ref(_SLOPPY_MOD)
except Exception:  # noqa: BLE001
    _sloppy_ref = None

requires_sloppy_ref = pytest.mark.skipif(
    _sloppy_ref is None, reason=f"cannot import {_SLOPPY_MOD} from the checkout"
)


@requires_sloppy_ref
def test_sloppy_goldens_pinned_in_the_cpp_suite_match_python():
    """Re-derive every sloppy expectation of test_edit_tool.cpp from Python."""
    SloppyInline = _sloppy_ref.SloppyInline
    SloppyOp = _sloppy_ref.SloppyOp
    parse = _sloppy_ref.parse_sloppy_input
    apply_block = _sloppy_ref._apply_block_op
    apply_inline = _sloppy_ref._apply_inline_op
    find_fuzzy = _sloppy_ref._find_fuzzy_block
    with pure_python(_sloppy_ref):
        # sloppy_parse_sections_and_modes
        ops = parse("\u00a7foo.txt\nMATCH\n\u00bb\nREWRITE\n\u00a7bar.txt\nline1\n")
        assert len(ops) == 2
        assert ops[0].path == "foo.txt"
        assert ops[0].all_match is False
        assert ops[0].match_lines == ["MATCH"]
        assert ops[0].rewrite_lines == ["REWRITE"]
        assert ops[1].path == "bar.txt"
        assert ops[1].rewrite_lines is None
        assert [(line, sels) for line, sels in ops[1].inline_lines] == [("line1", [])]

        # sloppy_parse_all_match_and_path_inheritance
        ops = parse("\u00a7*a.txt\nx\n\u00a7\n y \n")
        assert len(ops) == 2
        assert ops[0].all_match is True
        assert ops[0].path == "a.txt"
        assert ops[1].path == "a.txt"
        assert ops[1].inline_lines[0][0] == " y "

        # sloppy_parse_inline_selections_rescan
        ops = parse("\u00a7f.txt\nline \u27eaold1\u2502new1\u27eb and "
                    "\u27eaold2\u2502new2\u27eb\n")
        sels = ops[0].inline_lines[0][1]
        assert [(s.old, s.new) for s in sels] == [("old1", "new1"), ("old2", "new2")]

        # sloppy_parse_errors
        with pytest.raises(ValueError, match="No sloppy operations found"):
            parse("no section here\n")
        assert str(_exc_info(lambda: parse("no section here\n"))) == \
            "No sloppy operations found. Input must start with `\u00a7path`."
        assert str(_exc_info(lambda: parse("\u00a7\nx\n"))) == \
            "Bare `\u00a7` requires a previous section with a path."

        # sloppy_block_exact_and_deletion_swallow
        op = SloppyOp(path="f", match_lines=["b", "c"], rewrite_lines=["B", "C"])
        assert apply_block("a\nb\nc\nd\n", op) == "a\nB\nC\nd\n"
        op = SloppyOp(path="f", match_lines=["b", "c"], rewrite_lines=[])
        assert apply_block("a\nb\nc\nd\n", op) == "a\nd\n"

        # sloppy_block_all_match_non_overlapping
        op = SloppyOp(path="f", all_match=True, match_lines=["ab"], rewrite_lines=["X"])
        assert apply_block("ababab", op) == "XXX"

        # sloppy_block_fuzzy_and_errors
        assert find_fuzzy("the quick brown fox\njumps over\nlazy dog\n",
                          ["The quick brown fox"]) == (0, 19)
        op = SloppyOp(path="f", match_lines=["nope"], rewrite_lines=["X"])
        assert str(_exc_info(lambda: apply_block("zzz\n", op))) == \
            "Could not locate MATCH block:\nnope"
        op = SloppyOp(path="f", match_lines=[], rewrite_lines=["X"])
        assert str(_exc_info(lambda: apply_block("zzz\n", op))) == "MATCH block is empty."

        # sloppy_inline_apply_first_and_all
        op = SloppyOp(path="f", inline_lines=[("x old y", [SloppyInline("old", "new")])])
        assert apply_inline("x old y\n", op) == "x new y\n"
        op = SloppyOp(path="f", all_match=True,
                      inline_lines=[("l", [SloppyInline("a", "b")])])
        assert apply_inline("a a a", op) == "b b b"
        op = SloppyOp(path="f", inline_lines=[("l", [SloppyInline("zz", "q")])])
        assert str(_exc_info(lambda: apply_inline("abc\n", op))) == \
            "Could not locate inline selection: 'zz'"

        # sloppy_dispatch
        op = SloppyOp(path="f", match_lines=["b"], rewrite_lines=["B"])
        assert apply_block("a\nb\nc\n", op) == "a\nB\nc\n"
        op = SloppyOp(path="f", inline_lines=[("l", [SloppyInline("b", "B")])])
        assert apply_inline("a\nb\nc\n", op) == "a\nB\nc\n"


def _exc_info(fn):
    """Return the exception raised by fn(), or None."""
    try:
        fn()
    except Exception as exc:  # noqa: BLE001
        return exc
    return None


_HASH_LINE_REV = "2e0d464^"
_HASH_LINE_PATH = "kimi-cli/src/kimi_cli/tools/file/hash_line.py"


def _load_hash_line_recipe():
    """Extract compute_line_hash from the historical hash_line.py via ast.

    The module as a whole cannot be imported (it is full of relative imports to
    the edit package), so only the constants and the one function are executed
    -- exactly the technique test_parity_grep.py uses for nested reference code.
    """
    import ast
    import subprocess

    try:
        import xxhash
    except ImportError:
        return None
    try:
        proc = subprocess.run(
            ["git", "-C", str(KIMI_AGENT_ROOT), "show",
             f"{_HASH_LINE_REV}:{_HASH_LINE_PATH}"],
            capture_output=True, text=True, encoding="utf-8",
        )
    except OSError:
        return None
    if proc.returncode != 0 or not proc.stdout:
        return None
    wanted_names = {"NIBBLE_STR", "HASH_SEED", "_NIBBLE_LOOKUP"}
    namespace = {"xxhash": xxhash}
    for node in ast.parse(proc.stdout).body:
        if isinstance(node, ast.FunctionDef) and node.name == "compute_line_hash":
            exec(compile(ast.Module([node], []), "hash_line", "exec"), namespace)  # noqa: S102
        elif isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name) \
                and node.targets[0].id in wanted_names:
            exec(compile(ast.Module([node], []), "hash_line", "exec"), namespace)  # noqa: S102
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name) \
                and node.target.id in wanted_names:
            exec(compile(ast.Module([node], []), "hash_line", "exec"), namespace)  # noqa: S102
    return namespace.get("compute_line_hash")


_hash_line_ref = _load_hash_line_recipe()

requires_hash_line_ref = pytest.mark.skipif(
    _hash_line_ref is None,
    reason=f"cannot load compute_line_hash from {_HASH_LINE_PATH}@{_HASH_LINE_REV} "
           f"(needs git + the xxhash package)",
)


@requires_hash_line_ref
def test_hashline_hash_goldens_pinned_in_the_cpp_suite_match_python():
    """The 7 compute_line_hash vectors in test_edit_tool.cpp, from Python."""
    ref_hash = _hash_line_ref
    h1 = ref_hash(1, "hello", None)
    assert h1 == "HK"
    assert ref_hash(2, "world", h1) == "WV"
    assert ref_hash(1, "   ", None) == "KM"
    assert ref_hash(2, "   ", "AA") == "TJ"
    assert ref_hash(1, "hello\r", None) == "HK"
    assert ref_hash(1, "\u00e9\u00e9", None) == "ZX"
    assert ref_hash(1, "", "AB") == "ZK"


@requires_hash_line_ref
def test_line_hash_kernel_matches_the_python_recipe():
    """``runtime_py.tools.line_hash(line, seed)`` is the shared hash recipe.

    Contract from src/runtime/tools/line_hash.h: strip one trailing CR, drop
    whitespace code points (Python str.isspace set), then xxh32(data, seed)
    & 0xFF.  The edit tool's own compute_line_hash embeds the same recipe.
    """
    import random

    import xxhash

    rng = random.Random(20240611)
    lines = ["hello", "world", "", "   ", "\thello", "hello\r", "caf\u00e9",
             "\u4e16\u754c", "a" * 100, "MiXeD case", "  leading", "trailing  ",
             "def f(x):", "\U0001f600", "line with \t tab", "!", "_", "-",
             "\u00a7"]
    cases = [(ln, seed) for ln in lines
             for seed in (0, 1, 2, 7, 255, 65535, 0xFFFFFFFF)]
    for _ in range(300):
        cases.append((
            "".join(rng.choice("abc  \t\r\u00e9\u4e16")
                    for _ in range(rng.randint(0, 12))),
            rng.randint(0, 0xFFFFFFFF),
        ))
    for line, seed in cases:
        body = line[:-1] if line.endswith("\r") else line
        filtered = "".join(ch for ch in body if not ch.isspace())
        want = xxhash.xxh32(filtered.encode("utf-8"), seed).intdigest() & 0xFF
        got = runtime_py.tools.line_hash(line.encode("utf-8"), seed)
        assert got == want, f"line_hash({line!r}, {seed}) = {got}, want {want}"

