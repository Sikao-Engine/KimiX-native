"""Differential parity: kimix-base's Glob walker vs kimi-agent's Glob tool.

The native kernels under ``src/builtin_tools/glob_tool.*`` claim to mirror
``kimi_cli/tools/file/glob.py`` (its ``Glob.__call__`` walk + sort) plus the
``pathlib`` / ``kaos`` semantics that file delegates to.  This module compares

    native   runtime_py.builtin_tools.file.walk_matches_fs(
                 root, pattern, include_dirs, max_matches, ignore_rules)
    python   the *real* collection loop of Glob.__call__ (glob.py:569-605),
             driven by the real ``KaosPath.glob()`` / ``KaosPath.sort()``

over a synthetic on-disk tree, for the pattern corpus below.

Why the reference is built this way (and not with a paraphrase of pathlib):

* ``Glob.__call__`` calls ``dir_path.glob(pattern)`` on a ``KaosPath``.
  ``KaosPath.glob``'s signature is ``(pattern, *, case_sensitive: bool = True)``
  (kaos/path.py:153) and ``LocalKaos.glob`` forwards that straight into
  ``pathlib.Path.glob(pattern, case_sensitive=...)`` (kaos/local.py:111), so the
  shipped tool asks pathlib for **case-sensitive** matching on every platform -
  including Windows, where ``pathlib``'s own default would be case-insensitive.
  ``kimi-cli/tests/kaos/test_ssh_kaos.py::test_glob_is_case_sensitive`` pins that
  intent for the sibling backend.
* ``matches.sort()`` (glob.py:605) sorts ``KaosPath`` objects, whose ``__lt__``
  delegates to ``PurePath.__lt__`` (kaos/path.py:34) - i.e. Windows sorts by
  ``ntpath.normcase`` (lowercase + '/' -> '\\\\'), POSIX by code point.

Both are behaviours of the shipped tool that a naive re-implementation misses,
so the harness runs the real ones instead of re-deriving them.
"""

from __future__ import annotations

import asyncio
import os
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------------------
# Import the freshly built extension BEFORE anything imports kimi_cli.
#
# ``kimi_cli.native_loader`` stages ITS OWN native library: it inserts
# ``<kimi-agent>/bin`` at sys.path[0] and imports ``kimix_native`` from there,
# where an older released ``runtime_py.pyd`` lives (``C:/dev/kimi-agent/bin`` at
# the time of writing).  Any ``import runtime_py`` after that resolves to the
# foreign library and silently compares the port against an older build of
# itself.  So the repo's own bin/<mode> is forced to the front here, and the
# module refuses to run against a foreign extension at all.
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

pytestmark = pytest.mark.skipif(
    not ref_available(), reason=f"kimi-agent checkout not found at {KIMI_AGENT_ROOT}"
)

FILE = runtime_py.builtin_tools.file

MAX_MATCHES = 1000  # glob.py:35

# Real kaos backend, so KaosPath.glob()/sort() behave exactly like the tool's.
_kaos = ref("kaos")
_kaos.set_current_kaos(ref("kaos.local").LocalKaos())
KaosPath = ref("kaos.path").KaosPath


# ---------------------------------------------------------------------------
# reference (kimi-agent) and native walkers
# ---------------------------------------------------------------------------


def ref_walk(
    root: Path,
    pattern: str,
    include_dirs: bool = False,
    max_matches: int = MAX_MATCHES,
    rules=None,
):
    """Verbatim replica of the collection half of Glob.__call__ (glob.py:558-606).

    Returns ``(relative_paths, truncated, ignored_count)`` with the relative
    paths '\\'-normalized to '/' so they can be compared with the native side.
    """
    g = ref("kimi_cli.tools.file.glob")

    async def run():
        dir_path = KaosPath.unsafe_from_local_path(root)
        resolved_dir = Path(str(dir_path)).resolve()
        matches = []
        truncated = False
        ignored_count = 0
        with pure_python(g):
            async for match in dir_path.glob(pattern):  # case_sensitive=True
                if not include_dirs and not await match.is_file():
                    continue
                if rules:
                    match_path = Path(str(match))
                    match_resolved = match_path.resolve()
                    if g._is_ignored_by_gitignore(match_resolved, rules, resolved_dir):
                        ignored_count += 1
                        continue
                    if match_resolved != match_path and g._is_ignored_by_gitignore(
                        match_path, rules, resolved_dir
                    ):
                        ignored_count += 1
                        continue
                matches.append(match)
                if len(matches) > max_matches:
                    truncated = True
                    matches.pop()
                    break
        matches.sort()  # KaosPath.__lt__ -> PurePath.__lt__ (normcase on Windows)
        rel = [str(p.relative_to(dir_path)).replace("\\", "/") for p in matches]
        return rel, truncated, ignored_count

    return asyncio.run(run())


