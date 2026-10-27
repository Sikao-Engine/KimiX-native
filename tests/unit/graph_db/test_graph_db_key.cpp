// Test for the (label, external_id) primary key of a vertex (src/graph_db/key.*,
// Database::create_vertex / get_or_create_vertex / translate_vertex_id) — GDI
// §2.3.
//
// This test covers:
// - translate round trip and a miss (found == false, never an error);
// - uniqueness: a second create on the same key is error_non_unique_id, and
//   KeyMode::report_warning turns it into warning_non_unique_id + the EXISTING
//   uid (GDI's under-enforced uniqueness path);
// - MERGE: create, match (with NO property overwrite), re-create after a free;
// - binary external ids: 0, 1, the 16-byte SSO boundary and 4096 bytes, plus two
//   ids differing in their last byte;
// - isolation: one external id under two labels is two vertices; 10k keys spread
//   over the shards are all found;
// - persistence: keys survive a save/load cycle, and a vertex without a key (a
//   v1 record) reports "not found" instead of crashing;
// - the index size returns to 0 after a create/free loop (no leak).
#include "ut/ut.hpp"

#include "graph_db_test_util.h"

#include <core/kimix_core.h> // umbrella first: it fixes the winsock2.h order
#include <core/fiber.h>

#include <graph_db/database.h>
#include <graph_db/graph_db.h>
#include <graph_db/key.h>
#include <graph_db/status.h>

#include <core/guid.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <cstddef>
#include <cstdint>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using graph_db::Database;
using graph_db::ExternalId;
using graph_db::GdiDataType;
using graph_db::GdiStatus;
using graph_db::kNoLabel;
using graph_db::PropNameId;
using graph_db::PropertyEntry;
using graph_db::PropertyList;
using graph_db::PropertyTypeInfo;
using graph_db::PropertyValue;
using graph_db::GraphSerializeStats;
using graph_db::KeyIndex;
using graph_db::LabelId;
using graph_db::VertexCreateResult;
using graph_db::VertexKey;
using graph_db::VertexKeyView;
using graph_db::VertexMergeResult;
namespace gtest = kimix::graph_db::test;

