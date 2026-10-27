// Test for constraint-filtered, oriented, paged reads (src/graph_db/query.*,
// Database::get_edges_of_vertex / get_neighbor_vertices / count_neighbors /
// exists_neighbor) — GDI §2.6.
//
// This test covers:
// - the incidence matrix: directed / undirected / parallel / self edges x
//   {outgoing, incoming, both} x {edges, neighbours, count, exists};
// - the constraint matrix: label equal/not-equal, and property equal / not-equal /
//   greater / greater-or-equal / less / less-or-equal over int64, double, string,
//   blob and the two array kinds, plus the int64-vs-double cross comparison, a
//   missing property, an undeclared type and the label-with-an-ordering-op
//   rejection (error_op);
// - paging: cap -1 / 0 / 1 / 2 / N / large over a 10-item match set, asserting
//   items.size() AND total_count for each (GDI's two-phase count/fetch contract);
// - the §5.10 distinction: the SAME constraint filters EDGES on an edge read and
//   NEIGHBOUR VERTICES on a neighbour read, and therefore yields different results;
// - determinism: a repeated read returns the identical order;
// - a 4-reader / 1-writer stress run over filtered reads (no crash, no lock-order
//   violation).
#include "ut/ut.hpp"

#include "graph_db_test_util.h"

#include <core/kimix_core.h> // umbrella first: it fixes the winsock2.h order
#include <core/fiber.h>

#include <graph_db/database.h>
#include <graph_db/edge.h>
#include <graph_db/graph_db.h>
#include <graph_db/query.h>
#include <graph_db/status.h>

#include <core/guid.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <utility>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using graph_db::Constraint;
using graph_db::ConstraintCondition;
using graph_db::ConstraintHandle;
using graph_db::Database;
using graph_db::EdgeDirectionType;
using graph_db::EdgeOrientation;
using graph_db::EdgePage;
using graph_db::EdgeRecord;
using graph_db::GdiContainerType;
using graph_db::GdiDataType;
using graph_db::GdiOp;
using graph_db::GdiStatus;
using graph_db::LabelId;
using graph_db::PropNameId;
using graph_db::PropertyTypeInfo;
using graph_db::PropertyValue;
using graph_db::VertexCreateResult;
using graph_db::VertexKeyView;
using graph_db::VertexPage;
namespace gtest = kimix::graph_db::test;

namespace tests_unit_graph_db_test_graph_db_read {

inline VertexKeyView view_of(LabelId label, kimix::vector<std::byte> const &bytes) {
    return VertexKeyView{label, kimix::span<const std::byte>{bytes.data(), bytes.size()}};
}

PropertyTypeInfo make_type(kimix::string_view name, GdiDataType data, uint32_t count = 1) {
    PropertyTypeInfo info;
    info.name = kimix::string{name};
    info.data = data;
    info.count = count;
    info.container = count > 1 ? GdiContainerType::array : GdiContainerType::single;
    return info;
}

// A graph built once per case: 10 vertices ("n0".."n9") keyed by `node`, each
// carrying `age` = its index, optionally labelled `warm`, and a hub vertex
// connected to all of them.
struct fixture {
    Database db;
    LabelId node{};
    LabelId warm{};
    LabelId link{};
    PropNameId age{};
    PropNameId ratio{};
    PropNameId tag{};
    PropNameId blob{};
    PropNameId ages{};
    kimix::vector<Guid> vertices;
    Guid hub;
    kimix::vector<Guid> hub_edges;

