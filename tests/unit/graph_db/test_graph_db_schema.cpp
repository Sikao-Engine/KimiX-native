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
using graph_db::GdiSizeType;
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

// An ARRAY declaration with an explicit size type (ABI 8): make_type infers the
// container from the count, so `unlimited` (stored count 0) needs its own builder.
inline PropertyTypeInfo make_array_type(kimix::string_view name, GdiDataType data,
                                        GdiSizeType size_type, uint32_t count) {
    PropertyTypeInfo info;
    info.name = kimix::string{name};
    info.data = data;
    info.container = GdiContainerType::array;
    info.size_type = size_type;
    info.count = count;
    return info;
}

// A `count`-element string_array value (the variable-length write).
inline graph_db::PropertyValue make_strings(uint32_t count) {
    kimix::vector<kimix::string> owned;
    kimix::vector<kimix::string_view> views;
    owned.reserve(count);
    views.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        owned.push_back(kimix::format("s{}", i));
    }
    for (auto const &text : owned) {
        views.push_back(kimix::string_view{text});
    }
    return graph_db::PropertyValue::make_string_array(
        kimix::span<const kimix::string_view>{views.data(), views.size()});
}

// A little-endian u32 / u8 append, spelled here so a golden image does not depend
// on the codec it is checking.
inline void push_u32(kimix::vector<std::byte> &out, uint32_t value) {
    for (uint32_t shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
    }
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
                                 kimix::span<const LabelId>{edge_labels, 1}, nullptr, nullptr, edge),
                  GdiStatus::ok));
        Guid second_edge;
        expect(eq(db.edge_create(vertices[1], vertices[2], EdgeDirectionType::directed,
                                 kimix::span<const LabelId>{edge_labels, 1}, nullptr, nullptr, second_edge),
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
                                 &properties, nullptr, edge),
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

    // -----------------------------------------------------------------------
    // ABI 8 change 1: the declaration rules and the size type in the schema
    // table (plan §1.2 / §1.4).
    // -----------------------------------------------------------------------
    "schema_size_type_declaration_rules"_test = [] {
        Database db;
        PropNameId id{};
        // A single value is fixed-only.
        PropertyTypeInfo single = make_type("single_max", GdiDataType::char_, 1);
        single.size_type = GdiSizeType::max;
        expect(eq(db.create_property_type(single, id), GdiStatus::error_argument));
        single.size_type = GdiSizeType::unlimited;
        expect(eq(db.create_property_type(single, id), GdiStatus::error_argument));
        // An out-of-range wire value never becomes a declaration.
        expect(eq(db.create_property_type(
                      make_array_type("bad_size", GdiDataType::char_,
                                      static_cast<GdiSizeType>(200), 2),
                      id),
                  GdiStatus::error_argument))
            << "GDI's own 200/201/202 numbering is NOT the wire ABI";
        // fixed / max need count >= 1, unlimited normalizes the count to 0.
        expect(eq(db.create_property_type(
                      make_array_type("fixed0", GdiDataType::char_, GdiSizeType::fixed, 0), id),
                  GdiStatus::error_count));
        expect(eq(db.create_property_type(
                      make_array_type("max0", GdiDataType::char_, GdiSizeType::max, 0), id),
                  GdiStatus::error_count));
        PropNameId unlimited{};
        expect(eq(db.create_property_type(
                      make_array_type("unlimited_docs", GdiDataType::char_,
                                      GdiSizeType::unlimited, 99),
                      unlimited),
                  GdiStatus::ok));
        PropertyTypeInfo const stored = db.schema().property_type(unlimited);
        expect(stored.size_type == GdiSizeType::unlimited);
        expect(eq(stored.count, uint32_t{0})) << "the canonical form carries no bound";
        // The same rules apply on UPDATE.
        expect(eq(db.update_property_type(
                      unlimited, make_array_type("unlimited_docs", GdiDataType::char_,
                                                 static_cast<GdiSizeType>(3), 4)),
                  GdiStatus::error_argument));
        expect(eq(db.update_property_type(
                      unlimited, make_array_type("unlimited_docs", GdiDataType::char_,
                                                 GdiSizeType::fixed, 0)),
                  GdiStatus::error_count));
    };

    // -----------------------------------------------------------------------
    // ABI 9: the data-array declaration — container ARRAY + the `data` sentinel.
    // -----------------------------------------------------------------------
    "schema_data_array_declaration_rules"_test = [] {
        Database db;
        // (array, data) is legal with every size type, exactly like a string array.
        PropNameId fixed_id{};
        expect(eq(db.create_property_type(
                      make_array_type("payloads", GdiDataType::data, GdiSizeType::fixed, 2),
                      fixed_id),
                  GdiStatus::ok));
        PropertyTypeInfo stored = db.schema().property_type(fixed_id);
        expect(stored.container == GdiContainerType::array);
        expect(stored.data == GdiDataType::data) << "the self-describing sentinel datatype";
        expect(eq(stored.count, uint32_t{2}));
        expect(stored.size_type == GdiSizeType::fixed);

        PropNameId max_id{};
        expect(eq(db.create_property_type(
                      make_array_type("payloads_max", GdiDataType::data, GdiSizeType::max, 5),
                      max_id),
                  GdiStatus::ok));
        PropNameId unlimited_id{};
        expect(eq(db.create_property_type(
                      make_array_type("payloads_any", GdiDataType::data,
                                      GdiSizeType::unlimited, 77),
                      unlimited_id),
                  GdiStatus::ok));
        stored = db.schema().property_type(unlimited_id);
        expect(stored.size_type == GdiSizeType::unlimited);
        expect(eq(stored.count, uint32_t{0})) << "the canonical form carries no bound";

        // (single, data) is refused: a self-describing array needs an ARRAY to
        // describe, so the declaration could never be satisfied (D5).
        PropNameId refused{};
        expect(eq(db.create_property_type(make_type("lone_payload", GdiDataType::data, 1),
                                          refused),
                  GdiStatus::error_argument));
        expect(eq(db.update_property_type(
                      fixed_id,
                      make_type("payloads", GdiDataType::data, 1)),
                  GdiStatus::error_argument))
            << "the same rule applies on update";
        expect(!db.schema().has_property_type(refused)) << "a refused declaration never exists";

        // The declaration round-trips through the schema table codec unchanged
        // (dtype 200 is a byte like any other there).
        PropertyTypeList const all = db.schema().all_property_types();
        bool saw_data = false;
        for (auto const &item : all) {
            if (item.name_id == fixed_id) {
                saw_data = true;
                expect(item.data == GdiDataType::data);
                expect(eq(static_cast<int>(item.data), 200));
            }
        }
        expect(saw_data);
    };

    "schema_data_array_write_rules"_test = [] {
        Database db;
        LabelId node{};
        expect(eq(db.schema().create_label("node", node), GdiStatus::ok));
        PropNameId payloads{};
        expect(eq(db.create_property_type(
                      make_array_type("payloads", GdiDataType::data, GdiSizeType::max, 3),
                      payloads),
                  GdiStatus::ok));
        PropNameId ints{};
        expect(eq(db.create_property_type(
                      make_array_type("ints", GdiDataType::int64_, GdiSizeType::max, 3), ints),
                  GdiStatus::ok));
        auto const bytes = gtest::to_bytes("u1");
        VertexCreateResult created;
        expect(eq(db.create_vertex(view_of(node, bytes), {}, nullptr, created), GdiStatus::ok));

        int64_t const values[2]{1, 2};
        auto const data_array = PropertyValue::make_data_array_int64(
            kimix::span<const int64_t>{values, 2});
        expect(eq(db.add_property(created.uid, payloads, data_array), GdiStatus::ok))
            << "a data array against (array, data) is the legal pair";

        // A data array against (array, int64) is a type error: the value's datatype
        // is the `data` sentinel, never the element kind it happens to hold.
        expect(eq(db.add_property(created.uid, ints, data_array), GdiStatus::error_wrong_type));
        // ... and the mirror image: an int64 array against (array, data).
        auto const int_array =
            PropertyValue::make_int64_array(kimix::span<const int64_t>{values, 2});
        expect(eq(db.add_property(created.uid, payloads, int_array), GdiStatus::error_wrong_type));
        // The length rule is the array one (max 3 here).
        int64_t const four[4]{1, 2, 3, 4};
        expect(eq(db.add_property(created.uid, payloads,
                                 PropertyValue::make_data_array_int64(
                                     kimix::span<const int64_t>{four, 4})),
                  GdiStatus::error_count))
            << "count bounds the ELEMENT count, like every array kind";
        // A scalar against the array declaration.
        expect(eq(db.add_property(created.uid, payloads, PropertyValue::make_int64(1)),
                  GdiStatus::error_wrong_type));

        // Auto-create: the caller derives the declaration from the value —
        // container array, data = the sentinel, count = element_count, size_type
        // fixed — exactly the shape a string array derives today.
        graph_db::Schema schema;
        PropertyTypeInfo derived = make_array_type("", GdiDataType::data, GdiSizeType::fixed,
                                                   static_cast<uint32_t>(data_array.element_count()));
        PropNameId auto_created{};
        expect(eq(schema.resolve_property_type("auto_payloads", /*auto_create=*/true, derived,
                                              auto_created),
                  GdiStatus::ok));
        PropertyTypeInfo const auto_stored = schema.property_type(auto_created);
        expect(auto_stored.container == GdiContainerType::array);
        expect(auto_stored.data == GdiDataType::data);
        expect(eq(auto_stored.count, uint32_t{2}));
        expect(auto_stored.size_type == GdiSizeType::fixed)
            << "an auto-created declaration is exact-length by construction";
    };

    "schema_size_type_relaxation_with_live_values"_test = [] {
        Database db;
        LabelId node{};
        expect(eq(db.schema().create_label("node", node), GdiStatus::ok));
        PropNameId docs{};
        expect(eq(db.create_property_type(
                      make_array_type("docs", GdiDataType::char_, GdiSizeType::fixed, 3), docs),
                  GdiStatus::ok));
        auto const bytes = gtest::to_bytes("u1");
        VertexCreateResult created;
        expect(eq(db.create_vertex(view_of(node, bytes), {}, nullptr, created), GdiStatus::ok));
        // A 3-element value pins the declaration... except for a relaxation.
        expect(eq(db.add_property(created.uid, docs, make_strings(3)), GdiStatus::ok));
        expect(eq(db.update_property_type(
                      docs, make_array_type("docs", GdiDataType::char_, GdiSizeType::fixed, 4)),
                  GdiStatus::error_read_only_property_type))
            << "a count change with live values is refused when it NARROWS";
        expect(eq(db.update_property_type(
                      docs, make_array_type("docs", GdiDataType::char_, GdiSizeType::max, 5)),
                  GdiStatus::ok))
            << "fixed N -> max >= N is a relaxation: every stored value stays valid";
        expect(eq(db.schema().property_type(docs).size_type, GdiSizeType::max))
            << "the relaxation is stored";
        // The stored value is untouched and still readable.
        PropertyValue read_back;
        expect(db.graph().find(created.uid)->property(docs, read_back));
        expect(eq(read_back.element_count(), size_t{3}));
        expect(eq(db.update_property_type(
                      docs, make_array_type("docs", GdiDataType::char_, GdiSizeType::max, 4)),
                  GdiStatus::error_read_only_property_type))
            << "max 5 -> max 4 narrows";
        expect(eq(db.update_property_type(
                      docs, make_array_type("docs", GdiDataType::char_, GdiSizeType::unlimited, 0)),
                  GdiStatus::ok))
            << "max -> unlimited is a relaxation";
        expect(eq(db.update_property_type(
                      docs, make_array_type("docs", GdiDataType::char_,
                                            GdiSizeType::fixed, 3)),
                  GdiStatus::error_read_only_property_type))
            << "unlimited -> fixed puts a bound back on stored values";
        expect(eq(db.update_property_type(
                      docs, make_array_type("docs", GdiDataType::int64_,
                                            GdiSizeType::unlimited, 0)),
                  GdiStatus::error_read_only_property_type))
            << "a datatype change is never a relaxation";
        // The relaxed declaration really accepts the new lengths.
        expect(eq(db.update_property(created.uid, docs, make_strings(9)), GdiStatus::ok));
    };

    "schema_property_type_table_size_type_codec"_test = [] {
        // 1. A GOLDEN ABI-7 image: its two reserved bytes are zero, so the entry
        //    decodes as `fixed` with the count it always had.
        kimix::vector<std::byte> abi7;
        for (char c : kimix::string_view{"GDS2"}) {
            abi7.push_back(static_cast<std::byte>(c));
        }
        push_u32(abi7, 1); // TableKind::property_types
        push_u32(abi7, 1); // one entry
        push_u32(abi7, 7); // id
        abi7.push_back(std::byte{1});   // container = array
        abi7.push_back(std::byte{100}); // dtype = char_
        abi7.push_back(std::byte{0});   // ABI 7 reserved (low byte) -> fixed
        abi7.push_back(std::byte{0});   // ABI 7 reserved (high byte)
        push_u32(abi7, 3);              // count
        push_u32(abi7, 4);              // name length
        for (char c : kimix::string_view{"docs"}) {
            abi7.push_back(static_cast<std::byte>(c));
        }
        PropertyTypeList decoded;
        expect(graph_db::decode_property_type_table(
            kimix::span<const std::byte>{abi7.data(), abi7.size()}, decoded))
            << "an ABI-7 schema table still decodes";
        expect(eq(decoded.size(), size_t{1}));
        expect(eq(decoded[0].name_id, PropNameId{7}));
        expect(decoded[0].container == GdiContainerType::array);
        expect(decoded[0].data == GdiDataType::char_);
        expect(decoded[0].size_type == GdiSizeType::fixed) << "zero reserved byte == fixed";
        expect(eq(decoded[0].count, uint32_t{3}));
        expect(decoded[0].name == kimix::string{"docs"});

        // 2. Re-encoding it is byte-identical: the size type went into a byte that
        //    was already zero.
        kimix::vector<std::byte> reencoded;
        graph_db::encode_property_type_table(decoded, reencoded);
        expect(eq(reencoded.size(), abi7.size()));
        expect(std::equal(reencoded.begin(), reencoded.end(), abi7.begin()))
            << "an all-fixed table keeps its ABI-7 bytes";

        // 3. The size type lands in the FIRST reserved byte, and every value
        //    survives the round trip.
        PropertyTypeList entries;
        entries.push_back(make_array_type("unlimited_docs", GdiDataType::char_,
                                          GdiSizeType::unlimited, 0));
        entries.push_back(make_array_type("max_docs", GdiDataType::char_, GdiSizeType::max, 5));
        entries.push_back(make_array_type("fixed_docs", GdiDataType::char_, GdiSizeType::fixed, 3));
        // name_id is assigned by the schema, so set it here for the codec.
        for (size_t i = 0; i < entries.size(); ++i) {
            entries[i].name_id = static_cast<PropNameId>(i + 1);
        }
        kimix::vector<std::byte> bytes;
        graph_db::encode_property_type_table(entries, bytes);
        // Entry 1 starts after magic + kind + count; its size-type byte is the 7th
        // byte of the entry (4 id + 1 container + 1 dtype).
        constexpr size_t kHeader = 4 + 4 + 4;
        expect(eq(std::to_integer<uint32_t>(bytes[kHeader + 6]), uint32_t{2}))
            << "unlimited is 2 on the wire";
        PropertyTypeList back;
        expect(graph_db::decode_property_type_table(
            kimix::span<const std::byte>{bytes.data(), bytes.size()}, back));
        expect(eq(back.size(), size_t{3}));
        expect(back[0].size_type == GdiSizeType::unlimited);
        expect(eq(back[0].count, uint32_t{0}));
        expect(back[1].size_type == GdiSizeType::max);
        expect(eq(back[1].count, uint32_t{5}));
        expect(back[2].size_type == GdiSizeType::fixed);
        expect(eq(back[2].count, uint32_t{3}));

        // 4. An out-of-range size type fails the WHOLE table, so the load path
        //    reports a corrupt schema instead of adopting a bogus declaration.
        kimix::vector<std::byte> corrupt = bytes;
        corrupt[kHeader + 6] = std::byte{9};
        PropertyTypeList refused;
        expect(!graph_db::decode_property_type_table(
            kimix::span<const std::byte>{corrupt.data(), corrupt.size()}, refused))
            << "a size type no writer could produce fails the decode";
        expect(refused.empty());
    };
}
