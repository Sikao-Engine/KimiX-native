"""Binding tests for the native grep kernel exposed as ``runtime_py.grep``.

Plan: the pybind TU ``src/runtime/py/py_grep.cpp`` wraps
``kimix::builtin_tools::grep::run_grep`` (recursive walk + content scan,
regex_lite patterns) and ``regex_lite::Regex::compile`` behind two functions::

    grep.run(pattern, roots, work_dir, include_glob="",
             mode="files_with_matches", ignore_case=False, ctx_before=0,
             ctx_after=0, head_limit=250) -> dict
    grep.pattern_supported(pattern, ignore_case=False) -> bool

Coverage (semantics pinned by ``tests/unit/builtin_tools/test_grep_tool.cpp``
``grep_tool_native_io_branch_contract`` and by grep_engine.h):
- exact result-dict keys and value types
- the three output modes' rendering: path alone / ``path:count`` /
  ``path:LN:text`` hits with ``path-LN:text`` context and ``--`` separators
- ``line_match`` parallel to ``lines`` (1 hit, 0 context/separator)
- message format ``"{N} match(es) in {M} file(s)"`` (matched LINES counted)
- ignore_case, include_glob (file-name fnmatch, applies to file roots too),
  ctx_before / ctx_after, run merging
- head_limit caps files_with_matches lines while files[] stays complete;
  content/count lines are returned whole; head_limit <= 0 is unlimited
- relative roots resolve against work_dir; hidden entries are skipped;
  paths are walk paths (absolute, never base-stripped)
- parameter validation: bad mode / negative ctx raise ValueError; non-list
  roots / non-str pattern raise TypeError
- engine failures travel as status, not exceptions
- pattern_supported accepts the regex_lite subset and rejects look-around,
  back-references, possessive quantifiers, named groups and ``\\b``/``(?i)``
"""

from __future__ import annotations

import os

import pytest

import runtime_py

G = runtime_py.grep

KEYS = {
    "ok",
    "status",
    "message",
    "total_matches",
    "files",
    "lines",
    "line_match",
}

SEP = "/"


@pytest.fixture()
def tree(tmp_path):
    """Deterministic corpus (walk order is alphabetical):

    a.py       "hit one\\nmiss\\nHIT two\\n"
    c.txt      "hit\\n"
    d.py       "hit\\nx1\\nx2\\nhit\\n"
    sub/b.py   "miss\\nhit\\ntrail\\n"
    .hidden.py "hit\\n"          <- must never be searched
    """
    root = tmp_path
    (root / "a.py").write_text("hit one\nmiss\nHIT two\n", encoding="utf-8")
    (root / "c.txt").write_text("hit\n", encoding="utf-8")
    (root / "d.py").write_text("hit\nx1\nx2\nhit\n", encoding="utf-8")
    (root / "sub").mkdir()
    (root / "sub" / "b.py").write_text("miss\nhit\ntrail\n", encoding="utf-8")
    (root / ".hidden.py").write_text("hit\n", encoding="utf-8")
    return str(root)


def rel(tree, s):
    """Strip the search root and use '/' so expectations stay readable."""
    return s.replace(tree + os.sep, "").replace("\\", SEP)


def rel_lines(tree, r):
    return [rel(tree, l) for l in r["lines"]]


def rel_files(tree, r):
    return [(rel(tree, p), c) for p, c in r["files"]]


def run(tree, pattern="hit", **kw):
    return G.run(pattern, [tree], tree, **kw)


# ---------------------------------------------------------------------------
# result-dict shape
# ---------------------------------------------------------------------------
def test_result_keys_and_types(tree):
    r = run(tree)
    assert set(r.keys()) == KEYS
    assert r["ok"] is True
    assert r["status"] == "ok"
    assert isinstance(r["message"], str)
    assert isinstance(r["total_matches"], int) and not isinstance(r["total_matches"], bool)
    assert isinstance(r["files"], list)
    assert all(
        isinstance(f, tuple) and len(f) == 2 and isinstance(f[0], str) and isinstance(f[1], int)
        for f in r["files"]
    )
    assert isinstance(r["lines"], list)
    assert all(isinstance(l, str) for l in r["lines"])
    assert isinstance(r["line_match"], list)
    assert len(r["line_match"]) == len(r["lines"])
    assert all(type(v) is int and v in (0, 1) for v in r["line_match"])


