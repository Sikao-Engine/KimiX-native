// test_sqlite_history_index.cpp - unit tests for the durable SQLite FTS5
// history index (src/runtime/index/sqlite_history_index.{h,cpp} + fts5_query.h),
// the native port of kimi_cli/soul/history_index.py's apsw backend.
//
// Covers the report.md section D rows:
// - D3: schema/DDL created (turns, meta, turns_fts, turns_fts_trigram, the
//   3 sync triggers), WAL sidecar exists, append + search round-trip
// - D6: turn-id authority — the index assigns ids, ignores caller ids,
//   re-syncs from MAX(turn_id)+1 on reopen, collision-proof
// - D7: raw verbatim text stored (byte-exact recall incl. CJK + casing)
// - D8: sanitize_fts5_query matrix (quotes, specials, dangling booleans,
//   2048 cap, dotted/hyphenated re-quoting), CJK routing (trigram + LIKE
//   fallback), fts_stale degradation + rebuild_fts() repair
// - D9: no 500-turn cap (600 turns all searchable), append-after-search
//   incrementality, fuzzy typo retry, min_should_match gate,
//   search_with_recency recency weighting
// - persistence across close/reopen
//
// All test logic lives in main() scope; no file-scope static registrations.
// Every test works inside its own directory under the system temp dir.

#include "ut/ut.hpp"

#include <runtime/index/fts5_query.h>
#include <runtime/index/sqlite_history_index.h>

#include <core/stl/filesystem.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <system_error>

#include "sqlite3.h"

using namespace boost::ut;
using namespace boost::ut::literals;
using kimix::runtime::index::SqliteHistoryIndex;
using kimix::runtime::index::turn_meta;

namespace {

kimix::filesystem::path test_dir(const char *name) {
    std::error_code ec;
    kimix::filesystem::path dir = kimix::filesystem::temp_directory_path(ec) /
                                  "kimix_sqlite_history_index_test" / name;
    kimix::filesystem::remove_all(dir, ec);
    kimix::filesystem::create_directories(dir, ec);
    return dir;
}

turn_meta make_turn(kimix::string_view text, uint8_t role = 0,
                    double ts = 1.0) {
    turn_meta t;
    t.turn_id = 0; // durable index assigns its own ids (D6)
    t.timestamp = ts;
    t.role = role;
    t.is_compacted = false;
    t.text.assign(text.data(), text.size());
    return t;
}

void append_one(SqliteHistoryIndex &idx, kimix::string_view text,
                uint8_t role = 0, double ts = 1.0) {
    const turn_meta turns[1] = {make_turn(text, role, ts)};
    idx.append_turns(kimix::span<const turn_meta>(turns, 1));
}

// One raw SQL helper so schema/corruption assertions do not depend on the
// class under test.
bool raw_exec(const kimix::filesystem::path &db, const char *sql) {
    sqlite3 *conn = nullptr;
    if (sqlite3_open_v2(db.string().c_str(), &conn,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        nullptr) != SQLITE_OK) {
        if (conn != nullptr) {
            sqlite3_close(conn);
        }
        return false;
    }
    char *errmsg = nullptr;
    const int rc = sqlite3_exec(conn, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        sqlite3_free(errmsg);
        sqlite3_close(conn);
        return false;
    }
    sqlite3_close(conn);
    return true;
}

bool raw_has_row(const kimix::filesystem::path &db, const char *sql) {
    sqlite3 *conn = nullptr;
    if (sqlite3_open_v2(db.string().c_str(), &conn, SQLITE_OPEN_READWRITE,
                        nullptr) != SQLITE_OK) {
        if (conn != nullptr) {
            sqlite3_close(conn);
        }
        return false;
    }
    sqlite3_stmt *stmt = nullptr;
    bool found = false;
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        found = sqlite3_step(stmt) == SQLITE_ROW;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(conn);
    return found;
}

kimix::string repeat_char(char c, size_t n) {
    kimix::string s;
    s.append(n, c);
    return s;
}

// One raw SQL scalar (text) reader for doc-store assertions.
kimix::string raw_query_text(const kimix::filesystem::path &db, const char *sql,
                             bool &ok) {
    kimix::string out;
    ok = false;
    sqlite3 *conn = nullptr;
    if (sqlite3_open_v2(db.string().c_str(), &conn, SQLITE_OPEN_READWRITE,
                        nullptr) != SQLITE_OK) {
        if (conn != nullptr) {
            sqlite3_close(conn);
        }
        return out;
    }
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, nullptr) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *txt = sqlite3_column_text(stmt, 0);
        if (txt != nullptr) {
            out = reinterpret_cast<const char *>(txt);
        }
        ok = true;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(conn);
    return out;
}


} // namespace

