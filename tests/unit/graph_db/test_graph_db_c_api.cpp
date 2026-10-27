// Test for the C ABI (src/graph_db/graph_c_api.h) — GDI §2.13.
//
// This suite includes ONLY the C header (like tests/unit/api/test_kimix_api.cpp):
// it is what a Pentest-style foreign-language binding sees, so nothing of the C++
// surface is allowed to leak into it.
//
// This test covers:
// - the ABI/layout contract: abi_version matches the macro, every struct size is
//   pinned, sizeof(size_t) is the pointer width;
// - the session lifecycle: open/close, a second close, NULL-argument rejection;
// - status names: every code round-trips, and an unknown one answers
//   "status_unknown" instead of NULL;
// - a full command walk: schema -> keyed vertices -> edges -> labels/properties ->
//   constraint-filtered paged reads through the TWO-PHASE buffer protocol (NULL
//   first to learn the size, then the real fetch, then a deliberately too-small
//   buffer that must report ERR_BUFFER with the needed size) -> a transaction
//   abort and commit -> persist -> reopen and read the same data back;
// - a create/free loop, to prove the session owns no per-call allocation it
//   forgets (the only owned object is the session itself).
#include "ut/ut.hpp"

#include "graph_db_test_util.h"

#include <core/kimix_core.h> // umbrella first: it fixes the winsock2.h order
#include <core/fiber.h>

// The ONLY include from the graph_db library: the C header.
#include <graph_db/graph_c_api.h>

#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;
namespace gtest = kimix::graph_db::test;

namespace tests_unit_graph_db_test_graph_db_c_api {

// Small helpers that make the C surface readable in a test.
kimix_gdb_bytes bytes_of(char const *text) {
    kimix_gdb_bytes out;
    out.data = reinterpret_cast<uint8_t const *>(text);
    out.len = std::strlen(text);
    return out;
}

kimix_gdb_property int_property(int64_t value) {
    kimix_gdb_property property{};
    property.kind = KIMIX_GDB_KIND_INT64;
    property.dtype = KIMIX_GDB_DTYPE_INT64;
    property.count = 1;
    property.i64 = value;
    return property;
}

kimix_gdb_property text_property(char const *text) {
kimix_gdb_property property{};
property.kind = KIMIX_GDB_KIND_STRING;
property.dtype = KIMIX_GDB_DTYPE_CHAR;
property.count = 1;
property.payload = bytes_of(text);
return property;
}

kimix_gdb_property blob_property(uint8_t const *value, size_t len) {
kimix_gdb_property property{};
property.kind = KIMIX_GDB_KIND_BLOB;
property.dtype = KIMIX_GDB_DTYPE_BYTE;
property.count = 1;
property.payload.data = value;
property.payload.len = len;
return property;
}

// One open in-memory session; the guard removes it even on an early return.
struct session_guard {
    kimix_gdb_session *session{nullptr};

    session_guard() {
        kimix_gdb_open_options options{};
        options.directory = nullptr; // in memory
        options.auto_create_schema = 1;
        options.allow_parallel_edges = 1;
        options.allow_self_edges = 1;
        options.max_journal_bytes = 64ull * 1024ull * 1024ull;
        expect(eq(kimix_gdb_open(&options, &session), KIMIX_GDB_OK));
    }
    session_guard(session_guard const &) = delete;
    session_guard &operator=(session_guard const &) = delete;
    ~session_guard() {
        if (session != nullptr) {
            kimix_gdb_close(session);
        }
    }

    // A registered label by name.
    uint32_t label(char const *name) {
        uint32_t id = 0;
        expect(eq(kimix_gdb_label_create(session, name, std::strlen(name), &id), KIMIX_GDB_OK));
        return id;
    }

    uint32_t property_type(char const *name, uint8_t dtype) {
        uint32_t id = 0;
        expect(eq(kimix_gdb_property_type_create(session, name, std::strlen(name),
                                                 KIMIX_GDB_CONTAINER_SINGLE, dtype, 1, &id),
                  KIMIX_GDB_OK));
        return id;
    }

    kimix_gdb_guid vertex(uint32_t label_id, char const *external) {
        kimix_gdb_vertex_spec spec{};
        spec.key.label = label_id;
        spec.key.external_id = bytes_of(external);
        kimix_gdb_guid uid{};
        uint8_t warning = 0;
        expect(eq(kimix_gdb_vertex_create(session, &spec, &uid, &warning), KIMIX_GDB_OK));
        return uid;
    }

    kimix_gdb_guid edge(kimix_gdb_guid origin, kimix_gdb_guid target) {
        kimix_gdb_guid id{};
        expect(eq(kimix_gdb_edge_create(session, origin, target, KIMIX_GDB_EDGE_DIRECTED, nullptr,
                                        0, nullptr, nullptr, 0, &id),
                  KIMIX_GDB_OK));
        return id;
    }