def native_walk(
    root: Path,
    pattern: str,
    include_dirs: bool = False,
    max_matches: int = MAX_MATCHES,
    ignore_rules=None,
):
    """Native walker, exactly as the pybind11 binding exposes it.

    ``max_matches=0`` means *unlimited* in the native API, whereas the Python
    tool always caps at MAX_MATCHES, hence the default above.
    """
    return FILE.walk_matches_fs(
        str(root), pattern, include_dirs, max_matches, ignore_rules
    )


def gitignore_rules(content: str, source_dir: Path):
    """(python rules, native tuples) for one .gitignore file."""
    g = ref("kimi_cli.tools.file.glob")
    with pure_python(g):
        py_rules = g._parse_gitignore(content, source_dir)
    tuples = [
        (r.pattern, bool(r.negated), bool(r.anchored), bool(r.is_dir_only))
        for r in py_rules
    ]
    return py_rules, tuples


# ---------------------------------------------------------------------------
# synthetic tree (mirrors kimi-cli/tests/tools/test_glob.py's test_files fixture
# plus the extra shapes: case-only differences, dotfiles, brackets, spaces,
# unicode, empty dirs, symlink-free junk directories)
# ---------------------------------------------------------------------------

FILES = [
    # kimi-cli/tests/tools/test_glob.py::test_files
    "README.md",
    "setup.py",
    "src/main.py",
    "src/utils.py",
    "src/main/app.py",
    "src/main/config.py",
    "src/test/test_app.py",
    "src/test/test_config.py",
    "docs/guide.md",
    "docs/api.md",
    # kimi-cli/tests/tools/test_glob.py edge cases
    ".gitlab-ci.yml",
    ".eslintrc.json",
    "config.yml",
    "src/.config/settings.yml",
    ".github/workflows/ci.yml",
    "a/b/c/d/deep.txt",
    "file1.py",
    "file2.py",
    "file3.txt",
    # case-only probe (Windows case-insensitivity trap)
    "A.PY",
    # ordering probes (pathlib sort = normcase on Windows)
    "node_modules/pkg/index.js",
    ".venv/lib/site.py",
    # odd names
    "space dir/with space.txt",
    "br[ack]et.txt",
    "unicode-\u00e9\u4e16\u754c.txt",
    "empty.txt",
]

DIRS = [
    "docs/main",  # empty, for `docs/**/main/*.py`
    "nodir",
    "empty dir",
]


def _build_tree(root: Path) -> None:
    for d in DIRS:
        (root / d).mkdir(parents=True, exist_ok=True)
    for rel in FILES:
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("x")


@pytest.fixture(scope="module")
def tree(tmp_path_factory) -> Path:
    root = tmp_path_factory.mktemp("glob_tree")
    _build_tree(root)
    return root


# pattern corpus: `dir_only` (trailing '/'), `**` in every position, `?`,
# bracket classes, negated classes, dotfiles, separators, unicode, spaces,
# empty results, and the exact patterns kimi-cli's own test suite exercises.
PATTERNS = [
    # kimi-cli/tests/tools/test_glob.py vectors
    "*.py",
    "*.md",
    "**/*.py",
    "src/**/*.py",
    "main/**/*.py",
    "src/**/*test*.py",
    "*.xyz",
    "test_*",
    "**/*.txt",
    "file[1-2].py",
    "docs/**/main/*.py",
    "**/main/*.py",
    "src/**/test_*.py",
    "src/**",
    "*.yml",
    "src/**/*.yml",
    ".github/**/*.yml",
    "a/**/deep.txt",
    "?.md",
    "?.py",
    "test_file.txt",
    "*",
    # wildcards / classes / anchors
    "?.md",
    "??.py",
    "file[1-2].txt",
    "file[13].txt",
    "[!f]*.txt",
    "[br]*.txt",
    "br[ack]et.txt",
    "*.txt",
    "src/*.py",
    "src/*/*.py",
    "src/*",
    "*/",
    "*/*.txt",
    "*/*/",
    "**/index.js",
    "a/**/b",
    "a/**",
    "empty*",
    "no_such_dir/*",
    "**/*.xyz",
    "space dir/*.txt",
    "unicode-*.txt",
    # `**` in every position
    "**",
    "**/*",
    "**/",
    "*/**/*.py",
    "**/.config/*",
    "src/.config/*.yml",
    "node_modules/**",
    ".venv/**",
    "docs/*.md",
    # case probes (must stay case-sensitive, like the shipped tool)
    "*.PY",
    "A.PY",
    "**/*.PY",
    "src/MAIN.PY",
    # path-shape normalization
    "./src/*.py",
    "src//*.py",
    "src/",
    "src/**/",
    "src/*/",
]


