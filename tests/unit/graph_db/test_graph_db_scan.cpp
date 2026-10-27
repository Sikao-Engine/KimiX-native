// Test for the global vertex scan (src/graph_db/graph.h Graph::for_each_vertex
// + database.h/.cpp Database::scan_vertices — G1, plan v3 step 2.2) and the
// global aggregates (database.h/.cpp Database::aggregate_vertices with the
// fiber fan-out — G4, plan v3 step 2.3).
//
// This test covers:
// - the unfiltered paging matrix: cap -1 / 0 (count probe) / 1 / 5 / N / large
//   over an 11-vertex store, asserting items.size() AND total_count for each
//   (GDI's two-phase count/fetch contract) and that every page is a strict
//   prefix of the unbounded scan order;
// - the constraint matrix: label equal / not-equal, property equal /
//   not-equal / greater / greater-or-equal / less / less-or-equal over int64,
//   double, string (ordinal), blob and an int64 array, the int64-vs-double
//   cross comparison, a missing property, an AND of label + property, plus
//   membership of the returned set (not just its size);
// - handle equivalence: a constraint built through the handle API
//   (create_constraint + add_*_condition) scans exactly like the same
//   definition handed in directly;
// - determinism: repeated scans return the identical sequence, so a caller can
//   page over a stable order;
// - the Graph-level iterator: early stop (returning false), an empty graph,
//   and per-visit order stability;
// - the empty graph at the Database level: ok, 0 items, total_count 0;
// - the aggregates: count / min / max / sum / avg over every property kind the
//   fixture declares (int64, double, ordinal string, blob, int64 array),
//   filtered and unfiltered, the missing-property skip, the legality matrix
//   (error_op / error_property_type / error_argument decided before the scan),
//   and the parallel path: with the 4-worker fiber scheduler bound, repeated
//   runs return bit-identical results (shard-order merge) that equal the
//   hand-computed serial expectations.
#include "ut/ut.hpp"