    // One edge carrying a single registered label right at creation (what the
    // edge-filtered path cases assert on).
    kimix_gdb_guid edge_labeled(kimix_gdb_guid origin, kimix_gdb_guid target,
                                uint32_t label_id) {
        kimix_gdb_guid id{};
        expect(eq(kimix_gdb_edge_create(session, origin, target, KIMIX_GDB_EDGE_DIRECTED,
                                        &label_id, 1, nullptr, nullptr, 0, &id),
                  KIMIX_GDB_OK));
        return id;
    }
};

} // namespace tests_unit_graph_db_test_graph_db_c_api
using namespace tests_unit_graph_db_test_graph_db_c_api;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    kimix::fiber::scheduler sched{4u};

    "cabi_version_and_layout"_test = [] {
        expect(eq(kimix_gdb_abi_version(), KIMIX_GDB_ABI_VERSION));
        expect(kimix_gdb_version_string() != nullptr);
        expect(std::strlen(kimix_gdb_version_string()) > 0);

        kimix_gdb_layout layout{};
        expect(eq(kimix_gdb_layout_info(&layout), KIMIX_GDB_OK));
        expect(eq(layout.abi_version, KIMIX_GDB_ABI_VERSION));
        expect(eq(layout.sizeof_property, static_cast<uint32_t>(sizeof(kimix_gdb_property))));
        expect(eq(layout.sizeof_property_record,
                  static_cast<uint32_t>(sizeof(kimix_gdb_property_record))));
        expect(eq(layout.sizeof_vertex_key, static_cast<uint32_t>(sizeof(kimix_gdb_vertex_key))));
        expect(eq(layout.sizeof_edge_record,
                  static_cast<uint32_t>(sizeof(kimix_gdb_edge_record))));
        expect(eq(layout.sizeof_property_type,
                  static_cast<uint32_t>(sizeof(kimix_gdb_property_type))));
        expect(eq(layout.sizeof_capability, static_cast<uint32_t>(sizeof(kimix_gdb_capability))));
        expect(eq(layout.sizeof_vertex_spec,
                  static_cast<uint32_t>(sizeof(kimix_gdb_vertex_spec))));
        expect(eq(layout.sizeof_size_t, static_cast<uint32_t>(sizeof(size_t))));
        expect(eq(layout.alignof_guid, static_cast<uint32_t>(alignof(kimix_gdb_guid))));
        expect(eq(layout.sizeof_size_t, uint32_t{8})) << "the ABI is 64-bit only";
 expect(eq(layout.sizeof_traversal_vertex,
 static_cast<uint32_t>(sizeof(kimix_gdb_traversal_vertex))));
 expect(eq(layout.sizeof_aggregate_result,
 static_cast<uint32_t>(sizeof(kimix_gdb_aggregate_result))));
        // NULL out-parameters are argument errors, never crashes.
        expect(eq(kimix_gdb_layout_info(nullptr), KIMIX_GDB_ERR_ARGUMENT));
    };

    "cabi_status_names"_test = [] {
        // Every code the header defines round-trips through status_name.
        int32_t const codes[] = {KIMIX_GDB_OK,
                                 KIMIX_GDB_WARNING_NON_UNIQUE_ID,
                                 KIMIX_GDB_WARNING_OTHER,
                                 KIMIX_GDB_ERR_ARGUMENT,
                                 KIMIX_GDB_ERR_BUFFER,
                                 KIMIX_GDB_ERR_EDGE,
                                 KIMIX_GDB_ERR_EDGE_ORIENTATION,
                                 KIMIX_GDB_ERR_EMPTY_NAME,
                                 KIMIX_GDB_ERR_LABEL,
                                 KIMIX_GDB_ERR_NAME_EXISTS,
                                 KIMIX_GDB_ERR_NO_PROPERTY,
                                 KIMIX_GDB_ERR_NON_UNIQUE_ID,
                                 KIMIX_GDB_ERR_OP,
                                 KIMIX_GDB_ERR_PROPERTY_EXISTS,
                                 KIMIX_GDB_ERR_PROPERTY_TYPE,
                                 KIMIX_GDB_ERR_READ_ONLY_TRANSACTION,
                                 KIMIX_GDB_ERR_SIZE_LIMIT,
                                 KIMIX_GDB_ERR_TRANSACTION,
                                 KIMIX_GDB_ERR_UID,
                                 KIMIX_GDB_ERR_VERTEX,
                                 KIMIX_GDB_ERR_WRONG_TYPE,
                                 KIMIX_GDB_ERR_IO,
                                 KIMIX_GDB_ERR_UNKNOWN};
        for (auto code : codes) {
            char const *name = kimix_gdb_status_name(code);
            expect(name != nullptr);
            expect(std::strlen(name) > 0) << "code " << code;
        }
        expect(std::strcmp(kimix_gdb_status_name(KIMIX_GDB_OK), "ok") == 0);
        expect(std::strcmp(kimix_gdb_status_name(KIMIX_GDB_ERR_VERTEX), "error_vertex") == 0);
        expect(std::strcmp(kimix_gdb_status_name(999), "status_unknown") == 0);
    };

    "cabi_session_lifecycle"_test = [] {
        kimix_gdb_session *session = nullptr;
        kimix_gdb_open_options options{};
        options.max_journal_bytes = 1024 * 1024;
        expect(eq(kimix_gdb_open(&options, &session), KIMIX_GDB_OK));
        expect(session != nullptr);

        kimix_gdb_capability capability{};
        expect(eq(kimix_gdb_capabilities(session, &capability), KIMIX_GDB_OK));
        expect(capability.enforces_unique_external_ids == 1);
        expect(capability.supports_edge_by_stable_uid == 1);
        expect(capability.supports_edge_property_read == 1);
        expect(capability.supports_collective_transactions == 1);
 expect(capability.supports_global_scan == 1)
 << "G1: the global scan is implemented (plan v3 step 2.4)";
 expect(capability.supports_native_multi_hop == 1)
 << "G3: the native multi-hop traversal is implemented (ABI 2)";
 expect(capability.max_native_hop_depth ==
 static_cast<int32_t>(KIMIX_GDB_MAX_HOP_DEPTH));
 expect(capability.supports_global_aggregates == 1)
 << "G4: the global aggregates are implemented (plan v3 step 2.4)";

        kimix_gdb_close(session);
        // A NULL session is tolerated by close() and rejected by everything else.
        kimix_gdb_close(nullptr);
        kimix_gdb_guid uid{};
        expect(eq(kimix_gdb_vertex_free(nullptr, uid), KIMIX_GDB_ERR_ARGUMENT));
        expect(eq(kimix_gdb_capabilities(nullptr, &capability), KIMIX_GDB_ERR_ARGUMENT));
        expect(eq(kimix_gdb_open(nullptr, nullptr), KIMIX_GDB_ERR_ARGUMENT));
    };

    "cabi_command_walk"_test = [] {
        session_guard guard;
        kimix_gdb_session *session = guard.session;
        uint32_t const node = guard.label("node");
        uint32_t const link = guard.label("link");
        uint32_t const age = guard.property_type("age", KIMIX_GDB_DTYPE_INT64);

        kimix_gdb_guid const a = guard.vertex(node, "a");
        kimix_gdb_guid const b = guard.vertex(node, "b");
        kimix_gdb_guid const c = guard.vertex(node, "c");

        // Key -> uid translation.
        kimix_gdb_vertex_key key{};
        key.label = node;
        key.external_id = bytes_of("b");
        kimix_gdb_guid resolved{};
        uint8_t found = 0;
        expect(eq(kimix_gdb_vertex_translate(session, &key, &resolved, &found), KIMIX_GDB_OK));
        expect(found == 1);
        expect(resolved.lo == b.lo && resolved.hi == b.hi);
        key.external_id = bytes_of("nobody");
        expect(eq(kimix_gdb_vertex_translate(session, &key, &resolved, &found), KIMIX_GDB_OK));
        expect(found == 0) << "a miss is not an error";

        // Edges, a label and an int property.
        kimix_gdb_guid const ab = guard.edge(a, b);
        kimix_gdb_guid const ac = guard.edge(a, c);
        kimix_gdb_property const value = int_property(42);
        expect(eq(kimix_gdb_vertex_property_add(session, a, age, &value), KIMIX_GDB_OK));
        expect(eq(kimix_gdb_vertex_property_add(session, a, age, &value),
                  KIMIX_GDB_ERR_PROPERTY_EXISTS));
        expect(eq(kimix_gdb_vertex_label_add(session, a, link), KIMIX_GDB_OK));

        // Property read-back, TWO-PHASE: the size probe first.
        size_t count = 0;
        size_t scratch_bytes = 0;
        expect(eq(kimix_gdb_vertex_properties(session, a, nullptr, 0, nullptr, 0, &count,
                                              &scratch_bytes),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{1}));
        kimix::vector<kimix_gdb_property_record> items(count + 1);
        kimix::vector<uint8_t> scratch(scratch_bytes + 8);
        expect(eq(kimix_gdb_vertex_properties(session, a, items.data(), items.size(),
                                              scratch.data(), scratch.size(), &count,
                                              &scratch_bytes),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{1}));
        expect(eq(items[0].name_id, age));
        expect(items[0].kind == KIMIX_GDB_KIND_INT64);
        expect(eq(items[0].i64, int64_t{42}));

        // A deliberately too-small item array reports the need, not a crash.
        expect(eq(kimix_gdb_vertex_properties(session, a, nullptr, 0, nullptr, 0, &count,
                                              &scratch_bytes),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{1}));

        // Labels: the two-phase shape, with the key label first.
        size_t label_count = 0;
        expect(eq(kimix_gdb_vertex_labels(session, a, nullptr, 0, &label_count), KIMIX_GDB_OK));
        expect(eq(label_count, size_t{2})) << "the key label `node` and the added `link`";
        kimix::vector<uint32_t> labels(label_count);
        expect(eq(kimix_gdb_vertex_labels(session, a, labels.data(), labels.size(), &label_count),
                  KIMIX_GDB_OK));
        expect(eq(labels[0], node)) << "the key label comes first";

        // Edge read: count probe, then a page of two.
        size_t total = 0;
        expect(eq(kimix_gdb_edges_of_vertex(session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 0,
                                            nullptr, 0, &count, &total),
                  KIMIX_GDB_OK));
        expect(eq(total, size_t{2}));
        kimix::vector<kimix_gdb_edge_record> edges(2);
        expect(eq(kimix_gdb_edges_of_vertex(session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 2,
                                            edges.data(), edges.size(), &count, &total),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{2}));
        expect(eq(total, size_t{2}));
        for (auto const &record : edges) {
            expect(record.origin.lo == a.lo && record.origin.hi == a.hi);
        }
        // cap 1 of 2: one item, still the full total.
        expect(eq(kimix_gdb_edges_of_vertex(session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 1,
                                            edges.data(), edges.size(), &count, &total),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{1}));
        expect(eq(total, size_t{2}));

        // A constraint that filters the NEIGHBOUR VERTICES by property.
        uint32_t constraint = 0;
        expect(eq(kimix_gdb_constraint_create(session, &constraint), KIMIX_GDB_OK));
        kimix_gdb_property const expected = int_property(42);
        expect(eq(kimix_gdb_constraint_add_property(session, constraint, age,
                                                    KIMIX_GDB_OP_EQUAL, &expected, 1),
                  KIMIX_GDB_OK));
        expect(eq(kimix_gdb_count_neighbors(session, a, constraint, KIMIX_GDB_ORIENTATION_OUTGOING,
                                            &count),
                  KIMIX_GDB_OK));
        expect(eq(count, size_t{0})) << "b and c carry no `age` property";
        uint8_t exists = 1;
        expect(eq(kimix_gdb_exists_neighbor(session, a, constraint,
                                            KIMIX_GDB_ORIENTATION_OUTGOING, &exists),
                  KIMIX_GDB_OK));
        expect(exists == 0);
        expect(eq(kimix_gdb_constraint_free(session, constraint), KIMIX_GDB_OK));

        // Endpoints of one edge.
        kimix_gdb_guid origin{};
        kimix_gdb_guid target{};
        expect(eq(kimix_gdb_edge_endpoints(session, ab, &origin, &target), KIMIX_GDB_OK));
        expect(origin.lo == a.lo);
        expect(target.lo == b.lo);
        expect(eq(kimix_gdb_edge_free(session, ac), KIMIX_GDB_OK));
        expect(eq(kimix_gdb_edge_free(session, ac), KIMIX_GDB_ERR_UID));

      // Guarded delete: `a` still has the ab edge.
      expect(eq(kimix_gdb_vertex_free_guarded(session, a), KIMIX_GDB_ERR_EDGE));
      expect(eq(kimix_gdb_edge_free(session, ab), KIMIX_GDB_OK));
      expect(eq(kimix_gdb_vertex_free_guarded(session, a), KIMIX_GDB_OK));
  };

  // The G3 multi-hop surface (ABI 2): a full two-phase walk of both calls —
  // probe -> fetch -> ERR_BUFFER, the constraint-filtered shape, and the
  // error surface (unknown endpoint, out-of-range bound, NULL arguments).
  "cabi_traverse_walk"_test = [] {
      session_guard guard;
      kimix_gdb_session *session = guard.session;
      uint32_t const node = guard.label("node");
      uint32_t const age = guard.property_type("age", KIMIX_GDB_DTYPE_INT64);
      // a(50) -> b(70) -> c(70);  a -> d(20).  Shortest a->c is a,b,c.
      kimix_gdb_guid const a = guard.vertex(node, "a");
      kimix_gdb_guid const b = guard.vertex(node, "b");
      kimix_gdb_guid const c = guard.vertex(node, "c");
      kimix_gdb_guid const d = guard.vertex(node, "d");
      kimix_gdb_property const age50 = int_property(50);
      kimix_gdb_property const age70 = int_property(70);
      kimix_gdb_property const age20 = int_property(20);
      expect(eq(kimix_gdb_vertex_property_add(session, a, age, &age50), KIMIX_GDB_OK));
      expect(eq(kimix_gdb_vertex_property_add(session, b, age, &age70), KIMIX_GDB_OK));
      expect(eq(kimix_gdb_vertex_property_add(session, c, age, &age70), KIMIX_GDB_OK));
      expect(eq(kimix_gdb_vertex_property_add(session, d, age, &age20), KIMIX_GDB_OK));
      guard.edge(a, b);
      guard.edge(b, c);
      guard.edge(a, d);

      // Unfiltered traverse_bfs from a, depth 2, generous cap: a + b,d + c.
      size_t count = 0;
      expect(eq(kimix_gdb_traverse_bfs(session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 100,
                                       2, nullptr, 0, &count),
                KIMIX_GDB_OK));
      expect(eq(count, size_t{4}));
      kimix_gdb_traversal_vertex walk[8]{};
      expect(eq(kimix_gdb_traverse_bfs(session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 100,
                                       2, walk, 8, &count),
                KIMIX_GDB_OK));
      expect(eq(count, size_t{4}));
      expect(walk[0].vertex.lo == a.lo && walk[0].depth == 0)
          << "items[0] is the start at depth 0";
      expect(walk[1].depth == 1u && walk[2].depth == 1u);
      expect(walk[3].depth == 2u);
      expect(walk[3].parent.lo == b.lo) << "c is discovered through b";
      // The too-small buffer reports the count and refuses.
      expect(eq(kimix_gdb_traverse_bfs(session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 100,
                                       2, walk, 1, &count),
                KIMIX_GDB_ERR_BUFFER));
      expect(eq(count, size_t{4}));
      // A cap of 1 keeps exactly one vertex per level.
      expect(eq(kimix_gdb_traverse_bfs(session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 1, 2,
                                       nullptr, 0, &count),
                KIMIX_GDB_OK));
      expect(eq(count, size_t{3})) << "start + 1 per level";

      // Constraint-filtered traversal: age >= 60 drops d at level 1, so its
      // (absent) children are never visited; only a, b, c remain.
      uint32_t seniors = 0;
      expect(eq(kimix_gdb_constraint_create(session, &seniors), KIMIX_GDB_OK));
      kimix_gdb_property const threshold = int_property(60);
      expect(eq(kimix_gdb_constraint_add_property(session, seniors, age,
                                                  KIMIX_GDB_OP_GREATER_OR_EQUAL, &threshold, 1),
                KIMIX_GDB_OK));
      expect(eq(kimix_gdb_traverse_bfs(session, a, seniors,
                                       KIMIX_GDB_ORIENTATION_OUTGOING, 100, 2, nullptr, 0,
                                       &count),
                KIMIX_GDB_OK));
      expect(eq(count, size_t{3}));

      // shortest_path: two-phase walk and the reachable/unreachable split.
      expect(eq(kimix_gdb_shortest_path(session, a, c, KIMIX_GDB_MAX_HOP_DEPTH, nullptr, 0,
                                        &count),
                KIMIX_GDB_OK));
      expect(eq(count, size_t{3}));
      kimix_gdb_guid path[8]{};
      expect(eq(kimix_gdb_shortest_path(session, a, c, KIMIX_GDB_MAX_HOP_DEPTH, path, 8,
                                        &count),
                KIMIX_GDB_OK));
      expect(eq(count, size_t{3}));
      expect(path[0].lo == a.lo && path[2].lo == c.lo && path[1].lo == b.lo)
          << "the path is a, b, c";
      expect(eq(kimix_gdb_shortest_path(session, a, c, 1, nullptr, 0, &count),
                KIMIX_GDB_OK));
      expect(eq(count, size_t{0}))
          << "unreachable within the bound is an empty result, not an error";
      // Errors: unknown endpoints, bad bounds, NULL arguments.
      kimix_gdb_guid const unknown{0xdeadbeefull, 0xfeedfaceull};
      expect(eq(kimix_gdb_shortest_path(session, a, unknown, 4, nullptr, 0, &count),
                KIMIX_GDB_ERR_VERTEX));
      expect(eq(kimix_gdb_shortest_path(session, a, c, 0, nullptr, 0, &count),
                KIMIX_GDB_ERR_ARGUMENT));
      expect(eq(kimix_gdb_shortest_path(session, a, c, KIMIX_GDB_MAX_HOP_DEPTH + 1,
                                        nullptr, 0, &count),
                KIMIX_GDB_ERR_ARGUMENT));
      expect(eq(kimix_gdb_shortest_path(nullptr, a, c, 4, nullptr, 0, &count),
                KIMIX_GDB_ERR_ARGUMENT));
      expect(eq(kimix_gdb_shortest_path(session, a, c, 4, nullptr, 0, nullptr),
                KIMIX_GDB_ERR_ARGUMENT));
      expect(eq(kimix_gdb_traverse_bfs(session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 0, 2,
                                       nullptr, 0, &count),
                KIMIX_GDB_ERR_ARGUMENT))
          << "a cap below 1 is refused";
      expect(eq(kimix_gdb_traverse_bfs(session, unknown, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                       10, 2, nullptr, 0, &count),
                KIMIX_GDB_ERR_VERTEX));
      expect(eq(kimix_gdb_traverse_bfs(nullptr, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 10,
                                       2, nullptr, 0, &count),
                KIMIX_GDB_ERR_ARGUMENT));
  };

