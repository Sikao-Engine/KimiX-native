/*
 * file_cold_store.h — the second cold-store backend of kimix-graph-db: ONE
 * self-contained LMDB environment at <dir>\graph.xdb, written through
 * <core/lmdb.h>.
 *
 * This is the second backend behind the internal cold_store.h seam
 * (kimix::graph_db::ColdStore); ColdStorage (utils.h, LMDB in the store directory
 * itself) stays the default. Nothing in the public graph_db.h surface mentions
 * this header — a caller selects it through GraphOptions::store_backend, and
 * Database is what opens it.
 *
 * WHAT IT IS
 *   An LMDB environment of its OWN, rooted at <dir>\graph.xdb (kColdFileName)
 *   instead of at <dir>: the backend owns its data file (graph.xdb\data.mdb) and
 *   its lock file (graph.xdb\lock.mdb), so one directory can hold a store of this
 *   backend next to anything else, and a checkpoint is one directory copy.
 *
 *   This backend used to be a hand-framed append-only log written with
 *   fread/fwrite. It is now a key/value store — the records are exactly LMDB's
 *   key/value pairs — so
 *     - an incremental save is ONE LMDB transaction (only the batch's keys are
 *       touched: the map is updated in place, nothing is appended and no superseded
 *       copy of a record survives),
 *     - a load is one cursor over the memory-mapped file,
 *     - the atomicity boundary is LMDB's transaction commit, not a length-prefixed
 *       frame header: there is no torn tail to detect, no frame walk to repair and no
 *       last-wins dedupe to do, because a map has one record per key by construction,
 *     - the durable-write story is LMDB's: one commit per save batch.
 *
 *   THE STORE — <dir>\graph.xdb  (an LMDB environment DIRECTORY)
 *     graph.xdb\data.mdb   the database (LMDB page format, meta pages first)
 *     graph.xdb\lock.mdb   LMDB's lock table (created on open, never payload)
 *
 *   The keys are the existing store spellings — "v/" + 16 guid bytes, "e/" + 16
 *   edge-id bytes, "s/labels", "s/proptypes", "meta/root" (utils.h) — so the
 *   loader's prefix partition works unchanged, and a value with length 0 IS a
 *   tombstone (the vocabulary a save writes for a deleted record; a map store can
 *   also simply drop the key — ColdStorage does — and the loader reads both the
 *   same way).
 *
 *   INCREMENTAL vs FULL SAVE
 *     incremental save -> save_batch(): ONE transaction that puts the batch's keys
 *                         and nothing else. A record of a previous save round is
 *                         replaced in place, so the store never grows by more than
 *                         the batch.
 *     full save        -> rebuild(): ONE transaction that empties the database and
 *                         writes the whole snapshot (LMDB's mdb_drop + puts in the
 *                         same txn), so a reader sees either the old store or the
 *                         new snapshot — never a half-written one. This is how a
 *                         record that is no longer in the graph is dropped; it only
 *                         makes sense when the batch IS the complete store.
 *   A seam batch carries no "full" flag (serialize.cpp writes both kinds through
 *   the same save_batch()), so Database::persist(full=true) asks for the rebuild
 *   explicitly: set_rebuild_next_batch(true) makes the NEXT save_batch() a full
 *   save. rebuild() itself is public, so a caller can ask for it directly.
 *
 *   META IS A RECORD, NOT A HEADER FIELD
 *   MetaRecord is variable length (graph_name) and already has a versioned, total
 *   codec (encode_meta_record), so it is stored as the ordinary "meta/root" record
 *   the serializer already appends — same bytes, same decode path, no second copy
 *   of the format. read_meta() is one LMDB lookup (no scan, nothing to cache).
 *
 *   PATHS
 *   open()/path()/copy_to() speak DIRECTORIES, like ColdStorage: the store always
 *   lives at <dir>\graph.xdb, so `path() == dir` and a copy_to() checkpoint is a
 *   directory that reopens as a store of its own.
 *
 *   MIGRATION
 *   A graph.xdb written by the PREVIOUS flat-file layout is a regular FILE, and this
 *   backend's store is a DIRECTORY, so open() refuses such a path (plain false)
 *   instead of misreading it: a store of the old layout has to be re-saved from the
 *   live graph (or deleted) before this backend can use that directory. Nothing is
 *   converted automatically.
 *
 *   DETECTED DRIVERS / LIMITS
 *     - LMDB itself is fatal on error (see <core/lmdb.h>: a failing mdb_* call is
 *       reported to stderr and aborts). This backend therefore validates the store
 *       path BEFORE the environment is opened, so "not a store this build wrote" is a
 *       plain false instead of an abort: <dir>/graph.xdb must be absent (created on
 *       open) or a DIRECTORY, and a data.mdb inside it must carry LMDB's own meta page
 *       magic/version. A regular file at graph.xdb and a corrupt environment are both
 *       refused.
 *     - A key must be non-empty and at most kMaxKeyLength (511) bytes: that is LMDB's
 *       own MDB_MAXKEYSIZE, and a longer key inside LMDB would be an abort, so
 *       save_batch()/rebuild() refuse the whole batch instead.
 *     - The store's ceiling is the map size the environment was opened with
 *       (GraphOptions::cold_map_size, 64 GiB by default). A commit that runs past
 *       it is MDB_MAP_FULL, i.e. one of the LMDB-level failures kimix::LMDB
 *       reports and aborts on — so size cold_map_size for the store the caller
 *       intends to write. The volume, not the file, is what such a store runs out
 *       of.
 *
 * CONTRACT (identical to ColdStorage's, and to the seam's)
 *   Every operation reports what the backend reports: false is "not open / absent /
 *   failed", never an exception (the project is built without them). An empty scan
 *   result is NOT an error. Accepted-but-ignored: nothing — open()'s map_size is the
 *   LMDB map size this backend really uses (0 means default_map_size), the same knob
 *   ColdStorage takes.
 *
 * Example:
 *   kimix::graph_db::FileColdStore store;
 *   if (store.open(kimix::graph_db::default_cold_dir())) {   // creates <dir>\graph.xdb
 *       kimix::graph_db::ColdStore seam{store};              // what the serializer takes
 *       kimix::graph_db::MetaRecord meta;
 *       if (seam.valid() && seam.read_meta(meta)) { ... }
 *   }
 */