def test_paths_are_walk_paths_never_base_stripped(tree):
    r = run(tree)
    for line in r["lines"]:
        assert os.path.isabs(line)
        assert line.startswith(tree)
    for path, _count in r["files"]:
        assert os.path.isabs(path)
        assert path.startswith(tree)


# ---------------------------------------------------------------------------
# message format + counts (matching LINES, not occurrences)
# ---------------------------------------------------------------------------
def test_message_format(tree):
    r = run(tree)
    assert r["message"] == "5 match(es) in 4 file(s)"
    assert r["total_matches"] == 5
    assert rel_files(tree, r) == [("a.py", 1), ("c.txt", 1), ("d.py", 2), ("sub/b.py", 1)]


def test_empty_result(tree):
    r = run(tree, pattern="zzz-no-such-token")
    assert r["ok"] is True and r["status"] == "ok"
    assert r["message"] == "0 match(es) in 0 file(s)"
    assert r["total_matches"] == 0
    assert r["files"] == [] and r["lines"] == [] and r["line_match"] == []


def test_hidden_entries_are_skipped(tree):
    r = run(tree)
    assert ".hidden" not in "\n".join(r["lines"])
    assert all(".hidden" not in p for p, _ in r["files"])


# ---------------------------------------------------------------------------
# mode: files_with_matches
# ---------------------------------------------------------------------------
def test_mode_files_with_matches_renders_paths_only(tree):
    r = run(tree, mode="files_with_matches")
    assert rel_lines(tree, r) == ["a.py", "c.txt", "d.py", "sub/b.py"]
    assert r["line_match"] == [1, 1, 1, 1]
    assert len(r["files"]) == 4


def test_head_limit_caps_lines_but_not_files(tree):
    r = run(tree, mode="files_with_matches", head_limit=2)
    assert rel_lines(tree, r) == ["a.py", "c.txt"]
    assert r["line_match"] == [1, 1]
    assert len(r["files"]) == 4  # files[] stays complete
    assert r["total_matches"] == 5
    assert r["message"] == "5 match(es) in 4 file(s)"


@pytest.mark.parametrize("limit", [0, -1, -250])
def test_head_limit_non_positive_is_unlimited(tree, limit):
    r = run(tree, mode="files_with_matches", head_limit=limit)
    assert len(r["lines"]) == 4
    assert len(r["files"]) == 4


def test_default_head_limit_is_250(tree):
    assert run(tree)["lines"]  # small corpus: nothing clipped
    assert rel_lines(tree, run(tree)) == ["a.py", "c.txt", "d.py", "sub/b.py"]


# ---------------------------------------------------------------------------
# mode: count_matches
# ---------------------------------------------------------------------------
def test_mode_count_matches_rendering(tree):
    r = run(tree, mode="count_matches")
    assert rel_lines(tree, r) == ["a.py:1", "c.txt:1", "d.py:2", "sub/b.py:1"]
    assert r["line_match"] == [1, 1, 1, 1]
    counts = {os.path.basename(l.rpartition(":")[0]): int(l.rpartition(":")[2]) for l in r["lines"]}
    assert counts == {"a.py": 1, "c.txt": 1, "d.py": 2, "b.py": 1}
    assert counts == {os.path.basename(p): c for p, c in r["files"]}


def test_count_mode_lines_ignore_head_limit(tree):
    r = run(tree, mode="count_matches", head_limit=1)
    assert len(r["lines"]) == 4  # count/content lines are returned whole


# ---------------------------------------------------------------------------
# mode: content -- rg-style rendering, context, separators, line_match
# ---------------------------------------------------------------------------
def test_mode_content_hits(tree):
    r = run(tree, mode="content")
    assert rel_lines(tree, r) == [
        "a.py:1:hit one",
        "c.txt:1:hit",
        "d.py:1:hit",
        "--",
        "d.py:4:hit",
        "sub/b.py:2:hit",
    ]
    assert r["line_match"] == [1, 1, 1, 0, 1, 1]
    # a ':' delimiter for hits, and the text is the whole source line
    texts = {p: open(p, encoding="utf-8").read().splitlines() for p, _ in r["files"]}
    for line, flag in zip(r["lines"], r["line_match"]):
        if not flag:
            continue
        for path, src in texts.items():
            if line.startswith(path + ":"):
                ln, _, text = line[len(path) + 1 :].partition(":")
                assert text == src[int(ln) - 1], line
                break
        else:
            pytest.fail("hit line does not belong to a reported file: " + line)