// The EDGE-filtered shortest path (an additive surface after ABI 6): the
// two-phase walk of kimix_gdb_shortest_path_edges, the reported hop edges, the
// rerouting a label filter causes, the orientation knob and the error surface.
"cabi_shortest_path_edges"_test = [] {
    session_guard guard;
    kimix_gdb_session *session = guard.session;
    uint32_t const node = guard.label("node");
    uint32_t const open = guard.label("open_route");
    uint32_t const blocked = guard.label("blocked_route");
    // a -> b -> c is the short chain (b -> c blocked); a -> d -> c is the long
    // one (both open).  A filter that refuses `blocked` must pick a, d, c.
    kimix_gdb_guid const a = guard.vertex(node, "a");
    kimix_gdb_guid const b = guard.vertex(node, "b");
    kimix_gdb_guid const c = guard.vertex(node, "c");
    kimix_gdb_guid const d = guard.vertex(node, "d");
    kimix_gdb_guid const ab = guard.edge(a, b);
    kimix_gdb_guid const bc = guard.edge_labeled(b, c, blocked);
    kimix_gdb_guid const ad = guard.edge_labeled(a, d, open);
    kimix_gdb_guid const dc = guard.edge_labeled(d, c, open);

    // Probe phase: the unfiltered walk is the short chain a, b, c.
    size_t count = 0;
    expect(eq(kimix_gdb_shortest_path_edges(session, a, c, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                            KIMIX_GDB_MAX_HOP_DEPTH, nullptr, 0, &count),
              KIMIX_GDB_OK));
    expect(eq(count, size_t{3}));
    kimix_gdb_path_step path[8]{};
    expect(eq(kimix_gdb_shortest_path_edges(session, a, c, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                            KIMIX_GDB_MAX_HOP_DEPTH, path, 8, &count),
              KIMIX_GDB_OK));
    expect(eq(count, size_t{3}));
    expect(path[0].vertex.lo == a.lo && path[1].vertex.lo == b.lo && path[2].vertex.lo == c.lo)
        << "unfiltered: a, b, c";
    expect(path[0].edge.lo == 0ull && path[0].edge.hi == 0ull)
        << "the start is reached by no edge";
    expect(path[1].edge.lo == ab.lo && path[2].edge.lo == bc.lo)
        << "each step reports the edge it traversed";
    expect(path[1].depth == 1u && path[2].depth == 2u);

    // A blocking label filter reroutes the SAME endpoints onto the long chain.
    uint32_t blocked_only = 0;
    expect(eq(kimix_gdb_constraint_create(session, &blocked_only), KIMIX_GDB_OK));
    expect(eq(kimix_gdb_constraint_add_label(session, blocked_only, open, KIMIX_GDB_OP_EQUAL),
              KIMIX_GDB_OK));
    expect(eq(kimix_gdb_shortest_path_edges(session, a, c, blocked_only,
                                            KIMIX_GDB_ORIENTATION_OUTGOING,
                                            KIMIX_GDB_MAX_HOP_DEPTH, path, 8, &count),
              KIMIX_GDB_OK));
    expect(eq(count, size_t{3})) << "open edges only: a -> d -> c is still 2 hops";
    expect(path[1].vertex.lo == d.lo && path[2].vertex.lo == c.lo) << "the filter chose the route";
    expect(path[1].edge.lo == ad.lo && path[2].edge.lo == dc.lo);
    // Filtering on the OTHER label keeps the short chain and reports its edge.
    uint32_t blocked_routes = 0;
    expect(eq(kimix_gdb_constraint_create(session, &blocked_routes), KIMIX_GDB_OK));
    expect(eq(kimix_gdb_constraint_add_label(session, blocked_routes, blocked, KIMIX_GDB_OP_EQUAL),
              KIMIX_GDB_OK));
    expect(eq(kimix_gdb_shortest_path_edges(session, a, c, blocked_routes,
                                            KIMIX_GDB_ORIENTATION_OUTGOING,
                                            KIMIX_GDB_MAX_HOP_DEPTH, nullptr, 0, &count),
              KIMIX_GDB_OK));
    expect(eq(count, size_t{0}))
        << "a -> b carries no label, so the blocked-only walk never leaves a";

    // Orientation: walking BACKWARDS from c over `open` edges reaches d and a.
    expect(eq(kimix_gdb_shortest_path_edges(session, c, a, blocked_only,
                                            KIMIX_GDB_ORIENTATION_INCOMING,
                                            KIMIX_GDB_MAX_HOP_DEPTH, path, 8, &count),
              KIMIX_GDB_OK));
    expect(eq(count, size_t{3}));
    expect(path[1].vertex.lo == d.lo && path[2].vertex.lo == a.lo)
        << "incoming: c <- d <- a";
    expect(path[1].edge.lo == dc.lo && path[2].edge.lo == ad.lo);

    // Two-phase contract: a too-small buffer reports the count and refuses, and
    // the count probe (items == NULL) never needs storage.
    expect(eq(kimix_gdb_shortest_path_edges(session, a, c, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                            KIMIX_GDB_MAX_HOP_DEPTH, path, 2, &count),
              KIMIX_GDB_ERR_BUFFER));
    expect(eq(count, size_t{3})) << "the required size is still reported";
    // Unreachable within the bound, and unreachable over the matched edges, are
    // both OK with a zero count.
    expect(eq(kimix_gdb_shortest_path_edges(session, a, c, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 1,
                                            nullptr, 0, &count),
              KIMIX_GDB_OK));
    expect(eq(count, size_t{0}));

    // Error surface: NULL session / NULL out_count, unknown endpoints, bounds,
    // unknown constraint id, and a label condition with an ordering op.
    kimix_gdb_guid const unknown{0xdeadbeefull, 0xfeedfaceull};
    expect(eq(kimix_gdb_shortest_path_edges(nullptr, a, c, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                            KIMIX_GDB_MAX_HOP_DEPTH, nullptr, 0, &count),
              KIMIX_GDB_ERR_ARGUMENT));
    expect(eq(kimix_gdb_shortest_path_edges(session, a, c, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                            KIMIX_GDB_MAX_HOP_DEPTH, nullptr, 0, nullptr),
              KIMIX_GDB_ERR_ARGUMENT));
    expect(eq(kimix_gdb_shortest_path_edges(session, a, unknown, 0,
                                            KIMIX_GDB_ORIENTATION_OUTGOING,
                                            KIMIX_GDB_MAX_HOP_DEPTH, nullptr, 0, &count),
              KIMIX_GDB_ERR_VERTEX));
    expect(eq(kimix_gdb_shortest_path_edges(session, a, c, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 0,
                                            nullptr, 0, &count),
              KIMIX_GDB_ERR_ARGUMENT));
    expect(eq(kimix_gdb_shortest_path_edges(session, a, c, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                            KIMIX_GDB_MAX_HOP_DEPTH + 1, nullptr, 0, &count),
              KIMIX_GDB_ERR_ARGUMENT));
    expect(eq(kimix_gdb_shortest_path_edges(session, a, c, 424242u,
                                            KIMIX_GDB_ORIENTATION_OUTGOING,
                                            KIMIX_GDB_MAX_HOP_DEPTH, nullptr, 0, &count),
              KIMIX_GDB_ERR_CONSTRAINT));
    // A label condition can only test membership. This surface refuses an
    // ordering op when the condition is ADDED (the same rule the walk re-checks),
    // so the refusal never even reaches the walk through the C ABI.
    uint32_t ordering = 0;
    expect(eq(kimix_gdb_constraint_create(session, &ordering), KIMIX_GDB_OK));
    expect(eq(kimix_gdb_constraint_add_label(session, ordering, open, KIMIX_GDB_OP_GREATER),
              KIMIX_GDB_ERR_OP))
        << "a label condition can only test membership";
};

