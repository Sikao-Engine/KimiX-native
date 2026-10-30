// Test for the store metadata envelope and the v1 -> v2 migration path
// (src/graph_db/utils.h meta record + prefix-partitioned scans, and
// src/graph_db/serialize.cpp's format negotiation).
//
// This test covers:
// - the meta record codec: encode/decode round trip of every field (format
//   version, store uuid, both save timestamps, schema version, both id counters,
//   graph name), and a total decoder (truncated / wrong-magic / impossible-length
//   buffers are a `false`, never a crash).
// - a FULL save writes "meta/root" with kFormatVersion, and a save into a store
//   that already has one keeps its format version and store uuid.
// - prefix partitioning: scan_prefix() separates the "v/", "e/" and "s/" key
//   families of the single LMDB database, so an edge or schema record can never
//   be mistaken for a vertex.
// - a hand-crafted v1 store (GDB1 records, no "meta/root") loads as today:
//   migrated_from_v1 is true, format_version reads 1, every vertex arrives, and
//   edges are rebuilt from the dependency lists (synthesized_edge_count covers
//   them, the v1 reverse list is a cache).
// - the loaded VERTICES are CLEAN: an incremental save writes no v1 record, so the
//   stored "GDB1" values stay byte-identical. It does write the store's missing
//   "e/" records — the synthesized edges are adopted dirty for exactly that — once:
//   a second save is a byte-for-byte no-op.
// - the migrated edges round-trip: load a v1 store, save, reload, and every edge is
//   still there under the SAME id (the synthesized id is deterministic), now read
//   from its own record instead of from a projection.
// - a store whose meta announces format 3 is REFUSED with error_bad_file and
//   materializes nothing (no partial load of an unknown layout).
//
// The fiber scheduler is declared and bound first in main(), because the
// serializer's fan-outs submit to it.
#include "ut/ut.hpp"

#include "graph_db_test_util.h"

#include <core/kimix_core.h> // umbrella first: it fixes the winsock2.h order
#include <core/fiber.h>

#include <graph_db/graph.h>
#include <graph_db/graph_db.h>
#include <graph_db/serialize.h>
#include <graph_db/status.h>
#include <graph_db/utils.h>
#include <graph_db/vertex.h>

#include <core/guid.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using graph_db::ColdRecord;
using graph_db::ColdStorage;
using graph_db::GdiStatus;
using graph_db::Graph;
using graph_db::GraphDeserializer;
using graph_db::GraphSerializeStats;
using graph_db::GraphSerializer;
using graph_db::MetaRecord;
using graph_db::Vertex;
using graph_db::VertexRecord;
namespace gtest = kimix::graph_db::test;