#pragma once

#include "utils.h"

#include <core/lmdb.h>
#include <core/memory.h>
#include <core/stl/filesystem.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <cstddef>
#include <cstdint>

namespace kimix::graph_db {

// Name of the store INSIDE the store directory: an LMDB environment directory.
// The DIRECTORY is the unit that open() / path() / copy_to() speak (the same unit
// ColdStorage's LMDB environment directory is), so a caller's path handling is
// backend-agnostic; kColdDirName (utils.h) still names the default directory.
inline constexpr kimix::string_view kColdFileName = "graph.xdb";

/*
 * The LMDB-backed second backend: one environment at <dir>\graph.xdb, move-only
 * RAII, never throwing. See the file comment for the layout, the incremental/full
 * save contract and the limits.
 *
 * Threading: like ColdStorage, it is NOT internally synchronized — one writer (the
 * single-writer contract the LMDB backend also relies on) and readers that do not
 * race a save. LMDB's own reader table is what makes concurrent readers safe
 * against one writer; this class adds no lock of its own.
 */
class KIMIX_GRAPH_DB_API FileColdStore : public kimix::IOperatorNewBase {
public:
    // The map size a store opened without one gets: kimix::LMDB's own default and
    // ColdStorage::default_map_size, so the two backends agree when a caller has no
    // opinion (GraphOptions::cold_map_size == 0).
    static constexpr size_t default_map_size = 1024ull * 1024ull * 1024ull * 64ull;
    // LMDB's MDB_MAXKEYSIZE. A longer (or empty) key is a batch this backend
    // refuses; handing it to LMDB would abort the process.
    static constexpr size_t kMaxKeyLength = 511;
    // The two files an LMDB environment directory holds. data.mdb is the store;
    // lock.mdb is LMDB's reader/writer table and carries no payload.
    static constexpr kimix::string_view kDataFileName{"data.mdb"};
    static constexpr kimix::string_view kLockFileName{"lock.mdb"};

    FileColdStore() noexcept = default;
    // Opens (and creates) the store in `dir`; see open().
    explicit FileColdStore(kimix::filesystem::path const &dir,
                           size_t map_size = default_map_size) noexcept;
    FileColdStore(FileColdStore const &) = delete;
    FileColdStore &operator=(FileColdStore const &) = delete;
    FileColdStore(FileColdStore &&) noexcept = default;
    FileColdStore &operator=(FileColdStore &&) noexcept = default;
    ~FileColdStore() noexcept = default;

