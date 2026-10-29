/*
 * lmdb.h — kimix::LMDB, a thin RAII wrapper over the LMDB key-value store.
 *
 * kimix::LMDB opens (and creates) an LMDB environment backed by a directory
 * on disk and exposes a minimal typed key/value API over byte spans:
 *
 *   read(key)        — value span for `key`, empty span when not found.
 *   write(k, v)      — insert/replace a single key/value pair.
 *   write_all(cmds)  — apply a batch of LMDBWriteCommand in one transaction.
 *   replace_all(cmds) — empty the database and apply the batch in ONE
 *                      transaction (a full-snapshot write).
 *   remove(key)      — delete a single key.
 *   remove_all(keys) — delete a batch of keys in one transaction.
 *   copy_to(path)    — compact a snapshot of the database into `path`.
 *   begin()/end()    — iterate every stored pair (LMDBIterator).
 *
 * Keys and values are opaque byte spans (std::byte) or string_views; a
 * string_view overload is provided for convenience. The wrapper is move-only,
 * bool-convertible (true when the environment is open), and never throws:
 * LMDB-level failures are fatal (reported to stderr and abort), matching the
 * original LuisaCompute semantics. Filesystem operations use the
 * std::error_code overloads since this library is built without exceptions.
 *
 * Example:
 *   kimix::LMDB db("C:/cache/lmdb");
 *   if (db) {
 *       db.write("shader/abc", kimix::span{reinterpret_cast<const std::byte*>(blob.data()), blob.size()});
 *       auto v = db.read("shader/abc");   // empty span if missing
 *       for (auto it = db.begin(); it != kimix::LMDBIteratorEndTag{}; ++it) {
 *           auto [key, value] = *it;
 *       }
 *   }
 */
#pragma once

#include "dll_export.h"
#include "stl/optional.h"
#include "stl/string.h"
#include "stl/vector.h"
#include "stl/filesystem.h"

#include <cstdint>
#include <cstddef>
#include <memory>
#include <utility>

// The LMDB C types are kept opaque so this header does not pull in the
// vendored C API (mdb.h). The implementation includes it directly.
struct MDB_env;
struct MDB_txn;
struct MDB_cursor;
struct MDB_val;
struct MDB_stat;
struct MDB_envinfo;

namespace kimix {

/*
 * A single write command for `LMDB::write_all`:
 * a key and its associated value, both as byte vectors.
 */
struct LMDBWriteCommand {
    kimix::vector<std::byte> key;
    kimix::vector<std::byte> value;
};

/*
 * Sentinel tag comparing equal to a finished `LMDBIterator`; returned by `LMDB::end()`.
 */
struct LMDBIteratorEndTag {};

/*
 * Forward-only iterator over all key-value pairs in an `LMDB`.
 * Holds a read-only transaction and an open cursor until it reaches the end
 * (then both are released eagerly). Iterating yields a `Value` of spans into
 * the memory-mapped database; the spans are only valid until the iterator is
 * advanced or destroyed. Move-only.
 */
struct KIMIX_CORE_API LMDBIterator {
    friend class LMDB;

    struct Value {
        kimix::span<const std::byte> key;
        kimix::span<const std::byte> value;
    };

    Value const& operator*() const noexcept {
        return _value;
    }
    void operator++() noexcept;
    void operator++(int) noexcept {
        return operator++();
    }
    bool operator==(LMDBIteratorEndTag) const noexcept {
        return _finished;
    }

    LMDBIterator(LMDBIterator const&) = delete;
    LMDBIterator(LMDBIterator&& rhs) noexcept;
    LMDBIterator& operator=(LMDBIterator const&) = delete;
    LMDBIterator& operator=(LMDBIterator&& rhs) noexcept {
        std::destroy_at(this);
        std::construct_at(this, std::move(rhs));
        return *this;
    }
    ~LMDBIterator() noexcept;

private:
    MDB_txn* _txn{nullptr};
    MDB_cursor* _cursor{nullptr};
    Value _value{};
    bool _finished{false};
    LMDBIterator(MDB_env* env, uint32_t dbi) noexcept;
};

/*
 * LMDB database environment, opened on construction and closed on
 * destruction (and on move). A default-constructed or moved-from instance
 * converts to false and must not have its methods called.
 */
class KIMIX_CORE_API LMDB {
    kimix::string _path;
    size_t _map_size{};
    MDB_env* _env{nullptr};
    kimix::optional<uint32_t> _dbi{};
    uint32_t _flag{};
    void _dispose() noexcept;

public:
    LMDB();
    LMDB(
        kimix::filesystem::path const& db_dir,
        size_t max_reader = 126ull,
        // 64G as default (should be enough for shader?)
        size_t map_size = 1024ull * 1024ull * 1024ull * 64ull) noexcept;
    LMDB(LMDB const&) = delete;
    LMDB& operator=(LMDB const&) = delete;
    LMDB(LMDB&& rhs) noexcept;
    LMDB& operator=(LMDB&& rhs) noexcept {
        std::destroy_at(this);
        std::construct_at(this, std::move(rhs));
        return *this;
    }

    [[nodiscard]] operator bool() const {
        return _env != nullptr;
    }
    [[nodiscard]] bool operator==(std::nullptr_t) const {
        return _env == nullptr;
    }

    [[nodiscard]] kimix::span<const std::byte> read(kimix::span<const std::byte> key) const noexcept;
    [[nodiscard]] kimix::span<const std::byte> read(kimix::string_view key) const noexcept {
        // The explicit template argument is required: CTAD through the alias
        // template kimix::span breaks on GCC 13 (deduction lands on the
        // array guide with extent npos -> hard error), so `kimix::span{...}`
        // does not compile there even though MSVC and newer GCC accept it.
        return read(kimix::span<const std::byte>{reinterpret_cast<std::byte const*>(key.data()), key.size()});
    }

    void write(kimix::span<const std::byte> key, kimix::span<const std::byte> value) const noexcept;
    void write(kimix::string_view key, kimix::span<const std::byte> value) const noexcept {
        write(kimix::span<const std::byte>{reinterpret_cast<std::byte const*>(key.data()), key.size()}, value);
    }

    void write_all(kimix::vector<LMDBWriteCommand>&& commands) const noexcept;
    // Empty the database and apply `commands` in ONE transaction, so a reader sees
    // either the previous content or the whole new snapshot — never a half-written
    // store. That is the "full save" shape of a key/value store (write_all() is the
    // incremental one: it touches only the keys it carries).
    void replace_all(kimix::vector<LMDBWriteCommand>&& commands) const noexcept;
    void remove(kimix::span<const std::byte> key) const noexcept;
    void remove_all(kimix::vector<kimix::vector<std::byte>>&& keys) const noexcept;
    void copy_to(kimix::filesystem::path const& path) const noexcept;

    ~LMDB() noexcept;

    [[nodiscard]] LMDBIterator begin() const noexcept;
    [[nodiscard]] LMDBIteratorEndTag end() const noexcept { return {}; }
};

} // namespace kimix
