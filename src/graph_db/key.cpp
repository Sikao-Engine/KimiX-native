/*
 * key.cpp — ExternalId storage/hashing and the sharded KeyIndex (see key.h).
 *
 * ExternalId keeps up to 16 bytes inline, so the common key (a guid, a short
 * code) needs no allocation to build, compare or look up. A longer id spills to
 * one heap vector; both forms hash and compare through the same byte view, so
 * the representation is invisible to every caller.
 */
#include "key.h"

#include <core/stl/hash.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>

namespace kimix::graph_db {

namespace src_graph_db_key {

constexpr char kHexDigits[] = "0123456789abcdef";

// Mix two 64-bit halves into one hash (xxHash's avalanche constants).
inline uint64_t mix(uint64_t value) noexcept {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value;
}

} // namespace src_graph_db_key
using namespace src_graph_db_key;

// ---------------------------------------------------------------------------
// ExternalId
// ---------------------------------------------------------------------------

void ExternalId::_assign(kimix::span<const std::byte> bytes) {
    _size = static_cast<uint32_t>(bytes.size());
    if (bytes.size() <= kExternalIdInlineSize) {
        _spill.clear();
        std::memset(_inline_bytes, 0, sizeof(_inline_bytes));
        if (!bytes.empty()) {
            std::memcpy(_inline_bytes, bytes.data(), bytes.size());
        }
        return;
    }
    _spill.assign(bytes.begin(), bytes.end());
}

std::byte const *ExternalId::_data() const noexcept {
    return _size <= kExternalIdInlineSize ? _inline_bytes : _spill.data();
}

ExternalId ExternalId::from_bytes(kimix::span<const std::byte> bytes) {
    ExternalId id;
    id._assign(bytes);
    return id;
}

ExternalId ExternalId::from_string(kimix::string_view text) {
    return from_bytes(kimix::span<const std::byte>{
        reinterpret_cast<std::byte const *>(text.data()), text.size()});
}

ExternalId ExternalId::from_guid(kimix::Guid const &guid) noexcept {
    auto const &words = guid.to_binary();
    std::byte buffer[16];
    for (int i = 0; i < 8; ++i) {
        buffer[i] = static_cast<std::byte>((words.data0 >> (i * 8)) & 0xffull);
        buffer[8 + i] = static_cast<std::byte>((words.data1 >> (i * 8)) & 0xffull);
    }
    ExternalId id;
    id._assign(kimix::span<const std::byte>{buffer, sizeof(buffer)});
    return id;
}

kimix::span<const std::byte> ExternalId::bytes() const noexcept {
    if (_size == 0) {
        return {};
    }
    return kimix::span<const std::byte>{_data(), _size};
}

bool ExternalId::equals(ExternalId const &other) const noexcept {
    if (_size != other._size) {
        return false;
    }
    if (_size == 0) {
        return true;
    }
    return std::memcmp(_data(), other._data(), _size) == 0;
}

uint64_t ExternalId::hash() const noexcept {
    if (_size == 0) {
        return 0xcbf29ce484222325ull; // fnv-ish constant: an empty id is a value too
    }
    return kimix::hash64(reinterpret_cast<uint8_t const *>(_data()), _size);
}

kimix::string ExternalId::to_hex() const {
    auto const view = bytes();
    kimix::string out;
    out.reserve(view.size() * 2);
    for (auto byte : view) {
        uint8_t const value = std::to_integer<uint8_t>(byte);
        out.push_back(kHexDigits[value >> 4]);
        out.push_back(kHexDigits[value & 0x0f]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// VertexKey
// ---------------------------------------------------------------------------

uint64_t VertexKey::hash() const noexcept {
    uint64_t const id_hash = external_id.hash();
    // Combine the label id into the avalanche; the label is a small dense id, so
    // it must not dominate the low bits the shard index takes.
    return mix(static_cast<uint64_t>(label) * 0x9E3779B97F4A7C15ull + id_hash);
}

VertexKeyView key_view(VertexKey const &key) noexcept {
    VertexKeyView view;
    view.label = key.label;
    view.external_id = key.external_id.bytes();
    return view;
}

// ---------------------------------------------------------------------------
// KeyIndex
// ---------------------------------------------------------------------------

size_t KeyIndex::_shard_of(uint64_t hash) noexcept {
    if constexpr (kKeyShardCount == 1) {
        (void)hash;
        return 0;
    } else {
        constexpr unsigned shift = 64 - (kKeyShardCount == 2    ? 1u
                                         : kKeyShardCount == 4  ? 2u
                                         : kKeyShardCount == 8  ? 3u
                                         : kKeyShardCount == 16 ? 4u
                                         : kKeyShardCount == 32 ? 5u
                                                                : 6u);
        return static_cast<size_t>(hash >> shift);
    }
}

GdiStatus KeyIndex::insert(VertexKey const &key, kimix::Guid const &uid) {
    if (key.label == kNoLabel) {
        return GdiStatus::error_argument;
    }
    Shard &shard = _shards[_shard_of(key.hash())];
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    auto inserted = shard.index.try_emplace(key, uid);
    if (!inserted.second) {
        // A duplicate key is THE invariant this index exists for: GDI's
        // EnforcesUniqueExternalIds.
        return GdiStatus::error_non_unique_id;
    }
    _size.fetch_add(1, std::memory_order_relaxed);
    return GdiStatus::ok;
}

bool KeyIndex::find(VertexKeyView const &key, kimix::Guid &uid) const {
    if (key.label == kNoLabel) {
        return false;
    }
    // The probe is a VertexKey built from the borrowed view: ankerl's dense map
    // looks up by exact key type only, so a view has to be materialized. The id
    // is <= kExternalIdInlineSize for the common key, which means this copy never
    // touches the heap (see ExternalId).
    VertexKey probe;
    probe.label = key.label;
    probe.external_id = ExternalId::from_bytes(key.external_id);
    Shard const &shard = _shards[_shard_of(probe.hash())];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    auto it = shard.index.find(probe);
    if (it == shard.index.end()) {
        return false;
    }
    uid = it->second;
    return true;
}

bool KeyIndex::contains(VertexKeyView const &key) const {
    kimix::Guid ignored;
    return find(key, ignored);
}

bool KeyIndex::erase(VertexKeyView const &key, kimix::Guid &removed_uid) {
    if (key.label == kNoLabel) {
        return false;
    }
    VertexKey probe;
    probe.label = key.label;
    probe.external_id = ExternalId::from_bytes(key.external_id);
    Shard &shard = _shards[_shard_of(probe.hash())];
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    auto it = shard.index.find(probe);
    if (it == shard.index.end()) {
        return false;
    }
    removed_uid = it->second;
    shard.index.erase(it);
    _size.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

void KeyIndex::reserve(size_t entries) {
    size_t const per_shard = entries / kKeyShardCount + 8;
    for (auto &shard : _shards) {
        std::unique_lock<std::shared_mutex> lock(shard.mutex);
        shard.index.reserve(per_shard);
    }
}

size_t KeyIndex::size() const noexcept {
    return _size.load(std::memory_order_relaxed);
}

size_t KeyIndex::used_shard_count() const noexcept {
    size_t used = 0;
    for (auto const &shard : _shards) {
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        if (!shard.index.empty()) {
            ++used;
        }
    }
    return used;
}

void KeyIndex::clear() {
    for (auto &shard : _shards) {
        std::unique_lock<std::shared_mutex> lock(shard.mutex);
        shard.index.clear();
    }
    _size.store(0, std::memory_order_relaxed);
}

} // namespace kimix::graph_db
