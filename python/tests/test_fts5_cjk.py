# Test for the built fts5_cjk SQLite extension (cjk_unicode61 tokenizer).
# This test covers (end-to-end through stdlib sqlite3 load_extension):
# - extension discovery in bin/<mode> (release/releasedbg/debug)
# - CREATE VIRTUAL TABLE ... tokenize='cjk_unicode61'
# - 2-char CJK terms match at index speed (the unicode61/trigram gap)
# - 1-char CJK unigram, substring bigram match, ASCII passthrough
# - extra unicode61 args passthrough (remove_diacritics)
# - explicit entrypoint sqlite3_ftscjk_init
# - basename-derived entrypoint (sqlite3_fts5_cjk_init alias)
#
# The extension must be built first: `xmake build` (see tests/xmake.lua /
# src/xmake.lua target "fts5_cjk").

import os
import sqlite3
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]  # kimix-base repository root

# Platform-specific extension filename (xmake shared target naming).
_EXT_NAMES = ("fts5_cjk.dll", "libfts5_cjk.so")


def _find_extension() -> Path | None:
    """Locate the built fts5_cjk extension in bin/<mode> (prefer release)."""
    for mode in ("release", "releasedbg", "debug"):
        cand_dir = ROOT / "bin" / mode
        if not cand_dir.is_dir():
            continue
        for name in _EXT_NAMES:
            p = cand_dir / name
            if p.is_file():
                return p
    return None


def _open_with_extension(entrypoint: str | None = None) -> sqlite3.Connection:
    """Open an in-memory connection with the fts5_cjk extension loaded."""
    ext_path = _find_extension()
    if ext_path is None:
        pytest.skip("fts5_cjk extension not built; run 'xmake build' first")
    con = sqlite3.connect(":memory:")
    try:
        con.enable_load_extension(True)
    except Exception as exc:  # pragma: no cover - build-dependent
        con.close()
        pytest.skip(f"sqlite3 load_extension unavailable: {exc}")
    if entrypoint is not None:
        con.load_extension(str(ext_path), entrypoint=entrypoint)
    else:
        con.load_extension(str(ext_path))
    return con


@pytest.fixture()
def con() -> sqlite3.Connection:
    conn = _open_with_extension()
    yield conn
    conn.close()


def _rows(con: sqlite3.Connection, query: str) -> list[str]:
    return [r[0] for r in con.execute(query).fetchall()]


def test_extension_artifact_exists():
    ext = _find_extension()
    assert ext is not None, (
        "fts5_cjk extension not built; run 'xmake build' (target fts5_cjk)"
    )
    assert ext.stat().st_size > 0


def test_cjk_unicode61_virtual_table_2char_match(con):
    con.execute("CREATE VIRTUAL TABLE t USING fts5(content, tokenize='cjk_unicode61')")
    con.execute("INSERT INTO t VALUES (?)", ("오늘은 일본 여행을 갑니다",))
    con.execute("INSERT INTO t VALUES (?)", ("中文搜索测试",))
    con.execute("INSERT INTO t VALUES (?)", ("안녕하세요",))
    # 2-char Korean term — the gap unicode61 (whole-run token) and trigram
    # (>=3 chars) cannot serve.
    assert _rows(con, "SELECT content FROM t WHERE t MATCH '일본'") == [
        "오늘은 일본 여행을 갑니다"
    ]
    # 2-char Chinese term.
    assert _rows(con, "SELECT content FROM t WHERE t MATCH '中文'") == [
        "中文搜索测试"
    ]


def test_cjk_substring_bigram_match(con):
    con.execute("CREATE VIRTUAL TABLE t USING fts5(content, tokenize='cjk_unicode61')")
    con.execute("INSERT INTO t VALUES (?)", ("캘린더일본",))
    # Interior bigram of a 4-char run: 캘린더일본 -> 캘린 린더 더일 일본
    assert _rows(con, "SELECT content FROM t WHERE t MATCH '린더'") == ["캘린더일본"]
    assert _rows(con, "SELECT content FROM t WHERE t MATCH '일본'") == ["캘린더일본"]


def test_cjk_1char_unigram_match(con):
    con.execute("CREATE VIRTUAL TABLE t USING fts5(content, tokenize='cjk_unicode61')")
    con.execute("INSERT INTO t VALUES (?)", ("a\u4e2db",))  # 'a' + 中 (lone CJK run) + 'b'
    con.execute("INSERT INTO t VALUES (?)", ("\u4e2d",))  # 中 alone -> unigram
    # A lone CJK char in content is emitted as a unigram, so a 1-char query
    # matches content where the char forms its own run.
    assert _rows(con, "SELECT content FROM t WHERE t MATCH '\u4e2d'") == [
        "a\u4e2db", "\u4e2d"
    ]
    # 1-char query does NOT match inside a multi-char run (bigram-only index):
    # that is the documented trigram/LIKE fallback gap this tokenizer closes
    # for 2-char terms only.
    con.execute("INSERT INTO t VALUES (?)", ("\u4e2d\u6587\u5b57\u7b26",))  # 中文字符
    assert _rows(con, "SELECT content FROM t WHERE t MATCH '\u4e2d'") == [
        "a\u4e2db", "\u4e2d"
    ]


def test_ascii_passthrough_still_works(con):
    con.execute("CREATE VIRTUAL TABLE t USING fts5(content, tokenize='cjk_unicode61')")
    con.execute("INSERT INTO t VALUES (?)", ("SQLite FTS5 full text search",))
    con.execute("INSERT INTO t VALUES (?)", ("日本語のテスト",))
    assert _rows(con, "SELECT content FROM t WHERE t MATCH 'sqlite'") == [
        "SQLite FTS5 full text search"
    ]
    assert _rows(con, "SELECT content FROM t WHERE t MATCH 'full'") == [
        "SQLite FTS5 full text search"
    ]


def test_unicode61_args_passthrough(con):
    con.execute(
        "CREATE VIRTUAL TABLE t USING fts5(content, "
        "tokenize='cjk_unicode61 remove_diacritics 2')"
    )
    con.execute("INSERT INTO t VALUES (?)", ("café 中文",))
    # remove_diacritics 2: é indexes as 'e' (tokenizer arg reaches unicode61).
    assert _rows(con, "SELECT content FROM t WHERE t MATCH 'cafe'") == ["café 中文"]
    assert _rows(con, "SELECT content FROM t WHERE t MATCH '中文'") == ["café 中文"]


def test_explicit_entrypoint_sqlite3_ftscjk_init():
    con = _open_with_extension(entrypoint="sqlite3_ftscjk_init")
    try:
        con.execute("CREATE VIRTUAL TABLE t USING fts5(content, tokenize='cjk_unicode61')")
        con.execute("INSERT INTO t VALUES (?)", ("안녕하세요",))
        assert _rows(con, "SELECT content FROM t WHERE t MATCH '안녕'") == ["안녕하세요"]
    finally:
        con.close()


def test_basename_entrypoint_alias():
    # load_extension without an explicit entrypoint derives
    # sqlite3_<basename> (fts5_cjk -> sqlite3_fts5_cjk_init, the alias; on
    # Linux the 'lib' prefix is stripped and the same alias is tried).
    con = _open_with_extension()
    try:
        con.execute("CREATE VIRTUAL TABLE t USING fts5(content, tokenize='cjk_unicode61')")
        con.execute("INSERT INTO t VALUES (?)", ("日本語",))
        assert _rows(con, "SELECT content FROM t WHERE t MATCH '日本'") == ["日本語"]
    finally:
        con.close()