def test_content_mode_lines_ignore_head_limit(tree):
    r = run(tree, mode="content", head_limit=1)
    assert len(r["lines"]) == 6
    assert len(r["line_match"]) == 6


def test_mode_content_ctx_before_uses_dash_delimiter(tree):
    r = run(tree, "miss", mode="content", ctx_before=1)
    assert rel_lines(tree, r) == ["a.py-1-hit one", "a.py:2:miss", "sub/b.py:1:miss"]
    assert r["line_match"] == [0, 1, 1]
    assert "--" not in r["lines"]  # ctx run is adjacent to the match -> merged


def test_mode_content_ctx_after(tree):
    r = run(tree, "miss", mode="content", ctx_after=1)
    assert rel_lines(tree, r) == [
        "a.py:2:miss",
        "a.py-3-HIT two",
        "sub/b.py:1:miss",
        "sub/b.py-2-hit",
    ]
    assert r["line_match"] == [1, 0, 1, 0]


def test_mode_content_ctx_before_and_after_merges_runs(tree):
    r = run(tree, "hit", mode="content", ctx_before=1, ctx_after=1)
    assert rel_lines(tree, r) == [
        "a.py:1:hit one",
        "a.py-2-miss",
        "c.txt:1:hit",
        "d.py:1:hit",
        "d.py-2-x1",
        "d.py-3-x2",
        "d.py:4:hit",
        "sub/b.py-1-miss",
        "sub/b.py:2:hit",
        "sub/b.py-3-trail",
    ]
    assert r["line_match"] == [1, 0, 1, 1, 0, 0, 1, 0, 1, 0]
    # the -B/-A runs of d.py hits 1 and 4 touch, so the "--" separator is gone
    assert "--" not in r["lines"]


def test_mode_content_separator_between_disjoint_runs(tree):
    r = run(tree, "hit", mode="content", ctx_before=1)
    assert rel_lines(tree, r) == [
        "a.py:1:hit one",
        "c.txt:1:hit",
        "d.py:1:hit",
        "--",
        "d.py-3-x2",
        "d.py:4:hit",
        "sub/b.py-1-miss",
        "sub/b.py:2:hit",
    ]
    assert r["line_match"] == [1, 1, 1, 0, 0, 1, 0, 1]
    assert r["lines"].count("--") == 1
    sep = r["lines"].index("--")
    first = next(i for i, l in enumerate(r["lines"]) if l.endswith("d.py:1:hit"))
    last = next(i for i, l in enumerate(r["lines"]) if l.endswith("d.py:4:hit"))
    assert first < sep < last


def test_context_counts_are_not_matches(tree):
    """total_matches/files count MATCHING LINES only, never context lines."""
    plain = run(tree, mode="content")
    ctx = run(tree, mode="content", ctx_before=1, ctx_after=1)
    assert ctx["total_matches"] == plain["total_matches"] == 5
    assert ctx["files"] == plain["files"]
    assert sum(ctx["line_match"]) == ctx["total_matches"]
    assert len(ctx["lines"]) > len(plain["lines"])


# ---------------------------------------------------------------------------
# ignore_case + include_glob
# ---------------------------------------------------------------------------
def test_ignore_case(tree):
    r = run(tree, "hit", ignore_case=True)
    assert rel_files(tree, r) == [("a.py", 2), ("c.txt", 1), ("d.py", 2), ("sub/b.py", 1)]
    assert r["total_matches"] == 6
    assert r["message"] == "6 match(es) in 4 file(s)"
    assert run(tree, "hit", ignore_case=False)["total_matches"] == 5


def test_include_glob_filters_by_file_name(tree):
    r = run(tree, "hit", include_glob="*.py")
    assert rel_files(tree, r) == [("a.py", 1), ("d.py", 2), ("sub/b.py", 1)]
    assert r["message"] == "4 match(es) in 3 file(s)"
    r_txt = run(tree, "hit", include_glob="*.txt")
    assert rel_files(tree, r_txt) == [("c.txt", 1)]
    r_none = run(tree, "hit", include_glob="*")
    assert len(r_none["files"]) == 4


