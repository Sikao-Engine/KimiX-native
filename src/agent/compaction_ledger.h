// agent/compaction_ledger.h - C++ port of kimi_cli/soul/compaction_ledger.py:
// a durable, append-only JSONL ledger of compaction transactions.
//
// Reference (read in full): kimi-cli/src/kimi_cli/soul/compaction_ledger.py
//   * CompactionRecord{compaction_id, trigger in {auto,manual,overflow},
//     started_at, shadowed_range, shadowed_tokens, summary_tokens,
//     preserved_tokens, shrank, error} - one JSON object per line, written
//     with orjson (compact, OPT_APPEND_NEWLINE), key order exactly as
//     _record_to_dict emits it.
//   * Path: <session_dir>/".kimix_cache"/"compaction_ledger.jsonl" (the
//     nested cache directory of the session directory, so the durability
//     story matches the deterministic context_compacted.md export slot).
//   * record_start appends the start-of-transaction line BEFORE the LLM call
//     (summary_tokens 0, shrank false, error null).
//   * record_end REWRITES the single matching line in place (the ledger stays
//     exactly one line per transaction): the error key is popped on success
//     and set on failure; summary_tokens / shrank are updated when provided.
//     No matching line -> the file is left untouched (we never invent data for
//     a transaction we did not record).
//   * FAILURE ISOLATION IS THE CONTRACT: no public method may fail the
//     compaction. Every method reports through bool + `error` and the soul
//     ignores the result; a broken or unwritable path degrades to a no-op
//     ledger (path empty) exactly like the reference's CompactionLedger(None).
//   * latest() returns the last record; malformed lines are skipped. The
//     records loaded on session open back the telemetry view (records()).
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers,
// no exceptions / no RTTI, yyjson with the mimalloc allocator.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

namespace kimix::agent {

// One compaction transaction, as persisted to the ledger
// (compaction_ledger.py CompactionRecord).
struct CompactionRecord {
    kimix::string compaction_id; // uuid4 hex (random_hex(16) x2 here)
    kimix::string trigger;       // "auto" | "manual" | "overflow"
    double started_at = 0.0;     // epoch seconds (time.time())
    // shadowed_range: the history indices [start, end) replaced by the summary.
    int64_t shadowed_start = 0;
    int64_t shadowed_end = 0;
    int64_t shadowed_tokens = 0; // estimated tokens of the replaced region
    int64_t summary_tokens = 0;  // LLM usage.output when available, else estimate
    int64_t preserved_tokens = 0;// estimated tokens of the preserved tail
    bool shrank = false;         // summary_tokens < shadowed_tokens
    bool has_error = false;      // error: null when unset (the JSON key is ABSENT
    kimix::string error;         //  after a successful record_end, per _record_end)
};

// Durable append-only ledger of compaction transactions.
//
// Lifecycle: CompactionLedger(path) with an EMPTY path is the no-op ledger
// (every method returns immediately) - the analogue of the reference's
// CompactionLedger(None), used when the ledger is disabled or its directory
// cannot be created. Use for_session() to get exactly that behaviour.
class CompactionLedger {
public:
    CompactionLedger() = default;
    // An empty `path` creates the no-op ledger.
    explicit CompactionLedger(kimix::string path);

    // for_session(session_dir, enabled): path =
    // <session_dir>/.kimix_cache/compaction_ledger.jsonl. When `enabled` is
    // false (or the cache directory cannot be created) returns the no-op
    // ledger whose methods never touch the filesystem (compaction_ledger.py
    // for_session).
    static CompactionLedger for_session(kimix::string_view session_dir,
                                        bool enabled);

    bool enabled() const noexcept { return !_path.empty(); }
    const kimix::string &path() const noexcept { return _path; }

    // Append the start-of-transaction record. Never raises (failure isolation):
    // returns false + `error` when the write failed; the caller ignores it.
    bool record_start(const CompactionRecord &record, kimix::string &error);

    // Finalize a transaction: rewrite the file replacing the single line whose
    // compaction_id matches. `has_error`/`error` set the (or clear the) error
    // field; `update_summary` / `update_shrank` gate the corresponding field
    // updates (the reference's None == "leave unchanged"). Never raises.
    bool record_end(kimix::string_view compaction_id, bool has_error,
                    kimix::string_view error, bool update_summary,
                    int64_t summary_tokens, bool update_shrank, bool shrank,
                    kimix::string &error_out);

    // Convenience wrappers mirroring the reference's keyword form.
    bool record_end_success(kimix::string_view compaction_id,
                            int64_t summary_tokens, bool shrank,
                            kimix::string &error) {
        return record_end(compaction_id, false, {}, true, summary_tokens, true,
                          shrank, error);
    }
    bool record_end_failure(kimix::string_view compaction_id,
                            kimix::string_view error, kimix::string &error_out) {
        return record_end(compaction_id, true, error, false, 0, false, false,
                          error_out);
    }

    // The last record in the file, or nullopt. Never raises.
    kimix::optional<CompactionRecord> latest() const;
    // Every record, malformed lines skipped (compaction_ledger.py
    // _read_records). Loaded on session open for telemetry. Never raises.
    kimix::vector<CompactionRecord> records() const;

private:
    kimix::string _path; // empty == no-op ledger
};

} // namespace kimix::agent
