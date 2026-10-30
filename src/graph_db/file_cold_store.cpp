/*
 * file_cold_store.cpp — the LMDB-backed cold-store backend (see
 * file_cold_store.h for the layout, the contract and the limits).
 *
 * STRUCTURE
 *   open()             path validation, then ONE kimix::LMDB environment opened
 *                      (and kept open) at <dir>\graph.xdb. Nothing is lazy any
 *                      more: an LMDB lookup or cursor pass costs one transaction,
 *                      so there is no scan to avoid and no cache to keep.
 *   read_meta()        the ONE meta lookup ("meta/root").
 *   load_all_records() the ONE cursor pass.
 *   _write()           the ONE write path: incremental = one transaction putting
 *                      the batch; full = one transaction that empties the database
 *                      and puts the batch (LMDB's mdb_drop + puts).
 *   copy_to()          LMDB's compacting environment copy.
 *
 * The store path is validated BEFORE kimix::LMDB is handed it: that wrapper is
 * fatal on LMDB errors (a failing mdb_* call reports to stderr and aborts, see
 * <core/lmdb.h>), so "not a store this build wrote" has to be decided here or
 * open() could not report false at all. Every call below is non-throwing and
 * reports through bool; `_write()` and `_rebuild_next` are `mutable` because the
 * seam's operations are `const`, exactly like ColdStorage::save_batch(), which
 * mutates its LMDB environment through a const method.
 */
#include "file_cold_store.h"

#include "binary_le.h"

#include <core/binary_file_stream.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <cstddef>
#include <cstdint>
#include <system_error>
#include <utility>