@pytest.mark.parametrize("include_dirs", [False, True], ids=["files", "files+dirs"])
@pytest.mark.parametrize("pattern", PATTERNS)
def test_walk_matches_fs_parity(tree, pattern, include_dirs):
    expected, _, _ = ref_walk(tree, pattern, include_dirs)
    got = native_walk(tree, pattern, include_dirs)
    assert got == expected


# ---------------------------------------------------------------------------
# kimi-agent's own tool-level expectations (counts + membership), so the
# comparison above cannot silently pass with both sides empty/wrong.
# ---------------------------------------------------------------------------


def _expected_paths(tree, pattern, include_dirs=False):
    return ref_walk(tree, pattern, include_dirs)[0]


def test_recursive_pattern_matches_root_files(tree):
    """kimi-cli test_glob_recursive_pattern: `**/*.py` finds root files too.

    kimi's own fixture yields 7 matches; this tree adds file1.py, file2.py and
    .venv/lib/site.py, so the same rule produces 10.
    """
    got = native_walk(tree, "**/*.py")
    assert "setup.py" in got
    assert "src/main.py" in got
    assert "src/main/app.py" in got
    assert "src/test/test_app.py" in got
    assert len(got) == 10  # 7 of kimi's fixture + file1/file2 + .venv/lib/site.py
    assert got == ref_walk(tree, "**/*.py")[0]


def test_safe_recursive_pattern(tree):
    """kimi-cli test_glob_safe_recursive_pattern: `src/**/*.py` -> 6 matches."""
    got = native_walk(tree, "src/**/*.py")
    assert sorted(got) == sorted(
        [
            "src/main.py",
            "src/utils.py",
            "src/main/app.py",
            "src/main/config.py",
            "src/test/test_app.py",
            "src/test/test_config.py",
        ]
    )
    assert got == ref_walk(tree, "src/**/*.py")[0]


def test_no_slash_pattern_is_anchored_at_the_root(tree):
    """pathlib anchors a '/'-free pattern at the search root (so `*.py` does
    NOT find `src/main.py`), even though the tool description claims basename
    matching at any depth.  The shipped tool is the pathlib behaviour."""
    got = native_walk(tree, "*.py")
    assert got == ["file1.py", "file2.py", "setup.py"]
    assert native_walk(tree, "*.md") == ["README.md"]


def test_dotfiles_are_matched(tree):
    """kimi-cli test_glob_hidden_files / hidden_directory_contents."""
    got = native_walk(tree, "*.yml")
    assert ".gitlab-ci.yml" in got
    assert "config.yml" in got
    got = native_walk(tree, "src/**/*.yml")
    assert "src/.config/settings.yml" in got
    got = native_walk(tree, ".github/**/*.yml")
    assert got == [".github/workflows/ci.yml"]


def test_empty_result(tree):
    assert native_walk(tree, "*.xyz") == []
    assert native_walk(tree, "docs/**/main/*.py") == []
    assert native_walk(tree, "**/*.xyz") == []


def test_include_dirs_tree_dirs_only(tree):
    """kimi-cli test_glob_include_dirs_true / test_glob_exclude_directories."""
    got = native_walk(tree, "*", include_dirs=True)
    assert "src" in got and "empty dir" in got
    files = native_walk(tree, "*", include_dirs=False)
    assert "src" not in files and "empty dir" not in files


def test_character_class_pattern(tree):
    """kimi-cli test_glob_character_class (file[1-2].py)."""
    got = native_walk(tree, "file[1-2].py")
    assert got == ["file1.py", "file2.py"]
    assert native_walk(tree, "file[1-2].txt") == []


