/*
 * cold_store.h — INTERNAL storage-backend seam for the cold storage of
 * kimix-graph-db.
 *
 * Not part of the public API and not pulled in by graph_db.h: it exists so the
 * save/load path (serialize.h/.cpp) and the Database facade talk to "a cold
 * store" instead of to ColdStorage specifically. That is what lets the file
 * backend be selected without touching either caller.
 *
 * WHAT THE SEAM IS
 *   ColdStore is a NON-OWNING handle over exactly one of two backends:
 *
 *     ColdStorage   the LMDB environment of utils.h — the default, selected by
 *                   GraphOptions::store_backend == ColdStoreBackend::lmdb.
 *     FileColdStore the second backend: its OWN LMDB environment at
 *                   <dir>\graph.xdb (file_cold_store.h) — selected by
 *                   ColdStoreBackend::flat_file.
 *
 *   The two backends are the alternatives of ONE kimix::variant of POINTERS, so
 *   the handle is
 *   (a) non-owning — it never copies, closes or moves the store it refers to,
 *       exactly like the `ColdStorage &` it replaced. That matters: a caller
 *       keeps using its store after the serializer is gone (the serializer and
 *       the test suites both do), so the seam must not acquire ownership;
 *   (b) non-virtual — dispatching through std::visit resolves at compile time,
 *       so the save path pays no indirect call and the build needs no RTTI
 *       (RTTI is off project-wide);
 *   (c) LIVE ONLY WHILE ITS STORE IS. The store a handle points at exists inside a
 *       Database::ColdScope (database.h) and nowhere else: Database opens one
 *       environment per cold operation (open's load, persist, load_store), builds
 *       the serializer over the handle for that operation and destroys both before
 *       it returns. A handle therefore never outlives the operation that made it —
 *       and that is also why `Database::storage()` reports nullptr between
 *       operations instead of handing out a closed store.
 *
 *   ColdStore is copied freely (two pointers), and an implicit converting
 *   constructor from each backend is what keeps every existing call site spelled
 *   exactly as it was:
 *
 *     ColdStorage cold;
 *     cold.open(dir);
 *     GraphSerializer serializer{cold};          // ColdStorage & -> ColdStore
 *     deserializer.load(cold, graph, &schema);   // same, per call
 *
 * THE SURFACE IS EXACTLY WHAT THE CALLERS USE
 *   valid(), read_meta(), save_batch(), load_all_records(), copy_to(), path(),
 *   cancel_pending_rebuild().
 *   That is the complete set used by GraphSerializer, GraphDeserializer and
 *   Database. ColdStorage's other operations (read_record, scan_prefix,
 *   write_meta, save_records, remove_keys, remove_record) stay on ColdStorage and
 *   are deliberately NOT part of the seam: the test suites use them directly on a
 *   concrete ColdStorage, and FileColdStore should not have to grow an operation
 * open() / close() are likewise an owner concern — Database owns
 * the concrete store and selects a backend when it opens — not a seam operation.
 * Because the owner is now Database::ColdScope, open() and close() of the concrete
 * backend happen once per cold operation, not once per session: nothing is mapped
 * (and no data.mdb/lock.mdb handle is held) while the session idles.
 *
 * CONTRACT
 *   Every operation forwards to the backend and reports what the backend reports:
 *   the seam adds no caching, no retry, no fallback and no translation. A closed
 *   store therefore still reports false from valid() and from every operation,
 *   which is what keeps "the store is not open" one single code path
 *   (serialize.cpp reports error_io / empty stats) instead of two — including the
 *   FileColdStore whose open() failed because its file could not be read.
 *
 *   save_batch() is the seam's only write: it means "append this batch to the
 *   store", i.e. the incremental-save semantics. The FileColdStore's truncating
 *   full save is its own method (rebuild()), armed by Database::persist(full).
 *
 * Example:
 *   kimix::graph_db::ColdStorage cold;
 *   cold.open(kimix::graph_db::default_cold_dir());
 *   kimix::graph_db::ColdStore store{cold};
 *   kimix::graph_db::MetaRecord meta;
 *   if (store.valid() && store.read_meta(meta)) {
 *       // ... format negotiation, exactly as the deserializer does it
 *   }
 */