namespace kimix::graph_db {

namespace src_graph_db_file_cold_store {

// LMDB's on-disk identity, checked before the environment is opened: page 0 of a
// data.mdb is a meta page whose MDB_page_header (pgno_t + txnid_t + mh_pad + mh_flags
// + mh_lower/mh_upper = sizeof(uintptr_t) + 16 bytes: LMDB 1.0 stores the writing
// txnid in every page header, 0.9 did not) is followed by
// MDB_meta.mm_magic == MDB_MAGIC and .mm_version == MDB_DATA_VERSION. The vendored
// LMDB writes exactly this version, so anything else is not a store this build can
// open — and letting LMDB discover that by itself would abort the process. A store
// written by a 0.9-format build (mm_version 1) is therefore refused as a plain false,
// which is the migration path out of those files: dump them with a 0.9 build.
constexpr uint32_t kLmdbMagic{0xBEEFC0DEu};
constexpr uint32_t kLmdbDataVersion{3u};
constexpr size_t kMetaPageHeaderSize = sizeof(uintptr_t) + 16;

// True when `data` (an environment's data.mdb) carries LMDB's meta page. A missing,
// too short or foreign file makes open() report false instead of aborting.
[[nodiscard]] bool data_file_is_a_store(kimix::filesystem::path const &data) noexcept {
    kimix::BinaryFileStream stream{kimix::to_string(data)};
    if (!stream) {
        return false;
    }
    std::byte head[kMetaPageHeaderSize + 8]{};
    if (stream.read(head, sizeof head) != sizeof head) {
        return false;
    }
    return detail::read_u32(head + kMetaPageHeaderSize) == kLmdbMagic &&
           detail::read_u32(head + kMetaPageHeaderSize + 4) == kLmdbDataVersion;
}

// The path is only handed to kimix::LMDB when it really is (or can become) an
// environment: absent, or a DIRECTORY whose data.mdb is an LMDB store. A regular
// file at graph.xdb, a directory that is not an environment and a corrupt data.mdb
// are all refused here rather than inside LMDB (which would abort).
[[nodiscard]] bool store_path_is_openable(kimix::filesystem::path const &env) noexcept {
    std::error_code ec;
    bool const exists = kimix::filesystem::exists(env, ec);
    if (ec) {
        return false;
    }
    if (!exists) {
        return true; // the environment is created by the open below
    }
    if (!kimix::filesystem::is_directory(env, ec) || ec) {
        return false;
    }
    kimix::filesystem::path const data =
        env / kimix::filesystem::path{FileColdStore::kDataFileName};
    bool const has_data = kimix::filesystem::exists(data, ec);
    if (ec) {
        return false;
    }
    return !has_data || data_file_is_a_store(data);
}

// Every key of a batch must be inside LMDB's own limits before any of them reaches
// it: an empty key and one longer than MDB_MAXKEYSIZE are both refused by mdb_put,
// i.e. an abort through kimix::LMDB, so the whole batch is refused here instead.
[[nodiscard]] bool keys_are_storable(
    kimix::vector<kimix::LMDBWriteCommand> const &commands) noexcept {
    for (auto const &command : commands) {
        if (command.key.empty() || command.key.size() > FileColdStore::kMaxKeyLength) {
            return false; // not a store key: refuse the whole batch, write nothing
        }
    }
    return true;
}

} // namespace src_graph_db_file_cold_store
using namespace src_graph_db_file_cold_store;

// ---------------------------------------------------------------------------
// Construction / store lifecycle
// ---------------------------------------------------------------------------

FileColdStore::FileColdStore(kimix::filesystem::path const &dir,
                             size_t map_size) noexcept {
    (void)open(dir, map_size);
}

bool FileColdStore::open(kimix::filesystem::path const &dir, size_t map_size) noexcept {
    close();
    if (dir.empty()) {
        return false;
    }
    if (!ensure_dir(dir)) {
        return false;
    }
    kimix::filesystem::path const env = dir / kimix::filesystem::path{kColdFileName};
    if (!store_path_is_openable(env)) {
        return false;
    }
    // ONE environment, held open until close(): every lookup, cursor pass and save
    // below is a transaction on it, and LMDB's own reader table is what keeps
    // readers safe against the single writer.
    size_t const map = map_size != 0 ? map_size : default_map_size;
    _db = kimix::LMDB{env, 126ull, map};
    if (!_db) {
        _forget();
        return false;
    }
    _path = dir;
    _env_path = env;
    return true;
}

void FileColdStore::close() noexcept {
    // Closing the environment releases data.mdb/lock.mdb, which is what makes the
    // store directory deletable on Windows (an open handle is not).
    _db = kimix::LMDB{};
    _forget();
}

void FileColdStore::_forget() noexcept {
    _path.clear();
    _env_path.clear();
    _rebuild_next = false;
}

bool FileColdStore::valid() const noexcept {
    return static_cast<bool>(_db);
}

// ---------------------------------------------------------------------------
// Read side
// ---------------------------------------------------------------------------

bool FileColdStore::read_meta(MetaRecord &out) const noexcept {
    if (!_db) {
        return false;
    }
    auto const value = _db.read(kMetaRootKey);
    if (value.empty()) {
        return false; // absent, or a tombstone: the store has no meta record
    }
    // Decoded straight out of the memory-mapped region: the decode is synchronous and
    // this class is single-writer/single-threaded by contract, so no save can remap the
    // block between the lookup and the read. (It used to copy the value into a heap
    // vector first, which cost one allocation per call — and save_incremental() asks
    // for the meta record twice — without buying anything: the copy itself reads the
    // same map.)
    return decode_meta_record(value, out);
}

bool FileColdStore::load_all_records(kimix::vector<ColdRecord> &out) const noexcept {
    out.clear();
    if (!_db) {
        return false;
    }
    // ONE read transaction and one cursor for the whole pass; the iterator closes
    // both eagerly at the end. Keys come back in LMDB order (the map's byte order),
    // values are copied out because the spans die on the next ++.
    for (auto it = _db.begin(); it != _db.end(); ++it) {
        auto const &entry = *it;
        ColdRecord record;
        record.key.assign(entry.key.begin(), entry.key.end());
        record.value.assign(entry.value.begin(), entry.value.end());
        out.push_back(std::move(record));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Write side
// ---------------------------------------------------------------------------

bool FileColdStore::_write(kimix::vector<kimix::LMDBWriteCommand> &&commands,
                           bool rebuild_requested) const noexcept {
    if (!_db) {
        return false;
    }
    if (commands.empty()) {
        // Nothing to write is not a failure for an INCREMENTAL batch (it touches no
        // key). A REBUILD, on the other hand, is "this batch IS the whole store": an
        // empty one cannot be distinguished from a caller bug, and emptying the
        // environment for it would destroy a store the caller never meant to clear —
        // so it is refused, with the store left untouched (file_cold_store.h).
        return !rebuild_requested;
    }
    if (!keys_are_storable(commands)) {
        return false;
    }
    // ONE transaction per save batch. The incremental path only puts the batch's
    // keys (a record of an earlier round is replaced in place, so nothing is
    // appended and no superseded copy survives); the full path empties the database
    // in the same transaction, so a reader sees the old store or the new snapshot.
    if (rebuild_requested) {
        _db.replace_all(std::move(commands));
    } else {
        _db.write_all(std::move(commands));
    }
    return true;
}

bool FileColdStore::save_batch(kimix::vector<kimix::LMDBWriteCommand> &&commands) const noexcept {
    // The seam's batch carries no "full save" flag (serialize.cpp writes both kinds
    // through this one call), so Database::persist(full=true) arms the rebuild
    // explicitly. The arm is consumed here whether or not the write succeeds — but a
    // save that writes nothing never gets here at all, so it leaves the arm pending
    // (hence cancel_pending_rebuild() and the serializer's call for every incremental
    // save).
    bool const rebuild_requested = _rebuild_next;
    _rebuild_next = false;
    return _write(std::move(commands), rebuild_requested);
}

bool FileColdStore::rebuild(kimix::vector<kimix::LMDBWriteCommand> &&commands) const noexcept {
    return _write(std::move(commands), true);
}

// ---------------------------------------------------------------------------
// Checkpoint
// ---------------------------------------------------------------------------

bool FileColdStore::copy_to(kimix::filesystem::path const &target) const noexcept {
    if (!_db || target.empty()) {
        return false;
    }
    // A checkpoint ONTO the live store would delete the environment it copies from
    // (LMDB::copy_to clears the destination first), so the two paths are compared the
    // way the filesystem does, not byte for byte: `dir/./graph.xdb` and
    // `dir/../dir/graph.xdb` name the same environment as `_env_path` and a textual
    // comparison would wave them through.
    kimix::filesystem::path const destination =
        target / kimix::filesystem::path{kColdFileName};
    std::error_code ec;
    bool same = false;
    if (kimix::filesystem::exists(destination, ec) && !ec) {
        same = kimix::filesystem::equivalent(destination, _env_path, ec) && !ec;
    }
    if (same) {
        return false;
    }
    if (!ensure_dir(target)) {
        return false;
    }
    // LMDB's own compacting copy writes <target>\graph.xdb\data.mdb and leaves no
    // handle open, so the checkpoint is immediately reopenable/deletable (A7).
    // A previous checkpoint in `target` is replaced.
    _db.copy_to(destination);
    return true;
}

} // namespace kimix::graph_db
