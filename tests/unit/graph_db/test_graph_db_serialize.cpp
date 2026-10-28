// Test for the graph_db serializer/deserializer (src/graph_db/serialize.*).
// This test covers:
// - full save -> load round trip on a fresh temp LMDB dir: guids, content bytes
//   and content hashes, BOTH dependency directions and roots() preserved
// - a string_array property (ABI 7, plan §1.1) on a VERTEX and on an EDGE
//   surviving persist -> close -> reopen -> read-back byte-identically, on BOTH
//   cold-store backends (the LMDB default and the opt-in flat file): see the
//   serialize_string_array_* legs
// - incremental save: only the dirty vertices are rewritten; every untouched
//   record is proved BIT-IDENTICAL against a snapshot taken after the first save
//   (plus a new vertex and a new edge that must land in the second save only)
// - a clean graph makes an incremental save write nothing at all
// - a full save after an incremental one rewrites everything (stats == all)
// - the dual-stage guarantee on a hand-crafted store: a dependency cycle
//   (A <-> B) and a dangling guid (C -> missing Z) load successfully, the graph
//   holds A, B and C, the unresolvable edge is counted, nothing is lost
// - the link pass rebuilds missing reverse edges from the authoritative
//   outgoing list
// - parallel determinism: the same store loaded twice yields identical guid sets
//   and identical adjacency in both directions (and equals the source adjacency)
// - corrupt record (garbage / truncated bytes under a valid "v/" key): the
//   vertices are skipped, the load completes, the good vertices are intact and
//   the now-unresolvable edges are counted
// - a record whose embedded guid disagrees with its key is skipped, not adopted
// - a load never overwrites a vertex already present in the target graph
// - empty store / empty graph / closed store behaviour
// - copy_to() checkpoint passthrough produces a loadable store
//
// The fiber scheduler is declared and bound FIRST in main() so it outlives every
// suite (Boost.UT runs them from the _test destructors at the end of main), per
// the fiber skill's root-main pattern. All assertions run on the main thread; the
// serializer's own fan-out only touches plain data.

#include "ut/ut.hpp"
#include "graph_db_test_util.h"

#include <core/kimix_core.h> // umbrella first: it fixes the winsock2.h order
#include <core/fiber.h>

#include <graph_db/graph.h>
#include <graph_db/graph_db.h>
#include <graph_db/serialize.h>
#include <graph_db/utils.h>
#include <graph_db/vertex.h>

#include <core/guid.h>
#include <core/md5.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <algorithm>
#include <cstddef>
#include <thread>
#include <utility>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using graph_db::ColdStorage;
using graph_db::EdgeDirectionType;
using graph_db::FileColdStore;
using graph_db::Graph;
using graph_db::GraphDeserializer;
using graph_db::GraphSerializeStats;
using graph_db::GraphSerializer;
using graph_db::PropNameId;
using graph_db::PropertyKind;
using graph_db::PropertyValue;
using graph_db::Vertex;
using graph_db::VertexRecordV2;
namespace gtest = kimix::graph_db::test;