def test_include_glob_applies_to_direct_file_roots(tree):
    direct = os.path.join(tree, "c.txt")
    assert G.run("hit", [direct], tree, include_glob="*.txt")["total_matches"] == 1
    r = G.run("hit", [direct], tree, include_glob="*.py")
    assert r["ok"] is True
    assert r["files"] == [] and r["lines"] == []
    assert r["message"] == "0 match(es) in 0 file(s)"


# ---------------------------------------------------------------------------
# roots / work_dir handling
# ---------------------------------------------------------------------------
def test_relative_roots_resolve_against_work_dir(tree):
    r = G.run("hit", ["sub"], tree)
    assert rel_files(tree, r) == [("sub/b.py", 1)]
    assert r["lines"] and os.path.isabs(r["lines"][0])
    assert r["lines"][0].startswith(tree)


def test_multiple_roots_are_merged(tree):
    r = G.run("hit", [os.path.join(tree, "sub"), os.path.join(tree, "c.txt")], tree)
    assert dict(rel_files(tree, r)) == {"sub/b.py": 1, "c.txt": 1}
    assert len(r["lines"]) == 2
    assert r["total_matches"] == 2
    assert r["message"] == "2 match(es) in 2 file(s)"


def test_missing_root_scans_empty_without_raising(tree):
    r = G.run("hit", [os.path.join(tree, "nope")], tree)
    assert r["files"] == [] and r["lines"] == []
    assert isinstance(r["status"], str) and isinstance(r["message"], str)
    assert r["message"] == "0 match(es) in 0 file(s)"


def test_empty_roots_list(tree):
    r = G.run("hit", [], tree)
    assert r["ok"] is True and r["total_matches"] == 0


def test_non_ascii_path_survives_the_binding(tmp_path):
    """UTF-8 content round-trips; a non-ASCII walk path must not raise.

    The engine renders walk paths through the narrow/ANSI convention, so on a
    non-UTF-8 ANSI code page a non-ASCII path arrives as those bytes. The
    binding decodes them with surrogateescape (byte-lossless, re-encodable)
    instead of letting UnicodeDecodeError escape out of the dict build.
    """
    root = str(tmp_path)
    p = os.path.join(root, "中文.txt")
    with open(p, "w", encoding="utf-8") as f:
        f.write("第一行命中\nsecond\n")
    r = G.run("命中", [p], root, mode="content")
    assert r["ok"] is True
    assert r["total_matches"] == 1 and r["line_match"] == [1]
    # the file TEXT is raw UTF-8 and decodes cleanly
    assert r["lines"][0].endswith(":1:第一行命中")
    path, count = r["files"][0]
    assert count == 1
    assert path.encode("utf-8", "surrogateescape").endswith(b".txt")
    assert r["message"] == "1 match(es) in 1 file(s)"
    # ... and the same corpus with ASCII names round-trips exactly
    ascii_p = os.path.join(root, "ok.txt")
    with open(ascii_p, "w", encoding="utf-8") as f:
        f.write("第一行命中\nsecond\n")
    r2 = G.run("命中", [ascii_p], root, mode="content")
    assert r2["files"] == [(ascii_p, 1)]
    assert r2["lines"] == [ascii_p + ":1:第一行命中"]


# ---------------------------------------------------------------------------
# parameter validation -- the ONLY place this binding throws
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("mode", ["lines", "files", "", "CONTENT", "count", "Content"])
def test_invalid_mode_raises_value_error(tree, mode):
    with pytest.raises(ValueError):
        run(tree, mode=mode)


@pytest.mark.parametrize("mode", ["files_with_matches", "count_matches", "content"])
def test_valid_modes_accepted(tree, mode):
    assert run(tree, mode=mode)["status"] == "ok"


def test_negative_ctx_raises_value_error(tree):
    with pytest.raises(ValueError):
        run(tree, ctx_before=-1)
    with pytest.raises(ValueError):
        run(tree, ctx_after=-1)
    with pytest.raises(ValueError):
        run(tree, ctx_before=-2, ctx_after=3)


def test_bad_argument_types_raise_type_error(tree):
    with pytest.raises(TypeError):
        G.run("hit", tree, tree)  # roots must be list[str], not str
    with pytest.raises(TypeError):
        G.run("hit", (tree,), tree)  # and not a tuple either
    with pytest.raises(TypeError):
        G.run("hit", [1], tree)  # element must be str
    with pytest.raises(TypeError):
        G.run(5, [tree], tree)  # pattern must be str
    with pytest.raises(TypeError):
        G.run(None, [tree], tree)