namespace tests_unit_graph_db_test_graph_db_migrate {

template<typename T>
bool bytes_equal(kimix::vector<T> const &lhs, kimix::vector<T> const &rhs) {
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

// Store arbitrary bytes under an arbitrary key (a hand-crafted meta record, an
// edge record, a store that must look newer than this build).
bool write_raw(ColdStorage &cold, kimix::string_view key,
               kimix::span<const std::byte> bytes) {
    kimix::LMDBWriteCommand command;
    command.key.assign(reinterpret_cast<std::byte const *>(key.data()),
                       reinterpret_cast<std::byte const *>(key.data()) + key.size());
    command.value.assign(bytes.begin(), bytes.end());
    kimix::vector<kimix::LMDBWriteCommand> batch;
    batch.push_back(std::move(command));
    return cold.save_batch(std::move(batch));
}

bool store_meta(ColdStorage &cold, MetaRecord const &meta) {
    kimix::vector<std::byte> encoded;
    graph_db::encode_meta_record(meta, encoded);
    return write_raw(cold, graph_db::kMetaRootKey,
                     kimix::span<const std::byte>{encoded.data(), encoded.size()});
}

kimix::vector<kimix::string> keys_with_prefix(ColdStorage &cold, kimix::string_view prefix) {
    kimix::vector<ColdRecord> records;
    kimix::vector<kimix::string> out;
    if (!cold.scan_prefix(prefix, records)) {
        return out;
    }
    for (auto const &record : records) {
        out.push_back(kimix::string{reinterpret_cast<char const *>(record.key.data()),
                                    record.key.size()});
    }
    return out;
}

// A v1 store: GDB1 records only, no "meta/root". Three vertices, two edges
// (a -> b, a -> c), written by hand so the store is exactly a pre-metadata build's
// output.
struct v1_store {
    kimix::Guid a;
    kimix::Guid b;
    kimix::Guid c;
};

v1_store build_v1_store(ColdStorage &cold) {
    v1_store store;
    store.a = Guid{true};
    store.b = Guid{true};
    store.c = Guid{true};
    // a depends on b and c; the reverse lists are what a v1 save wrote (a cache).
    gtest::write_record(cold, gtest::make_record(store.a, gtest::to_bytes("a"),
                                                 {store.b, store.c}, {}));
    gtest::write_record(cold, gtest::make_record(store.b, gtest::to_bytes("b"), {},
                                                 {store.a}));
    gtest::write_record(cold, gtest::make_record(store.c, gtest::to_bytes("c"), {},
                                                 {store.a}));
    return store;
}

} // namespace tests_unit_graph_db_test_graph_db_migrate
using namespace tests_unit_graph_db_test_graph_db_migrate;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    // FIRST, before any suite can run: bind a scheduler for the whole process.
    kimix::fiber::scheduler sched{4u};

    "migrate_meta_record_round_trip"_test = [] {
        MetaRecord meta;
        meta.format_version = graph_db::kFormatVersion;
        meta.store_uuid = Guid{true};
        meta.last_full_save_ms = 1234567890123ull;
        meta.last_incr_save_ms = 1234567890124ull;
        meta.schema_version = 7;
        meta.next_label_id = 11;
        meta.next_prop_name_id = 13;
        meta.graph_name = "main";

        kimix::vector<std::byte> encoded;
        graph_db::encode_meta_record(meta, encoded);
        expect(eq(encoded.size(), graph_db::encoded_meta_size(meta)));

        MetaRecord decoded;
        expect(graph_db::decode_meta_record(
            kimix::span<const std::byte>{encoded.data(), encoded.size()}, decoded));
        expect(decoded.format_version == meta.format_version);
        expect(decoded.store_uuid == meta.store_uuid);
        expect(decoded.last_full_save_ms == meta.last_full_save_ms);
        expect(decoded.last_incr_save_ms == meta.last_incr_save_ms);
        expect(eq(decoded.schema_version, meta.schema_version));
        expect(eq(decoded.next_label_id, meta.next_label_id));
        expect(eq(decoded.next_prop_name_id, meta.next_prop_name_id));
        expect(decoded.graph_name == meta.graph_name);

        // Total decoder: a truncated buffer, a wrong magic and an impossible name
        // length are all `false`, never an out-of-bounds read.
        MetaRecord scratch;
        expect(!graph_db::decode_meta_record(
            kimix::span<const std::byte>{encoded.data(), encoded.size() - 1}, scratch));
        expect(!graph_db::decode_meta_record(kimix::span<const std::byte>{}, scratch));
        kimix::vector<std::byte> corrupt = encoded;
        corrupt[0] = std::byte{'X'};
        expect(!graph_db::decode_meta_record(
            kimix::span<const std::byte>{corrupt.data(), corrupt.size()}, scratch));

        // An empty graph name is a legal (and common) value.
        MetaRecord unnamed;
        kimix::vector<std::byte> unnamed_bytes;
        graph_db::encode_meta_record(unnamed, unnamed_bytes);
        MetaRecord unnamed_back;
        expect(graph_db::decode_meta_record(
            kimix::span<const std::byte>{unnamed_bytes.data(), unnamed_bytes.size()},
            unnamed_back));
        expect(unnamed_back.graph_name.empty());
    };

