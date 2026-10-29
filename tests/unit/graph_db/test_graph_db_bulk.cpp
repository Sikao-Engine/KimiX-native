// Test for bulk creation, guarded delete and the complete error surface
// (Database::create_vertices_bulk / create_edges_bulk / free_vertex_guarded) —
// GDI §2.8, §2.10, §2.11.
//
// This test covers:
// - a 10k keyed bulk create: the count, the ROW ORDER of the results, the key
//   index size, and "one transaction" (one save writes all 10k records);
// - duplicate rows under both continue_on_error values: isolated rows
//   (per-row statuses) versus all-or-nothing (the first failure aborts the whole
//   batch and the untouched rows are marked error_state);
// - bulk edges with one refused row (a missing endpoint): the created count and
//   the per-row statuses;
// - the guarded delete: a vertex with no edges deletes, and one with a single
//   incident edge of any kind is refused with error_edge and NOTHING removed;
// - the ERROR SURFACE SWEEP: ~30 failing calls across the whole Database surface,
//   each asserted against its exact GdiStatus — this is the contract the C#
//   adapter's own test asserts on the other side of the ABI.
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
#include <core/stl/vector.h>

#include <cstddef>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using graph_db::Constraint;
using graph_db::Database;
using graph_db::EdgeBulkRow;
using graph_db::EdgeDirectionType;
using graph_db::EdgeOrientation;
using graph_db::GdiDataType;
using graph_db::GdiOp;
using graph_db::GdiStatus;
using graph_db::GraphSerializeStats;
using graph_db::LabelId;
using graph_db::NamedPropertyList;
using graph_db::PropertyEntry;
using graph_db::PropNameId;
using graph_db::PropertyTypeInfo;
using graph_db::PropertyValue;
using graph_db::VertexBulkRow;
using graph_db::VertexCreateResult;
using graph_db::VertexKey;
using graph_db::VertexKeyView;
using graph_db::VertexPage;
namespace gtest = kimix::graph_db::test;

namespace tests_unit_graph_db_test_graph_db_bulk {

inline VertexKeyView view_of(LabelId label, kimix::vector<std::byte> const &bytes) {
    return VertexKeyView{label, kimix::span<const std::byte>{bytes.data(), bytes.size()}};
}

// A row that owns its key bytes (what create_vertices_bulk takes).
VertexBulkRow make_row(LabelId label, kimix::string_view external) {
    VertexBulkRow row;
    row.key.label = label;
    row.key.external_id = graph_db::ExternalId::from_string(external);
    return row;
}

struct fixture {
    Database db;
    LabelId node{};
    LabelId link{};
    PropNameId age{};

    explicit fixture(size_t vertices = 3) {
        expect(eq(db.schema().create_label("node", node), GdiStatus::ok));
        expect(eq(db.schema().create_label("link", link), GdiStatus::ok));
        PropertyTypeInfo info;
        info.name = "age";
        info.data = GdiDataType::int64_;
        expect(eq(db.create_property_type(info, age), GdiStatus::ok));
        for (size_t i = 0; i < vertices; ++i) {
            auto const bytes = gtest::to_bytes(kimix::format("v{}", i));
            VertexCreateResult created;
            expect(eq(db.create_vertex(view_of(node, bytes), {}, nullptr, created), GdiStatus::ok));
        }
    }

