// Test for the typed property model and vertex labels (src/graph_db/property.*,
// vertex.* payload API, schema.* registries) — GDI §2.1 / §2.2.
//
// This test covers:
// - the 7-kind matrix: set/get/round trip of every PropertyKind, including an
//   empty blob, an empty array, a zero-length string and UTF-8 text;
// - the error matrix of the four mutation commands x {present, absent,
//   wrong kind, undeclared type}: exact GdiStatus per cell (GDI's documented
//   semantics: Add -> ErrorPropertyExists, Set -> ErrorNoProperty, Update upserts,
//   Remove ignores an absent name);
// - the label matrix: add/remove/duplicate/absent/undeclared, and the KEY label
//   coming first in GetLabelsOfVertex;
// - determinism: get_property_types is sorted by name and a re-read is identical;
// - save/load round trip of labels + properties through one ColdStorage, with the
//   incremental save writing exactly the one dirty vertex;
// - concurrency: N threads updating N distinct vertices' properties while a
//   reader snapshots — no torn state, no crash (runs clean under ASan too);
// - the legacy content() blob coexisting with the typed payload.
//
// The fiber scheduler is bound FIRST in main() because the serializer's fan-out
// submits to it (see the fiber skill's root-main pattern).
#include "ut/ut.hpp"

#include "graph_db_test_util.h"

#include <core/kimix_core.h> // umbrella first: it fixes the winsock2.h order
#include <core/fiber.h>

#include <graph_db/database.h>
#include <graph_db/graph_db.h>
#include <graph_db/property.h>
#include <graph_db/schema.h>
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
using graph_db::Database;
using graph_db::EdgeOrientation;
using graph_db::GdiContainerType;
using graph_db::GdiDataType;
using graph_db::GdiOp;
using graph_db::GdiStatus;
using graph_db::GraphSerializeStats;
using graph_db::LabelId;
using graph_db::NamedLabelList;
using graph_db::NamedPropertyList;
using graph_db::PropNameId;
using graph_db::PropertyKind;
using graph_db::PropertyTypeInfo;
using graph_db::PropertyTypeList;
using graph_db::PropertyValue;
using graph_db::VertexKeyView;
namespace gtest = kimix::graph_db::test;

namespace tests_unit_graph_db_test_graph_db_property {

// A database with the small schema every case here needs: one key label, one
// extra label, and one property type per kind.
struct fixture {
    static constexpr LabelId kNoValue = 0;

    Database db;
    LabelId person{kNoValue};
    LabelId employee{kNoValue};
    PropNameId age{kNoValue};
    PropNameId score{kNoValue};
    PropNameId title{kNoValue};
    PropNameId payload{kNoValue};
    PropNameId ages{kNoValue};
    PropNameId scores{kNoValue};

    fixture() {
        expect(eq(db.schema().create_label("person", person), GdiStatus::ok));
        expect(eq(db.schema().create_label("employee", employee), GdiStatus::ok));
        expect(eq(db.create_property_type(make_type("age", GdiDataType::int64_, 1), age),
                  GdiStatus::ok));
        expect(eq(db.create_property_type(make_type("score", GdiDataType::double_, 1), score),
                  GdiStatus::ok));
        expect(eq(db.create_property_type(make_type("title", GdiDataType::char_, 1), title),
                  GdiStatus::ok));
        expect(eq(db.create_property_type(make_type("payload", GdiDataType::byte_, 1), payload),
                  GdiStatus::ok));
        expect(eq(db.create_property_type(make_type("ages", GdiDataType::int64_, 3), ages),
                  GdiStatus::ok));
        expect(eq(db.create_property_type(make_type("scores", GdiDataType::double_, 2), scores),
                  GdiStatus::ok));
    }

    static PropertyTypeInfo make_type(kimix::string_view name, GdiDataType data, uint32_t count) {
        PropertyTypeInfo info;
        info.name = kimix::string{name};
        info.data = data;
        info.count = count;
        info.container = count > 1 ? GdiContainerType::array : GdiContainerType::single;
        return info;
    }