    "migrate_format_version_supported"_test = [] {
        expect(!graph_db::format_version_supported(0)) << "0 is not a format";
        expect(graph_db::format_version_supported(1)) << "a v1 store is loadable";
        expect(graph_db::format_version_supported(2))
            << "a v2 store (ABI-7 era: no size type) still loads";
        expect(graph_db::format_version_supported(graph_db::kFormatVersion));
        expect(!graph_db::format_version_supported(graph_db::kFormatVersion + 1))
            << "a newer store must be refused, never partially loaded";
        expect(eq(graph_db::kFormatVersion, uint32_t{3}))
            << "v3 = a property-type declaration carries its size type";
        // The one-way gate an OLD build applies to a store THIS build wrote: a v3
        // store read by an ABI-7 build, whose ceiling was 2. Its range check
        // (utils.cpp: format_version >= 1 && <= kFormatVersion) refuses the byte
        // image, and the check is pinned against the DECODED meta record because a
        // test cannot run an old binary.
        graph_db::MetaRecord stamped;
        stamped.format_version = graph_db::kFormatVersion;
        kimix::vector<std::byte> bytes;
        graph_db::encode_meta_record(stamped, bytes);
        graph_db::MetaRecord decoded;
        expect(graph_db::decode_meta_record(
            kimix::span<const std::byte>{bytes.data(), bytes.size()}, decoded));
        expect(eq(decoded.format_version, uint32_t{3}));
        constexpr uint32_t abi7_ceiling = 2; // what an ABI-7 build accepted
        expect(!(decoded.format_version >= 1 && decoded.format_version <= abi7_ceiling))
            << "an ABI-7 build refuses a v3 store through the meta record alone";
    };