int main() {
    // -----------------------------------------------------------------------
    // D3: schema/DDL created + WAL sidecar
    // -----------------------------------------------------------------------
    "open_creates_schema_and_wal"_test = [] {
        const auto dir = test_dir("schema");
        const auto db = dir / "history.db";
        SqliteHistoryIndex idx(db);
        kimix::string err;
        expect(idx.open(err)) << err.c_str();
        expect(idx.is_open());
        append_one(idx, "hello schema world");
        // The doc store + both FTS tables exist (queries succeed).
        expect(raw_has_row(db, "SELECT 1 FROM turns LIMIT 1"));
        expect(raw_has_row(db,
                           "SELECT 1 FROM turns_fts WHERE turns_fts MATCH 'hello'"));
        expect(raw_has_row(db, "SELECT 1 FROM turns_fts_trigram WHERE "
                               "turns_fts_trigram MATCH '\"sch\"'"));
        // The 3 sync triggers were created.
        for (const char *trig : {"turns_fts_insert", "turns_fts_delete",
                                 "turns_fts_update"}) {
            const kimix::string sql = kimix::format(
                "SELECT 1 FROM sqlite_master WHERE type='trigger' AND name='{}'",
                trig);
            expect(raw_has_row(db, sql.c_str())) << trig;
        }
        // WAL sidecar exists while the connection is open.
        expect(kimix::filesystem::exists(db.string() + "-wal"))
            << "WAL mode must produce a -wal sidecar";
        idx.close();
        expect(!idx.is_open());
        idx.close(); // safe twice
    };

    // -----------------------------------------------------------------------
    // D3/D7: append + search returns RAW verbatim text (byte-exact, CJK)
    // -----------------------------------------------------------------------
    "append_search_returns_raw_verbatim_text"_test = [] {
        const auto dir = test_dir("raw_text");
        SqliteHistoryIndex idx(dir / "history.db");
        kimix::string err;
        expect(idx.open(err)) << err.c_str();
        const kimix::string cjk =
            "Path Src/Main.cpp APIToken \xe6\x9f\xa5\xe8\xaf\xa2\xe7\xbb\x93"
            "\xe6\x9e\x9c"; // "查询结果"
        append_one(idx, cjk, 1);
        append_one(idx, "unrelated plain text", 0);
        const auto results = idx.search("APIToken", 3);
        expect(results.size() == 1_u);
        expect(results[0].text == cjk)
            << "D7: recall must be byte-exact raw text, never normalized";
      expect(results[0].role == 1_u);
      expect(results[0].score > 0.0);
      idx.close();
  };

  // -----------------------------------------------------------------------
  // D7: the DOC STORE keeps the raw bytes (never normalized) while the FTS
  // POSTINGS are case-folded by the unicode61 tokenizer - a differently-cased
  // query still reaches the document, and the recalled text is verbatim.
  "fts_postings_normalized_doc_store_raw"_test = [] {
      const auto dir = test_dir("raw_vs_postings");
      const auto db = dir / "history.db";
      SqliteHistoryIndex idx(db);
      kimix::string err;
      expect(idx.open(err)) << err.c_str();
      const kimix::string raw =
          "MixedCase FunctionName Gh\xe4\x9d\xa9Token"; // incl. a 3-byte char
      append_one(idx, raw, 1);
      append_one(idx, "unrelated filler body", 0);
      // A query in a different case still matches the raw document.
      const auto lowered = idx.search("mixedcase functionname", 3);
      expect(lowered.size() == 1_u);
      expect(lowered[0].text == raw)
          << "recall must return the raw stored text, not the postings form";
      // The stored COLUMN itself is the raw bytes (never lower-cased).
      bool ok = false;
      const kimix::string stored =
          raw_query_text(db, "SELECT text FROM turns ORDER BY turn_id", ok);
      expect(ok);
      expect(stored == raw) << stored;
      idx.close();
  };

    // -----------------------------------------------------------------------
    // D6: turn-id authority
    // -----------------------------------------------------------------------
    "turn_id_authority_and_resync_after_reopen"_test = [] {
        const auto dir = test_dir("turn_ids");
        const auto db = dir / "history.db";
        uint32_t first_id = 0;
        {
            SqliteHistoryIndex idx(db);
            kimix::string err;
            expect(idx.open(err)) << err.c_str();
            expect(idx.next_turn_id() == 0_u);
            // Callers may NOT supply arbitrary turn ids: bogus ids are
            // ignored and the index assigns its own monotonic ids.
            turn_meta t = make_turn("first turn");
            t.turn_id = 999; // must be ignored
            const turn_meta turns[1] = {t};
            idx.append_turns(kimix::span<const turn_meta>(turns, 1));
            expect(idx.next_turn_id() == 1_u);
            const auto got = idx.get_by_id(0);
            expect(got.has_value());
            expect(got->text == "first turn");
            expect(!idx.get_by_id(999).has_value());
            first_id = idx.next_turn_id();
        } // close
        {
            // Reopen: the counter re-syncs from MAX(turn_id)+1 — appended
            // ids continue, no collisions.
            SqliteHistoryIndex idx(db);
            kimix::string err;
            expect(idx.open(err)) << err.c_str();
            expect(idx.next_turn_id() == first_id);
            append_one(idx, "second turn");
            expect(idx.next_turn_id() == first_id + 1);
            const auto got = idx.get_by_id(first_id);
            expect(got.has_value());
            expect(got->text == "second turn");
            expect(idx.get_by_id(0)->text == "first turn");
        }
    };

    // -----------------------------------------------------------------------
    // D9/D3: NO 500-turn cap — 600 turns, all searchable
    // -----------------------------------------------------------------------
    "no_five_hundred_turn_cap"_test = [] {
        const auto dir = test_dir("no_cap");
        SqliteHistoryIndex idx(dir / "history.db");
        kimix::string err;
        expect(idx.open(err)) << err.c_str();
        for (int i = 0; i < 600; ++i) {
            const kimix::string text = kimix::format("turn number {} marker", i);
            append_one(idx, text, 0, 1000.0 + i);
        }
        expect(idx.turn_count() == 600_u);
        // The OLDEST turn is still indexed and searchable (the in-memory
        // kernel would have evicted it at 500).
        const auto first = idx.search("number 0 marker", 5);
        expect(first.size() >= 1_u);
        expect(first[0].text == "turn number 0 marker");
        // And the newest.
        const auto last = idx.search("number 599 marker", 5);
        expect(last.size() >= 1_u);
        expect(last[0].text == "turn number 599 marker");
        idx.close();
    };

    // -----------------------------------------------------------------------
    // D9: append-after-search is incremental (no rebuild path)
    // -----------------------------------------------------------------------
    "append_after_search_is_incremental"_test = [] {
        const auto dir = test_dir("incremental");
        SqliteHistoryIndex idx(dir / "history.db");
        kimix::string err;
        expect(idx.open(err)) << err.c_str();
        append_one(idx, "alpha content");
        expect(idx.search("alpha", 3).size() == 1_u);
        // A prior search must not prevent appends from becoming searchable.
        append_one(idx, "bravo content");
        expect(idx.search("bravo", 3).size() == 1_u);
        expect(idx.search("alpha", 3).size() == 1_u);
        idx.close();
    };

    // -----------------------------------------------------------------------
    // D8: sanitize_fts5_query matrix
    // -----------------------------------------------------------------------
    "sanitize_fts5_query_matrix"_test = [] {
        using kimix::runtime::index::sanitize_fts5_query;
        // Balanced quoted phrases are preserved verbatim.
        expect(sanitize_fts5_query("\"exact phrase\" rest") ==
               "\"exact phrase\" rest");
      // Unmatched quote becomes whitespace.
      // Unmatched quote becomes whitespace (each kept char becomes one space,
      // exactly like the Python placeholder pass — no space collapsing).
      expect(sanitize_fts5_query("hello \" world") == "hello   world");
      // FTS5-special characters are stripped to spaces.
      expect(sanitize_fts5_query("a+b(c)d:e^f@g") == "a b c d e f g");
      // Column-filter colon cannot produce "no such column" (each special
      // becomes one space; the original space stays — Python parity).
      expect(sanitize_fts5_query("TODO: fix") == "TODO  fix");
        // Dangling boolean operators are stripped.
        expect(sanitize_fts5_query("hello AND") == "hello");
        expect(sanitize_fts5_query("OR world") == "world");
        expect(sanitize_fts5_query("NOT") == "");
        // Dotted / hyphenated terms get re-quoted; underscores do not.
        expect(sanitize_fts5_query("my-app.config.ts") == "\"my-app.config.ts\"");
        expect(sanitize_fts5_query("my_app") == "my_app");
        // Repeated '*' collapses; leading '*' drops.
        expect(sanitize_fts5_query("foo**") == "foo*");
        expect(sanitize_fts5_query("*foo") == "foo");
        // 2048-char cap (codepoints): 3000 chars -> truncated.
        const kimix::string long_q = repeat_char('a', 1500) + " " +
                                     repeat_char('b', 1500);
        expect(sanitize_fts5_query(long_q).size() <= 2048_u);
        // '%' is stripped for non-CJK queries but kept for CJK ones.
        expect(sanitize_fts5_query("100% sure").find('%') ==
               kimix::string::npos);
        const kimix::string cjk_pct =
            "100%\xe6\x9f\xa5\xe8\xaf\xa2"; // "100%查询"
        expect(sanitize_fts5_query(cjk_pct).find('%') != kimix::string::npos);
    };

    // -----------------------------------------------------------------------
    // D8: CJK routing — trigram for eligible queries, LIKE for short runs
    // -----------------------------------------------------------------------
    "cjk_routing_trigram_and_like"_test = [] {
        const auto dir = test_dir("cjk");
        SqliteHistoryIndex idx(dir / "history.db");
        kimix::string err;
        expect(idx.open(err)) << err.c_str();
        // 数据库索引优化 ("database index optimization", 8 CJK chars)
        const kimix::string cjk_text = "\xe6\x95\xb0\xe6\x8d\xae\xe5\xba\x93"
                                       "\xe7\xb4\xa2\xe5\xbc\x95\xe4\xbc\x98"
                                       "\xe5\x8c\x96";
        append_one(idx, cjk_text, 0, 5000.0);
        append_one(idx, "unrelated english text", 0, 5001.0);
        // Eligible CJK query (>=3 chars per token, no lone run) -> trigram.
        const kimix::string cjk_q = "\xe6\x95\xb0\xe6\x8d\xae\xe5\xba\x93"
                                    "\xe7\xb4\xa2\xe5\xbc\x95"; // 数据库索引
        const auto trigram = idx.search(cjk_q, 3);
        expect(trigram.size() == 1_u);
        expect(trigram[0].text == cjk_text);
        expect(trigram[0].score > 0.0) << "trigram path scores via -bm25";
        // Lone CJK run ("优" alone between spaces) -> LIKE fallback: score
        // 0.0, ordered by timestamp DESC.
        const kimix::string lone_q = "\xe4\xbc\x98"; // 优
        const auto like = idx.search(lone_q, 3);
        expect(like.size() == 1_u);
        expect(like[0].text == cjk_text);
        expect(like[0].score == 0.0) << "LIKE fallback serves score 0.0";
        idx.close();
    };

    // -----------------------------------------------------------------------
    // D8: fts_stale degradation + rebuild_fts() repair
    // -----------------------------------------------------------------------
    "fts_error_sets_stale_and_rebuild_repairs"_test = [] {
        const auto dir = test_dir("stale");
        const auto db = dir / "history.db";
        SqliteHistoryIndex idx(db);
        kimix::string err;
        expect(idx.open(err)) << err.c_str();
        append_one(idx, "precious recoverable content", 0, 6000.0);
        expect(idx.search("precious", 3).size() == 1_u);
        expect(!idx.fts_stale());
        // Simulate corruption: destroy the unicode61 FTS table out from
        // under the index.
        expect(raw_exec(db, "DROP TABLE turns_fts"));
        // Any FTS error must NEVER raise: search degrades to LIKE and the
        // stale breadcrumb persists.
        const auto degraded = idx.search("precious", 3);
        expect(degraded.size() == 1_u)
            << "LIKE fallback still answers after the FTS failure";
        expect(degraded[0].score == 0.0);
        expect(idx.fts_stale()) << "stale breadcrumb must be set";
        idx.close();
        // The breadcrumb survives reopen (meta table).
        SqliteHistoryIndex idx2(db);
        expect(idx2.open(err)) << err.c_str();
        expect(idx2.fts_stale());
        expect(idx2.search("precious", 3).size() == 1_u);
        // rebuild_fts() repairs: FTS serving resumes, marker cleared.
        idx2.rebuild_fts();
        expect(!idx2.fts_stale());
        const auto repaired = idx2.search("precious", 3);
        expect(repaired.size() == 1_u);
        expect(repaired[0].score > 0.0)
            << "after rebuild_fts() the FTS path scores again";
    };

    // -----------------------------------------------------------------------
    // Persistence across close/reopen (raw text byte-exact)
    // -----------------------------------------------------------------------
    "persistence_across_reopen"_test = [] {
        const auto dir = test_dir("persist");
        const auto db = dir / "history.db";
        const kimix::string raw =
            "Keep Me Verbatim \xe4\xb8\xad\xe6\x96\x87 ABCdef";
        {
            SqliteHistoryIndex idx(db);
            kimix::string err;
            expect(idx.open(err)) << err.c_str();
            append_one(idx, raw, 2, 7000.0);
            idx.mark_compacted();
            expect(idx.turn_count() == 1_u);
        }
        {
            SqliteHistoryIndex idx(db);
            kimix::string err;
            expect(idx.open(err)) << err.c_str();
            expect(idx.turn_count() == 1_u);
            const auto got = idx.get_by_id(0);
            expect(got.has_value());
            expect(got->text == raw) << "byte-exact after reopen";
            expect(got->role == 2_u);
            expect(got->is_compacted) << "mark_compacted persists";
        }
    };

    // -----------------------------------------------------------------------
    // Reference role/blank filter (history_index.py index_messages)
    // -----------------------------------------------------------------------
    "role_and_blank_text_filter"_test = [] {
        const auto dir = test_dir("filter");
        SqliteHistoryIndex idx(dir / "history.db");
        kimix::string err;
        expect(idx.open(err)) << err.c_str();
        append_one(idx, "indexable user text", 0);
        append_one(idx, "   \n\t  ", 0);          // blank -> skipped
        append_one(idx, "system noise", 3);        // role other -> skipped
        expect(idx.turn_count() == 1_u);
        expect(idx.search("indexable", 3).size() == 1_u);
        expect(idx.search("noise", 3).empty());
    };

    // -----------------------------------------------------------------------
    // mark_compacted flags all stored turns
    // -----------------------------------------------------------------------
    "mark_compacted_flags_all_turns"_test = [] {
        const auto dir = test_dir("compacted");
        SqliteHistoryIndex idx(dir / "history.db");
        kimix::string err;
        expect(idx.open(err)) << err.c_str();
        append_one(idx, "old one", 0, 1.0);
        append_one(idx, "old two", 1, 2.0);
        idx.mark_compacted();
        for (uint32_t id : {0u, 1u}) {
            const auto got = idx.get_by_id(id);
            expect(got.has_value());
            expect(got->is_compacted);
        }
    };

    // -----------------------------------------------------------------------
    // D9: fuzzy typo retry (symmetric-delete expansion)
    // -----------------------------------------------------------------------
    "fuzzy_typo_retry_matches"_test = [] {
        const auto dir = test_dir("fuzzy");
        SqliteHistoryIndex idx(dir / "history.db");
        kimix::string err;
        expect(idx.open(err)) << err.c_str();
        append_one(idx, "the configuration file is loaded", 0, 8000.0);
        // Exact query matches.
        expect(idx.search("configuration", 3).size() == 1_u);
        // Typo (missing 'u') still matches via the fuzzy retry.
        const auto typo = idx.search("configration", 3);
        expect(typo.size() == 1_u)
            << "symmetric-delete expansion must recover the typo";
        expect(typo[0].text == "the configuration file is loaded");
    };

    // -----------------------------------------------------------------------
    // D9: min_should_match gate (0.5 of unique query tokens must be present)
    // -----------------------------------------------------------------------
    "min_should_match_gate_filters_partial_docs"_test = [] {
        const auto dir = test_dir("min_should");
        SqliteHistoryIndex idx(dir / "history.db");
        kimix::string err;
        expect(idx.open(err)) << err.c_str();
        // Full doc matches every query token; the partial doc only "bravo".
        append_one(idx, "alpha bravo charlie delta", 0, 9000.0);
        append_one(idx, "alpha bravo something else", 0, 9001.0);
        // The gate requires ceil(3*0.5)=2 of the 3 unique tokens in the row
        // text: the OR query pulls both docs into the candidate pool, but
        // the partial doc (1 token) is filtered out.
        const auto gated = idx.search("bravo OR charlie OR delta", 5);
        expect(gated.size() == 1_u)
            << "doc matching 1/3 tokens is gated out; the 3/3 doc survives";
        expect(gated[0].text == "alpha bravo charlie delta");
        // Sanity: the full doc carries all three tokens.
        expect(gated[0].score > 0.0);
    };

    // -----------------------------------------------------------------------
    // D9: search_with_recency recency weighting
    // -----------------------------------------------------------------------
    "search_with_recency_prefers_recent"_test = [] {
        const auto dir = test_dir("recency");
        SqliteHistoryIndex idx(dir / "history.db");
        kimix::string err;
        expect(idx.open(err)) << err.c_str();
        const double now = std::chrono::duration<double>(
                               std::chrono::system_clock::now()
                                   .time_since_epoch())
                               .count();
        // Same distinctive term in both; the old one has the richer text
        // (higher bm25 under equal conditions we can't easily force, so
        // rely on recency weight to reorder).
        append_one(idx, "needle", 0, now - 30.0 * 24.0 * 3600.0); // 30 days old
        append_one(idx, "needle", 0, now);                        // now
        // weight 0: pool order preserved-ish (both score equal -> stable).
        const auto unweighted = idx.search_with_recency("needle", 2, 0.0);
        expect(unweighted.size() == 2_u);
        const auto weighted = idx.search_with_recency("needle", 2, 5.0);
        expect(weighted.size() == 2_u);
        expect(weighted[0].timestamp > weighted[1].timestamp)
            << "heavy recency weight pulls the fresh turn first";
        expect(weighted[0].score >= 0.0);
    };

    // -----------------------------------------------------------------------
    // clear() removes the DB files
    // -----------------------------------------------------------------------
    "clear_deletes_db_files"_test = [] {
        const auto dir = test_dir("clear");
        const auto db = dir / "history.db";
        SqliteHistoryIndex idx(db);
        kimix::string err;
        expect(idx.open(err)) << err.c_str();
        append_one(idx, "doomed content");
        idx.clear();
        expect(!idx.is_open());
        expect(!kimix::filesystem::exists(db));
        expect(!kimix::filesystem::exists(db.string() + "-wal"));
        expect(idx.turn_count() == 0_u);
        expect(idx.next_turn_id() == 0_u);
    };

    return 0;
}