namespace tests_unit_graph_db_test_graph_db_serialize {

// Sorted guid texts of a vertex list, resolved through the graph: an order-free
// identity check that also proves every guid is resolvable.
kimix::vector<kimix::string> resolved_guids(Graph const &graph,
                                            kimix::vector<Vertex *> const &vertices) {
    kimix::vector<Vertex *> resolved;
    resolved.reserve(vertices.size());
    for (auto *vertex : vertices) {
        if (vertex != nullptr) {
            resolved.push_back(graph.find(vertex->guid()));
        }
    }
    return gtest::sorted_guid_text(resolved);
}

// True when `bytes` is exactly `other` (bit-identical, size included).
bool bytes_equal(kimix::span<const std::byte> bytes,
                 kimix::span<const std::byte> other) noexcept {
    return bytes.size() == other.size() &&
           std::equal(bytes.begin(), bytes.end(), other.begin());
}

// Number of records a store holds (only "v/" keys count).
size_t record_count(ColdStorage const &cold) {
    return gtest::snapshot_store(cold).size();
}

// The raw stored value of `guid` decoded back into a record. False when the value
// is missing or does not decode.
bool read_stored_record(ColdStorage const &cold, kimix::Guid const &guid,
                        graph_db::VertexRecord &out) {
    auto value = gtest::stored_value(cold, guid);
    if (value.empty()) {
        return false;
    }
    return graph_db::decode_vertex_record(value, out);
}

// ---------------------------------------------------------------------------
// string_array persistence fixtures (ABI 7, plan §1.1)
// ---------------------------------------------------------------------------

// True when `got` is a string_array holding exactly the same elements as `want`,
// in the same order, AND its wire block is byte-identical to `want`'s. The block
// compare is the strong half: it proves the store reproduced the canonical §1.1
// image rather than merely a value that happens to compare equal.
bool string_arrays_equal(PropertyValue const &got, PropertyValue const &want) {
    if (got.kind() != PropertyKind::string_array || want.kind() != PropertyKind::string_array) {
        return false;
    }
    if (!bytes_equal(got.as_string_array_block(), want.as_string_array_block())) {
        return false;
    }
    auto const lhs = got.as_string_array();
    auto const rhs = want.as_string_array();
    if (lhs.element_count() != rhs.element_count()) {
        return false;
    }
    uint32_t index = 0;
    for (auto const element : lhs) {
        if (index >= rhs.element_count() || element != rhs.element(index)) {
            return false;
        }
        ++index;
    }
    return index == lhs.element_count();
}

// The offset of the first occurrence of `needle` in `haystack`, or
// haystack.size() when absent. Used to locate a property payload inside a stored
// record so a test can damage exactly one byte of it.
size_t find_span(kimix::span<const std::byte> haystack, kimix::span<const std::byte> needle) {
    if (needle.size() > haystack.size()) {
        return haystack.size();
    }
    for (size_t at = 0; at + needle.size() <= haystack.size(); ++at) {
        if (bytes_equal(haystack.subspan(at, needle.size()), needle)) {
            return at;
        }
    }
    return haystack.size();
}

// Two vertices joined by one edge, the vertex AND the edge carrying a
// string_array. Built at the container level (Graph + Vertex + the edge table):
// a typed property is stored exactly as it is given here, so this exercises the
// record codec and the persistence path with no schema in the way. Graph is
// non-movable, so the fixture is filled in place rather than returned.
struct string_array_graph {
    Graph graph;
    kimix::Guid a;
    kimix::Guid b;
    kimix::Guid edge;
    PropNameId vertex_docs{7001};
    PropNameId edge_docs{7002};
    PropertyValue expected_vertex;
    PropertyValue expected_edge;
};

void fill_string_array_graph(string_array_graph &f) {
    Vertex *a = f.graph.create_vertex(gtest::to_bytes("docs-a"));
    Vertex *b = f.graph.create_vertex(gtest::to_bytes("docs-b"));
    expect(a != nullptr);
    expect(b != nullptr);
    f.a = a->guid();
    f.b = b->guid();

    // Multi-byte UTF-8, an empty element and a long id: every shape the block has
    // to survive.
    char const *const utf8 = "\xE4\xB8\xAD\xE6\x96\x87"; // 中文
    kimix::vector<kimix::string_view> vertex_elements{
        "doc-1", kimix::string_view{utf8, 6}, "", "readers/42"};
    f.expected_vertex = PropertyValue::make_string_array(
        kimix::span<const kimix::string_view>{vertex_elements.data(), vertex_elements.size()});
    kimix::vector<kimix::string_view> edge_elements{"evidence-1", "evidence-2"};
    f.expected_edge = PropertyValue::make_string_array(
        kimix::span<const kimix::string_view>{edge_elements.data(), edge_elements.size()});

    expect(a->add_property(f.vertex_docs, f.expected_vertex)) << "the vertex stores it";
    graph_db::PropertyList edge_properties;
    edge_properties.push_back(graph_db::PropertyEntry{f.edge_docs, f.expected_edge});
    expect(eq(f.graph.edge_create(f.a, f.b, EdgeDirectionType::directed, {}, &edge_properties,
                                  f.edge),
              graph_db::GdiStatus::ok))
        << "the edge carries its own string_array";
}

// Persist `source` into a store of `Store` (the LMDB ColdStorage or the flat-file
// FileColdStore — both speak open(path)/close()), REOPEN it through a fresh
// handle, and load it into a fresh graph. Every string_array must come back
// byte-identical, on the vertex and on the edge alike. The two backends run the
// same body, so their legs cannot drift apart.
template<typename Store>
void check_string_array_reopens(kimix::string_view tag, kimix::filesystem::path const &dir,
                                string_array_graph &source) {
    Store cold;
    expect(cold.open(dir)) << tag << ": the store must open";
    GraphSerializer serializer{cold};
    GraphSerializeStats const saved = serializer.save(source.graph);
    expect(eq(saved.saved_count, size_t{2})) << tag << ": a full save writes both vertices";
    expect(eq(saved.saved_edge_count, size_t{1})) << tag << ": and the edge record";
    cold.close();

    Store reopened;
    expect(reopened.open(dir)) << tag << ": the store reopens";
    Graph restored;
    GraphDeserializer deserializer;
    GraphSerializeStats const loaded = deserializer.load(reopened, restored);
    expect(eq(loaded.loaded_count, size_t{2})) << tag << ": both vertices load";
    expect(eq(loaded.loaded_edge_count, size_t{1})) << tag << ": the edge loads";
    expect(eq(loaded.skipped_count, size_t{0})) << tag << ": nothing was refused";
    expect(eq(loaded.skipped_edge_count, size_t{0})) << tag << ": and it resolved";

    Vertex *vertex = restored.find(source.a);
    expect(vertex != nullptr) << tag;
    if (vertex != nullptr) {
        PropertyValue back;
        expect(vertex->property(source.vertex_docs, back))
            << tag << ": the vertex's string_array came back";
        expect(back.kind() == PropertyKind::string_array) << tag;
        expect(string_arrays_equal(back, source.expected_vertex))
            << tag << ": the vertex property is byte-identical after a reopen";
    }
    PropertyValue edge_back;
    expect(restored.edge_property(source.edge, source.edge_docs, edge_back))
        << tag << ": the edge's string_array came back";
    expect(edge_back.kind() == PropertyKind::string_array) << tag;
    expect(string_arrays_equal(edge_back, source.expected_edge))
        << tag << ": the edge property is byte-identical after a reopen";

    // The loaded graph is clean and its payload re-encodes to the same bytes, so a
    // second save would be a no-op rather than a rewrite.
    expect(restored.dirty_vertices().empty()) << tag;
    PropertyValue again;
    expect(restored.edge_property(source.edge, source.edge_docs, again));
    expect(bytes_equal(again.as_string_array_block(),
                       source.expected_edge.as_string_array_block()))
        << tag << ": repeated reads of the same edge payload agree";
    reopened.close();
}

} // namespace tests_unit_graph_db_test_graph_db_serialize
using namespace tests_unit_graph_db_test_graph_db_serialize;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    // FIRST, before any suite can run: bind a scheduler for the whole process.
    // The serializer's fan-outs submit to it; declared here so it outlives every
    // suite Boost.UT runs from the _test destructors at the end of main().
    kimix::fiber::scheduler sched{4u};

    "serialize_scheduler_is_bound"_test = [] {
        expect(kimix::fiber::is_bound())
            << "the serializer's fan-out needs a scheduler bound to this thread";
        expect(ge(kimix::fiber::worker_thread_count(), 2u))
            << "the tests must exercise a real fan-out, not the inline path";
    };

    "serialize_full_round_trip"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok()) << "a temp directory must be creatable";
        if (!dir.ok()) {
            return;
        }

        // Source graph: the known 6-vertex DAG (a->b, a->c, b->d, c->d, f->d).
        Graph source;
        gtest::known_dag dag = gtest::small_dag(source);
        expect(eq(source.size(), size_t{6}));
        // A root is a vertex nothing depends on: a, e (isolated) and f.
        expect(eq(source.roots().size(), size_t{3})) << "a, e and f are the roots";

        ColdStorage cold;
        expect(cold.open(dir.path()));
        GraphSerializer serializer{cold};
        expect(serializer.valid());

        GraphSerializeStats saved = serializer.save(source);
        expect(eq(saved.saved_count, size_t{6})) << "a full save writes every vertex";
        expect(eq(saved.skipped_count, size_t{0}));
        expect(gt(saved.bytes_written, size_t{0}));
        expect(ge(saved.worker_count, size_t{1}));
        expect(eq(record_count(cold), size_t{6}));
        expect(source.dirty_vertices().empty())
            << "a full save clears the dirty flags of the written vertices";
        expect(serializer.last_stats().saved_count == saved.saved_count)
            << "last_stats() mirrors the returned stats";

        // Round trip into a fresh graph.
        Graph restored;
        GraphDeserializer deserializer;
        GraphSerializeStats loaded = deserializer.load(cold, restored);
        expect(eq(loaded.loaded_count, size_t{6}));
        expect(eq(loaded.skipped_count, size_t{0}));
        expect(eq(loaded.skipped_edge_count, size_t{0}))
            << "a self-consistent store resolves every edge";
        expect(gt(loaded.bytes_read, size_t{0}));
        expect(ge(loaded.worker_count, size_t{1}));
        expect(eq(restored.size(), size_t{6}));
        expect(deserializer.last_stats().loaded_count == loaded.loaded_count);

        // Guids survive, and the guid set is exactly the source's.
        expect(resolved_guids(restored, dag.vertices) == gtest::sorted_guid_text(dag.vertices))
            << "every source guid is present in the restored graph";
        for (auto *vertex : dag.vertices) {
            expect(restored.contains(vertex->guid()));
        }

        // Content bytes and cached hashes survive, byte for byte.
        for (auto *vertex : dag.vertices) {
            Vertex *copy = restored.find(vertex->guid());
            expect(copy != nullptr);
            if (copy == nullptr) {
                continue;
            }
            expect(bytes_equal(copy->content_copy(), vertex->content_copy()))
                << "the content blob survives the round trip";
            expect(copy->content_hash() == vertex->content_hash())
                << "the content hash survives the round trip";
        }
        expect(gtest::blob_equal(restored.find(dag.e->guid())->content_copy(), "e"))
            << "the isolated vertex's content is intact";

        // Both dependency directions survive.
        expect(gtest::sorted_guid_text(restored.dependencies(dag.a->guid())) ==
               gtest::sorted_guid_text(
                   kimix::vector<kimix::Guid>{dag.b->guid(), dag.c->guid()}))
            << "a still depends on b and c";
        expect(gtest::sorted_guid_text(restored.dependents(dag.d->guid())) ==
               gtest::sorted_guid_text(kimix::vector<kimix::Guid>{
                   dag.b->guid(), dag.c->guid(), dag.f->guid()}))
            << "d is still depended on by b, c and f";
        expect(eq(restored.dependencies(dag.b->guid()).size(), size_t{1}))
            << "b depends on d only";
        expect(restored.dependencies(dag.b->guid()).front() == dag.d->guid());
        expect(restored.dependencies(dag.e->guid()).empty()) << "e keeps no outgoing edge";
        expect(restored.dependents(dag.e->guid()).empty()) << "e keeps no incoming edge";

        // The traversals work on the restored graph, so the edges really are edges.
        expect(restored.has_path(dag.a->guid(), dag.d->guid()));
        expect(!restored.has_path(dag.d->guid(), dag.a->guid()));
        expect(eq(restored.shortest_path(dag.a->guid(), dag.d->guid()).size(), size_t{3}))
            << "a -> b -> d is the shortest path";

        // Roots are preserved (a root is a vertex nothing depends on).
        kimix::vector<kimix::Guid> restored_root_guids;
        for (auto *root : restored.roots()) {
            expect(root != nullptr);
            if (root != nullptr) {
                restored_root_guids.push_back(root->guid());
            }
        }
        expect(gtest::sorted_guid_text(restored_root_guids) ==
               gtest::sorted_guid_text(
                   kimix::vector<kimix::Guid>{dag.a->guid(), dag.e->guid(), dag.f->guid()}))
            << "the restored roots are exactly a, e and f";

        // A load leaves the vertices CLEAN: they match the store.
        expect(restored.dirty_vertices().empty())
            << "loaded vertices are clean, so an immediate incremental save is a no-op";
        GraphSerializer restored_serializer{cold};
        GraphSerializeStats noop = restored_serializer.save_incremental(restored);
        expect(eq(noop.saved_count, size_t{0})) << "nothing to write after a load";
        expect(eq(noop.bytes_written, size_t{0}));
    };

      "serialize_content_digest_policy_round_trip"_test = [] {
          // Plan v3 §4.5: the content digest is selectable (MD5 default, XXH3-128
          // behind GraphOptions::content_digest). Both fill the same 16-byte
          // record field, so the on-disk format is unchanged: a store written
          // under one policy reads cleanly under the other (the loader
          // RECOMPUTES the cached hash at materialization), and the record the
          // save writes carries the graph's own policy so the post-write
          // clear-dirty comparison keeps matching.
          using graph_db::ContentDigest;
          using graph_db::Database;
          using graph_db::GdiStatus;
          using graph_db::GraphOptions;
          using graph_db::LabelId;
          using graph_db::VertexCreateResult;
          // The helper: same bytes, two policies, different non-zero digests,
          // and the md5 policy reproduces kimix::MD5 exactly.
          auto const sample = gtest::to_bytes("the quick brown fox jumps over");
          kimix::span<const std::byte> const sample_span{sample.data(), sample.size()};
          kimix::MD5 const as_md5 = graph_db::content_digest_of(sample_span, ContentDigest::md5);
          kimix::MD5 const as_xxh3 = graph_db::content_digest_of(sample_span, ContentDigest::xxh3);
          expect(as_md5 == kimix::MD5{kimix::string_view{"the quick brown fox jumps over"}});
          expect(!(as_md5 == as_xxh3))
              << "the two policies produce different digests";
          expect(!(as_xxh3 == kimix::MD5{}))
              << "XXH3 of non-empty content is not the zero digest";

          // One store, built under XXH3, read back under both policies.
          gtest::temp_dir_guard dir;
          expect(dir.ok()) << "a temp directory must be creatable";
          if (!dir.ok()) {
              return;
          }
          LabelId node{};
          kimix::Guid uid;
          {
              Database db;
              GraphOptions options;
              options.content_digest = ContentDigest::xxh3;
              expect(eq(db.open(dir.path(), options), GdiStatus::ok));
              expect(eq(db.schema().create_label("node", node), GdiStatus::ok));
              VertexCreateResult created;
              auto const key = gtest::to_bytes("digest-policy");
              expect(eq(db.create_vertex(graph_db::VertexKeyView{
                            node, kimix::span<const std::byte>{key.data(), key.size()}},
                                       {}, nullptr, created),
                        GdiStatus::ok));
              uid = created.uid;
              expect(db.graph().update_content(uid, sample_span));
              expect(db.persist(/*full=*/true, nullptr) == GdiStatus::ok);
          }
          auto const read_back = [&](ContentDigest policy, kimix::MD5 &hash_out) {
              Database db;
              GraphOptions options;
              options.content_digest = policy;
              expect(eq(db.open(dir.path(), options), GdiStatus::ok));
              Vertex *vertex = db.graph().find(uid);
              expect(vertex != nullptr);
              kimix::vector<std::byte> content;
              vertex->content(content);
              expect(eq(content.size(), sample.size()));
              expect(std::equal(content.begin(), content.end(), sample.begin()))
                  << "content is byte-identical regardless of the digest policy";
              hash_out = vertex->content_hash();
              // The record the save wrote must carry the graph's own policy.
              GraphSerializeStats stats;
              expect(eq(db.persist(/*full=*/false, &stats), GdiStatus::ok));
              return stats;
          };
          kimix::MD5 xxh3_hash;
          GraphSerializeStats const under_xxh3 = read_back(ContentDigest::xxh3, xxh3_hash);
          expect(under_xxh3.saved_count == size_t{0})
              << "a clean xxh3 reopen writes nothing incrementally";
          expect(xxh3_hash == as_xxh3)
              << "the xxh3 cache matches the xxh3 digest of the content";
          kimix::MD5 md5_hash;
          GraphSerializeStats const under_md5 = read_back(ContentDigest::md5, md5_hash);
          expect(under_md5.saved_count == size_t{0})
              << "a clean md5 reopen also writes nothing (the loader recomputed the hash)";
          expect(md5_hash == as_md5)
              << "the md5-reopened vertex carries the recomputed md5 digest";
          expect(!(md5_hash == xxh3_hash));
      };

      "serialize_incremental_writes_only_dirty_records"_test = [] {
          gtest::temp_dir_guard dir;
          expect(dir.ok());
        if (!dir.ok()) {
            return;
        }

        Graph graph;
        kimix::vector<Vertex *> vertices = gtest::build_layered_dag(graph, 24);
        expect(eq(graph.size(), size_t{24}));

        ColdStorage cold;
        expect(cold.open(dir.path()));
        GraphSerializer serializer{cold};

        GraphSerializeStats first = serializer.save(graph);
        expect(eq(first.saved_count, size_t{24})) << "the first save writes the whole graph";
        auto snapshot = gtest::snapshot_store(cold);
        expect(eq(snapshot.size(), size_t{24}));

        // One content update, one new vertex and one new edge. Structural changes
        // (create_vertex / connect) do not set dirty flags — the caller marks the
        // affected vertices, which is what a real mutator does.
        Vertex *changed = vertices[7];
        Vertex *edge_source = vertices[3];
        Vertex *edge_target = vertices[19];
        auto new_content = gtest::to_bytes("changed-content");
        expect(graph.update_content(changed->guid(), new_content));
        expect(changed->is_dirty()) << "update_content marks the vertex dirty";

        Vertex *added = graph.create_vertex(gtest::to_bytes("added-later"));
        expect(graph.connect(added->guid(), edge_target->guid()));
        edge_source->mark_dirty(); // the new edge below changes its outgoing list
        expect(graph.connect(edge_source->guid(), added->guid()));
        added->mark_dirty();       // ... and the other endpoint's incoming list
        expect(eq(graph.dirty_vertices().size(), size_t{3}))
            << "exactly three vertices are dirty before the second save";

        GraphSerializeStats second = serializer.save_incremental(graph);
        expect(eq(second.saved_count, size_t{3}))
            << "incremental save writes exactly the dirty vertices";
        expect(eq(second.skipped_count, size_t{0}));
        expect(second.saved_count < first.saved_count)
            << "an incremental save writes fewer records than the full one";
        expect(second.bytes_written < first.bytes_written);

        // Every record that was NOT one of the three dirty vertices is still
        // bit-identical to the first-save snapshot: the write set is exact.
        auto after = gtest::snapshot_store(cold);
        expect(eq(after.size(), size_t{25})) << "the new vertex got a record too";
        size_t compared = 0;
        for (auto const &before_entry : snapshot) {
            bool const touched = before_entry.guid == changed->guid() ||
                                 before_entry.guid == edge_source->guid() ||
                                 before_entry.guid == added->guid();
            if (touched) {
                continue;
            }
            auto same_guid = std::find_if(after.begin(), after.end(),
                                          [&before_entry](gtest::raw_store_entry const &entry) {
                                              return entry.guid == before_entry.guid;
                                          });
            expect(same_guid != after.end()) << "every untouched record still exists";
            if (same_guid == after.end()) {
                continue;
            }
            expect(bytes_equal(same_guid->value, before_entry.value))
                << "an untouched vertex's record is bit-identical after an incremental save";
            ++compared;
        }
        expect(eq(compared, size_t{22})) << "22 of the 24 first-save records are untouched";

        // The rewritten records really changed and carry the new data.
        graph_db::VertexRecord decoded;
        expect(read_stored_record(cold, changed->guid(), decoded));
        expect(gtest::blob_equal(decoded.content, "changed-content"))
            << "the rewritten record holds the new content";
        expect(decoded.content_hash == graph_db::md5_of(decoded.content))
            << "the stored hash matches the stored content";

        expect(read_stored_record(cold, added->guid(), decoded));
        expect(gtest::blob_equal(decoded.content, "added-later"));
        expect(eq(decoded.depending_guids.size(), size_t{1}))
            << "the new outgoing edge is serialized on the new vertex";
        expect(decoded.depending_guids.front() == edge_target->guid());

        // A full save then rewrites everything, untouched vertices included.
        GraphSerializeStats third = serializer.save(graph);
        expect(eq(third.saved_count, size_t{25})) << "a full save writes every vertex";
        expect(ge(third.bytes_written, second.bytes_written));
        expect(eq(record_count(cold), size_t{25}));

        // Reload and check the incremental result is a faithful store.
        Graph restored;
        GraphDeserializer deserializer;
        GraphSerializeStats loaded = deserializer.load(cold, restored);
        expect(eq(loaded.loaded_count, size_t{25}));
        expect(eq(loaded.skipped_count, size_t{0}));
        expect(eq(loaded.skipped_edge_count, size_t{0}));
        expect(resolved_guids(restored, vertices) == gtest::sorted_guid_text(vertices))
            << "the original guid set survived both saves";

        Vertex *restored_added = restored.find(added->guid());
        expect(restored_added != nullptr);
        if (restored_added != nullptr) {
            expect(gtest::blob_equal(restored_added->content_copy(), "added-later"));
            auto restored_deps = restored.dependencies(added->guid());
            expect(eq(restored_deps.size(), size_t{1}));
            expect(restored_deps.front() == edge_target->guid());
            expect(eq(restored.dependents(added->guid()).size(), size_t{1}))
                << "edge_source -> added survived too";
            expect(restored.dependents(added->guid()).front() == edge_source->guid());
        }

        Vertex *restored_changed = restored.find(changed->guid());
        expect(restored_changed != nullptr);
        if (restored_changed != nullptr) {
            expect(gtest::blob_equal(restored_changed->content_copy(), "changed-content"));
            expect(restored_changed->content_hash() == changed->content_hash());
        }

        // The full adjacency of the source graph is reproduced exactly.
        kimix::vector<Vertex *> restored_vertices;
        restored_vertices.reserve(vertices.size());
        for (auto *vertex : vertices) {
            restored_vertices.push_back(restored.find(vertex->guid()));
        }
        expect(gtest::adjacency_lines(restored, restored_vertices) ==
               gtest::adjacency_lines(graph, vertices))
            << "the incremental path produces the same adjacency as the source";
    };

    "serialize_incremental_clean_graph_writes_nothing"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }

        Graph graph;
        gtest::small_dag(graph);
        ColdStorage cold;
        expect(cold.open(dir.path()));
        GraphSerializer serializer{cold};

        expect(eq(serializer.save(graph).saved_count, size_t{6}));
        auto snapshot = gtest::snapshot_store(cold);

        // Nothing changed: the second incremental save must not touch the store.
        GraphSerializeStats stats = serializer.save_incremental(graph);
        expect(eq(stats.saved_count, size_t{0}));
        expect(eq(stats.bytes_written, size_t{0}));
        expect(eq(stats.skipped_count, size_t{0}));

        auto rewritten = gtest::snapshot_store(cold);
        expect(eq(rewritten.size(), snapshot.size()));
        for (size_t i = 0; i < snapshot.size() && i < rewritten.size(); ++i) {
            expect(snapshot[i].guid == rewritten[i].guid);
            expect(bytes_equal(rewritten[i].value, snapshot[i].value))
                << "a clean graph leaves every stored byte alone";
        }
    };

    "serialize_dual_stage_cycle_and_dangling_deps"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }

        // Hand-craft the store: A <-> B is a dependency cycle, and C depends on a
        // guid that has no record at all. Nothing here goes through a Graph, so
        // Graph::connect() can never have refused the cycle while building it.
        kimix::Guid const a{true};
        kimix::Guid const b{true};
        kimix::Guid const c{true};
        kimix::Guid const missing{true};

        ColdStorage cold;
        expect(cold.open(dir.path()));
        expect(gtest::write_record(cold, gtest::make_record(a, gtest::to_bytes("A"), {b}, {})));
        expect(gtest::write_record(cold, gtest::make_record(b, gtest::to_bytes("B"), {a}, {})));
        expect(gtest::write_record(
            cold, gtest::make_record(c, gtest::to_bytes("C"), {missing, a}, {})));
        expect(eq(record_count(cold), size_t{3}));

        Graph graph;
        GraphDeserializer deserializer;
        GraphSerializeStats stats = deserializer.load(cold, graph);

        expect(eq(stats.loaded_count, size_t{3}))
            << "a dependency cycle or a dangling guid never costs a vertex";
        expect(eq(stats.skipped_count, size_t{0})) << "every record itself was valid";
        expect(eq(stats.skipped_edge_count, size_t{1}))
            << "exactly the C -> missing edge is unresolvable";
        expect(eq(graph.size(), size_t{3}));

        expect(graph.contains(a));
        expect(graph.contains(b));
        expect(graph.contains(c));
        expect(!graph.contains(missing)) << "the dangling guid never materializes";

        // The cycle is resolvable, so BOTH of its edges are real edges.
        expect(eq(graph.dependencies(a).size(), size_t{1}));
        expect(graph.dependencies(a).front() == b);
        expect(eq(graph.dependencies(b).size(), size_t{1}));
        expect(graph.dependencies(b).front() == a);
        expect(graph.has_path(a, b));
        expect(graph.has_path(b, a)) << "a cycle is a valid dependency structure";

        // C keeps only its resolvable edge, and its reverse side is rebuilt.
        auto c_dependencies = graph.dependencies(c);
        expect(eq(c_dependencies.size(), size_t{1}));
        expect(c_dependencies.front() == a) << "C -> A resolves, C -> missing does not";
        auto a_dependents = gtest::sorted_guid_text(graph.dependents(a));
        expect(a_dependents == gtest::sorted_guid_text(kimix::vector<kimix::Guid>{b, c}))
            << "a is depended on by both the cycle edge and C";

        // Content survived for all three.
        expect(gtest::blob_equal(graph.find(a)->content_copy(), "A"));
        expect(gtest::blob_equal(graph.find(b)->content_copy(), "B"));
        expect(gtest::blob_equal(graph.find(c)->content_copy(), "C"));

        // A satisfied load leaves the graph clean and immediately re-savable.
        expect(graph.dirty_vertices().empty());
        GraphSerializer serializer{cold};
        expect(eq(serializer.save_incremental(graph).saved_count, size_t{0}));

        // The reloaded graph still traverses the cycle without hanging.
        expect(graph.has_path(a, a)) << "a vertex always reaches itself";
    };

    "serialize_dual_stage_reverse_edges_rebuilt"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }

        // A store written with only the outgoing direction: the link pass must
        // rebuild the reverse edges from the outgoing ones (the outgoing list is
        // the authoritative form; the reverse list is its cache).
        kimix::Guid const from{true};
        kimix::Guid const to{true};
        ColdStorage cold;
        expect(cold.open(dir.path()));
        expect(gtest::write_record(
            cold, gtest::make_record(from, gtest::to_bytes("from"), {to}, {})));
        expect(gtest::write_record(cold, gtest::make_record(to, gtest::to_bytes("to"), {}, {})));

        Graph graph;
        GraphDeserializer deserializer;
        GraphSerializeStats stats = deserializer.load(cold, graph);
        expect(eq(stats.loaded_count, size_t{2}));
        expect(eq(stats.skipped_edge_count, size_t{0}));
        expect(eq(graph.dependencies(from).size(), size_t{1}));
        expect(eq(graph.dependents(to).size(), size_t{1}))
            << "the reverse edge is rebuilt by the link pass";
        expect(graph.dependents(to).front() == from);
        expect(graph.dependents(from).empty());
    };

    "serialize_parallel_determinism"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }

        Graph source;
        kimix::vector<Vertex *> vertices = gtest::build_layered_dag(source, 64);
        ColdStorage cold;
        expect(cold.open(dir.path()));
        GraphSerializer serializer{cold};
        expect(eq(serializer.save(source).saved_count, size_t{64}));

        // Two independent loads of the SAME store, each with its own fan-out.
        Graph first;
        GraphDeserializer deserializer;
        expect(eq(deserializer.load(cold, first).loaded_count, size_t{64}));
        Graph second;
        expect(eq(deserializer.load(cold, second).loaded_count, size_t{64}));
        expect(eq(first.size(), second.size()));

        kimix::vector<Vertex *> first_vertices;
        kimix::vector<Vertex *> second_vertices;
        first_vertices.reserve(vertices.size());
        second_vertices.reserve(vertices.size());
        for (auto *vertex : vertices) {
            first_vertices.push_back(first.find(vertex->guid()));
            second_vertices.push_back(second.find(vertex->guid()));
        }

        // Identical guid sets ...
        auto first_guids = gtest::sorted_guid_text(first_vertices);
        expect(first_guids == gtest::sorted_guid_text(second_vertices))
            << "both loads materialized the same guid set";
        expect(first_guids == gtest::sorted_guid_text(vertices));

        // ... and identical adjacency in BOTH directions.
        auto first_adjacency = gtest::adjacency_lines(first, first_vertices);
        auto second_adjacency = gtest::adjacency_lines(second, second_vertices);
        expect(eq(first_adjacency.size(), size_t{64}));
        expect(first_adjacency == second_adjacency)
            << "the parallel fan-out is deterministic";

        // Cross-check against the source: the load neither invented nor dropped
        // an edge.
        expect(first_adjacency == gtest::adjacency_lines(source, vertices))
            << "the restored adjacency equals the source adjacency";

        // A fresh ColdStorage over the same directory loads the same too.
        ColdStorage reopened;
        expect(reopened.open(dir.path()));
        Graph third;
        GraphDeserializer third_deserializer;
        expect(eq(third_deserializer.load(reopened, third).loaded_count, size_t{64}));
        kimix::vector<Vertex *> third_vertices;
        third_vertices.reserve(vertices.size());
        for (auto *vertex : vertices) {
            third_vertices.push_back(third.find(vertex->guid()));
        }
        expect(gtest::adjacency_lines(third, third_vertices) == first_adjacency);
    };

    "serialize_corrupt_record_is_skipped"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }

        Graph source;
        gtest::known_dag dag = gtest::small_dag(source);
        ColdStorage cold;
        expect(cold.open(dir.path()));
        GraphSerializer serializer{cold};
        expect(eq(serializer.save(source).saved_count, size_t{6}));

        // Overwrite one good record with bytes that are not a record at all, and
        // add a second "v/" key holding a value that is too short to be one.
        expect(gtest::write_raw_value(cold, dag.c->guid(),
                                      gtest::to_bytes("this-is-not-a-vertex-record")));
        kimix::Guid const junk_guid{true};
        auto const short_value = gtest::to_bytes("GDBshort");
        expect(gtest::write_raw_value(cold, junk_guid, short_value));
        expect(eq(record_count(cold), size_t{7}));

        Graph restored;
        GraphDeserializer deserializer;
        GraphSerializeStats stats = deserializer.load(cold, restored);

        expect(eq(stats.loaded_count, size_t{5}))
            << "the two corrupt records are dropped, the five good ones load";
        expect(eq(stats.skipped_count, size_t{2})) << "both corrupt records are counted";
        expect(eq(restored.size(), size_t{5}));
        expect(!restored.contains(dag.c->guid())) << "the corrupted vertex is gone";
        expect(!restored.contains(junk_guid));

        // The good vertices are intact, content and hash included.
        for (auto *vertex : {dag.a, dag.b, dag.d, dag.e, dag.f}) {
            expect(restored.contains(vertex->guid()));
            Vertex *copy = restored.find(vertex->guid());
            expect(copy != nullptr);
            if (copy == nullptr) {
                continue;
            }
            expect(copy->content_hash() == vertex->content_hash())
                << "an intact record keeps its content and hash";
        }

            // c's record is the corrupt one, so the only reference to it that
            // survives from the VERTEX side is a's outgoing link.
            auto a_dependencies = restored.dependencies(dag.a->guid());
            expect(eq(a_dependencies.size(), size_t{1})) << "a keeps only its edge to b";
            expect(a_dependencies.front() == dag.b->guid());
            // TWO edges are now unresolvable, not one: a -> c (a's link, which
            // needs c) and c -> d (c's own EDGE record, which also needs c). An
            // edge is a record of its own since epoch 4, so it survives the loss
            // of its endpoints' vertex records and is reported on its own — the
            // v1 model could not even see the second one, because it only ever
            // knew edges as entries inside a vertex record. Both are counted once
            // (the link fallback skips a pair an edge record already reported).
            expect(eq(stats.skipped_edge_count, size_t{2}));

        // The surviving part of the DAG still traverses.
        expect(restored.has_path(dag.a->guid(), dag.d->guid()));
        expect(gtest::sorted_guid_text(restored.dependents(dag.d->guid())) ==
               gtest::sorted_guid_text(
                   kimix::vector<kimix::Guid>{dag.b->guid(), dag.f->guid()}));

        // A corrupt record never fails the load: the result is re-savable.
        GraphSerializeStats resaved = serializer.save_incremental(restored);
        expect(eq(resaved.saved_count, size_t{0}))
            << "the loaded vertices are clean, so nothing is rewritten";
        expect(eq(resaved.skipped_count, size_t{0}));
    };

    "serialize_record_key_guid_mismatch_is_skipped"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }

        // A perfectly valid record stored under ANOTHER vertex's key: adopting it
        // would let one record impersonate a different vertex, so it is dropped.
        kimix::Guid const record_guid{true};
        kimix::Guid const key_guid{true};
        ColdStorage cold;
        expect(cold.open(dir.path()));
        kimix::vector<std::byte> encoded;
        graph_db::encode_vertex_record(
            gtest::make_record(record_guid, gtest::to_bytes("impostor")), encoded);
        expect(gtest::write_raw_value(cold, key_guid, encoded));

        // ... alongside one honest record.
        kimix::Guid const honest{true};
        expect(gtest::write_record(cold, gtest::make_record(honest, gtest::to_bytes("honest"))));

        Graph graph;
        GraphDeserializer deserializer;
        GraphSerializeStats stats = deserializer.load(cold, graph);
        expect(eq(stats.loaded_count, size_t{1}));
        expect(eq(stats.skipped_count, size_t{1}));
        expect(graph.contains(honest));
        expect(!graph.contains(record_guid)) << "the key wins over the record's own guid";
        expect(!graph.contains(key_guid));
    };

    "serialize_load_does_not_overwrite_existing_vertex"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }

        Graph source;
        gtest::known_dag dag = gtest::small_dag(source);
        ColdStorage cold;
        expect(cold.open(dir.path()));
        GraphSerializer serializer{cold};
        expect(eq(serializer.save(source).saved_count, size_t{6}));

        // A target graph that already holds one of the saved guids: a load is not
        // a merge, so that record is skipped and the in-memory vertex is kept.
        Graph target;
        kimix::unique_ptr<Vertex> existing{
            new Vertex(dag.d->guid(), gtest::to_bytes("already-here"))};
        expect(target.add_vertex(std::move(existing)));

        GraphDeserializer deserializer;
        GraphSerializeStats stats = deserializer.load(cold, target);
        expect(eq(stats.loaded_count, size_t{5}));
        expect(eq(stats.skipped_count, size_t{1}));
        expect(gtest::blob_equal(target.find(dag.d->guid())->content_copy(), "already-here"))
            << "the existing vertex is never overwritten by a load";
        // d stayed in place, so every incoming edge to it still resolves: the
        // pre-existing vertex is a valid edge target.
        expect(eq(target.dependents(dag.d->guid()).size(), size_t{3}))
            << "b, c and f all point at the pre-existing d";
        expect(eq(stats.skipped_edge_count, size_t{0}))
            << "a skipped record is not a skipped edge when the guid is in the graph";
        expect(eq(target.size(), size_t{6}));
    };

    "serialize_empty_store_and_empty_graph"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }

        ColdStorage cold;
        expect(cold.open(dir.path()));

        // Empty graph -> empty store -> empty graph: no warnings, no failures.
        Graph graph;
        GraphSerializer serializer{cold};
        GraphSerializeStats saved = serializer.save(graph);
        expect(eq(saved.saved_count, size_t{0}));
        expect(eq(saved.bytes_written, size_t{0}));
        expect(eq(record_count(cold), size_t{0}));

        Graph restored;
        GraphDeserializer deserializer;
        GraphSerializeStats loaded = deserializer.load(cold, restored);
        expect(eq(loaded.loaded_count, size_t{0}));
        expect(eq(loaded.skipped_count, size_t{0}));
        expect(eq(loaded.skipped_edge_count, size_t{0}));
        expect(restored.empty());

        // A closed store reports empty stats instead of failing.
        ColdStorage closed;
        GraphSerializer closed_serializer{closed};
        expect(!closed_serializer.valid());
        expect(eq(closed_serializer.save(graph).saved_count, size_t{0}));
        GraphDeserializer closed_deserializer;
        expect(eq(closed_deserializer.load(closed, restored).loaded_count, size_t{0}));
        expect(restored.empty()) << "a failed load leaves the target graph alone";
    };

    "serialize_unbound_thread_runs_inline"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }

        // The serializer documents that a thread with NO scheduler bound falls back
        // to kimix::fiber::parallel()'s inline path. A std::thread has no binding,
        // so this worker exercises exactly that: the whole save + load round trip
        // runs there and must produce the same result as the bound path. The worker
        // touches its OWN graph and its OWN store (the single-writer contract is per
        // graph), and only plain values cross back to main, where the assertions run.
        struct outcome {
            bool bound{true};
            bool round_tripped{false};
            size_t saved{0};
            size_t loaded{0};
            size_t skipped{0};
            size_t skipped_edges{0};
            size_t restored_size{0};
        } result;
        kimix::filesystem::path const store_dir = dir.path();

        std::thread worker{[&result, store_dir] {
            result.bound = kimix::fiber::is_bound();
            ColdStorage cold;
            if (!cold.open(store_dir)) {
                return;
            }
            Graph graph;
            gtest::known_dag dag = gtest::small_dag(graph);
            GraphSerializer serializer{cold};
            result.saved = serializer.save(graph).saved_count;

            Graph restored;
            GraphDeserializer deserializer;
            GraphSerializeStats stats = deserializer.load(cold, restored);
            result.loaded = stats.loaded_count;
            result.skipped = stats.skipped_count;
            result.skipped_edges = stats.skipped_edge_count;
            result.restored_size = restored.size();
            bool ok = restored.size() == 6;
            for (auto *vertex : dag.vertices) {
                Vertex *copy = restored.find(vertex->guid());
                if (copy == nullptr || copy->content_hash() != vertex->content_hash()) {
                    ok = false;
                }
            }
            if (restored.dependencies(dag.a->guid()).size() != 2) {
                ok = false;
            }
            result.round_tripped = ok;
        }};
        worker.join();

        expect(!result.bound)
            << "the worker must be an unbound thread for the inline path to be taken";
        expect(eq(result.saved, size_t{6})) << "the inline save writes every vertex";
        expect(eq(result.loaded, size_t{6})) << "the inline load adopts every vertex";
        expect(eq(result.skipped, size_t{0}));
        expect(eq(result.skipped_edges, size_t{0}));
        expect(eq(result.restored_size, size_t{6}));
        expect(result.round_tripped)
            << "an unbound thread round-trips the graph through the inline path";
    };

    "serialize_copy_to_checkpoint"_test = [] {
        gtest::temp_dir_guard dir;
        gtest::temp_dir_guard checkpoint;
        expect(dir.ok());
        expect(checkpoint.ok());
        if (!dir.ok() || !checkpoint.ok()) {
            return;
        }

        Graph source;
        gtest::known_dag dag = gtest::small_dag(source);
        ColdStorage cold;
        expect(cold.open(dir.path()));
        GraphSerializer serializer{cold};
        expect(eq(serializer.save(source).saved_count, size_t{6}));
        expect(serializer.copy_to(checkpoint.path())) << "copy_to checkpoints the store";

        // The checkpoint is a usable store of its own.
        ColdStorage copied;
        expect(copied.open(checkpoint.path()));
        expect(eq(record_count(copied), size_t{6}));
        Graph restored;
        GraphDeserializer deserializer;
        GraphSerializeStats stats = deserializer.load(copied, restored);
        expect(eq(stats.loaded_count, size_t{6}));
        expect(eq(stats.skipped_edge_count, size_t{0}));
        expect(resolved_guids(restored, dag.vertices) == gtest::sorted_guid_text(dag.vertices));
        expect(gtest::adjacency_lines(restored, dag.vertices) ==
               gtest::adjacency_lines(source, dag.vertices))
            << "the checkpoint reproduces the whole graph, edges included";
    };

    "serialize_string_array_vertex_and_edge_reopen"_test = [] {
        // ABI 7 §1.1: a string_array on a VERTEX and on an EDGE must survive
        // persist -> close -> reopen -> read-back byte-identically, on BOTH cold
        // stores. Two separate directories: each backend owns its own layout.
        gtest::temp_dir_guard lmdb_dir;
        gtest::temp_dir_guard file_dir;
        expect(lmdb_dir.ok()) << "a temp directory must be creatable";
        expect(file_dir.ok());
        if (!lmdb_dir.ok() || !file_dir.ok()) {
            return;
        }
        {
            string_array_graph source;
            fill_string_array_graph(source);
            expect(eq(source.graph.size(), size_t{2}));
            graph_db::EdgeRecord edge;
            expect(source.graph.edge_record(source.edge, edge))
                << "the edge exists and carries its own property list";
            expect(eq(edge.properties.size(), size_t{1}));
            check_string_array_reopens<ColdStorage>("lmdb", lmdb_dir.path(), source);
        }
        {
            string_array_graph source;
            fill_string_array_graph(source);
            check_string_array_reopens<FileColdStore>("flat_file", file_dir.path(), source);
        }
    };

    "serialize_corrupt_string_array_block_is_refused"_test = [] {
        // The block decoder is STRICT, and a store whose payload was damaged on the
        // way in is refused rather than trusted: the record decode fails, the
        // deserializer drops exactly that record, counts it, and loads the rest —
        // the same posture every other corrupt-record leg here proves (at the
        // Database layer that refusal is what surfaces as error_bad_file).
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }

        ColdStorage cold;
        expect(cold.open(dir.path()));
        string_array_graph source;
        fill_string_array_graph(source);
        GraphSerializer serializer{cold};
        GraphSerializeStats const saved = serializer.save(source.graph);
        expect(eq(saved.saved_count, size_t{2}));
        expect(eq(saved.saved_edge_count, size_t{1}));

        // Control first: the store is loadable as written, so the refusal below is
        // about the damaged bytes and not about the setup.
        Graph control;
        GraphDeserializer probe;
        expect(eq(probe.load(cold, control).loaded_count, size_t{2}))
            << "the untouched store loads both vertices";

        // Locate the §1.1 block inside a's stored record and damage ONE byte: the
        // high byte of the FIRST element's u32 length field (4 bytes of count, then
        // bytes 4..7 of the block), so the element claims some 4 GiB of text.
        auto const value = gtest::stored_value(cold, source.a);
        expect(!value.empty()) << "a is the vertex that carries the array";
        kimix::span<const std::byte> const record{value.data(), value.size()};
        auto const block = source.expected_vertex.as_string_array_block();
        size_t const at = find_span(record, block);
        expect(at != record.size()) << "the stored record holds the block verbatim";
        auto damaged = value;
        damaged[at + 7] = static_cast<std::byte>(0xff);
        expect(!bytes_equal(kimix::span<const std::byte>{damaged.data(), damaged.size()}, record))
            << "the damage landed inside the payload";
        expect(gtest::write_raw_value(
            cold, source.a, kimix::span<const std::byte>{damaged.data(), damaged.size()}));

        // The record codec refuses it outright, leaving nothing half-decoded.
        VertexRecordV2 decoded;
        expect(!graph_db::decode_vertex_record_v2(
            kimix::span<const std::byte>{damaged.data(), damaged.size()}, decoded))
            << "a corrupt string_array block fails the record decode";
        expect(decoded.properties.empty()) << "and the decoded record stays empty";

        // The load drops only the damaged record, and its edge becomes the
        // unresolvable one.
        Graph restored;
        GraphDeserializer deserializer;
        GraphSerializeStats const stats = deserializer.load(cold, restored);
        expect(eq(stats.loaded_count, size_t{1})) << "the intact vertex still loads";
        expect(eq(stats.skipped_count, size_t{1})) << "the corrupt record is counted";
        expect(!restored.contains(source.a)) << "the vertex with the damaged block is gone";
        expect(restored.contains(source.b)) << "its neighbour survives";
            expect(eq(stats.skipped_edge_count, size_t{1}))
                << "and the edge that needed it cannot resolve";
    };
}