/*
 * transaction.cpp — the undo journal and its size accounting (see transaction.h).
 *
 * The rollback itself lives in database.cpp, because replaying an entry needs the
 * Database's graph and schema (a Transaction owns no state of its own beyond the
 * log). This file is deliberately dumb: it stores entries, sums their footprint
 * and enforces the cap.
 */
#include "transaction.h"

#include "database.h" // the rollback lives there (Transaction only owns the log)

#include <core/guid.h>

#include <utility>

namespace kimix::graph_db {

namespace src_graph_db_transaction {

uint64_t next_transaction_id() noexcept {
    // One process-wide counter: transaction ids only ever appear in diagnostics.
    // (A function-local static, not a file-scope one: the library is built with
    // unity batches, and this keeps the counter in one translation unit's
    // function rather than at namespace scope.)
    static uint64_t counter = 0;
    return ++counter;
}

} // namespace src_graph_db_transaction
using namespace src_graph_db_transaction;

size_t JournalEntry::approximate_bytes() const noexcept {
    size_t bytes = sizeof(JournalEntry);
    if (!fat) {
        return bytes;
    }
    bytes += sizeof(Fat);
    bytes += fat->name.size();
    bytes += fat->info.name.size();
    bytes += fat->labels.size() * sizeof(LabelId);
    for (auto const &entry : fat->properties) {
        bytes += entry.value.as_string().size() + entry.value.as_blob().size();
        bytes += entry.value.as_int64_array().size() * sizeof(int64_t);
        bytes += entry.value.as_double_array().size() * sizeof(double);
        // A string_array owns its whole §1.1 wire block, count and per-element
        // lengths included (ABI 7) — the block's byte size IS the payload footprint.
        bytes += entry.value.as_string_array_block().size();
    }
    if (fat->vertex) {
        bytes += sizeof(Vertex) + fat->vertex->content_size();
    }
    for (auto const &removed : fat->removed_edges) {
        if (removed) {
            bytes += sizeof(Edge);
        }
    }
    if (fat->edge) {
        bytes += sizeof(Edge);
    }
    return bytes;
}

Transaction::Transaction(Database &database, bool read_only,
                         size_t max_journal_bytes) noexcept
    : _database(database),
      _max_journal_bytes(max_journal_bytes),
      _id(next_transaction_id()),
      _read_only(read_only) {
}

Transaction::~Transaction() noexcept {
    // An open transaction is aborted on destruction, so an early return can never
    // leave a half-applied batch behind. `_database` is only touched when the
    // transaction is still open AND not read-only (a read-only one has nothing to
    // undo); Database::abort() clears `_tx`, so this cannot recurse.
    if (!_closed && !_read_only) {
        (void)_database.abort();
    }
}

bool Transaction::push(JournalEntry entry) {
    size_t const bytes = entry.approximate_bytes();
    if (_max_journal_bytes != 0 && _journal_bytes + bytes > _max_journal_bytes) {
        // The cap is a REFUSAL, not a crash: the caller reports error_size_limit
        // and the transaction stays open (and therefore abortable).
        _overflowed = true;
        return false;
    }
    _journal_bytes += bytes;
    _journal.push_back(std::move(entry));
    return true;
}

void Transaction::clear_journal() noexcept {
    _journal.clear();
    _journal_bytes = 0;
    _overflowed = false;
}

kimix::vector<JournalEntry> Transaction::take_journal() noexcept {
    kimix::vector<JournalEntry> out;
    out.swap(_journal);
    _journal_bytes = 0;
    return out;
}

} // namespace kimix::graph_db