# ---------------------------------------------------------------------------
# regression: case sensitivity of the walk (kaos case_sensitive=True)
# ---------------------------------------------------------------------------


def test_walk_is_case_sensitive_on_windows(tmp_path):
    """The shipped tool is case-SENSITIVE on every platform.

    Minimal repro (before the fix):
      input   root/{A.PY}, pattern '*.py'
      python  []                      (KaosPath.glob -> Path.glob(case_sensitive=True))
      native  ['A.PY']                (walk_matches_fs -> parse_pattern_default_case)
    """
    root = tmp_path / "case"
    root.mkdir()
    (root / "A.PY").write_text("x")
    (root / "b.py").write_text("x")
    assert ref_walk(root, "*.py")[0] == ["b.py"]
    assert native_walk(root, "*.py") == ["b.py"]
    assert ref_walk(root, "*.PY")[0] == ["A.PY"]
    assert native_walk(root, "*.PY") == ["A.PY"]
    assert ref_walk(root, "**/*.py")[0] == ["b.py"]
    assert native_walk(root, "**/*.py") == ["b.py"]
    # include_dirs must not change the case rule
    (root / "SUB").mkdir()
    assert native_walk(root, "sub", include_dirs=True) == []


# ---------------------------------------------------------------------------
# regression: match ordering (matches.sort() is normcase order on Windows)
# ---------------------------------------------------------------------------


def test_walk_sort_order_matches_pathlib_normcase(tmp_path):
    """`matches.sort()` sorts KaosPath objects -> PurePath.__lt__.

    Python 3.14 compares `_parts_normcase`: str(path).lower().split(sep) as a
    *tuple* (pathlib.PurePath.__lt__), i.e. component by component - not the
    joined string.  Minimal repro (before the fix), root {README.md, docs/a.md}:
      python  ['docs/a.md', 'README.md']  (tuple ('docs','a.md') < ('readme.md',))
      native  ['README.md', 'docs/a.md']  (b byte order)
    and the component rule also beats byte order inside one directory level:
      ('src','a.py') < ('src0.py',)  although '0' (0x30) < '/' (0x2F) is false.
    """
    root = tmp_path / "order"
    (root / "docs").mkdir(parents=True)
    (root / "README.md").write_text("x")
    (root / "docs" / "a.md").write_text("x")
    (root / "src0.py").write_text("x")
    (root / "src").mkdir()
    (root / "src" / "a.py").write_text("x")

    expected = ref_walk(root, "**/*.md")[0]
    assert expected == ["docs/a.md", "README.md"]
    assert native_walk(root, "**/*.md") == expected
    expected_py = ref_walk(root, "**/*.py")[0]
    assert expected_py == ["src/a.py", "src0.py"]
    assert native_walk(root, "**/*.py") == expected_py


# ---------------------------------------------------------------------------
# regression: trailing-slash (dir-only) patterns obey the include_dirs gate
# ---------------------------------------------------------------------------


def test_dir_only_pattern_still_needs_include_dirs(tmp_path):
    """pathlib yields directories for a trailing-'/' pattern, but the tool then
    applies `if not params.include_dirs and not is_file: continue`
    (glob.py:570) - so a dir-only pattern returns nothing by default.

    Minimal repro (before the fix):
      input   root/{src/a.py}, pattern 'src/', include_dirs=False
      python  []            (the only yielded entry is the dir `src`)
      native  ['src']       (dir-only matches bypassed the include_dirs gate)
    """
    root = tmp_path / "dironly"
    (root / "src").mkdir(parents=True)
    (root / "src" / "a.py").write_text("x")
    (root / "srcfile").write_text("x")

    assert ref_walk(root, "src/")[0] == []
    assert native_walk(root, "src/") == []
    assert ref_walk(root, "src/", include_dirs=True)[0] == ["src"]
    assert native_walk(root, "src/", include_dirs=True) == ["src"]
    # `*/` and `**/` behave the same way
    assert native_walk(root, "*/") == ref_walk(root, "*/")[0] == []
    assert native_walk(root, "**/", include_dirs=True) == [
        ".",  # pathlib yields the search root itself for a nullable pattern
        "src",
    ]
    assert native_walk(root, "**/") == []


# ---------------------------------------------------------------------------
# regression: symlinked / junctioned directories
# ---------------------------------------------------------------------------


