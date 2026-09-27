/*
 * sqlite_history_index.h — durable FTS5-backed history index
 * (kimix::runtime::index). Rows D3/D4(durable)/D6/D7/D8/D9 of report.md §D.
 *
 * Native port of kimi-cli/src/kimi_cli/soul/history_index.py's SQLite/apsw
 * backend (read in full):
 *   - history.db schema: turns + meta tables, turns_fts (unicode61) and
 *     turns_fts_trigram FTS5 external-content tables, the 3 sync triggers
 *     (insert/delete/update, each maintaining BOTH FTS tables), WAL +
 *     synchronous=NORMAL, bounded FTS merge after 500 writes (200 pages).
 *   - NO 500-turn cap: unbounded, durable across reopen; appends are
 *     incremental (triggers keep the FTS tables in sync — never a rebuild).
 *   - D6 turn-id authority: the index owns the monotonic doc_id/turn-id
 *     counter, re-synced from COALESCE(MAX(turn_id),-1)+1 on open.
 *     append_turns IGNORES any caller-supplied turn_id and assigns ids
 *     itself (history_index.py index_messages parity: prune_N refs stay
 *     valid across restarts; two hosts can never collide).
 *   - D7: turns.text stores the RAW verbatim message text (byte-exact on
 *     recall, CJK included); normalisation (case folding) happens only
 *     inside the FTS5 tokenizers, never on the stored value.
 *   - D8: query routing ported via fts5_query.h (sanitize_fts5_query with
 *     the 2048 cap, CJK -> trigram -> LIKE substring fallback), FTS errors
 *     NEVER propagate: they set the persisted fts_stale breadcrumb
 *     (meta table) and degrade to the LIKE scan; rebuild_fts() repairs.
 *   - D9: the legacy Searcher pipeline improvements — query-token dedup,
 *     the symmetric-delete fuzzy retry (fuzziness AUTO: 0-2 chars -> 0,
 *     3-5 -> 1, >5 -> 2 edits; max_expansions 50, prefix_length 1), the
 *     min_should_match=0.5 candidate gate, and search_with_recency()
 *     (score*(1+w*exp(-hours/24)) over a top_k*3 pool, stable sort).
 *
 * Exception-free by construction (the sqlite3 C API never throws); failures
 * report through bool + `error` out-parameters on open, everything else
 * degrades silently (never raise to the LLM tool). RAII: the destructor
 * closes the connection (passive WAL checkpoint first).
 */

#pragma once

#include <core/kimix_core.h>

#include <runtime/index/history_index.h> // turn_meta
#include <runtime/search/fuzzy.h>

struct sqlite3;// sqlite3.h is C; forward-declared so this header stays light
struct sqlite3_stmt;

namespace kimix {
namespace runtime {
namespace index {

class KIMIX_RUNTIME_API SqliteHistoryIndex {
public:
    explicit SqliteHistoryIndex(kimix::filesystem::path db_path);
    ~SqliteHistoryIndex();
    SqliteHistoryIndex(SqliteHistoryIndex &&other) noexcept;
    SqliteHistoryIndex &operator=(SqliteHistoryIndex &&other) noexcept;
    SqliteHistoryIndex(const SqliteHistoryIndex &) = delete;
    SqliteHistoryIndex &operator=(const SqliteHistoryIndex &) = delete;

    const kimix::filesystem::path &db_path() const noexcept { return _db_path; }
    bool is_open() const noexcept { return _db != nullptr; }

    // Opens (creating if needed) the DB file, applies the WAL pragma set,
    // creates the schema when missing, then re-syncs the turn-id counter
    // (D6) and loads the fts_stale breadcrumb (D8).
    bool open(kimix::string &error);
    // Passive WAL checkpoint (best effort) + connection close; safe twice.
    void close() noexcept;

    // ---- D6: turn-id authority ---- //

    // Next turn id the index will assign (0-based, matching the reference
    // _doc_id_counter so prune_<n> references stay valid).
    uint32_t next_turn_id() const noexcept { return _doc_id_counter; }

    // D5: reserve a turn id WITHOUT inserting a row. The context pruner uses
    // this as the prune_N ref authority: the stub names the reserved id and
    // insert_turn_with_id() later stores the archived original under it, so
    // get_by_id(prune_N) resolves end-to-end even across restarts.
    uint32_t reserve_turn_id() noexcept { return _doc_id_counter++; }

    // Insert ONE turn under an EXPLICIT turn_id (must come from
    // reserve_turn_id()). Unlike append_turns() the caller's id is honored so
    // the row lines up with the prune_N reference. Silently skipped when the
    // id is taken (INSERT OR IGNORE) or the row is filtered (role "other" /
    // blank text) - never raises.
    void insert_turn_with_id(const turn_meta &turn);

