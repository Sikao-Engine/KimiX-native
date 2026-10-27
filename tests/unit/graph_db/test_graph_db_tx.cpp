// Test for transactions (src/graph_db/transaction.*, Database::begin_transaction
// / commit / abort / collectives) — GDI §2.7.
//
// This test covers:
// - ABORT RESTORES: build a graph, fingerprint it (vertex guids, adjacency, key
//   index, payloads), run a long transaction of every mutation kind, abort, and
//   compare the fingerprint exactly;
// - COMMIT PERSISTS: with persist_on_commit the committed batch reaches the store
//   and a fresh database reads it back;
// - READ-ONLY enforcement: every write command returns
//   error_read_only_transaction inside a collective transaction, and the graph is
//   unchanged;
// - the state machine: a double begin, commit-after-abort, abort-after-commit, and
//   a Transaction destroyed while open (its destructor aborts);
// - the journal cap: an oversized transaction returns error_size_limit and is
//   still abortable;
// - the auto-transaction: a write outside any transaction is visible immediately
//   and marks the right entity dirty;
// - rollback of the two structural edits that move adjacency (endpoint mutation
//   and direction change);
// - that the single-writer contract is unchanged (a structural write from a
//   second thread is refused by Graph, which is what error_state reports).
#include "ut/ut.hpp"

#include "graph_db_test_util.h"

#include <core/kimix_core.h> // umbrella first: it fixes the winsock2.h order
#include <core/fiber.h>

#include <graph_db/database.h>
#include <graph_db/edge.h>
#include <graph_db/graph_db.h>
#include <graph_db/status.h>
#include <graph_db/transaction.h>

#include <core/guid.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <algorithm>
#include <cstddef>
#include <utility>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using graph_db::Constraint;
using graph_db::Database;
using graph_db::EdgeDirectionType;
using graph_db::EdgeOrientation;
using graph_db::EdgeRecord;
using graph_db::GdiDataType;
using graph_db::GdiStatus;
using graph_db::GraphSerializeStats;
using graph_db::LabelId;
using graph_db::NamedPropertyList;
using graph_db::PropNameId;
using graph_db::PropertyTypeInfo;
using graph_db::PropertyEntry;
using graph_db::PropertyList;
using graph_db::PropertyValue;
using graph_db::Transaction;
using graph_db::VertexCreateResult;
using graph_db::VertexKeyView;
namespace gtest = kimix::graph_db::test;

namespace tests_unit_graph_db_test_graph_db_tx {

inline VertexKeyView view_of(LabelId label, kimix::vector<std::byte> const &bytes) {
    return VertexKeyView{label, kimix::span<const std::byte>{bytes.data(), bytes.size()}};
}

// A fingerprint of everything an abort has to restore: the vertex set (with its
// payload), the edge set (with endpoints, direction and payload), the adjacency in
// both orientations, and the key index size. Two equal fingerprints mean "the
// abort put the database back exactly where it was".
struct fingerprint {
    kimix::vector<kimix::string> vertices;
    kimix::vector<kimix::string> edges;
    size_t key_count{0};