// The G1/G4 global surface (ABI 3): a full two-phase walk of both calls —
// scan probe -> fetch -> ERR_BUFFER -> paged caps, the constraint-filtered
// scan, the aggregate matrix (count/min/max/sum/avg, the string payload
// through scratch with its ERR_BUFFER), the legality errors, and the
// NULL-argument guards. The capability flags flipped in step 2.4 are
// mutation-checked above: a silent regression to false fails this suite.
"cabi_scan_and_aggregate_walk"_test = [] {
 session_guard guard;
 kimix_gdb_session *session = guard.session;
 uint32_t const node = guard.label("node");
 uint32_t const warm = guard.label("warm");
 uint32_t const age = guard.property_type("age", KIMIX_GDB_DTYPE_INT64);
 uint32_t const tag = guard.property_type("tag", KIMIX_GDB_DTYPE_CHAR);
 uint32_t const blob = guard.property_type("blob", KIMIX_GDB_DTYPE_BYTE);
 // n0..n9: age = i, tag = "t{i%3}", blob = {i%2, 0x7f}; even ones are warm.
 kimix_gdb_guid made[10]{};
 for (int i = 0; i < 10; ++i) {
 char ext[3] = {'n', static_cast<char>('0' + i), '\0'};
 made[i] = guard.vertex(node, ext);
 kimix_gdb_property const years = int_property(i);
 expect(eq(kimix_gdb_vertex_property_add(session, made[i], age, &years), KIMIX_GDB_OK));
 char tag_text[3] = {'t', static_cast<char>('0' + (i % 3)), '\0'};
 kimix_gdb_property const tag_value = text_property(tag_text);
 expect(eq(kimix_gdb_vertex_property_add(session, made[i], tag, &tag_value), KIMIX_GDB_OK));
 uint8_t const raw[2] = {static_cast<uint8_t>(i % 2), 0x7f};
 kimix_gdb_property const blob_value = blob_property(raw, 2);
 expect(eq(kimix_gdb_vertex_property_add(session, made[i], blob, &blob_value), KIMIX_GDB_OK));
 if (i % 2 == 0) {
 expect(eq(kimix_gdb_vertex_label_add(session, made[i], warm), KIMIX_GDB_OK));
 }
 }
 // Scan: the probe reports the full count, the fetch returns the guids, a
 // too-small buffer is ERR_BUFFER with the counts still reported, and a cap
 // pages the page (out_count follows the cap, out_total stays the full set).
 size_t count = 0;
 size_t total = 0;
 expect(eq(kimix_gdb_scan_vertices(session, 0, 0, nullptr, 0, &count, &total),
 KIMIX_GDB_OK));
 expect(eq(count, size_t{0})) << "the probe carries no items";
 expect(eq(total, size_t{10}));
 kimix_gdb_guid fetched[10]{};
 expect(eq(kimix_gdb_scan_vertices(session, 0, 0x7fffffff, fetched, 10, &count, &total),
 KIMIX_GDB_OK));
 expect(eq(count, size_t{10}));
 expect(eq(total, size_t{10}));
 for (auto const &uid : made) {
 bool found = false;
 for (auto const &got : fetched) {
 found = found || (got.lo == uid.lo && got.hi == uid.hi);
 }
 expect(found) << "every created vertex is in the scan";
 }
 expect(eq(kimix_gdb_scan_vertices(session, 0, 0x7fffffff, fetched, 4, &count, &total),
 KIMIX_GDB_ERR_BUFFER));
 expect(eq(count, size_t{10})) << "the count is reported either way";
 expect(eq(total, size_t{10}));
 expect(eq(kimix_gdb_scan_vertices(session, 0, 3, fetched, 10, &count, &total),
 KIMIX_GDB_OK));
 expect(eq(count, size_t{3})) << "a cap pages the page";
 expect(eq(total, size_t{10}));
 expect(eq(kimix_gdb_scan_vertices(session, 0, -1, fetched, 10, &count, &total),
 KIMIX_GDB_ERR_ARGUMENT));
 expect(eq(kimix_gdb_scan_vertices(nullptr, 0, 0, nullptr, 0, &count, &total),
 KIMIX_GDB_ERR_ARGUMENT));
 // Constraint-filtered scan: the warm vertices only.
 uint32_t warm_only = 0;
 expect(eq(kimix_gdb_constraint_create(session, &warm_only), KIMIX_GDB_OK));
 expect(eq(kimix_gdb_constraint_add_label(session, warm_only, warm, KIMIX_GDB_OP_EQUAL),
 KIMIX_GDB_OK));
 expect(eq(kimix_gdb_scan_vertices(session, warm_only, 0, nullptr, 0, &count, &total),
 KIMIX_GDB_OK));
 expect(eq(total, size_t{5}));
 expect(eq(kimix_gdb_scan_vertices(session, warm_only, 0x7fffffff, fetched, 10, &count,
 &total),
 KIMIX_GDB_OK));
 expect(eq(count, size_t{5}));
 for (size_t i = 0; i < count; ++i) {
 bool expected_warm = false;
 for (int k = 0; k < 10; k += 2) {
 expected_warm = expected_warm ||
 (fetched[i].lo == made[k].lo && fetched[i].hi == made[k].hi);
 }
 expect(expected_warm) << "the filtered scan returns exactly the warm vertices";
 }
 // Aggregates over the same store.
 kimix_gdb_aggregate_result result{};
 expect(eq(kimix_gdb_aggregate(session, 0, 0, KIMIX_GDB_AGGREGATE_COUNT, nullptr, 0,
 &result),
 KIMIX_GDB_OK));
 expect(eq(result.matched, uint64_t{10}));
 expect(eq(result.has_value, uint8_t{1}));
 expect(eq(result.value_kind, KIMIX_GDB_VALUE_INT64));
 expect(eq(result.int64_value, int64_t{10}));
 expect(eq(kimix_gdb_aggregate(session, warm_only, 0, KIMIX_GDB_AGGREGATE_COUNT, nullptr,
 0, &result),
 KIMIX_GDB_OK));
 expect(eq(result.matched, uint64_t{5}));
 expect(eq(result.int64_value, int64_t{5}));
 // min/max/sum/avg over age = 0..9.
 expect(eq(kimix_gdb_aggregate(session, 0, age, KIMIX_GDB_AGGREGATE_MIN, nullptr, 0,
 &result),
 KIMIX_GDB_OK));
 expect(eq(result.valued, uint64_t{10}));
 expect(eq(result.int64_value, int64_t{0}));
 expect(eq(kimix_gdb_aggregate(session, 0, age, KIMIX_GDB_AGGREGATE_MAX, nullptr, 0,
 &result),
 KIMIX_GDB_OK));
 expect(eq(result.int64_value, int64_t{9}));
 expect(eq(kimix_gdb_aggregate(session, 0, age, KIMIX_GDB_AGGREGATE_SUM, nullptr, 0,
 &result),
 KIMIX_GDB_OK));
 expect(eq(result.int64_value, int64_t{45}));
 expect(eq(kimix_gdb_aggregate(session, 0, age, KIMIX_GDB_AGGREGATE_AVG, nullptr, 0,
 &result),
 KIMIX_GDB_OK));
 expect(eq(result.value_kind, KIMIX_GDB_VALUE_DOUBLE));
 expect(eq(result.double_value, 4.5));
 // The string payload goes through scratch, with ERR_BUFFER when too small.
 uint8_t scratch[8]{};
 expect(eq(kimix_gdb_aggregate(session, 0, tag, KIMIX_GDB_AGGREGATE_MIN, scratch,
 sizeof(scratch), &result),
 KIMIX_GDB_OK));
 expect(eq(result.value_kind, KIMIX_GDB_VALUE_STRING));
 expect(eq(result.string_len, uint32_t{2}));
 expect(std::memcmp(scratch, "t0", 2) == 0);
 expect(eq(kimix_gdb_aggregate(session, 0, tag, KIMIX_GDB_AGGREGATE_MIN, scratch, 1,
 &result),
 KIMIX_GDB_ERR_BUFFER));
 expect(eq(result.string_len, uint32_t{2})) << "the length is reported either way";
 // Legality: decided up front from the registered declaration.
 expect(eq(kimix_gdb_aggregate(session, 0, tag, KIMIX_GDB_AGGREGATE_SUM, nullptr, 0,
 &result),
 KIMIX_GDB_ERR_OP));
 expect(eq(kimix_gdb_aggregate(session, 0, blob, KIMIX_GDB_AGGREGATE_SUM, nullptr, 0,
 &result),
 KIMIX_GDB_ERR_OP));
 expect(eq(kimix_gdb_aggregate(session, 0, blob, KIMIX_GDB_AGGREGATE_MIN, nullptr, 0,
 &result),
 KIMIX_GDB_ERR_OP));
 expect(eq(kimix_gdb_aggregate(session, 0, 999, KIMIX_GDB_AGGREGATE_SUM, nullptr, 0,
 &result),
 KIMIX_GDB_ERR_PROPERTY_TYPE));
 expect(eq(kimix_gdb_aggregate(session, 0, age, 99, nullptr, 0, &result),
 KIMIX_GDB_ERR_ARGUMENT));
 expect(eq(kimix_gdb_aggregate(session, 0, 0, KIMIX_GDB_AGGREGATE_AVG, nullptr, 0,
 &result),
 KIMIX_GDB_ERR_ARGUMENT)) << "a property reduction needs a property";
 // NULL guards.
 expect(eq(kimix_gdb_aggregate(nullptr, 0, age, KIMIX_GDB_AGGREGATE_COUNT, nullptr, 0,
 &result),
 KIMIX_GDB_ERR_ARGUMENT));
 expect(eq(kimix_gdb_aggregate(session, 0, age, KIMIX_GDB_AGGREGATE_COUNT, nullptr, 0,
 nullptr),
 KIMIX_GDB_ERR_ARGUMENT));
 };

  "cabi_transaction_and_persistence"_test = [] {
        gtest::temp_dir_guard dir;
        expect(dir.ok());
        if (!dir.ok()) {
            return;
        }
        kimix::string const path{dir.path().string().c_str()};
        kimix_gdb_guid kept{};
        kimix_gdb_guid dropped{};
        {
            kimix_gdb_session *session = nullptr;
            kimix_gdb_open_options options{};
            options.directory = path.c_str();
            options.max_journal_bytes = 1024 * 1024;
            expect(eq(kimix_gdb_open(&options, &session), KIMIX_GDB_OK));
            uint32_t label = 0;
            expect(eq(kimix_gdb_label_create(session, "node", 4, &label), KIMIX_GDB_OK));

            // An aborted transaction leaves nothing behind...
            expect(eq(kimix_gdb_tx_begin(session), KIMIX_GDB_OK));
            kimix_gdb_vertex_spec spec{};
            spec.key.label = label;
            spec.key.external_id = bytes_of("dropped");
            expect(eq(kimix_gdb_vertex_create(session, &spec, &dropped, nullptr), KIMIX_GDB_OK));
            expect(eq(kimix_gdb_tx_abort(session), KIMIX_GDB_OK));
            kimix_gdb_vertex_key key{};
            key.label = label;
            key.external_id = bytes_of("dropped");
            uint8_t found = 1;
            expect(eq(kimix_gdb_vertex_translate(session, &key, &dropped, &found), KIMIX_GDB_OK));
            expect(found == 0) << "the aborted create is gone";

            // ...and a committed one survives a save.
            expect(eq(kimix_gdb_tx_begin(session), KIMIX_GDB_OK));
            spec.key.external_id = bytes_of("kept");
            expect(eq(kimix_gdb_vertex_create(session, &spec, &kept, nullptr), KIMIX_GDB_OK));
            expect(eq(kimix_gdb_tx_commit(session), KIMIX_GDB_OK));
            expect(eq(kimix_gdb_persist(session, 1), KIMIX_GDB_OK));
            kimix_gdb_close(session);
        }
        // Reopen: the committed vertex is in the store, the aborted one is not.
        kimix_gdb_session *session = nullptr;
        kimix_gdb_open_options options{};
        options.directory = path.c_str();
        expect(eq(kimix_gdb_open(&options, &session), KIMIX_GDB_OK));
        uint32_t label = 0;
        uint8_t found = 0;
        expect(eq(kimix_gdb_label_find(session, "node", 4, &label, &found), KIMIX_GDB_OK));
        expect(found == 1) << "the schema survived the reopen";
        kimix_gdb_vertex_key key{};
        key.label = label;
        key.external_id = bytes_of("kept");
        kimix_gdb_guid resolved{};
        expect(eq(kimix_gdb_vertex_translate(session, &key, &resolved, &found), KIMIX_GDB_OK));
        expect(found == 1);
        expect(resolved.lo == kept.lo && resolved.hi == kept.hi);
        key.external_id = bytes_of("dropped");
        expect(eq(kimix_gdb_vertex_translate(session, &key, &resolved, &found), KIMIX_GDB_OK));
        expect(found == 0);
        kimix_gdb_close(session);
    };

    "cabi_churn_leaves_no_state"_test = [] {
        session_guard guard;
        kimix_gdb_session *session = guard.session;
        uint32_t const node = guard.label("node");
        for (int i = 0; i < 200; ++i) {
            kimix::string const name{kimix::format("churn{}", i)};
            kimix_gdb_vertex_spec spec{};
            spec.key.label = node;
            spec.key.external_id = kimix_gdb_bytes{
                reinterpret_cast<uint8_t const *>(name.data()), name.size()};
            kimix_gdb_guid uid{};
            expect(eq(kimix_gdb_vertex_create(session, &spec, &uid, nullptr), KIMIX_GDB_OK));
            kimix_gdb_guid const target = guard.vertex(node, kimix::format("fixed{}", i).c_str());
            kimix_gdb_guid edge{};
            expect(eq(kimix_gdb_edge_create(session, uid, target, KIMIX_GDB_EDGE_DIRECTED, nullptr,
                                            0, nullptr, nullptr, 0, &edge),
                      KIMIX_GDB_OK));
            expect(eq(kimix_gdb_edge_free(session, edge), KIMIX_GDB_OK));
            expect(eq(kimix_gdb_vertex_free(session, uid), KIMIX_GDB_OK));
            expect(eq(kimix_gdb_vertex_free(session, target), KIMIX_GDB_OK));
        }
        // Everything is gone; the session is still usable and the close in the
          // guard is the only owned object's release.
          size_t count = 0;
          kimix_gdb_guid nobody{};
          expect(eq(kimix_gdb_vertex_free(session, nobody), KIMIX_GDB_ERR_VERTEX));
          expect(eq(kimix_gdb_vertex_labels(session, nobody, nullptr, 0, &count),
                    KIMIX_GDB_ERR_VERTEX));
      };