    "migrate_full_save_writes_meta"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }
        ColdStorage cold;
        expect(cold.open(dir.path()));
        Graph graph;
        auto dag = gtest::small_dag(graph);
        GraphSerializer serializer{cold};
        GraphSerializeStats saved = serializer.save(graph);
        expect(eq(saved.saved_count, size_t{6}));
        expect(eq(saved.status, GdiStatus::ok));
        expect(eq(saved.format_version, graph_db::kFormatVersion));

        MetaRecord meta;
        expect(cold.read_meta(meta)) << "a full save stamps the store";
        expect(eq(meta.format_version, graph_db::kFormatVersion));
        expect(static_cast<bool>(meta.store_uuid)) << "the store gets an identity";
        expect(meta.last_full_save_ms > 0);

        // The identity survives the next save, and an incremental save updates
        // only its own timestamp.
        Guid const identity = meta.store_uuid;
        graph.update_content(dag.a->guid(), gtest::to_bytes("a2"));
        GraphSerializeStats incremental = serializer.save_incremental(graph);
        expect(eq(incremental.saved_count, size_t{1}));
        MetaRecord after;
        expect(cold.read_meta(after));
        expect(after.store_uuid == identity) << "the store keeps its uuid";
        expect(after.last_incr_save_ms > 0);
        expect(after.last_full_save_ms == meta.last_full_save_ms);
    };

    "migrate_scan_prefix_partitions_key_families"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }
        ColdStorage cold;
        expect(cold.open(dir.path()));
        Guid vertex{true};
        Guid edge{true};
        gtest::write_record(cold, gtest::make_record(vertex, gtest::to_bytes("v"),
                                                     {}, {}));
        // A hand-written edge record and a schema blob: neither is a vertex.
        auto edge_key_bytes = graph_db::edge_key(edge);
        write_raw(cold, kimix::string_view{reinterpret_cast<char const *>(edge_key_bytes.data()),
                                           edge_key_bytes.size()},
                  gtest::to_bytes("GDE2-not-a-real-record"));
        write_raw(cold, kimix::string_view{"s/labels"}, gtest::to_bytes("GDS2"));
        write_raw(cold, kimix::string_view{"meta/root"}, gtest::to_bytes("GDM2"));

        expect(eq(keys_with_prefix(cold, graph_db::kVertexKeyPrefix).size(), size_t{1}));
        expect(eq(keys_with_prefix(cold, graph_db::kEdgeKeyPrefix).size(), size_t{1}));
        expect(eq(keys_with_prefix(cold, graph_db::kSchemaKeyPrefix).size(), size_t{1}));

        // The vertex loader only ever sees the "v/" family.
        kimix::vector<VertexRecord> records;
        size_t skipped = 0;
        expect(cold.load_vertex_records(records, &skipped));
        expect(eq(records.size(), size_t{1}));
        expect(eq(skipped, size_t{0})) << "a foreign key is not a corrupt record";
        expect(records[0].guid == vertex);
    };

    "migrate_v1_store_loads_and_stays_byte_identical"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }
        ColdStorage cold;
        expect(cold.open(dir.path()));
        v1_store const store = build_v1_store(cold);
        // A v1 store carries no meta record at all.
        MetaRecord absent;
        expect(!cold.read_meta(absent));

        auto const before = gtest::snapshot_store(cold);
        expect(eq(before.size(), size_t{3}));

        Graph graph;
        GraphDeserializer deserializer;
        GraphSerializeStats loaded = deserializer.load(cold, graph);
        expect(eq(loaded.status, GdiStatus::ok));
        expect(eq(loaded.format_version, uint32_t{1}));
        expect(loaded.migrated_from_v1) << "an absent meta record means format 1";
        expect(eq(loaded.loaded_count, size_t{3}));
        expect(eq(loaded.skipped_count, size_t{0}));
        expect(eq(graph.size(), size_t{3}));
        expect(graph.contains(store.a));
        expect(graph.contains(store.b));
        expect(graph.contains(store.c));
        // The dependency lists became edges (a -> b, a -> c).
        expect(eq(graph.dependencies(store.a).size(), size_t{2}));
        expect(eq(graph.dependents(store.b).size(), size_t{1}));
        expect(graph.has_path(store.a, store.b));
        expect(graph.has_path(store.a, store.c));
        // Content survived the migration.
        expect(gtest::blob_equal(graph.find(store.a)->content_copy(), "a"));

        // The vertices load CLEAN, so every v1 vertex record keeps its exact bytes —
        // but the migration DOES write the store's missing "e/" records: a v1 store
        // has none, and this build no longer projects the adjacency into a vertex
        // record, so the edges synthesized above are adopted DIRTY precisely so this
        // save persists them once.
        GraphSerializer serializer{cold};
        GraphSerializeStats resaved = serializer.save_incremental(graph);
        expect(eq(resaved.saved_count, size_t{0}))
            << "the migration save rewrites no vertex record";
        expect(eq(resaved.saved_edge_count, size_t{2}))
            << "the two synthesized edges are written as real e/ records";
        expect(gt(resaved.bytes_written, size_t{0})) << "the edge records are bytes on disk";
        auto const after = gtest::snapshot_store(cold);
        expect(eq(after.size(), before.size()));
        for (size_t i = 0; i < before.size() && i < after.size(); ++i) {
            expect(before[i].guid == after[i].guid);
            expect(bytes_equal(before[i].value, after[i].value))
                << "a migrated v1 record must not be rewritten by a clean save";
        }
        expect(eq(keys_with_prefix(cold, graph_db::kEdgeKeyPrefix).size(), size_t{2}))
            << "the migration wrote the store's first edge records";

        // One write, not one per save: the adopted edges are clean now.
        GraphSerializeStats const settled = serializer.save_incremental(graph);
        expect(eq(settled.saved_count, size_t{0}));
        expect(eq(settled.saved_edge_count, size_t{0}));
        expect(eq(settled.bytes_written, size_t{0})) << "a second save writes nothing";
    };

    "migrate_v1_synthesized_edges_persist_through_a_save"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }
        ColdStorage cold;
        expect(cold.open(dir.path()));
        v1_store const store = build_v1_store(cold);
        expect(eq(keys_with_prefix(cold, graph_db::kEdgeKeyPrefix).size(), size_t{0}))
            << "a v1 store holds no edge records at all";

        // First load: the two v1 dependency links become edges with a DETERMINISTIC
        // id (serialize.cpp's synthesize_edge_id: MD5 over the two endpoint guids),
        // so a v1 store loads to the SAME edge ids every time.
        Graph first;
        GraphDeserializer deserializer;
        GraphSerializeStats const loaded = deserializer.load(cold, first);
        expect(eq(loaded.synthesized_edge_count, size_t{2}));
        kimix::vector<kimix::Guid> const first_ids = first.edge_ids();
        expect(eq(first_ids.size(), size_t{2}));

        // The migration save turns them into real records (a synthesized edge is
        // adopted dirty for exactly this reason).
        GraphSerializer serializer{cold};
        GraphSerializeStats const resaved = serializer.save_incremental(first);
        expect(eq(resaved.saved_count, size_t{0})) << "no vertex record is rewritten";
        expect(eq(resaved.saved_edge_count, size_t{2})) << "both edges become e/ records";
        expect(eq(keys_with_prefix(cold, graph_db::kEdgeKeyPrefix).size(), size_t{2}));

        // Reload from scratch: every edge now comes from its own record, nothing is
        // synthesized, and the ids are the SAME ones the first load produced.
        Graph second;
        GraphDeserializer reloader;
        GraphSerializeStats const reloaded = reloader.load(cold, second);
        expect(eq(reloaded.loaded_count, size_t{3}));
        expect(eq(reloaded.loaded_edge_count, size_t{2}));
        expect(eq(reloaded.synthesized_edge_count, size_t{0}))
            << "the edges come from their records, not from a projection";
        expect(!reloaded.migrated_from_v1)
            << "the save stamped meta/root (format 3), so this is no v1 store any more";
        expect(gtest::sorted_guid_text(second.edge_ids()) ==
               gtest::sorted_guid_text(first_ids))
            << "the synthesized id is deterministic, so a reload keeps the same edge ids";
        expect(eq(second.dependencies(store.a).size(), size_t{2}));
        expect(second.has_path(store.a, store.b));
        expect(second.has_path(store.a, store.c));
        expect(gtest::blob_equal(second.find(store.a)->content_copy(), "a"));

        // Nothing is left dirty by the second load, so a further save is a no-op.
        GraphSerializer resaver{cold};
        GraphSerializeStats const third = resaver.save_incremental(second);
        expect(eq(third.saved_count, size_t{0}));
        expect(eq(third.saved_edge_count, size_t{0}));
        expect(eq(third.bytes_written, size_t{0}));
    };

    "migrate_newer_store_is_refused"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }
        ColdStorage cold;
        expect(cold.open(dir.path()));
        v1_store const store = build_v1_store(cold);
        MetaRecord newer;
        newer.format_version = graph_db::kFormatVersion + 1;
        newer.store_uuid = Guid{true};
        newer.graph_name = "from-the-future";
        expect(store_meta(cold, newer));

        Graph graph;
        GraphDeserializer deserializer;
        GraphSerializeStats loaded = deserializer.load(cold, graph);
        expect(eq(loaded.status, GdiStatus::error_bad_file))
            << "a newer format is refused, not silently mis-read";
        expect(eq(loaded.format_version, graph_db::kFormatVersion + 1));
        expect(!loaded.migrated_from_v1);
        expect(eq(loaded.loaded_count, size_t{0})) << "nothing is materialized";
        expect(eq(loaded.skipped_count, size_t{0}));
        expect(eq(graph.size(), size_t{0}));
        expect(!graph.contains(store.a));
        // The refusal is a status, not a corrupted graph: the store is untouched.
        expect(eq(gtest::snapshot_store(cold).size(), size_t{3}));
    };

    "migrate_closed_store_reports_io"_test = [] {
        // A store that is not open is a recoverable precondition, i.e. error_io
        // (a failing mdb_* call inside LMDB stays fatal by design — see status.h).
        ColdStorage cold;
        Graph graph;
        GraphSerializer serializer{cold};
        expect(eq(serializer.save(graph).status, GdiStatus::error_io));
        GraphDeserializer deserializer;
        expect(eq(deserializer.load(cold, graph).status, GdiStatus::error_io));
    };
}
