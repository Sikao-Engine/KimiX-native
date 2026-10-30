/*
 * key.h — the (label, external_id) primary key of a vertex (GDI §2.3).
 *
 * GDI's identity model is two-level: an internal, stable 16-byte uid (this
 * library's kimix::Guid) AND a first-class external key, (label, external id),
 * which is what a caller names a vertex by and what TranslateVertexId /
 * GetOrCreateVertex (MERGE) resolve. This header carries the key type, its
 * hashing and the KeyIndex that makes a key lookup O(1).
 *
 * ExternalId: ARBITRARY BINARY with 16-byte small-string optimization. The
 * overwhelmingly common keys (a GUID, a short code, a small integer) therefore
 * fit inline and a lookup allocates nothing at all. A longer id spills to one
 * heap vector and stays correct — the plan's documented cost, not a limit.
 *
 * THE KEY IS IMMUTABLE. A vertex's key fields are set when it is created and
 * never change afterwards: GDI has no "rename the external id" command, and a
 * change would have to re-index the vertex under a second key. `value()`,
 * `key()` and `external_id()` are therefore lock-free reads (like guid()).
 *
 * KeyIndex: SHARDED by hash(key) >> (64 - log2(kKeyShardCount)), one
 * kimix::shared_atomic_mutex + one ankerl dense map per shard. The lock is the
 * 4-byte atomic shared/exclusive SPIN lock (core/shared_atomic_mutex.h), not
 * std::shared_mutex, because it passes that header's admission rule: every
 * holder's section is one hash-map operation (insert/find/erase of an inline-key
 * probe), i.e. tens of nanoseconds, with no I/O, no callback and no unbounded
 * loop. A waiter therefore spins briefly instead of parking, which is what keeps
 * a 6 ns lookup at 6 ns under contention. The two LONG holders — reserve() and
 * clear() — run only in the loader's QUIESCENT phase (a single-threaded load /
 * restore, where no reader exists by contract), so they break the rule in name
 * only: a spin lock nobody waits on costs nothing. Never convert reserve()/clear()
 * into a per-request path without re-reading that rule.
 * It is LEVEL 4 of the database lock hierarchy (schema -> vertex shards -> edge
 * shards -> key shards -> leaf locks), so a structural writer takes it LAST, after
 * the vertex shards it is changing; a reader takes exactly one key shard and
 * releases it before touching any entity.
 *
 * SHARED RECURSION WARNING: the atomic type counts a second lock_shared() on the
 * same thread instead of refusing it (UB on std::shared_mutex), so a nested shared
 * acquisition of ONE shard would now "work" and could self-deadlock against a
 * writer only later. No path here takes two key shards, let alone one twice.
 *
 * Uniqueness: insert() refuses a duplicate with error_non_unique_id — that is
 * Capability::enforces_unique_external_ids. GraphOptions::key_mode =
 * report_warning turns the refusal into the GDI warning path (the existing
 * vertex's uid plus warning_non_unique_id), decided by the caller (Graph), not
 * here: the index only ever answers "was the key taken, and by whom".
 *
 * Example:
 *   kimix::graph_db::KeyIndex index;
 *   kimix::graph_db::VertexKey key;
 *   key.label = label_id;
 *   key.external_id = kimix::graph_db::ExternalId::from_string("user-42");
 *   kimix::Guid uid{true};
 *   index.insert(key, uid);                       // error_non_unique_id on a hit
 *   kimix::Guid found;
 *   bool present = index.find(kimix::graph_db::VertexKeyView{label_id, bytes}, found);
 */
#pragma once

#include "graph_db_api.h"
#include "property.h" // LabelId
#include "status.h"

#include <core/guid.h>
#include <core/memory.h>
#include <core/shared_atomic_mutex.h> // kimix::shared_atomic_mutex + its RAII guards
#include <core/stl/hash.h>
#include <core/stl/string.h>
#include <core/stl/unordered_map.h>
#include <core/stl/vector.h>