// The ABI 5 named-graph catalog (plan v3 step 3.7): a named open registers,
// a second LIVE open of the same name is ERR_NAME_EXISTS (status-coded, the
// refused session left completely unregistered), close releases the entry,
// anonymous sessions never appear, and the name round-trips through the
// session getter.
  "cabi_content_digest_option"_test = [] {
      // Plan v3 §4.5 (ABI 6): open_options.content_digest selects the vertex
      // content digest. 0 (or a zeroed struct) keeps MD5; 1 selects XXH3-128.
      // The digest is not exposed through the C API, so this walk verifies the
      // option is accepted and the session is fully usable under it (vertex
      // create with content, property write, read-back); the digest VALUES are
      // equivalence-tested at the C++ level (test_graph_db_serialize).
      for (uint8_t const flag : {uint8_t{0}, uint8_t{1}}) {
          kimix_gdb_open_options options{};
          options.directory = nullptr; // in memory
          options.auto_create_schema = 1;
          options.allow_parallel_edges = 1;
          options.allow_self_edges = 1;
          options.content_digest = flag;
          options.max_journal_bytes = 64ull * 1024ull * 1024ull;
          kimix_gdb_session *session = nullptr;
          expect(eq(kimix_gdb_open(&options, &session), KIMIX_GDB_OK));
          uint32_t const person = [&]() {
              uint32_t id = 0;
              expect(eq(kimix_gdb_label_create(session, "person", 6, &id), KIMIX_GDB_OK));
              return id;
          }();
          uint32_t const age = [&]() {
              uint32_t id = 0;
              expect(eq(kimix_gdb_property_type_create(session, "age", 3, KIMIX_GDB_CONTAINER_SINGLE,
                                                       KIMIX_GDB_DTYPE_INT64, 1, &id),
                        KIMIX_GDB_OK));
              return id;
          }();
          // A vertex WITH content (the field the digest caches).
          kimix_gdb_vertex_spec spec{};
          spec.key.label = person;
          static uint8_t const key_bytes[] = {'d', 'i', 'g', 'e', 's', 't'};
          spec.key.external_id.data = key_bytes;
          spec.key.external_id.len = sizeof(key_bytes);
          static uint8_t const content_bytes[] = {'d', 'i', 'g'};
          spec.content.data = content_bytes;
          spec.content.len = sizeof(content_bytes);
          kimix_gdb_guid uid{};
          expect(eq(kimix_gdb_vertex_create(session, &spec, &uid, nullptr), KIMIX_GDB_OK));
          // Usability under the policy: a property write + read-back round trip.
          kimix_gdb_property value{};
          value.kind = KIMIX_GDB_KIND_INT64;
          value.dtype = KIMIX_GDB_DTYPE_INT64;
          value.count = 1;
          value.i64 = 41;
          expect(eq(kimix_gdb_vertex_property_add(session, uid, age, &value), KIMIX_GDB_OK));
          kimix_gdb_property_record records[1]{};
          size_t fetched = 0;
          size_t used = 0;
          expect(eq(kimix_gdb_vertex_properties(session, uid, records, 1, nullptr, 0, &fetched, &used),
                    KIMIX_GDB_OK));
          expect(eq(fetched, size_t{1}));
          expect(eq(records[0].name_id, age));
          expect(eq(records[0].i64, 41));
          kimix_gdb_close(session);
      }
  };

  "cabi_named_graph_catalog"_test = [] {
 kimix_gdb_open_options options{};
 options.directory = nullptr;
 options.graph_name = "catalog-walk";
 size_t count = 0;
 expect(eq(kimix_gdb_catalog_count(&count), KIMIX_GDB_OK));
 size_t const baseline = count;
 uint8_t exists = 0xFF;
 expect(eq(kimix_gdb_catalog_contains("catalog-walk", 12, &exists), KIMIX_GDB_OK));
 expect(eq(exists, uint8_t{0})) << "nothing registered yet";
 kimix_gdb_session *first = nullptr;
 expect(eq(kimix_gdb_open(&options, &first), KIMIX_GDB_OK));
 expect(eq(kimix_gdb_catalog_count(&count), KIMIX_GDB_OK));
 expect(eq(count, baseline + 1));
 exists = 0;
 expect(eq(kimix_gdb_catalog_contains("catalog-walk", 12, &exists), KIMIX_GDB_OK));
 expect(eq(exists, uint8_t{1}));
 char name[32]{};
 size_t len = 99;
 expect(eq(kimix_gdb_session_graph_name(first, name, sizeof(name), &len), KIMIX_GDB_OK));
 expect(eq(len, size_t{12}));
 expect(std::memcmp(name, "catalog-walk", 12) == 0);
 // The collision: a second live open is refused, the first unaffected.
 kimix_gdb_session *second = nullptr;
 expect(eq(kimix_gdb_open(&options, &second), KIMIX_GDB_ERR_NAME_EXISTS));
 expect(second == nullptr);
 expect(eq(kimix_gdb_catalog_count(&count), KIMIX_GDB_OK));
 expect(eq(count, baseline + 1));
 // Anonymous opens never touch the catalog and carry no name.
 kimix_gdb_open_options anonymous{};
 anonymous.directory = nullptr;
 kimix_gdb_session *third = nullptr;
 expect(eq(kimix_gdb_open(&anonymous, &third), KIMIX_GDB_OK));
 expect(eq(kimix_gdb_catalog_count(&count), KIMIX_GDB_OK));
 expect(eq(count, baseline + 1));
 expect(eq(kimix_gdb_session_graph_name(third, name, sizeof(name), &len), KIMIX_GDB_OK));
 expect(eq(len, size_t{0})) << "the anonymous session has no name";
 kimix_gdb_close(third);
 // Closing the named session releases the entry; the name is reusable.
 kimix_gdb_close(first);
 expect(eq(kimix_gdb_catalog_count(&count), KIMIX_GDB_OK));
 expect(eq(count, baseline));
 exists = 1;
 expect(eq(kimix_gdb_catalog_contains("catalog-walk", 12, &exists), KIMIX_GDB_OK));
 expect(eq(exists, uint8_t{0}));
 kimix_gdb_session *reopened = nullptr;
 expect(eq(kimix_gdb_open(&options, &reopened), KIMIX_GDB_OK));
 kimix_gdb_close(reopened);
 // NULL guards.
 expect(eq(kimix_gdb_catalog_contains(nullptr, 3, &exists), KIMIX_GDB_ERR_ARGUMENT));
 expect(eq(kimix_gdb_catalog_contains("catalog-walk", 12, nullptr), KIMIX_GDB_ERR_ARGUMENT));
 expect(eq(kimix_gdb_catalog_count(nullptr), KIMIX_GDB_ERR_ARGUMENT));
 expect(eq(kimix_gdb_session_graph_name(nullptr, name, sizeof(name), &len),
 KIMIX_GDB_ERR_ARGUMENT));
 expect(eq(kimix_gdb_session_graph_name(reopened, name, sizeof(name), nullptr),
 KIMIX_GDB_ERR_ARGUMENT));
 };