    explicit fixture(size_t count = 10) {
        expect(eq(db.schema().create_label("node", node), GdiStatus::ok));
        expect(eq(db.schema().create_label("warm", warm), GdiStatus::ok));
        expect(eq(db.schema().create_label("link", link), GdiStatus::ok));
        expect(eq(db.create_property_type(make_type("age", GdiDataType::int64_), age),
                  GdiStatus::ok));
        expect(eq(db.create_property_type(make_type("ratio", GdiDataType::double_), ratio),
                  GdiStatus::ok));
        expect(eq(db.create_property_type(make_type("tag", GdiDataType::char_), tag),
                  GdiStatus::ok));
        expect(eq(db.create_property_type(make_type("blob", GdiDataType::byte_), blob),
                  GdiStatus::ok));
        expect(eq(db.create_property_type(make_type("ages", GdiDataType::int64_, 2), ages),
                  GdiStatus::ok));

        for (size_t i = 0; i < count; ++i) {
            auto const bytes = gtest::to_bytes(kimix::format("n{}", i));
            VertexCreateResult created;
            expect(eq(db.create_vertex(view_of(node, bytes), {}, nullptr, created), GdiStatus::ok));
            expect(eq(db.add_property(created.uid, this->age,
                                      PropertyValue::make_int64(static_cast<int64_t>(i))),
                      GdiStatus::ok));
            expect(eq(db.add_property(created.uid, ratio,
                                      PropertyValue::make_double(static_cast<double>(i) + 0.5)),
                      GdiStatus::ok));
            expect(eq(db.add_property(created.uid, tag,
                                      PropertyValue::make_string(
                                          kimix::format("tag{}", i % 3))),
                      GdiStatus::ok));
            std::byte raw[2]{std::byte{i % 2}, std::byte{0x7f}};
            expect(eq(db.add_property(created.uid, blob,
                                      PropertyValue::make_blob(
                                          kimix::span<const std::byte>{raw, 2})),
                      GdiStatus::ok));
            int64_t const pair[2]{static_cast<int64_t>(i), 0};
            expect(eq(db.add_property(created.uid, ages,
                                      PropertyValue::make_int64_array(
                                          kimix::span<const int64_t>{pair, 2})),
                      GdiStatus::ok));
            if (i % 2 == 0) {
                expect(eq(db.add_label(created.uid, warm), GdiStatus::ok));
            }
            vertices.push_back(created.uid);
        }
        // A hub connected to every vertex, with the `link` label on the edges.
        auto const hub_bytes = gtest::to_bytes("hub");
        VertexCreateResult created_hub;
        expect(eq(db.create_vertex(view_of(node, hub_bytes), {}, nullptr, created_hub),
                  GdiStatus::ok));
        hub = created_hub.uid;
        LabelId const labels[]{link};
        for (auto const &target : vertices) {
            Guid edge;
            expect(eq(db.edge_create(hub, target, EdgeDirectionType::directed,
                                     kimix::span<const LabelId>{labels, 1}, nullptr, edge),
                      GdiStatus::ok));
            hub_edges.push_back(edge);
        }
    }

    Constraint label_constraint(LabelId label, GdiOp op) const {
        Constraint constraint;
        constraint.conditions.push_back(ConstraintCondition::on_label(label, op));
        return constraint;
    }

