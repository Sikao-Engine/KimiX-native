// Test for the typed property model and vertex labels (src/graph_db/property.*,
// vertex.* payload API, schema.* registries) — GDI §2.1 / §2.2.
//
// This test covers:
// - the 8-kind matrix: set/get/round trip of every PropertyKind, including an
//   empty blob, an empty array, a zero-length string and UTF-8 text;
// - the string_array kind (ABI 7, plan §1.1): the canonical wire block, the
//   zero-allocation cursor accessor, whole-array equality, the char_/array
//   declaration match, deep-copy ownership, and the record codec's strict
//   decoder (every malformed block is refused) — see the string_array_* suites;
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
// - ANY_IN (ABI 7): the membership semantics definition, its probe index, and the
//   parity of the two across the kind matrix (see any_in_semantics_and_probe_index_parity).
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

// ---------------------------------------------------------------------------
// string_array fixtures (ABI 7, plan §1.1)
// ---------------------------------------------------------------------------

// A string_array value from a literal element list. The views are borrowed for
// the call only — make_string_array copies them into its own block, so the
// initializer_list's temporary buffer never outlives the full expression.
inline graph_db::PropertyValue make_strings(std::initializer_list<kimix::string_view> elements) {
    return graph_db::PropertyValue::make_string_array(
        kimix::span<const kimix::string_view>{
            elements.size() == 0 ? static_cast<kimix::string_view const *>(nullptr)
                                 : elements.begin(),
            elements.size()});
}

// The §1.1 block bytes of `elements`, spelled INDEPENDENTLY of the library:
// [u32 count] then per element [u32 byte length][utf-8 bytes], little-endian, no
// padding. A golden image — the value's block, the record payload and the C ABI
// payload must all be exactly these bytes.
inline kimix::vector<std::byte> golden_block(std::initializer_list<kimix::string_view> elements) {
    kimix::vector<std::byte> out;
    auto push_u32 = [&out](uint32_t value) {
        for (uint32_t shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
        }
    };
    push_u32(static_cast<uint32_t>(elements.size()));
    for (auto const element : elements) {
        push_u32(static_cast<uint32_t>(element.size()));
        for (char c : element) {
            out.push_back(static_cast<std::byte>(c));
        }
    }
    return out;
}

// Exact byte-for-byte comparison of two spans (size included).
inline bool bytes_same(kimix::span<const std::byte> lhs,
                       kimix::span<const std::byte> rhs) noexcept {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

// A little-endian u32 read, spelled independently of the library's codec, so a
// record header can be pinned against this file rather than against itself.
inline uint32_t le32(kimix::span<const std::byte> bytes, size_t at) noexcept {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(std::to_integer<uint32_t>(bytes[at + i])) << (8u * i);
    }
    return value;
}

inline uint64_t le64(kimix::span<const std::byte> bytes, size_t at) noexcept {
    return static_cast<uint64_t>(le32(bytes, at)) |
           (static_cast<uint64_t>(le32(bytes, at + 4)) << 32);
}

// Encode one entry into a fresh buffer, sized ONLY by encoded_property_entry_size
// — which is therefore also proved to agree with what encode writes.
inline kimix::vector<std::byte> encode_entry(graph_db::PropertyEntry const &entry) {
    kimix::vector<std::byte> out;
    out.resize(graph_db::encoded_property_entry_size(entry));
    graph_db::encode_property_entry(out.data(), entry);
    return out;
}

// Decode one whole entry: true only when the codec accepts the record AND lands
// exactly on the end of the buffer.
inline bool decode_entry(kimix::span<const std::byte> bytes,
                         graph_db::PropertyEntry &out) noexcept {
    std::byte const *cursor = bytes.data();
    std::byte const *const end = bytes.data() + bytes.size();
    return graph_db::decode_property_entry(cursor, end, out) && cursor == end;
}