#pragma once

#include "file_cold_store.h"
#include "utils.h"

#include <core/lmdb.h>
#include <core/stl/filesystem.h>
#include <core/stl/variant.h>
#include <core/stl/vector.h>

#include <utility>

namespace kimix::graph_db {

// ---------------------------------------------------------------------------
// ColdStore — the seam
// ---------------------------------------------------------------------------

/*
 * A handle, not a store: it holds a pointer to the backend it was built from and
 * nothing else, so it must not outlive that backend (neither does the reference
 * it replaced). Both converting constructors are implicit on purpose — that is
 * what lets `GraphSerializer serializer{cold}` and `load(cold, graph)` keep
 * taking the concrete ColdStorage the test suites own.
 *
 * Handles are copyable and assignable, so an owner that switches backends can
 * re-point one instead of rebuilding its members (that is what Database::open
 * does, and its serializer is rebuilt with the re-pointed handle).
 */
class ColdStore {
public:
    ColdStore(ColdStorage &storage) noexcept
        : _store(&storage) {
    }
    ColdStore(FileColdStore &storage) noexcept
        : _store(&storage) {
    }
    ColdStore(ColdStore const &) noexcept = default;
    ColdStore &operator=(ColdStore const &) noexcept = default;
    ~ColdStore() noexcept = default;

    // True when the backend is open. Every other operation of a backend that is
    // not open reports false without touching the disk.
    [[nodiscard]] bool valid() const noexcept {
        return std::visit([](auto const *store) noexcept { return store->valid(); }, _store);
    }

    // The directory the backend lives in; empty for a closed store (the file
    // backend's file is its kColdFileName inside that directory).
    [[nodiscard]] kimix::filesystem::path path() const noexcept {
        return std::visit([](auto const *store) noexcept { return store->path(); }, _store);
    }

    // The store's meta record; false when it is absent (a v1 store) or does not
    // decode.
    [[nodiscard]] bool read_meta(MetaRecord &out) const noexcept {
        return std::visit([&out](auto const *store) noexcept { return store->read_meta(out); },
                          _store);
    }

    // One sequential scan of every key/value pair in the store (both backends
    // return LMDB's map order; the second one keeps its own environment, so the
    // result is the same set of records either way).
    [[nodiscard]] bool load_all_records(kimix::vector<ColdRecord> &out) const noexcept {
        return std::visit(
            [&out](auto const *store) noexcept { return store->load_all_records(out); }, _store);
    }

    // One atomic write batch (incremental save). The key spellings are already
    // guid_key() / edge_key() / "meta/root" by the time they arrive here.
    [[nodiscard]] bool
    save_batch(kimix::vector<kimix::LMDBWriteCommand> &&commands) const noexcept {
        return std::visit(
            [&commands](auto const *store) noexcept {
                return store->save_batch(std::move(commands));
            },
            _store);
    }

    // Compacted snapshot of the whole store into `target` (checkpoint): both
    // backends copy their LMDB environment and both speak a DIRECTORY.
    [[nodiscard]] bool copy_to(kimix::filesystem::path const &target) const noexcept {
        return std::visit([&target](auto const *store) noexcept { return store->copy_to(target); },
                          _store);
    }

    // Drop any pending "the next batch is a full snapshot" arm. A no-op for the
    // LMDB-in-the-store-directory backend, which has no such arm; the file backend
    // clears it. GraphSerializer::save_incremental calls this so an arm left behind
    // by a caller can never turn a later INCREMENTAL batch into a full save (which
    // would mdb_drop every record the batch does not carry). Inside a Database scope
    // the arm cannot outlive the store anyway — the store dies with the scope — so
    // this stays as the guarantee for a caller that owns a long-lived store.
    void cancel_pending_rebuild() const noexcept {
        std::visit([](auto const *store) noexcept { store->cancel_pending_rebuild(); }, _store);
    }

private:
    // Which backend, not what state it is in: the store itself owns its open /
    // closed state and reports it through valid().
    kimix::variant<ColdStorage *, FileColdStore *> _store;
};

} // namespace kimix::graph_db