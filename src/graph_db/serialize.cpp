/*
 * serialize.cpp — the parallel incremental save and the dual-stage load.
 * See serialize.h for the stage split, the guarantees and the caller contract;
 * this file only carries the plumbing.
 *
 * Stage layout (never mixed):
 *   save   collect(dirty) -> [fiber parallel] encode -> [calling thread] sort into
 *          LMDB key order -> [calling thread] 1 LMDB txn -> clear the dirty flags
 *          of exactly the written vertices.
 *   load   [sequential] LMDB scan -> [fiber parallel] decode -> [fiber parallel]
 *          materialize bare vertices -> [single thread] insert into the Graph
 *          -> [single thread] reserve the recorded degrees -> [single thread] link
 *          pass (Graph::connect) -> warnings.
 *
 * The load's ORDER is what makes it dependency-proof: no vertex is in the graph
 * while its edges are resolved, so no record can be rejected because of an edge,
 * and a cycle simply becomes two resolvable edges. See serialize.h.
 *
 * Nothing here blocks a fiber: the two fiber fan-outs only copy bytes and hash
 * them, and the one blocking step (the LMDB write transaction) runs on the
 * calling thread outside every fiber job — the graph_db_plan.md §9 risk note. On
 * a thread with no scheduler bound kimix::fiber::parallel() runs the bodies
 * inline, so both stages are also correct (just serial) from a foreign thread.
 */
#include <core/kimix_core.h> // umbrella first: it fixes the winsock2/windows.h order

#include "serialize.h"

#include <core/clock.h>
#include <core/fiber.h>
#include <core/stl/string.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <utility>

