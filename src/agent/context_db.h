// agent/context_db.h - C++ port of kimi_cli/soul/context_db.py's ContextDB:
// a SQLite-backed durable store for conversation context, plus the
// context.py:558-576 backend auto-detection / JSONL->SQLite migration helpers.
//
// Reference sources (read-only, kimi-agent):
// kimi-cli/src/kimi_cli/soul/context_db.py (ContextDB: schema, WAL, pragma
//   tuning, explicit transactions, rowid pagination, import_jsonl_line,
//   fix_checkpoint_message_rowids)
// kimi-cli/src/kimi_cli/soul/context.py (ContextStorage protocol,
//   _detect_storage_backend / _resolve_storage_path / _needs_migration)
// kimi-cli/src/kimi_cli/session.py (_migrate_jsonl_to_sqlite: transaction,
//   row-count verify, .jsonl.bak rename, partial-DB cleanup)
// kimi-cli/src/kosong/utils/jsonx.py (loads_relaxed: strict parse first,
//   json_repair fallback) and the JSONL record byte shape asserted by
//   tests/unit/cli/test_cli.cpp (session_history_roundtrip)
//
// Exception-free by construction: the sqlite3 C API never throws and every
// method reports failure through bool + `error` out-parameters, matching the
// session store conventions (src/cli/cli_session.{h,cpp}). RAII: the
// destructor closes the connection (passive WAL checkpoint first).
#pragma once

#include <cstdint>
#include <core/kimix_core.h>
#include "llm/llm.h"

struct sqlite3;// sqlite3.h is C; forward-declared so this header stays light
struct sqlite3_stmt;

namespace kimix::agent {

// ---------------------------------------------------------------------------
// Backend auto-detection (context.py:558-576)
// ---------------------------------------------------------------------------

enum class ContextBackend {
    jsonl,// legacy flat-file backend
    sqlite// ContextDB backend (".db" suffix)
};

// _detect_storage_backend: ".db" suffix -> SQLite, anything else -> JSONL.
ContextBackend detect_context_backend(kimix::string_view path) noexcept;

// _resolve_storage_path: a ".jsonl" path maps onto the corresponding ".db"
// path; any other path is returned unchanged.
kimix::string resolve_context_db_path(kimix::string_view path);

// _needs_migration: true when the JSONL side exists and the DB side does not.
// Unrepresentable paths count as "does not exist".
bool needs_context_migration(kimix::string_view path) noexcept;

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------

// One durable context record. `content` carries the exact serialized JSON the
// legacy context.jsonl stores on one line (a message dump, or a meta payload
// for _system_prompt / _usage / _checkpoint), so a DB row round-trips to the
// same bytes a JSONL line would have had.
struct ContextRecord {
    kimix::string role;
    kimix::string content;// serialized record JSON (no trailing newline)
};

// Serialize one chat message as the reference's JSONL record
// (model_dump_json(exclude_none=True)): role, bare-string-or-part-array
// content (think/text parts), tool_calls, tool_call_id. Byte-compatible with
// the writer the native CLI persists today (src/cli/cli_session.cpp).
kimix::string context_record_from_message(const kimix::llm::Message &msg);

// Inverse of context_record_from_message for message records (roles system /
// user / assistant / tool). Returns false for meta roles (_system_prompt,
// _usage, _checkpoint) and structurally invalid records, mirroring the
// reference's lenient read path which skips them.
bool context_message_from_record(kimix::string_view record, kimix::llm::Message &out);

// ---------------------------------------------------------------------------
// ContextDb
// ---------------------------------------------------------------------------

// SQLite context store. Lifecycle: construct -> open() -> use -> close()
// (or let the destructor close). One instance holds one sqlite3 connection;
// the library is compiled SQLITE_THREADSAFE=1 and busy_timeout is set, so
// distinct instances may touch the same DB file concurrently.
class ContextDb {
public:
    explicit ContextDb(kimix::filesystem::path db_path);
    ~ContextDb();
    ContextDb(ContextDb &&other) noexcept;
    ContextDb &operator=(ContextDb &&other) noexcept;
    ContextDb(const ContextDb &) = delete;
    ContextDb &operator=(const ContextDb &) = delete;

    const kimix::filesystem::path &db_path() const noexcept { return _db_path; }
    bool is_open() const noexcept { return _db != nullptr; }

    // Opens (creating if needed) the DB file, applies the WAL pragma set and
    // creates the 5-table schema inside a transaction when missing.
    bool open(kimix::string &error);
    // Passive WAL checkpoint (best effort) + connection close; safe to call twice.
    void close() noexcept;

    // Explicit transaction bracket for bulk work (context_db.py:275-293).
    // Mutating methods auto-commit when no explicit transaction is active.
    bool begin_transaction(kimix::string &error);
    bool commit_transaction(kimix::string &error);
    bool rollback_transaction(kimix::string &error);
    bool in_transaction() const noexcept { return _in_transaction; }

