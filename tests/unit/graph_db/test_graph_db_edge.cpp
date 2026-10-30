// Test for first-class edges (src/graph_db/edge.*, Graph::edge_*, Database edge
// commands) — GDI §2.4 / §2.14.
//
// This test covers:
// - edge identity: create -> endpoints -> free -> error_uid on a second free;
// - PARALLEL edges between one ordered pair: three edges, three adjacency links,
//   one neighbour from a neighbour read, and freeing one leaves two;
// - direction: directed vs undirected x {outgoing, incoming, both} as an exact
//   table (an undirected edge is incident in both directions);
// - endpoint mutation: moving the origin moves the adjacency and nothing else,
//   creating and collapsing a self loop;
// - the self-loop option (refused when allow_self_edges is off);
// - cascade: a vertex with two outgoing and two incoming edges loses all four
//   when it is freed, and the neighbours' adjacency is clean;
// - persistence: edge ids, direction, endpoints, labels and properties survive a
//   save/load round trip, and an incremental save of one changed edge writes one
//   record;
// - the dirty EDGE index (EdgeDirtySink): create/free/move/set-label/set-property/
//   adopt/erase/re-insert/clear_dirty are each cross-checked against the full O(E)
//   shard scan, so no record can silently drop out of the next incremental save;
// - the LEGACY connect/connect_bulk contract (one edge per ordered pair, no self
//   loops, duplicate refusal) is unchanged by the new model.
#include "ut/ut.hpp"

#include "graph_db_test_util.h"

#include <core/kimix_core.h> // umbrella first: it fixes the winsock2.h order
#include <core/fiber.h>

#include <graph_db/database.h>
#include <graph_db/edge.h>
#include <graph_db/graph_db.h>
#include <graph_db/status.h>

#include <core/guid.h>
#include <core/stl/string.h>
#include <core/stl/unordered_map.h> // kimix::unordered_set: the dirty-index cross-check
#include <core/stl/vector.h>

#include <algorithm>
#include <cstddef>
#include <utility>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using graph_db::Database;
using graph_db::EdgeBulkRow;
using graph_db::EdgeDirectionType;
using graph_db::EdgeOrientation;
using graph_db::EdgePage;
using graph_db::EdgeRecord;
using graph_db::GdiDataType;
using graph_db::GdiStatus;
using graph_db::Graph;
using graph_db::GraphSerializeStats;
using graph_db::LabelId;
using graph_db::NamedPropertyList;
using graph_db::PropNameId;
using graph_db::PropertyEntry;
using graph_db::PropertyList;
using graph_db::PropertyValue;
using graph_db::VertexCreateResult;
using graph_db::VertexKeyView;
using graph_db::VertexPage;
namespace gtest = kimix::graph_db::test;

namespace tests_unit_graph_db_test_graph_db_edge {

inline VertexKeyView view_of(LabelId label, kimix::vector<std::byte> const &bytes) {
    return VertexKeyView{label, kimix::span<const std::byte>{bytes.data(), bytes.size()}};
}

// A fixture with the labels/property types the edge cases need.
struct fixture {
    Database db;
    LabelId node{};
    LabelId link{};
    PropNameId weight{};

    fixture() {
        expect(eq(db.schema().create_label("node", node), GdiStatus::ok));
        expect(eq(db.schema().create_label("link", link), GdiStatus::ok));
        graph_db::PropertyTypeInfo info;
        info.name = "weight";
        info.data = GdiDataType::double_;
        expect(eq(db.create_property_type(info, weight), GdiStatus::ok));
    }