namespace kimix::graph_db {

namespace src_graph_db_serialize {

// Fan-out budget: the serializer never claims more workers than it can keep
// busy (a save is memory-bandwidth work, not latency work), and never more than
// the ambient pool has. `kimix::fiber::parallel` runs inline when this is 1,
// which is exactly the "no scheduler bound" case.
constexpr uint32_t k_max_fan_out = 8u;
// Records one fiber claim takes at a time. Small enough that the pack of
// workers still shares the work evenly for a handful of dirty vertices.
constexpr uint32_t k_claim_size = 4u;
// Individual stderr warnings printed per load; a badly broken store would
// otherwise flood the console. The counters in the stats stay exact.
constexpr size_t k_max_warnings = 8u;

[[nodiscard]] inline uint32_t fan_out_width() noexcept {
    return std::min<uint32_t>(kimix::fiber::worker_thread_count(), k_max_fan_out);
}

// Run [0, count) over the ambient fiber pool, `k_claim_size` ids per claim and at
// most `fan_out_width()` tasks at a time. Inline (and therefore deterministic in
// order) whenever the pool cannot help. `body` takes a uint32_t job id and must
// only touch the slot that id names.
template<typename Body>
void parallel_over(size_t count, Body &&body) noexcept {
    uint32_t const jobs =
        static_cast<uint32_t>(count > 0xffffffffull ? 0xffffffffull : count);
    kimix::fiber::parallel(jobs, std::forward<Body>(body), k_claim_size,
                           fan_out_width());
}

// Guid as text for a warning. Only used on the warning path, which runs on the
// calling thread after every fan-out has joined.
[[nodiscard]] kimix::string guid_text(kimix::Guid const &guid) noexcept {
    return guid.to_string();
}

void warn_record(char const *reason, kimix::Guid const &guid) noexcept {
    std::fprintf(stderr, "[kimix][graph_db] deserialize: %s, guid %s - record skipped\n",
                 reason, guid_text(guid).c_str());
}

void warn_edge(kimix::Guid const &from, kimix::Guid const &to,
               char const *reason) noexcept {
    std::fprintf(stderr,
                 "[kimix][graph_db] deserialize: %s, edge %s -> %s - dependency skipped\n",
                 reason, guid_text(from).c_str(), guid_text(to).c_str());
}

// One collected vertex record: the stage-1 collection slot, filled by the
// parallel decode, turned into a bare Vertex by the parallel materialize.
// Only the OUTGOING guid list is kept: the reverse list a record carries is a
// cache of the forward edges (Graph::connect rebuilds it), and the stored content
// hash is recomputed by the Vertex constructor, so keeping either here would be
// dead state. Both are still decoded (so a truncated or impossible count is
// caught), just not retained.
struct LoadSlot {
    kimix::Guid key_guid;                 // guid parsed out of the "v/" key
    kimix::vector<std::byte> raw;         // record bytes copied out of LMDB
    kimix::vector<std::byte> content;     // decoded content blob
    // v2 payload (empty for a v1 record)
    LabelId key_label{kNoLabel};
    kimix::vector<std::byte> external_id;
    kimix::vector<LabelId> labels;
    PropertyList properties;
    // The record's compatibility projection of its incident edges: used to
    // synthesize an edge that no "e/" record covers (all of a v1 store, and any
    // hand-written or partially written v2 store).
    kimix::vector<kimix::Guid> depending_guids;
    kimix::vector<kimix::Guid> depended_guids;
    kimix::Guid guid;                     // guid carried by the record itself
    kimix::unique_ptr<Vertex> vertex;     // materialized vertex (stage 2)
    Vertex *placed{nullptr};              // non-owning: the vertex now inside the graph
    bool valid{false};                    // decoded and self-consistent
    bool was_v1{false};                   // decoded from a "GDB1" record
};

// A sibling of EdgeRequest with the record's full payload, for the load's edge
// pass (an edge record carries labels and properties, unlike a bare adjacency).
struct LoadEdge {
    EdgeRecord record;
    bool valid{false};
};

[[nodiscard]] bool key_has_prefix(kimix::vector<std::byte> const &key,
                                  kimix::string_view prefix) noexcept {
    if (key.size() < prefix.size()) {
        return false;
    }
    return std::memcmp(key.data(), prefix.data(), prefix.size()) == 0;
}

// The deterministic edge id a v1 dependency becomes: MD5 over the two 16-byte
// endpoint guids. A v1 store therefore loads twice to the SAME edge ids, which is
// what makes a migrated store stable under repeated load + save cycles.
[[nodiscard]] kimix::Guid synthesize_edge_id(kimix::Guid const &origin,
                                             kimix::Guid const &target) noexcept {
    std::byte buffer[32];
    auto write_guid = [](std::byte *dst, kimix::Guid const &guid) noexcept {
        auto const &words = guid.to_binary();
        for (int i = 0; i < 8; ++i) {
            dst[i] = static_cast<std::byte>((words.data0 >> (i * 8)) & 0xffull);
            dst[8 + i] = static_cast<std::byte>((words.data1 >> (i * 8)) & 0xffull);
        }
    };
    write_guid(buffer, origin);
    write_guid(buffer + 16, target);
    kimix::MD5 const hash{kimix::span<uint8_t const>{
        reinterpret_cast<uint8_t const *>(buffer), sizeof(buffer)}};
    return kimix::Guid{hash};
}

} // namespace src_graph_db_serialize
using namespace src_graph_db_serialize;

// ===========================================================================
// Save
// ===========================================================================

GraphSerializeStats GraphSerializer::save(Graph &graph) {
    // FULL save = the incremental pipeline over EVERY vertex: mark first so the
    // collection below is the whole graph, whatever the flags said before.
    graph.mark_dirty();
    return _write(graph, /*full=*/true);
}

GraphSerializeStats GraphSerializer::save_incremental(Graph &graph) {
    return _write(graph, /*full=*/false);
}

GraphSerializeStats GraphSerializer::_write(Graph &graph, bool full) {
    _last_stats = GraphSerializeStats{};
    if (!_cold.valid()) {
        _last_stats.status = GdiStatus::error_io;
        return _last_stats;
    }
    kimix::Clock clock;
    GraphSerializeStats stats;
    stats.worker_count = fan_out_width();

    // --- stage 1: collect ---------------------------------------------------
    // Graph::dirty_vertices() is the ONLY source of the vertex write set: it takes
    // each vertex's SHARD shared lock for the map and the vertex's SHARED lock for
    // the cached hash, and returns guid + hash + Vertex* per dirty vertex. Clean
    // vertices never enter this vector, so an incremental save cannot touch them.
    kimix::vector<DirtyVertex> dirty = graph.dirty_vertices();
    dirty.erase(std::remove_if(dirty.begin(), dirty.end(),
                               [](DirtyVertex const &item) { return item.vertex == nullptr; }),
                dirty.end());
    // The EDGE family is a separate key space with its own dirty set: an edge is a
    // record of its own, so an edge-only change writes exactly one edge record and
    // no vertex record at all.
    kimix::vector<EdgeRecord> dirty_edges = graph.dirty_edges();
    size_t const count = dirty.size();
    size_t const edge_count = dirty_edges.size();
    // Deletions since the last save: one EMPTY value per removed key, written in
    // the same transaction as the records (see GraphSerializeStats).
    kimix::vector<kimix::Guid> removed_vertices = graph.take_removed_vertices();
    kimix::vector<EdgeId> removed_edges = graph.take_removed_edges();
    size_t const removed_count = removed_vertices.size() + removed_edges.size();

    // --- schema tables ------------------------------------------------------
    // Written by a full save, and by an incremental save when the registry moved
    // on. The tables are tiny (one record each), so re-writing them costs nothing
    // measurable and keeps "a save makes the store self-describing" true.
    kimix::vector<std::byte> label_table;
    kimix::vector<std::byte> property_type_table;
    size_t schema_bytes = 0;
    bool write_schema = false;
    if (_schema != nullptr) {
        MetaRecord stored_for_schema;
        bool const have_stored = _cold.read_meta(stored_for_schema);
        write_schema = full || !have_stored ||
                       stored_for_schema.schema_version != _schema->version();
        if (write_schema) {
            kimix::vector<LabelTableEntry> labels;
            PropertyTypeList types;
            uint32_t next_label = 1;
            uint32_t next_prop = 1;
            _schema->snapshot(labels, types, next_label, next_prop);
            encode_label_table(labels, label_table);
            encode_property_type_table(types, property_type_table);
            schema_bytes = label_table.size() + property_type_table.size();
        }
    }

    // --- meta record --------------------------------------------------------
    // The store's self-description ("meta/root", see utils.h). It is written
    //   - on a FULL save, always (that is the call that stamps the format), and
    //   - on an incremental save only when something else is being written, so a
    //     clean incremental save still touches no bytes at all (asserted by
    //     serialize_incremental_clean_graph_writes_nothing).
    MetaRecord stored_meta;
    bool const have_meta = _cold.read_meta(stored_meta);
    stats.format_version = have_meta ? stored_meta.format_version : 1;
    bool const need_meta =
        full || count > 0 || edge_count > 0 || removed_count > 0 || write_schema;
    if (need_meta) {
        MetaRecord meta = _meta;
        if (!have_meta) {
            // First write into this store: it becomes format 2 now, and it gets
            // an identity if the caller did not supply one.
            meta.format_version = kFormatVersion;
            if (!meta.store_uuid) {
                meta.store_uuid = kimix::Guid{true};
            }
        }
        if (_schema != nullptr) {
            meta.schema_version = _schema->version();
            kimix::vector<LabelTableEntry> labels;
            PropertyTypeList types;
            _schema->snapshot(labels, types, meta.next_label_id, meta.next_prop_name_id);
        }
        uint64_t const now_ms = static_cast<uint64_t>(kimix::Clock::now_ms());
        if (full) {
            meta.last_full_save_ms = now_ms;
        } else {
            meta.last_incr_save_ms = now_ms;
        }
        _meta = meta;
        stats.format_version = meta.format_version;
        _pending_meta = meta;
        _meta_dirty = true;
    } else {
        _meta_dirty = false;
    }

    if (count == 0 && edge_count == 0 && removed_count == 0) {
        // Nothing dirty: a full save still stamps the format and the schema, but
        // NOTHING else is written (an incremental save of a clean graph is a
        // no-op, byte for byte).
        if (_meta_dirty) {
            kimix::vector<kimix::LMDBWriteCommand> batch;
            if (write_schema) {
                kimix::LMDBWriteCommand labels_command;
                labels_command.key.assign(
                    reinterpret_cast<std::byte const *>(kSchemaKeyPrefix.data()),
                    reinterpret_cast<std::byte const *>(kSchemaKeyPrefix.data()) +
                        kSchemaKeyPrefix.size());
                for (char c : kimix::string_view{"labels"}) {
                    labels_command.key.push_back(static_cast<std::byte>(c));
                }
                labels_command.value = label_table;
                batch.push_back(std::move(labels_command));
                kimix::LMDBWriteCommand types_command;
                types_command.key.assign(
                    reinterpret_cast<std::byte const *>(kSchemaKeyPrefix.data()),
                    reinterpret_cast<std::byte const *>(kSchemaKeyPrefix.data()) +
                        kSchemaKeyPrefix.size());
                for (char c : kimix::string_view{"proptypes"}) {
                    types_command.key.push_back(static_cast<std::byte>(c));
                }
                types_command.value = property_type_table;
                batch.push_back(std::move(types_command));
            }
            kimix::LMDBWriteCommand meta_command;
            meta_command.key.assign(reinterpret_cast<std::byte const *>(kMetaRootKey.data()),
                                    reinterpret_cast<std::byte const *>(kMetaRootKey.data()) +
                                        kMetaRootKey.size());
            encode_meta_record(_pending_meta, meta_command.value);
            batch.push_back(std::move(meta_command));
            _cold.save_batch(std::move(batch));
            _meta_dirty = false;
            stats.bytes_written = schema_bytes;
        }
        _last_stats = stats;
        return _last_stats;
    }

    // One command + one written-hash per dirty vertex, one command per dirty
    // edge; the fan-out below fills DISJOINT slots, so it needs no lock.
    kimix::vector<kimix::LMDBWriteCommand> commands(count + edge_count + removed_count);
    for (size_t i = 0; i < removed_vertices.size(); ++i) {
        guid_key(removed_vertices[i], commands[count + edge_count + i].key);
        // An empty value IS the tombstone: the decoder rejects a zero-length
        // buffer, so an old reader would skip it rather than mis-read it.
        commands[count + edge_count + i].value.clear();
    }
    for (size_t i = 0; i < removed_edges.size(); ++i) {
        edge_key(removed_edges[i], commands[count + edge_count + removed_vertices.size() + i].key);
        commands[count + edge_count + removed_vertices.size() + i].value.clear();
    }
      kimix::vector<kimix::MD5> written_hashes(count);
      ContentDigest const digest_policy = graph.options().content_digest;

      // --- stage A: encode the vertices (fiber parallel) ----------------------
      // Each job copies its vertex's payload under the vertex lock, asks the graph
      // for the two degree hints under that vertex's shard shared lock, and encodes
      // the record straight into its own command slot. The adjacency itself is
      // NOT part of a vertex record any more: edges own their endpoints (see
      // utils.h, VertexRecordV2).
      parallel_over(count, [&graph, &dirty, &commands,
                            &written_hashes, digest_policy](uint32_t id) noexcept {
        DirtyVertex const &item = dirty[id];
        Vertex *vertex = item.vertex; // never null: filtered above
        VertexRecordV2 record;
        record.guid = item.guid;
        record.key_label = vertex->key_label();
        auto const &external_id = vertex->external_id().bytes();
        record.external_id.assign(external_id.begin(), external_id.end());
        kimix::vector<LabelId> labels;
        vertex->labels(labels);
        record.labels = std::move(labels);
        record.properties = vertex->property_list();
      vertex->content(record.content); // vertex SHARED lock, copies the blob
      // The record carries the GRAPH's digest policy (plan v3 §4.5): under
      // XXH3 the stored hash must equal the vertex's cached hash or the
      // post-write clear-dirty comparison would never match and every
      // incremental save would rewrite the vertex forever.
      record.content_hash = content_digest_of(record.content, digest_policy);
        // The adjacency projection: one guid per incident edge, read through the
        // graph's shard-locked accessors (a vertex record describes its own
        // adjacency, the edge records are the authority — see utils.h).
        AdjacencyBuffer adjacency;
        if (graph.dependencies(item.guid, adjacency)) {
            record.depending_guids.assign(adjacency.begin(), adjacency.end());
        }
        if (graph.dependents(item.guid, adjacency)) {
            record.depended_guids.assign(adjacency.begin(), adjacency.end());
        }
        guid_key(record.guid, commands[id].key);
        encode_vertex_record_v2(record, commands[id].value);
        written_hashes[id] = record.content_hash;
    });

    // --- stage A2: encode the edges (fiber parallel) ------------------------
    parallel_over(edge_count, [&dirty_edges, &commands, count](uint32_t id) noexcept {
        EdgeRecord const &record = dirty_edges[id];
        edge_key(record.id, commands[count + id].key);
        encode_edge_record(record, commands[count + id].value);
    });

    size_t bytes = 0;
    for (auto const &command : commands) {
        bytes += command.value.size();
    }
    stats.bytes_written = bytes + schema_bytes;

    // --- stage A': bulk-load order (epoch 2) --------------------------------
    // Sort the batch into LMDB key order before the transaction. The batch is
    // fully materialized here, so the sort cannot race anything, and it changes
    // neither the records nor the atomicity (still ONE transaction): it turns a
    // random-insert B-tree build into a bulk-load one. The keys are
    // "<prefix>/" + 16 raw id bytes, i.e. already the exact LMDB sort order.
    if (commands.size() > 1) {
        kimix::sort(commands.begin(), commands.end(),
                  [](kimix::LMDBWriteCommand const &lhs,
                     kimix::LMDBWriteCommand const &rhs) noexcept {
                      size_t const common = std::min(lhs.key.size(), rhs.key.size());
                      int const order = common == 0
                                            ? 0
                                            : std::memcmp(lhs.key.data(), rhs.key.data(), common);
                      if (order != 0) {
                          return order < 0;
                      }
                      return lhs.key.size() < rhs.key.size();
                  });
    }

    // The schema tables and the meta record ride in the SAME transaction as the
    // records they describe, so a crash can never leave a store whose metadata
    // claims a state its records do not have.
    if (write_schema) {
        kimix::LMDBWriteCommand labels_command;
        labels_command.key.assign(reinterpret_cast<std::byte const *>(kSchemaKeyPrefix.data()),
                                  reinterpret_cast<std::byte const *>(kSchemaKeyPrefix.data()) +
                                      kSchemaKeyPrefix.size());
        for (char c : kimix::string_view{"labels"}) {
            labels_command.key.push_back(static_cast<std::byte>(c));
        }
        labels_command.value = label_table;
        commands.push_back(std::move(labels_command));
        kimix::LMDBWriteCommand types_command;
        types_command.key.assign(reinterpret_cast<std::byte const *>(kSchemaKeyPrefix.data()),
                                 reinterpret_cast<std::byte const *>(kSchemaKeyPrefix.data()) +
                                     kSchemaKeyPrefix.size());
        for (char c : kimix::string_view{"proptypes"}) {
            types_command.key.push_back(static_cast<std::byte>(c));
        }
        types_command.value = property_type_table;
        commands.push_back(std::move(types_command));
    }

    // --- stage B: write (blocking, on the CALLING thread) -------------------
    // ONE ColdStorage::save_batch() = one LMDB transaction, i.e. the whole batch
    // is atomic: a crash leaves either every record or none. It stays out of the
    // fan-out on purpose - write_all() is a batch of blocking LMDB syscalls, so
    // running it inside a fiber job would park a pool worker for its duration
    // (graph_db_plan.md §9). A caller that is itself a fiber wraps the whole
    // save in kimix::fiber::blocking_call().
    if (_meta_dirty) {
        kimix::LMDBWriteCommand meta_command;
        meta_command.key.assign(reinterpret_cast<std::byte const *>(kMetaRootKey.data()),
                                reinterpret_cast<std::byte const *>(kMetaRootKey.data()) +
                                    kMetaRootKey.size());
        encode_meta_record(_pending_meta, meta_command.value);
        commands.push_back(std::move(meta_command));
        _meta_dirty = false;
    }
    if (!_cold.save_batch(std::move(commands))) {
        _last_stats = stats;
        return _last_stats;
    }
    stats.saved_count = count;
    stats.saved_edge_count = edge_count;
    stats.deleted_vertex_count = removed_vertices.size();
    stats.deleted_edge_count = removed_edges.size();

    // --- clear the flags of exactly the written vertices --------------------
    // A vertex is only marked clean when its cached hash still equals the hash
    // that was just written: a concurrent level-2 update between the copy and
    // this loop leaves it dirty, so the next incremental save rewrites it and
    // the change is not lost. Never touches a vertex that was not written.
    for (size_t i = 0; i < count; ++i) {
        Vertex *vertex = dirty[i].vertex;
        if (vertex->content_hash() == written_hashes[i]) {
            vertex->clear_dirty();
        }
    }
    // Same rule for the edge family: an edge marks itself dirty again on every
    // mutation, so clearing unconditionally is safe (a concurrent writer is
    // excluded by the single-writer contract).
    for (auto const &record : dirty_edges) {
        graph.edges().clear_dirty(record.id);
    }

    stats.save_ms = clock.toc();
    _last_stats = stats;
    return _last_stats;
}

// ===========================================================================
// Load
// ===========================================================================

GraphSerializeStats GraphDeserializer::load(ColdStore cold, Graph &graph,
                                           Schema *schema) {
    _last_stats = GraphSerializeStats{};
    if (!cold.valid()) {
        _last_stats.status = GdiStatus::error_io;
        return _last_stats;
    }
    kimix::Clock clock;
    GraphSerializeStats stats;
    stats.worker_count = fan_out_width();

    // --- format negotiation (BEFORE anything is materialized) ---------------
    // A store that announces a newer format is REFUSED outright: a partial load
    // of an unknown layout would hand the caller a graph that silently lost half
    // its state, which is strictly worse than an error it can report.
    {
        MetaRecord meta;
        if (cold.read_meta(meta)) {
            stats.format_version = meta.format_version;
            if (!format_version_supported(meta.format_version)) {
                std::fprintf(stderr,
                             "[kimix][graph_db] deserialize: store format %u is newer "
                             "than this build (%u) - refusing to load\n",
                             meta.format_version, kFormatVersion);
                stats.status = GdiStatus::error_bad_file;
                _last_stats = stats;
                return _last_stats;
            }
            // A v1 store is the one without a meta record; the flag names the
            // FORMAT it came from, not "older than this build" (a v2 store is
            // upgraded in place by the loader but is not a v1 migration).
            stats.migrated_from_v1 = meta.format_version < 2;
        } else {
            // No meta record at all: a v1 store.
            stats.format_version = 1;
            stats.migrated_from_v1 = true;
        }
    }

    // --- stage 1: collect (ONE sequential scan, partitioned by key prefix) ---
    // kimix::LMDB's iterator owns ONE cursor over one read transaction, so the
    // scan itself is sequential (graph_db_plan.md §9: shard the cursor in a later
    // epoch). The spans it yields die on the next ++, hence the copy into `raw`.
    // The key prefix decides the family: "v/" is a vertex, "e/" an edge, "s/" a
    // schema table, everything else (meta/root) is not an entity at all.
    kimix::vector<ColdRecord> scanned;
    if (!cold.load_all_records(scanned)) {
        _last_stats = stats;
        return _last_stats;
    }

    kimix::vector<LoadSlot> slots;
    kimix::vector<kimix::vector<std::byte>> raw_edges;
    kimix::vector<std::byte> label_table;
    kimix::vector<std::byte> property_type_table;
    slots.reserve(scanned.size() / 2 + 8);
    size_t tombstone_count = 0;
    for (auto &entry : scanned) {
        if (entry.value.empty()) {
            // A tombstone: this entity was deleted and the save stored an empty
            // value under its key. Not an entity, and NOT a corrupt record.
            if (key_has_prefix(entry.key, kVertexKeyPrefix) ||
                key_has_prefix(entry.key, kEdgeKeyPrefix)) {
                ++tombstone_count;
                continue;
            }
        }
        if (key_has_prefix(entry.key, kVertexKeyPrefix)) {
            auto key_guid = guid_from_key(
                kimix::span<const std::byte>{entry.key.data(), entry.key.size()});
            if (!key_guid) {
                ++stats.skipped_count; // a "v/" key that is not 18 bytes
                continue;
            }
            LoadSlot slot;
            slot.key_guid = *key_guid;
            slot.raw = std::move(entry.value);
            slots.push_back(std::move(slot));
            continue;
        }
        if (key_has_prefix(entry.key, kEdgeKeyPrefix)) {
            raw_edges.push_back(std::move(entry.value));
            continue;
        }
        if (key_has_prefix(entry.key, kSchemaKeyPrefix)) {
            kimix::string_view const name{
                reinterpret_cast<char const *>(entry.key.data()) + kSchemaKeyPrefix.size(),
                entry.key.size() - kSchemaKeyPrefix.size()};
            if (name == kimix::string_view{"labels"}) {
                label_table = std::move(entry.value);
            } else if (name == kimix::string_view{"proptypes"}) {
                property_type_table = std::move(entry.value);
            }
            continue;
        }
        // "meta/root" was already consumed above; an unknown key family is simply
        // not an entity (a forward-compatible store must not fail to load).
    }
    scanned.clear();
    stats.tombstone_count = tombstone_count;

    // --- schema tables (before the records that name them) ------------------
    if (schema != nullptr) {
        kimix::vector<LabelTableEntry> labels;
        PropertyTypeList types;
        bool const have_labels = !label_table.empty() &&
                                 decode_label_table(
                                     kimix::span<const std::byte>{label_table.data(),
                                                                  label_table.size()},
                                     labels);
        bool const have_types = !property_type_table.empty() &&
                                decode_property_type_table(
                                    kimix::span<const std::byte>{property_type_table.data(),
                                                                 property_type_table.size()},
                                    types);
        if (have_labels || have_types) {
            MetaRecord meta;
            uint32_t next_label = 1;
            uint32_t next_prop = 1;
            uint32_t schema_version = 0;
            if (cold.read_meta(meta)) {
                next_label = meta.next_label_id;
                next_prop = meta.next_prop_name_id;
                schema_version = meta.schema_version;
            }
            if (!schema->restore(labels, types, next_label, next_prop, schema_version)) {
                // A corrupt table leaves the schema empty rather than half
                // restored; the records still load (their ids simply do not
                // resolve to names, which the read-back reports as an unknown id).
                std::fprintf(stderr,
                             "[kimix][graph_db] deserialize: corrupt schema table - "
                             "names are unavailable, records still load\n");
            } else {
                stats.restored_label_count = labels.size();
                stats.restored_property_type_count = types.size();
            }
        }
    }

    // Parallel decode: every slot parses its own bytes, no shared state at all.
    parallel_over(slots.size(), [&slots](uint32_t id) noexcept {
        LoadSlot &slot = slots[id];
        VertexRecordV2 record;
        bool was_v1 = false;
        if (!decode_any_vertex_record(
                kimix::span<const std::byte>{slot.raw.data(), slot.raw.size()}, record,
                was_v1)) {
            slot.valid = false; // corrupt: bad magic / truncated / impossible counts
            return;
        }
        // The record must agree with its own key, otherwise a copy of one record
        // could impersonate another vertex.
        if (record.guid != slot.key_guid) {
            slot.valid = false;
            return;
        }
        slot.guid = record.guid;
        slot.was_v1 = was_v1;
        slot.key_label = record.key_label;
        slot.external_id = std::move(record.external_id);
        slot.labels = std::move(record.labels);
        slot.properties = std::move(record.properties);
        slot.depending_guids = std::move(record.depending_guids);
        slot.depended_guids = std::move(record.depended_guids);
        slot.content = std::move(record.content);
        slot.valid = true;
    });

    size_t bytes_read = 0;
    for (auto const &slot : slots) {
        bytes_read += slot.raw.size();
    }
    for (auto const &raw : raw_edges) {
        bytes_read += raw.size();
    }
    stats.bytes_read = bytes_read;
    // The record bytes are no longer needed: the decode above moved everything
    // useful out of them.
    for (auto &slot : slots) {
        kimix::vector<std::byte>{}.swap(slot.raw);
    }

    // --- stage 2: materialize (fiber parallel) ------------------------------
    // A bare vertex: guid, key, labels, properties and content, and NO edge. A
    // vertex's existence cannot depend on any other vertex, which is the
    // structural half of the dual-stage guarantee. The constructor recomputes the
    // content hash, so a store carrying a stale hash is repaired here.
      ContentDigest const content_digest = graph.options().content_digest;
      parallel_over(slots.size(), [&slots, content_digest](uint32_t id) noexcept {
          LoadSlot &slot = slots[id];
          if (!slot.valid) {
              return;
          }
          slot.vertex = kimix::unique_ptr<Vertex>{
              new Vertex(slot.guid,
                         kimix::span<const std::byte>{slot.content.data(), slot.content.size()},
                         content_digest)};
      });

    // --- insert (single thread: adoption is a structural write) -------------
    kimix::vector<LoadSlot *> placed_slots;
    placed_slots.reserve(slots.size());
    size_t printed_records = 0;
    for (auto &slot : slots) {
        if (!slot.valid || !slot.vertex) {
            ++stats.skipped_count;
            if (printed_records < k_max_warnings) {
                ++printed_records;
                warn_record("corrupt or key/guid mismatch", slot.key_guid);
            }
            continue;
        }
        // Fill the GDI payload before the adoption, so the vertex is complete the
        // moment it becomes visible (a reader can never see a keyed vertex without
        // its labels).
        slot.vertex->assign_payload(
            slot.key_label,
            kimix::span<const std::byte>{slot.external_id.data(), slot.external_id.size()},
            kimix::span<const LabelId>{slot.labels.data(), slot.labels.size()},
            slot.properties);
        GdiStatus const status = graph.adopt_vertex(std::move(slot.vertex));
        if (is_error(status)) {
            // A refused insert (already present guid, or the caller is not the
            // graph's writer thread): the record is dropped, never forced.
            ++stats.skipped_count;
            if (printed_records < k_max_warnings) {
                ++printed_records;
                warn_record(status == GdiStatus::error_vertex
                                ? "guid already present in the target graph"
                                : "vertex insert refused by the graph",
                            slot.guid);
            }
            continue;
        }
        slot.placed = graph.find(slot.guid);
        if (slot.placed == nullptr) {
            ++stats.skipped_count;
            continue;
        }
        placed_slots.push_back(&slot);
        ++stats.loaded_count;
    }

    // --- edges: the v2 "e/" records are authoritative -----------------------
    // Every edge record is adopted with its uid, direction, labels and properties
    // verbatim; a record whose endpoint is missing is a dangling edge, skipped and
    // counted (the vertex pass is complete by now, so this really is a bad record).
    size_t skipped_edges = 0;
    size_t printed_edges = 0;
    // Records that named a pair but could not be adopted (a dangling endpoint).
    // The link fallback below must not count the same logical edge a second time.
    kimix::vector<EdgeRecord> failed_edges;
    for (auto &raw : raw_edges) {
        EdgeRecord record;
        if (!decode_edge_record(kimix::span<const std::byte>{raw.data(), raw.size()}, record)) {
            ++skipped_edges;
            if (printed_edges < k_max_warnings) {
                ++printed_edges;
                std::fprintf(stderr,
                             "[kimix][graph_db] deserialize: corrupt edge record - skipped\n");
            }
            continue;
        }
        kimix::unique_ptr<Edge> edge{new Edge(record.id, record.origin, record.target,
                                             record.direction)};
        edge->assign_payload(kimix::span<const LabelId>{record.labels.data(),
                                                       record.labels.size()},
                             record.properties);
        edge->clear_dirty(); // exactly what the store holds
        GdiStatus const status = graph.adopt_edge(std::move(edge));
        if (is_error(status)) {
            ++skipped_edges;
            failed_edges.push_back(record);
            if (printed_edges < k_max_warnings) {
                ++printed_edges;
                warn_edge(record.origin, record.target, "edge record could not be adopted");
            }
            continue;
        }
        ++stats.loaded_edge_count;
    }

    // --- link fallback: a link with no edge record becomes an edge ----------
    // A v1 record carries its outgoing guid list instead of edge objects, and a
    // partially written v2 store can hold a link whose edge record is missing.
    // Every such link becomes a directed edge with a DETERMINISTIC id (see
    // synthesize_edge_id), so a v1 store loads to the same graph twice and a
    // later save writes stable ids. A link an edge record already covers is left
    // alone (the edge records are authoritative).
    for (auto *slot : placed_slots) {
        if (slot->placed == nullptr || slot->depending_guids.empty()) {
            continue;
        }
        for (auto const &target_guid : slot->depending_guids) {
            if (target_guid == slot->guid) {
                ++stats.skipped_edge_count;
                if (printed_edges < k_max_warnings) {
                    ++printed_edges;
                    warn_edge(slot->guid, target_guid, "self dependency");
                }
                continue;
            }
            if (graph.has_edge(slot->guid, target_guid)) {
                continue; // already covered by an "e/" record
            }
            bool already_counted = false;
            for (auto const &failed : failed_edges) {
                if (failed.origin == slot->guid && failed.target == target_guid) {
                    already_counted = true;
                    break;
                }
            }
            if (already_counted) {
                continue; // the edge RECORD already reported this link
            }
            if (!graph.contains(target_guid)) {
                ++stats.skipped_edge_count;
                if (printed_edges < k_max_warnings) {
                    ++printed_edges;
                    warn_edge(slot->guid, target_guid, "missing dependency");
                }
                continue;
            }
            kimix::unique_ptr<Edge> edge{
                new Edge(synthesize_edge_id(slot->guid, target_guid), slot->guid, target_guid,
                         EdgeDirectionType::directed)};
            edge->clear_dirty();
            GdiStatus const status = graph.adopt_edge(std::move(edge));
            if (is_error(status)) {
                ++stats.skipped_edge_count;
                if (printed_edges < k_max_warnings) {
                    ++printed_edges;
                    warn_edge(slot->guid, target_guid, "edge refused by the graph");
                }
                continue;
            }
            ++stats.synthesized_edge_count;
            ++stats.loaded_edge_count;
        }
    }

    // --- settle the dirty flags --------------------------------------------
    // Loaded vertices are CLEAN: their content and edges are exactly what the
    // store holds, so an immediate save_incremental() writes nothing.
    for (auto *slot : placed_slots) {
        if (slot->placed != nullptr) {
            slot->placed->clear_dirty();
        }
    }

    // One-line summary, always: a broken store should never fail silently.
    stats.skipped_edge_count += skipped_edges;
    if (stats.skipped_count > printed_records ||
        stats.skipped_edge_count > printed_edges) {
        std::fprintf(stderr,
                     "[kimix][graph_db] deserialize: %zu record(s) and %zu edge(s) skipped "
                     "(%zu + %zu warning(s) shown)\n",
                     stats.skipped_count, stats.skipped_edge_count, printed_records,
                     printed_edges);
    }

    stats.load_ms = clock.toc();
    _last_stats = stats;
    return _last_stats;
}

} // namespace kimix::graph_db
