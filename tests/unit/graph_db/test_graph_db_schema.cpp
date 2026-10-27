// Test for the schema registry (src/graph_db/schema.*, Database schema commands)
// — GDI §2.5.
//
// This test covers:
// - the label lifecycle: create / duplicate (error_name_exists) / find-missing /
//   rename (with NO entity touched — entities store ids) / getAll sorted by id;
// - the FreeLabel cascade: three vertices and two edges carry the label, then it
//   is freed and every entity is clean;
// - the property-type lifecycle: create (name/container/dtype/count), duplicate,
//   find, getAll (sorted by name), update (rename ok, shape change refused while a
//   value exists), free;
// - the FreePropertyType cascade: every value of that type disappears from every
//   vertex AND every edge;
// - persistence: the schema survives a save/load cycle, ids included;
// - the auto-create path (GraphOptions::auto_create_schema) through
//   Schema::resolve_label / resolve_property_type, and the strict path's
//   error_label / error_property_type;
// - 10k labels / property types: create, find, free, and ids are NEVER recycled.
#include "ut/ut.hpp"

#include "graph_db_test_util.h"

#include <core/kimix_core.h> // umbrella first: it fixes the winsock2.h order
#include <core/fiber.h>

#include <graph_db/database.h>
#include <graph_db/graph_db.h>
#include <graph_db/schema.h>
#include <graph_db/status.h>

#include <core/guid.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <algorithm>
#include <cstddef>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using graph_db::Database;
using graph_db::EdgeRecord;
using graph_db::EdgeDirectionType;
using graph_db::GdiContainerType;
using graph_db::GdiDataType;
using graph_db::GdiStatus;
using graph_db::LabelId;
using graph_db::NamedLabelList;
using graph_db::PropNameId;
using graph_db::PropertyTypeInfo;
using graph_db::PropertyTypeList;
using graph_db::NamedPropertyList;
using graph_db::PropertyValue;
using graph_db::VertexCreateResult;
using graph_db::Vertex;
using graph_db::VertexKeyView;
namespace gtest = kimix::graph_db::test;