    // ---- Indexing ---- //

    // Append turns in ONE transaction (each row fires the FTS triggers).
    // The index assigns turn ids from its own counter — the caller-supplied
    // turn_meta::turn_id is IGNORED. Rows with role "other" (3) or
    // blank/whitespace-only text are skipped (history_index.py index_messages
    // filter). The stored text is the raw verbatim turn_meta::text (D7).
    void append_turns(kimix::span<const turn_meta> turns);

    // Mark every stored turn compacted/archived (UPDATE turns SET
    // is_compacted = 1) — kimisoul.py calls this after compaction.
    void mark_compacted();

    // ---- Search ---- //

    // Top-k matching turns, `score` filled (-bm25(...) for FTS paths, 0.0
    // for the LIKE fallback), ordered like the reference. Never raises: any
    // FTS error degrades to the LIKE scan (and persists the fts_stale
    // breadcrumb so later searches skip FTS until rebuild_fts()).
    kimix::vector<turn_meta> search(kimix::string_view query, uint32_t top_k);

    // D9/reference search_with_recency: candidate pool of top_k*3 from
    // search(), boosted_score = score*(1+w*exp(-hours_ago/24)), stable
    // descending sort by boosted_score, truncated to top_k. The raw bm25
    // value stays in turn_meta::score and the boosted value in the transient
    // turn_meta::boosted_score (LIKE-fallback scores are 0.0 and keep pool
    // order on ties).
    kimix::vector<turn_meta> search_with_recency(kimix::string_view query,
                                                 uint32_t top_k,
                                                 double recency_weight = 1.0);

    // One turn by id, raw text verbatim, score 0.0. Nullopt when absent.
    // The "prune_N" prefix strip happens in the caller (Retrieve view).
    kimix::optional<turn_meta> get_by_id(uint32_t turn_id) const;

    uint64_t turn_count() const;

    // kimisoul.py auto-retrieval (the _turns scan): ids of every stored turn
    // that is NOT marked compacted, ascending.
    kimix::vector<uint32_t> non_compacted_turn_ids() const;

    // ---- Persistence / maintenance ---- //

    // Reference save(): no-op for the durable backend other than committing
    // any pending transaction (the index IS the durable store).
    void save() noexcept;
    // Repair path (history_index.py rebuild_fts): close+reopen, drop the 6
    // trigger names + both FTS tables, recreate them, backfill from turns,
    // clear the fts_stale breadcrumb.
    void rebuild_fts();
    // True after an FTS error degraded search to LIKE; cleared by
    // rebuild_fts() (or open() with no breadcrumb in meta).
    bool fts_stale() const noexcept { return _fts_stale; }

    // Reference clear(): close + delete history.db (+ -wal/-shm sidecars)
    // and reset all in-memory state.
    void clear() noexcept;

private:
    bool exec(const char *sql) const;
    bool init_schema();
    void sync_doc_id_counter();
    void load_fts_stale();
    void set_fts_stale() noexcept;
    void clear_fts_stale() noexcept;
    void maybe_merge_fts() noexcept;

    kimix::vector<turn_meta> search_fts_unicode61(kimix::string_view query,
                                                  uint32_t top_k);
    kimix::vector<turn_meta> search_fts_trigram(kimix::string_view raw_query,
                                                uint32_t top_k);
    kimix::vector<turn_meta> search_like(kimix::string_view raw_query,
                                         uint32_t top_k);
    // D9: run one FTS5 MATCH with the given MATCH string, apply the
    // min_should_match gate when there are >=2 unique query tokens, and
    // truncate to top_k. Returns false when the FTS layer errored (caller
    // degrades to LIKE + stale breadcrumb).
    bool run_fts_match(const char *fts_table, kimix::string_view match,
                       kimix::span<const kimix::string> unique_tokens,
                       uint32_t top_k, kimix::vector<turn_meta> &out);
    // D9 fuzzy retry: build ("tok" OR "fuzzy-variant" ...) AND groups from
    // the symmetric-delete index fed by appended corpus words. No-op (false)
    // when the fuzzy index is empty or the query has no Latin tokens.
    bool fuzzy_match_query(kimix::span<const kimix::string> unique_tokens,
                           kimix::string &match_out) const;
    // Feed the ASCII word vocabulary of an appended text into _fuzzy.
    static void feed_fuzzy(search::SymmetricDeleteIndex &fuzzy,
                           kimix::string_view text);

    kimix::filesystem::path _db_path;
    sqlite3 *_db = nullptr;
    uint32_t _doc_id_counter = 0; // D6
    bool _fts_stale = false;
    uint32_t _writes_since_merge = 0;
    search::SymmetricDeleteIndex _fuzzy; // D9
};

} // namespace index
} // namespace runtime
} // namespace kimix