// True when `value` is a string_array holding exactly `expected`, element by
// element. `expected` is spelled as a golden block so the check does not reuse
// the accessor it is testing for its notion of the element count.
inline bool holds_strings(graph_db::PropertyValue const &value,
                          std::initializer_list<kimix::string_view> expected) {
    auto const want = golden_block(expected);
    if (value.kind() != PropertyKind::string_array) {
        return false;
    }
    if (!bytes_same(value.as_string_array_block(),
                    kimix::span<const std::byte>{want.data(), want.size()})) {
        return false;
    }
    auto const view = value.as_string_array();
    if (view.element_count() != static_cast<uint32_t>(expected.size())) {
        return false;
    }
    size_t walked = 0;
    for (auto const element : view) {
        if (walked >= expected.size() || element != *(expected.begin() + walked)) {
            return false;
        }
        ++walked;
    }
    return walked == expected.size();
}

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

    "string_array_make_and_access"_test = [] {
        // The kind's numeric value is pinned: it mirrors ValueKind.StringArray and
        // KIMIX_GDB_KIND_STRING_ARRAY, so neither side may renumber it.
        expect(eq(static_cast<int>(PropertyKind::string_array), 7));
        expect(kimix::string_view{graph_db::property_kind_name(PropertyKind::string_array)} ==
               kimix::string_view{"string_array"});

        // An EMPTY array is a value, not an absence: the canonical block is exactly
        // the 4-byte count field.
        auto const empty = make_strings({});
        expect(empty.kind() == PropertyKind::string_array);
        expect(!empty.is_none()) << "an empty string_array is not `none`";
        expect(eq(empty.element_count(), size_t{0}));
        expect(empty.as_string_array().empty());
        expect(eq(empty.as_string_array_block().size(), size_t{4}));
        expect(!empty.as_string_array_block().empty())
            << "an empty array still owns its 4-byte count block";
        // The block is the canonical [u32 0] and nothing else (checked against the
        // golden image below, and accepted by the validator with a zero count).
        uint32_t empty_count = 12345;
        expect(graph_db::string_array_block_validate(empty.as_string_array_block(), empty_count));
        expect(eq(empty_count, uint32_t{0}));

        // Elements, including multi-byte UTF-8 and an empty element. UTF-8 is
        // byte-transparent: no transcoding, the length is the BYTE length.
        char const *const utf8 = "\xE4\xB8\xAD\xE6\x96\x87"; // 中文
        auto const value = make_strings({"alpha", kimix::string_view{utf8, 6}, ""});
        expect(eq(value.element_count(), size_t{3})) << "ELEMENTS, not bytes";
        auto const view = value.as_string_array();
        expect(eq(view.element_count(), uint32_t{3}));
        expect(view.element(0) == kimix::string_view{"alpha"});
        expect(view.element(1) == kimix::string_view{utf8, 6});
        expect(eq(view.element(1).size(), size_t{6})) << "6 bytes of UTF-8 survive as 6 bytes";
        expect(view.element(2).empty()) << "an empty element is a value";
        expect(view.element(3).empty()) << "out of range is empty: the accessor is total";
        expect(eq(view.block().size(), value.as_string_array_block().size()));

        // The cursor walk visits every element once, in order, without allocating,
        // and agrees with the indexed accessor.
        size_t walked = 0;
        size_t text_bytes = 0;
        for (auto const element : view) {
            expect(element == view.element(static_cast<uint32_t>(walked)));
            text_bytes += element.size();
            ++walked;
        }
        expect(eq(walked, size_t{3}));
        expect(eq(text_bytes, size_t{11})) << "5 + 6 + 0 bytes of text in the block";

        // The golden byte image is the frozen §1.1 layout, spelled here by hand.
        auto const want = golden_block({"alpha", kimix::string_view{utf8, 6}, ""});
        expect(bytes_same(value.as_string_array_block(),
                          kimix::span<const std::byte>{want.data(), want.size()}))
            << "[u32 count][u32 len][bytes]... little-endian, no padding";

        // Every other kind reads as an EMPTY array — total accessors, no assert.
        expect(make_strings({"x"}).as_string().empty()) << "the inverse: a string_view of a";
        auto const other = PropertyValue::make_string("not an array");
        expect(other.as_string_array().element_count() == 0u);
        expect(other.as_string_array().begin() == other.as_string_array().end());
        expect(other.as_string_array_block().empty());
        expect(eq(other.element_count(), size_t{0}));

        // The validator is the ONE well-formedness gate: it accepts exactly the
        // canonical blocks and refuses the rest, without ever reading further.
        uint32_t count = 0;
        expect(graph_db::string_array_block_validate(
            kimix::span<const std::byte>{want.data(), want.size()}, count));
        expect(eq(count, uint32_t{3}));
        // ... a block that is shorter than the count field,
        kimix::vector<std::byte> const stub{std::byte{1}, std::byte{0}, std::byte{0}};
        expect(!graph_db::string_array_block_validate(
            kimix::span<const std::byte>{stub.data(), stub.size()}, count));
        // ... an empty array with a trailing byte (a non-canonical count 0),
        kimix::vector<std::byte> trailing{std::byte{0}, std::byte{0}, std::byte{0},
                                          std::byte{0}, std::byte{7}};
        expect(!graph_db::string_array_block_validate(
            kimix::span<const std::byte>{trailing.data(), trailing.size()}, count));
        // ... an element length that runs past the block,
        auto const overlong = golden_block({"alpha"});
        kimix::vector<std::byte> hostile{overlong};
        hostile[4] = static_cast<std::byte>(0xff); // claims 255+ bytes for 5
        expect(!graph_db::string_array_block_validate(
            kimix::span<const std::byte>{hostile.data(), hostile.size()}, count));
        // ... and a count that is larger than the block can hold.
        auto const short_count = golden_block({"a", "b"});
        kimix::vector<std::byte> greedy{short_count};
        greedy[0] = static_cast<std::byte>(5); // five elements, two are present
        expect(!graph_db::string_array_block_validate(
            kimix::span<const std::byte>{greedy.data(), greedy.size()}, count));
        // make_string_array_from_block adopts a validated copy and refuses the rest.
        auto const adopted =
            PropertyValue::make_string_array_from_block(
                kimix::span<const std::byte>{want.data(), want.size()});
        expect(adopted.equals(value)) << "the block round-trips into the same value";
        expect(PropertyValue::make_string_array_from_block(
                   kimix::span<const std::byte>{hostile.data(), hostile.size()})
                   .is_none())
            << "a malformed block never becomes a value";
    };

    "string_array_equality_is_whole_array"_test = [] {
        auto const left = make_strings({"a", "bb"});
        auto const same = make_strings({"a", "bb"});
        auto const swapped = make_strings({"bb", "a"});
        auto const different_element = make_strings({"a", "bc"});
        auto const fewer = make_strings({"a"});
        expect(left.equals(same));
        expect(!left.equals(swapped))
            << "order is part of the value (the C# reference compares element by element)";
        expect(!left.equals(different_element)) << "one differing element byte is enough";
        expect(!left.equals(fewer)) << "a different element count is enough";
        expect(!left.equals(PropertyValue::make_string("a")))
            << "a String is never equal to a StringArray";
        expect(make_strings({}).equals(make_strings({})));
        expect(!make_strings({}).equals(make_strings({""})))
            << "zero elements is not one empty element";

        // Equality-only, like the other array kinds: an equal pair compares as 0,
        // an unequal pair is UNORDERABLE (nullopt), never -1/+1.
        auto const equal_order = graph_db::compare_values(left, same);
        expect(equal_order.has_value());
        expect(eq(*equal_order, int32_t{0}));
        auto const unequal_order = graph_db::compare_values(left, swapped);
        expect(!unequal_order.has_value())
            << "arrays do not order; only (in)equality is defined";
        expect(graph_db::evaluate(GdiOp::equal, left, same));
        expect(graph_db::evaluate(GdiOp::not_equal, left, swapped));
        expect(!graph_db::evaluate(GdiOp::greater, left, same));
        expect(!graph_db::evaluate(GdiOp::less, left, swapped))
            << "an ordering op on an unorderable pair is false, not an error";
        expect(!graph_db::evaluate(GdiOp::greater, left, PropertyValue::make_int64(7)))
            << "a mixed kind pair is unequal and unorderable";
    };

    "string_array_declaration_is_an_array_of_char"_test = [] {
        auto const value = make_strings({"a", "bb", "ccc"});
        expect(value.data_type() == GdiDataType::char_)
            << "one char_ declaration serves String and StringArray, like Int64 for";
        expect(eq(static_cast<int>(value.data_type()), 100));

        // Container array + dtype char_ + the ELEMENT count (mirrors int64_array).
        expect(value.matches_declaration(GdiContainerType::array, GdiDataType::char_, 3));
        expect(!value.matches_declaration(GdiContainerType::array, GdiDataType::char_, 2))
            << "count is the element count, not the byte total";
        expect(!value.matches_declaration(GdiContainerType::array, GdiDataType::char_, 12))
            << "12 is this value's byte total, and it means nothing here";
        expect(!value.matches_declaration(GdiContainerType::single, GdiDataType::char_, 1))
            << "a char_ SINGLE declaration is a String, never a StringArray";
        expect(!value.matches_declaration(GdiContainerType::array, GdiDataType::int64_, 3))
            << "the dtype must be char_";
        expect(!value.matches_declaration(GdiContainerType::array, GdiDataType::byte_, 3));
        expect(!value.matches_declaration(GdiContainerType::array, GdiDataType::char_, 0));

        // The mirror image: a scalar string is single + char_, and never an array.
        auto const text = PropertyValue::make_string("hi");
        expect(text.matches_declaration(GdiContainerType::single, GdiDataType::char_, 1));
        expect(!text.matches_declaration(GdiContainerType::array, GdiDataType::char_, 2));
        expect(!PropertyValue{}.matches_declaration(GdiContainerType::array,
                                                   GdiDataType::char_, 0))
            << "a `none` value matches nothing";
    };

    "string_array_copies_own_their_block"_test = [] {
        char const *const utf8 = "\xE4\xB8\xAD"; // 中 (3 bytes)
        auto const original = make_strings({"first", "second element", kimix::string_view{utf8, 3}});

        auto copy = original; // copy ctor
        expect(copy.equals(original));
        auto assigned = PropertyValue::make_string("placeholder");
        assigned = original; // copy assignment, over a different kind
        expect(assigned.equals(original));
        expect(assigned.kind() == PropertyKind::string_array);

        // A DEEP copy: distinct allocations, so no element byte aliases the source.
        expect(copy.as_string_array_block().data() != original.as_string_array_block().data());
        expect(copy.as_string_array().element(1).data() !=
               original.as_string_array().element(1).data());

        // Independence under destruction: `kept` must still read its elements after
        // the value it was copied from is gone (a shallow block copy would be a
        // use-after-free here, which ASan catches).
        graph_db::PropertyValue kept;
        {
            auto const temporary = make_strings({"ephemeral", ""});
            kept = temporary;
            expect(eq(kept.element_count(), size_t{2}));
        }
        expect(holds_strings(kept, {"ephemeral", ""}));
        expect(kept.equals(make_strings({"ephemeral", ""})));

        // A move hands the block over and leaves the source `none`.
        auto moved = std::move(copy);
        expect(moved.equals(original));
        expect(copy.is_none());
        expect(copy.as_string_array_block().empty());
        auto reassigned = PropertyValue::make_int64(1);
        reassigned = std::move(moved);
        expect(reassigned.equals(original));
        expect(moved.is_none());

        // clear() releases the block: the value becomes `none` and every accessor
        // is empty; the surviving copies are untouched.
        auto scratch = original;
        scratch.clear();
        expect(scratch.is_none());
        expect(eq(scratch.element_count(), size_t{0}));
        expect(scratch.as_string_array().element_count() == 0u);
        expect(scratch.as_string_array_block().empty());
        expect(assigned.equals(original)) << "clearing one value does not touch another";
        expect(holds_strings(assigned, {"first", "second element", kimix::string_view{utf8, 3}}));
    };

    "string_array_record_codec_round_trip"_test = [] {
        char const *const utf8 = "\xE4\xB8\xAD\xE6\x96\x87"; // 中文
        auto const value = make_strings({"alpha", kimix::string_view{utf8, 6}, ""});
        graph_db::PropertyEntry entry;
        entry.name = 77;
        entry.value = value;
        auto const bytes = encode_entry(entry);
        auto const block = value.as_string_array_block();

        // [4B name][1B kind][1B dtype][2B reserved][4B count][8B payload bytes][payload]
        // — the layout ABI 7 did not change; the size is header + the block.
        expect(eq(bytes.size(), size_t{20} + block.size()));
        kimix::span<const std::byte> const record{bytes.data(), bytes.size()};
        expect(eq(le32(record, 0), uint32_t{77})) << "the interned name id";
        expect(eq(std::to_integer<uint32_t>(record[4]), uint32_t{7})) << "kind = string_array";
        expect(eq(std::to_integer<uint32_t>(record[5]), uint32_t{100})) << "dtype = char_";
        expect(record[6] == std::byte{0} && record[7] == std::byte{0})
            << "the 2B reserved field stays zero";
        expect(eq(le32(record, 8), uint32_t{3})) << "count = ELEMENTS, not bytes";
        expect(eq(le64(record, 12), static_cast<uint64_t>(block.size())))
            << "payload bytes = the whole block";
        // The payload IS the block, verbatim: no second framing, no re-encoding.
        kimix::span<const std::byte> const payload{record.data() + 20, block.size()};
        expect(bytes_same(payload, block));

        // Decode it back: byte-identical, name and kind included, cursor at the end.
        graph_db::PropertyEntry back;
        expect(decode_entry(record, back));
        expect(eq(back.name, PropNameId{77}));
        expect(back.value.kind() == PropertyKind::string_array);
        expect(back.value.equals(value)) << "the round trip is byte-equal";
        expect(holds_strings(back.value, {"alpha", kimix::string_view{utf8, 6}, ""}));
        expect(back.value.as_string_array_block().data() != block.data())
            << "the decoded value owns its own block";

        // An EMPTY string_array round-trips too (count 0, a 4-byte payload).
        graph_db::PropertyEntry empty_entry;
        empty_entry.name = 78;
        empty_entry.value = make_strings({});
        auto const empty_bytes = encode_entry(empty_entry);
        expect(eq(empty_bytes.size(), size_t{24}));
        graph_db::PropertyEntry empty_back;
        expect(decode_entry(kimix::span<const std::byte>{empty_bytes.data(), empty_bytes.size()},
                            empty_back));
        expect(empty_back.value.kind() == PropertyKind::string_array);
        expect(eq(empty_back.value.element_count(), size_t{0}));

        // A one-element array of an empty string is NOT the empty array.
        graph_db::PropertyEntry one_empty;
        one_empty.name = 79;
        one_empty.value = make_strings({""});
        auto const one_bytes = encode_entry(one_empty);
        expect(eq(one_bytes.size(), size_t{28})) << "4B count + 4B length + 0 text bytes";
        graph_db::PropertyEntry one_back;
        expect(decode_entry(kimix::span<const std::byte>{one_bytes.data(), one_bytes.size()},
                            one_back));
        expect(eq(one_back.value.element_count(), size_t{1}));
        expect(!one_back.value.equals(make_strings({})))
            << "one empty element differs from zero elements";

        // Several entries of MIXED kinds decode in sequence off one cursor — the
        // record path's real shape (a property list inside a vertex record).
        kimix::vector<std::byte> stream;
        graph_db::PropertyEntry const others[]{
            graph_db::PropertyEntry{1, PropertyValue::make_int64(-7)},
            entry,
            graph_db::PropertyEntry{2, PropertyValue::make_string("text")},
            entry,
            graph_db::PropertyEntry{3, PropertyValue::make_blob(kimix::span<const std::byte>{})},
        };
        for (auto const &item : others) {
            auto const encoded = encode_entry(item);
            stream.insert(stream.end(), encoded.begin(), encoded.end());
        }
        std::byte const *cursor = stream.data();
        std::byte const *const end = stream.data() + stream.size();
        for (size_t i = 0; i < 5; ++i) {
            graph_db::PropertyEntry item;
            expect(graph_db::decode_property_entry(cursor, end, item)) << "entry " << i;
            if (item.name == PropNameId{1}) {
                expect(eq(item.value.as_int64(), int64_t{-7}));
            } else if (item.name == PropNameId{2}) {
                expect(item.value.as_string() == kimix::string_view{"text"});
            } else if (item.name == PropNameId{3}) {
                expect(item.value.kind() == PropertyKind::blob);
            } else {
                expect(item.value.equals(value)) << "the string_array survived in place";
            }
        }
        expect(cursor == end) << "the stream decoded exactly";
    };

    "string_array_record_codec_refuses_a_malformed_block"_test = [] {
        // Every corruption below must make decode_property_entry return false and
        // leave `out` default-constructed — the same defensive posture the fixed-
        // width arrays already have, and what turns a damaged store into the
        // caller's error_bad_file instead of a wild read.
        auto const value = make_strings({"alpha", "beta", "gamma"});
        graph_db::PropertyEntry entry;
        entry.name = 91;
        entry.value = value;
        auto const good = encode_entry(entry);
        auto const check = [](kimix::vector<std::byte> &buffer, char const *what) {
            graph_db::PropertyEntry out;
            bool const decoded =
                decode_entry(kimix::span<const std::byte>{buffer.data(), buffer.size()}, out);
            expect(!decoded) << what;
            expect(out.value.kind() == PropertyKind::none) << "no partial value on a refusal";
            expect(eq(out.name, PropNameId{0}));
        };

        // (a) an element length that runs past the payload,
        auto too_long{good};
        too_long[24] = static_cast<std::byte>(0x40); // element 0 claims 64 bytes for "alpha"
        check(too_long, "an element length that does not fit is refused");

        // (b) a truncated payload (the record's last element cut off),
        auto truncated{good};
        truncated.resize(good.size() - 3);
        check(truncated, "a truncated payload is refused");

        // (c) the record's `count` field disagreeing with the block's own count:
        // the block is self-consistent, so ONLY the cross-check can catch this.
        auto wrong_count{good};
        wrong_count[8] = static_cast<std::byte>(2);
        check(wrong_count, "count vs block disagreement is refused");

        // (d) trailing bytes after the walk (the declared count does not describe
        // the block): payload_len grows by one while the block stays canonical.
        auto trailing{good};
        trailing.push_back(std::byte{0});
        trailing[12] = static_cast<std::byte>(
            static_cast<uint8_t>(std::to_integer<uint8_t>(trailing[12]) + 1));
        check(trailing, "trailing payload bytes are refused");

        // (e) a payload too short to hold even the count field,
        auto stub{good};
        stub.resize(23);
        stub[12] = static_cast<std::byte>(3);
        check(stub, "a payload shorter than 4 bytes is refused");

        // (f) an element count the payload cannot possibly hold,
        auto greedy{good};
        greedy[20] = static_cast<std::byte>(200);
        greedy[8] = static_cast<std::byte>(200); // keep the two counts in agreement
        check(greedy, "an element count larger than the block is refused");

        // The untouched record still decodes, so none of the checks above mutated
        // the buffer of the good case.
        graph_db::PropertyEntry back;
        expect(decode_entry(kimix::span<const std::byte>{good.data(), good.size()}, back));
        expect(back.value.equals(value));
    };

    "string_array_large_record_round_trip"_test = [] {
        // The realistic permission-set shape: tens of thousands of short ids, no
        // artificial cap (plan §1.4). The owners are built first, so the borrowed
        // views stay valid for the make_string_array call.
        constexpr uint32_t kCount = 50'000;
        kimix::vector<kimix::string> ids;
        ids.reserve(kCount);
        for (uint32_t i = 0; i < kCount; ++i) {
            ids.push_back(kimix::format("doc{}", i));
        }
        kimix::vector<kimix::string_view> texts;
        texts.reserve(kCount);
        for (auto const &id : ids) {
            texts.push_back(kimix::string_view{id});
        }
        auto const value = PropertyValue::make_string_array(
            kimix::span<const kimix::string_view>{texts.data(), texts.size()});
        expect(eq(value.element_count(), size_t{kCount}));
        // 4B count + one [4B length][id bytes] entry per element.
        size_t expected_bytes = 4;
        size_t expected_text = 0;
        for (auto const &id : ids) {
            expected_bytes += 4 + id.size();
            expected_text += id.size();
        }
        expect(eq(value.as_string_array_block().size(), expected_bytes));

        graph_db::PropertyEntry entry;
        entry.name = 500;
        entry.value = value;
        auto const bytes = encode_entry(entry);
        expect(eq(bytes.size(), size_t{20} + expected_bytes));
        graph_db::PropertyEntry back;
        expect(decode_entry(kimix::span<const std::byte>{bytes.data(), bytes.size()}, back));
        expect(back.value.equals(value)) << "50k elements survive the codec byte-identically";

        // A cursor walk of the decoded value sees every element in order (ONE
        // aggregate assertion, not one per element), and the spot checks land on
        // the right bytes.
        auto const view = back.value.as_string_array();
        expect(eq(view.element_count(), uint32_t{kCount}));
        uint32_t walked = 0;
        size_t text_bytes = 0;
        for (auto const element : view) {
            text_bytes += element.size();
            ++walked;
        }
        expect(eq(walked, kCount));
        expect(eq(text_bytes, expected_text)) << "every element's bytes survived";
        expect(view.element(0) == kimix::string_view{"doc0"});
        expect(view.element(kCount - 1) == kimix::string_view{"doc49999"});
        expect(view.element(kCount).empty()) << "one past the end is still empty";
        // One corrupt element length in the middle of a big block is caught, not
        // skipped: element 1's length field sits at 20 + 4B count + [4B len + "doc0"].
        kimix::vector<std::byte> damaged{bytes};
        damaged[35] = static_cast<std::byte>(0xff); // ~4 GiB claimed for "doc1"
        graph_db::PropertyEntry refused;
        expect(!decode_entry(kimix::span<const std::byte>{damaged.data(), damaged.size()}, refused))
            << "the decoder does not tolerate a damaged element in a large block";
    };

    // ANY_IN (ABI 7, plan §1.2): the semantics DEFINITION (evaluate/any_in_matches,
    // a linear walk of the set), the probe INDEX that answers the same question on the
    // hot path, and the parity of the two across the whole kind matrix. The C ABI suite
    // exercises the index through a constraint; this suite pins the rule itself, so a
    // divergence between the two implementations is caught at the unit level.
    "any_in_semantics_and_probe_index_parity"_test = [] {
        // The op is part of the ABI: 6 in native, in C and in the managed mirror.
        expect(eq(static_cast<int>(GdiOp::any_in), 6));
        expect(kimix::string_view{graph_db::op_name(GdiOp::any_in)} ==
               kimix::string_view{"any_in"});

        auto const text_set = make_strings({"beta", "alpha", "gamma"});
        auto const empty_set = make_strings({});
        kimix::vector<int64_t> const int_elements{5, 3};
        auto const int_set = graph_db::PropertyValue::make_int64_array(
            kimix::span<const int64_t>{int_elements.data(), int_elements.size()});
        kimix::vector<double> const real_elements{1.0, 2.0};
        auto const real_array = graph_db::PropertyValue::make_double_array(
            kimix::span<const double>{real_elements.data(), real_elements.size()});
        kimix::vector<int64_t> const hit_ints{4, 3};
        kimix::vector<int64_t> const miss_ints{4, 6};
        auto const ints_hit = graph_db::PropertyValue::make_int64_array(
            kimix::span<const int64_t>{hit_ints.data(), hit_ints.size()});
        auto const ints_miss = graph_db::PropertyValue::make_int64_array(
            kimix::span<const int64_t>{miss_ints.data(), miss_ints.size()});
        auto const strings_hit = make_strings({"zz", "gamma"});
        auto const strings_miss = make_strings({"zz", "delta"});
        auto const not_a_set = make_strings({"a"});  // reused below as an ACTUAL only

        struct Case {
            graph_db::PropertyValue set;
            graph_db::PropertyValue actual;
            bool expected;
            char const *what;
        };
        kimix::vector<Case> cases;
        cases.push_back({text_set, graph_db::PropertyValue::make_string("alpha"), true,
                         "scalar string in the set"});
        cases.push_back({text_set, graph_db::PropertyValue::make_string("delta"), false,
                         "scalar string not in the set"});
        cases.push_back({text_set, graph_db::PropertyValue::make_string(""), false,
                         "the empty string is only a member if the set carries one"});
        cases.push_back({text_set, strings_hit, true, "array actual: ANY element in the set"});
        cases.push_back({text_set, strings_miss, false, "array actual: no element in the set"});
        cases.push_back({int_set, graph_db::PropertyValue::make_int64(3), true,
                         "scalar int64 in the set"});
        cases.push_back({int_set, graph_db::PropertyValue::make_int64(4), false,
                         "scalar int64 not in the set"});
        cases.push_back({int_set, ints_hit, true, "int64 array actual: any element"});
        cases.push_back({int_set, ints_miss, false, "int64 array actual: none"});
        cases.push_back({int_set, graph_db::PropertyValue::make_string("3"), false,
                         "a string is never a member of an int64 set"});
        cases.push_back({text_set, graph_db::PropertyValue::make_int64(3), false,
                         "an int64 is never a member of a string set"});
        cases.push_back({empty_set, graph_db::PropertyValue::make_string("alpha"), false,
                         "an empty set matches nothing"});
        cases.push_back({empty_set, strings_hit, false, "an empty set matches no array"});
        cases.push_back({empty_set, graph_db::PropertyValue::make_int64(3), false,
                         "an empty set matches no scalar"});
        cases.push_back({graph_db::PropertyValue::make_string("alpha"),
                         graph_db::PropertyValue::make_string("alpha"), false,
                         "a scalar condition value is not a set"});
        cases.push_back({not_a_set, not_a_set, true,
                         "an array actual probes its ELEMENTS, not the whole array"});
        cases.push_back({make_strings({"a", "b"}), make_strings({"c"}), false,
                         "no element of the actual is in the set"});
        cases.push_back({text_set, graph_db::PropertyValue::make_double(1.5), false,
                         "a double actual is not a member of anything"});
        cases.push_back({text_set, real_array, false, "a double_array actual never matches"});
        cases.push_back({int_set, real_array, false, "nor against an int64 set"});
        cases.push_back({real_array, strings_hit, false, "a double_array is not a set"});
        cases.push_back({graph_db::PropertyValue::make_blob(
                             kimix::span<const std::byte>{reinterpret_cast<std::byte const *>(
                                                            "beta"), 4}),
                         graph_db::PropertyValue::make_string("beta"), false,
                         "a blob is not a set either"});

        for (auto const &item : cases) {
            // 1. the semantics definition (linear walk), reached by evaluate().
            expect(eq(graph_db::evaluate(GdiOp::any_in, item.actual, item.set), item.expected))
                << "evaluate: " << item.what;
            // 2. the index a condition built at add time (the hot-path answer).
            auto const condition = graph_db::ConstraintCondition::on_property(
                1, GdiOp::any_in, item.set, static_cast<uint32_t>(item.set.element_count()));
            expect(eq(condition.matches_value(item.actual), item.expected))
                << "index: " << item.what;
            // 3. the defensive fallback: a hand-built condition carries the set but no
            // index; it must answer exactly like the other two, never silently false.
            graph_db::ConstraintCondition const hand_built{
                .kind = graph_db::ConstraintCondition::Kind::property,
                .property = 1,
                .op = GdiOp::any_in,
                .value = item.set};
            expect(hand_built.any_in_set.shape == graph_db::AnyInIndex::Shape::none)
                << "aggregate initialisation builds no index by design";
            expect(eq(hand_built.matches_value(item.actual), item.expected)) << "fallback: "
                                                                            << item.what;
            // and the same condition through the other ops keeps the old behaviour
            expect(eq(condition.matches_value(item.actual),
                      graph_db::evaluate(GdiOp::any_in, item.actual, item.set)))
                << "index and definition never disagree: " << item.what;
        }

        // Ordering ops on a set value are NOT membership tests: evaluate() keeps its
        // equality-only rule for two same-kind arrays, and any_in is the only op that
        // looks inside one.
        expect(graph_db::evaluate(GdiOp::equal, not_a_set, not_a_set))
            << "whole-array equality is still available";
        expect(!graph_db::evaluate(GdiOp::greater, text_set, text_set))
            << "a set is not orderable";

        // The count of an any_in condition is the SET's exact element count (§1.4), and
        // 0 survives for the empty set instead of being normalised to 1.
        auto const empty_condition = graph_db::ConstraintCondition::on_property(
            1, GdiOp::any_in, empty_set, 0);
        expect(eq(empty_condition.count, uint32_t{0})) << "the empty set keeps count 0";
        expect(!empty_condition.matches_value(graph_db::PropertyValue::make_string("alpha")));
        auto const scalar_condition =
            graph_db::ConstraintCondition::on_property(1, GdiOp::equal,
                                                       graph_db::PropertyValue::make_int64(7), 0);
        expect(eq(scalar_condition.count, uint32_t{1}))
            << "every other op still normalises a 0 count to the scalar reading";

        // A Constraint copy carries a DEEP copy of the index: the registry and every
        // Constraint::with snapshot keep answering from it (value semantics, §1.2).
        auto const big = [] {
            kimix::vector<kimix::string_view> elements;
            kimix::vector<kimix::string> names;
            names.reserve(2000);
            for (uint32_t i = 0; i < 2000; ++i) {
                names.push_back(kimix::format("id-{:04}", i));
            }
            for (auto const &name : names) {
                elements.push_back(kimix::string_view{name.data(), name.size()});
            }
            return graph_db::PropertyValue::make_string_array(
                kimix::span<const kimix::string_view>{elements.data(), elements.size()});
        }();
        auto const big_condition = graph_db::ConstraintCondition::on_property(
            1, GdiOp::any_in, big, 2000);
        expect(eq(big_condition.count, uint32_t{2000}));
        auto const snapshot = Constraint{}.with(big_condition); // copies the condition
        expect(eq(static_cast<int>(snapshot.conditions[0].any_in_set.shape),
                  static_cast<int>(graph_db::AnyInIndex::Shape::strings)))
            << "the compiler-generated copy deep-copied the index, not just the value";
        expect(!snapshot.conditions[0].any_in_set.strings.empty() &&
               snapshot.conditions[0].any_in_set.strings.data() !=
                   big_condition.any_in_set.strings.data())
            << "the copy owns its own element storage";
        for (char const *member : {"id-0000", "id-0999", "id-1999"}) {
            auto const actual = graph_db::PropertyValue::make_string(member);
            expect(eq(big_condition.matches_value(actual), true)) << member;
            expect(eq(snapshot.conditions[0].matches_value(actual), true))
                << member << " through a Constraint copy";
            expect(eq(graph_db::evaluate(GdiOp::any_in, actual, big), true))
                << member << " against the linear definition";
        }
        for (char const *miss : {"id-2000", "id-099", "", "ID-0000"}) {
            auto const actual = graph_db::PropertyValue::make_string(miss);
            expect(eq(big_condition.matches_value(actual), false)) << miss;
            expect(eq(graph_db::evaluate(GdiOp::any_in, actual, big), false)) << miss;
        }
        auto const big_array_actual = make_strings({"x", "id-1500"});
        expect(eq(big_condition.matches_value(big_array_actual), true))
            << "an array actual probes the index element by element";
        expect(eq(graph_db::evaluate(GdiOp::any_in, big_array_actual, big), true));
    };
}