namespace tests_unit_graph_db_test_graph_db_schema {

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

} // namespace tests_unit_graph_db_test_graph_db_schema
using namespace tests_unit_graph_db_test_graph_db_schema;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    kimix::fiber::scheduler sched{4u};

    "schema_label_lifecycle"_test = [] {
        Database db;
        LabelId person{};
        expect(eq(db.create_label("person", person), GdiStatus::ok));
        expect(eq(db.create_label("person", person), GdiStatus::error_name_exists));
        expect(eq(db.create_label("", person), GdiStatus::error_empty_name));
        expect(eq(db.create_label("   ", person), GdiStatus::error_empty_name))
            << "a blank name is empty, not a name";

        LabelId found{};
        bool present = false;
        expect(eq(db.get_label_by_name("person", found, present), GdiStatus::ok));
        expect(present);
        expect(eq(found, person));
        expect(eq(db.get_label_by_name("nobody", found, present), GdiStatus::ok));
        expect(!present);

        // Rename keeps the id, and therefore every entity.
        auto const bytes = gtest::to_bytes("u1");
        VertexCreateResult created;
        expect(eq(db.create_vertex(view_of(person, bytes), {}, nullptr, created), GdiStatus::ok));
        LabelId const extra = person;
        expect(eq(db.add_label(created.uid, extra), GdiStatus::ok));
        expect(eq(db.update_label(person, "human"), GdiStatus::ok));
        expect(eq(db.get_label_by_name("human", found, present), GdiStatus::ok));
        expect(present);
        expect(eq(found, person)) << "the id is stable across a rename";
        expect(eq(db.get_label_by_name("person", found, present), GdiStatus::ok));
        expect(!present);
        // The entity still carries the (renamed) label, and it is still the key.
        kimix::vector<kimix::string> labels;
        expect(eq(db.get_labels(created.uid, labels), GdiStatus::ok));
        expect(labels[0] == kimix::string{"human"});

        expect(eq(db.update_label(person, ""), GdiStatus::error_empty_name));
        expect(eq(db.update_label(999, "x"), GdiStatus::error_label));

        // getAll returns every label, in id order.
        LabelId second{};
        expect(eq(db.create_label("second", second), GdiStatus::ok));
        NamedLabelList all;
        expect(eq(db.get_all_labels(all), GdiStatus::ok));
        expect(eq(all.size(), size_t{2}));
        expect(eq(all[0].id, person));
        expect(all[0].name == kimix::string{"human"});
        expect(eq(all[1].id, second));
    };

    "schema_free_label_cascades"_test = [] {
        Database db;
        LabelId node{};
        LabelId doomed{};
        expect(eq(db.schema().create_label("node", node), GdiStatus::ok));
        expect(eq(db.schema().create_label("doomed", doomed), GdiStatus::ok));

        kimix::vector<Guid> vertices;
        for (int i = 0; i < 3; ++i) {
            auto const bytes = gtest::to_bytes(kimix::format("v{}", i));
            VertexCreateResult created;
            expect(eq(db.create_vertex(view_of(node, bytes), {}, nullptr, created), GdiStatus::ok));
            expect(eq(db.add_label(created.uid, doomed), GdiStatus::ok));
            vertices.push_back(created.uid);
        }
        Guid edge;
        LabelId const edge_labels[]{doomed};
        expect(eq(db.edge_create(vertices[0], vertices[1], EdgeDirectionType::directed,
                                 kimix::span<const LabelId>{edge_labels, 1}, nullptr, edge),
                  GdiStatus::ok));
        Guid second_edge;
        expect(eq(db.edge_create(vertices[1], vertices[2], EdgeDirectionType::directed,
                                 kimix::span<const LabelId>{edge_labels, 1}, nullptr, second_edge),
                  GdiStatus::ok));

        expect(eq(db.free_label(doomed), GdiStatus::ok));
        expect(!db.schema().has_label(doomed));
        for (auto const &uid : vertices) {
            Vertex *vertex = db.graph().find(uid);
            expect(vertex != nullptr);
            expect(!vertex->has_label(doomed)) << "the label was stripped from every vertex";
            expect(eq(vertex->key_label(), node))
                << "the vertex is still keyed by `node` (the key label is not part of "
                   "the explicit label set, exactly like the reference backend)";
        }
        for (auto const &id : kimix::vector<Guid>{edge, second_edge}) {
            EdgeRecord record;
            expect(db.graph().edge_record(id, record));
            expect(record.labels.empty()) << "and from every edge";
        }
        expect(eq(db.free_label(doomed), GdiStatus::error_label))
            << "freeing an unknown label is ErrorLabel";
    };

    "schema_free_key_label_is_refused"_test = [] {
        // Freeing a label a live vertex uses as its KEY would orphan the key index,
        // so the Database refuses it with error_state (documented superset of the
        // reference backend, which has no key-label concept).
        Database db;
        LabelId person{};
        expect(eq(db.schema().create_label("person", person), GdiStatus::ok));
        auto const bytes = gtest::to_bytes("u1");
        VertexCreateResult created;
        expect(eq(db.create_vertex(view_of(person, bytes), {}, nullptr, created), GdiStatus::ok));
        expect(eq(db.free_label(person), GdiStatus::error_state));
        expect(db.schema().has_label(person));
        // Once the vertex is gone, the label is freeable.
        expect(eq(db.free_vertex(created.uid), GdiStatus::ok));
        expect(eq(db.free_label(person), GdiStatus::ok));
    };

    "schema_property_type_lifecycle"_test = [] {
        Database db;
        PropNameId age{};
        expect(eq(db.create_property_type(make_type("age", GdiDataType::int64_), age),
                  GdiStatus::ok));
        PropNameId duplicate{};
        expect(eq(db.create_property_type(make_type("age", GdiDataType::double_), duplicate),
                  GdiStatus::error_name_exists));
        PropNameId blank{};
        expect(eq(db.create_property_type(make_type("", GdiDataType::int64_), blank),
                  GdiStatus::error_empty_name));
        PropNameId zero_count{};
        PropertyTypeInfo bad = make_type("bad", GdiDataType::int64_);
        bad.container = GdiContainerType::array;
        bad.count = 0;
        expect(eq(db.create_property_type(bad, zero_count), GdiStatus::error_count));

        PropNameId found{};
        bool present = false;
        expect(eq(db.get_property_type_by_name("age", found, present), GdiStatus::ok));
        expect(present);
        expect(eq(found, age));
        expect(eq(db.get_property_type_by_name("nope", found, present), GdiStatus::ok));
        expect(!present);

        // getAll is sorted by name.
        PropNameId count{};
        expect(eq(db.create_property_type(make_type("count", GdiDataType::int64_, 4), count),
                  GdiStatus::ok));
        PropertyTypeList all;
        expect(eq(db.get_all_property_types(all), GdiStatus::ok));
        expect(eq(all.size(), size_t{2}));
        expect(all[0].name == kimix::string{"age"});
        expect(all[1].name == kimix::string{"count"});
        expect(all[1].container == GdiContainerType::array);
        expect(eq(all[1].count, uint32_t{4}));

        // A rename with no live value is free; a SHAPE change is refused while one
        // exists (error_read_only_property_type).
        auto const bytes = gtest::to_bytes("u1");
        LabelId node{};
        expect(eq(db.schema().create_label("node", node), GdiStatus::ok));
        VertexCreateResult created;
        expect(eq(db.create_vertex(view_of(node, bytes), {}, nullptr, created), GdiStatus::ok));
        expect(eq(db.add_property(created.uid, age, PropertyValue::make_int64(3)), GdiStatus::ok));

        expect(eq(db.update_property_type(age, make_type("years", GdiDataType::int64_)),
                  GdiStatus::ok))
            << "a rename alone is allowed";
        expect(eq(db.update_property_type(age, make_type("years", GdiDataType::double_)),
                  GdiStatus::error_read_only_property_type))
            << "a live value pins the declaration's shape";
        expect(eq(db.update_property_type(999, make_type("ghost", GdiDataType::int64_)),
                  GdiStatus::error_property_type));
    };

    "schema_free_property_type_cascades"_test = [] {
        Database db;
        LabelId node{};
        expect(eq(db.schema().create_label("node", node), GdiStatus::ok));
        PropNameId weight{};
        PropNameId other{};
        expect(eq(db.create_property_type(make_type("weight", GdiDataType::double_), weight),
                  GdiStatus::ok));
        expect(eq(db.create_property_type(make_type("other", GdiDataType::int64_), other),
                  GdiStatus::ok));

        kimix::vector<Guid> vertices;
        for (int i = 0; i < 2; ++i) {
            auto const bytes = gtest::to_bytes(kimix::format("v{}", i));
            VertexCreateResult created;
            expect(eq(db.create_vertex(view_of(node, bytes), {}, nullptr, created), GdiStatus::ok));
            expect(eq(db.add_property(created.uid, weight, PropertyValue::make_double(1.0)),
                      GdiStatus::ok));
            expect(eq(db.add_property(created.uid, other, PropertyValue::make_int64(1)),
                      GdiStatus::ok));
            vertices.push_back(created.uid);
        }
        graph_db::PropertyList properties;
        graph_db::PropertyEntry entry;
        entry.name = weight;
        entry.value = PropertyValue::make_double(2.0);
        properties.push_back(entry);
        Guid edge;
        expect(eq(db.edge_create(vertices[0], vertices[1], EdgeDirectionType::directed, {},
                                 &properties, edge),
                  GdiStatus::ok));

        expect(eq(db.free_property_type(weight), GdiStatus::ok));
        for (auto const &uid : vertices) {
            Vertex *vertex = db.graph().find(uid);
            expect(vertex != nullptr);
            expect(!vertex->has_property(weight)) << "the value is gone from every vertex";
            expect(vertex->has_property(other)) << "and the other type is untouched";
        }
        NamedPropertyList edge_properties;
        expect(eq(db.get_edge_properties(edge, edge_properties), GdiStatus::ok));
        expect(edge_properties.empty()) << "and from every edge";
        expect(eq(db.free_property_type(weight), GdiStatus::error_property_type));
    };

    "schema_persistence_round_trip"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }
        LabelId person{};
        PropNameId age{};
        {
            Database db;
            expect(eq(db.open(dir.path()), GdiStatus::ok));
            expect(eq(db.schema().create_label("person", person), GdiStatus::ok));
            LabelId employee{};
            expect(eq(db.schema().create_label("employee", employee), GdiStatus::ok));
            expect(eq(db.create_property_type(make_type("age", GdiDataType::int64_), age),
                      GdiStatus::ok));
            bool const incremental = false;
            expect(eq(db.persist(incremental), GdiStatus::ok));
        }
        Database restored;
        expect(eq(restored.open(dir.path()), GdiStatus::ok));
        NamedLabelList labels;
        expect(eq(restored.get_all_labels(labels), GdiStatus::ok));
        expect(eq(labels.size(), size_t{2}));
        expect(labels[0].name == kimix::string{"person"});
        expect(eq(labels[0].id, person)) << "the id space is not restarted by a reload";
        expect(labels[1].name == kimix::string{"employee"});
        PropertyTypeList types;
        expect(eq(restored.get_all_property_types(types), GdiStatus::ok));
        expect(eq(types.size(), size_t{1}));
        expect(types[0].name == kimix::string{"age"});
        expect(types[0].data == GdiDataType::int64_);
        // A NEW registration continues the id space instead of reusing a dead id.
        LabelId fresh{};
        expect(eq(restored.schema().create_label("fresh", fresh), GdiStatus::ok));
        expect(gt(fresh, labels[1].id)) << "ids are never recycled";
    };

    "schema_auto_create_and_strict_paths"_test = [] {
        graph_db::Schema schema;
        LabelId created{};
        GdiStatus status = schema.resolve_label("implicit", /*auto_create=*/true, created);
        expect(eq(status, GdiStatus::ok));
        expect(schema.has_label(created));
        status = schema.resolve_label("implicit", /*auto_create=*/true, created);
        expect(eq(status, GdiStatus::ok)) << "resolving an existing name is not an error";
        status = schema.resolve_label("strict", /*auto_create=*/false, created);
        expect(eq(status, GdiStatus::error_label))
            << "the strict mode refuses an unregistered name";

        PropNameId property{};
        PropertyTypeInfo declaration = make_type("weight", GdiDataType::double_);
        status = schema.resolve_property_type("weight", /*auto_create=*/true, declaration,
                                              property);
        expect(eq(status, GdiStatus::ok));
        PropertyTypeInfo const stored = schema.property_type(property);
        expect(stored.name == kimix::string{"weight"});
        expect(stored.data == GdiDataType::double_);
        status = schema.resolve_property_type("unknown", /*auto_create=*/false, declaration,
                                             property);
        expect(eq(status, GdiStatus::error_property_type));
    };

    "schema_many_ids_are_dense_and_never_recycled"_test = [] {
        Database db;
        constexpr size_t kCount = 10000;
        kimix::vector<LabelId> ids;
        ids.reserve(kCount);
        for (size_t i = 0; i < kCount; ++i) {
            LabelId id{};
            expect(eq(db.create_label(kimix::string{kimix::format("l{}", i)}, id),
                      GdiStatus::ok));
            ids.push_back(id);
        }
        // Dense from 1, no gaps, no duplicates.
        for (size_t i = 0; i < kCount; ++i) {
            expect(eq(ids[i], static_cast<LabelId>(i + 1)));
        }
        // Every name still resolves.
        for (size_t i = 0; i < kCount; i += 137) {
            LabelId found{};
            bool present = false;
            expect(eq(db.get_label_by_name(
                          kimix::string{kimix::format("l{}", i)}, found, present),
                      GdiStatus::ok));
            expect(present);
            expect(eq(found, ids[i]));
        }
        // Freeing leaves a permanent hole: the next id is higher, never a reuse.
        expect(eq(db.free_label(ids[5]), GdiStatus::ok));
        LabelId next{};
        expect(eq(db.create_label("after-free", next), GdiStatus::ok));
        expect(gt(next, ids.back()));
        expect(!db.schema().has_label(ids[5]));
    };
}