#include <array>
#include <atomic> // std::atomic<size_t> _size: MSVC pulls it in transitively, gcc does not
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace kimix::graph_db {

// Bytes an ExternalId keeps inline (a guid, a short code, a 128-bit hash).
inline constexpr size_t kExternalIdInlineSize = 16;

// Shards of the key index. Independent of kGraphShardCount on purpose (key.h is
// included by utils.h, which graph.h includes; graph.h static_asserts that the
// two are equal so the lock-order argument stays one story).
inline constexpr size_t kKeyShardCount = 64;
static_assert((kKeyShardCount & (kKeyShardCount - 1)) == 0,
              "kKeyShardCount must be a power of two (the shard index is a mask)");

/*
 * An arbitrary-size binary external id with 16-byte SSO. Value semantics: a copy
 * owns its bytes, so an id may outlive the buffer it was built from.
 */
class KIMIX_GRAPH_DB_API ExternalId {
public:
    ExternalId() noexcept = default;
    ExternalId(ExternalId const &) = default;
    ExternalId(ExternalId &&) noexcept = default;
    ExternalId &operator=(ExternalId const &) = default;
    ExternalId &operator=(ExternalId &&) noexcept = default;
    ~ExternalId() noexcept = default;

    static ExternalId from_bytes(kimix::span<const std::byte> bytes);
    // UTF-8 (or any) text, byte for byte — no transcoding, no NUL requirement.
    static ExternalId from_string(kimix::string_view text);
    // The 16 bytes of a guid (Guid::to_binary(), data0 then data1), which is the
    // shape a caller that keys by guid wants.
    static ExternalId from_guid(kimix::Guid const &guid) noexcept;

    [[nodiscard]] kimix::span<const std::byte> bytes() const noexcept;
    [[nodiscard]] size_t size() const noexcept { return _size; }
    [[nodiscard]] bool empty() const noexcept { return _size == 0; }
    // True while the payload lives in the inline buffer (i.e. size() <=
    // kExternalIdInlineSize): the documented "lookup allocates nothing" case.
    [[nodiscard]] bool is_inline() const noexcept { return _size <= kExternalIdInlineSize; }

    [[nodiscard]] bool equals(ExternalId const &other) const noexcept;
    [[nodiscard]] bool operator==(ExternalId const &other) const noexcept { return equals(other); }
    [[nodiscard]] bool operator!=(ExternalId const &other) const noexcept { return !equals(other); }

    [[nodiscard]] uint64_t hash() const noexcept;

    // Hex text of the bytes (empty for an empty id) — diagnostics only.
    [[nodiscard]] kimix::string to_hex() const;

private:
    void _assign(kimix::span<const std::byte> bytes);
    [[nodiscard]] std::byte const *_data() const noexcept;

    std::byte _inline_bytes[kExternalIdInlineSize]{};
    kimix::vector<std::byte> _spill; // only for size() > kExternalIdInlineSize
    uint32_t _size{0};
};

// Ownership form of a key (what an entity stores).
struct KIMIX_GRAPH_DB_API VertexKey {
    LabelId label{kNoLabel};
    ExternalId external_id;

    [[nodiscard]] bool valid() const noexcept { return label != kNoLabel; }
    [[nodiscard]] bool equals(VertexKey const &other) const noexcept {
        return label == other.label && external_id.equals(other.external_id);
    }
    [[nodiscard]] bool operator==(VertexKey const &other) const noexcept { return equals(other); }
    [[nodiscard]] bool operator!=(VertexKey const &other) const noexcept { return !equals(other); }
    [[nodiscard]] uint64_t hash() const noexcept;
};

// Borrowed form of a key (what a lookup uses — no allocation for the id).
struct KIMIX_GRAPH_DB_API VertexKeyView {
    LabelId label{kNoLabel};
    kimix::span<const std::byte> external_id;
};

// Interned key of a lookup, freed with the view.
[[nodiscard]] KIMIX_GRAPH_DB_API VertexKeyView key_view(VertexKey const &key) noexcept;

/*
 * Sharded (label, external_id) -> uid index. See the file header for the lock
 * level and the uniqueness contract. Every method takes exactly one shard.
 */
class KIMIX_GRAPH_DB_API KeyIndex : public kimix::IOperatorNewBase {
public:
    KeyIndex() noexcept = default;
    KeyIndex(KeyIndex const &) = delete;
    KeyIndex &operator=(KeyIndex const &) = delete;
    KeyIndex(KeyIndex &&) = delete;
    KeyIndex &operator=(KeyIndex &&) = delete;
    ~KeyIndex() noexcept = default;

    // Record `key` -> `uid`. error_non_unique_id when the key is taken; a key
    // with no label is error_argument.
    [[nodiscard]] GdiStatus insert(VertexKey const &key, kimix::Guid const &uid);
    // Resolve a key. False when absent (`uid` untouched).
    [[nodiscard]] bool find(VertexKeyView const &key, kimix::Guid &uid) const;
    [[nodiscard]] bool contains(VertexKeyView const &key) const;
    // Drop a key. Returns the uid that was stored, false when it was absent.
    bool erase(VertexKeyView const &key, kimix::Guid &removed_uid);
    bool erase(VertexKey const &key, kimix::Guid &removed_uid) {
        return erase(key_view(key), removed_uid);
    }
    // Raise the per-shard capacity before a bulk insert (a load or a bulk
    // create); only ever raises, so it cannot change a single entry.
    void reserve(size_t entries);
    [[nodiscard]] size_t size() const noexcept;
    // Shards whose index is non-empty — the load path's determinism check.
    [[nodiscard]] size_t used_shard_count() const noexcept;
    void clear();

private:
    struct Shard {
        Shard() noexcept = default;
        Shard(Shard const &) = delete;
        Shard &operator=(Shard const &) = delete;
        Shard(Shard &&) = delete;
        Shard &operator=(Shard &&) = delete;

        // alignas(64): the lock word is written by every holder, so it must not
        // share a cache line with the map's node array. mutable so the const
        // lookups (find/contains/used_shard_count) take it shared without a
        // const_cast; the lock itself is logically mutable by design.
        alignas(64) mutable kimix::shared_atomic_mutex mutex;
        kimix::unordered_map<VertexKey, kimix::Guid> index;
    };

    [[nodiscard]] static size_t _shard_of(uint64_t hash) noexcept;

    std::array<Shard, kKeyShardCount> _shards;
    // Entry count, maintained by the writers so size() is lock-free.
    std::atomic<size_t> _size{0};
};

} // namespace kimix::graph_db

// NOTE: no explicit kimix::hash<VertexKey> specialization is needed (or legal):
// core/stl/hash.h already provides hash<T> for any type with a .hash() method,
// and VertexKey has exactly that. Declaring one here would be a second, ambiguous
// definition of the same specialization.