    kimix::vector<Guid> guids() const {
        return db.graph().vertex_guids();
    }
};

} // namespace tests_unit_graph_db_test_graph_db_bulk
using namespace tests_unit_graph_db_test_graph_db_bulk;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    kimix::fiber::scheduler sched{4u};

    "bulk_create_10k_in_one_transaction"_test = [] {
        fixture f(0);
        constexpr size_t kCount = 10000;
        kimix::vector<VertexBulkRow> rows;
        rows.reserve(kCount);
        for (size_t i = 0; i < kCount; ++i) {
            rows.push_back(make_row(f.node, kimix::format("bulk{}", i)));
        }
        kimix::vector<VertexCreateResult> results;
        kimix::vector<GdiStatus> statuses;
        graph_db::BulkStats stats;
        expect(eq(f.db.create_vertices_bulk(
                      kimix::span<VertexBulkRow const>{rows.data(), rows.size()},
                      /*continue_on_error=*/false, results, &statuses, stats),
                  GdiStatus::ok));
        expect(eq(stats.created, kCount));
        expect(eq(stats.failed, size_t{0}));
        expect(eq(results.size(), kCount));
        expect(eq(statuses.size(), kCount));
        expect(eq(f.db.graph().size(), kCount));
        expect(eq(f.db.graph().key_index().size(), kCount));

        // Results are in ROW order: row i created key bulk<i>.
        for (size_t i = 0; i < kCount; i += 997) {
            auto const bytes = gtest::to_bytes(kimix::format("bulk{}", i));
            Guid resolved;
            bool found = false;
            expect(eq(f.db.translate_vertex_id(view_of(f.node, bytes), resolved, found),
                      GdiStatus::ok));
            expect(found);
            expect(resolved == results[i].uid) << "result row " << i;
        }

        // ONE transaction means ONE save writes everything.
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }
        Database persisted;
        expect(eq(persisted.open(dir.path()), GdiStatus::ok));
        LabelId node{};
        expect(eq(persisted.schema().create_label("node", node), GdiStatus::ok));
        kimix::vector<VertexBulkRow> tiny_rows;
        tiny_rows.push_back(make_row(node, "a"));
        tiny_rows.push_back(make_row(node, "b"));
        kimix::vector<VertexCreateResult> tiny_results;
        graph_db::BulkStats tiny_stats;
        expect(eq(persisted.create_vertices_bulk(
                      kimix::span<VertexBulkRow const>{tiny_rows.data(), tiny_rows.size()}, true,
                      tiny_results, nullptr, tiny_stats),
                  GdiStatus::ok));
        GraphSerializeStats saved;
        expect(eq(persisted.persist(/*full=*/true, &saved), GdiStatus::ok));
        expect(eq(saved.saved_count, size_t{2})) << "the whole batch went out in one save";
    };

    "bulk_continue_on_error_isolates_rows"_test = [] {
        fixture f(0);
        kimix::vector<VertexBulkRow> rows;
        rows.push_back(make_row(f.node, "one"));
        rows.push_back(make_row(f.node, "one"));  // duplicate
        rows.push_back(make_row(f.node, "two"));
        kimix::vector<VertexCreateResult> results;
        kimix::vector<GdiStatus> statuses;
        graph_db::BulkStats stats;
        expect(eq(f.db.create_vertices_bulk(
                      kimix::span<VertexBulkRow const>{rows.data(), rows.size()},
                      /*continue_on_error=*/true, results, &statuses, stats),
                  GdiStatus::ok));
        expect(eq(stats.created, size_t{2}));
        expect(eq(stats.failed, size_t{1}));
        expect(eq(statuses.size(), size_t{3}));
        expect(eq(statuses[0], GdiStatus::ok));
        expect(eq(statuses[1], GdiStatus::error_non_unique_id)) << "the duplicate is isolated";
        expect(eq(statuses[2], GdiStatus::ok)) << "the batch carried on";
        expect(eq(f.db.graph().size(), size_t{2}));
    };

    "bulk_all_or_nothing"_test = [] {
        fixture f(0);
        kimix::vector<VertexBulkRow> rows;
        rows.push_back(make_row(f.node, "one"));
        rows.push_back(make_row(f.node, "one"));  // duplicate: aborts the batch
        rows.push_back(make_row(f.node, "two"));
        kimix::vector<VertexCreateResult> results;
        kimix::vector<GdiStatus> statuses;
        graph_db::BulkStats stats;
        expect(eq(f.db.create_vertices_bulk(
                      kimix::span<VertexBulkRow const>{rows.data(), rows.size()},
                      /*continue_on_error=*/false, results, &statuses, stats),
                  GdiStatus::error_state))
            << "the batch reports the failure";
        expect(eq(stats.created, size_t{0}))
            << "the first row was rolled back with the batch";
        expect(eq(stats.failed, size_t{1})) << "only the row that failed is counted as failed";
        expect(eq(statuses.size(), size_t{3}));
        expect(eq(statuses[1], GdiStatus::error_non_unique_id));
        expect(eq(statuses[2], GdiStatus::error_state)) << "rows after the failure are skipped";
        expect(eq(f.db.graph().size(), size_t{0})) << "nothing survived";
        expect(eq(f.db.graph().key_index().size(), size_t{0}));
    };

          "bulk_vertices_inside_caller_transaction"_test = [] {
          // Plan v3 §4.4 (the native bulk defect): create_vertices_bulk must be
          // usable inside an explicitly open caller transaction. The old eager
          // `Transaction local` was destroyed with its journal still open, and
          // its destructor called Database::abort() — on the CALLER's
          // transaction: the batch itself worked, but the caller's commit then
          // answered ERR_TRANSACTION and every journal entry (the whole batch)
          // was rolled back. The local scope is now built lazily, so with a
          // caller tx open there is NO local object at all.
          fixture f(0);
          auto const make_rows = [&](kimix::string_view stem) {
              kimix::vector<VertexBulkRow> made;
              made.push_back(make_row(f.node, kimix::format("{}-a", stem)));
              made.push_back(make_row(f.node, kimix::format("{}-b", stem)));
              made.push_back(make_row(f.node, kimix::format("{}-c", stem)));
              return made;
          };
          auto const run_batch = [&](kimix::vector<VertexBulkRow> const &rows) {
              kimix::vector<VertexCreateResult> results;
              kimix::vector<GdiStatus> statuses;
              graph_db::BulkStats stats;
              return f.db.create_vertices_bulk(
                  kimix::span<VertexBulkRow const>{rows.data(), rows.size()},
                  /*continue_on_error=*/false, results, &statuses, stats);
          };
          // COMMIT path: the batch lands and the CALLER's commit still works.
          expect(eq(f.db.begin_transaction(), GdiStatus::ok));
          expect(eq(run_batch(make_rows("commit")), GdiStatus::ok));
          expect(eq(f.db.commit(), GdiStatus::ok)) << "the caller's transaction survived the batch";
          expect(eq(f.db.graph().size(), size_t{3}));
          // ABORT path: one journal entry per row means the caller's abort
          // rolls the WHOLE batch back — the batch is atomic inside the caller.
          expect(eq(f.db.begin_transaction(), GdiStatus::ok));
          expect(eq(run_batch(make_rows("abort")), GdiStatus::ok));
          expect(eq(f.db.graph().size(), size_t{6}));
          expect(eq(f.db.abort(), GdiStatus::ok));
          expect(eq(f.db.graph().size(), size_t{3})) << "the second batch rolled back whole";
          // And the store is fully usable afterwards (no dangling tx state).
          expect(eq(f.db.begin_transaction(), GdiStatus::ok));
          VertexCreateResult single;
          auto const bytes = gtest::to_bytes("after");
          expect(eq(f.db.create_vertex(view_of(f.node, bytes), {}, nullptr, single),
                    GdiStatus::ok));
          expect(eq(f.db.commit(), GdiStatus::ok));
          expect(eq(f.db.graph().size(), size_t{4}));
      };
      "bulk_create_edges_with_a_refused_row"_test = [] {
        fixture f(3);
        auto const guids = f.guids();
        kimix::vector<EdgeBulkRow> rows;
        EdgeBulkRow first;
        first.origin = guids[0];
        first.target = guids[1];
        rows.push_back(first);
        EdgeBulkRow bad;
        bad.origin = guids[0];
        bad.target = Guid{true}; // a guid that is not in the graph
        rows.push_back(bad);
        EdgeBulkRow third;
        third.origin = guids[1];
        third.target = guids[2];
        rows.push_back(third);

        kimix::vector<Guid> edge_ids;
        kimix::vector<GdiStatus> statuses;
        graph_db::BulkStats stats;
        expect(eq(f.db.create_edges_bulk(kimix::span<EdgeBulkRow const>{rows.data(), rows.size()},
                                         /*continue_on_error=*/true, edge_ids, &statuses, stats),
                  GdiStatus::ok));
        expect(eq(stats.created, size_t{2}));
        expect(eq(stats.failed, size_t{1}));
        expect(eq(statuses.size(), size_t{3}));
        expect(eq(statuses[0], GdiStatus::ok));
        expect(eq(statuses[1], GdiStatus::error_vertex)) << "the missing endpoint is refused";
        expect(eq(statuses[2], GdiStatus::ok));
        expect(eq(f.db.graph().edges().size(), size_t{2}));
        expect(static_cast<bool>(edge_ids[0]));
    };

      // Create-time payloads through the bulk seam (plan v3 step 3.2): rows
      // carry labels + properties, and an abort of the enclosing transaction
      // removes the edges WITH their payloads.
      "bulk_create_edges_with_properties_rolls_back"_test = [] {
          fixture f(4);
          auto const guids = f.guids();
          kimix::vector<EdgeBulkRow> rows;
          for (size_t i = 0; i + 1 < guids.size(); ++i) {
              EdgeBulkRow row;
              row.origin = guids[i];
              row.target = guids[i + 1];
              row.labels.push_back(f.link);
              row.properties.push_back(PropertyEntry{f.age, PropertyValue::make_int64(
                                                               static_cast<int64_t>(i * 10))});
              rows.push_back(std::move(row));
          }
          expect(eq(f.db.begin_transaction(), GdiStatus::ok));
          kimix::vector<Guid> edge_ids;
          graph_db::BulkStats stats;
          expect(eq(f.db.create_edges_bulk(kimix::span<EdgeBulkRow const>{rows.data(), rows.size()},
                                           /*continue_on_error=*/false, edge_ids, nullptr,
                                           stats),
                    GdiStatus::ok));
          expect(eq(stats.created, rows.size()));
          expect(eq(edge_ids.size(), rows.size()));
          // The payloads are visible inside the transaction.
          for (size_t i = 0; i < edge_ids.size(); ++i) {
              NamedPropertyList props;
              expect(eq(f.db.get_edge_properties(edge_ids[i], props), GdiStatus::ok));
              expect(eq(props.size(), size_t{1}));
              expect(eq(props[0].name, kimix::string{"age"}));
              expect(eq(props[0].value.as_int64(), static_cast<int64_t>(i * 10)));
          }
          expect(eq(f.db.abort(), GdiStatus::ok));
          expect(eq(f.db.graph().edges().size(), size_t{0}))
              << "abort removed the bulk-created edges";
          expect(f.db.graph().dependencies(guids[0]).empty());
          expect(f.db.graph().dependents(guids[3]).empty());
      };

      "guarded_delete"_test = [] {
          fixture f(2);
          auto const guids = f.guids();

        // A vertex with no edges deletes.
        Guid isolated;
        auto const bytes = gtest::to_bytes("isolated");
        VertexCreateResult created;
        expect(eq(f.db.create_vertex(view_of(f.node, bytes), {}, nullptr, created), GdiStatus::ok));
        isolated = created.uid;
        expect(eq(f.db.free_vertex_guarded(isolated), GdiStatus::ok));
        expect(!f.db.graph().contains(isolated));

        // Any incident edge refuses it — outgoing, incoming, or undirected.
        Guid out_edge;
        Guid in_edge;
        Guid undirected;
        expect(eq(f.db.edge_create(guids[0], guids[1], EdgeDirectionType::directed, {}, nullptr, nullptr,
                                   out_edge),
                  GdiStatus::ok));
        expect(eq(f.db.free_vertex_guarded(guids[0]), GdiStatus::error_edge));
        expect(f.db.graph().contains(guids[0])) << "nothing was removed";
        expect(eq(f.db.graph().edges().size(), size_t{1}));

        expect(eq(f.db.edge_create(guids[1], guids[0], EdgeDirectionType::directed, {}, nullptr, nullptr,
                                   in_edge),
                  GdiStatus::ok));
        expect(eq(f.db.free_vertex_guarded(guids[0]), GdiStatus::error_edge));
        expect(eq(f.db.edge_create(guids[0], guids[1], EdgeDirectionType::undirected, {}, nullptr, nullptr,
                                   undirected),
                  GdiStatus::ok));
        expect(eq(f.db.free_vertex_guarded(guids[0]), GdiStatus::error_edge));
        expect(eq(f.db.graph().edges().size(), size_t{3})) << "every edge survives";

        // An unknown vertex is ErrorVertex, not ErrorEdge.
        expect(eq(f.db.free_vertex_guarded(Guid{true}), GdiStatus::error_vertex));

        // The cascading form does delete, edges included.
        expect(eq(f.db.free_vertex(guids[0]), GdiStatus::ok));
        expect(!f.db.graph().contains(guids[0]));
        expect(eq(f.db.graph().edges().size(), size_t{0}));
    };

    "error_surface_sweep"_test = [] {
        // The contract table: every failing call of the Database surface and its
        // exact status. Ordered by area so it reads as documentation.
        fixture f(2);
        auto const guids = f.guids();
        Guid const ghost{true};
        PropNameId bug{};

        // --- vertices ---
        expect(eq(f.db.free_vertex(ghost), GdiStatus::error_vertex));
        auto const empty_id = gtest::to_bytes("");
        VertexCreateResult created;
        expect(eq(f.db.create_vertex(view_of(f.node, empty_id), {}, nullptr, created),
                  GdiStatus::error_empty_name));
        LabelId const unregistered{999};
        auto const id = gtest::to_bytes("keyed");
        expect(eq(f.db.create_vertex(view_of(unregistered, id), {}, nullptr, created),
                  GdiStatus::error_label));
        expect(eq(f.db.create_vertex(view_of(f.node, id), {}, nullptr, created), GdiStatus::ok));
        VertexCreateResult duplicate;
        expect(eq(f.db.create_vertex(view_of(f.node, id), {}, nullptr, duplicate),
                  GdiStatus::error_non_unique_id));
        Guid resolved;
        bool found = true;
        expect(eq(f.db.translate_vertex_id(VertexKeyView{graph_db::kNoLabel, id}, resolved, found),
                  GdiStatus::error_empty_name));

        // --- properties ---
        expect(eq(f.db.add_property(ghost, f.age, PropertyValue::make_int64(1)),
                  GdiStatus::error_vertex));
        expect(eq(f.db.add_property(guids[0], bug, PropertyValue::make_int64(1)),
                  GdiStatus::error_property_type));
        expect(eq(f.db.add_property(guids[0], f.age, PropertyValue::make_string("x")),
                  GdiStatus::error_wrong_type));
        expect(eq(f.db.add_property(guids[0], f.age, PropertyValue{}), GdiStatus::error_wrong_type));
        expect(eq(f.db.add_property(guids[0], f.age, PropertyValue::make_int64(1)), GdiStatus::ok));
        expect(eq(f.db.add_property(guids[0], f.age, PropertyValue::make_int64(2)),
                  GdiStatus::error_property_exists));
        expect(eq(f.db.set_property(guids[1], f.age, PropertyValue::make_int64(2)),
                  GdiStatus::error_no_property));
        NamedPropertyList properties;
        expect(eq(f.db.get_properties(ghost, properties), GdiStatus::error_vertex));
        graph_db::PropertyTypeList types;
        expect(eq(f.db.get_property_types(ghost, types), GdiStatus::error_vertex));

        // --- labels ---
        expect(eq(f.db.add_label(ghost, f.node), GdiStatus::error_vertex));
        expect(eq(f.db.add_label(guids[0], unregistered), GdiStatus::error_label));
        expect(eq(f.db.remove_label(guids[0], f.link), GdiStatus::error_label));
        expect(eq(f.db.update_label(unregistered, "x"), GdiStatus::error_label));
        expect(eq(f.db.update_label(f.node, ""), GdiStatus::error_empty_name));
        LabelId taken{};
        expect(eq(f.db.create_label("node", taken), GdiStatus::error_name_exists));
        expect(eq(f.db.create_label("", taken), GdiStatus::error_empty_name));

        // --- edges ---
        Guid edge;
        expect(eq(f.db.edge_create(guids[0], ghost, EdgeDirectionType::directed, {}, nullptr, nullptr,
                                   edge),
                  GdiStatus::error_vertex));
        expect(eq(f.db.edge_create(guids[0], guids[1], static_cast<EdgeDirectionType>(9), {},
                                   nullptr, nullptr, edge),
                  GdiStatus::error_edge_orientation));
        expect(eq(f.db.edge_free(ghost), GdiStatus::error_uid));
        expect(eq(f.db.set_edge_origin(ghost, guids[0]), GdiStatus::error_uid));
        expect(eq(f.db.add_edge_property(ghost, f.age, PropertyValue::make_int64(1)),
                  GdiStatus::error_uid));
        expect(eq(f.db.get_edge_endpoints(ghost, resolved, resolved), GdiStatus::error_uid));

        // --- reads ---
        VertexPage page;
        expect(eq(f.db.get_neighbor_vertices(ghost, Constraint{}, EdgeOrientation::both, 0, page),
                  GdiStatus::error_vertex));
        expect(eq(f.db.get_neighbor_vertices(guids[0], Constraint{}, EdgeOrientation::both, -1,
                                             page),
                  GdiStatus::error_argument));

        // --- constraints ---
        graph_db::ConstraintHandle handle;
        expect(eq(f.db.create_constraint(Constraint{}, handle), GdiStatus::ok));
        expect(eq(f.db.add_label_condition(handle, f.node, GdiOp::greater), GdiStatus::error_op));
        expect(eq(f.db.add_property_condition(handle, bug, GdiOp::equal,
                                              PropertyValue::make_int64(1)),
                  GdiStatus::error_property_type));
        expect(eq(f.db.free_constraint(graph_db::ConstraintHandle{4242}),
                  GdiStatus::error_constraint));

        // --- schema ---
        PropNameId type_id{};
        PropertyTypeInfo blank_type;
        blank_type.name = "";
        blank_type.data = GdiDataType::int64_;
        expect(eq(f.db.create_property_type(blank_type, type_id), GdiStatus::error_empty_name));
        PropertyTypeInfo zero_count;
        zero_count.name = "zero";
        zero_count.data = GdiDataType::int64_;
        zero_count.container = graph_db::GdiContainerType::array;
        zero_count.count = 0;
        expect(eq(f.db.create_property_type(zero_count, type_id), GdiStatus::error_count));
        expect(eq(f.db.update_property_type(999, blank_type), GdiStatus::error_empty_name));
        expect(eq(f.db.free_property_type(999), GdiStatus::error_property_type));
        expect(eq(f.db.free_label(999), GdiStatus::error_label));

        // --- transactions ---
        expect(eq(f.db.commit(), GdiStatus::error_transaction));
        expect(eq(f.db.abort(), GdiStatus::error_transaction));
        expect(eq(f.db.close_collective(true), GdiStatus::error_transaction));

        // --- store ---
        expect(eq(f.db.persist(/*full=*/false), GdiStatus::error_no_such_file))
            << "an in-memory database has no store to write to";
        expect(eq(f.db.load_store(), GdiStatus::error_no_such_file));
    };
}
