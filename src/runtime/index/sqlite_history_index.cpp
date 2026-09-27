/*
 * sqlite_history_index.cpp — implementation of SqliteHistoryIndex
 * (see the header for the row mapping and design notes).
 *
 * Anonymous-namespace symbols are prefixed SqliteHist* because kimix-llm
 * builds with unity (jumbo) compilation — the other runtime/index TUs in
 * the same batch already claim generic helper names.
 */

#include <runtime/index/sqlite_history_index.h>

#include <runtime/index/fts5_query.h>

#include <sqlite3.h>

#include <chrono>
#include <system_error>

namespace kimix {
namespace runtime {
namespace index {

namespace {

// ---- Schema (history_index.py _SCHEMA_STATEMENTS, verbatim) ---- //

constexpr const char *kSqliteHistSchema[] = {
    "CREATE TABLE IF NOT EXISTS turns ("
    "turn_id INTEGER PRIMARY KEY,"
    "role TEXT NOT NULL,"
    "text TEXT NOT NULL,"
    "timestamp REAL NOT NULL,"
    "is_compacted INTEGER NOT NULL DEFAULT 0)",

    "CREATE TABLE IF NOT EXISTS meta ("
    "key   TEXT PRIMARY KEY,"
    "value TEXT NOT NULL)",

    "CREATE VIRTUAL TABLE IF NOT EXISTS turns_fts USING fts5("
    "text,"
    "content='turns',"
    "content_rowid='turn_id',"
    "tokenize='unicode61')",

    "CREATE VIRTUAL TABLE IF NOT EXISTS turns_fts_trigram USING fts5("
    "text,"
    "content='turns',"
    "content_rowid='turn_id',"
    "tokenize='trigram')",

    // The 3 sync triggers — each maintains BOTH FTS tables.
    "CREATE TRIGGER IF NOT EXISTS turns_fts_insert AFTER INSERT ON turns BEGIN"
    " INSERT INTO turns_fts(rowid, text) VALUES (new.turn_id, new.text);"
    " INSERT INTO turns_fts_trigram(rowid, text) VALUES (new.turn_id, new.text);"
    "END;",

    "CREATE TRIGGER IF NOT EXISTS turns_fts_delete AFTER DELETE ON turns BEGIN"
    " INSERT INTO turns_fts(turns_fts, rowid, text) VALUES ('delete', old.turn_id, old.text);"
    " INSERT INTO turns_fts_trigram(turns_fts_trigram, rowid, text) VALUES ('delete', old.turn_id, old.text);"
    "END;",

    "CREATE TRIGGER IF NOT EXISTS turns_fts_update AFTER UPDATE OF text ON turns BEGIN"
    " INSERT INTO turns_fts(turns_fts, rowid, text) VALUES ('delete', old.turn_id, old.text);"
    " INSERT INTO turns_fts(rowid, text) VALUES (new.turn_id, new.text);"
    " INSERT INTO turns_fts_trigram(turns_fts_trigram, rowid, text) VALUES ('delete', old.turn_id, old.text);"
    " INSERT INTO turns_fts_trigram(rowid, text) VALUES (new.turn_id, new.text);"
    "END;",
};

// history_index.py _FTS_TRIGGER_NAMES: rebuild_fts drops all six names (the
// three trigram-named ones are historical no-ops).
constexpr const char *kSqliteHistTriggerNames[] = {
    "turns_fts_insert",         "turns_fts_delete",         "turns_fts_update",
    "turns_fts_trigram_insert", "turns_fts_trigram_delete", "turns_fts_trigram_update",
};

constexpr const char *kSqliteHistStaleKey = "fts_stale";

// Bounded FTS merge cadence (history_index.py:52-53).
constexpr uint32_t kSqliteHistMergeInterval = 500; // writes
constexpr int kSqliteHistMergeMaxPages = 200;      // pages per merge

constexpr const char *kSqliteHistRoleText[] = {"user", "assistant", "tool"};

// RAII statement finalizer.
struct SqliteHistStmt {
    sqlite3_stmt *st = nullptr;
    ~SqliteHistStmt() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }
    SqliteHistStmt() = default;
    SqliteHistStmt(const SqliteHistStmt &) = delete;
    SqliteHistStmt &operator=(const SqliteHistStmt &) = delete;
};

bool sqlite_hist_is_blank(kimix::string_view text) noexcept {
    for (char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\v' &&
            c != '\f') {
            return false;
        }
    }
    return true;
}

kimix::string sqlite_hist_ascii_lower(kimix::string_view text) {
    kimix::string out;
    out.reserve(text.size());
    for (char c : text) {
        out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a')
                                           : c);
    }
    return out;
}

// strip('"').strip() — all leading/trailing '"' then whitespace.
kimix::string sqlite_hist_strip_quotes(kimix::string_view q) {
    size_t b = 0, e = q.size();
    while (b < e && q[b] == '"') {
        ++b;
    }
    while (e > b && q[e - 1] == '"') {
        --e;
    }
    while (b < e && is_ascii_space(q[b])) {
        ++b;
    }
    while (e > b && is_ascii_space(q[e - 1])) {
        --e;
    }
    return kimix::string(q.substr(b, e - b));
}

// D9 (G12): the reference Searcher de-duplicates query tokens
// (list(dict.fromkeys(query_tokens))). Whitespace-split, boolean operators
// dropped, order-preserving dedup.
kimix::vector<kimix::string>
sqlite_hist_unique_tokens(kimix::string_view query) {
    kimix::vector<kimix::string> out;
    kimix::set<kimix::string> seen;
    size_t i = 0;
    while (i <= query.size()) {
        size_t j = i;
        while (j < query.size() && !is_ascii_space(query[j])) {
            ++j;
        }
        if (j > i) {
            kimix::string tok(query.substr(i, j - i));
            if (!is_boolean_operator(tok) && seen.insert(tok).second) {
                out.push_back(std::move(tok));
            }
        }
        if (j >= query.size()) {
            break;
        }
        i = j + 1;
    }
    return out;
}

// Execute one FTS5 MATCH and collect rows. Returns false when the FTS layer
// errored (prepare or step) — the caller degrades to LIKE.
bool sqlite_hist_exec_match(sqlite3 *db, const char *fts_table,
                            kimix::string_view match, uint32_t limit,
                            kimix::vector<turn_meta> &out) {
    const kimix::string sql = kimix::format(
        "SELECT t.turn_id, t.role, t.text, t.timestamp, t.is_compacted,"
        " -bm25({0}) AS score"
        " FROM {0}"
        " JOIN turns t ON t.turn_id = {0}.rowid"
        " WHERE {0} MATCH ?"
        " ORDER BY score DESC LIMIT ?", fts_table);
    sqlite3_stmt *raw = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) {
        return false;
    }
    SqliteHistStmt stmt;
    stmt.st = raw;
    if (sqlite3_bind_text(stmt.st, 1, match.data(),
                          static_cast<int>(match.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int(stmt.st, 2, static_cast<int>(limit)) != SQLITE_OK) {
        return false;
    }
    for (;;) {
        const int rc = sqlite3_step(stmt.st);
        if (rc == SQLITE_DONE) {
            return true;
        }
        if (rc != SQLITE_ROW) {
            return false; // corrupted/locked FTS table mid-scan
        }
        turn_meta t;
        t.turn_id = static_cast<uint32_t>(sqlite3_column_int64(stmt.st, 0));
        const unsigned char *role = sqlite3_column_text(stmt.st, 1);
        t.role = 3;
        if (role != nullptr) {
            const kimix::string_view r(reinterpret_cast<const char *>(role));
            t.role = r == "user" ? 0 : r == "assistant" ? 1 : r == "tool" ? 2 : 3;
        }
        const unsigned char *text = sqlite3_column_text(stmt.st, 2);
        if (text != nullptr) {
            t.text.assign(reinterpret_cast<const char *>(text),
                          static_cast<size_t>(sqlite3_column_bytes(stmt.st, 2)));
        }
        t.timestamp = sqlite3_column_double(stmt.st, 3);
        t.is_compacted = sqlite3_column_int(stmt.st, 4) != 0;
        t.score = sqlite3_column_double(stmt.st, 5);
        out.push_back(std::move(t));
    }
}

// D9: true for tokens the reference would fuzzy-expand ("Latin" tokens:
// pure ASCII with at least one letter).
bool sqlite_hist_is_latin_token(kimix::string_view tok) noexcept {
    bool has_alpha = false;
    for (char c : tok) {
        const auto b = static_cast<uint8_t>(c);
        if (b >= 0x80u) {
            return false;
        }
        if ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z')) {
            has_alpha = true;
        }
    }
    return has_alpha;
}

// D9: reference fuzziness="AUTO" (retrieval.py:871) — 0-2 chars -> 0,
// 3-5 -> 1, >5 -> 2 edits.
uint32_t sqlite_hist_auto_edits(size_t len) noexcept {
    return len < 3 ? 0 : (len < 6 ? 1 : 2);
}

double sqlite_hist_now_seconds() {
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

SqliteHistoryIndex::SqliteHistoryIndex(kimix::filesystem::path db_path)
    : _db_path(std::move(db_path)) {}

SqliteHistoryIndex::~SqliteHistoryIndex() {
    close();
}

SqliteHistoryIndex::SqliteHistoryIndex(SqliteHistoryIndex &&other) noexcept
    : _db_path(std::move(other._db_path)),
      _db(other._db),
      _doc_id_counter(other._doc_id_counter),
      _fts_stale(other._fts_stale),
      _writes_since_merge(other._writes_since_merge) {
    other._db = nullptr;
    other._doc_id_counter = 0;
    other._fts_stale = false;
    other._writes_since_merge = 0;
}

SqliteHistoryIndex &
SqliteHistoryIndex::operator=(SqliteHistoryIndex &&other) noexcept {
    if (this != &other) {
        close();
        _db_path = std::move(other._db_path);
        _db = other._db;
        _doc_id_counter = other._doc_id_counter;
        _fts_stale = other._fts_stale;
        _writes_since_merge = other._writes_since_merge;
        other._db = nullptr;
        other._doc_id_counter = 0;
        other._fts_stale = false;
        other._writes_since_merge = 0;
    }
    return *this;
}

bool SqliteHistoryIndex::exec(const char *sql) const {
    if (_db == nullptr) {
        return false;
    }
    char *errmsg = nullptr;
    const int rc = sqlite3_exec(_db, sql, nullptr, nullptr, &errmsg);
    if (errmsg != nullptr) {
        sqlite3_free(errmsg);
    }
    return rc == SQLITE_OK;
}

bool SqliteHistoryIndex::init_schema() {
    for (const char *statement : kSqliteHistSchema) {
        if (!exec(statement)) {
            return false;
        }
    }
    return true;
}

void SqliteHistoryIndex::sync_doc_id_counter() {
    // history_index.py _sync_doc_id_counter: COALESCE(MAX(turn_id), -1) + 1
    // so prune_<n> references stay valid across restarts.
    _doc_id_counter = 0;
    if (_db == nullptr) {
        return;
    }
    sqlite3_stmt *raw = nullptr;
    if (sqlite3_prepare_v2(_db,
                           "SELECT COALESCE(MAX(turn_id), -1) + 1 FROM turns",
                           -1, &raw, nullptr) != SQLITE_OK) {
        return;
    }
    SqliteHistStmt stmt;
    stmt.st = raw;
    if (sqlite3_step(stmt.st) == SQLITE_ROW) {
        _doc_id_counter =
            static_cast<uint32_t>(sqlite3_column_int64(stmt.st, 0));
    }
}

void SqliteHistoryIndex::load_fts_stale() {
    // history_index.py _load_fts_stale: breadcrumb row in meta.
    _fts_stale = false;
    if (_db == nullptr) {
        return;
    }
    sqlite3_stmt *raw = nullptr;
    if (sqlite3_prepare_v2(_db, "SELECT 1 FROM meta WHERE key = ?", -1, &raw,
                           nullptr) != SQLITE_OK) {
        return;
    }
    SqliteHistStmt stmt;
    stmt.st = raw;
    if (sqlite3_bind_text(stmt.st, 1, kSqliteHistStaleKey, -1,
                          SQLITE_STATIC) != SQLITE_OK) {
        return;
    }
    _fts_stale = sqlite3_step(stmt.st) == SQLITE_ROW;
}

void SqliteHistoryIndex::set_fts_stale() noexcept {
    // Never raise: persist best-effort (history_index.py _set_fts_stale).
    _fts_stale = true;
    if (_db != nullptr) {
        sqlite3_stmt *raw = nullptr;
        if (sqlite3_prepare_v2(_db,
                               "INSERT INTO meta (key, value) VALUES (?, ?) "
                               "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
                               -1, &raw, nullptr) == SQLITE_OK) {
            SqliteHistStmt stmt;
            stmt.st = raw;
            sqlite3_bind_text(stmt.st, 1, kSqliteHistStaleKey, -1,
                              SQLITE_STATIC);
            sqlite3_bind_text(stmt.st, 2, "1", -1, SQLITE_STATIC);
            sqlite3_step(stmt.st);
        }
    }
}

void SqliteHistoryIndex::clear_fts_stale() noexcept {
    _fts_stale = false;
    if (_db != nullptr) {
        sqlite3_stmt *raw = nullptr;
        if (sqlite3_prepare_v2(_db, "DELETE FROM meta WHERE key = ?", -1,
                               &raw, nullptr) == SQLITE_OK) {
            SqliteHistStmt stmt;
            stmt.st = raw;
            sqlite3_bind_text(stmt.st, 1, kSqliteHistStaleKey, -1,
                              SQLITE_STATIC);
            sqlite3_step(stmt.st);
        }
    }
}

bool SqliteHistoryIndex::open(kimix::string &error) {
    if (_db != nullptr) {
        return true;
    }
    std::error_code ec;
    const auto parent = _db_path.parent_path();
    if (!parent.empty()) {
        kimix::filesystem::create_directories(parent, ec);
        if (ec) {
            error = kimix::format("cannot create history.db directory: {}",
                                  ec.message());
            return false;
        }
    }
    const kimix::string path_str = kimix::to_string(_db_path);
    const int rc =
        sqlite3_open_v2(path_str.c_str(), &_db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                            SQLITE_OPEN_FULLMUTEX,
                        nullptr);
    if (rc != SQLITE_OK) {
        error = _db != nullptr ? sqlite3_errmsg(_db) : "sqlite open failed";
        close();
        return false;
    }
    // history_index.py _ensure_conn pragmas + busy_timeout (so distinct
    // instances on the same session DB do not wedge on WAL locks).
    static const char *kPragmas[] = {
        "PRAGMA journal_mode=WAL",
        "PRAGMA synchronous=NORMAL",
        "PRAGMA busy_timeout=5000",
    };
    for (const char *pragma : kPragmas) {
        if (!exec(pragma)) {
            error = kimix::format("pragma failed: {}", pragma);
            close();
            return false;
        }
    }
    if (!init_schema()) {
        error = "history.db schema creation failed: ";
        error += sqlite3_errmsg(_db);
        close();
        return false;
    }
    sync_doc_id_counter();
    load_fts_stale();
    return true;
}

void SqliteHistoryIndex::close() noexcept {
    if (_db == nullptr) {
        return;
    }
    sqlite3_exec(_db, "PRAGMA wal_checkpoint(PASSIVE)", nullptr, nullptr,
                 nullptr);
    sqlite3_close(_db);
    _db = nullptr;
}

void SqliteHistoryIndex::append_turns(kimix::span<const turn_meta> turns) {
    if (_db == nullptr || turns.empty()) {
        return;
    }
    // history_index.py index_messages: only user/assistant/tool with non-blank
    // text become turns. The index assigns turn ids from its OWN counter
    // (D6): the caller's turn_meta::turn_id is ignored.
    struct row {
        const turn_meta *meta;
        const char *role;
    };
    kimix::vector<row> rows;
    for (const turn_meta &t : turns) {
        if (t.role > 2 || sqlite_hist_is_blank(t.text)) {
            continue;
        }
        rows.push_back(row{&t, kSqliteHistRoleText[t.role]});
    }
    if (rows.empty()) {
        return;
    }
    // One transaction: each INSERT fires the FTS triggers; batching is what
    // keeps a backfill fast (reference comment at index_messages).
    uint32_t next_id = _doc_id_counter;
    bool ok = exec("BEGIN");
    sqlite3_stmt *raw = nullptr;
    if (ok && sqlite3_prepare_v2(_db,
                                 "INSERT INTO turns (turn_id, role, text, "
                                 "timestamp) VALUES (?, ?, ?, ?)",
                                 -1, &raw, nullptr) != SQLITE_OK) {
        ok = false;
    }
    SqliteHistStmt stmt;
    stmt.st = raw;
    if (ok) {
        for (const row &r : rows) {
            const turn_meta &t = *r.meta;
            sqlite3_reset(stmt.st);
            sqlite3_clear_bindings(stmt.st);
            if (sqlite3_bind_int64(stmt.st, 1, next_id) != SQLITE_OK ||
                sqlite3_bind_text(stmt.st, 2, r.role, -1, SQLITE_STATIC) !=
                    SQLITE_OK ||
                sqlite3_bind_text(stmt.st, 3, t.text.c_str(),
                                  static_cast<int>(t.text.size()),
                                  SQLITE_STATIC) != SQLITE_OK ||
                sqlite3_bind_double(stmt.st, 4, t.timestamp) != SQLITE_OK ||
                sqlite3_step(stmt.st) != SQLITE_DONE) {
                ok = false;
                break;
            }
            ++next_id;
        }
    }
    if (!exec(ok ? "COMMIT" : "ROLLBACK")) {
        ok = false;
    }
    if (!ok) {
        return; // never raise: the session keeps running without the row
    }
    _doc_id_counter = next_id;
    for (const row &r : rows) {
        feed_fuzzy(_fuzzy, r.meta->text);
    }
    maybe_merge_fts();
}

void SqliteHistoryIndex::maybe_merge_fts() noexcept {
    // history_index.py _maybe_merge_fts: bounded merge every 500 writes.
    // Errors are swallowed (maintenance only).
    ++_writes_since_merge;
    if (_writes_since_merge < kSqliteHistMergeInterval || _db == nullptr) {
        return;
    }
    _writes_since_merge = 0;
    sqlite3_stmt *raw = nullptr;
    for (const char *table : {"turns_fts", "turns_fts_trigram"}) {
        const kimix::string sql = kimix::format(
            "INSERT INTO {0}({0}, rank) VALUES('merge', ?)", table);
        if (sqlite3_prepare_v2(_db, sql.c_str(), -1, &raw, nullptr) ==
            SQLITE_OK) {
            SqliteHistStmt stmt;
            stmt.st = raw;
            raw = nullptr;
            sqlite3_bind_int(stmt.st, 1, kSqliteHistMergeMaxPages);
            sqlite3_step(stmt.st);
        }
    }
}

void SqliteHistoryIndex::insert_turn_with_id(const turn_meta &turn) {
    if (_db == nullptr || turn.role > 2 || sqlite_hist_is_blank(turn.text)) {
        return;
    }
    // INSERT OR IGNORE: a collision with an existing turn_id (a host that did
    // not route its ids through this index) silently drops the archive row
    // rather than corrupting the real turn - never raise to the pruner.
    bool ok = exec("BEGIN");
    sqlite3_stmt *raw = nullptr;
    if (ok && sqlite3_prepare_v2(_db,
                                 "INSERT OR IGNORE INTO turns (turn_id, role, text, "
                                 "timestamp) VALUES (?, ?, ?, ?)",
                                 -1, &raw, nullptr) != SQLITE_OK) {
        ok = false;
    }
    SqliteHistStmt stmt;
    stmt.st = raw;
    if (ok) {
        if (sqlite3_bind_int64(stmt.st, 1, static_cast<int64_t>(turn.turn_id)) !=
                SQLITE_OK ||
            sqlite3_bind_text(stmt.st, 2, kSqliteHistRoleText[turn.role], -1,
                              SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_text(stmt.st, 3, turn.text.c_str(),
                              static_cast<int>(turn.text.size()),
                              SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_double(stmt.st, 4, turn.timestamp) != SQLITE_OK ||
            sqlite3_step(stmt.st) != SQLITE_DONE) {
            ok = false;
        }
    }
    if (!exec(ok ? "COMMIT" : "ROLLBACK")) {
        ok = false;
    }
    if (!ok) {
        return; // never raise: the elided original is simply not recallable
    }
    feed_fuzzy(_fuzzy, turn.text);
    maybe_merge_fts();
}

void SqliteHistoryIndex::mark_compacted() {
    if (_db == nullptr) {
        return;
    }
    if (exec("BEGIN")) {
        if (exec("UPDATE turns SET is_compacted = 1")) {
            exec("COMMIT");
            return;
        }
        exec("ROLLBACK");
    }
}

kimix::vector<turn_meta>
SqliteHistoryIndex::search(kimix::string_view query, uint32_t top_k) {
    kimix::vector<turn_meta> out;
    if (_db == nullptr || top_k == 0) {
        return out;
    }
    const kimix::string q = sanitize_fts5_query(query);
    if (q.empty()) {
        return out;
    }
    // Stale-index breadcrumb (D8): a previously-corrupted FTS table serves
    // LIKE results until rebuild_fts() clears the marker.
    if (_fts_stale) {
        return search_like(q, top_k);
    }
    if (contains_cjk(q)) {
        const kimix::string raw = sqlite_hist_strip_quotes(q);
        if (trigram_eligible_tokens(q) && !has_lone_cjk_run(raw)) {
            kimix::vector<turn_meta> trigram = search_fts_trigram(raw, top_k);
            if (!_fts_stale) {
                return trigram;
            }
            return search_like(raw, top_k);
        }
        return search_like(raw, top_k);
    }
    const kimix::vector<kimix::string> unique = sqlite_hist_unique_tokens(q);
    if (!run_fts_match("turns_fts", q, unique, top_k, out)) {
        // Never raise to the LLM tool — degrade to LIKE on any FTS error.
        return search_like(q, top_k);
    }
    return out;
}

bool SqliteHistoryIndex::run_fts_match(const char *fts_table,
                                       kimix::string_view match,
                                       kimix::span<const kimix::string>
                                           unique_tokens,
                                       uint32_t top_k,
                                       kimix::vector<turn_meta> &out) {
    // D9 min_should_match=0.5: with >=2 unique tokens, fetch a wider pool
    // (top_k*3, the same widening search_with_recency uses) so the gate has
    // headroom, then truncate.
    const bool gate = unique_tokens.size() >= 2;
    const uint32_t limit = gate ? top_k * 3 : top_k;
    if (!sqlite_hist_exec_match(_db, fts_table, match, limit, out)) {
        set_fts_stale();
        return false;
    }
    if (out.empty() && _fuzzy.term_count() > 0) {
        // D9 fuzzy retry (reference Searcher headline behavior: a typo'd
        // query still matches via symmetric-delete expansion).
        kimix::string fmatch;
        if (fuzzy_match_query(unique_tokens, fmatch)) {
            kimix::vector<turn_meta> retry;
            if (sqlite_hist_exec_match(_db, fts_table, fmatch, limit, retry)) {
                out = std::move(retry);
            } else {
                set_fts_stale();
                return false;
            }
        }
    }
    if (!gate) {
        return true;
    }
    // Token presence: quote-stripped, ASCII-lowercased substring of the raw
    // text (the corpus is stored verbatim — D7 — so the gate lowercases both
    // sides instead of consulting a normalized copy).
    kimix::vector<kimix::string> toks;
    toks.reserve(unique_tokens.size());
    for (const kimix::string &tok : unique_tokens) {
        kimix::string cleaned;
        for (char c : tok) {
            if (c != '"') {
                cleaned.push_back(c);
            }
        }
        if (!cleaned.empty()) {
            toks.push_back(sqlite_hist_ascii_lower(cleaned));
        }
    }
    if (toks.size() < 2) {
        if (out.size() > top_k) {
            out.resize(top_k);
        }
        return true;
    }
    const size_t required = (toks.size() + 1) / 2; // ceil(n*0.5)
    kimix::vector<turn_meta> gated;
    gated.reserve(out.size());
    for (turn_meta &t : out) {
        const kimix::string hay = sqlite_hist_ascii_lower(t.text);
        size_t matched = 0;
        for (const kimix::string &tok : toks) {
            if (hay.find(tok) != kimix::string::npos) {
                ++matched;
            }
        }
        if (matched >= required) {
            gated.push_back(std::move(t));
        }
    }
    if (gated.size() > top_k) {
        gated.resize(top_k);
    }
    out = std::move(gated);
    return true;
}

bool SqliteHistoryIndex::fuzzy_match_query(
    kimix::span<const kimix::string> unique_tokens,
    kimix::string &match_out) const {
    kimix::vector<kimix::string> groups;
    bool any_expansion = false;
    for (const kimix::string &raw : unique_tokens) {
        if (!sqlite_hist_is_latin_token(raw)) {
            continue;
        }
        kimix::string tok;
        for (char c : raw) {
            if (c != '"' && c != '*') {
                tok.push_back(c >= 'A' && c <= 'Z'
                                 ? static_cast<char>(c - 'A' + 'a')
                                 : c);
            }
        }
        if (tok.empty()) {
            continue;
        }
        const uint32_t edits = sqlite_hist_auto_edits(tok.size());
        if (edits == 0) {
            continue; // exact-only token: no group needed
        }
        kimix::vector<search::fuzzy_candidate> cands;
        _fuzzy.expand(tok, edits, cands);// max_expansions=50 default
        if (cands.empty()) {
            continue;
        }
        any_expansion = true;
        kimix::string group("(\"");
        group += tok;
        group += "\"";
        size_t used = 0;
        for (const search::fuzzy_candidate &c : cands) {
            if (used >= 8 || c.term == tok) { // keep the MATCH string bounded
                continue;
            }
            group += " OR \"";
            for (char ch : c.term) {
                if (ch == '"') {
                    group.push_back('"');
                }
                group.push_back(ch);
            }
            group.push_back('"');
            ++used;
        }
        group.push_back(')');
        groups.push_back(std::move(group));
    }
    if (!any_expansion) {
        return false;
    }
    kimix::string match;
    for (size_t i = 0; i < groups.size(); ++i) {
        if (i != 0) {
            match += " AND ";
        }
        match += groups[i];
    }
    match_out = std::move(match);
    return true;
}

void SqliteHistoryIndex::feed_fuzzy(search::SymmetricDeleteIndex &fuzzy,
                                    kimix::string_view text) {
    // Feed ASCII word runs (len >= 2, lowercased) — the reference expands
    // only Latin tokens, so only Latin vocabulary is worth indexing.
    size_t i = 0;
    while (i < text.size()) {
        const auto b = static_cast<uint8_t>(text[i]);
        const bool word = (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
                          (b >= '0' && b <= '9');
        if (!word) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < text.size()) {
            const auto c = static_cast<uint8_t>(text[j]);
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9'))) {
                break;
            }
            ++j;
        }
        if (j - i >= 2) {
            fuzzy.add_term(sqlite_hist_ascii_lower(text.substr(i, j - i)));
        }
        i = j;
    }
}

kimix::vector<turn_meta>
SqliteHistoryIndex::search_fts_unicode61(kimix::string_view query,
                                         uint32_t top_k) {
    kimix::vector<turn_meta> out;
    const kimix::vector<kimix::string> unique = sqlite_hist_unique_tokens(query);
    run_fts_match("turns_fts", query, unique, top_k, out);
    return out;
}

kimix::vector<turn_meta>
SqliteHistoryIndex::search_fts_trigram(kimix::string_view raw_query,
                                       uint32_t top_k) {
    kimix::vector<turn_meta> out;
    const kimix::string trigram_query = quote_fts_tokens(raw_query);
    // No min_should_match/fuzzy on the CJK paths (reference parity — the
    // LIKE fallback and trigram MATCH are served as-is).
    if (!sqlite_hist_exec_match(_db, "turns_fts_trigram", trigram_query, top_k,
                                out)) {
        set_fts_stale();
    }
    return out;
}

kimix::vector<turn_meta>
SqliteHistoryIndex::search_like(kimix::string_view raw_query, uint32_t top_k) {
    // history_index.py _search_like: whitespace tokens minus AND/OR/NOT, %
    // wrapped LIKE clauses (escaped), score 0.0, recency ordering.
    kimix::vector<kimix::string> tokens;
    {
        size_t i = 0;
        while (i <= raw_query.size()) {
            size_t j = i;
            while (j < raw_query.size() && !is_ascii_space(raw_query[j])) {
                ++j;
            }
            if (j > i) {
                kimix::string tok(raw_query.substr(i, j - i));
                if (!is_boolean_operator(tok)) {
                    tokens.push_back(std::move(tok));
                }
            }
            if (j >= raw_query.size()) {
                break;
            }
            i = j + 1;
        }
        if (tokens.empty()) {
            tokens.emplace_back(raw_query);
        }
    }
    kimix::string sql =
        "SELECT turn_id, role, text, timestamp, is_compacted, 0.0 AS score"
        " FROM turns WHERE ";
    for (size_t k = 0; k < tokens.size(); ++k) {
        if (k != 0) {
            sql += " OR ";
        }
        sql += "text LIKE ? ESCAPE '\\'";
    }
    sql += " ORDER BY timestamp DESC, turn_id DESC LIMIT ?";
    sqlite3_stmt *raw = nullptr;
    if (sqlite3_prepare_v2(_db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) {
        return {};
    }
    SqliteHistStmt stmt;
    stmt.st = raw;
    int idx = 1;
    for (const kimix::string &tok : tokens) {
        const kimix::string esc = escape_like(tok);
        const kimix::string wrapped("%");
        kimix::string param(wrapped);
        param += esc;
        param.push_back('%');
        if (sqlite3_bind_text(stmt.st, idx, param.c_str(),
                              static_cast<int>(param.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK) {
            return {};
        }
        ++idx;
    }
    sqlite3_bind_int(stmt.st, idx, static_cast<int>(top_k));
    kimix::vector<turn_meta> out;
    for (;;) {
        const int rc = sqlite3_step(stmt.st);
        if (rc == SQLITE_DONE) {
            return out;
        }
        if (rc != SQLITE_ROW) {
            return {};
        }
        turn_meta t;
        t.turn_id = static_cast<uint32_t>(sqlite3_column_int64(stmt.st, 0));
        const unsigned char *role = sqlite3_column_text(stmt.st, 1);
        t.role = 3;
        if (role != nullptr) {
            const kimix::string_view r(reinterpret_cast<const char *>(role));
            t.role = r == "user" ? 0 : r == "assistant" ? 1 : r == "tool" ? 2 : 3;
        }
        const unsigned char *text = sqlite3_column_text(stmt.st, 2);
        if (text != nullptr) {
            t.text.assign(reinterpret_cast<const char *>(text),
                          static_cast<size_t>(sqlite3_column_bytes(stmt.st, 2)));
        }
        t.timestamp = sqlite3_column_double(stmt.st, 3);
        t.is_compacted = sqlite3_column_int(stmt.st, 4) != 0;
        t.score = 0.0;
        out.push_back(std::move(t));
    }
}

kimix::vector<turn_meta>
SqliteHistoryIndex::search_with_recency(kimix::string_view query,
                                        uint32_t top_k,
                                        double recency_weight) {
    // history_index.py search_with_recency: top_k*3 candidate pool, boost =
    // 1 + w*exp(-hours_ago/24), stable descending sort, truncate.
    kimix::vector<turn_meta> pool = search(query, top_k * 3);
    if (pool.empty()) {
        return pool;
    }
    const double now = sqlite_hist_now_seconds();
    for (turn_meta &t : pool) {
        const double hours_ago = (now - t.timestamp) / 3600.0;
        const double boost =
            1.0 + recency_weight * std::exp(-hours_ago / 24.0);
        // The raw bm25 score stays in `score`; the boosted value lands in the
        // transient `boosted_score` (D11 gates tiers on the two separately).
        t.boosted_score = t.score * boost;
    }
    std::stable_sort(pool.begin(), pool.end(),
                     [](const turn_meta &a, const turn_meta &b) {
                         return a.boosted_score > b.boosted_score;
                     });
    if (pool.size() > top_k) {
        pool.resize(top_k);
    }
    return pool;
}

kimix::optional<turn_meta>
SqliteHistoryIndex::get_by_id(uint32_t turn_id) const {
    if (_db == nullptr) {
        return kimix::optional<turn_meta>();
    }
    sqlite3_stmt *raw = nullptr;
    if (sqlite3_prepare_v2(_db,
                           "SELECT turn_id, role, text, timestamp, "
                           "is_compacted FROM turns WHERE turn_id = ?",
                           -1, &raw, nullptr) != SQLITE_OK) {
        return kimix::optional<turn_meta>();
    }
    SqliteHistStmt stmt;
    stmt.st = raw;
    if (sqlite3_bind_int64(stmt.st, 1, turn_id) != SQLITE_OK ||
        sqlite3_step(stmt.st) != SQLITE_ROW) {
        return kimix::optional<turn_meta>();
    }
    turn_meta t;
    t.turn_id = static_cast<uint32_t>(sqlite3_column_int64(stmt.st, 0));
    const unsigned char *role = sqlite3_column_text(stmt.st, 1);
    t.role = 3;
    if (role != nullptr) {
        const kimix::string_view r(reinterpret_cast<const char *>(role));
        t.role = r == "user" ? 0 : r == "assistant" ? 1 : r == "tool" ? 2 : 3;
    }
    const unsigned char *text = sqlite3_column_text(stmt.st, 2);
    if (text != nullptr) {
        t.text.assign(reinterpret_cast<const char *>(text),
                      static_cast<size_t>(sqlite3_column_bytes(stmt.st, 2)));
    }
    t.timestamp = sqlite3_column_double(stmt.st, 3);
    t.is_compacted = sqlite3_column_int(stmt.st, 4) != 0;
    return kimix::optional<turn_meta>(std::move(t));
}

uint64_t SqliteHistoryIndex::turn_count() const {
    if (_db == nullptr) {
        return 0;
    }
    sqlite3_stmt *raw = nullptr;
    if (sqlite3_prepare_v2(_db, "SELECT COUNT(*) FROM turns", -1, &raw,
                           nullptr) != SQLITE_OK) {
        return 0;
    }
    SqliteHistStmt stmt;
    stmt.st = raw;
    if (sqlite3_step(stmt.st) == SQLITE_ROW) {
        return static_cast<uint64_t>(sqlite3_column_int64(stmt.st, 0));
    }
    return 0;
}

kimix::vector<uint32_t> SqliteHistoryIndex::non_compacted_turn_ids() const {
    kimix::vector<uint32_t> out;
    if (_db == nullptr) {
        return out;
    }
    sqlite3_stmt *raw = nullptr;
    if (sqlite3_prepare_v2(_db,
                           "SELECT turn_id FROM turns WHERE is_compacted = 0 "
                           "ORDER BY turn_id",
                           -1, &raw, nullptr) != SQLITE_OK) {
        return out;
    }
    SqliteHistStmt stmt;
    stmt.st = raw;
    while (sqlite3_step(stmt.st) == SQLITE_ROW) {
        out.push_back(static_cast<uint32_t>(sqlite3_column_int64(stmt.st, 0)));
    }
    return out;
}

void SqliteHistoryIndex::save() noexcept {
    // Durable backend: commit any pending transaction (reference save()).
    if (_db != nullptr) {
        exec("COMMIT");
    }
}

void SqliteHistoryIndex::rebuild_fts() {
    // history_index.py rebuild_fts: close first (so a caller-held cursor
    // cannot block the DDL), then drop + recreate + backfill + clear marker.
    if (_db == nullptr) {
        return;
    }
    close();
    kimix::string error;
    if (!open(error)) {
        return;
    }
    if (exec("BEGIN")) {
        for (const char *name : kSqliteHistTriggerNames) {
            const kimix::string sql =
                kimix::format("DROP TRIGGER IF EXISTS {}", name);
            exec(sql.c_str());
        }
        exec("DROP TABLE IF EXISTS turns_fts");
        exec("DROP TABLE IF EXISTS turns_fts_trigram");
        exec("COMMIT");
    } else {
        exec("ROLLBACK");
    }
    // Recreate the FTS tables + triggers (schema statements after the base
    // turns/meta tables).
    for (size_t i = 2; i < sizeof(kSqliteHistSchema) / sizeof(char *); ++i) {
        exec(kSqliteHistSchema[i]);
    }
    if (exec("BEGIN")) {
        const bool ok =
            exec("INSERT INTO turns_fts(rowid, text) SELECT turn_id, text "
                 "FROM turns") &&
            exec("INSERT INTO turns_fts_trigram(rowid, text) SELECT turn_id, "
                 "text FROM turns");
        exec(ok ? "COMMIT" : "ROLLBACK");
    }
    clear_fts_stale();
    _writes_since_merge = 0;
}

void SqliteHistoryIndex::clear() noexcept {
    // history_index.py clear(): close + delete <db>* files, reset state.
    close();
    std::error_code ec;
    kimix::filesystem::remove(_db_path, ec);
    kimix::filesystem::remove(
        kimix::filesystem::path(kimix::to_string(_db_path) + "-wal"), ec);
    kimix::filesystem::remove(
        kimix::filesystem::path(kimix::to_string(_db_path) + "-shm"), ec);
    _doc_id_counter = 0;
    _fts_stale = false;
    _writes_since_merge = 0;
    _fuzzy.reset();
}

} // namespace index
} // namespace runtime
} // namespace kimix