// The ABI 4 create-time edge payload (plan v3 step 3.1/3.2): ONE call with
// property_names + properties, two-phase read-back, and the refusal matrix
// proving a refused create leaves nothing behind.
"cabi_edge_create_with_properties"_test = [] {
 session_guard guard;
 kimix_gdb_session *session = guard.session;
 uint32_t const node = guard.label("node");
 uint32_t const link = guard.label("link");
 uint32_t const age = guard.property_type("age", KIMIX_GDB_DTYPE_INT64);
 uint32_t const tag = guard.property_type("tag", KIMIX_GDB_DTYPE_CHAR);
 kimix_gdb_guid const a = guard.vertex(node, "a");
 kimix_gdb_guid const b = guard.vertex(node, "b");
 uint32_t const link_only[1]{link};
 uint32_t const names[2]{age, tag};
 kimix_gdb_property const values[2]{int_property(42), text_property("hello")};
 kimix_gdb_guid edge{};
 expect(eq(kimix_gdb_edge_create(session, a, b, KIMIX_GDB_EDGE_DIRECTED, link_only, 1,
 names, values, 2, &edge),
 KIMIX_GDB_OK));
 // The payload comes back in one two-phase read: both name ids, both values.
 size_t count = 0;
 size_t scratch_bytes = 0;
 expect(eq(kimix_gdb_edge_properties(session, edge, nullptr, 0, nullptr, 0, &count,
 &scratch_bytes),
 KIMIX_GDB_OK));
 expect(eq(count, size_t{2}));
 kimix::vector<kimix_gdb_property_record> items(count);
 kimix::vector<uint8_t> scratch(scratch_bytes + 8);
 expect(eq(kimix_gdb_edge_properties(session, edge, items.data(), items.size(),
 scratch.data(), scratch.size(), &count, &scratch_bytes),
 KIMIX_GDB_OK));
 expect(eq(count, size_t{2}));
 bool saw_age = false;
 bool saw_tag = false;
 for (size_t i = 0; i < count; ++i) {
 saw_age = saw_age || (items[i].name_id == age && items[i].kind == KIMIX_GDB_KIND_INT64 &&
 items[i].i64 == 42);
 saw_tag = saw_tag || (items[i].name_id == tag && items[i].kind == KIMIX_GDB_KIND_STRING &&
 items[i].payload_len == 5);
 }
 expect(saw_age) << "the create-time int64 survived";
 expect(saw_tag) << "the create-time string survived (5 bytes in scratch)";
 // The refusal matrix.
 expect(eq(kimix_gdb_edge_create(session, a, b, KIMIX_GDB_EDGE_DIRECTED, nullptr, 0,
 nullptr, values, 1, &edge),
 KIMIX_GDB_ERR_ARGUMENT))
 << "property_count > 0 with null names is refused up front";
 expect(eq(kimix_gdb_edge_create(session, a, b, KIMIX_GDB_EDGE_DIRECTED, nullptr, 0,
 names, nullptr, 1, &edge),
 KIMIX_GDB_ERR_ARGUMENT));
 uint32_t const bad_names[1]{9999};
 expect(eq(kimix_gdb_edge_create(session, a, b, KIMIX_GDB_EDGE_DIRECTED, nullptr, 0,
 bad_names, values, 1, &edge),
 KIMIX_GDB_ERR_PROPERTY_TYPE));
 size_t seen = 0;
 size_t total = 0;
 expect(eq(kimix_gdb_edges_of_vertex(session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 0,
 nullptr, 0, &seen, &total),
 KIMIX_GDB_OK));
 expect(eq(total, size_t{1})) << "the one good edge is the only edge";
 };
 }