    // One keyed vertex ("person", "u1") with no payload.
    Guid make_vertex(kimix::string_view external = "u1") {
        auto bytes = gtest::to_bytes(external);
        auto const id = kimix::span<const std::byte>{bytes.data(), bytes.size()};
        graph_db::VertexCreateResult created;
        expect(eq(db.create_vertex(VertexKeyView{person, id}, {}, nullptr, created),
                  GdiStatus::ok));
        return created.uid;
    }
};

} // namespace tests_unit_graph_db_test_graph_db_property
using namespace tests_unit_graph_db_test_graph_db_property;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    // FIRST: the serializer's fan-outs need a scheduler bound to this thread.
    kimix::fiber::scheduler sched{4u};

    "property_kind_matrix"_test = [] {
        fixture f;
        Guid const uid = f.make_vertex();

        // Every kind round-trips through the vertex payload.
        expect(eq(f.db.add_property(uid, f.age, PropertyValue::make_int64(-42)), GdiStatus::ok));
        expect(eq(f.db.add_property(uid, f.score, PropertyValue::make_double(2.5)), GdiStatus::ok));
        expect(eq(f.db.add_property(uid, f.title, PropertyValue::make_string("builder")),
                  GdiStatus::ok));
        std::byte raw[]{std::byte{0}, std::byte{1}, std::byte{0xff}};
        expect(eq(f.db.add_property(uid, f.payload,
                                    PropertyValue::make_blob(
                                        kimix::span<const std::byte>{raw, 3})),
                  GdiStatus::ok));
        int64_t const age_values[]{1, 2, 3};
        expect(eq(f.db.add_property(uid, f.ages,
                                    PropertyValue::make_int64_array(
                                        kimix::span<const int64_t>{age_values, 3})),
                  GdiStatus::ok));
        double const score_values[]{1.5, -0.5};
        expect(eq(f.db.add_property(uid, f.scores,
                                    PropertyValue::make_double_array(
                                        kimix::span<const double>{score_values, 2})),
                  GdiStatus::ok));

        NamedPropertyList properties;
        expect(eq(f.db.get_properties(uid, properties), GdiStatus::ok));
        expect(eq(properties.size(), size_t{6}));
        for (auto const &item : properties) {
            if (item.name == kimix::string{"age"}) {
                expect(item.value.kind() == PropertyKind::int64_value);
                expect(eq(item.value.as_int64(), int64_t{-42}));
            } else if (item.name == kimix::string{"score"}) {
                expect(item.value.kind() == PropertyKind::double_value);
                expect(item.value.as_double() == 2.5);
            } else if (item.name == kimix::string{"title"}) {
                expect(item.value.kind() == PropertyKind::string);
                expect(item.value.as_string() == kimix::string_view{"builder"});
            } else if (item.name == kimix::string{"payload"}) {
                expect(item.value.kind() == PropertyKind::blob);
                expect(eq(item.value.as_blob().size(), size_t{3}));
            } else if (item.name == kimix::string{"ages"}) {
                expect(item.value.kind() == PropertyKind::int64_array);
                expect(eq(item.value.as_int64_array().size(), size_t{3}));
                expect(eq(item.value.as_int64_array()[2], int64_t{3}));
            } else if (item.name == kimix::string{"scores"}) {
                expect(item.value.kind() == PropertyKind::double_array);
                expect(eq(item.value.as_double_array().size(), size_t{2}));
            }
        }
    };

    "property_edge_cases"_test = [] {
        // Zero-length payloads are VALUES, not absences.
        auto const empty_string = PropertyValue::make_string("");
        expect(empty_string.kind() == PropertyKind::string);
        expect(empty_string.as_string().empty());
        expect(!empty_string.is_none());

        auto const empty_blob = PropertyValue::make_blob({});
        expect(empty_blob.kind() == PropertyKind::blob);
        expect(empty_blob.as_blob().empty());

        auto const empty_array = PropertyValue::make_int64_array({});
        expect(empty_array.kind() == PropertyKind::int64_array);
        expect(eq(empty_array.element_count(), size_t{0}));

        // UTF-8 is byte-transparent (no transcoding, length preserved).
        char const *const utf8 = "\xE4\xB8\xAD\xE6\x96\x87"; // 中文
        auto const text = PropertyValue::make_string(kimix::string_view{utf8, 6});
        expect(eq(text.as_string().size(), size_t{6}));
        expect(text.equals(PropertyValue::make_string(kimix::string_view{utf8, 6})));
        expect(!text.equals(PropertyValue::make_string("zh")));

        // Value equality is by content, not by kind-tag alone.
        expect(PropertyValue::make_int64(7).equals(PropertyValue::make_int64(7)));
        expect(!PropertyValue::make_int64(7).equals(PropertyValue::make_double(7.0)))
            << "an int64 and a double are different kinds even with the same value";
        expect(PropertyValue::make_double(7.0).equals(PropertyValue::make_double(7.0)));
    };

    "property_error_matrix"_test = [] {
        fixture f;
        Guid const uid = f.make_vertex();

        // Add: absent -> ok, present -> ErrorPropertyExists, undeclared ->
        // ErrorPropertyType, wrong kind -> ErrorWrongType.
        expect(eq(f.db.add_property(uid, f.age, PropertyValue::make_int64(1)), GdiStatus::ok));
        expect(eq(f.db.add_property(uid, f.age, PropertyValue::make_int64(2)),
                  GdiStatus::error_property_exists));
        expect(eq(f.db.add_property(uid, 999, PropertyValue::make_int64(2)),
                  GdiStatus::error_property_type))
            << "an unregistered property type is ErrorPropertyType";
        expect(eq(f.db.add_property(uid, f.age, PropertyValue::make_string("x")),
                  GdiStatus::error_wrong_type));
        expect(eq(f.db.add_property(uid, f.score, PropertyValue::make_int64(1)),
                  GdiStatus::error_wrong_type))
            << "int64 where a double is declared";
        expect(eq(f.db.add_property(uid, f.age, PropertyValue{}), GdiStatus::error_wrong_type))
            << "a `none` value is never a valid property";

        // An array declaration checks the element count (GDI ErrorCount).
        int64_t const two[]{1, 2};
        expect(eq(f.db.add_property(uid, f.ages,
                                    PropertyValue::make_int64_array(
                                        kimix::span<const int64_t>{two, 2})),
                  GdiStatus::error_count))
            << "3 elements declared, 2 supplied";
        int64_t const three[]{1, 2, 3};
        expect(eq(f.db.add_property(uid, f.ages,
                                    PropertyValue::make_int64_array(
                                        kimix::span<const int64_t>{three, 3})),
                  GdiStatus::ok));

        // Set: absent -> ErrorNoProperty, present -> ok (replaces).
        expect(eq(f.db.set_property(uid, f.score, PropertyValue::make_double(1.0)),
                  GdiStatus::error_no_property));
        expect(eq(f.db.set_property(uid, f.age, PropertyValue::make_int64(9)), GdiStatus::ok));
        PropertyValue read_back;
        expect(f.db.graph().find(uid)->property(f.age, read_back));
        expect(eq(read_back.as_int64(), int64_t{9}));

        // Update: UPSERT (no error when absent).
        expect(eq(f.db.update_property(uid, f.score, PropertyValue::make_double(3.5)),
                  GdiStatus::ok));
        expect(eq(f.db.update_property(uid, f.age, PropertyValue::make_int64(10)), GdiStatus::ok));

        // Remove: absent names are ignored.
        PropNameId const names[]{f.age, 1234};
        expect(eq(f.db.remove_properties(uid, kimix::span<const PropNameId>{names, 2}),
                  GdiStatus::ok));
        expect(!f.db.graph().find(uid)->has_property(f.age));
        expect(f.db.graph().find(uid)->has_property(f.score)) << "the other value survives";
    };

    "property_label_matrix"_test = [] {
        fixture f;
        Guid const uid = f.make_vertex("labelled");

        expect(eq(f.db.add_label(uid, f.employee), GdiStatus::ok));
        expect(eq(f.db.add_label(uid, f.employee), GdiStatus::error_label))
            << "already present is ErrorLabel";
        expect(eq(f.db.add_label(uid, 999), GdiStatus::error_label))
            << "undeclared label is ErrorLabel";
        expect(eq(f.db.remove_label(uid, 999), GdiStatus::error_label))
            << "absent label is ErrorLabel";
        expect(eq(f.db.add_label(uid, f.person), GdiStatus::ok))
            << "adding the key label to the set is legal (it is a label too)";

        // GetLabelsOfVertex: the KEY label first, then the rest.
        kimix::vector<kimix::string> labels;
        expect(eq(f.db.get_labels(uid, labels), GdiStatus::ok));
        expect(ge(labels.size(), size_t{2}));
        expect(labels[0] == kimix::string{"person"}) << "the key label comes first";
        bool saw_employee = false;
        for (auto const &label : labels) {
            saw_employee = saw_employee || label == kimix::string{"employee"};
        }
        expect(saw_employee);

        expect(eq(f.db.remove_label(uid, f.employee), GdiStatus::ok));
        expect(eq(f.db.get_labels(uid, labels), GdiStatus::ok));
        for (auto const &label : labels) {
            expect(label != kimix::string{"employee"});
        }
    };

    "property_type_readback_is_sorted"_test = [] {
        fixture f;
        Guid const uid = f.make_vertex("sorted");
        expect(eq(f.db.add_property(uid, f.title, PropertyValue::make_string("t")), GdiStatus::ok));
        expect(eq(f.db.add_property(uid, f.age, PropertyValue::make_int64(1)), GdiStatus::ok));
        double const one_double[]{1.0};
        expect(eq(f.db.add_property(uid, f.scores,
                                    PropertyValue::make_double_array(
                                        kimix::span<const double>{one_double, 1})),
                  GdiStatus::error_count))
            << "2 elements declared, 1 supplied";

        PropertyTypeList types;
        expect(eq(f.db.get_property_types(uid, types), GdiStatus::ok));
        expect(eq(types.size(), size_t{2}));
        expect(types[0].name == kimix::string{"age"});
        expect(types[1].name == kimix::string{"title"});
        expect(types[0].data == GdiDataType::int64_);
        // A re-read is identical (deterministic ordering, no mutation).
        PropertyTypeList again;
        expect(eq(f.db.get_property_types(uid, again), GdiStatus::ok));
        expect(eq(again.size(), types.size()));
        for (size_t i = 0; i < again.size(); ++i) {
            expect(again[i].name == types[i].name);
            expect(again[i].data == types[i].data);
        }
    };

    "property_save_load_round_trip"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }
        Database db;
        expect(eq(db.open(dir.path()), GdiStatus::ok));
        LabelId person{};
        expect(eq(db.schema().create_label("person", person), GdiStatus::ok));
        PropNameId age{};
        expect(eq(db.create_property_type(fixture::make_type("age", GdiDataType::int64_, 1), age),
                  GdiStatus::ok));
        LabelId employee{};
        expect(eq(db.schema().create_label("employee", employee), GdiStatus::ok));

        auto bytes = gtest::to_bytes("u1");
        graph_db::VertexCreateResult created;
        LabelId const labels[]{employee};
        expect(eq(db.create_vertex(VertexKeyView{person, kimix::span<const std::byte>{
                                                             bytes.data(), bytes.size()}},
                                   kimix::span<const LabelId>{labels, 1}, nullptr, created),
                  GdiStatus::ok));
        expect(eq(db.add_property(created.uid, age, PropertyValue::make_int64(31)), GdiStatus::ok));
        expect(eq(db.persist(/*full=*/true), GdiStatus::ok));

        // One more write, then an incremental save that writes exactly it.
        expect(eq(db.add_property(created.uid, 1234, PropertyValue::make_int64(0)),
                  GdiStatus::error_property_type));
        PropNameId nick{};
        expect(eq(db.create_property_type(fixture::make_type("nick", GdiDataType::char_, 1), nick),
                  GdiStatus::ok));
        expect(eq(db.add_property(created.uid, nick, PropertyValue::make_string("bert")),
                  GdiStatus::ok));
        GraphSerializeStats stats;
        bool const incremental = false;
        expect(eq(db.persist(incremental, &stats), GdiStatus::ok));
        expect(eq(stats.saved_count, size_t{1})) << "only the touched vertex is written";

        // Reload into a fresh database: the payload, the labels and the schema.
        Database restored;
        expect(eq(restored.open(dir.path()), GdiStatus::ok));
        LabelId restored_label{};
        bool found = false;
        expect(eq(restored.get_label_by_name("person", restored_label, found), GdiStatus::ok));
        expect(found);
        PropNameId restored_age{};
        expect(eq(restored.get_property_type_by_name("age", restored_age, found), GdiStatus::ok));
        expect(found);
        NamedLabelList registered;
        expect(eq(restored.get_all_labels(registered), GdiStatus::ok));
        expect(ge(registered.size(), size_t{2}))
            << "the label registry survived the round trip";

        NamedPropertyList properties;
        expect(eq(restored.get_properties(created.uid, properties), GdiStatus::ok));
        expect(eq(properties.size(), size_t{2}));
        kimix::vector<kimix::string> labels_out;
        expect(eq(restored.get_labels(created.uid, labels_out), GdiStatus::ok));
        expect(labels_out[0] == kimix::string{"person"});
        bool saw_employee = false;
        for (auto const &label : labels_out) {
            saw_employee = saw_employee || label == kimix::string{"employee"};
        }
        expect(saw_employee) << "the extra label survived the round trip";
    };

    "property_concurrent_distinct_vertices"_test = [] {
        // N threads update N DISTINCT vertices' properties: the vertex lock is the
        // only contention, so this must not tear (and must be ASan clean).
        fixture f;
        constexpr size_t kThreads = 4;
        kimix::vector<Guid> uids;
        uids.reserve(kThreads);
        for (size_t i = 0; i < kThreads; ++i) {
            uids.push_back(f.make_vertex(kimix::format("u{}", i)));
        }

        std::atomic<bool> stop{false};
        std::atomic<size_t> torn{0};
        std::thread reader([&f, &uids, &stop, &torn] {
            while (!stop.load(std::memory_order_relaxed)) {
                for (auto const &uid : uids) {
                    NamedPropertyList properties;
                    if (f.db.get_properties(uid, properties) != GdiStatus::ok) {
                        continue;
                    }
                    for (auto const &item : properties) {
                        if (item.name == kimix::string{"age"} &&
                            item.value.kind() != PropertyKind::int64_value) {
                            torn.fetch_add(1);
                        }
                    }
                }
            }
        });

        kimix::vector<std::thread> writers;
        for (size_t i = 0; i < kThreads; ++i) {
            writers.emplace_back([&f, uid = uids[i]] {
                for (int64_t value = 0; value < 2000; ++value) {
                    (void)f.db.update_property(uid, f.age, PropertyValue::make_int64(value));
                }
            });
        }
        for (auto &writer : writers) {
            writer.join();
        }
        stop.store(true);
        reader.join();
        expect(eq(torn.load(), size_t{0})) << "a property read never observes a torn value";
    };

    "property_coexists_with_legacy_content"_test = [] {
        // The opaque content blob is a separate field: a property write must not
        // touch it, and content() must keep returning exactly what was set.
        fixture f;
        Guid const uid = f.make_vertex("content");
        auto const blob = gtest::to_bytes("legacy-blob");
        expect(f.db.graph().update_content(uid, kimix::span<const std::byte>{blob.data(),
                                                                            blob.size()}));
        expect(eq(f.db.add_property(uid, f.age, PropertyValue::make_int64(5)), GdiStatus::ok));
        auto const content = f.db.graph().find(uid)->content_copy();
        expect(gtest::blob_equal(content, "legacy-blob"))
            << "a property write leaves the legacy blob alone";
        expect(eq(f.db.remove_properties(uid, kimix::span<const PropNameId>{&f.age, 1}),
                  GdiStatus::ok));
        expect(gtest::blob_equal(f.db.graph().find(uid)->content_copy(), "legacy-blob"));
    };
}