    // ---- Messages (append + rowid pagination) ---- //

    // Appends one already-serialized record (any role, meta roles included).
    bool append(const ContextRecord &rec, kimix::string &error);
    bool append_batch(kimix::span<const ContextRecord> recs, kimix::string &error);
    // Serializes msg with context_record_from_message() and appends it.
    bool append_message(const kimix::llm::Message &msg, kimix::string &error);

    // One messages row: the stored record plus its rowid and created_at.
    struct MessageRow {
        int64_t rowid = 0;
        double created_at = 0.0;
        kimix::string role;
        kimix::string content;// serialized record JSON
    };

    // get_messages(after_rowid, limit): rows with rowid > after_rowid ordered
    // by rowid; limit < 0 means no limit (the reference's limit=None).
    bool read_after(int64_t after_rowid, int64_t limit, kimix::vector<MessageRow> &out,
                    kimix::string &error) const;
    bool read_all(kimix::vector<MessageRow> &out, kimix::string &error) const {
        return read_after(0, -1, out, error);
    }
    bool message_count(int64_t &out, kimix::string &error) const;
    bool last_message_rowid(int64_t &out, kimix::string &error) const;
    // True when a non-meta role message exists (context_db.py:373-388).
    bool has_visible_messages(bool &out, kimix::string &error) const;

    // Delete-range: removes every message with rowid > `rowid`.
    bool delete_after(int64_t rowid, kimix::string &error);
    // Removes every row from all 5 tables, inside a transaction.
    bool clear(kimix::string &error);
    // Selective resets used by the session store's divergent-rewrite path
    // (a compaction/prune replaced the history = the reference's
    // replace_history): the checkpoints and usage snapshots anchored to the
    // replaced rows are reset, the system prompt singleton is kept.
    bool clear_checkpoints(kimix::string &error);
    bool clear_usage(kimix::string &error);

    // ---- System prompt ---- //

    bool set_system_prompt(kimix::string_view content, kimix::string &error);
    bool get_system_prompt(kimix::string &out, bool &found, kimix::string &error) const;

    // ---- Usage snapshots ---- //

    bool record_usage(int64_t token_count, kimix::string &error);
    bool latest_usage(int64_t &out, bool &found, kimix::string &error) const;
    // Every usage snapshot, oldest first (context_db.py export's usages list).
    bool export_usage_history(kimix::vector<int64_t> &out,
                              kimix::string &error) const;

    // ---- Checkpoints ---- //

    // Records a checkpoint at the current max message rowid and returns that
    // rowid through `message_rowid`.
    bool create_checkpoint(int64_t checkpoint_id, int64_t &message_rowid,
                           kimix::string &error);
    // -1 when no checkpoint exists (context_db.py:747-752).
    bool latest_checkpoint_id(int64_t &out, kimix::string &error) const;
    // Every checkpoint id, ascending (context_db.py export's checkpoint list).
    bool list_checkpoint_ids(kimix::vector<int64_t> &out,
                             kimix::string &error) const;
    bool checkpoint_message_rowid(int64_t checkpoint_id, int64_t &out, bool &found,
                                  kimix::string &error) const;
    // Deletes all messages / checkpoints / usage snapshots after the given
    // checkpoint, in a transaction with rollback on error.
    bool revert_to_checkpoint(int64_t checkpoint_id, kimix::string &error);
    // import-time repair: checkpoints still at rowid 0 get message_rowid = id
    // (context_db.py:989-1003).
    bool fix_checkpoint_message_rowids(kimix::string &error);

    // ---- JSONL -> SQLite migration (session.py:740-850) ---- //

    // Imports every record of `jsonl_path` into this DB in one transaction,
    // then renames the JSONL to ".jsonl.bak" (session.py's backup) and returns
    // `migrated = true`. Idempotent: when the JSONL is gone, or a DB file
    // already exists at the resolved path, nothing happens and
    // `migrated = false`. Malformed lines are skipped (loads_relaxed
    // semantics). On failure the transaction rolls back and a DB file this
    // call created is removed, mirroring session.py's partial-DB cleanup.
    bool migrate_jsonl(const kimix::filesystem::path &jsonl_path, bool &migrated,
                       kimix::string &error);

private:
    bool exec(const char *sql, kimix::string &error) const;
    bool prepare(const char *sql, sqlite3_stmt *&stmt, kimix::string &error) const;
    // COMMIT helper used by mutating paths when no explicit tx is active.
    bool maybe_commit(kimix::string &error) const;

    kimix::filesystem::path _db_path;
    sqlite3 *_db = nullptr;
    bool _in_transaction = false;
    int64_t _last_message_rowid = 0;// import-time checkpoint anchor
};

} // namespace kimix::agent
