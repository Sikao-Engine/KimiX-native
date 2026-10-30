/*
 * lmdb.cpp — kimix::LMDB / kimix::LMDBIterator implementation over the vendored
 * LMDB C API (src/core/lmdb/lmdb.h).
 *
 * LMDB-level failures are fatal: the original LuisaCompute wrapper aborted on
 * them (LUISA_ERROR_WITH_LOCATION throws/terminates). This library is built
 * without C++ exceptions, so KIMIX_CHECK_LMDB_ERROR reports to stderr in the
 * project's "[kimix][error] ... (%s:%d)" style and aborts, preserving that
 * semantics. Filesystem operations use the std::error_code overloads and abort
 * on error for the same reason (the original let std::filesystem throw).
 */
#include <core/lmdb.h>
#include <core/lmdb/lmdb.h>

#include <cstdio>
#include <cstdlib>
#include <system_error>

namespace kimix {

#define KIMIX_CHECK_LMDB_ERROR(call)                            \
    do {                                                        \
        if (auto rc = (call); rc != MDB_SUCCESS) [[unlikely]] { \
            std::fprintf(stderr,                                \
                         "[kimix][error] LMDB call failed '%s': %s (%s:%d)\n", \
                         #call, mdb_strerror(rc), __FILE__, __LINE__);         \
            std::abort();                                       \
        }                                                       \
    } while (false)

// Fatal-on-error filesystem handling (the helpers below): std::filesystem
// operations report through std::error_code (exceptions are disabled) and
// abort the process when the operation fails, mirroring the original's intent
// of letting the throwing std::filesystem overloads terminate.

LMDB::LMDB(
    kimix::filesystem::path const& db_dir,
    size_t max_reader,
    size_t map_size) noexcept
    : _path(kimix::to_string(db_dir)),
      _map_size(map_size) {
    KIMIX_CHECK_LMDB_ERROR(mdb_env_create(&_env));
    KIMIX_CHECK_LMDB_ERROR(mdb_env_set_maxreaders(_env, max_reader));
    KIMIX_CHECK_LMDB_ERROR(mdb_env_set_mapsize(_env, _map_size));
    std::error_code ec;
    if (!kimix::filesystem::exists(db_dir, ec)) {
        if (ec) [[unlikely]] {
            std::fprintf(stderr,
                         "[kimix][error] Failed to query database directory '%s': %s (%s:%d)\n",
                         kimix::to_string(db_dir).c_str(), ec.message().c_str(),
                         __FILE__, __LINE__);
            std::abort();
        }
        kimix::filesystem::create_directories(db_dir, ec);
        if (ec) [[unlikely]] {
            std::fprintf(stderr,
                         "[kimix][error] Failed to create database directory '%s': %s (%s:%d)\n",
                         kimix::to_string(db_dir).c_str(), ec.message().c_str(),
                         __FILE__, __LINE__);
            std::abort();
        }
    }
    KIMIX_CHECK_LMDB_ERROR(mdb_env_open(_env, _path.c_str(), MDB_NORDAHEAD, 0664));
    MDB_txn* txn;
    KIMIX_CHECK_LMDB_ERROR(mdb_txn_begin(_env, nullptr, MDB_RDONLY, &txn));
    _dbi = 0;
    KIMIX_CHECK_LMDB_ERROR(mdb_dbi_open(txn, nullptr, 0, &*_dbi));
    mdb_txn_abort(txn);
}

kimix::span<const std::byte> LMDB::read(kimix::span<const std::byte> key) const noexcept {
    MDB_txn* txn;
    KIMIX_CHECK_LMDB_ERROR(mdb_txn_begin(_env, nullptr, MDB_RDONLY, &txn));
    MDB_val key_v{key.size_bytes(), const_cast<std::byte*>(key.data())};
    MDB_val value_v;
    auto r = mdb_get(txn, *_dbi, &key_v, &value_v);
    mdb_txn_abort(txn);
    // ONLY a successful get fills value_v. Every other rc — MDB_NOTFOUND, and also a
    // REFUSED get such as MDB_BAD_VALSIZE for an empty key — must report "absent":
    // returning {} + value_v.mv_size would hand out a span over uninitialized memory.
    if (r != MDB_SUCCESS) {
        return {};
    }
    return {static_cast<std::byte const*>(value_v.mv_data), value_v.mv_size};
}

bool LMDB::read_into(kimix::span<const std::byte> key,
                     kimix::vector<std::byte>& out) const noexcept {
    out.clear();
    if (_env == nullptr || !_dbi) {
        return false;
    }
    MDB_txn* txn;
    KIMIX_CHECK_LMDB_ERROR(mdb_txn_begin(_env, nullptr, MDB_RDONLY, &txn));
    MDB_val key_v{key.size_bytes(), const_cast<std::byte*>(key.data())};
    MDB_val value_v{};
    auto const rc = mdb_get(txn, *_dbi, &key_v, &value_v);
    if (rc != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        return false;
    }
    // Copy BEFORE the abort: the pages belong to the mapping and this read
    // transaction is the only thing keeping them from being reused by the (single)
    // writer, so a copy taken afterwards would be a torn read.
    out.assign(static_cast<std::byte const*>(value_v.mv_data),
               static_cast<std::byte const*>(value_v.mv_data) + value_v.mv_size);
    mdb_txn_abort(txn);
    return true;
}

void LMDB::write(kimix::span<const std::byte> key, kimix::span<const std::byte> value) const noexcept {
    MDB_txn* txn;
    KIMIX_CHECK_LMDB_ERROR(mdb_txn_begin(_env, nullptr, 0, &txn));
    MDB_val key_v{key.size_bytes(), const_cast<std::byte*>(key.data())};
    MDB_val value_v{value.size_bytes(), const_cast<std::byte*>(value.data())};
    // Checked like every other call: a refused put does not fail the commit, so
    // ignoring it would silently drop the write (see put_all below).
    KIMIX_CHECK_LMDB_ERROR(mdb_put(txn, *_dbi, &key_v, &value_v, 0));
    KIMIX_CHECK_LMDB_ERROR(mdb_txn_commit(txn));
}

namespace src_core_lmdb {

// Apply one batch inside an already-open write transaction. A refused put is fatal,
// exactly like every other LMDB failure in this file: a put that failed does NOT make
// the commit below fail, so ignoring it would report "written" for a batch LMDB stored
// only in part (the caller has already cleared its dirty flags by then, i.e. the
// records would be lost silently). Callers that forward untrusted keys validate them
// first (see the graph_db file backend), so this can only fire on a real fault —
// MDB_MAP_FULL, a key past ENV_MAXKEY, a bad value size.
void put_all(MDB_txn* txn, uint32_t dbi,
             kimix::vector<LMDBWriteCommand> const& commands) noexcept {
    for (auto& i : commands) {
        MDB_val key_v{kimix::size_bytes(i.key), const_cast<std::byte*>(i.key.data())};
        MDB_val value_v{kimix::size_bytes(i.value), const_cast<std::byte*>(i.value.data())};
        KIMIX_CHECK_LMDB_ERROR(mdb_put(txn, dbi, &key_v, &value_v, 0));
    }
}

} // namespace src_core_lmdb
using namespace src_core_lmdb;

void LMDB::write_all(kimix::vector<LMDBWriteCommand>&& commands) const noexcept {
    MDB_txn* txn;
    KIMIX_CHECK_LMDB_ERROR(mdb_txn_begin(_env, nullptr, 0, &txn));
    src_core_lmdb::put_all(txn, *_dbi, commands);
    KIMIX_CHECK_LMDB_ERROR(mdb_txn_commit(txn));
}

void LMDB::replace_all(kimix::vector<LMDBWriteCommand>&& commands) const noexcept {
    MDB_txn* txn;
    KIMIX_CHECK_LMDB_ERROR(mdb_txn_begin(_env, nullptr, 0, &txn));
    // del = 0: the (unnamed) DB stays open, only its content is dropped, so the
    // writes below land in the SAME transaction as the truncation.
    KIMIX_CHECK_LMDB_ERROR(mdb_drop(txn, *_dbi, 0));
    src_core_lmdb::put_all(txn, *_dbi, commands);
    KIMIX_CHECK_LMDB_ERROR(mdb_txn_commit(txn));
}

LMDB::LMDB(LMDB&& rhs) noexcept
    : _path(std::move(rhs._path)),
      _map_size(rhs._map_size),
      _env(rhs._env),
      _dbi(std::move(rhs._dbi)),
      _flag(rhs._flag),
      _live(std::move(rhs._live)) {
    rhs._env = nullptr;
}

LMDB::LMDB() = default;

void LMDB::_dispose() noexcept {
    // FIRST: tell every iterator still alive that this environment is going away, so
    // a later ++ / ~LMDBIterator only drops its handles instead of calling into a
    // closed MDB_env (lmdb.cpp defect D2).
    if (_live) {
        _live->open.store(false, std::memory_order_release);
    }
    if (_env) {
        if (_dbi) {
            mdb_dbi_close(_env, *_dbi);
            _dbi.reset();
        }
        mdb_env_close(_env);
        _env = nullptr;
    }
}

void LMDB::copy_to(kimix::filesystem::path const& path) const noexcept {
    std::error_code ec;
    if (!kimix::filesystem::exists(path, ec)) {
        if (ec) [[unlikely]] {
            std::fprintf(stderr,
                         "[kimix][error] Failed to query copy destination '%s': %s (%s:%d)\n",
                         kimix::to_string(path).c_str(), ec.message().c_str(),
                         __FILE__, __LINE__);
            std::abort();
        }
        kimix::filesystem::create_directories(path, ec);
        if (ec) [[unlikely]] {
            std::fprintf(stderr,
                         "[kimix][error] Failed to create copy destination '%s': %s (%s:%d)\n",
                         kimix::to_string(path).c_str(), ec.message().c_str(),
                         __FILE__, __LINE__);
            std::abort();
        }
    } else {
        kimix::filesystem::remove_all(path, ec);
        if (ec) [[unlikely]] {
            std::fprintf(stderr,
                         "[kimix][error] Failed to clear copy destination '%s': %s (%s:%d)\n",
                         kimix::to_string(path).c_str(), ec.message().c_str(),
                         __FILE__, __LINE__);
            std::abort();
        }
        kimix::filesystem::create_directories(path, ec);
        if (ec) [[unlikely]] {
            std::fprintf(stderr,
                         "[kimix][error] Failed to create copy destination '%s': %s (%s:%d)\n",
                         kimix::to_string(path).c_str(), ec.message().c_str(),
                         __FILE__, __LINE__);
            std::abort();
        }
    }
    KIMIX_CHECK_LMDB_ERROR(mdb_env_copy2(_env, kimix::to_string(path).c_str(), MDB_CP_COMPACT));
}

void LMDB::remove(kimix::span<const std::byte> key) const noexcept {
    MDB_txn* txn;
    KIMIX_CHECK_LMDB_ERROR(mdb_txn_begin(_env, nullptr, 0, &txn));
    MDB_val key_v{key.size_bytes(), const_cast<std::byte*>(key.data())};
    mdb_del(txn, *_dbi, &key_v, nullptr);
    KIMIX_CHECK_LMDB_ERROR(mdb_txn_commit(txn));
}

void LMDB::remove_all(kimix::vector<kimix::vector<std::byte>>&& keys) const noexcept {
    MDB_txn* txn;
    KIMIX_CHECK_LMDB_ERROR(mdb_txn_begin(_env, nullptr, 0, &txn));
    for (auto& i : keys) {
        MDB_val key_v{kimix::size_bytes(i), const_cast<std::byte*>(i.data())};
        mdb_del(txn, *_dbi, &key_v, nullptr);
    }
    KIMIX_CHECK_LMDB_ERROR(mdb_txn_commit(txn));
}

LMDB::~LMDB() noexcept {
    _dispose();
}

LMDBIterator LMDB::begin() const noexcept {
    // A closed environment (or a default-constructed one) has no cursor to open: hand
    // back a FINISHED iterator instead of dereferencing an empty _dbi.
    if (_env == nullptr || !_dbi) {
        return LMDBIterator{};
    }
    return LMDBIterator{_env, *_dbi, _live};
}

LMDBIterator::LMDBIterator(LMDBIterator&& rhs) noexcept
    : _txn(rhs._txn),
      _cursor(rhs._cursor),
      _value(rhs._value),
      _finished(rhs._finished),
      _live(std::move(rhs._live)) {
    rhs._txn = nullptr;
    rhs._cursor = nullptr;
    rhs._value = {};
    rhs._finished = true;
}

void LMDBIterator::_release() noexcept {
    if (_cursor != nullptr) {
        mdb_cursor_close(_cursor);
        _cursor = nullptr;
    }
    if (_txn != nullptr) {
        mdb_txn_abort(_txn);
        _txn = nullptr;
    }
    _value = {};
    _finished = true;
}

void LMDBIterator::_forget() noexcept {
    // The environment is gone: these pointers refer to freed memory, so NOTHING may
    // call into LMDB on them.
    _cursor = nullptr;
    _txn = nullptr;
    _value = {};
    _finished = true;
}

LMDBIterator::~LMDBIterator() noexcept {
    if (_txn == nullptr && _cursor == nullptr) {
        return;
    }
    if (_live && !_live->open.load(std::memory_order_acquire)) {
        _forget();
        return;
    }
    _release();
}

LMDBIterator::LMDBIterator(MDB_env* env, uint32_t dbi, std::shared_ptr<LMDBLiveness> live) noexcept
    : _live(std::move(live)) {
    KIMIX_CHECK_LMDB_ERROR(mdb_txn_begin(env, nullptr, MDB_RDONLY, &_txn));
    KIMIX_CHECK_LMDB_ERROR(mdb_cursor_open(_txn, dbi, &_cursor));
    operator++();
}

void LMDBIterator::operator++() noexcept {
    // Incrementing a finished iterator is a NO-OP. Before this guard a hand-written
    // loop (or a std::move'd iterator) called mdb_cursor_get(nullptr, ...) and
    // dereferenced a null cursor.
    if (_finished) {
        return;
    }
    if (_txn == nullptr || _cursor == nullptr ||
        (_live && !_live->open.load(std::memory_order_acquire))) {
        // The environment was destroyed under this iterator: degrade to finished.
        _forget();
        return;
    }
    MDB_val key{};
    MDB_val data{};
    auto const rc = mdb_cursor_get(_cursor, &key, &data, MDB_NEXT);
    if (rc != 0) {
        _release();
        return;
    }
    _value.key = {
        reinterpret_cast<std::byte const*>(key.mv_data),
        key.mv_size};
    _value.value = {
        reinterpret_cast<std::byte const*>(data.mv_data),
        data.mv_size};
}

#undef KIMIX_CHECK_LMDB_ERROR

} // namespace kimix