    /*
     * Open (and create) the LMDB environment at <dir>\graph.xdb, creating `dir` and
     * the environment directory as needed. The environment (and therefore the store
     * handle) is held open until close(), which is what makes the store deletable on
     * Windows only after it is closed.
     *
     * Returns false — leaving the store closed, with every operation a no-op that
     * reports false — for: an empty path, a directory that cannot be created, a
     * graph.xdb that is a regular FILE instead of an environment directory, and an
     * environment whose data.mdb does not carry LMDB's meta page magic/version
     * (a corrupt or foreign store).
     *
     * `map_size` is the LMDB map size (GraphOptions::cold_map_size); 0 means
     * default_map_size.
     */
    bool open(kimix::filesystem::path const &dir,
              size_t map_size = default_map_size) noexcept;
    // Close the environment (releasing its files, so the directory becomes
    // deletable on Windows). Idempotent; nothing is written.
    void close() noexcept;
    [[nodiscard]] bool valid() const noexcept;
    explicit operator bool() const noexcept { return valid(); }
    // The store DIRECTORY (empty while closed) — the environment inside it is
    // <dir>\graph.xdb, never the path this returns.
    [[nodiscard]] kimix::filesystem::path path() const noexcept { return _path; }

    // The store's "meta/root" record. False when the store is closed, when it
    // carries no meta record (a v1 store: no metadata at all), when the record is a
    // tombstone, or when it does not decode.
    [[nodiscard]] bool read_meta(MetaRecord &out) const noexcept;

    // One cursor pass over the environment: every record of the store, in LMDB key
    // order, tombstones included as empty values. Returns false only when the store
    // is closed; an empty store scans to an empty vector and returns true.
    [[nodiscard]] bool
    load_all_records(kimix::vector<ColdRecord> &out) const noexcept;

    // INCREMENTAL save: put this batch's records in ONE transaction and touch
    // nothing else. The command order is irrelevant (LMDB stores a map), and the
    // keys must already be store spellings. Returns false — with nothing written —
    // when the store is closed or when a key is empty or longer than kMaxKeyLength.
    [[nodiscard]] bool
    save_batch(kimix::vector<kimix::LMDBWriteCommand> &&commands) const noexcept;

    // FULL save: empty the environment and write the whole batch in ONE
    // transaction. Only valid when `commands` is a COMPLETE snapshot of what the
    // store must hold: an incremental (dirty-only) batch written this way would drop
    // everything else. Returns false without destroying the old store when the store
    // is closed, when a key is invalid, or when nothing is written.
    [[nodiscard]] bool
    rebuild(kimix::vector<kimix::LMDBWriteCommand> &&commands) const noexcept;

    // Route the NEXT save_batch() through rebuild() instead of the incremental put,
    // because the seam's save_batch() cannot tell a full batch from an incremental one
    // (Database::persist() sets it for a full save). Only ever set it when the batch
    // about to be written IS the complete store.
    //
    // The arm is consumed by the next save_batch() that REACHES the store — so a save
    // that writes nothing at all (a clean incremental save returns before calling
    // save_batch()) leaves it armed, and the batch after that one would be written with
    // the database emptied first. cancel_pending_rebuild() drops a stale arm, and
    // GraphSerializer::save_incremental() calls it before it writes.
    void set_rebuild_next_batch(bool rebuild) noexcept { _rebuild_next = rebuild; }

    // Drop a pending "the next batch is a full snapshot" arm (see
    // set_rebuild_next_batch()); a no-op when none is pending.
    void cancel_pending_rebuild() const noexcept { _rebuild_next = false; }

    // Compacted snapshot of the whole store into the `target` DIRECTORY as
    // <target>\graph.xdb (created on demand, replacing an earlier checkpoint), by
    // LMDB's own compacting environment copy. The copy is closed before returning,
    // so the target directory is immediately deletable/reopenable on Windows.
    //
    // Refused for a `target` that IS this store's own directory: the copy would
    // delete the environment it is reading.
    [[nodiscard]] bool copy_to(kimix::filesystem::path const &target) const noexcept;

private:
    // The single write path behind save_batch() and rebuild().
    [[nodiscard]] bool
    _write(kimix::vector<kimix::LMDBWriteCommand> &&commands, bool rebuild) const noexcept;
    // Drop every resolved path/handle (close(), and after a failed open).
    void _forget() noexcept;

    // The store directory, the environment directory inside it, and the handle.
    kimix::filesystem::path _path;
    kimix::filesystem::path _env_path;
    // An open kimix::LMDB; false while the store is closed.
    kimix::LMDB _db;
    // Consumed by the next _write() (see set_rebuild_next_batch()).
    mutable bool _rebuild_next{false};
};

} // namespace kimix::graph_db