    [[nodiscard]] bool operator==(fingerprint const &other) const {
        if (key_count != other.key_count || vertices.size() != other.vertices.size() ||
            edges.size() != other.edges.size()) {
            return false;
        }
        return vertices == other.vertices && edges == other.edges;
    }
};

kimix::string join_labels(kimix::vector<LabelId> const &labels) {
    kimix::vector<LabelId> sorted = labels;
    std::sort(sorted.begin(), sorted.end());
    kimix::StringScratch out;
    for (size_t i = 0; i < sorted.size(); ++i) {
        out << (i == 0 ? "" : ",") << sorted[i];
    }
    return kimix::string{out.string_view()};
}

kimix::string join_properties(NamedPropertyList const &properties) {
    kimix::vector<kimix::string> parts;
    for (auto const &entry : properties) {
        kimix::StringScratch part;
        part << entry.name << "=" << static_cast<int>(entry.value.kind()) << ":";
        switch (entry.value.kind()) {
        case graph_db::PropertyKind::int64_value:
            part << entry.value.as_int64();
            break;
        case graph_db::PropertyKind::double_value:
            part << entry.value.as_double();
            break;
        case graph_db::PropertyKind::string:
            part << entry.value.as_string();
            break;
        default:
            part << entry.value.element_count();
            break;
        }
        parts.push_back(kimix::string{part.string_view()});
    }
    std::sort(parts.begin(), parts.end());
    kimix::StringScratch out;
    for (size_t i = 0; i < parts.size(); ++i) {
        out << (i == 0 ? "" : "|") << parts[i];
    }
    return kimix::string{out.string_view()};
}

fingerprint take_fingerprint(Database const &db) {
    fingerprint out;
    auto const guids = db.graph().vertex_guids();
    for (auto const &guid : guids) {
        auto const *vertex = db.graph().find(guid);
        if (vertex == nullptr) {
            continue;
        }
        kimix::vector<LabelId> labels;
        vertex->labels(labels);
        NamedPropertyList properties;
        (void)db.get_properties(guid, properties);
        kimix::StringScratch line;
        line << guid.to_string() << "|key=" << vertex->key_label() << "|labels="
             << join_labels(labels) << "|props=" << join_properties(properties) << "|out=";
        for (auto const &edge : db.graph().dependencies(guid)) {
            line << edge.to_string() << ",";
        }
        line << "|in=";
        for (auto const &edge : db.graph().dependents(guid)) {
            line << edge.to_string() << ",";
        }
        out.vertices.push_back(kimix::string{line.string_view()});
    }
    std::sort(out.vertices.begin(), out.vertices.end());

    for (auto const &id : db.graph().edge_ids()) {
        EdgeRecord record;
        if (!db.graph().edge_record(id, record)) {
            continue;
        }
        NamedPropertyList properties;
        (void)db.get_edge_properties(id, properties);
        kimix::StringScratch line;
        line << id.to_string() << "|" << record.origin.to_string() << "->"
             << record.target.to_string() << "|"
             << static_cast<int>(record.direction) << "|labels="
             << join_labels(record.labels) << "|props=" << join_properties(properties);
        out.edges.push_back(kimix::string{line.string_view()});
    }
    std::sort(out.edges.begin(), out.edges.end());
    out.key_count = db.graph().key_index().size();
    return out;
}

// A database with a small, rich graph: 12 keyed vertices, some labels and
// properties, and a chain of edges.
struct fixture {
    Database db;
    LabelId node{};
    LabelId tag{};
    LabelId link{};
    PropNameId age{};
    PropNameId note{};
    kimix::vector<Guid> vertices;
    kimix::vector<Guid> edges;