def test_walk_does_not_recurse_into_symlinked_dirs(tmp_path):
    """pathlib's '**' does not follow symlinks (3.13+ default)."""
    root = tmp_path / "symlink"
    (root / "real").mkdir(parents=True)
    (root / "a.py").write_text("x")
    (root / "real" / "b.py").write_text("x")
    try:
        os.symlink(root / "real", root / "link", target_is_directory=True)
    except (OSError, NotImplementedError) as exc:  # pragma: no cover
        pytest.skip(f"directory symlinks unavailable: {exc}")
    if not (root / "link").is_symlink():  # pragma: no cover
        pytest.skip("symlink creation did not produce a symlink")
    assert ref_walk(root, "**/*.py")[0] == ["a.py", "real/b.py"]
    assert native_walk(root, "**/*.py") == ["a.py", "real/b.py"]
    assert native_walk(root, "**", include_dirs=True) == ref_walk(
        root, "**", include_dirs=True
    )[0]


def test_walk_does_recurse_into_junctions(tmp_path):
    """A Windows directory junction is NOT a symlink for Python.

    ``os.DirEntry.is_symlink()`` is False for IO_REPARSE_TAG_MOUNT_POINT (only
    ``is_junction()`` is True), so pathlib's '**' descends into it - uv creates
    ``.venv`` exactly that way.  Minimal repro (before the fix):
      tree    root/{a.py, real/b.py, link -> real (junction)}
      python  ['a.py', 'link/b.py', 'real/b.py']
      native  ['a.py', 'real/b.py']
    """
    if os.name != "nt":  # pragma: no cover
        pytest.skip("junctions are Windows-only")
    root = tmp_path / "junction"
    (root / "real").mkdir(parents=True)
    (root / "a.py").write_text("x")
    (root / "real" / "b.py").write_text("x")
    created = subprocess.run(
        ["cmd", "/c", "mklink", "/J", str(root / "link"), str(root / "real")],
        capture_output=True,
    )
    if created.returncode != 0 or not (root / "link").is_junction():  # pragma: no cover
        pytest.skip("could not create a junction")
    assert (root / "link").is_symlink() is False  # what Python sees
    assert ref_walk(root, "**/*.py")[0] == ["a.py", "link/b.py", "real/b.py"]
    assert native_walk(root, "**/*.py") == ["a.py", "link/b.py", "real/b.py"]
    assert native_walk(root, "**", include_dirs=True) == ref_walk(
        root, "**", include_dirs=True
    )[0]


# ---------------------------------------------------------------------------
# max_matches boundary
# ---------------------------------------------------------------------------


@pytest.fixture(scope="module")
def flat_tree(tmp_path_factory) -> Path:
    root = tmp_path_factory.mktemp("glob_flat")
    for i in range(MAX_MATCHES + 2):
        (root / f"file_{i:05d}.txt").write_text("x")
    return root


@pytest.fixture(scope="module")
def exact_tree(tmp_path_factory) -> Path:
    root = tmp_path_factory.mktemp("glob_exact")
    for i in range(25):
        (root / f"file_{i:02d}.txt").write_text("x")
    return root


def test_exactly_max_matches_is_not_capped(exact_tree):
    """glob.py:597-600 only aborts on the (N+1)-th candidate, so exactly N
    matches must come back complete and not truncated."""
    limit = 25
    r, truncated, _ = ref_walk(exact_tree, "*.txt", max_matches=limit)
    assert len(r) == limit
    assert truncated is False
    got = native_walk(exact_tree, "*.txt", max_matches=limit)
    assert got == r


def test_max_matches_cap_is_not_off_by_one(exact_tree):
    """One file below the cap: nothing is dropped either."""
    r, truncated, _ = ref_walk(exact_tree, "*.txt", max_matches=26)
    assert len(r) == 25
    assert truncated is False
    assert native_walk(exact_tree, "*.txt", max_matches=26) == r


def test_over_max_matches_is_capped_at_max_matches(flat_tree):
    limit = MAX_MATCHES + 1  # tree holds MAX_MATCHES + 2 files
    r, truncated, _ = ref_walk(flat_tree, "*.txt", max_matches=limit)
    assert len(r) == limit
    assert truncated is True
    got = native_walk(flat_tree, "*.txt", max_matches=limit)
    assert got == r
    # the default (MAX_MATCHES) caps the collection at exactly MAX_MATCHES
    r2, truncated2, _ = ref_walk(flat_tree, "*.txt")
    assert len(r2) == MAX_MATCHES
    assert truncated2 is True
    assert native_walk(flat_tree, "*.txt") == r2