def test_pinned_defaults_in_signature_docstring():
    sig = G.run.__doc__.splitlines()[0]
    for pinned in (
        "include_glob: str = ''",
        "mode: str = 'files_with_matches'",
        "ignore_case: bool = False",
        "ctx_before:",
        "= 0",
        "head_limit:",
        "= 250",
    ):
        assert pinned in sig, pinned
    ps = G.pattern_supported.__doc__.splitlines()[0]
    assert "ignore_case: bool = False" in ps


def test_keyword_and_positional_forms_agree(tree):
    kw = G.run(
        pattern="hit",
        roots=[tree],
        work_dir=tree,
        include_glob="",
        mode="count_matches",
        ignore_case=False,
        ctx_before=0,
        ctx_after=0,
        head_limit=250,
    )
    pos = G.run("hit", [tree], tree, "", "count_matches", False, 0, 0, 250)
    assert kw == pos


# ---------------------------------------------------------------------------
# engine failures travel in status, not exceptions
# ---------------------------------------------------------------------------
def test_invalid_pattern_is_status_not_exception(tree):
    r = run(tree, "a(")
    assert r["ok"] is False
    assert r["status"] == "invalid_input"
    assert r["message"].startswith("invalid pattern:")
    assert r["files"] == [] and r["lines"] == [] and r["line_match"] == []
    assert r["total_matches"] == 0


def test_status_is_a_snake_case_enumerator_name(tree):
    allowed = {
        "ok",
        "invalid_input",
        "not_found",
        "no_change",
        "ambiguous",
        "blocked",
        "too_large",
        "unsupported",
        "external_library",
    }
    assert run(tree)["status"] in allowed
    assert run(tree, "a(")["status"] in allowed


# ---------------------------------------------------------------------------
# pattern_supported
# ---------------------------------------------------------------------------
SUPPORTED = [
    "hit",
    "foo.*bar",
    r"\d+\.\d+",
    r"[a-z]+[^0-9]",
    r"^anchored$",
    "a|b",
    "(?:group|ed)",
    r"\s\S\w\W\d\D\n\t",
    r"[\d]",
    "lazy.*?quantifier",
    "count{2,3}",
    "x{1,2}?",
]

REJECTED = [
    r"(?=lookahead)",
    r"(?!neg)",
    r"(?<=lookbehind)",
    r"(a)\1",  # back-reference
    r"(?P<n>named)",  # named group
    r"a*+",  # possessive
    r"\bhit\b",  # word boundary not in the subset
    "(?i)foo",  # inline flags not in the subset
    "a(",
    "[z-a]",
]


@pytest.mark.parametrize("pattern", SUPPORTED)
def test_pattern_supported_true(pattern):
    assert G.pattern_supported(pattern) is True


@pytest.mark.parametrize("pattern", REJECTED)
def test_pattern_supported_false(pattern):
    assert G.pattern_supported(pattern) is False


def test_pattern_supported_ignore_case_flag():
    assert G.pattern_supported("foo", ignore_case=True) is True
    assert G.pattern_supported(r"(?=foo)", ignore_case=True) is False


def test_pattern_supported_predicts_run_outcome(tree):
    for pat in SUPPORTED:
        assert run(tree, pat)["ok"] is True, pat
    for pat in REJECTED:
        r = run(tree, pat)
        assert r["ok"] is False and r["status"] == "invalid_input", pat


# ---------------------------------------------------------------------------
# GIL policy: the scan must not deadlock / must stay reentrant under threads
# ---------------------------------------------------------------------------
def test_runs_from_a_worker_thread(tree):
    import threading

    box = {}

    def work():
        box["r"] = run(tree, mode="content")

    t = threading.Thread(target=work)
    t.start()
    t.join(timeout=60)
    assert not t.is_alive()
    assert box["r"]["total_matches"] == 5


def test_gil_released_during_scan(tree):
    """A second thread must make progress while run() is scanning."""
    import threading
    import time

    progress = []
    stop = threading.Event()

    def ticker():
        while not stop.is_set():
            progress.append(time.monotonic())
            time.sleep(0.001)

    # a corpus big enough that the scan takes measurable time
    big = os.path.join(tree, "big")
    os.makedirs(big, exist_ok=True)
    for i in range(200):
        with open(os.path.join(big, "f%03d.txt" % i), "w", encoding="utf-8") as f:
            for j in range(400):
                f.write("line %d noise\n" % j)
                f.write("needle here\n")
    t = threading.Thread(target=ticker)
    t.start()
    try:
        r = G.run("needle", [big], tree, mode="count_matches")
    finally:
        stop.set()
        t.join(timeout=30)
    assert r["total_matches"] == 400 * 200
    assert len(progress) > 5