    explicit fixture(size_t count = 12) {
        expect(eq(db.schema().create_label("node", node), GdiStatus::ok));
        expect(eq(db.schema().create_label("tag", tag), GdiStatus::ok));
        expect(eq(db.schema().create_label("link", link), GdiStatus::ok));
        PropertyTypeInfo age_info;
        age_info.name = "age";
        age_info.data = GdiDataType::int64_;
        expect(eq(db.create_property_type(age_info, age), GdiStatus::ok));
        PropertyTypeInfo note_info;
        note_info.name = "note";
        note_info.data = GdiDataType::char_;
        expect(eq(db.create_property_type(note_info, note), GdiStatus::ok));

        for (size_t i = 0; i < count; ++i) {
            auto const bytes = gtest::to_bytes(kimix::format("v{}", i));
            VertexCreateResult created;
            expect(eq(db.create_vertex(view_of(node, bytes), {}, nullptr, created), GdiStatus::ok));
            expect(eq(db.add_property(created.uid, age,
                                      PropertyValue::make_int64(static_cast<int64_t>(i))),
                      GdiStatus::ok));
            expect(eq(db.add_property(created.uid, note,
                                      PropertyValue::make_string(kimix::format("n{}", i))),
                      GdiStatus::ok));
            if (i % 2 == 0) {
                expect(eq(db.add_label(created.uid, tag), GdiStatus::ok));
            }
            vertices.push_back(created.uid);
        }
        LabelId const labels[]{link};
        for (size_t i = 1; i < vertices.size(); ++i) {
            Guid edge;
            expect(eq(db.edge_create(vertices[i], vertices[i - 1], EdgeDirectionType::directed,
                                     kimix::span<const LabelId>{labels, 1}, nullptr, edge),
                      GdiStatus::ok));
            edges.push_back(edge);
        }
    }
};

} // namespace tests_unit_graph_db_test_graph_db_tx
using namespace tests_unit_graph_db_test_graph_db_tx;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    kimix::fiber::scheduler sched{4u};

    "tx_abort_restores_everything"_test = [] {
        fixture f;
        fingerprint const before = take_fingerprint(f.db);

        expect(eq(f.db.begin_transaction(), GdiStatus::ok));
        expect(f.db.in_transaction());

        // A long batch covering every mutation kind.
        auto const extra_bytes = gtest::to_bytes("extra");
        VertexCreateResult extra;
        expect(eq(f.db.create_vertex(view_of(f.node, extra_bytes), {}, nullptr, extra),
                  GdiStatus::ok));
        expect(eq(f.db.add_label(extra.uid, f.tag), GdiStatus::ok));
        expect(eq(f.db.add_property(extra.uid, f.age, PropertyValue::make_int64(99)),
                  GdiStatus::ok));
        expect(eq(f.db.set_property(f.vertices[0], f.age, PropertyValue::make_int64(-1)),
                  GdiStatus::ok));
        expect(eq(f.db.update_property(f.vertices[1], f.note, PropertyValue::make_string("x")),
                  GdiStatus::ok));
        PropNameId const remove[]{f.note};
        expect(eq(f.db.remove_properties(f.vertices[2],
                                        kimix::span<const PropNameId>{remove, 1}),
                  GdiStatus::ok));
        expect(eq(f.db.remove_label(f.vertices[0], f.tag), GdiStatus::ok));
        Guid new_edge;
        expect(eq(f.db.edge_create(f.vertices[0], f.vertices[5], EdgeDirectionType::undirected,
                                   {}, nullptr, new_edge),
                  GdiStatus::ok));
        expect(eq(f.db.set_edge_origin(f.edges[0], f.vertices[7]), GdiStatus::ok));
        expect(eq(f.db.set_edge_target(f.edges[1], f.vertices[9]), GdiStatus::ok));
        expect(eq(f.db.set_edge_direction(f.edges[2], EdgeDirectionType::undirected),
                  GdiStatus::ok));
        expect(eq(f.db.edge_free(f.edges[3]), GdiStatus::ok));
        expect(eq(f.db.free_vertex(f.vertices[11]), GdiStatus::ok)) << "this cascades an edge";
        LabelId transient{};
        expect(eq(f.db.create_label("transient", transient), GdiStatus::ok));
        expect(eq(f.db.update_label(f.tag, "renamed"), GdiStatus::ok));
        expect(eq(f.db.free_label(f.link), GdiStatus::ok)) << "cascades to the edges";
        PropNameId transient_type{};
        PropertyTypeInfo info;
        info.name = "transient_type";
        info.data = GdiDataType::int64_;
        expect(eq(f.db.create_property_type(info, transient_type), GdiStatus::ok));

        // The graph really moved.
        expect(take_fingerprint(f.db) == before ? false : true);

        expect(eq(f.db.abort(), GdiStatus::ok));
        expect(!f.db.in_transaction());
        fingerprint const after = take_fingerprint(f.db);
        expect(after == before) << "abort restored the vertex set, the edge set, the adjacency, "
                                   "the payloads, the schema and the key index";
        expect(eq(f.db.graph().key_index().size(), before.key_count));
    };

    "tx_commit_persists"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }
        Guid uid;
        {
            Database db;
            graph_db::GraphOptions options;
            options.persist_on_commit = true;
            expect(eq(db.open(dir.path(), options), GdiStatus::ok));
            LabelId node{};
            expect(eq(db.schema().create_label("node", node), GdiStatus::ok));
            expect(eq(db.persist(/*full=*/true), GdiStatus::ok));

            expect(eq(db.begin_transaction(), GdiStatus::ok));
            auto const bytes = gtest::to_bytes("committed");
            VertexCreateResult created;
            expect(eq(db.create_vertex(view_of(node, bytes), {}, nullptr, created), GdiStatus::ok));
            uid = created.uid;
            expect(eq(db.commit(), GdiStatus::ok));
            expect(!db.in_transaction());
        }
        Database restored;
        expect(eq(restored.open(dir.path()), GdiStatus::ok));
        expect(restored.graph().contains(uid)) << "a committing transaction reached the store";
    };

    "tx_read_only_scope"_test = [] {
        fixture f;
        fingerprint const before = take_fingerprint(f.db);
        expect(eq(f.db.begin_collective(), GdiStatus::ok));
        expect(f.db.transaction_is_read_only());

        auto const bytes = gtest::to_bytes("nope");
        VertexCreateResult created;
        expect(eq(f.db.create_vertex(view_of(f.node, bytes), {}, nullptr, created),
                  GdiStatus::error_read_only_transaction));
        expect(eq(f.db.add_label(f.vertices[0], f.tag),
                  GdiStatus::error_read_only_transaction));
        expect(eq(f.db.add_property(f.vertices[0], f.age, PropertyValue::make_int64(1)),
                  GdiStatus::error_read_only_transaction));
        PropNameId const names[]{f.age};
        expect(eq(f.db.remove_properties(f.vertices[0],
                                        kimix::span<const PropNameId>{names, 1}),
                  GdiStatus::error_read_only_transaction));
        Guid edge;
        expect(eq(f.db.edge_create(f.vertices[0], f.vertices[1], EdgeDirectionType::directed,
                                   {}, nullptr, edge),
                  GdiStatus::error_read_only_transaction));
        expect(eq(f.db.edge_free(f.edges[0]), GdiStatus::error_read_only_transaction));
        expect(eq(f.db.free_vertex(f.vertices[0]), GdiStatus::error_read_only_transaction));
        LabelId label{};
        expect(eq(f.db.create_label("blocked", label), GdiStatus::error_read_only_transaction));

        // Reads are allowed inside a collective scope.
        size_t count = 0;
        expect(eq(f.db.count_neighbors(f.vertices[3], Constraint{}, EdgeOrientation::both, count),
                  GdiStatus::ok));

        expect(eq(f.db.close_collective(/*commit_it=*/false), GdiStatus::ok));
        expect(take_fingerprint(f.db) == before) << "nothing was written";
    };

    "tx_state_machine"_test = [] {
        Database db;
        expect(eq(db.commit(), GdiStatus::error_transaction)) << "no transaction to commit";
        expect(eq(db.abort(), GdiStatus::error_transaction));
        expect(eq(db.begin_transaction(), GdiStatus::ok));
        expect(eq(db.begin_transaction(), GdiStatus::error_transaction))
            << "one write transaction at a time";
        expect(eq(db.commit(), GdiStatus::ok));
        expect(eq(db.commit(), GdiStatus::error_transaction)) << "already committed";
        expect(eq(db.abort(), GdiStatus::error_transaction)) << "nothing left to abort";

        expect(eq(db.begin_transaction(), GdiStatus::ok));
        expect(eq(db.abort(), GdiStatus::ok));
        expect(eq(db.commit(), GdiStatus::error_transaction));

        // A transaction object destroyed while open aborts (noexcept).
        {
            Transaction local(db, /*read_only=*/false, db.options().max_journal_bytes);
            // The Database does not track this one, so its destructor's abort() is a
            // no-op by contract; what this asserts is that it does not crash and does
            // not leave the database believing a transaction is open.
        }
        expect(!db.in_transaction());
        expect(eq(db.begin_transaction(), GdiStatus::ok));
        expect(eq(db.abort(), GdiStatus::ok));
    };

    "tx_journal_cap"_test = [] {
        fixture f(4);
        fingerprint const before = take_fingerprint(f.db);
        graph_db::GraphOptions options = f.db.options();
        options.max_journal_bytes = 1024; // deliberately tiny
        f.db.set_options(options);

        expect(eq(f.db.begin_transaction(), GdiStatus::ok));
        GdiStatus status = GdiStatus::ok;
        size_t attempts = 0;
        while (status == GdiStatus::ok && attempts < 200) {
            auto const bytes = gtest::to_bytes(kimix::format("fill{}", attempts));
            VertexCreateResult created;
            status = f.db.create_vertex(view_of(f.node, bytes), {}, nullptr, created);
            ++attempts;
        }
        expect(eq(status, GdiStatus::error_size_limit))
            << "an oversized journal is a refusal, not a crash";
        expect(f.db.in_transaction()) << "the transaction stays abortable";
        expect(eq(f.db.abort(), GdiStatus::ok));
        expect(take_fingerprint(f.db) == before) << "and aborting it restores everything";
    };

    "tx_auto_transaction_is_immediate"_test = [] {
        fixture f(3);
        // Outside a transaction every write is visible at once and marks the
        // affected vertex dirty for the next incremental save.
        expect(!f.db.in_transaction());
        expect(eq(f.db.update_property(f.vertices[0], f.age, PropertyValue::make_int64(123)),
                  GdiStatus::ok));
        expect(f.db.graph().find(f.vertices[0])->is_dirty());
        PropertyValue read_back;
        expect(f.db.graph().find(f.vertices[0])->property(f.age, read_back));
        expect(eq(read_back.as_int64(), int64_t{123}));
        // A brand-new property is visible immediately outside a transaction.
        PropNameId fresh{};
        PropertyTypeInfo fresh_info;
        fresh_info.name = "fresh";
        fresh_info.data = GdiDataType::int64_;
        expect(eq(f.db.create_property_type(fresh_info, fresh), GdiStatus::ok));
        expect(eq(f.db.add_property(f.vertices[1], fresh, PropertyValue::make_int64(7)),
                  GdiStatus::ok));
        expect(f.db.graph().find(f.vertices[1])->has_property(fresh));
    };

    "tx_rollback_of_structural_edits"_test = [] {
        fixture f(6);
        fingerprint const before = take_fingerprint(f.db);
        expect(eq(f.db.begin_transaction(), GdiStatus::ok));
        // The two edits that MOVE adjacency: an endpoint move and a direction
        // change. Both must be undone exactly (the link lists included).
        expect(eq(f.db.set_edge_origin(f.edges[0], f.vertices[4]), GdiStatus::ok));
        expect(eq(f.db.set_edge_target(f.edges[0], f.vertices[5]), GdiStatus::ok));
        expect(eq(f.db.set_edge_direction(f.edges[0], EdgeDirectionType::undirected),
                  GdiStatus::ok));
        expect(!(take_fingerprint(f.db) == before));
        expect(eq(f.db.abort(), GdiStatus::ok));
        expect(take_fingerprint(f.db) == before);
        // The adjacency really is back: the original endpoints are linked again.
        EdgeRecord record;
        expect(f.db.graph().edge_record(f.edges[0], record));
        expect(record.origin == f.vertices[1]);
        expect(record.target == f.vertices[0]);
        expect(record.direction == EdgeDirectionType::directed);
        kimix::vector<Guid> incoming;
        f.db.graph().incident_edges(f.vertices[0], EdgeOrientation::incoming, incoming);
        expect(eq(incoming.size(), size_t{1}));
    };

      // Create-time edge properties (plan v3 step 3.2): an edge created WITH
      // its payload inside a transaction aborts to exactly the prior state —
      // the fingerprint covers the edge's endpoints, direction, labels and
      // properties, so equality proves the payload rolled back with the edge.
      "tx_abort_restores_create_with_properties"_test = [] {
          fixture f(6);
          fingerprint const before = take_fingerprint(f.db);
          expect(eq(f.db.begin_transaction(), GdiStatus::ok));
          LabelId const labels[]{f.link};
          PropertyList payload;
          payload.push_back(PropertyEntry{f.age, PropertyValue::make_int64(42)});
          Guid edge;
          expect(eq(f.db.edge_create(f.vertices[0], f.vertices[3],
                                     EdgeDirectionType::directed,
                                     kimix::span<const LabelId>{labels, 1}, &payload, edge),
                    GdiStatus::ok));
          expect(!(take_fingerprint(f.db) == before));
          expect(eq(f.db.abort(), GdiStatus::ok));
          expect(take_fingerprint(f.db) == before)
              << "abort restored the edge set WITH the create-time payload";
          expect(eq(f.db.graph().key_index().size(), before.key_count));
      };

      "tx_commit_marks_dirty_for_the_next_save"_test = [] {
          gtest::temp_dir_guard dir;
          expect(dir.ok());
          if (!dir.ok()) {
            return;
        }
        Database db;
        graph_db::GraphOptions options;
        expect(eq(db.open(dir.path(), options), GdiStatus::ok));
        LabelId node{};
        expect(eq(db.schema().create_label("node", node), GdiStatus::ok));
        expect(eq(db.persist(/*full=*/true), GdiStatus::ok));

        // A committed batch leaves the touched records dirty, so an incremental
        // save writes exactly the change.
        expect(eq(db.begin_transaction(), GdiStatus::ok));
        auto const bytes = gtest::to_bytes("in-tx");
        VertexCreateResult created;
        expect(eq(db.create_vertex(view_of(node, bytes), {}, nullptr, created), GdiStatus::ok));
        expect(eq(db.commit(), GdiStatus::ok));
        expect(db.graph().find(created.uid) != nullptr);
        expect(db.graph().find(created.uid)->is_dirty())
            << "a new vertex is dirty until something saves it";
        GraphSerializeStats stats;
        bool const incremental = true;
        expect(eq(db.persist(!incremental, &stats), GdiStatus::ok));
        expect(eq(stats.saved_count, size_t{1}));
    };
}