def test_max_matches_cap_is_walk_order_then_sort(flat_tree):
    """glob.py:597-600 pops the overflow candidate during collection and sorts
    afterwards, so the capped result is the first N collected, sorted."""
    got = native_walk(flat_tree, "*.txt", max_matches=5)
    assert got == ref_walk(flat_tree, "*.txt", max_matches=5)[0]
    assert got == sorted(got)
    assert len(got) == 5
    assert got == [f"file_{i:05d}.txt" for i in range(5)]


# ---------------------------------------------------------------------------
# gitignore filter applied by the walker (glob.py:573-595)
# ---------------------------------------------------------------------------

GITIGNORE_VECTORS = [
    ".venv/\nnode_modules/\n",
    ".venv/\nnode_modules/\n*.md\n!README.md\n",
    "*.txt\n!/file1.txt\n",
    "docs/\n",
    "**/pkg\n",
    "src/**/*.py\n",
]


@pytest.mark.parametrize("include_dirs", [False, True], ids=["files", "files+dirs"])
@pytest.mark.parametrize("content", GITIGNORE_VECTORS)
@pytest.mark.parametrize("pattern", ["**/*", "**/*.py", "**/*.md", "**/*.txt", "*"])
def test_walk_gitignore_filter_parity(tree, pattern, content, include_dirs):
    py_rules, tuples = gitignore_rules(content, tree.resolve())
    expected, _, _ = ref_walk(tree, pattern, include_dirs, rules=py_rules)
    got = native_walk(tree, pattern, include_dirs, ignore_rules=tuples)
    assert got == expected


def test_walk_gitignore_dir_only_rule_excludes_descendants(tmp_path):
    """kimi-cli test_glob_gitignore_dir_only_excludes_descendants."""
    root = tmp_path / "gi"
    (root / ".venv").mkdir(parents=True)
    (root / "sub" / ".venv").mkdir(parents=True)
    (root / "keep.py").write_text("x")
    (root / ".venv" / "a.py").write_text("x")
    (root / "sub" / ".venv" / "b.py").write_text("x")
    py_rules, tuples = gitignore_rules(".venv/\n", root.resolve())
    expected, _, _ = ref_walk(root, "**/*.py", rules=py_rules)
    assert expected == ["keep.py"]
    assert native_walk(root, "**/*.py", ignore_rules=tuples) == ["keep.py"]


def test_walk_gitignore_negation_unignores(tmp_path):
    """kimi-cli test_glob_gitignore_dir_only_negated_unignores."""
    root = tmp_path / "gi2"
    (root / ".venv" / "sub").mkdir(parents=True)
    (root / ".venv" / "a.py").write_text("x")
    (root / ".venv" / "sub" / "b.py").write_text("x")
    py_rules, tuples = gitignore_rules(".venv/\n!.venv/\n", root.resolve())
    expected, _, _ = ref_walk(root, "**/*.py", rules=py_rules)
    assert expected == [".venv/a.py", ".venv/sub/b.py"]
    assert native_walk(root, "**/*.py", ignore_rules=tuples) == expected


# ---------------------------------------------------------------------------
# rejected patterns
# ---------------------------------------------------------------------------

BAD_PATTERNS = ["", ".", "/", "//", "C:/tmp/*", "/abs/*.py", "./", "C:\\tmp\\*"]


@pytest.mark.parametrize("pattern", BAD_PATTERNS)
def test_rejected_patterns(tree, pattern):
    """The Python tool routes a pathlib pattern error into ToolError; the
    native binding raises ValueError for the same inputs."""
    with pytest.raises((ValueError, NotImplementedError)):
        ref_walk(tree, pattern)
    with pytest.raises(ValueError):
        native_walk(tree, pattern)


def test_unsafe_patterns_are_rejected_by_the_kernel(tree):
    """Both sides block the same '**'-everything patterns (glob.py:514)."""
    g = ref("kimi_cli.tools.file.glob")
    with pure_python(g):
        for pattern in ["**", "**/*", "**/**", "**\\*", "*/**", "*/*"]:
            assert FILE.is_unsafe_recursive_pattern(pattern) == (
                g._is_unsafe_recursive_pattern(pattern)
            ), pattern