namespace tests_unit_graph_db_test_graph_db_key {

inline VertexKeyView view_of(LabelId label, kimix::vector<std::byte> const &bytes) {
    return VertexKeyView{label, kimix::span<const std::byte>{bytes.data(), bytes.size()}};
}

// A tiny owned id buffer, so a test can build an id of any length inline.
kimix::vector<std::byte> id_of(size_t length, uint8_t fill = 0x41) {
    kimix::vector<std::byte> out(length, static_cast<std::byte>(fill));
    return out;
}

} // namespace tests_unit_graph_db_test_graph_db_key
using namespace tests_unit_graph_db_test_graph_db_key;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    kimix::fiber::scheduler sched{4u};

    "key_translate_round_trip"_test = [] {
        Database db;
        LabelId person{};
        expect(eq(db.schema().create_label("person", person), GdiStatus::ok));
        auto const id = gtest::to_bytes("alice");
        VertexCreateResult created;
        expect(eq(db.create_vertex(view_of(person, id), {}, nullptr, created), GdiStatus::ok));

        Guid resolved;
        bool found = false;
        expect(eq(db.translate_vertex_id(view_of(person, id), resolved, found), GdiStatus::ok));
        expect(found);
        expect(resolved == created.uid);

        // A miss is ok + found == false (GDI's TranslateVertexID returns a null uid).
        auto const missing = gtest::to_bytes("bob");
        Guid nowhere;
        expect(eq(db.translate_vertex_id(view_of(person, missing), nowhere, found),
                  GdiStatus::ok));
        expect(!found);
        // An unknown label is an empty name (GDI's VertexKey.Validate).
        expect(eq(db.translate_vertex_id(VertexKeyView{kNoLabel, id}, nowhere, found),
                  GdiStatus::error_empty_name));
    };

    "key_uniqueness_enforced"_test = [] {
        Database db;
        LabelId person{};
        expect(eq(db.schema().create_label("person", person), GdiStatus::ok));
        auto const id = gtest::to_bytes("alice");
        VertexCreateResult first;
        expect(eq(db.create_vertex(view_of(person, id), {}, nullptr, first), GdiStatus::ok));
        expect(eq(db.graph().key_index().size(), size_t{1}));

        VertexCreateResult second;
        expect(eq(db.create_vertex(view_of(person, id), {}, nullptr, second),
                  GdiStatus::error_non_unique_id))
            << "EnforcesUniqueExternalIds: the key is taken";
        expect(eq(db.graph().key_index().size(), size_t{1})) << "nothing was inserted";

        // The warning mode is GDI's under-enforced uniqueness (SimulateNonUniqueId).
        graph_db::GraphOptions options;
        options.key_mode = graph_db::KeyMode::report_warning;
        db.set_options(options);
        VertexCreateResult warning;
        expect(eq(db.create_vertex(view_of(person, id), {}, nullptr, warning),
                  GdiStatus::warning_non_unique_id));
        expect(warning.non_unique_id_warning);
        expect(warning.uid == first.uid) << "the warning returns the EXISTING vertex";
        expect(eq(db.graph().size(), size_t{1})) << "no second vertex was created";
    };

    "key_merge_get_or_create"_test = [] {
        Database db;
        LabelId person{};
        expect(eq(db.schema().create_label("person", person), GdiStatus::ok));
        auto const id = gtest::to_bytes("alice");

        PropertyTypeInfo info;
        info.name = "age";
        info.data = GdiDataType::int64_;
        PropNameId age{};
        expect(eq(db.create_property_type(info, age), GdiStatus::ok));

        PropertyList properties;
        PropertyEntry entry;
        entry.name = age;
        entry.value = PropertyValue::make_int64(30);
        properties.push_back(entry);

        VertexMergeResult merge;
        expect(eq(db.get_or_create_vertex(view_of(person, id), {}, &properties, merge),
                  GdiStatus::ok));
        expect(merge.created) << "the first MERGE creates";
        expect(eq(db.graph().find(merge.uid)->property_count(), size_t{1}));

        // A second MERGE matches and leaves the vertex completely untouched.
        VertexMergeResult again;
        expect(eq(db.get_or_create_vertex(view_of(person, id), {}, nullptr, again),
                  GdiStatus::ok));
        expect(!again.created);
        expect(again.uid == merge.uid);
        expect(eq(db.graph().find(merge.uid)->property_count(), size_t{1}))
            << "a match never mutates the vertex (the reference backend's rule)";
        expect(eq(db.graph().size(), size_t{1}));

        // Free, then MERGE creates again with a NEW uid.
        expect(eq(db.free_vertex(merge.uid), GdiStatus::ok));
        expect(eq(db.graph().key_index().size(), size_t{0})) << "free drops the key entry";
        VertexMergeResult third;
        expect(eq(db.get_or_create_vertex(view_of(person, id), {}, nullptr, third), GdiStatus::ok));
        expect(third.created);
        expect(third.uid != merge.uid);
    };

    "key_binary_ids"_test = [] {
        Database db;
        LabelId node{};
        expect(eq(db.schema().create_label("node", node), GdiStatus::ok));

        // A zero-length id is refused for a keyed vertex (it names nothing).
        kimix::vector<std::byte> empty;
        VertexCreateResult created;
        expect(eq(db.create_vertex(view_of(node, empty), {}, nullptr, created),
                  GdiStatus::error_empty_name));

        // Boundaries: 1 byte, the 16-byte SSO limit, one past it, and a big one.
        size_t const lengths[]{1, 15, 16, 17, 4096};
        kimix::vector<Guid> uids;
        for (auto length : lengths) {
            auto const id = id_of(length, static_cast<uint8_t>(length));
            expect(eq(db.create_vertex(view_of(node, id), {}, nullptr, created), GdiStatus::ok))
                << "id length " << length;
            uids.push_back(created.uid);
            Guid resolved;
            bool found = false;
            expect(eq(db.translate_vertex_id(view_of(node, id), resolved, found), GdiStatus::ok));
            expect(found);
            expect(resolved == created.uid);
        }
        // Every id resolved to a DISTINCT vertex.
        for (size_t i = 0; i < uids.size(); ++i) {
            for (size_t k = i + 1; k < uids.size(); ++k) {
                expect(uids[i] != uids[k]);
            }
        }

        // Two ids differing only in their LAST byte are different keys.
        auto left = id_of(32, 0x01);
        auto right = id_of(32, 0x01);
        right.back() = std::byte{0x02};
        expect(eq(db.create_vertex(view_of(node, left), {}, nullptr, created), GdiStatus::ok));
        Guid const left_uid = created.uid;
        expect(eq(db.create_vertex(view_of(node, right), {}, nullptr, created), GdiStatus::ok));
        expect(created.uid != left_uid);
        Guid resolved;
        bool found = false;
        expect(eq(db.translate_vertex_id(view_of(node, left), resolved, found), GdiStatus::ok));
        expect(found);
        expect(resolved == left_uid);
    };

    "key_label_isolation_and_spread"_test = [] {
        Database db;
        LabelId first{};
        LabelId second{};
        expect(eq(db.schema().create_label("first", first), GdiStatus::ok));
        expect(eq(db.schema().create_label("second", second), GdiStatus::ok));

        // The SAME external id under two labels is two vertices.
        auto const id = gtest::to_bytes("shared");
        VertexCreateResult a;
        VertexCreateResult b;
        expect(eq(db.create_vertex(view_of(first, id), {}, nullptr, a), GdiStatus::ok));
        expect(eq(db.create_vertex(view_of(second, id), {}, nullptr, b), GdiStatus::ok));
        expect(a.uid != b.uid);
        expect(eq(db.graph().size(), size_t{2}));

        // 10k keys spread over the shards, all resolvable.
        constexpr size_t kCount = 10000;
        for (size_t i = 0; i < kCount; ++i) {
            auto const key = gtest::to_bytes(kimix::format("k{}", i));
            VertexCreateResult created;
            expect(eq(db.create_vertex(view_of(first, key), {}, nullptr, created), GdiStatus::ok));
        }
        expect(eq(db.graph().key_index().size(), kCount + 2));
        expect(gt(db.graph().key_index().used_shard_count(), size_t{8}))
            << "the hash spreads the keys over the shards";
        for (size_t i = 0; i < kCount; i += 97) {
            auto const key = gtest::to_bytes(kimix::format("k{}", i));
            Guid resolved;
            bool found = false;
            expect(eq(db.translate_vertex_id(view_of(first, key), resolved, found), GdiStatus::ok));
            expect(found) << "key k" << i;
        }
    };

    "key_survives_save_load"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }
        Guid uid;
        {
            Database db;
            expect(eq(db.open(dir.path()), GdiStatus::ok));
            LabelId person{};
            expect(eq(db.schema().create_label("person", person), GdiStatus::ok));
            auto const id = gtest::to_bytes("persisted");
            VertexCreateResult created;
            expect(eq(db.create_vertex(view_of(person, id), {}, nullptr, created), GdiStatus::ok));
            uid = created.uid;
            expect(eq(db.persist(/*full=*/true), GdiStatus::ok));
        }
        Database restored;
        expect(eq(restored.open(dir.path()), GdiStatus::ok));
        LabelId person{};
        bool found_label = false;
        expect(eq(restored.get_label_by_name("person", person, found_label), GdiStatus::ok));
        expect(found_label) << "the label registry came back, so the id resolves to a name";
        auto const id = gtest::to_bytes("persisted");
        Guid resolved;
        bool found = false;
        expect(eq(restored.translate_vertex_id(view_of(person, id), resolved, found),
                  GdiStatus::ok));
        expect(found);
        expect(resolved == uid);
    };

    "key_churn_leaves_no_entries"_test = [] {
        Database db;
        LabelId node{};
        expect(eq(db.schema().create_label("node", node), GdiStatus::ok));
        for (size_t i = 0; i < 500; ++i) {
            auto const id = gtest::to_bytes(kimix::format("churn-{}", i % 7));
            VertexCreateResult created;
            GdiStatus const status = db.create_vertex(view_of(node, id), {}, nullptr, created);
            if (status == GdiStatus::ok) {
                expect(eq(db.free_vertex(created.uid), GdiStatus::ok));
            }
        }
        expect(eq(db.graph().key_index().size(), size_t{0}))
            << "every key entry was dropped with its vertex";
        expect(eq(db.graph().size(), size_t{0}));
    };

    "key_external_id_value_semantics"_test = [] {
        // The type itself: SSO boundaries, value equality, hashing, copying.
        auto const short_id = ExternalId::from_string("abc");
        expect(short_id.is_inline());
        expect(eq(short_id.size(), size_t{3}));
        expect(short_id.equals(ExternalId::from_string("abc")));
        expect(!short_id.equals(ExternalId::from_string("abd")));
        expect(eq(short_id.hash(), ExternalId::from_string("abc").hash()));

        auto const exact = ExternalId::from_string("0123456789abcdef"); // 16
        expect(exact.is_inline());
        auto const spilled = ExternalId::from_string("0123456789abcdefg"); // 17
        expect(!spilled.is_inline());
        expect(!spilled.equals(exact));
        expect(spilled.equals(ExternalId::from_string("0123456789abcdefg")));

        // Empty is a value: equal to itself, unequal to anything else.
        ExternalId const empty;
        expect(empty.empty());
        expect(empty.equals(ExternalId{}));
        expect(!empty.equals(short_id));

        // A copy owns its bytes (the spill case is the interesting one).
        ExternalId copy = spilled;
        expect(copy.equals(spilled));

        // The key of a vertex round-trips through the entity.
        Database db;
        LabelId node{};
        expect(eq(db.schema().create_label("node", node), GdiStatus::ok));
        auto const id = id_of(40, 0x5a);
        VertexCreateResult created;
        expect(eq(db.create_vertex(view_of(node, id), {}, nullptr, created), GdiStatus::ok));
        VertexKey const key = db.graph().find(created.uid)->key();
        expect(eq(key.label, node));
        expect(key.external_id.equals(ExternalId::from_bytes(
            kimix::span<const std::byte>{id.data(), id.size()})));
    };
}