# ---------------------------------------------------------------------------
# REGRESSION: embedded NUL and non-UTF-8 bytes must survive the conversion
#
# Every engine string crosses into Python through to_py_str(), which uses the
# size-aware PyUnicode_DecodeUTF8(data, size, "surrogateescape"). A
# NUL-terminated conversion (PyUnicode_FromString, or feeding x.c_str() to a
# ctor) would TRUNCATE the rendered text at the first embedded NUL, and a
# strict decoder would RAISE on the engine's raw file bytes. These tests pin
# both failure modes away: content-mode text keeps its NUL byte-exactly on the
# regex path AND the literal fast path, and invalid UTF-8 stays recoverable.
#
# Context for the corpus: the engine sniffs the first 64 KiB for NUL and skips
# such a file silently (rg convention), so the NUL-bearing line is padded past
# that window -- see test_nul_in_the_first_64kib_skips_the_file_entirely.
# ---------------------------------------------------------------------------
NUL_SNIFF_WINDOW = 64 * 1024
NUL_PAD = b"x" * NUL_SNIFF_WINDOW


def nul_padded_file(path, line_text=b"hit\x00tail"):
    """Write `path` = 64 KiB of 'x' + ONE line containing an embedded NUL."""
    with open(path, "wb") as f:
        f.write(NUL_PAD)
        f.write(line_text + b"\n")
    return path


def test_nul_in_content_line_regex_path_is_not_truncated(tmp_path):
    root = str(tmp_path)
    p = nul_padded_file(os.path.join(root, "pad_regex.txt"))
    r = G.run("h.t", [p], root, mode="content")  # metachars -> regex_lite path
    assert r["ok"] is True and r["total_matches"] == 1
    line = r["lines"][0]
    assert chr(0) in line, "rendered text truncated at the embedded NUL"
    expected = p + ":1:" + "x" * NUL_SNIFF_WINDOW + "hit" + chr(0) + "tail"
    assert line == expected
    assert len(line) == len(expected)
    assert r["line_match"] == [1]
    assert r["line_match"] == [1]


def test_nul_in_content_line_literal_fast_path_is_not_truncated(tmp_path):
    """A pure-literal pattern skips regex_lite (memchr + memcmp path) -- the
    rendered line must still keep the NUL."""
    root = str(tmp_path)
    p = nul_padded_file(os.path.join(root, "pad_literal.txt"), b"needle\x00tail")
    r = G.run("needle", [p], root, mode="content")
    assert r["ok"] is True and r["total_matches"] == 1
    line = r["lines"][0]
    assert line == p + ":1:" + "x" * NUL_SNIFF_WINDOW + "needle" + chr(0) + "tail"
    assert chr(0) in line


def test_nul_at_the_start_of_a_matched_line(tmp_path):
    """NUL-leading text is the worst case for a NUL-terminated conversion."""
    root = str(tmp_path)
    p = os.path.join(root, "nul_first.txt")
    with open(p, "wb") as f:
        f.write(NUL_PAD)
        f.write(b"needle\n")
        f.write(chr(0).encode() + b"HEADmatch\n")
    r = G.run("HEADmatch", [p], root, mode="content")
    assert r["total_matches"] == 1
    line = r["lines"][0]
    # the pad has no newline, so "needle" ends line 1 and the NUL leads line 2
    assert line.endswith(chr(0) + "HEADmatch")
    assert line.startswith(p + ":2:")


def test_nul_survives_raw_byte_roundtrip(tmp_path):
    """The Python side can recover the engine's exact bytes."""
    root = str(tmp_path)
    p = nul_padded_file(os.path.join(root, "roundtrip.txt"))
    line = G.run("hit", [p], root, mode="content")["lines"][0]
    raw = line.encode("utf-8", "surrogateescape")
    assert raw.endswith(b"hit\x00tail")
    assert raw.count(b"\x00") == 1
    assert b"\n" not in raw  # the rendered line carries no newline
    assert len(raw) == len(line)  # pure ASCII + NUL -> one byte per code point