    Guid vertex(kimix::string_view external) {
        auto const bytes = gtest::to_bytes(external);
        VertexCreateResult created;
        expect(eq(db.create_vertex(view_of(node, bytes), {}, nullptr, created), GdiStatus::ok));
        return created.uid;
    }
};

// The edge ids incident to `uid` in one orientation, sorted, for exact matrix
// comparisons.
kimix::vector<kimix::string> incident_text(Database const &db, Guid const &uid,
                                           EdgeOrientation orientation) {
    kimix::vector<Guid> ids;
    db.graph().incident_edges(uid, orientation, ids);
    return gtest::sorted_guid_text(ids);
}

} // namespace tests_unit_graph_db_test_graph_db_edge
using namespace tests_unit_graph_db_test_graph_db_edge;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    kimix::fiber::scheduler sched{4u};

    "edge_identity"_test = [] {
        fixture f;
        Guid const a = f.vertex("a");
        Guid const b = f.vertex("b");
        Guid edge;
        expect(eq(f.db.edge_create(a, b, EdgeDirectionType::directed, {}, nullptr, nullptr, edge),
                  GdiStatus::ok));
        expect(static_cast<bool>(edge)) << "a fresh edge gets a real uid";

        Guid origin;
        Guid target;
        expect(eq(f.db.get_edge_endpoints(edge, origin, target), GdiStatus::ok));
        expect(origin == a);
        expect(target == b);

        EdgeRecord record;
        expect(f.db.graph().edge_record(edge, record));
        expect(record.id == edge);
        expect(record.origin == a);
        expect(record.target == b);
        expect(record.direction == EdgeDirectionType::directed);
        expect(!record.is_self_loop());

        expect(eq(f.db.edge_free(edge), GdiStatus::ok));
        expect(eq(f.db.edge_free(edge), GdiStatus::error_uid))
            << "a second free reports ErrorUid";
        expect(eq(f.db.graph().edges().size(), size_t{0}));
        expect(!f.db.graph().edge_record(edge, record));
        // The endpoints are untouched by an edge removal.
        expect(f.db.graph().contains(a));
        expect(f.db.graph().contains(b));
        expect(incident_text(f.db, a, EdgeOrientation::both).empty());
    };

    "edge_parallel_multi_edges"_test = [] {
        fixture f;
        Guid const a = f.vertex("a");
        Guid const b = f.vertex("b");

        kimix::vector<Guid> edges;
        for (int i = 0; i < 3; ++i) {
            Guid edge;
            expect(eq(f.db.edge_create(a, b, EdgeDirectionType::directed, {}, nullptr, nullptr, edge),
                      GdiStatus::ok));
            edges.push_back(edge);
        }
        // Three adjacency entries on each side, three distinct ids.
        kimix::vector<Guid> out_ids;
        f.db.graph().incident_edges(a, EdgeOrientation::outgoing, out_ids);
        expect(eq(out_ids.size(), size_t{3})) << "parallel edges are three edges";
        kimix::vector<Guid> in_ids;
        f.db.graph().incident_edges(b, EdgeOrientation::incoming, in_ids);
        expect(eq(in_ids.size(), size_t{3}));

        // A neighbour read collapses them to ONE neighbour.
        VertexPage page;
        expect(eq(f.db.get_neighbor_vertices(a, graph_db::Constraint{},
                                             EdgeOrientation::outgoing, 0, page),
                  GdiStatus::ok));
        expect(eq(page.total_count, size_t{1}))
            << "multi-edges do not multiply neighbours (reachability is unchanged)";
        // The edge read returns all three.
        EdgePage edge_page;
        expect(eq(f.db.get_edges_of_vertex(a, graph_db::Constraint{},
                                           EdgeOrientation::outgoing, 0, edge_page),
                  GdiStatus::ok));
        expect(eq(edge_page.total_count, size_t{3}));

        // Freeing ONE leaves two.
        expect(eq(f.db.edge_free(edges[1]), GdiStatus::ok));
        expect(eq(f.db.graph().edges().size(), size_t{2}));
        expect(eq(f.db.count_neighbors(a, graph_db::Constraint{},
                                       EdgeOrientation::outgoing, page.total_count),
                  GdiStatus::ok));
        expect(eq(page.total_count, size_t{1}));
        f.db.graph().incident_edges(a, EdgeOrientation::outgoing, out_ids);
        expect(eq(out_ids.size(), size_t{2}));
    };

    "edge_direction_and_orientation_matrix"_test = [] {
        fixture f;
        Guid const hub = f.vertex("hub");
        Guid const out = f.vertex("out");
        Guid const in = f.vertex("in");
        Guid const both = f.vertex("both");

        Guid out_edge;
        Guid in_edge;
        Guid both_edge;
        expect(eq(f.db.edge_create(hub, out, EdgeDirectionType::directed, {}, nullptr, nullptr, out_edge),
                  GdiStatus::ok));
        expect(eq(f.db.edge_create(in, hub, EdgeDirectionType::directed, {}, nullptr, nullptr, in_edge),
                  GdiStatus::ok));
        expect(eq(f.db.edge_create(hub, both, EdgeDirectionType::undirected, {}, nullptr, nullptr,
                                   both_edge),
                  GdiStatus::ok));

        // Outgoing: the hub is the origin (the directed one and the undirected one).
        auto const outgoing = incident_text(f.db, hub, EdgeOrientation::outgoing);
        auto const expected_outgoing = gtest::sorted_guid_text(
            kimix::vector<Guid>{out_edge, both_edge});
        expect(outgoing == expected_outgoing);

        // Incoming: the hub is the target.
        auto const incoming = incident_text(f.db, hub, EdgeOrientation::incoming);
        auto const expected_incoming = gtest::sorted_guid_text(kimix::vector<Guid>{in_edge});
        expect(incoming == expected_incoming);

        // Both: the union.
        auto const all = incident_text(f.db, hub, EdgeOrientation::both);
        auto const expected_all =
            gtest::sorted_guid_text(kimix::vector<Guid>{out_edge, in_edge, both_edge});
        expect(all == expected_all);

        // An undirected edge still has an origin/target, and INCIDENCE follows those
        // endpoints exactly like the reference backend's IsIncident (which does not
        // special-case the direction flag): the edge above is outgoing for the hub
        // and incoming for `both`, never both at once.
        Guid origin;
        Guid target;
        expect(eq(f.db.get_edge_endpoints(both_edge, origin, target), GdiStatus::ok));
        expect(origin == hub);
        expect(target == both);

        // Toggling the direction changes no adjacency, only the flag.
        expect(eq(f.db.set_edge_direction(out_edge, EdgeDirectionType::undirected), GdiStatus::ok));
        EdgeRecord record;
        expect(f.db.graph().edge_record(out_edge, record));
        expect(record.direction == EdgeDirectionType::undirected);
        expect(incident_text(f.db, hub, EdgeOrientation::outgoing) == expected_outgoing)
            << "direction is a property of the edge, not of the adjacency";
        expect(eq(f.db.set_edge_direction(out_edge, static_cast<EdgeDirectionType>(7)),
                  GdiStatus::error_edge_orientation));
    };

    "edge_endpoint_mutation"_test = [] {
        fixture f;
        Guid const a = f.vertex("a");
        Guid const b = f.vertex("b");
        Guid const c = f.vertex("c");
        Guid edge;
        expect(eq(f.db.edge_create(a, b, EdgeDirectionType::directed, {}, nullptr, nullptr, edge),
                  GdiStatus::ok));

        // Move the origin a -> c: a's outgoing list loses it, c's gains it, b only
        // keeps its incoming link.
        expect(eq(f.db.set_edge_origin(edge, c), GdiStatus::ok));
        Guid origin;
        Guid target;
        expect(eq(f.db.get_edge_endpoints(edge, origin, target), GdiStatus::ok));
        expect(origin == c);
        expect(target == b);
        expect(incident_text(f.db, a, EdgeOrientation::outgoing).empty());
        expect(eq(incident_text(f.db, c, EdgeOrientation::outgoing).size(), size_t{1}));
        expect(eq(incident_text(f.db, b, EdgeOrientation::incoming).size(), size_t{1}));

        // Move the target b -> a.
        expect(eq(f.db.set_edge_target(edge, a), GdiStatus::ok));
        expect(eq(f.db.get_edge_endpoints(edge, origin, target), GdiStatus::ok));
        expect(origin == c);
        expect(target == a);
        expect(incident_text(f.db, b, EdgeOrientation::incoming).empty());
        expect(eq(incident_text(f.db, a, EdgeOrientation::incoming).size(), size_t{1}));

        // A missing endpoint is ErrorVertex and moves nothing.
        Guid const ghost{true};
        expect(eq(f.db.set_edge_origin(edge, ghost), GdiStatus::error_vertex));
        expect(eq(f.db.get_edge_endpoints(edge, origin, target), GdiStatus::ok));
        expect(origin == c) << "a refused move leaves the edge where it was";

        // Self loop: c -> c, which collapses both links onto one vertex.
        expect(eq(f.db.set_edge_target(edge, c), GdiStatus::ok));
        expect(eq(f.db.get_edge_endpoints(edge, origin, target), GdiStatus::ok));
        expect(origin == c);
        expect(target == c);
        expect(eq(incident_text(f.db, c, EdgeOrientation::outgoing).size(), size_t{1}));
        expect(eq(incident_text(f.db, c, EdgeOrientation::incoming).size(), size_t{1}));
        expect(incident_text(f.db, a, EdgeOrientation::both).empty());
        expect(eq(f.db.graph().edges().size(), size_t{1}));
    };

    "edge_self_loop_option"_test = [] {
        fixture f;
        Guid const a = f.vertex("a");

        graph_db::GraphOptions options;
        options.allow_self_edges = false;
        f.db.set_options(options);
        Guid blocked;
        expect(eq(f.db.edge_create(a, a, EdgeDirectionType::directed, {}, nullptr, nullptr, blocked),
                  GdiStatus::error_edge))
            << "allow_self_edges = false refuses the loop";

        options.allow_self_edges = true;
        f.db.set_options(options);
        Guid allowed;
        expect(eq(f.db.edge_create(a, a, EdgeDirectionType::directed, {}, nullptr, nullptr, allowed),
                  GdiStatus::ok));
        expect(eq(f.db.graph().edges().size(), size_t{1}));
        // A self loop is ONE adjacency entry on each side of the same vertex.
        expect(eq(incident_text(f.db, a, EdgeOrientation::both).size(), size_t{2}))
            << "the loop is its own incoming and outgoing edge";
    };

    "edge_cascade_free_vertex"_test = [] {
        fixture f;
        Guid const victim = f.vertex("victim");
        Guid const out1 = f.vertex("out1");
        Guid const out2 = f.vertex("out2");
        Guid const in1 = f.vertex("in1");
        Guid const in2 = f.vertex("in2");

        kimix::vector<Guid> edges;
        for (auto const &target : {out1, out2}) {
            Guid edge;
            expect(eq(f.db.edge_create(victim, target, EdgeDirectionType::directed, {}, nullptr, nullptr,
                                       edge),
                      GdiStatus::ok));
            edges.push_back(edge);
        }
        for (auto const &source : {in1, in2}) {
            Guid edge;
            expect(eq(f.db.edge_create(source, victim, EdgeDirectionType::directed, {}, nullptr, nullptr,
                                       edge),
                      GdiStatus::ok));
            edges.push_back(edge);
        }
        expect(eq(f.db.graph().edges().size(), size_t{4}));

        expect(eq(f.db.free_vertex(victim), GdiStatus::ok)) << "FreeVertex cascades";
        expect(eq(f.db.graph().edges().size(), size_t{0})) << "every incident edge is gone";
        for (auto const &edge : edges) {
            EdgeRecord record;
            expect(!f.db.graph().edge_record(edge, record))
                << "a cascaded edge is not addressable any more";
            expect(eq(f.db.edge_free(edge), GdiStatus::error_uid));
        }
        // The neighbours' adjacency is clean.
        for (auto const &other : {out1, out2, in1, in2}) {
            expect(incident_text(f.db, other, EdgeOrientation::both).empty());
            expect(f.db.graph().contains(other));
        }
    };

    "edge_persistence_round_trip"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }
        Guid a;
        Guid b;
        Guid edge;
        {
            Database db;
            expect(eq(db.open(dir.path()), GdiStatus::ok));
            LabelId node{};
            LabelId link{};
            expect(eq(db.schema().create_label("node", node), GdiStatus::ok));
            expect(eq(db.schema().create_label("link", link), GdiStatus::ok));
            graph_db::PropertyTypeInfo info;
            info.name = "weight";
            info.data = GdiDataType::double_;
            PropNameId weight{};
            expect(eq(db.create_property_type(info, weight), GdiStatus::ok));

            auto const a_bytes = gtest::to_bytes("a");
            auto const b_bytes = gtest::to_bytes("b");
            VertexCreateResult created_a;
            VertexCreateResult created_b;
            expect(eq(db.create_vertex(view_of(node, a_bytes), {}, nullptr, created_a),
                      GdiStatus::ok));
            expect(eq(db.create_vertex(view_of(node, b_bytes), {}, nullptr, created_b),
                      GdiStatus::ok));
            a = created_a.uid;
            b = created_b.uid;
            LabelId const labels[]{link};
            graph_db::PropertyList properties;
            graph_db::PropertyEntry entry;
            entry.name = weight;
            entry.value = PropertyValue::make_double(0.75);
            properties.push_back(entry);
            expect(eq(db.edge_create(a, b, EdgeDirectionType::undirected,
                                     kimix::span<const LabelId>{labels, 1}, &properties, nullptr, edge),
                      GdiStatus::ok));
            expect(eq(db.persist(/*full=*/true), GdiStatus::ok));

            // One changed edge -> exactly one incremental record.
            expect(eq(db.set_edge_direction(edge, EdgeDirectionType::directed), GdiStatus::ok));
            GraphSerializeStats stats;
            bool const incremental = true;
            expect(eq(db.persist(!incremental, &stats), GdiStatus::ok));
            expect(eq(stats.saved_count, size_t{0})) << "no vertex record changed";
            expect(eq(stats.saved_edge_count, size_t{1})) << "only the changed edge is written";
        }

        Database restored;
        expect(eq(restored.open(dir.path()), GdiStatus::ok));
        EdgeRecord record;
        expect(restored.graph().edge_record(edge, record)) << "the edge id survived";
        expect(record.origin == a);
        expect(record.target == b);
        expect(record.direction == EdgeDirectionType::directed) << "the update was persisted";
        expect(eq(record.labels.size(), size_t{1}));
        NamedPropertyList properties;
        expect(eq(restored.get_edge_properties(edge, properties), GdiStatus::ok));
        expect(eq(properties.size(), size_t{1}));
        expect(properties[0].name == kimix::string{"weight"});
        expect(properties[0].value.as_double() == 0.75);
        kimix::vector<kimix::string> labels;
        expect(eq(restored.get_edge_labels(edge, labels), GdiStatus::ok));
        expect(eq(labels.size(), size_t{1}));
        expect(labels[0] == kimix::string{"link"});
        // Adjacency was rebuilt from the edge records.
        expect(eq(incident_text(restored, a, EdgeOrientation::outgoing).size(), size_t{1}));
        expect(eq(incident_text(restored, b, EdgeOrientation::incoming).size(), size_t{1}));
    };

    "edge_and_vertex_deletion_survive_a_reload"_test = [] {
        // A save is a WRITE transaction and cannot remove a key, so the serializer
        // stores an EMPTY value (a tombstone) under a deleted entity's key. Without
        // that the next load would resurrect it — this test is the guard.
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }
        LabelId node{};
        Guid kept;
        Guid doomed_edge;
        Guid doomed_vertex;
        {
            Database db;
            expect(eq(db.open(dir.path()), GdiStatus::ok));
            expect(eq(db.schema().create_label("node", node), GdiStatus::ok));
            auto const a_bytes = gtest::to_bytes("a");
            auto const b_bytes = gtest::to_bytes("b");
            auto const c_bytes = gtest::to_bytes("c");
            VertexCreateResult a;
            VertexCreateResult b;
            VertexCreateResult c;
            expect(eq(db.create_vertex(view_of(node, a_bytes), {}, nullptr, a), GdiStatus::ok));
            expect(eq(db.create_vertex(view_of(node, b_bytes), {}, nullptr, b), GdiStatus::ok));
            expect(eq(db.create_vertex(view_of(node, c_bytes), {}, nullptr, c), GdiStatus::ok));
            Guid ab;
            Guid bc;
            expect(eq(db.edge_create(a.uid, b.uid, EdgeDirectionType::directed, {}, nullptr, nullptr, ab),
                      GdiStatus::ok));
            expect(eq(db.edge_create(b.uid, c.uid, EdgeDirectionType::directed, {}, nullptr, nullptr, bc),
                      GdiStatus::ok));
            expect(eq(db.persist(/*full=*/true), GdiStatus::ok));
            kept = bc;
            doomed_edge = ab;
            doomed_vertex = c.uid;

            // Delete one edge and one vertex, then save incrementally.
            expect(eq(db.edge_free(ab), GdiStatus::ok));
            expect(eq(db.free_vertex(c.uid), GdiStatus::ok)) << "this cascades the bc edge";
            GraphSerializeStats stats;
            bool const full = false;
            expect(eq(db.persist(full, &stats), GdiStatus::ok));
            expect(eq(stats.deleted_edge_count, size_t{2}))
                << "the freed edge and the cascaded one are both tombstoned";
            expect(eq(stats.deleted_vertex_count, size_t{1}));
        }
        Database restored;
        expect(eq(restored.open(dir.path()), GdiStatus::ok));
        EdgeRecord record;
        expect(!restored.graph().edge_record(doomed_edge, record))
            << "a deleted edge must not come back";
        expect(!restored.graph().edge_record(kept, record));
        expect(!restored.graph().contains(doomed_vertex))
            << "a deleted vertex must not come back";
        expect(restored.last_stats().tombstone_count >= 3)
            << "the loader recognised the empty values as tombstones, not as corrupt records";
        expect(eq(restored.last_stats().skipped_count, size_t{0}))
            << "a tombstone is not a corrupt record";
        // The surviving endpoint is intact and has no dangling adjacency.
        kimix::vector<Guid> incident;
        restored.graph().incident_edges(doomed_vertex, EdgeOrientation::both, incident);
        expect(incident.empty());
    };

    "edge_legacy_connect_contract_unchanged"_test = [] {
        // The container API keeps its epoch-1..3 semantics even though edges are
        // now objects: one directed edge per ordered pair, no self loop, and the
        // same EdgeOutcome vocabulary.
        Graph graph;
        auto *a = graph.create_vertex(gtest::to_bytes("a"));
        auto *b = graph.create_vertex(gtest::to_bytes("b"));
        expect(a != nullptr);
        expect(b != nullptr);
        expect(graph.connect(a->guid(), b->guid()));
        expect(!graph.connect(a->guid(), b->guid())) << "a second edge is refused";
        expect(!graph.connect(a->guid(), a->guid())) << "a self loop is refused";
        expect(eq(graph.dependencies(a->guid()).size(), size_t{1}));
        expect(eq(graph.dependents(b->guid()).size(), size_t{1}));
        expect(eq(graph.edges().size(), size_t{1})) << "the edge is a record too";

        kimix::vector<graph_db::EdgeOutcome> outcomes(4, graph_db::EdgeOutcome::created);
        kimix::vector<graph_db::EdgeRequest> batch;
        batch.push_back(graph_db::EdgeRequest{a->guid(), b->guid()});   // duplicate
        batch.push_back(graph_db::EdgeRequest{b->guid(), a->guid()});   // new
        batch.push_back(graph_db::EdgeRequest{a->guid(), a->guid()});   // self
        Guid const ghost{true};
        batch.push_back(graph_db::EdgeRequest{a->guid(), ghost});       // missing target
        size_t const created = graph.connect_bulk(
            kimix::span<graph_db::EdgeRequest const>{batch.data(), batch.size()},
            kimix::span<graph_db::EdgeOutcome>{outcomes.data(), outcomes.size()});
        expect(eq(created, size_t{1}));
        expect(outcomes[0] == graph_db::EdgeOutcome::duplicate_edge);
        expect(outcomes[1] == graph_db::EdgeOutcome::created);
        expect(outcomes[2] == graph_db::EdgeOutcome::self_edge);
        expect(outcomes[3] == graph_db::EdgeOutcome::missing_target);
        expect(graph.disconnect(a->guid(), b->guid()));
        expect(!graph.disconnect(a->guid(), b->guid()));
        // a -> b was just disconnected; the b -> a edge created by connect_bulk
        // is what remains, so reachability runs the other way round.
        expect(!graph.has_path(a->guid(), b->guid()));
        expect(graph.has_path(b->guid(), a->guid()));
    };

    "edge_lookup_errors"_test = [] {
        fixture f;
        Guid const a = f.vertex("a");
        Guid const ghost{true};
        Guid edge;
        expect(eq(f.db.edge_create(a, ghost, EdgeDirectionType::directed, {}, nullptr, nullptr, edge),
                  GdiStatus::error_vertex))
            << "a missing endpoint is ErrorVertex";
        expect(eq(f.db.edge_create(ghost, a, EdgeDirectionType::directed, {}, nullptr, nullptr, edge),
                  GdiStatus::error_vertex));
        // A label id that is not registered.
        LabelId const unregistered{4242};
        LabelId const labels[]{unregistered};
        expect(eq(f.db.edge_create(a, a, EdgeDirectionType::directed,
                                   kimix::span<const LabelId>{labels, 1}, nullptr, nullptr, edge),
                  GdiStatus::error_label));
        // Every read of an unknown uid is ErrorUid.
        Guid origin;
        Guid target;
        expect(eq(f.db.get_edge_endpoints(ghost, origin, target), GdiStatus::error_uid));
        NamedPropertyList properties;
        expect(eq(f.db.get_edge_properties(ghost, properties), GdiStatus::error_uid));
        kimix::vector<kimix::string> labels_out;
        expect(eq(f.db.get_edge_labels(ghost, labels_out), GdiStatus::error_uid));
          expect(eq(f.db.add_edge_label(ghost, f.link), GdiStatus::error_uid));
      };

      // Create-time properties (plan v3 step 3.1/3.2): ONE call carries labels
      // and (name, value) properties; ONE journal entry; a refused create
      // leaves NOTHING behind (no edge, no adjacency).
      "edge_create_with_properties"_test = [] {
          fixture f;
          // A second property type for the payload mix.
          graph_db::PropertyTypeInfo info;
          info.name = "hops";
          info.data = GdiDataType::int64_;
          PropNameId hops{};
          expect(eq(f.db.create_property_type(info, hops), GdiStatus::ok));
          Guid const a = f.vertex("a");
          Guid const b = f.vertex("b");
          LabelId const labels[]{f.link};
          PropertyList payload;
          payload.push_back(PropertyEntry{f.weight, PropertyValue::make_double(2.5)});
          payload.push_back(PropertyEntry{hops, PropertyValue::make_int64(3)});
          Guid edge;
          expect(eq(f.db.edge_create(a, b, EdgeDirectionType::directed,
                                     kimix::span<const LabelId>{labels, 1}, &payload, nullptr, edge),
                    GdiStatus::ok));
          // The payload is readable in the same shape it was created.
          NamedPropertyList read_back;
          expect(eq(f.db.get_edge_properties(edge, read_back), GdiStatus::ok));
          expect(eq(read_back.size(), size_t{2}));
          // NamedPropertyList entries carry the names as strings; both landed.
          kimix::vector<kimix::string_view> names;
          names.push_back(read_back[0].name);
          names.push_back(read_back[1].name);
          std::sort(names.begin(), names.end());
          expect(eq(names[0], kimix::string_view{"hops"}));
          expect(eq(names[1], kimix::string_view{"weight"}));
          kimix::vector<kimix::string> edge_labels;
          expect(eq(f.db.get_edge_labels(edge, edge_labels), GdiStatus::ok));
          expect(eq(edge_labels.size(), size_t{1}));
          expect(eq(edge_labels[0], kimix::string{"link"}));
          // Adjacency was installed by the same call.
          expect(eq(f.db.graph().dependencies(a).size(), size_t{1}));
          expect(eq(f.db.graph().dependents(b).size(), size_t{1}));
          // The error matrix: every refusal happens BEFORE anything is created.
          size_t const edges_before = f.db.graph().edge_ids().size();
          LabelId const bad_label{4242};
          LabelId const bad_labels[]{bad_label};
          PropertyList bad;
          expect(eq(f.db.edge_create(a, b, EdgeDirectionType::directed,
                                     kimix::span<const LabelId>{bad_labels, 1}, &payload, nullptr, edge),
                    GdiStatus::error_label));
          bad.push_back(PropertyEntry{PropNameId{9999}, PropertyValue::make_int64(1)});
          expect(eq(f.db.edge_create(a, b, EdgeDirectionType::directed, {}, &bad, nullptr, edge),
                    GdiStatus::error_property_type));
          bad.clear();
          bad.push_back(PropertyEntry{f.weight, PropertyValue::make_int64(1)});
          expect(eq(f.db.edge_create(a, b, EdgeDirectionType::directed, {}, &bad, nullptr, edge),
                    GdiStatus::error_wrong_type))
              << "an int64 value for the double_ weight type is refused";
          bad.clear();
          bad.push_back(PropertyEntry{hops, PropertyValue::make_int64(7)});
          bad.push_back(PropertyEntry{f.weight, PropertyValue::make_int64(1)});
          expect(eq(f.db.edge_create(a, b, EdgeDirectionType::directed, {}, &bad, nullptr, edge),
                    GdiStatus::error_wrong_type))
              << "validation runs over the WHOLE payload before any mutation";
          expect(eq(f.db.graph().edge_ids().size(), edges_before))
              << "a refused create leaves no edge behind";
          expect(eq(f.db.graph().dependencies(a).size(), size_t{1}))
              << "and no adjacency behind";
      };

      // The same create, inside a transaction, aborts to nothing: the edge
      // record, its payload and its adjacency links all roll back.
      "edge_create_with_properties_rolls_back"_test = [] {
          fixture f;
          Guid const a = f.vertex("a");
          Guid const b = f.vertex("b");
          expect(eq(f.db.begin_transaction(), GdiStatus::ok));
          LabelId const labels[]{f.link};
          PropertyList payload;
          payload.push_back(PropertyEntry{f.weight, PropertyValue::make_double(9.5)});
          Guid edge;
          expect(eq(f.db.edge_create(a, b, EdgeDirectionType::directed,
                                     kimix::span<const LabelId>{labels, 1}, &payload, nullptr, edge),
                    GdiStatus::ok));
          EdgeRecord inside;
          expect(f.db.graph().edge_record(edge, inside));
          expect(eq(f.db.abort(), GdiStatus::ok));
          EdgeRecord after;
          expect(!f.db.graph().edge_record(edge, after))
              << "abort removed the edge record";
          NamedPropertyList props;
          expect(eq(f.db.get_edge_properties(edge, props), GdiStatus::error_uid))
              << "abort removed the payload with it";
          expect(f.db.graph().dependencies(a).empty());
          expect(f.db.graph().dependents(b).empty());
      };
  
    // -----------------------------------------------------------------------
    // ABI 8 change 3: a caller-chosen edge uid (plan §3.1/§3.2).
    // -----------------------------------------------------------------------
    "edge_create_with_a_chosen_uid"_test = [] {
        fixture f;
        Guid const a = f.vertex("a");
        Guid const b = f.vertex("b");
        Guid const chosen{true}; // a real, caller-supplied guid
        Guid created;
        expect(eq(f.db.edge_create(a, b, EdgeDirectionType::directed, {}, nullptr, &chosen,
                                   created),
                  GdiStatus::ok));
        expect(created == chosen) << "the edge has exactly the uid the caller asked for";
        EdgeRecord record;
        expect(f.db.graph().edge_record(chosen, record)) << "and the record is under that uid";
        expect(record.origin == a);
        expect(record.target == b);
        // A live duplicate is the duplicate-id answer, and nothing is created.
        Guid const c = f.vertex("c");
        Guid refused;
        expect(eq(f.db.edge_create(a, c, EdgeDirectionType::directed, {}, nullptr, &chosen, refused),
                  GdiStatus::error_non_unique_id));
        expect(eq(incident_text(f.db, a, EdgeOrientation::outgoing).size(), size_t{1}))
            << "the refused create added no adjacency";
        // A NULL uid still mints one (the ABI-7 behavior), and so does a zeroed one at
        // the C ABI (checked there).
        Guid minted;
        expect(eq(f.db.edge_create(a, c, EdgeDirectionType::directed, {}, nullptr, nullptr, minted),
                  GdiStatus::ok));
        expect(minted != chosen);
    };

    "edge_chosen_uid_survives_a_reopen"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }
        Guid chosen{true};
        Guid a;
        Guid b;
        {
            Database db;
            expect(eq(db.open(dir.path()), GdiStatus::ok));
            LabelId node{};
            expect(eq(db.schema().create_label("node", node), GdiStatus::ok));
            auto const a_bytes = gtest::to_bytes("a");
            auto const b_bytes = gtest::to_bytes("b");
            VertexCreateResult created_a;
            VertexCreateResult created_b;
            expect(eq(db.create_vertex(view_of(node, a_bytes), {}, nullptr, created_a),
                      GdiStatus::ok));
            expect(eq(db.create_vertex(view_of(node, b_bytes), {}, nullptr, created_b),
                      GdiStatus::ok));
            a = created_a.uid;
            b = created_b.uid;
            Guid edge;
            expect(eq(db.edge_create(a, b, EdgeDirectionType::directed, {}, nullptr, &chosen, edge),
                      GdiStatus::ok));
            expect(edge == chosen);
            expect(eq(db.persist(/*full=*/true), GdiStatus::ok));
        }
        Database restored;
        expect(eq(restored.open(dir.path()), GdiStatus::ok));
        EdgeRecord record;
        expect(restored.graph().edge_record(chosen, record))
            << "the chosen uid is the persisted record's key, not a re-minted one";
        expect(record.origin == a);
        expect(record.target == b);
        expect(eq(incident_text(restored, a, EdgeOrientation::outgoing).size(), size_t{1}));
    };

    "edge_free_then_recreate_the_same_uid_in_one_transaction"_test = [] {
        fixture f;
        Guid const a = f.vertex("a");
        Guid const b = f.vertex("b");
        PropertyList payload;
        payload.push_back(PropertyEntry{f.weight, PropertyValue::make_double(1.25)});
        Guid const chosen{true};
        Guid edge;
        expect(eq(f.db.edge_create(a, b, EdgeDirectionType::directed, {}, &payload, &chosen, edge),
                  GdiStatus::ok));
        expect(edge == chosen);
        expect(eq(f.db.begin_transaction(), GdiStatus::ok));
        expect(eq(f.db.edge_free(chosen), GdiStatus::ok));
        // The uid is free again inside the same transaction: the record store forgets
        // it the moment it is erased, so the create is a clean insert.
        Guid recreated;
        PropertyList new_payload;
        new_payload.push_back(PropertyEntry{f.weight, PropertyValue::make_double(8.5)});
        expect(eq(f.db.edge_create(a, b, EdgeDirectionType::directed, {}, &new_payload, &chosen,
                                   recreated),
                  GdiStatus::ok))
            << "a freed uid can be recreated inside the same transaction";
        expect(recreated == chosen);
        expect(eq(f.db.commit(), GdiStatus::ok));
        EdgeRecord record;
        expect(f.db.graph().edge_record(chosen, record)) << "the committed edge is the new one";
        NamedPropertyList properties;
        expect(eq(f.db.get_edge_properties(chosen, properties), GdiStatus::ok));
        expect(eq(properties.size(), size_t{1}));
        expect(properties[0].value.as_double() == 8.5) << "with the recreate's payload";
        expect(eq(incident_text(f.db, a, EdgeOrientation::outgoing).size(), size_t{1}))
            << "and exactly one adjacency entry";
    };

    "edge_free_and_recreate_then_abort_restores_the_original"_test = [] {
        fixture f;
        Guid const a = f.vertex("a");
        Guid const b = f.vertex("b");
        LabelId const labels[]{f.link};
        PropertyList payload;
        payload.push_back(PropertyEntry{f.weight, PropertyValue::make_double(2.5)});
        Guid const chosen{true};
        Guid edge;
        expect(eq(f.db.edge_create(a, b, EdgeDirectionType::directed,
                                   kimix::span<const LabelId>{labels, 1}, &payload, &chosen, edge),
                  GdiStatus::ok));
        expect(eq(f.db.begin_transaction(), GdiStatus::ok));
        expect(eq(f.db.edge_free(chosen), GdiStatus::ok));
        PropertyList new_payload;
        new_payload.push_back(PropertyEntry{f.weight, PropertyValue::make_double(7.5)});
        Guid recreated;
        expect(eq(f.db.edge_create(a, b, EdgeDirectionType::directed, {}, &new_payload, &chosen,
                                   recreated),
                  GdiStatus::ok));
        expect(eq(f.db.abort(), GdiStatus::ok));
        // The journal replay runs backwards: free the edge the recreate created, then
        // adopt the fat record the free captured — the ORIGINAL edge, with its labels
        // AND its properties.
        EdgeRecord record;
        expect(f.db.graph().edge_record(chosen, record))
            << "the abort restores the original edge under the same uid";
        expect(record.origin == a);
        expect(record.target == b);
        expect(eq(record.labels.size(), size_t{1})) << "the original label came back";
        expect(record.labels[0] == f.link);
        NamedPropertyList properties;
        expect(eq(f.db.get_edge_properties(chosen, properties), GdiStatus::ok));
        expect(eq(properties.size(), size_t{1}));
        expect(properties[0].value.as_double() == 2.5) << "the original payload, not the recreate's";
        expect(eq(incident_text(f.db, a, EdgeOrientation::outgoing).size(), size_t{1}))
            << "exactly one adjacency entry survives the replay";
        expect(eq(incident_text(f.db, b, EdgeOrientation::incoming).size(), size_t{1}));
    };

    "edge_dirty_index_matches_scan"_test = [] {
        // The dirty EDGE index (EdgeDirtySink: edge.h) replaced the O(E) shard scan
        // dirty_edges() used to run. The invariant: the index must never lose a dirty
        // record, or the next incremental save silently drops that record's write. A
        // STALE entry is the harmless direction (the collect re-checks the flag and
        // skips it), which is why each step compares dirty_edges().size() against a
        // full shard scan instead of pinning guid lists — an unexpected dirty record
        // fails the count, not the identity.
        Graph graph;
        auto check = [&graph](char const *where) {
            kimix::vector<EdgeRecord> const dirty = graph.dirty_edges();
            expect(eq(dirty.size(), graph.edges().test_count_dirty_edges_by_scan()))
                << where << ": the dirty edge index must hold every dirty record";
            kimix::unordered_set<Guid> seen;
            bool sound = true;
            for (auto const &record : dirty) {
                sound = seen.insert(record.id).second && sound;
                graph_db::Edge *live = graph.edges().find(record.id);
                sound = live != nullptr && live->is_dirty() && sound;
            }
            expect(sound) << where << ": each record is reported once and is still dirty";
        };

        graph_db::Vertex *a = graph.create_vertex(gtest::to_bytes("a"));
        graph_db::Vertex *b = graph.create_vertex(gtest::to_bytes("b"));
        graph_db::Vertex *c = graph.create_vertex(gtest::to_bytes("c"));
        expect(a != nullptr && b != nullptr && c != nullptr);
        check("create_vertex");
        expect(graph.dirty_edges().empty()) << "vertices are not edge records";

        // --- edge_create: a fresh Edge starts dirty, so insert() indexes it ------
        Guid ab;
        expect(eq(graph.edge_create(a->guid(), b->guid(), EdgeDirectionType::directed, {},
                                    nullptr, nullptr, ab),
                  GdiStatus::ok));
        check("edge_create");
        expect(eq(graph.dirty_edges().size(), size_t{1}))
            << "exactly the created record is in the write set";
        expect(graph.dirty_edges().front().id == ab);

        // A PARALLEL edge between the same ordered pair is a separate record.
        Guid ab2;
        expect(eq(graph.edge_create(a->guid(), b->guid(), EdgeDirectionType::directed, {},
                                    nullptr, nullptr, ab2),
                  GdiStatus::ok));
        check("edge_create (parallel)");
        expect(eq(graph.dirty_edges().size(), size_t{2}));

        // --- clear_dirty empties both the flags and the index -------------------
        graph.edges().clear_dirty();
        check("clear_dirty");
        expect(graph.dirty_edges().empty());
        expect(eq(graph.edges().test_count_dirty_edges_by_scan(), size_t{0}));

        // --- payload writes (one edge-shard unique lock each) -------------------
        expect(eq(graph.edge_add_label(ab, LabelId{4}), GdiStatus::ok));
        check("edge_add_label");
        expect(eq(graph.dirty_edges().size(), size_t{1}));
        graph.edges().clear_dirty();

        expect(eq(graph.edge_remove_label(ab, LabelId{4}), GdiStatus::ok));
        check("edge_remove_label");
        graph.edges().clear_dirty();

        // A refused payload write is not a write: no flag, no entry.
        expect(eq(graph.edge_remove_label(ab, LabelId{9}), GdiStatus::error_label));
        check("edge_remove_label (absent)");
        expect(graph.dirty_edges().empty());

        expect(eq(graph.edge_add_property(ab, PropNameId{5}, PropertyValue::make_int64(1)),
                  GdiStatus::ok));
        check("edge_add_property");
        expect(eq(graph.dirty_edges().size(), size_t{1}));
        graph.edges().clear_dirty();

        expect(eq(graph.edge_set_property(ab, PropNameId{5}, PropertyValue::make_int64(2)),
                  GdiStatus::ok));
        check("edge_set_property");
        graph.edges().clear_dirty();

        expect(eq(graph.edge_put_property(ab, PropNameId{5}, PropertyValue::make_int64(3)),
                  GdiStatus::ok));
        check("edge_put_property");
        graph.edges().clear_dirty();

        PropNameId const names[]{PropNameId{5}};
        expect(eq(graph.edge_remove_properties(ab, kimix::span<PropNameId const>{names, 1}),
                  GdiStatus::ok));
        check("edge_remove_properties");
        graph.edges().clear_dirty();

        // --- a direction change is an edge-record write -------------------------
        expect(eq(graph.edge_set_direction(ab, EdgeDirectionType::undirected), GdiStatus::ok));
        check("edge_set_direction");
        expect(eq(graph.dirty_edges().size(), size_t{1}));
        graph.edges().clear_dirty();

        // --- endpoint moves rewrite the adjacency AND the record ----------------
        expect(eq(graph.edge_set_origin(ab, c->guid()), GdiStatus::ok));
        check("edge_set_origin");
        expect(eq(graph.dirty_edges().size(), size_t{1}))
            << "the moved record is dirty (its two endpoints are vertex records)";
        graph.edges().clear_dirty();

        expect(eq(graph.edge_set_target(ab, c->guid()), GdiStatus::ok));
        check("edge_set_target");
        expect(eq(graph.dirty_edges().size(), size_t{1}));
        graph.edges().clear_dirty();

        // An endpoint move to the value it already holds is a no-op, so it must not
        // dirty anything (the early return in _move_edge_endpoint).
        expect(eq(graph.edge_set_target(ab, c->guid()), GdiStatus::ok));
        check("edge_set_target (already there)");
        expect(graph.dirty_edges().empty());

        // --- edge_free: the record leaves the index with the record -------------
        expect(eq(graph.edge_free(ab), GdiStatus::ok));
        check("edge_free");
        expect(graph.dirty_edges().empty())
            << "a freed record must not linger in the write set";
        expect(!graph.edges().contains(ab));

        // --- the transient edges the list APIs hand back ------------------------
        // EdgeTable::erase DETACHES the edge (no index entry any more) but leaves its
        // dirty FLAG alone: the record may still be unwritten, and the journal-undo
        // path re-inserts it verbatim. Marking it dirty while detached writes only
        // that edge's own flag — no index entry can be resurrected for a record the
        // table no longer holds.
        //
        // The record is made dirty FIRST, so the assertion below is about the detach
        // preserving the caller's state and not about a flag that happened to be clear
        // (ab2 was cleaned by the clear_dirty() above and never written again).
        graph_db::Edge *dirty_again = graph.edges().find(ab2);
        expect(dirty_again != nullptr);
        if (dirty_again != nullptr) {
            dirty_again->mark_dirty();
        }
        check("mark_dirty before erase");
        expect(eq(graph.dirty_edges().size(), size_t{1}));
        kimix::unique_ptr<graph_db::Edge> taken = graph.edges().erase(ab2);
        expect(taken != nullptr);
        expect(taken->is_dirty())
            << "erase leaves the caller's own dirty state alone (the record is still unwritten)";
        check("erase");
        taken->mark_dirty();
        check("mark_dirty on an erased edge");
        expect(graph.dirty_edges().empty()) << "a detached edge has no entry to resurrect";
        expect(taken->is_dirty()) << "the caller's own flag still works";
        // Re-inserting it (the journal-undo shape) re-attaches the sink and ADOPTS the
        // caller's dirty state: the record really is unwritten, so it belongs in the
        // write set again.
        Guid const ab2_id = ab2;
        expect(graph.edges().insert(std::move(taken)));
        check("re-insert");
        expect(eq(graph.dirty_edges().size(), size_t{1}))
            << "insert indexes a record that arrives dirty";
        expect(graph.dirty_edges().front().id == ab2_id);
        graph.edges().clear_dirty();
        check("clear_dirty after re-insert");
        expect(graph.dirty_edges().empty());
        // ... and the re-attached edge reports its own later writes.
        graph_db::Edge *reattached = graph.edges().find(ab2_id);
        expect(reattached != nullptr);
        if (reattached != nullptr) {
            reattached->mark_dirty();
        }
        check("mark_dirty after re-insert");
        expect(eq(graph.dirty_edges().size(), size_t{1}));
        graph.edges().clear_dirty();

        // --- adoption (the loader / journal path) -------------------------------
        // A clean record (assign_payload + clear_dirty, exactly what the loader
        // builds) must not enter the write set.
        kimix::unique_ptr<graph_db::Edge> loaded{
            new graph_db::Edge(Guid{true}, a->guid(), c->guid(), EdgeDirectionType::directed)};
        Guid const loaded_id = loaded->id();
        loaded->clear_dirty();
        expect(eq(graph.adopt_edge(std::move(loaded)), GdiStatus::ok));
        check("adopt_edge (clean)");
        expect(graph.dirty_edges().empty()) << "a store-faithful record is not rewritten";

        // A record that was never settled is dirty, so adopt_edge() must index it.
        kimix::unique_ptr<graph_db::Edge> fresh{
            new graph_db::Edge(Guid{true}, a->guid(), c->guid(), EdgeDirectionType::directed)};
        Guid const fresh_id = fresh->id();
        expect(eq(graph.adopt_edge(std::move(fresh)), GdiStatus::ok));
        check("adopt_edge (dirty)");
        expect(eq(graph.dirty_edges().size(), size_t{1}));
        expect(graph.dirty_edges().front().id == fresh_id);
        expect(graph.dirty_edges().front().origin == a->guid());
        expect(graph.dirty_edges().front().target == c->guid());
        (void)loaded_id;

        // --- whole-table helpers ------------------------------------------------
        graph.edges().mark_dirty();
        check("EdgeTable::mark_dirty");
        expect(eq(graph.dirty_edges().size(), graph.edges().size()))
            << "mark_dirty puts every record back in the write set";
        graph.edges().clear_dirty();
        check("EdgeTable::clear_dirty");
        expect(graph.dirty_edges().empty());

        // The Graph-level pair walks vertices AND edges, and must leave the two
        // indices consistent with the flags either way.
        graph.mark_dirty();
        check("Graph::mark_dirty");
        expect(eq(graph.dirty_edges().size(), graph.edges().size()));
        graph.clear_dirty();
        check("Graph::clear_dirty");
        expect(graph.dirty_edges().empty());
    };
}
