/*
 * edge.cpp — the sharded edge table and the edge-record codec (see edge.h).
 *
 * Every public method here takes exactly ONE shard lock and copies what it
 * returns, so the table can never be part of a multi-lock cycle on its own; the
 * paths that need several locks (vertex adjacency + edge record) live on Graph,
 * which owns the vertex shards as well.
 */
#include "edge.h"

#include "utils.h" // kEdgeRecordMagic / kRecordMagicSize (the key+magic vocabulary)

#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>

namespace kimix::graph_db {

namespace src_graph_db_edge {

using namespace detail;

// Header of an encoded edge record: magic + id + origin + target + direction +
// reserved + label count + property count.
constexpr size_t kEdgeRecordHeaderSize = 4 + 16 + 16 + 16 + 1 + 3 + 4 + 4;

// Label table + property entries, the part after the header.
[[nodiscard]] size_t payload_size(EdgeRecord const &record) noexcept {
    size_t size = record.labels.size() * 4;
    for (auto const &entry : record.properties) {
        size += encoded_property_entry_size(entry);
    }
    return size;
}

} // namespace src_graph_db_edge
using namespace src_graph_db_edge;

char const *edge_direction_name(EdgeDirectionType direction) noexcept {
    switch (direction) {
    case EdgeDirectionType::directed: return "directed";
    case EdgeDirectionType::undirected: return "undirected";
    }
    return "direction_unknown";
}

char const *edge_orientation_name(EdgeOrientation orientation) noexcept {
    switch (orientation) {
    case EdgeOrientation::outgoing: return "outgoing";
    case EdgeOrientation::incoming: return "incoming";
    case EdgeOrientation::both: return "both";
    }
    return "orientation_unknown";
}

// ---------------------------------------------------------------------------
// Edge dirty-flag protocol (the ONE place the flag is written — see edge.h)
// ---------------------------------------------------------------------------

void Edge::_mark_dirty() noexcept {
    if (_dirty_sink != nullptr) {
        // The owner sets the flag AND inserts the id into its dirty index under
        // one mutex.
        _dirty_sink->note_edge_dirty(this);
        return;
    }
    _dirty = true;
}

void Edge::_clear_dirty() noexcept {
    if (_dirty_sink != nullptr) {
        _dirty_sink->clear_edge_dirty(this);
        return;
    }
    _dirty = false;
}

// ---------------------------------------------------------------------------
// EdgeTable
// ---------------------------------------------------------------------------

EdgeTable::~EdgeTable() noexcept = default;

size_t EdgeTable::shard_of(EdgeId const &id) noexcept {
    if constexpr (kEdgeShardCount == 1) {
        (void)id;
        return 0;
    } else {
        auto const &words = id.to_binary();
        // Same Fibonacci-hash spread as Graph::_shard_of: Windows guids come from
        // ::CoCreateGuid and their top bits are constant, so the multiply is what
        // keeps the shards balanced (a poor spread costs contention only).
        uint64_t const mixed = (words.data0 ^ words.data1) * 0x9E3779B97F4A7C15ull;
        constexpr unsigned shift = 64 - (kEdgeShardCount == 2    ? 1u
                                         : kEdgeShardCount == 4  ? 2u
                                         : kEdgeShardCount == 8  ? 3u
                                         : kEdgeShardCount == 16 ? 4u
                                         : kEdgeShardCount == 32 ? 5u
                                                                 : 6u);
        return static_cast<size_t>(mixed >> shift);
    }
}

Edge *EdgeTable::_find_locked(size_t shard, EdgeId const &id) const noexcept {
    auto it = _shards[shard].edges.find(id);
    if (it == _shards[shard].edges.end()) {
        return nullptr;
    }
    return it->second.get();
}

void EdgeTable::ids_of_shard(size_t shard, kimix::vector<EdgeId> &out) const {
    out.reserve(out.size() + _shards[shard].edges.size());
    for (auto const &entry : _shards[shard].edges) {
        out.push_back(entry.first);
    }
}

bool EdgeTable::insert(kimix::unique_ptr<Edge> edge) {
    if (!edge) {
        return false;
    }
    EdgeId const id = edge->id();
    size_t const shard = shard_of(id);
    std::unique_lock<std::shared_mutex> lock(_shards[shard].mutex);
    auto inserted = _shards[shard].edges.try_emplace(id, std::move(edge));
    if (!inserted.second) {
        return false;
    }
    _size.fetch_add(1, std::memory_order_relaxed);
    // Attach the dirty sink while the shard is still UNIQUE-locked: no reader can
    // resolve the edge before it has a sink, so nothing can flip its flag behind
    // the index's back (a fresh Edge starts dirty, hence the registration).
    _attach_dirty(inserted.first->second.get());
    return true;
}

kimix::unique_ptr<Edge> EdgeTable::erase(EdgeId const &id) {
    size_t const shard = shard_of(id);
    std::unique_lock<std::shared_mutex> lock(_shards[shard].mutex);
    auto it = _shards[shard].edges.find(id);
    if (it == _shards[shard].edges.end()) {
        return nullptr;
    }
    kimix::unique_ptr<Edge> removed = std::move(it->second);
    _shards[shard].edges.erase(it);
    _size.fetch_sub(1, std::memory_order_relaxed);
    // The edge belongs to nobody now: drop its index entry and clear its flag
    // while the shard is still locked, so a caller that marks the returned edge
    // dirty afterwards only writes that edge's own flag.
    _detach_dirty(removed.get());
    return removed;
}

// ---------------------------------------------------------------------------
// The dirty index (the EdgeDirtySink half of the flag protocol)
// ---------------------------------------------------------------------------
// All four functions take ONLY _dirty_edges_mutex and touch nothing but the hash
// container and the edge's own flag, which is what makes that mutex the innermost
// lock: it is taken under an edge shard lock (insert/erase/clear_dirty) or under
// a vertex shard lock (the Graph paths that dirtied an endpoint) and never holds
// anything itself.

void EdgeTable::note_edge_dirty(Edge *edge) noexcept {
    if (edge == nullptr) {
        return;
    }
    std::lock_guard<kimix::spin_mutex> lock(_dirty_edges_mutex);
    if (!edge->_dirty) {
        edge->_dirty = true;
        _dirty_edge_index.insert(edge->_id);
    }
    // Already dirty: the entry is either there, or a clear is in flight and wins.
}

void EdgeTable::clear_edge_dirty(Edge *edge) noexcept {
    if (edge == nullptr) {
        return;
    }
    std::lock_guard<kimix::spin_mutex> lock(_dirty_edges_mutex);
    // Erase first, then clear the flag: both halves of the pair happen inside this
    // one critical section, so no window exists in which the index says "clean"
    // while the flag still says "dirty" (or the reverse).
    _dirty_edge_index.erase(edge->_id);
    edge->_dirty = false;
}

void EdgeTable::_attach_dirty(Edge *edge) noexcept {
    if (edge == nullptr) {
        return;
    }
    std::lock_guard<kimix::spin_mutex> lock(_dirty_edges_mutex);
    edge->_dirty_sink = this;
    if (edge->_dirty) {
        _dirty_edge_index.insert(edge->_id);
    }
}

void EdgeTable::_detach_dirty(Edge *edge) noexcept {
    if (edge == nullptr) {
        return;
    }
    std::lock_guard<kimix::spin_mutex> lock(_dirty_edges_mutex);
    _dirty_edge_index.erase(edge->_id);
    edge->_dirty_sink = nullptr;
    // The FLAG is left exactly as it was (same rule as Graph::_detach_dirty): an
    // edge handed back by erase() may still be unwritten, and the journal-undo path
    // re-inserts it verbatim. Keeping the flag means insert() re-indexes it, so the
    // record is written instead of being silently dropped from the next save.
}

Edge *EdgeTable::find(EdgeId const &id) const {
    size_t const shard = shard_of(id);
    std::shared_lock<std::shared_mutex> lock(_shards[shard].mutex);
    return _find_locked(shard, id);
}

bool EdgeTable::contains(EdgeId const &id) const {
    return find(id) != nullptr;
}

bool EdgeTable::record(EdgeId const &id, EdgeRecord &out) const {
    size_t const shard = shard_of(id);
    std::shared_lock<std::shared_mutex> lock(_shards[shard].mutex);
    Edge *edge = _find_locked(shard, id);
    if (edge == nullptr) {
        return false;
    }
    out.id = edge->_id;
    out.origin = edge->_origin;
    out.target = edge->_target;
    out.direction = edge->_direction;
    out.labels = edge->_labels;
    out.properties.clear();
    out.properties.reserve(edge->_properties.size());
    for (auto const &entry : edge->_properties) {
        PropertyEntry item;
        item.name = entry.first;
        item.value = entry.second;
        out.properties.push_back(std::move(item));
    }
    return true;
}

bool EdgeTable::endpoints(EdgeId const &id, kimix::Guid &origin,
                          kimix::Guid &target) const {
    size_t const shard = shard_of(id);
    std::shared_lock<std::shared_mutex> lock(_shards[shard].mutex);
    Edge *edge = _find_locked(shard, id);
    if (edge == nullptr) {
        return false;
    }
    origin = edge->_origin;
    target = edge->_target;
    return true;
}

bool EdgeTable::direction(EdgeId const &id, EdgeDirectionType &out) const {
    size_t const shard = shard_of(id);
    std::shared_lock<std::shared_mutex> lock(_shards[shard].mutex);
    Edge *edge = _find_locked(shard, id);
    if (edge == nullptr) {
        return false;
    }
    out = edge->_direction;
    return true;
}

bool EdgeTable::labels(EdgeId const &id, kimix::vector<LabelId> &out) const {
    size_t const shard = shard_of(id);
    std::shared_lock<std::shared_mutex> lock(_shards[shard].mutex);
    Edge *edge = _find_locked(shard, id);
    if (edge == nullptr) {
        return false;
    }
    out = edge->_labels;
    return true;
}

bool EdgeTable::properties(EdgeId const &id, PropertyList &out) const {
    EdgeRecord record_copy;
    if (!record(id, record_copy)) {
        return false;
    }
    out = std::move(record_copy.properties);
    return true;
}

bool EdgeTable::has_label(EdgeId const &id, LabelId label) const {
    size_t const shard = shard_of(id);
    std::shared_lock<std::shared_mutex> lock(_shards[shard].mutex);
    Edge *edge = _find_locked(shard, id);
    if (edge == nullptr) {
        return false;
    }
    return std::find(edge->_labels.begin(), edge->_labels.end(), label) !=
           edge->_labels.end();
}

bool EdgeTable::property(EdgeId const &id, PropNameId name, PropertyValue &out) const {
    size_t const shard = shard_of(id);
    std::shared_lock<std::shared_mutex> lock(_shards[shard].mutex);
    Edge *edge = _find_locked(shard, id);
    if (edge == nullptr) {
        return false;
    }
    auto it = edge->_properties.find(name);
    if (it == edge->_properties.end()) {
        return false;
    }
    out = it->second;
    return true;
}

kimix::vector<EdgeRecord> EdgeTable::dirty_edges() const {
    // Two steps, O(dirty) each: snapshot the dirty IDS under the (innermost)
    // index mutex, release it, then resolve each id under ONE edge-shard SHARED
    // lock. The pair is not one atomic step, so a record cleaned between the two
    // is re-checked below — the index can report a record that is clean by now,
    // but never hide a dirty one.
    kimix::vector<EdgeId> ids;
    {
        std::lock_guard<kimix::spin_mutex> lock(_dirty_edges_mutex);
        ids.reserve(_dirty_edge_index.size());
        for (auto const &id : _dirty_edge_index) {
            ids.push_back(id);
        }
    }
    kimix::vector<EdgeRecord> out;
    out.reserve(ids.size());
    for (auto const &id : ids) {
        size_t const shard = shard_of(id);
        std::shared_lock<std::shared_mutex> lock(_shards[shard].mutex);
        Edge *edge = _find_locked(shard, id);
        if (edge == nullptr || !edge->is_dirty()) {
            continue; // erased after the snapshot, or cleared since
        }
        EdgeRecord record_copy;
        record_copy.id = edge->_id;
        record_copy.origin = edge->_origin;
        record_copy.target = edge->_target;
        record_copy.direction = edge->_direction;
        record_copy.labels = edge->_labels;
        record_copy.properties.reserve(edge->_properties.size());
        for (auto const &property : edge->_properties) {
            PropertyEntry item;
            item.name = property.first;
            item.value = property.second;
            record_copy.properties.push_back(std::move(item));
        }
        out.push_back(std::move(record_copy));
    }
    return out;
}

size_t EdgeTable::test_count_dirty_edges_by_scan() const noexcept {
    // TEST SEAM (see edge.h): the OLD linear collect's locking — every shard
    // shared, in index order — counting the flags instead of copying records.
    size_t count = 0;
    for (size_t shard_index = 0; shard_index < kEdgeShardCount; ++shard_index) {
        std::shared_lock<std::shared_mutex> lock(_shards[shard_index].mutex);
        for (auto const &entry : _shards[shard_index].edges) {
            Edge *edge = entry.second.get();
            if (edge != nullptr && edge->is_dirty()) {
                ++count;
            }
        }
    }
    return count;
}

void EdgeTable::mark_dirty() noexcept {
    for (size_t shard_index = 0; shard_index < kEdgeShardCount; ++shard_index) {
        std::shared_lock<std::shared_mutex> lock(_shards[shard_index].mutex);
        for (auto const &entry : _shards[shard_index].edges) {
            if (entry.second) {
                entry.second->mark_dirty();
            }
        }
    }
}

void EdgeTable::clear_dirty() noexcept {
    for (size_t shard_index = 0; shard_index < kEdgeShardCount; ++shard_index) {
        std::shared_lock<std::shared_mutex> lock(_shards[shard_index].mutex);
        for (auto const &entry : _shards[shard_index].edges) {
            if (entry.second) {
                entry.second->clear_dirty();
            }
        }
    }
}

void EdgeTable::clear_dirty(EdgeId const &id) noexcept {
    size_t const shard = shard_of(id);
    std::unique_lock<std::shared_mutex> lock(_shards[shard].mutex);
    Edge *edge = _find_locked(shard, id);
    if (edge != nullptr) {
        edge->clear_dirty();
    }
}

void EdgeTable::reserve(size_t entries) {
    size_t const per_shard = entries / kEdgeShardCount + 8;
    for (auto &shard : _shards) {
        std::unique_lock<std::shared_mutex> lock(shard.mutex);
        shard.edges.reserve(per_shard);
    }
}

// ---------------------------------------------------------------------------
// Edge record codec
// ---------------------------------------------------------------------------

size_t encoded_edge_record_size(EdgeRecord const &record) noexcept {
    return kEdgeRecordHeaderSize + payload_size(record);
}

void encode_edge_record(EdgeRecord const &record, kimix::vector<std::byte> &out) {
    out.clear();
    out.resize(encoded_edge_record_size(record));
    std::byte *cursor = out.data();
    std::memcpy(cursor, kEdgeRecordMagic.data(), kRecordMagicSize);
    cursor += kRecordMagicSize;
    write_guid(cursor, record.id);
    cursor += 16;
    write_guid(cursor, record.origin);
    cursor += 16;
    write_guid(cursor, record.target);
    cursor += 16;
    write_u8(cursor, static_cast<uint8_t>(record.direction));
    cursor += 1;
    write_u16(cursor, 0); // reserved
    cursor += 2;
    write_u8(cursor, 0); // reserved byte (keeps the header 8-byte aligned)
    cursor += 1;
    write_u32(cursor, static_cast<uint32_t>(record.labels.size()));
    cursor += 4;
    write_u32(cursor, static_cast<uint32_t>(record.properties.size()));
    cursor += 4;
    for (auto label : record.labels) {
        write_u32(cursor, label);
        cursor += 4;
    }
    for (auto const &entry : record.properties) {
        encode_property_entry(cursor, entry);
        cursor += encoded_property_entry_size(entry);
    }
}

bool decode_edge_record(kimix::span<const std::byte> bytes, EdgeRecord &out) noexcept {
    out = EdgeRecord{};
    if (bytes.size() < kEdgeRecordHeaderSize) {
        return false;
    }
    if (std::memcmp(bytes.data(), kEdgeRecordMagic.data(), kRecordMagicSize) != 0) {
        return false;
    }
    std::byte const *cursor = bytes.data() + kRecordMagicSize;
    out.id = read_guid(cursor);
    cursor += 16;
    out.origin = read_guid(cursor);
    cursor += 16;
    out.target = read_guid(cursor);
    cursor += 16;
    auto const direction = read_u8(cursor);
    cursor += 1;
    if (direction > static_cast<uint8_t>(EdgeDirectionType::undirected)) {
        return false;
    }
    out.direction = static_cast<EdgeDirectionType>(direction);
    cursor += 2; // reserved
    cursor += 1; // reserved
    uint32_t const label_count = read_u32(cursor);
    cursor += 4;
    uint32_t const property_count = read_u32(cursor);
    cursor += 4;
    std::byte const *end = bytes.data() + bytes.size();
    // Reject an impossible count before it drives an allocation.
    if (static_cast<uint64_t>(label_count) * 4ull > static_cast<uint64_t>(end - cursor)) {
        return false;
    }
    out.labels.reserve(label_count);
    for (uint32_t i = 0; i < label_count; ++i) {
        out.labels.push_back(read_u32(cursor));
        cursor += 4;
    }
    out.properties.reserve(property_count);
    for (uint32_t i = 0; i < property_count; ++i) {
        PropertyEntry entry;
        if (!decode_property_entry(cursor, end, entry)) {
            out = EdgeRecord{};
            return false;
        }
        out.properties.push_back(std::move(entry));
    }
    return true;
}

} // namespace kimix::graph_db