def test_nul_kept_in_context_lines(tmp_path):
    root = str(tmp_path)
    p = os.path.join(root, "ctx.txt")
    with open(p, "wb") as f:
        f.write(NUL_PAD)
        f.write(b"before\n")
        f.write(b"needle\x00tail\n")
        f.write(b"after\n")
    r = G.run("needle", [p], root, mode="content", ctx_before=1, ctx_after=1)
    # line 1 = the 64 KiB pad + "before" (ctx), line 2 = the NUL-bearing hit,
    # line 3 = "after" (ctx)
    assert r["line_match"] == [0, 1, 0]
    assert r["lines"][1] == p + ":2:" + "needle" + chr(0) + "tail"
    assert chr(0) in r["lines"][1]
    assert r["lines"][0].endswith("-1-" + "x" * NUL_SNIFF_WINDOW + "before")
    assert r["lines"][2].endswith("-3-after")  # context -> '-' delimiter


def test_invalid_utf8_byte_in_content_line_is_byte_recoverable(tmp_path):
    """Same class of defect without a NUL: a strict decoder would raise."""
    root = str(tmp_path)
    p = os.path.join(root, "bad_utf8.txt")
    with open(p, "wb") as f:
        f.write(b"line1\xff\xfe tail\n")
    r = G.run("line1", [p], root, mode="content")
    assert r["ok"] is True and r["total_matches"] == 1
    line = r["lines"][0]
    assert line.endswith("line1\udcff\udcfe tail")  # one surrogate escape per bad byte
    assert line.encode("utf-8", "surrogateescape").endswith(b"line1\xff\xfe tail")


def test_nul_in_the_first_64kib_skips_the_file_entirely(tmp_path):
    """Engine semantics, pinned so the padded tests above stay meaningful: a NUL
    inside the sniff window marks the file binary and it is skipped silently --
    that is an empty result, NOT a truncated line."""
    root = str(tmp_path)
    p = os.path.join(root, "early_nul.txt")
    open(p, "wb").write(b"line1\x00tail\nsecond line\n")
    r = G.run("line1", [p], root, mode="content")
    assert r["ok"] is True
    assert r["total_matches"] == 0 and r["files"] == [] and r["lines"] == []
    assert r["message"] == "0 match(es) in 0 file(s)"


def test_nul_in_the_pattern_is_not_truncated(tmp_path):
    """Input side: a pattern truncated at its NUL would degrade to "abc" and
    wrongly match a NUL-free line."""
    root = str(tmp_path)
    decoy = os.path.join(root, "decoy.txt")
    open(decoy, "wb").write(b"abcXXXdef\n")
    hit = os.path.join(root, "real.txt")
    with open(hit, "wb") as f:
        f.write(NUL_PAD)
        f.write(b"abc\x00def\n")

    pat = "abc" + chr(0) + "def"
    r_hit = G.run(pat, [hit], root, mode="content")
    assert r_hit["total_matches"] == 1, "pattern lost its NUL / line lost its NUL"
    assert r_hit["lines"][0].endswith("abc" + chr(0) + "def")

    r_decoy = G.run(pat, [decoy], root, mode="content")
    assert r_decoy["total_matches"] == 0, "pattern truncated at the NUL ('abc' matched)"


def test_paths_counts_and_message_unaffected_by_content_nul(tmp_path):
    root = str(tmp_path)
    p = nul_padded_file(os.path.join(root, "pad_paths.txt"))
    r = G.run("hit", [p], root, mode="files_with_matches")
    assert r["lines"] == [p] and r["files"] == [(p, 1)]
    r = G.run("hit", [p], root, mode="count_matches")
    assert r["lines"] == [p + ":1"] and r["message"] == "1 match(es) in 1 file(s)"
    assert chr(0) not in r["message"] and r["status"] == "ok"


@pytest.mark.parametrize("mode", ["files_with_matches", "count_matches", "content"])
def test_nul_corpus_is_consistent_across_modes(tmp_path, mode):
    root = str(tmp_path)
    p = nul_padded_file(os.path.join(root, "all_modes.txt"))
    r = G.run("hit", [p], root, mode=mode)
    assert r["total_matches"] == 1
    assert r["files"] == [(p, 1)]
    assert r["message"] == "1 match(es) in 1 file(s)"
    assert len(r["lines"]) == len(r["line_match"]) == 1
    if mode == "content":
        assert chr(0) in r["lines"][0]