#include "graph_db_test_util.h"
#include <core/kimix_core.h> // umbrella first: it fixes the winsock2.h order
#include <core/fiber.h>
#include <graph_db/database.h>
#include <graph_db/graph.h>
#include <graph_db/graph_db.h>
#include <graph_db/query.h>
#include <core/guid.h>
#include <core/stl/unordered_map.h> // kimix::unordered_set lives here
#include <core/stl/vector.h>
#include <cstddef>
#include <cstdint>
using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using graph_db::AggregateResult;
using graph_db::AggregateSpec;
using graph_db::Constraint;
using graph_db::ConstraintCondition;
using graph_db::ConstraintHandle;
using graph_db::Database;
using graph_db::GdiAggregate;
using graph_db::EdgeDirectionType;
using graph_db::GdiContainerType;
using graph_db::GdiDataType;
using graph_db::GdiOp;
using graph_db::GdiStatus;
using graph_db::Graph;
using graph_db::LabelId;
using graph_db::PropNameId;
using graph_db::PropertyKind;
using graph_db::PropertyTypeInfo;
using graph_db::PropertyValue;
using graph_db::Vertex;
using graph_db::VertexCreateResult;
using graph_db::VertexKeyView;
using graph_db::VertexPage;
namespace gtest = kimix::graph_db::test;
namespace tests_unit_graph_db_test_graph_db_scan {
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
// The same shape the read suite uses: `count` keyed vertices "n0".."n9" with
// age = index, ratio = index + 0.5, tag = "tag{i%3}", a 2-byte blob, a 2-slot
// int64 array {i, 0}, a `warm` label on the even indices, plus an unlabeled
// hub vertex (11 vertices total at count = 10).
struct fixture {
    Database db;
    LabelId node{};
    LabelId warm{};
    PropNameId age{};
    PropNameId ratio{};
    PropNameId tag{};
    PropNameId blob{};
    PropNameId ages{};
    kimix::vector<Guid> vertices;
    Guid hub;
    explicit fixture(size_t count = 10) {
        expect(eq(db.schema().create_label("node", node), GdiStatus::ok));
        expect(eq(db.schema().create_label("warm", warm), GdiStatus::ok));
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
                                      PropertyValue::make_string(kimix::format("tag{}", i % 3))),
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
        auto const hub_bytes = gtest::to_bytes("hub");
        VertexCreateResult created_hub;
        expect(eq(db.create_vertex(view_of(node, hub_bytes), {}, nullptr, created_hub),
                  GdiStatus::ok));
        hub = created_hub.uid;
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
// Strict prefix check: page.items == full_scan[0 .. page.items.size()).
bool is_prefix(kimix::vector<Guid> const &full, VertexPage const &page) {
    if (page.items.size() > full.size()) {
        return false;
    }
    for (size_t i = 0; i < page.items.size(); ++i) {
        if (!(page.items[i] == full[i])) {
            return false;
        }
    }
    return true;
}
kimix::unordered_set<Guid> guid_set(VertexPage const &page) {
    kimix::unordered_set<Guid> set;
    set.reserve(page.items.size());
    for (auto const &guid : page.items) {
        set.insert(guid);
    }
    return set;
}
} // namespace tests_unit_graph_db_test_graph_db_scan
using namespace tests_unit_graph_db_test_graph_db_scan;
int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));
    kimix::fiber::scheduler sched{4u};
    "scan_unfiltered_paging"_test = [] {
        fixture f(10); // 11 vertices in total
        VertexPage page;
        expect(eq(f.db.scan_vertices(Constraint{}, -1, page), GdiStatus::error_argument));
        // Count probe: no items, the real total.
        expect(eq(f.db.scan_vertices(Constraint{}, 0, page), GdiStatus::ok));
        expect(eq(page.items.size(), size_t{0}));
        expect(eq(page.total_count, size_t{11}));
        // The unbounded reference order.
        expect(eq(f.db.scan_vertices(Constraint{}, 1'000'000, page), GdiStatus::ok));
        expect(eq(page.items.size(), size_t{11}));
        expect(eq(page.total_count, size_t{11}));
        kimix::vector<Guid> const full = page.items;
        for (int64_t const cap : {1, 2, 5, 10, 11}) {
            expect(eq(f.db.scan_vertices(Constraint{}, cap, page), GdiStatus::ok));
            expect(eq(page.items.size(), static_cast<size_t>(cap)));
            expect(eq(page.total_count, size_t{11}));
            expect(is_prefix(full, page));
        }
    };
    "scan_constraint_matrix"_test = [] {
        fixture f(10);
        VertexPage page;
        // Label equal: the even vertices only (the hub is unlabeled).
        expect(eq(f.db.scan_vertices(f.label_constraint(f.warm, GdiOp::equal), 0, page),
                  GdiStatus::ok));
        expect(eq(page.total_count, size_t{5}));
        expect(eq(f.db.scan_vertices(f.label_constraint(f.warm, GdiOp::equal), 1'000'000, page),
                  GdiStatus::ok));
        expect(eq(page.items.size(), size_t{5}));
        auto warm = guid_set(page);
        for (size_t i = 0; i < 10; ++i) {
            expect(eq(warm.contains(f.vertices[i]), i % 2 == 0));
        }
        expect(eq(warm.contains(f.hub), false));
        // Label not-equal: the odd vertices AND the hub.
        expect(eq(f.db.scan_vertices(f.label_constraint(f.warm, GdiOp::not_equal), 0, page),
                  GdiStatus::ok));
        expect(eq(page.total_count, size_t{6}));
        expect(eq(f.db.scan_vertices(f.label_constraint(f.warm, GdiOp::not_equal), 1'000'000,
                                     page),
                  GdiStatus::ok));
        auto cold = guid_set(page);
        expect(eq(cold.contains(f.hub), true));
        for (size_t i = 0; i < 10; ++i) {
            expect(eq(cold.contains(f.vertices[i]), i % 2 == 1));
        }
        // int64 property comparisons (age = index; the hub has no age).
        expect(eq(f.db.scan_vertices(f.property_constraint(f.age, GdiOp::equal,
                                                           PropertyValue::make_int64(4)),
                                     0, page),
                  GdiStatus::ok));
        expect(eq(page.total_count, size_t{1}));
        expect(eq(f.db.scan_vertices(f.property_constraint(f.age, GdiOp::equal,
                                                           PropertyValue::make_int64(4)),
                                     1'000'000, page),
                  GdiStatus::ok));
        expect(eq(page.items.size(), size_t{1}));
        expect(eq(page.items[0] == f.vertices[4], true));
        expect(eq(f.db.scan_vertices(f.property_constraint(f.age, GdiOp::greater,
                                                           PropertyValue::make_int64(6)),
                                     0, page),
                  GdiStatus::ok));
        expect(eq(page.total_count, size_t{3})); // n7, n8, n9
        expect(eq(f.db.scan_vertices(f.property_constraint(f.age, GdiOp::greater_or_equal,
                                                           PropertyValue::make_int64(8)),
                                     0, page),
                  GdiStatus::ok));
        expect(eq(page.total_count, size_t{2}));
        expect(eq(f.db.scan_vertices(f.property_constraint(f.age, GdiOp::less,
                                                           PropertyValue::make_int64(2)),
                                     0, page),
                  GdiStatus::ok));
        expect(eq(page.total_count, size_t{2})); // n0, n1
        expect(eq(f.db.scan_vertices(f.property_constraint(f.age, GdiOp::less_or_equal,
                                                           PropertyValue::make_int64(1)),
                                     0, page),
                  GdiStatus::ok));
        expect(eq(page.total_count, size_t{2}));
        expect(eq(f.db.scan_vertices(f.property_constraint(f.age, GdiOp::not_equal,
                                                           PropertyValue::make_int64(5)),
                                     0, page),
                  GdiStatus::ok));
        expect(eq(page.total_count, size_t{9})); // every age except 5
        // double property (ratio = index + 0.5).
        expect(eq(f.db.scan_vertices(f.property_constraint(f.ratio, GdiOp::greater,
                                                           PropertyValue::make_double(8.5)),
                                     0, page),
                  GdiStatus::ok));
        expect(eq(page.total_count, size_t{1})); // n9
        // Cross-kind: an int64 property compared against a double value.
        expect(eq(f.db.scan_vertices(f.property_constraint(f.age, GdiOp::greater,
                                                           PropertyValue::make_double(6.5)),
                                     0, page),
                  GdiStatus::ok));
        expect(eq(page.total_count, size_t{3})); // n7, n8, n9
        // Ordinal strings (tag = "tag{i%3}").
        expect(eq(f.db.scan_vertices(f.property_constraint(f.tag, GdiOp::equal,
                                                           PropertyValue::make_string("tag1")),
                                     0, page),
                  GdiStatus::ok));
        expect(eq(page.total_count, size_t{3})); // n1, n4, n7
        expect(eq(f.db.scan_vertices(f.property_constraint(f.tag, GdiOp::less,
                                                           PropertyValue::make_string("tag2")),
                                     0, page),
                  GdiStatus::ok));
        expect(eq(page.total_count, size_t{7})); // tag0 + tag1 rows
        // Blob equality (blob = {i%2, 0x7f}).
        std::byte const odd_blob[2]{std::byte{1}, std::byte{0x7f}};
        expect(eq(f.db.scan_vertices(f.property_constraint(
                                         f.blob, GdiOp::equal,
                                         PropertyValue::make_blob(
                                             kimix::span<const std::byte>{odd_blob, 2})),
                                     0, page),
                  GdiStatus::ok));
        expect(eq(page.total_count, size_t{5})); // the odd rows
        // int64 array equality (ages = {i, 0}).
        int64_t const zero_pair[2]{0, 0};
        expect(eq(f.db.scan_vertices(f.property_constraint(
                                         f.ages, GdiOp::equal,
                                         PropertyValue::make_int64_array(
                                             kimix::span<const int64_t>{zero_pair, 2})),
                                     0, page),
                  GdiStatus::ok));
        expect(eq(page.total_count, size_t{1})); // n0
        // A property condition requires the property to be PRESENT: a name id
        // nothing carries matches nothing.
        expect(eq(f.db.scan_vertices(f.property_constraint(PropNameId{999}, GdiOp::equal,
                                                           PropertyValue::make_int64(0)),
                                     0, page),
                  GdiStatus::ok));
        expect(eq(page.total_count, size_t{0}));
        // AND of a label and a property: warm AND age >= 6 -> n6, n8.
        Constraint both;
        both.conditions.push_back(ConstraintCondition::on_label(f.warm, GdiOp::equal));
        both.conditions.push_back(
            ConstraintCondition::on_property(f.age, GdiOp::greater_or_equal,
                                             PropertyValue::make_int64(6)));
        expect(eq(f.db.scan_vertices(both, 0, page), GdiStatus::ok));
        expect(eq(page.total_count, size_t{2}));
        expect(eq(f.db.scan_vertices(both, 1'000'000, page), GdiStatus::ok));
        auto both_set = guid_set(page);
        expect(eq(both_set.contains(f.vertices[6]), true));
        expect(eq(both_set.contains(f.vertices[8]), true));
        // An empty constraint matches everything.
        expect(eq(f.db.scan_vertices(Constraint{}, 0, page), GdiStatus::ok));
        expect(eq(page.total_count, size_t{11}));
    };
    "scan_handle_equivalence"_test = [] {
        fixture f(10);
        // Build the same constraint two ways: through the handle API and as a
        // plain definition. Both must scan identically (handle path exercises
        // the resolved-handle branch of _resolve_constraint).
        ConstraintHandle handle{};
        expect(eq(f.db.create_constraint(Constraint{}, handle), GdiStatus::ok));
        expect(eq(f.db.add_label_condition(handle, f.warm, GdiOp::equal), GdiStatus::ok));
        expect(eq(f.db.add_property_condition(handle, f.age, GdiOp::greater_or_equal,
                                              PropertyValue::make_int64(4)),
                  GdiStatus::ok));
        Constraint via_handle;
        expect(eq(f.db.schema().constraint(handle, via_handle), true));
        Constraint direct;
        direct.conditions.push_back(ConstraintCondition::on_label(f.warm, GdiOp::equal));
        direct.conditions.push_back(
            ConstraintCondition::on_property(f.age, GdiOp::greater_or_equal,
                                             PropertyValue::make_int64(4)));
        VertexPage a;
        VertexPage b;
        expect(eq(f.db.scan_vertices(via_handle, 1'000'000, a), GdiStatus::ok));
        expect(eq(f.db.scan_vertices(direct, 1'000'000, b), GdiStatus::ok));
        expect(eq(a.total_count, b.total_count));
        expect(eq(a.items.size(), b.items.size()));
        expect(eq(a.items.size(), a.total_count));
        for (size_t i = 0; i < a.items.size(); ++i) {
            expect(eq(a.items[i] == b.items[i], true));
        }
        expect(eq(f.db.free_constraint(handle), GdiStatus::ok));
    };
    "scan_determinism"_test = [] {
        fixture f(10);
        VertexPage first;
        expect(eq(f.db.scan_vertices(Constraint{}, 1'000'000, first), GdiStatus::ok));
        for (int repeat = 0; repeat < 10; ++repeat) {
            VertexPage page;
            expect(eq(f.db.scan_vertices(Constraint{}, 1'000'000, page), GdiStatus::ok));
            expect(eq(page.items.size(), first.items.size()));
            for (size_t i = 0; i < first.items.size(); ++i) {
                expect(eq(page.items[i] == first.items[i], true));
            }
        }
        // A filtered scan is deterministic too.
        Constraint warm;
        warm.conditions.push_back(ConstraintCondition::on_label(f.warm, GdiOp::equal));
        expect(eq(f.db.scan_vertices(warm, 1'000'000, first), GdiStatus::ok));
        for (int repeat = 0; repeat < 5; ++repeat) {
            VertexPage page;
            expect(eq(f.db.scan_vertices(warm, 1'000'000, page), GdiStatus::ok));
            expect(eq(page.items.size(), first.items.size()));
            for (size_t i = 0; i < first.items.size(); ++i) {
                expect(eq(page.items[i] == first.items[i], true));
            }
        }
    };
    "scan_empty_graph"_test = [] {
        Database db;
        VertexPage page;
        expect(eq(db.scan_vertices(Constraint{}, 0, page), GdiStatus::ok));
        expect(eq(page.items.size(), size_t{0}));
        expect(eq(page.total_count, size_t{0}));
        expect(eq(db.scan_vertices(Constraint{}, 10, page), GdiStatus::ok));
        expect(eq(page.items.size(), size_t{0}));
        expect(eq(page.total_count, size_t{0}));
        // A constraint over an empty graph is also an empty (ok) page.
        Constraint c;
        c.conditions.push_back(
            ConstraintCondition::on_label(LabelId{1}, GdiOp::equal));
        expect(eq(db.scan_vertices(c, 10, page), GdiStatus::ok));
        expect(eq(page.total_count, size_t{0}));
    };
    "graph_for_each_vertex"_test = [] {
        Graph graph;
        kimix::vector<Vertex *> made;
        for (int i = 0; i < 10; ++i) {
            made.push_back(graph.create_vertex());
        }
        // Full scan sees every vertex exactly once.
        size_t calls = 0;
        kimix::unordered_set<Guid> seen;
        expect(eq(graph.for_each_vertex([&](Vertex const &vertex) {
              ++calls;
              seen.insert(vertex.guid());
              return true;
          }),
                  GdiStatus::ok));
        expect(eq(calls, size_t{10}));
        expect(eq(seen.size(), size_t{10}));
        for (auto const *vertex : made) {
            expect(eq(seen.contains(vertex->guid()), true));
        }
        // Early stop: returning false ends the scan immediately.
        calls = 0;
        expect(eq(graph.for_each_vertex([&](Vertex const &) {
              ++calls;
              return calls < 3;
          }),
                  GdiStatus::ok));
        expect(eq(calls, size_t{3}));
        // Order is stable across runs (shard-ascending, map order within).
        kimix::vector<Guid> run_a;
        kimix::vector<Guid> run_b;
        graph.for_each_vertex([&](Vertex const &vertex) {
            run_a.push_back(vertex.guid());
            return true;
        });
        graph.for_each_vertex([&](Vertex const &vertex) {
            run_b.push_back(vertex.guid());
            return true;
        });
        expect(eq(run_a.size(), run_b.size()));
        for (size_t i = 0; i < run_a.size(); ++i) {
            expect(eq(run_a[i] == run_b[i], true));
        }
        // An empty graph invokes the callback never.
        Graph empty;
        calls = 0;
        expect(eq(empty.for_each_vertex([&](Vertex const &) {
              ++calls;
              return true;
          }),
                  GdiStatus::ok));
        expect(eq(calls, size_t{0}));
    };
    "aggregate_count"_test = [] {
        fixture f(10); // 10 property-carrying vertices + the bare hub
        AggregateResult result;
        AggregateSpec spec;
        spec.function = GdiAggregate::count;
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::ok));
        expect(eq(result.matched, uint64_t{11}));
        expect(eq(result.valued, uint64_t{0}));
        expect(eq(result.has_value, true));
        expect(eq(result.value.kind(), PropertyKind::int64_value));
        expect(eq(result.value.as_int64(), int64_t{11}));
        // Filtered: the even vertices only.
        spec.constraint = f.label_constraint(f.warm, GdiOp::equal);
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::ok));
        expect(eq(result.matched, uint64_t{5}));
        expect(eq(result.value.as_int64(), int64_t{5}));
        // A constraint matching nothing: no value, the zero is in `matched`.
        spec.constraint =
            f.property_constraint(f.age, GdiOp::greater, PropertyValue::make_int64(1000));
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::ok));
        expect(eq(result.matched, uint64_t{0}));
        expect(eq(result.has_value, false));
        expect(eq(result.value.kind(), PropertyKind::none));
        // count ignores the property entirely (even a blob one).
        spec.constraint = Constraint{};
        spec.property = f.blob;
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::ok));
        expect(eq(result.matched, uint64_t{11}));
    };
    "aggregate_min_max"_test = [] {
        fixture f(10);
        AggregateResult result;
        AggregateSpec spec;
        spec.function = GdiAggregate::min;
        spec.property = f.age;
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::ok));
        expect(eq(result.matched, uint64_t{11}));
        expect(eq(result.valued, uint64_t{10})); // the hub has no age
        expect(eq(result.has_value, true));
        expect(eq(result.value.kind(), PropertyKind::int64_value));
        expect(eq(result.value.as_int64(), int64_t{0}));
        spec.function = GdiAggregate::max;
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::ok));
        expect(eq(result.value.as_int64(), int64_t{9}));
        // doubles keep their kind and value.
        spec.property = f.ratio;
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::ok));
        expect(eq(result.value.kind(), PropertyKind::double_value));
        expect(eq(result.value.as_double(), 9.5));
        spec.function = GdiAggregate::min;
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::ok));
        expect(eq(result.value.as_double(), 0.5));
        // strings order ordinally.
        spec.property = f.tag;
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::ok));
        expect(eq(result.value.kind(), PropertyKind::string));
        expect(result.value.as_string() == kimix::string_view{"tag0"});
        spec.function = GdiAggregate::max;
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::ok));
        expect(result.value.as_string() == kimix::string_view{"tag2"});
        // Filtered min/max (warm = even indices).
        spec.property = f.age;
        spec.constraint = f.label_constraint(f.warm, GdiOp::equal);
        spec.function = GdiAggregate::min;
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::ok));
        expect(eq(result.matched, uint64_t{5}));
        expect(eq(result.valued, uint64_t{5}));
        expect(eq(result.value.as_int64(), int64_t{0}));
        spec.function = GdiAggregate::max;
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::ok));
        expect(eq(result.value.as_int64(), int64_t{8}));
    };
    "aggregate_sum_avg"_test = [] {
        fixture f(10); // ages 0..9 (sum 45), ratios i + 0.5 (sum 50.0)
        AggregateResult result;
        AggregateSpec spec;
        spec.function = GdiAggregate::sum;
        spec.property = f.age;
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::ok));
        expect(eq(result.valued, uint64_t{10}));
        expect(eq(result.has_value, true));
        expect(eq(result.value.kind(), PropertyKind::int64_value));
        expect(eq(result.value.as_int64(), int64_t{45}));
        spec.function = GdiAggregate::avg;
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::ok));
        expect(eq(result.value.kind(), PropertyKind::double_value));
        expect(eq(result.value.as_double(), 4.5));
        // doubles sum as doubles.
        spec.property = f.ratio;
        spec.function = GdiAggregate::sum;
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::ok));
        expect(eq(result.value.kind(), PropertyKind::double_value));
        expect(eq(result.value.as_double(), 50.0));
        spec.function = GdiAggregate::avg;
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::ok));
        expect(eq(result.value.as_double(), 5.0));
        // Filtered: warm vertices carry ages 0,2,4,6,8 (sum 20, avg 4.0).
        spec.property = f.age;
        spec.constraint = f.label_constraint(f.warm, GdiOp::equal);
        spec.function = GdiAggregate::sum;
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::ok));
        expect(eq(result.valued, uint64_t{5}));
        expect(eq(result.value.as_int64(), int64_t{20}));
        spec.function = GdiAggregate::avg;
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::ok));
        expect(eq(result.value.as_double(), 4.0));
    };
    "aggregate_legality"_test = [] {
        fixture f(10);
        AggregateResult result;
        AggregateSpec spec;
        // sum/avg need a numeric scalar declaration: char_ (tag), byte_ (blob)
        // and the int64 ARRAY (ages) are all refused with error_op, as is
        // min/max on blob — before any scan, whatever the data says.
        spec.function = GdiAggregate::sum;
        spec.property = f.tag;
        spec.function = GdiAggregate::sum;
        spec.property = f.blob;
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::error_op));
        spec.property = f.ages; // the int64 ARRAY declaration
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::error_op));
        spec.function = GdiAggregate::avg;
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::error_op));
        spec.function = GdiAggregate::min;
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::error_op));
        // An unknown property name id.
        spec.function = GdiAggregate::sum;
        spec.property = PropNameId{999};
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::error_property_type));
        // A property reduction without a property.
        spec.property = graph_db::kNoPropName;
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::error_argument));
        // An unknown function value.
        spec.function = static_cast<GdiAggregate>(99);
        spec.property = f.age;
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::error_argument));
        // A label condition with an ordering op is error_op (the
        // _resolve_constraint rule), like every other read.
        spec.function = GdiAggregate::count;
        spec.constraint = f.label_constraint(f.warm, GdiOp::greater);
        expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::error_op));
    };
    "aggregate_parallel_determinism"_test = [] {
        fixture f(200); // enough vertices to spread over the 4 workers
        expect(gt(kimix::fiber::worker_thread_count(), 1u))
            << "the bound scheduler must fan the aggregate out";
        AggregateResult first;
        AggregateSpec spec;
        spec.function = GdiAggregate::avg;
        spec.property = f.ratio;
        expect(eq(f.db.aggregate_vertices(spec, first), GdiStatus::ok));
        expect(eq(first.has_value, true));
        expect(eq(first.value.as_double(), 100.0)); // mean of 0.5 .. 199.5
        // Repeated parallel runs are bit-identical (shard-order merge) and keep
        // matching the hand-computed expectation.
        for (int repeat = 0; repeat < 25; ++repeat) {
            AggregateResult result;
            expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::ok));
            expect(eq(result.matched, first.matched));
            expect(eq(result.valued, first.valued));
            expect(eq(result.has_value, first.has_value));
            expect(eq(result.value.kind(), first.value.kind()));
            expect(eq(result.value.as_double(), first.value.as_double()));
        }
        // A filtered double sum is deterministic too, and int64 sums stay exact.
        spec.function = GdiAggregate::sum;
        spec.property = f.age;
        spec.constraint = f.label_constraint(f.warm, GdiOp::equal);
        expect(eq(f.db.aggregate_vertices(spec, first), GdiStatus::ok));
        // warm = even indices 0..198: 100 values summing to 9900.
        expect(eq(first.value.as_int64(), int64_t{9900}));
        for (int repeat = 0; repeat < 10; ++repeat) {
            AggregateResult result;
            expect(eq(f.db.aggregate_vertices(spec, result), GdiStatus::ok));
            expect(eq(result.value.as_int64(), first.value.as_int64()));
        }
    };
}