    Constraint property_constraint(PropNameId name, GdiOp op, PropertyValue value) const {
        Constraint constraint;
        constraint.conditions.push_back(ConstraintCondition::on_property(name, op,
                                                                        std::move(value)));
        return constraint;
    }
};

} // namespace tests_unit_graph_db_test_graph_db_read
using namespace tests_unit_graph_db_test_graph_db_read;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    kimix::fiber::scheduler sched{4u};

    "read_incidence_matrix"_test = [] {
        fixture f(3);
        Guid const a = f.vertices[0];
        Guid const b = f.vertices[1];
        Guid const c = f.vertices[2];

        // a -> b (directed), b -> a (directed), a -> c (undirected), a -> a (loop).
        Guid forward;
        Guid backward;
        Guid undirected;
        Guid loop;
        expect(eq(f.db.edge_create(a, b, EdgeDirectionType::directed, {}, nullptr, forward),
                  GdiStatus::ok));
        expect(eq(f.db.edge_create(b, a, EdgeDirectionType::directed, {}, nullptr, backward),
                  GdiStatus::ok));
        expect(eq(f.db.edge_create(a, c, EdgeDirectionType::undirected, {}, nullptr, undirected),
                  GdiStatus::ok));
        expect(eq(f.db.edge_create(a, a, EdgeDirectionType::directed, {}, nullptr, loop),
                  GdiStatus::ok));

        size_t out_count = 0;
        size_t in_count = 0;
        size_t both_count = 0;
        expect(eq(f.db.count_neighbors(a, Constraint{}, EdgeOrientation::outgoing, out_count),
                  GdiStatus::ok));
        expect(eq(f.db.count_neighbors(a, Constraint{}, EdgeOrientation::incoming, in_count),
                  GdiStatus::ok));
        expect(eq(f.db.count_neighbors(a, Constraint{}, EdgeOrientation::both, both_count),
                  GdiStatus::ok));
        // a is n0, which the fixture also wired the hub to, so its incoming side
        // carries the hub edge ON TOP of the three edges created here.
        expect(eq(out_count, size_t{3})) << "a -> b, a -> c (undirected), the self loop";
        // INCIDENCE IS BY ORIGIN/TARGET, exactly like the reference backend's
        // IsIncident: the undirected edge a -> c is OUTGOING for a and INCOMING for
        // c, because the record's endpoints decide, not the direction flag (the flag
        // is metadata a reader may report; it does not move an edge between lists).
        expect(eq(in_count, size_t{3})) << "hub -> a, b -> a, the loop";
        expect(eq(both_count, size_t{4}));

        // Neighbours DEDUPLICATE: {hub, b, c, a itself (the loop)}.
        VertexPage page;
        expect(eq(f.db.get_neighbor_vertices(a, Constraint{}, EdgeOrientation::both, 0, page),
                  GdiStatus::ok));
        expect(eq(page.total_count, size_t{4}));

        bool exists = false;
        expect(eq(f.db.exists_neighbor(a, Constraint{}, EdgeOrientation::outgoing, exists),
                  GdiStatus::ok));
        expect(exists);
        expect(eq(f.db.exists_neighbor(c, Constraint{}, EdgeOrientation::incoming, exists),
                  GdiStatus::ok));
        expect(exists) << "the undirected edge is incident for c";
    };

    "read_unfiltered_fast_path"_test = [] {
      // Plan v3 §4.1: the unfiltered neighbourhood read answers from ONE shard
      // SHARED lock snapshot of the anchor's adjacency projection (no per-edge
      // endpoint round-trips, no per-neighbour record/label/property reads).
      // Semantics must stay identical to the filtered walk: distinct neighbour
      // guids, parallel edges collapsed, self loop counted once per side.
      fixture f(4);
      Guid const a = f.vertices[0];
      Guid const b = f.vertices[1];
      Guid const c = f.vertices[2];
      Guid const d = f.vertices[3];
      // a -> b twice (parallel), a -> c undirected, a -> a loop, b -> a back.
      Guid e1;
      Guid e2;
      Guid e3;
      Guid e4;
      Guid e5;
      expect(eq(f.db.edge_create(a, b, EdgeDirectionType::directed, {}, nullptr, e1),
                GdiStatus::ok));
      expect(eq(f.db.edge_create(a, b, EdgeDirectionType::directed, {}, nullptr, e2),
                GdiStatus::ok)) << "parallel edges are allowed on this store";
      expect(eq(f.db.edge_create(a, c, EdgeDirectionType::undirected, {}, nullptr, e3),
                GdiStatus::ok));
      expect(eq(f.db.edge_create(a, a, EdgeDirectionType::directed, {}, nullptr, e4),
                GdiStatus::ok));
      expect(eq(f.db.edge_create(b, a, EdgeDirectionType::directed, {}, nullptr, e5),
                GdiStatus::ok));
      // The fixture hub also links hub -> a and hub -> every other vertex.
      size_t count = 0;
      expect(eq(f.db.count_neighbors(a, Constraint{}, EdgeOrientation::outgoing, count),
                GdiStatus::ok));
      // out: b (two parallel edges collapse), c (undirected), the loop = 3.
      expect(eq(count, size_t{3}));
      expect(eq(f.db.count_neighbors(a, Constraint{}, EdgeOrientation::incoming, count),
                GdiStatus::ok));
      // in: hub, b, the loop = 3 (the undirected a -> c edge is OUTGOING for
      // a by the record's endpoints, exactly like the incidence matrix).
      expect(eq(count, size_t{3}));
      expect(eq(f.db.count_neighbors(a, Constraint{}, EdgeOrientation::both, count),
                GdiStatus::ok));
      // distinct union: {hub, b, c, a} = 4.
      expect(eq(count, size_t{4}));
      // The cap>0 page returns the same distinct set with the real total.
      VertexPage page;
      expect(eq(f.db.get_neighbor_vertices(a, Constraint{}, EdgeOrientation::outgoing, 10, page),
                GdiStatus::ok));
      expect(eq(page.total_count, size_t{3}));
      expect(eq(page.items.size(), size_t{3}));
      // Every reported neighbour exists (the projection only names live edges).
      for (auto const &guid : page.items) {
          expect(f.db.graph().contains(guid));
      }
      // The unfiltered read sees a mutation immediately (no stale counter cache).
      Guid e6;
      expect(eq(f.db.edge_create(a, d, EdgeDirectionType::directed, {}, nullptr, e6),
                GdiStatus::ok));
      expect(eq(f.db.count_neighbors(a, Constraint{}, EdgeOrientation::outgoing, count),
                GdiStatus::ok));
      expect(eq(count, size_t{4})) << "d joins the outgoing neighbourhood";
      // Removing an edge updates the count the same way.
      expect(eq(f.db.edge_free(e1), GdiStatus::ok));
      expect(eq(f.db.count_neighbors(a, Constraint{}, EdgeOrientation::outgoing, count),
                GdiStatus::ok));
      expect(eq(count, size_t{4})) << "one parallel edge to b remains";
      expect(eq(f.db.edge_free(e2), GdiStatus::ok));
      expect(eq(f.db.count_neighbors(a, Constraint{}, EdgeOrientation::outgoing, count),
                GdiStatus::ok));
      expect(eq(count, size_t{3})) << "b is gone from the outgoing side";
      // Unknown vertex: error_vertex on both the probe and the page.
      Guid const missing = Guid{true};
      expect(eq(f.db.count_neighbors(missing, Constraint{}, EdgeOrientation::outgoing, count),
                GdiStatus::error_vertex));
      expect(eq(f.db.get_neighbor_vertices(missing, Constraint{}, EdgeOrientation::both, 0, page),
                GdiStatus::error_vertex));
  };
  "read_constraint_matrix"_test = [] {
        fixture f(10);

        // Label conditions: equal / not-equal over the explicit label set.
        Constraint warm_only = f.label_constraint(f.warm, GdiOp::equal);
        size_t count = 0;
        expect(eq(f.db.count_neighbors(f.hub, warm_only, EdgeOrientation::outgoing, count),
                  GdiStatus::ok));
        expect(eq(count, size_t{5})) << "half the vertices are warm";

        Constraint cold_only = f.label_constraint(f.warm, GdiOp::not_equal);
        expect(eq(f.db.count_neighbors(f.hub, cold_only, EdgeOrientation::outgoing, count),
                  GdiStatus::ok));
        expect(eq(count, size_t{5}));

        // Property conditions on every kind and op.
        struct case_t {
            GdiOp op;
            PropertyValue value;
            size_t expected;
        };
        kimix::vector<case_t> cases;
        cases.push_back(case_t{GdiOp::equal, PropertyValue::make_int64(4), 1});
        cases.push_back(case_t{GdiOp::not_equal, PropertyValue::make_int64(4), 9});
        cases.push_back(case_t{GdiOp::greater, PropertyValue::make_int64(4), 5});
        cases.push_back(case_t{GdiOp::greater_or_equal, PropertyValue::make_int64(4), 6});
        cases.push_back(case_t{GdiOp::less, PropertyValue::make_int64(4), 4});
        cases.push_back(case_t{GdiOp::less_or_equal, PropertyValue::make_int64(4), 5});
        // int64 vs double compares across kinds (the reference widens).
        cases.push_back(case_t{GdiOp::greater, PropertyValue::make_double(6.5), 3});
        cases.push_back(case_t{GdiOp::less_or_equal, PropertyValue::make_double(1.5), 2});
        for (auto const &item : cases) {
            Constraint const constraint = f.property_constraint(f.age, item.op, item.value);
            expect(eq(f.db.count_neighbors(f.hub, constraint, EdgeOrientation::outgoing, count),
                      GdiStatus::ok))
                << graph_db::op_name(item.op);
            expect(eq(count, item.expected)) << graph_db::op_name(item.op);
        }

        // Strings compare ordinally; blobs and arrays support EQUALITY only.
        Constraint text = f.property_constraint(f.tag, GdiOp::equal,
                                               PropertyValue::make_string("tag0"));
        expect(eq(f.db.count_neighbors(f.hub, text, EdgeOrientation::outgoing, count),
                  GdiStatus::ok));
        expect(eq(count, size_t{4})) << "indices 0, 3, 6, 9";
        Constraint text_order = f.property_constraint(f.tag, GdiOp::greater,
                                                     PropertyValue::make_string("tag1"));
        expect(eq(f.db.count_neighbors(f.hub, text_order, EdgeOrientation::outgoing, count),
                  GdiStatus::ok));
        expect(eq(count, size_t{3})) << "ordinal order: tag0 < tag1 < tag2, so only tag2";

        std::byte raw[2]{std::byte{0}, std::byte{0x7f}};
        Constraint blob_equal = f.property_constraint(f.blob, GdiOp::equal,
                                                     PropertyValue::make_blob(
                                                         kimix::span<const std::byte>{raw, 2}));
        expect(eq(f.db.count_neighbors(f.hub, blob_equal, EdgeOrientation::outgoing, count),
                  GdiStatus::ok));
        expect(eq(count, size_t{5})) << "the even indices carry blob[0] == 0";

        // A missing property never matches; an undeclared one is ErrorPropertyType
        // (checked at add_property_condition time, not here).
        Constraint missing = f.property_constraint(f.age, GdiOp::equal, PropertyValue{});
        expect(eq(f.db.count_neighbors(f.hub, missing, EdgeOrientation::outgoing, count),
                  GdiStatus::ok));
        expect(eq(count, size_t{0})) << "a `none` expected value matches nothing";

        // An ordering op on a LABEL is rejected when the condition is added.
        ConstraintHandle handle;
        expect(eq(f.db.create_constraint(Constraint{}, handle), GdiStatus::ok));
        expect(eq(f.db.add_label_condition(handle, f.warm, GdiOp::greater),
                  GdiStatus::error_op));
        expect(eq(f.db.add_label_condition(handle, f.warm, GdiOp::equal), GdiStatus::ok));
        expect(eq(f.db.add_label_condition(handle, 999, GdiOp::equal), GdiStatus::error_label));
        expect(eq(f.db.add_property_condition(handle, 999, GdiOp::equal,
                                              PropertyValue::make_int64(1)),
                  GdiStatus::error_property_type));
    };

    "read_paging_contract"_test = [] {
        fixture f(10);
        Constraint const none;
        struct case_t {
            int64_t cap;
            size_t items;
            size_t total;
        };
        kimix::vector<case_t> cases;
        cases.push_back(case_t{-1, 0, 0});  // error_argument, page untouched
        cases.push_back(case_t{0, 0, 10});  // count probe
        cases.push_back(case_t{1, 1, 10});
        cases.push_back(case_t{2, 2, 10});
        cases.push_back(case_t{10, 10, 10});
        cases.push_back(case_t{100, 10, 10});
        for (auto const &item : cases) {
            VertexPage page;
            GdiStatus const status =
                f.db.get_neighbor_vertices(f.hub, none, EdgeOrientation::outgoing, item.cap, page);
            if (item.cap < 0) {
                expect(eq(status, GdiStatus::error_argument)) << "cap must be >= 0";
                continue;
            }
            expect(eq(status, GdiStatus::ok));
            expect(eq(page.items.size(), item.items)) << "cap " << item.cap;
            expect(eq(page.total_count, item.total)) << "cap " << item.cap;
        }
        // The edge read follows the same contract.
        EdgePage page;
        expect(eq(f.db.get_edges_of_vertex(f.hub, none, EdgeOrientation::outgoing, 0, page),
                  GdiStatus::ok));
        expect(eq(page.items.size(), size_t{0}));
        expect(eq(page.total_count, size_t{10}));
        expect(eq(f.db.get_edges_of_vertex(f.hub, none, EdgeOrientation::outgoing, 3, page),
                  GdiStatus::ok));
        expect(eq(page.items.size(), size_t{3}));
        expect(eq(page.total_count, size_t{10}));
        for (auto const &record : page.items) {
            expect(record.origin == f.hub);
        }
    };

    "read_edge_filter_versus_neighbour_filter"_test = [] {
        // §5.10: an EDGE read filters the edge, a NEIGHBOUR read filters the
        // neighbour. The same constraint therefore returns different things.
        fixture f(4);
        Constraint const link_only = f.label_constraint(f.link, GdiOp::equal);
        Constraint const warm_only = f.label_constraint(f.warm, GdiOp::equal);

        EdgePage edges;
        // The edges carry `link`, so the edge read matches all of them.
        expect(eq(f.db.get_edges_of_vertex(f.hub, link_only, EdgeOrientation::outgoing, 0, edges),
                  GdiStatus::ok));
        expect(eq(edges.total_count, size_t{4}));
        // The NEIGHBOURS do not carry `link`, so the neighbour read matches none.
        VertexPage vertices;
        expect(eq(f.db.get_neighbor_vertices(f.hub, link_only, EdgeOrientation::outgoing, 0,
                                             vertices),
                  GdiStatus::ok));
        expect(eq(vertices.total_count, size_t{0}))
            << "a neighbour read filters the vertex, not the edge";

        // With the vertex-side constraint the roles flip.
        expect(eq(f.db.get_edges_of_vertex(f.hub, warm_only, EdgeOrientation::outgoing, 0, edges),
                  GdiStatus::ok));
        expect(eq(edges.total_count, size_t{0})) << "the edges carry no `warm` label";
        expect(eq(f.db.get_neighbor_vertices(f.hub, warm_only, EdgeOrientation::outgoing, 0,
                                             vertices),
                  GdiStatus::ok));
        expect(eq(vertices.total_count, size_t{2})) << "half the neighbours are warm";
    };

    "read_determinism"_test = [] {
        fixture f(10);
        Constraint const none;
        VertexPage first;
        VertexPage second;
        expect(eq(f.db.get_neighbor_vertices(f.hub, none, EdgeOrientation::both, 0, first),
                  GdiStatus::ok));
        expect(eq(f.db.get_neighbor_vertices(f.hub, none, EdgeOrientation::both, 0, second),
                  GdiStatus::ok));
        expect(eq(first.items.size(), second.items.size()));
        for (size_t i = 0; i < first.items.size(); ++i) {
            expect(first.items[i] == second.items[i])
                << "a repeated read returns the identical order (link insertion order)";
        }
    };

    "read_errors"_test = [] {
        fixture f(2);
        Guid const ghost{true};
        VertexPage page;
        expect(eq(f.db.get_neighbor_vertices(ghost, Constraint{}, EdgeOrientation::both, 0, page),
                  GdiStatus::error_vertex));
        EdgePage edges;
        expect(eq(f.db.get_edges_of_vertex(ghost, Constraint{}, EdgeOrientation::both, 0, edges),
                  GdiStatus::error_vertex));
        size_t count = 1;
        expect(eq(f.db.count_neighbors(ghost, Constraint{}, EdgeOrientation::both, count),
                  GdiStatus::error_vertex));
        bool exists = true;
        expect(eq(f.db.exists_neighbor(ghost, Constraint{}, EdgeOrientation::both, exists),
                  GdiStatus::error_vertex));
    };

    "read_stress_readers_with_one_writer"_test = [] {
        // 4 reader threads hammer filtered reads while the main thread adds
        // vertices and edges. What is asserted: no crash, no lock-order violation,
        // and every returned page is self-consistent (its total_count is at least
        // its item count).
        fixture f(20);
        std::atomic<bool> stop{false};
        std::atomic<size_t> bad{0};
        kimix::vector<std::thread> readers;
        for (int i = 0; i < 4; ++i) {
            readers.emplace_back([&f, &stop, &bad, i] {
                Constraint const constraint =
                    (i % 2 == 0) ? f.property_constraint(f.age, GdiOp::greater_or_equal,
                                                         PropertyValue::make_int64(5))
                                 : f.label_constraint(f.warm, GdiOp::equal);
                while (!stop.load(std::memory_order_relaxed)) {
                    VertexPage page;
                    if (f.db.get_neighbor_vertices(f.hub, constraint, EdgeOrientation::outgoing,
                                                   4, page) == GdiStatus::ok) {
                        if (page.items.size() > page.total_count || page.items.size() > 4) {
                            bad.fetch_add(1);
                        }
                    }
                    EdgePage edges;
                    if (f.db.get_edges_of_vertex(f.hub, Constraint{}, EdgeOrientation::both, 3,
                                                 edges) == GdiStatus::ok) {
                        if (edges.items.size() > edges.total_count || edges.items.size() > 3) {
                            bad.fetch_add(1);
                        }
                    }
                }
            });
        }
        for (int i = 0; i < 200; ++i) {
            auto const bytes = gtest::to_bytes(kimix::format("extra{}", i));
            VertexCreateResult created;
            if (f.db.create_vertex(view_of(f.node, bytes), {}, nullptr, created) != GdiStatus::ok) {
                continue;
            }
            (void)f.db.add_property(created.uid, f.age,
                                    PropertyValue::make_int64(static_cast<int64_t>(i)));
            (void)f.db.edge_create(f.hub, created.uid, EdgeDirectionType::directed, {}, nullptr,
                                   created.uid);
        }
        stop.store(true);
        for (auto &reader : readers) {
            reader.join();
        }
        expect(eq(bad.load(), size_t{0})) << "every page was self-consistent";
        expect(eq(f.db.graph().edges().size(), size_t{20 + 200}));
    };
}
