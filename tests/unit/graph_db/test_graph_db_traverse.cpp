// Test for the native multi-hop traversal (src/graph_db/graph.{h,cpp}
// Graph::traverse_bfs / Graph::shortest_path bounded overload — G3, plan v3
// step 1.2).
//
// This test covers:
// - exact equivalence against a brute-force BFS (over the public allocating
//   adjacency forms) on the known 6-vertex DAG and a 200-vertex layered DAG,
//   over the cap x depth x orientation matrix, filtered and unfiltered;
// - cycle safety (a ring with a chord terminates and still matches the
//   reference);
// - the cap contract: at most cap_per_level vertices per level, a full level
//   stops expanding, depth 0 yields exactly the start;
// - the §5.10 filter contract: a filtered-out vertex is not enqueued, so its
//   neighbourhood is never visited (label and property conditions);
// - argument errors: cap < 1, depth 0 / > kMaxNativeHopDepth, unknown start,
//   a label condition with an ordering op (error_op);
// - determinism: repeated traversals return the identical sequence;
// - the bounded shortest_path: same result as the unbounded form when the
//   bound suffices, empty when it does not, error_vertex on unknown endpoints;
// - the EDGE-filtered shortest_path (the edge-searching form): equivalence
//   against a brute-force walk over the PUBLIC edge reads over the
//   constraint x orientation x depth x endpoint matrix, the reported hop edges
//   (parallel edges included), the zero-length and unreachable answers, the
//   error surface, oriented_links / edge_matches, and the filtered has_path;
// - the EDGE-filtered BFS (plan §1.3, ABI 7): Graph::traverse_bfs_edges against
//   a brute force over the public edge reads (discovery order, discovering edge,
//   cap rule and the internal-edge sweep included), and the
//   kimix_gdb_traverse_bfs_edges C ABI — parallel edges both reported, the
//   triangle's internal edge reported, max_vertices truncation and its flag, the
//   two-phase probe/fill contract with ERR_BUFFER on a short capacity, the error
//   surface, the depth shape, and the W007/S3 leak scenario through the ANY_IN
//   edge filter;
// - a 4-reader / 1-writer stress run (no crash, BFS invariants hold, and a
//   post-run quiescent traversal matches the brute force exactly).
#include "ut/ut.hpp"

#include "graph_db_test_util.h"

#include <core/kimix_core.h> // umbrella first: it fixes the winsock2.h order
#include <core/fiber.h>

#include <graph_db/graph.h>
#include <graph_db/graph_c_api.h> // the ABI-7 traverse_bfs_edges legs below
#include <graph_db/graph_db.h>
#include <graph_db/query.h>
#include <graph_db/schema.h>

#include <core/guid.h>
#include <core/stl/unordered_map.h> // kimix::unordered_set lives here
#include <core/stl/vector.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring> // std::strlen for the C ABI byte views
#include <thread>
#include <utility>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;
using graph_db::AdjacencyBuffer;
using graph_db::Constraint;
using graph_db::ConstraintCondition;
using graph_db::ConstraintHandle;
using graph_db::Database;
using graph_db::EdgeId;
using graph_db::EdgeLink;
using graph_db::EdgeOrientation;
using graph_db::EdgeRecord;
using graph_db::GdiOp;
using graph_db::GdiStatus;
using graph_db::Graph;
using graph_db::LabelId;
using graph_db::PropNameId;
using graph_db::PropertyBag;
using graph_db::PropertyList;
using graph_db::PropertyValue;
using graph_db::Vertex;
using PathStep = graph_db::Graph::PathStep;
using TraversalVertex = graph_db::Graph::TraversalVertex;
namespace gtest = kimix::graph_db::test;

namespace tests_unit_graph_db_test_graph_db_traverse {

// Fabricated interned ids (the Graph level has no schema to register them in;
// LabelId / PropNameId are plain uint32_t aliases, property.h).
inline constexpr LabelId kPersonLabel = 7u;
inline constexpr LabelId kRobotLabel = 8u;
inline constexpr PropNameId kAgeName = 5u;
inline constexpr PropNameId kNameName = 6u;

// One vertex of the brute-force reference: same shape as TraversalVertex.
struct RefVertex {
    kimix::Guid guid;
    kimix::Guid parent;
    uint32_t depth;
};

// The oriented adjacency as a fresh owning copy (the public allocating forms).
kimix::vector<kimix::Guid> oriented_adjacency(Graph const &graph, kimix::Guid const &guid,
                                              EdgeOrientation orientation) {
    if (orientation == EdgeOrientation::outgoing) {
        return graph.dependencies(guid);
    }
    if (orientation == EdgeOrientation::incoming) {
        return graph.dependents(guid);
    }
    // Both: out then in, concatenated (the same order _assign_two uses); the
    // global visited set below removes the cross-list duplicates.
    kimix::vector<kimix::Guid> out = graph.dependencies(guid);
    kimix::vector<kimix::Guid> in = graph.dependents(guid);
    out.insert(out.end(), in.begin(), in.end());
    return out;
}

// Does `vertex` satisfy `constraint` (the §5.10 vertex filter)?
bool vertex_matches(Graph const &graph, kimix::Guid const &guid, Constraint const &constraint) {
    Vertex *vertex = graph.find(guid);
    if (vertex == nullptr) {
        return false; // a vanished endpoint is skipped, not an error
    }
    kimix::vector<LabelId> labels;
    vertex->labels(labels);
    PropertyBag bag;
    PropertyList list = vertex->property_list();
    for (auto const &entry : list) {
        bag.emplace(entry.name, entry.value);
    }
    return graph_db::matches(constraint, kimix::span<const LabelId>{labels.data(),
                                                                    labels.size()},
                             bag);
}

// Brute-force BFS with exactly Graph::traverse_bfs's semantics: the start at
// depth 0 unconditionally; per-level discovery in adjacency order; a level
// stops expanding the moment it reaches `cap`; `max_depth` bounds the walk;
// an optional vertex filter gates ENQUEUE (a failed vertex's neighbourhood
// is never visited); duplicates never re-enqueued.
kimix::vector<RefVertex> brute_force_bfs(Graph const &graph, kimix::Guid const &start,
                                         Constraint const *constraint,
                                         EdgeOrientation orientation, int64_t cap,
                                         uint32_t max_depth) {
    kimix::vector<RefVertex> result;
    result.push_back(RefVertex{start, kimix::Guid{}, 0});
    kimix::unordered_set<kimix::Guid> visited;
    visited.insert(start);
    kimix::vector<kimix::Guid> frontier{start};
    for (uint32_t depth = 1; depth <= max_depth && !frontier.empty(); ++depth) {
        kimix::vector<kimix::Guid> next;
        int64_t level_count = 0;
        bool level_full = false;
        for (auto const &current : frontier) {
            if (level_full) {
                break;
            }
            for (auto const &candidate : oriented_adjacency(graph, current, orientation)) {
                if (level_count >= cap) {
                    level_full = true;
                    break;
                }
                if (visited.find(candidate) != visited.end()) {
                    continue;
                }
                if (constraint != nullptr && !vertex_matches(graph, candidate, *constraint)) {
                    continue;
                }
                visited.insert(candidate);
                result.push_back(RefVertex{candidate, current, depth});
                next.push_back(candidate);
                ++level_count;
            }
        }
        frontier = std::move(next);
    }
    return result;
}

// Exact sequence equality (guid, parent, depth — order included).
bool same_walk(kimix::vector<TraversalVertex> const &actual,
               kimix::vector<RefVertex> const &expected) {
    if (actual.size() != expected.size()) {
        return false;
    }
    for (size_t i = 0; i < actual.size(); ++i) {
        if (!(actual[i].guid == expected[i].guid &&
              actual[i].parent == expected[i].parent &&
              actual[i].depth == expected[i].depth)) {
            return false;
        }
    }
    return true;
}

// Build a star-of-stars hub graph: `spokes` leaves around one hub, plus a
// second-level hub reached from the first (so 2-hop walks have real work).
struct hub_graph {
    Graph graph;
    Vertex *hub{nullptr};
    Vertex *hub2{nullptr};
    kimix::vector<Vertex *> leaves;
    kimix::vector<Vertex *> outer;

    explicit hub_graph(size_t spokes) {
        hub = graph.create_vertex();
        hub2 = graph.create_vertex();
        leaves.reserve(spokes);
        outer.reserve(spokes);
        graph.connect(hub->guid(), hub2->guid());
        for (size_t i = 0; i < spokes; ++i) {
            Vertex *leaf = graph.create_vertex();
            graph.connect(hub->guid(), leaf->guid()); // hub -> leaf (outgoing)
            leaves.push_back(leaf);
            Vertex *tip = graph.create_vertex();
            graph.connect(leaf->guid(), tip->guid()); // leaf -> far (2-hop)
            outer.push_back(tip);
        }
    }
};

// A ring of `count` vertices plus one chord — cycles everywhere, but a
// bounded BFS over it must terminate and match the reference.
void build_ring(Graph &graph, size_t count, kimix::vector<Vertex *> &out) {
    out.clear();
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        out.push_back(graph.create_vertex());
    }
    for (size_t i = 0; i < count; ++i) {
        graph.connect(out[i]->guid(), out[(i + 1) % count]->guid());
    }
    if (count > 3) {
        graph.connect(out[0]->guid(), out[count / 2]->guid()); // the chord
    }
}

// The BFS invariants every traverse_bfs result must satisfy DURING a
// concurrent-writer stress run.  Deliberately time-local only: start first at
// depth 0; no duplicates; per-level caps; depths increase by at most one per
// step.  Edge existence is NOT checked here — a parent edge may legitimately
// be removed by the writer between the discovery and this check (the walk was
// consistent at discovery time; the graph need not be consistent with it
// afterwards).  Quiescent consistency is verified separately by the
// brute-force comparison after the stress.
bool walk_is_consistent(kimix::vector<TraversalVertex> const &walk, kimix::Guid const &start,
                        int64_t cap_per_level) {
    if (walk.empty() || !(walk[0].guid == start) || walk[0].depth != 0) {
        return false;
    }
    kimix::unordered_set<kimix::Guid> seen;
    seen.insert(start);
    kimix::vector<int64_t> level_count(walk.back().depth + 1, 0);
    for (size_t i = 1; i < walk.size(); ++i) {
        TraversalVertex const &v = walk[i];
        if (v.depth == 0 || v.depth > walk[i - 1].depth + 1) {
            return false;
        }
        if (seen.find(v.guid) != seen.end()) {
            return false; // duplicate
        }
        seen.insert(v.guid);
        if (level_count[v.depth]++ >= cap_per_level) {
            return false; // per-level cap violated
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// The EDGE-filtered path fixtures (the edge-searching form of shortest_path)
// ---------------------------------------------------------------------------

// Fabricated ids for the edge filters. The Graph level has no schema to register
// them in (LabelId / PropNameId are plain uint32_t aliases, property.h).
inline constexpr LabelId kOpenEdgeLabel = 21u;
inline constexpr LabelId kBlockedEdgeLabel = 22u;
inline constexpr PropNameId kCostName = 11u;
// A label no filter below selects: the wide-visit fixtures fill the HEAD and the
// first drained CHUNK with these, so nothing but the deliberately placed links can
// become the hop.
inline constexpr LabelId kDecoyEdgeLabel = 23u;
// The visit width that puts the walk on the BATCHED edge filter
// (graph.cpp's kEdgeFilterMinLinks), and the link index the wide-visit fixtures
// place their interesting links at: the head is the first 64 links taken per link,
// the first drained chunk is links[64,128), and the second chunk starts at 128. 130
// is therefore INSIDE a drained chunk and not on its boundary, so the batch's
// staging and its consume loop both decide what the hop reports, and an off-by-one
// in either moves the hop onto an edge that does not join the two steps.
inline constexpr size_t kWideVisitLinks = 200;
inline constexpr size_t kWideVisitPairAt = 130;

// The (neighbour, edge) pairs a brute force steps over from `current`, computed
// through the PUBLIC edge reads only — incident_edges + edge_record + matches —
// so the reference below shares no code with the engine's link-level walk.
void ref_hops(Graph const &graph, kimix::Guid const &current, EdgeOrientation orientation,
              Constraint const &constraint,
              kimix::vector<std::pair<kimix::Guid, EdgeId>> &out) {
    out.clear();
    kimix::vector<EdgeId> ids;
    graph.incident_edges(current, orientation, ids);
    for (auto const &id : ids) {
        EdgeRecord record;
        if (!graph.edge_record(id, record)) {
            continue;
        }
        if (!graph_db::matches(constraint, record.labels, record.properties)) {
            continue;
        }
        // The other end: an out link points at the target, an in link at the
        // origin; a self loop is both, and either expression gives `current`.
        kimix::Guid const neighbour = record.origin == current ? record.target : record.origin;
        out.push_back({neighbour, id});
    }
}

// A brute-force edge-filtered bounded shortest path: `vertices` is the path with
// its endpoints included, `edges[i]` the edge that reaches `vertices[i]`, both
// empty when `to` is unreachable within the bound.
struct RefPath {
    kimix::vector<kimix::Guid> vertices;
    kimix::vector<EdgeId> edges;
};

RefPath ref_filtered_path(Graph const &graph, kimix::Guid const &from, kimix::Guid const &to,
                          Constraint const &constraint, EdgeOrientation orientation,
                          uint32_t max_depth) {
    RefPath path;
    if (from == to) {
        path.vertices.push_back(from);
        path.edges.push_back(EdgeId{});
        return path;
    }
    kimix::unordered_map<kimix::Guid, kimix::Guid> parent;
    kimix::unordered_map<kimix::Guid, EdgeId> via;
    parent.emplace(from, from);
    via.emplace(from, EdgeId{});
    kimix::vector<kimix::Guid> frontier;
    kimix::vector<kimix::Guid> next_frontier;
    kimix::vector<std::pair<kimix::Guid, EdgeId>> hops;
    frontier.push_back(from);
    uint32_t depth = 0;
    bool found = false;
    while (!frontier.empty() && !found && depth < max_depth) {
        ++depth;
        next_frontier.clear();
        for (auto const &current : frontier) {
            ref_hops(graph, current, orientation, constraint, hops);
            for (auto const &hop : hops) {
                if (parent.find(hop.first) != parent.end()) {
                    continue;
                }
                parent.emplace(hop.first, current);
                via.emplace(hop.first, hop.second);
                if (hop.first == to) {
                    found = true;
                    break;
                }
                next_frontier.push_back(hop.first);
            }
            if (found) {
                break;
            }
        }
        frontier.swap(next_frontier);
    }
    if (!found) {
        return path;
    }
    kimix::vector<kimix::Guid> reversed;
    kimix::vector<EdgeId> reversed_edges;
    for (kimix::Guid cursor = to;; cursor = parent.find(cursor)->second) {
        reversed.push_back(cursor);
        reversed_edges.push_back(via.find(cursor)->second);
        if (cursor == from) {
            break;
        }
    }
    std::reverse(reversed.begin(), reversed.end());
    std::reverse(reversed_edges.begin(), reversed_edges.end());
    path.vertices = std::move(reversed);
    path.edges = std::move(reversed_edges);
    return path;
}

// The engine's path equals the reference: same vertices, same edges (so a
// parallel-edge choice is pinned too), and depth == index.
void expect_same_path(kimix::vector<Graph::PathStep> const &actual, RefPath const &expected,
                      char const *what) {
    expect(eq(actual.size(), expected.vertices.size())) << what;
    if (actual.size() != expected.vertices.size()) {
        return;
    }
    for (size_t i = 0; i < actual.size(); ++i) {
        expect(actual[i].vertex == expected.vertices[i]) << what;
        expect(actual[i].edge == expected.edges[i]) << what;
        expect(eq(actual[i].depth, static_cast<uint32_t>(i))) << what;
    }
}

// Every hop of a returned path must be REAL: the reported edge exists, joins the
// two consecutive vertices in the walked orientation, and satisfies the filter.
void expect_path_is_sound(Graph const &graph, kimix::vector<Graph::PathStep> const &path,
                          Constraint const &constraint, EdgeOrientation orientation,
                          kimix::Guid const &from, kimix::Guid const &to, char const *what) {
    expect(!path.empty()) << what;
    if (path.empty()) {
        return;
    }
    expect(path.front().vertex == from) << what;
    expect(path.back().vertex == to) << what;
    expect(!static_cast<bool>(path.front().edge)) << what << ": the start is reached by no edge";
    kimix::unordered_set<kimix::Guid> seen;
    for (size_t i = 0; i < path.size(); ++i) {
        expect(seen.insert(path[i].vertex).second) << what << ": the path is simple";
        if (i == 0) {
            continue;
        }
        EdgeRecord record;
        expect(graph.edge_record(path[i].edge, record)) << what << ": the hop's edge exists";
        expect(graph.edge_matches(path[i].edge, constraint)) << what << ": the hop's edge matches";
        bool const forward =
            record.origin == path[i - 1].vertex && record.target == path[i].vertex;
        bool const backward =
            record.origin == path[i].vertex && record.target == path[i - 1].vertex;
        bool const connects = orientation == EdgeOrientation::outgoing
                                  ? forward
                                  : orientation == EdgeOrientation::incoming ? backward
                                                                             : (forward || backward);
        expect(connects) << what << ": the hop's edge joins the two steps";
    }
}



// One directed edge carrying `labels`; true when the graph accepted it. A fixture
// failure is folded into a single call-site count instead of an assert per edge, so
// a 200-link fixture does not swamp the suite's assert count.
bool wide_link(Graph &graph, kimix::Guid const &from, kimix::Guid const &to,
               kimix::span<const LabelId> labels, EdgeId &out) {
    return graph.edge_create(from, to, graph_db::EdgeDirectionType::directed, labels, nullptr, nullptr,
                             out) == GdiStatus::ok;
}

// ---------------------------------------------------------------------------
// The EDGE-filtered BFS (plan §1.3): the brute-force reference.
//
// It shares NOTHING with Graph::traverse_bfs_edges: hops come from the PUBLIC edge
// reads (incident_edges + edge_record + the deep-copy matches() overload, the
// ref_hops() helper above), the vertex cap is applied in the same place the engine
// applies it (a hop that passed both tests, refused because the array is full), and
// the internal-edge sweep enumerates edge_ids() — the whole table — instead of
// following adjacency. So a pass pins the engine's discovery order, its discovering
// edge choice, its cap rule AND the "every internal edge, deduplicated by uid"
// guarantee against a source the walk cannot drift from.
// ---------------------------------------------------------------------------
struct RefBfsVertex {
    kimix::Guid vertex;
    EdgeId edge;
    uint32_t depth;
};

struct RefBfs {
    kimix::vector<RefBfsVertex> vertices;
    kimix::vector<EdgeId> edges; // the internal-edge set, table order
    bool truncated{false};
};

// The multi-start reference (ABI 8): every DISTINCT start is a depth-0 item in the
// caller's order (a duplicate collapses), all of them seed the depth-0 frontier, and
// the walk then runs the ABI-7 rule unchanged -- so `depth` is the distance to the
// nearest start. Distinct starts alone past the cap keep the first `max_vertices`,
// set the truncation flag and report no edges (the walk never started).
RefBfs ref_edge_bfs_multi(Graph const &graph, kimix::span<const kimix::Guid> starts,
                          Constraint const &constraint, EdgeOrientation orientation,
                          uint32_t max_depth, uint32_t max_vertices) {
    RefBfs out;
    kimix::unordered_set<kimix::Guid> visited;
    kimix::vector<kimix::Guid> frontier;
    for (auto const &start : starts) {
        if (!visited.insert(start).second) {
            continue;
        }
        if (out.vertices.size() >= max_vertices) {
            out.truncated = true;
            return out;
        }
        out.vertices.push_back(RefBfsVertex{start, EdgeId{}, 0});
        frontier.push_back(start);
    }
    kimix::vector<std::pair<kimix::Guid, EdgeId>> hops;
    bool truncated = false;
    uint32_t depth = 0;
    while (!frontier.empty() && !truncated && depth < max_depth) {
        ++depth;
        kimix::vector<kimix::Guid> next;
        for (auto const &current : frontier) {
            if (truncated) {
                break;
            }
            ref_hops(graph, current, orientation, constraint, hops);
            for (auto const &hop : hops) {
                if (visited.find(hop.first) != visited.end()) {
                    continue; // discovered already: not a hop
                }
                if (out.vertices.size() >= max_vertices) {
                    truncated = true; // the cap refused a hop that had passed both tests
                    break;
                }
                visited.insert(hop.first);
                out.vertices.push_back(RefBfsVertex{hop.first, hop.second, depth});
                next.push_back(hop.first);
            }
        }
        frontier = std::move(next);
    }
    out.truncated = truncated;
    // The sweep, brute forced: every edge whose OWN payload satisfies the constraint
    // and whose two endpoints are both in the discovered set. The table scan is the
    // whole point — the engine walks adjacency, this reads the edge list.
    for (auto const &id : graph.edge_ids()) {
        EdgeRecord record;
        if (!graph.edge_record(id, record)) {
            continue;
        }
        if (!graph_db::matches(constraint, record.labels, record.properties)) {
            continue;
        }
        if (visited.find(record.origin) == visited.end() ||
            visited.find(record.target) == visited.end()) {
            continue; // not internal to the discovered set
        }
        out.edges.push_back(id);
    }
    return out;
}

// The ABI-7 single-start reference: the multi-start body with one start.
RefBfs ref_edge_bfs(Graph const &graph, kimix::Guid const &start, Constraint const &constraint,
                    EdgeOrientation orientation, uint32_t max_depth, uint32_t max_vertices) {
    return ref_edge_bfs_multi(graph, kimix::span<const kimix::Guid>{&start, 1}, constraint,
                              orientation, max_depth, max_vertices);
}

// The engine's discovery sequence equals the reference's: same vertices, in the same
// order, each with the same discovering edge and depth.
void expect_same_bfs_vertices(kimix::vector<Graph::BfsVertex> const &actual,
                              kimix::vector<RefBfsVertex> const &expected, char const *what) {
    expect(eq(actual.size(), expected.size())) << what;
    if (actual.size() != expected.size()) {
        return;
    }
    for (size_t i = 0; i < actual.size(); ++i) {
        expect(actual[i].vertex == expected[i].vertex) << what << ": vertex " << i;
        expect(actual[i].edge == expected[i].edge) << what << ": discovering edge of " << i;
        expect(eq(actual[i].depth, expected[i].depth)) << what << ": depth of " << i;
    }
}

// The engine's internal-edge SET equals the reference's (the ORDER is the sweep's own
// contract — discovery order then link order — so it is compared as a set here and
// pinned for shape separately by the C ABI legs below).
void expect_same_bfs_edges(kimix::vector<Graph::BfsEdge> const &actual,
                           kimix::vector<EdgeId> const &expected, char const *what) {
    expect(eq(actual.size(), expected.size())) << what;
    kimix::unordered_set<kimix::Guid> got;
    for (auto const &edge : actual) {
        expect(got.insert(edge.edge).second) << what << ": one record per edge uid";
    }
    for (auto const &id : expected) {
        expect(got.find(id) != got.end()) << what << ": the edge is reported";
    }
}

// Every reported vertex is SOUND: its discovering edge exists, satisfies the
// constraint, reaches that vertex, and the depths form a non-decreasing BFS level
// sequence (0 for the start, whose edge is zeroed).
void expect_bfs_vertices_are_sound(Graph const &graph,
                                   kimix::vector<Graph::BfsVertex> const &walk,
                                   Constraint const &constraint, char const *what) {
    expect(!walk.empty()) << what;
    if (walk.empty()) {
        return;
    }
    expect(!static_cast<bool>(walk[0].edge)) << what << ": the start is reached by no edge";
    expect(eq(walk[0].depth, uint32_t{0})) << what << ": the start is level 0";
    kimix::unordered_set<kimix::Guid> seen;
    for (size_t i = 0; i < walk.size(); ++i) {
        expect(seen.insert(walk[i].vertex).second) << what << ": a vertex is discovered once";
        if (i == 0) {
            continue;
        }
        EdgeRecord record;
        expect(graph.edge_record(walk[i].edge, record)) << what << ": the hop's edge exists";
        expect(graph.edge_matches(walk[i].edge, constraint)) << what << ": the hop's edge matches";
        expect(record.origin == walk[i].vertex || record.target == walk[i].vertex)
            << what << ": the hop's edge reaches the vertex it discovered";
        expect(walk[i].depth >= 1u && walk[i].depth <= walk[i - 1].depth + 1u)
            << what << ": levels ascend by at most one";
    }
}

// ---------------------------------------------------------------------------
// The C ABI scaffolding for the traverse_bfs_edges legs. This suite is otherwise
// the C++ level; these legs walk the frozen ABI-7 entry point itself (the shapes
// the managed wrapper binds), so they need the C header and an open session —
// the same helpers test_graph_db_c_api.cpp uses for its shortest_path_edges legs,
// spelled HERE because the BFS legs belong with the walk they exercise, and the
// string-array block (§1.1) is written out byte by byte rather than borrowed: an
// independent encoder is the point of the W007/S3 leg.
// ---------------------------------------------------------------------------

// The frozen POD shapes: two guids + depth + reserved, and three guids. Pinned at
// compile time because a drift is an ABI break, not a test failure.
static_assert(sizeof(kimix_gdb_bfs_vertex) == 40,
              "kimix_gdb_bfs_vertex is 40 bytes (guid, guid, u32, u32)");
static_assert(sizeof(kimix_gdb_bfs_edge) == 48,
              "kimix_gdb_bfs_edge is 48 bytes (three guids, no reserved word)");

kimix_gdb_bytes cabi_bytes(char const *text) {
    kimix_gdb_bytes out;
    out.data = reinterpret_cast<uint8_t const *>(text);
    out.len = std::strlen(text);
    return out;
}

void push_le32(kimix::vector<uint8_t> &block, uint32_t value) {
    block.push_back(static_cast<uint8_t>(value & 0xffu));
    block.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
    block.push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
    block.push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
}

// The §1.1 string-array block: [u32 element_count]{[u32 byte_len][UTF-8 bytes]}*.
kimix::vector<uint8_t> sa_block(std::initializer_list<char const *> elements) {
    kimix::vector<uint8_t> block;
    push_le32(block, static_cast<uint32_t>(elements.size()));
    for (char const *text : elements) {
        uint32_t const len = static_cast<uint32_t>(std::strlen(text));
        push_le32(block, len);
        for (uint32_t i = 0; i < len; ++i) {
            block.push_back(static_cast<uint8_t>(text[i]));
        }
    }
    return block;
}

kimix_gdb_property sa_property(kimix::vector<uint8_t> const &block, uint32_t count) {
    kimix_gdb_property property{};
    property.kind = KIMIX_GDB_KIND_STRING_ARRAY;
    property.dtype = KIMIX_GDB_DTYPE_CHAR;
    property.count = count;
    property.payload.data = block.data();
    property.payload.len = block.size();
    return property;
}

// One open in-memory session (parallel edges and self edges allowed — the fixtures
// that pin the sweep's dedupe need both), closed even on an early return.
struct cabi_session {
    kimix_gdb_session *session{nullptr};

    cabi_session() {
        kimix_gdb_open_options options{};
        options.directory = nullptr; // in memory
        options.auto_create_schema = 1;
        options.allow_parallel_edges = 1;
        options.allow_self_edges = 1;
        options.max_journal_bytes = 64ull * 1024ull * 1024ull;
        expect(eq(kimix_gdb_open(&options, &session), KIMIX_GDB_OK));
    }
    cabi_session(cabi_session const &) = delete;
    cabi_session &operator=(cabi_session const &) = delete;
    ~cabi_session() {
        if (session != nullptr) {
            kimix_gdb_close(session);
        }
    }

    uint32_t label(char const *name) {
        uint32_t id = 0;
        expect(eq(kimix_gdb_label_create(session, name, std::strlen(name), &id), KIMIX_GDB_OK));
        return id;
    }
    // An ARRAY declaration (container array + the exact element count): the shape a
    // string_array value must satisfy (plan §1.1 — char_ + array serves String and
    // StringArray alike).
    uint32_t array_type(char const *name, uint32_t count) {
        uint32_t id = 0;
        expect(eq(kimix_gdb_property_type_create(session, name, std::strlen(name),
                                                 KIMIX_GDB_CONTAINER_ARRAY,
                                                 KIMIX_GDB_DTYPE_CHAR, KIMIX_GDB_SIZE_FIXED, count, &id),
                  KIMIX_GDB_OK));
        return id;
    }
    kimix_gdb_guid vertex(uint32_t label_id, char const *external) {
        kimix_gdb_vertex_spec spec{};
        spec.key.label = label_id;
        spec.key.external_id = cabi_bytes(external);
        kimix_gdb_guid uid{};
        uint8_t warning = 0;
        expect(eq(kimix_gdb_vertex_create(session, &spec, &uid, &warning), KIMIX_GDB_OK));
        return uid;
    }
    void add_vertex_docs(kimix_gdb_guid uid, uint32_t name,
                         kimix_gdb_property const &value) {
        expect(eq(kimix_gdb_vertex_property_add(session, uid, name, &value), KIMIX_GDB_OK));
    }
    kimix_gdb_guid edge(kimix_gdb_guid origin, kimix_gdb_guid target) {
        kimix_gdb_guid id{};
        expect(eq(kimix_gdb_edge_create(session, origin, target, KIMIX_GDB_EDGE_DIRECTED, nullptr,
                                        0, nullptr, nullptr, 0, nullptr, &id),
                  KIMIX_GDB_OK));
        return id;
    }
    kimix_gdb_guid edge_labeled(kimix_gdb_guid origin, kimix_gdb_guid target, uint32_t label_id) {
        kimix_gdb_guid id{};
        expect(eq(kimix_gdb_edge_create(session, origin, target, KIMIX_GDB_EDGE_DIRECTED,
                                        &label_id, 1, nullptr, nullptr, 0, nullptr, &id),
                  KIMIX_GDB_OK));
        return id;
    }
    // One edge carrying one property — the evidence-document shape of the W007/S3 leg.
    kimix_gdb_guid edge_with_property(kimix_gdb_guid origin, kimix_gdb_guid target,
                                      uint32_t name, kimix_gdb_property const &value) {
        kimix_gdb_guid id{};
        uint32_t const names[1]{name};
        const kimix_gdb_property values[1]{value};
        expect(eq(kimix_gdb_edge_create(session, origin, target, KIMIX_GDB_EDGE_DIRECTED, nullptr,
                                        0, names, values, 1, nullptr, &id),
                  KIMIX_GDB_OK));
        return id;
    }
    uint32_t constraint() {
        uint32_t id = 0;
        expect(eq(kimix_gdb_constraint_create(session, &id), KIMIX_GDB_OK));
        return id;
    }
};

// Both arrays of one traverse_bfs_edges result, as C++ vectors (the legs below
// assert on these; the raw caller storage is the test's, as the contract demands).
struct bfs_result {
    kimix::vector<kimix_gdb_bfs_vertex> vertices;
    kimix::vector<kimix_gdb_bfs_edge> edges;
    size_t vertex_count{0};
    size_t edge_count{0};
    uint8_t truncated{0};
};

// The documented two-phase call: PROBE (both arrays NULL) for the three outputs,
// then FILL with exactly that many records. The probe's counts and flag are kept so
// a leg can assert probe == fill; a failed phase returns with empty vectors.
struct two_phase_result {
    bfs_result probe;   // the vectors stay empty in the probe phase
    bfs_result fill;
    int32_t probe_code{0};
    int32_t fill_code{0};
};

// The ABI-7 request shape, forwarded to the ABI-8 entry point: ONE start vertex
// passed as a one-element array. Every leg that pins a single-start answer goes
// through this, so what it checks is the frozen ABI-7 answer rather than a
// rewritten one; the legs that exercise several starts call the entry point
// directly.
int32_t bfs_edges_one(kimix_gdb_session *session, kimix_gdb_guid start,
                      uint32_t edge_constraint, uint8_t orientation, uint32_t max_depth,
                      uint32_t max_vertices, kimix_gdb_bfs_vertex *items,
                      size_t vertex_capacity, kimix_gdb_bfs_edge *edges, size_t edge_capacity,
                      size_t *out_vertex_count, size_t *out_edge_count,
                      uint8_t *out_truncated) {
    return kimix_gdb_traverse_bfs_edges(session, &start, 1, edge_constraint, orientation,
                                        max_depth, max_vertices, items, vertex_capacity, edges,
                                        edge_capacity, out_vertex_count, out_edge_count,
                                        out_truncated);
}

two_phase_result two_phase_bfs(kimix_gdb_session *session, kimix_gdb_guid start,
                               uint32_t constraint, uint8_t orientation, uint32_t max_depth,
                               uint32_t max_vertices) {
    two_phase_result out;
    out.probe_code = bfs_edges_one(
        session, start, constraint, orientation, max_depth, max_vertices, nullptr, 0, nullptr, 0,
        &out.probe.vertex_count, &out.probe.edge_count, &out.probe.truncated);
    if (out.probe_code != KIMIX_GDB_OK) {
        return out;
    }
    out.fill.vertices.resize(out.probe.vertex_count);
    out.fill.edges.resize(out.probe.edge_count);
    out.fill_code = bfs_edges_one(
        session, start, constraint, orientation, max_depth, max_vertices,
        out.fill.vertices.data(), out.fill.vertices.size(), out.fill.edges.data(),
        out.fill.edges.size(), &out.fill.vertex_count, &out.fill.edge_count,
        &out.fill.truncated);
    return out;
}

// The same two-phase call with SEVERAL starts (the ABI-8 request).
two_phase_result two_phase_bfs_multi(kimix_gdb_session *session,
                                    kimix::vector<kimix_gdb_guid> const &starts,
                                    uint32_t constraint, uint8_t orientation, uint32_t max_depth,
                                    uint32_t max_vertices) {
    two_phase_result out;
    out.probe_code = kimix_gdb_traverse_bfs_edges(
        session, starts.data(), starts.size(), constraint, orientation, max_depth, max_vertices,
        nullptr, 0, nullptr, 0, &out.probe.vertex_count, &out.probe.edge_count,
        &out.probe.truncated);
    if (out.probe_code != KIMIX_GDB_OK) {
        return out;
    }
    out.fill.vertices.resize(out.probe.vertex_count);
    out.fill.edges.resize(out.probe.edge_count);
    out.fill_code = kimix_gdb_traverse_bfs_edges(
        session, starts.data(), starts.size(), constraint, orientation, max_depth, max_vertices,
        out.fill.vertices.data(), out.fill.vertices.size(), out.fill.edges.data(),
        out.fill.edges.size(), &out.fill.vertex_count, &out.fill.edge_count,
        &out.fill.truncated);
    return out;
}

bool guid_eq(kimix_gdb_guid const &a, kimix_gdb_guid const &b) noexcept {
    return a.lo == b.lo && a.hi == b.hi;
}

size_t count_uid(kimix::vector<kimix_gdb_bfs_edge> const &edges, kimix_gdb_guid const &uid) {
    size_t n = 0;
    for (auto const &edge : edges) {
        if (guid_eq(edge.edge, uid)) {
            ++n;
        }
    }
    return n;
}

} // namespace tests_unit_graph_db_test_graph_db_traverse
using namespace tests_unit_graph_db_test_graph_db_traverse;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    kimix::fiber::scheduler sched{4u};

    // -----------------------------------------------------------------------
    // Equivalence vs the brute force, over the whole cap x depth x orientation
    // matrix, on the known 6-vertex DAG.
    // -----------------------------------------------------------------------
    "traverse_bfs_matches_brute_force_small_dag"_test = [] {
        Graph graph;
        auto dag = gtest::small_dag(graph);
        int64_t const caps[] = {1, 2, 3, 100};
        uint32_t const depths[] = {1u, 2u, 3u, Graph::kMaxNativeHopDepth};
        EdgeOrientation const orientations[] = {EdgeOrientation::outgoing,
                                                EdgeOrientation::incoming,
                                                EdgeOrientation::both};
        for (EdgeOrientation orientation : orientations) {
            for (int64_t cap : caps) {
                for (uint32_t depth : depths) {
                    for (size_t s = 0; s < dag.vertices.size(); ++s) {
                        kimix::Guid const start = dag.vertices[s]->guid();
                        kimix::vector<TraversalVertex> actual;
                        expect(eq(graph.traverse_bfs(start, {}, orientation, cap, depth,
                                                     actual),
                                  GdiStatus::ok));
                        auto expected =
                            brute_force_bfs(graph, start, nullptr, orientation, cap, depth);
                        expect(same_walk(actual, expected))
                            << "traverse_bfs matches the brute force (cap/depth/orientation matrix)";
                    }
                }
            }
        }
    };

    // The same, on a 200-vertex layered DAG with a sparse random structure.
    "traverse_bfs_matches_brute_force_layered"_test = [] {
        Graph graph;
        auto vertices = gtest::build_layered_dag(graph, 200, 777u);
        expect(eq(graph.size(), size_t{200}));
        std::mt19937 rng(9u);
        for (int round = 0; round < 40; ++round) {
            kimix::Guid const start = vertices[rng() % vertices.size()]->guid();
            int64_t const cap = 1 + static_cast<int64_t>(rng() % 12);
            uint32_t const depth = 1 + rng() % 4;
            EdgeOrientation const orientation =
                static_cast<EdgeOrientation>(rng() % 3u);
            kimix::vector<TraversalVertex> actual;
            expect(eq(graph.traverse_bfs(start, {}, orientation, cap, depth, actual),
                      GdiStatus::ok));
            auto expected =
                brute_force_bfs(graph, start, nullptr, orientation, cap, depth);
            expect(same_walk(actual, expected))
                << "traverse_bfs matches the brute force on the layered DAG";
        }
    };

    // Cycles: a ring with a chord must terminate and match the reference.
    "traverse_bfs_cycles_terminate_and_match"_test = [] {
        Graph graph;
        kimix::vector<Vertex *> ring;
        build_ring(graph, 24, ring);
        for (int64_t cap : {1, 3, 7, 100}) {
            for (uint32_t depth : {1u, 2u, 4u, 12u, Graph::kMaxNativeHopDepth}) {
                kimix::vector<TraversalVertex> actual;
                expect(eq(graph.traverse_bfs(ring[3]->guid(), {},
                                             EdgeOrientation::outgoing, cap, depth,
                                             actual),
                          GdiStatus::ok));
                auto expected = brute_force_bfs(graph, ring[3]->guid(), nullptr,
                                                EdgeOrientation::outgoing, cap, depth);
                expect(same_walk(actual, expected))
                    << "a cyclic graph terminates and matches the brute force";
            }
        }
    };

    // The cap and depth contract on a hub graph.
    "traverse_bfs_cap_and_depth_contract"_test = [] {
        hub_graph hubs(48);
        // Depth 1 from the hub: exactly the hub's out-adjacency (48 leaves + hub2),
        // capped at 10 -> 10 vertices, all at depth 1.
        {
            kimix::vector<TraversalVertex> walk;
            expect(eq(hubs.graph.traverse_bfs(hubs.hub->guid(), {},
                                              EdgeOrientation::outgoing, 10, 1, walk),
                      GdiStatus::ok));
            expect(eq(walk.size(), size_t{11})) << "start + 10 capped level-1 vertices";
            expect(walk.back().depth == 1_u);
        }
        // Depth 0 request is an error; so is a 0 or negative cap.
        {
            kimix::vector<TraversalVertex> walk;
            expect(eq(hubs.graph.traverse_bfs(hubs.hub->guid(), {},
                                              EdgeOrientation::outgoing, 10, 0, walk),
                      GdiStatus::error_argument));
            expect(eq(hubs.graph.traverse_bfs(hubs.hub->guid(), {},
                                              EdgeOrientation::outgoing, 0, 1, walk),
                      GdiStatus::error_argument));
            expect(eq(hubs.graph.traverse_bfs(hubs.hub->guid(), {},
                                              EdgeOrientation::outgoing, -5, 1, walk),
                      GdiStatus::error_argument));
            expect(eq(hubs.graph.traverse_bfs(hubs.hub->guid(), {},
                                              EdgeOrientation::outgoing, 10,
                                              Graph::kMaxNativeHopDepth + 1, walk),
                      GdiStatus::error_argument))
                << "beyond the advertised max native hop depth";
        }
        // Depth 2 from the hub reaches outer vertices (depth 2) via leaves.
        {
            kimix::vector<TraversalVertex> walk;
            expect(eq(hubs.graph.traverse_bfs(hubs.hub->guid(), {},
                                              EdgeOrientation::outgoing, 100, 2, walk),
                      GdiStatus::ok));
            expect(eq(walk.back().depth, 2u));
            expect(eq(walk.size(), size_t{1 + 49 + 48})) // hub + leaves/hub2 + outer
                << "the 2-hop walk covers every reachable vertex";
        }
        // Unknown start.
        {
            kimix::vector<TraversalVertex> walk;
            kimix::Guid unknown = kimix::Guid(true);
            expect(eq(hubs.graph.traverse_bfs(unknown, {}, EdgeOrientation::outgoing, 10,
                                              1, walk),
                      GdiStatus::error_vertex));
            expect(walk.empty());
        }
    };

    // The §5.10 filter: a filtered-out vertex is not enqueued, so its own
    // neighbourhood is never visited — checked against the brute force.
    // Graph: hub (person, age 50) -> 8 leaves (alternating robot/age 70 and
    // person/age 20); each leaf -> one far vertex carrying the leaf's label
    // and age.
    "traverse_bfs_filter_gates_discovery"_test = [] {
        Graph filtered;
        auto *hub = filtered.create_vertex();
        hub->add_label(kPersonLabel);
        hub->put_property(kAgeName, PropertyValue::make_int64(50));
        size_t robot_leaves = 0;
        for (size_t i = 0; i < 8; ++i) {
            bool const is_robot = (i % 2u) == 0u;
            robot_leaves += is_robot ? 1u : 0u;
            LabelId const leaf_label = is_robot ? kRobotLabel : kPersonLabel;
            int64_t const leaf_age = is_robot ? 70 : 20;
            auto *leaf = filtered.create_vertex();
            leaf->add_label(leaf_label);
            leaf->put_property(kAgeName, PropertyValue::make_int64(leaf_age));
            filtered.connect(hub->guid(), leaf->guid());
            auto *tip = filtered.create_vertex();
            tip->add_label(leaf_label);
            tip->put_property(kAgeName, PropertyValue::make_int64(leaf_age));
            filtered.connect(leaf->guid(), tip->guid());
        }
        // Label filter: only robots pass, so the walk is hub + robot leaves +
        // robot fars — every discovered vertex at any depth is a robot.
        {
            Constraint robots;
            robots.conditions.push_back(
                ConstraintCondition::on_label(kRobotLabel, GdiOp::equal));
            kimix::vector<TraversalVertex> walk;
            expect(eq(filtered.traverse_bfs(hub->guid(), robots,
                                            EdgeOrientation::outgoing, 100, 2, walk),
                      GdiStatus::ok));
            auto expected = brute_force_bfs(filtered, hub->guid(), &robots,
                                            EdgeOrientation::outgoing, 100, 2);
            expect(same_walk(walk, expected))
                << "a label-filtered 2-hop walk matches the brute force";
            expect(eq(walk.size(), size_t{1 + robot_leaves * 2}));
            for (auto const &v : walk) {
                if (v.depth == 0) {
                    continue; // the start is returned unconditionally
                }
                Vertex *vertex = filtered.find(v.guid);
                expect(vertex->has_label(kRobotLabel)) << "only robots discovered";
            }
        }
        // Property filter: age >= 60 keeps the robots (70) and drops the person
        // leaves (20) — and the fars off the dropped leaves are never visited.
        {
            Constraint seniors;
            seniors.conditions.push_back(ConstraintCondition::on_property(
                kAgeName, GdiOp::greater_or_equal, PropertyValue::make_int64(60)));
            kimix::vector<TraversalVertex> walk;
            expect(eq(filtered.traverse_bfs(hub->guid(), seniors,
                                            EdgeOrientation::outgoing, 100, 2, walk),
                      GdiStatus::ok));
            auto expected = brute_force_bfs(filtered, hub->guid(), &seniors,
                                            EdgeOrientation::outgoing, 100, 2);
            expect(same_walk(walk, expected))
                << "a property-filtered 2-hop walk matches the brute force";
            expect(eq(walk.size(), size_t{1 + robot_leaves * 2}))
                << "hub + robot leaves + their fars; the person branch is gated";
        }
    };

    // Argument validation: the error surface is exact.
    "traverse_bfs_argument_errors"_test = [] {
        Graph graph;
        auto dag = gtest::small_dag(graph);
        // A label condition with an ordering op is error_op, validated up front.
        {
            Constraint bad;
            bad.conditions.push_back(
                ConstraintCondition::on_label(kPersonLabel, GdiOp::greater));
            kimix::vector<TraversalVertex> walk;
            expect(eq(graph.traverse_bfs(dag.a->guid(), bad, EdgeOrientation::outgoing,
                                         10, 2, walk),
                      GdiStatus::error_op));
            expect(walk.empty());
        }
        // An unknown START with a bad cap still reports the cap first
        // (validation order: caps, then constraint, then existence).
        {
            kimix::vector<TraversalVertex> walk;
            kimix::Guid unknown = kimix::Guid(true);
            expect(eq(graph.traverse_bfs(unknown, {}, EdgeOrientation::outgoing, 0, 1,
                                         walk),
                      GdiStatus::error_argument));
        }
    };

    // Determinism: the same traversal twice is byte-identical.
    "traverse_bfs_determinism"_test = [] {
        Graph graph;
        auto vertices = gtest::build_layered_dag(graph, 120, 4242u);
        kimix::Guid const start = vertices[7]->guid();
        kimix::vector<TraversalVertex> first;
        kimix::vector<TraversalVertex> second;
        expect(eq(graph.traverse_bfs(start, {}, EdgeOrientation::both, 9, 3, first),
                  GdiStatus::ok));
        expect(eq(graph.traverse_bfs(start, {}, EdgeOrientation::both, 9, 3, second),
                  GdiStatus::ok));
        expect(eq(first.size(), second.size()));
        for (size_t i = 0; i < first.size(); ++i) {
            expect(first[i].guid == second[i].guid && first[i].parent == second[i].parent &&
                   first[i].depth == second[i].depth)
                << "repeated traversals return the identical sequence";
        }
    };

    // The bounded shortest_path: equivalence with the unbounded form, the
    // bound's effect, and the error surface.
    "shortest_path_bounded"_test = [] {
        Graph graph;
        auto dag = gtest::small_dag(graph);
        // A bound at the true distance returns the same path as the unbounded form.
        {
            auto unbounded = graph.shortest_path(dag.a->guid(), dag.d->guid());
            expect(ge(unbounded.size(), size_t{2}));
            kimix::vector<kimix::Guid> bounded;
            expect(eq(graph.shortest_path(dag.a->guid(), dag.d->guid(),
                                          static_cast<uint32_t>(unbounded.size() - 1),
                                          bounded),
                      GdiStatus::ok));
            expect(eq(bounded.size(), unbounded.size()));
            for (size_t i = 0; i < bounded.size(); ++i) {
                expect(bounded[i] == unbounded[i]) << "bounded == unbounded within the bound";
            }
        }
        // A bound one below the true distance finds nothing (ok, empty).
        {
            auto unbounded = graph.shortest_path(dag.a->guid(), dag.d->guid());
            kimix::vector<kimix::Guid> bounded;
            expect(eq(graph.shortest_path(dag.a->guid(), dag.d->guid(),
                                          static_cast<uint32_t>(unbounded.size() - 2),
                                          bounded),
                      GdiStatus::ok));
            expect(bounded.empty()) << "beyond the bound is an empty result, not an error";
        }
        // from == to on an existing vertex is {from}; on an unknown one it is
        // error_vertex.
        {
            kimix::vector<kimix::Guid> path;
            expect(eq(graph.shortest_path(dag.b->guid(), dag.b->guid(), 3, path),
                      GdiStatus::ok));
            expect(eq(path.size(), size_t{1}));
            kimix::Guid unknown = kimix::Guid(true);
            expect(eq(graph.shortest_path(unknown, unknown, 3, path),
                      GdiStatus::error_vertex));
            expect(eq(graph.shortest_path(dag.a->guid(), unknown, 3, path),
                      GdiStatus::error_vertex));
        }
        // Depth bound validation.
        {
            kimix::vector<kimix::Guid> path;
            expect(eq(graph.shortest_path(dag.a->guid(), dag.d->guid(), 0, path),
                      GdiStatus::error_argument));
            expect(eq(graph.shortest_path(dag.a->guid(), dag.d->guid(),
                                          Graph::kMaxNativeHopDepth + 1, path),
                      GdiStatus::error_argument));
        }
        // On a cyclic graph the bounded form still matches the unbounded one.
        {
            Graph ring_graph;
            kimix::vector<Vertex *> ring;
            build_ring(ring_graph, 16, ring);
            auto unbounded = ring_graph.shortest_path(ring[0]->guid(), ring[7]->guid());
            kimix::vector<kimix::Guid> bounded;
            expect(eq(ring_graph.shortest_path(ring[0]->guid(), ring[7]->guid(),
                                               Graph::kMaxNativeHopDepth, bounded),
                      GdiStatus::ok));
            expect(eq(bounded.size(), unbounded.size()));
            for (size_t i = 0; i < bounded.size(); ++i) {
                expect(bounded[i] == unbounded[i]) << "cyclic graph: bounded == unbounded";
            }
        }
    };

    // -----------------------------------------------------------------------
    // EDGE-filtered shortest path (the edge-searching form): the walk keeps a hop
    // only when the EDGE it steps over matches, so the filter selects a ROUTE,
    // not a set of vertices. Equivalence against the brute force over the
    // constraint x orientation x depth matrix, then the contract details.
    // -----------------------------------------------------------------------
    "edge_filtered_shortest_path_matches_brute_force"_test = [] {
        Graph graph;
        auto dag = gtest::small_dag(graph);
        // Label every edge by its ORIGIN: b's and f's edges are `blocked`, the
        // rest `open`, and each carries a `cost` (blocked 9.0, open 1.0) so a
        // label filter and a property filter select the same routes from
        // different conditions.
        for (auto const &id : graph.edge_ids()) {
            EdgeRecord record;
            expect(graph.edge_record(id, record));
            bool const blocked = record.origin == dag.b->guid() || record.origin == dag.f->guid();
            expect(eq(graph.edge_add_label(id, blocked ? kBlockedEdgeLabel : kOpenEdgeLabel),
                      GdiStatus::ok));
            expect(eq(graph.edge_put_property(id, kCostName,
                                              PropertyValue::make_double(blocked ? 9.0 : 1.0)),
                      GdiStatus::ok));
        }
        kimix::vector<Constraint> constraints;
        constraints.push_back(Constraint{}); // no filter: every edge
        constraints.push_back(
            Constraint{}.with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::equal)));
        constraints.push_back(
            Constraint{}.with(ConstraintCondition::on_label(kBlockedEdgeLabel, GdiOp::equal)));
        constraints.push_back(Constraint{}.with(ConstraintCondition::on_property(
            kCostName, GdiOp::less_or_equal, PropertyValue::make_double(5.0))));
        constraints.push_back(Constraint{}.with(ConstraintCondition::on_property(
            kCostName, GdiOp::greater, PropertyValue::make_double(5.0))));
        constraints.push_back(
            Constraint{}
                .with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::equal))
                .with(ConstraintCondition::on_property(
                    kCostName, GdiOp::greater, PropertyValue::make_double(5.0))));
        EdgeOrientation const orientations[] = {EdgeOrientation::outgoing,
                                                EdgeOrientation::incoming,
                                                EdgeOrientation::both};
        uint32_t const depths[] = {1u, 2u, 3u, Graph::kMaxNativeHopDepth};
        size_t compared = 0;
        for (auto const &constraint : constraints) {
            for (EdgeOrientation orientation : orientations) {
                for (uint32_t depth : depths) {
                    for (size_t s = 0; s < dag.vertices.size(); ++s) {
                        for (size_t t = 0; t < dag.vertices.size(); ++t) {
                            kimix::Guid const from = dag.vertices[s]->guid();
                            kimix::Guid const to = dag.vertices[t]->guid();
                            kimix::vector<PathStep> actual;
                            expect(eq(graph.shortest_path(from, to, constraint, orientation,
                                                          depth, actual),
                                      GdiStatus::ok));
                            RefPath const expected = ref_filtered_path(
                                graph, from, to, constraint, orientation, depth);
                            expect_same_path(actual, expected,
                                             "edge-filtered path matches the brute force");
                            if (!actual.empty()) {
                                expect_path_is_sound(graph, actual, constraint, orientation, from,
                                                     to, "every reported hop is real");
                            }
                            ++compared;
                        }
                    }
                }
            }
        }
        expect(gt(compared, size_t{1000})) << "the matrix really ran";
    };

    // The same equivalence at scale: a 120-vertex layered DAG with every edge
    // labeled and costed, and random endpoint / orientation / depth / filter
    // draws. A DAG is the shape that makes "no matching chain" the common answer,
    // which is exactly the case a length-only comparison would hide.
    "edge_filtered_shortest_path_layered"_test = [] {
        Graph graph;
        auto vertices = gtest::build_layered_dag(graph, 120, 4242u);
        // Label by the POSITIONS of the endpoints, not by edge_ids() order: that
        // order is shard order (a hash of a random guid), so keying off it would
        // make every run filter a different subset. This keeps the fixture
        // reproducible while still spreading the labels unevenly over the graph.
        kimix::unordered_map<kimix::Guid, size_t> rank;
        rank.reserve(vertices.size());
        for (size_t i = 0; i < vertices.size(); ++i) {
            rank.emplace(vertices[i]->guid(), i);
        }
        for (auto const &id : graph.edge_ids()) {
            EdgeRecord record;
            expect(graph.edge_record(id, record));
            bool const blocked =
                ((rank.find(record.origin)->second + rank.find(record.target)->second) % 3u) ==
                0u;
            expect(eq(graph.edge_add_label(id, blocked ? kBlockedEdgeLabel : kOpenEdgeLabel),
                      GdiStatus::ok));
            expect(eq(graph.edge_put_property(id, kCostName,
                                              PropertyValue::make_double(blocked ? 9.0 : 1.0)),
                      GdiStatus::ok));
        }
        kimix::vector<Constraint> constraints;
        constraints.push_back(Constraint{});
        constraints.push_back(
            Constraint{}.with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::equal)));
        constraints.push_back(
            Constraint{}.with(ConstraintCondition::on_label(kBlockedEdgeLabel, GdiOp::equal)));
        constraints.push_back(Constraint{}.with(ConstraintCondition::on_property(
            kCostName, GdiOp::less, PropertyValue::make_double(5.0))));
        std::mt19937 rng(17u);
        size_t found = 0;
        for (int round = 0; round < 160; ++round) {
            kimix::Guid const from = vertices[rng() % vertices.size()]->guid();
            kimix::Guid const to = vertices[rng() % vertices.size()]->guid();
            Constraint const &constraint = constraints[rng() % constraints.size()];
            EdgeOrientation const orientation =
                static_cast<EdgeOrientation>(rng() % 3u);
            uint32_t const depth = 1 + static_cast<uint32_t>(rng() % 6);
            kimix::vector<PathStep> actual;
            expect(eq(graph.shortest_path(from, to, constraint, orientation, depth, actual),
                      GdiStatus::ok));
            expect_same_path(actual,
                             ref_filtered_path(graph, from, to, constraint, orientation, depth),
                             "layered DAG: the edge-filtered path matches the brute force");
            if (!actual.empty()) {
                ++found;
                expect_path_is_sound(graph, actual, constraint, orientation, from, to,
                                     "every reported hop is real");
            }
        }
        expect(gt(found, size_t{0})) << "the draws did produce reachable pairs";
    };

    // The contract: which edge a hop reports (parallel edges included), the
    // zero-length path, the unreachable answer and the error surface.
    "edge_filtered_shortest_path_contract"_test = [] {
        Graph graph;
        auto dag = gtest::small_dag(graph);
        // a -> b already exists (connect); add a PARALLEL a -> b that only the
        // filter can pick, which is the case a vertex-only path cannot express.
        EdgeId const first = [&] {
            kimix::vector<EdgeId> ids;
            graph.incident_edges(dag.a->guid(), EdgeOrientation::outgoing, ids);
            expect(eq(ids.size(), size_t{2})); // a -> b, a -> c
            return ids[0];
        }();
        EdgeId parallel;
        LabelId const open_labels[]{kOpenEdgeLabel};
        expect(eq(graph.edge_create(dag.a->guid(), dag.b->guid(),
                                    graph_db::EdgeDirectionType::directed,
                                    kimix::span<const LabelId>{open_labels, 1}, nullptr, nullptr, parallel),
                  GdiStatus::ok));
        expect(parallel != first) << "a parallel edge is its own id";
        expect(eq(graph.edge_add_label(first, kBlockedEdgeLabel), GdiStatus::ok));

        // Unfiltered: the FIRST a -> b edge in link order carries the hop.
        {
            kimix::vector<PathStep> path;
            expect(eq(graph.shortest_path(dag.a->guid(), dag.b->guid(), Constraint{},
                                          EdgeOrientation::outgoing, 4, path),
                      GdiStatus::ok));
            expect(eq(path.size(), size_t{2}));
            expect(path[1].edge == first) << "the unfiltered walk reports the first parallel edge";
        }
        // Refusing that edge reroutes the SAME hop onto the parallel one.
        {
            Constraint open_only =
                Constraint{}.with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::equal));
            kimix::vector<PathStep> path;
            expect(eq(graph.shortest_path(dag.a->guid(), dag.b->guid(), open_only,
                                          EdgeOrientation::outgoing, 4, path),
                      GdiStatus::ok));
            expect(eq(path.size(), size_t{2}));
            expect(path[1].edge == parallel)
                << "the filter selects WHICH parallel edge carried the hop";
        }
        // A zero-length path traverses no edge, so no edge filter can exclude it.
        {
            Constraint blocked =
                Constraint{}.with(ConstraintCondition::on_label(kBlockedEdgeLabel, GdiOp::equal));
            kimix::vector<PathStep> path;
            expect(eq(graph.shortest_path(dag.b->guid(), dag.b->guid(), blocked,
                                          EdgeOrientation::outgoing, 3, path),
                      GdiStatus::ok));
            expect(eq(path.size(), size_t{1}));
            expect(!static_cast<bool>(path[0].edge)) << "the start is reached by no edge";
        }
        // Unreachable within the bound, and unreachable over the matched edges:
        // both are an empty result with status ok (GDI's nullable-list answer).
        {
            Constraint blocked =
                Constraint{}.with(ConstraintCondition::on_label(kBlockedEdgeLabel, GdiOp::equal));
            kimix::vector<PathStep> path;
            expect(eq(graph.shortest_path(dag.a->guid(), dag.d->guid(), Constraint{},
                                          EdgeOrientation::outgoing, 1, path),
                      GdiStatus::ok));
            expect(path.empty()) << "a -> d needs two hops, the bound is one";
            expect(eq(graph.shortest_path(dag.a->guid(), dag.d->guid(), blocked,
                                          EdgeOrientation::outgoing, 8, path),
                      GdiStatus::ok));
            expect(path.empty())
                << "a -> b is blocked, but b -> d carries no label, so d stays unreachable";
        }
        // Error surface: bound, unknown endpoints, and the label-op rule.
        {
            kimix::vector<PathStep> path;
            kimix::Guid const unknown(true);
            Constraint const none;
            expect(eq(graph.shortest_path(dag.a->guid(), dag.d->guid(), none,
                                          EdgeOrientation::outgoing, 0, path),
                      GdiStatus::error_argument));
            expect(eq(graph.shortest_path(dag.a->guid(), dag.d->guid(), none,
                                          EdgeOrientation::outgoing,
                                          Graph::kMaxNativeHopDepth + 1, path),
                      GdiStatus::error_argument));
            expect(eq(graph.shortest_path(unknown, dag.d->guid(), none, EdgeOrientation::outgoing,
                                          4, path),
                      GdiStatus::error_vertex));
            expect(eq(graph.shortest_path(dag.a->guid(), unknown, none, EdgeOrientation::outgoing,
                                          4, path),
                      GdiStatus::error_vertex));
            Constraint ordering =
                Constraint{}.with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::greater));
            expect(eq(graph.shortest_path(dag.a->guid(), dag.d->guid(), ordering,
                                          EdgeOrientation::outgoing, 4, path),
                      GdiStatus::error_op))
                << "a label condition can only test membership";
        }
        // A self loop is a hop in both orientations but never shortens a path.
        {
            Vertex *loop = graph.create_vertex();
            kimix::vector<EdgeId> ids;
            graph.incident_edges(loop->guid(), EdgeOrientation::both, ids);
            expect(ids.empty());
            EdgeId loop_edge;
            LabelId const open_labels2[]{kOpenEdgeLabel};
            expect(eq(graph.edge_create(loop->guid(), loop->guid(),
                                        graph_db::EdgeDirectionType::directed,
                                        kimix::span<const LabelId>{open_labels2, 1}, nullptr, nullptr,
                                        loop_edge),
                      GdiStatus::ok));
            kimix::vector<PathStep> path;
            Constraint open_only =
                Constraint{}.with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::equal));
            expect(eq(graph.shortest_path(loop->guid(), loop->guid(), open_only,
                                          EdgeOrientation::outgoing, 4, path),
                      GdiStatus::ok));
            expect(eq(path.size(), size_t{1})) << "from == to stays the zero-length path";
        }
    };

    // The two primitives the walk is built on, plus the edge-filtered
    // reachability probe.
    "edge_filtered_primitives"_test = [] {
        Graph graph;
        auto dag = gtest::small_dag(graph);
        // oriented_links: (edge, neighbour) pairs, one shard snapshot per call.
        kimix::vector<EdgeLink> links;
        expect(graph.oriented_links(dag.a->guid(), EdgeOrientation::outgoing, links));
        expect(eq(links.size(), size_t{2}));
        expect(links[0].neighbour == dag.b->guid()) << "insertion order: a -> b first";
        expect(links[1].neighbour == dag.c->guid());
        EdgeRecord record;
        expect(graph.edge_record(links[0].edge, record));
        expect(record.origin == dag.a->guid() && record.target == dag.b->guid())
            << "the link carries the EDGE that makes the hop";
        expect(graph.oriented_links(dag.a->guid(), EdgeOrientation::incoming, links));
        expect(links.empty()) << "a has no incoming edge";
        expect(graph.oriented_links(dag.d->guid(), EdgeOrientation::both, links));
        expect(eq(links.size(), size_t{3})) << "b -> d, c -> d and f -> d are all incident";
        kimix::Guid const unknown(true);
        expect(!graph.oriented_links(unknown, EdgeOrientation::both, links));
        expect(links.empty()) << "a failed read leaves the buffer empty, not stale";
        // edge_matches: an empty constraint is "the edge exists", a label filter
        // reads the EDGE's own payload.
        kimix::vector<EdgeId> a_out;
        graph.incident_edges(dag.a->guid(), EdgeOrientation::outgoing, a_out);
        expect(eq(graph.edge_add_label(a_out[1], kBlockedEdgeLabel), GdiStatus::ok));
        expect(graph.edge_matches(a_out[0], Constraint{}));
        expect(!graph.edge_matches(unknown, Constraint{}))
            << "an unknown edge does not match, not even the empty constraint";
        Constraint blocked =
            Constraint{}.with(ConstraintCondition::on_label(kBlockedEdgeLabel, GdiOp::equal));
        expect(graph.edge_matches(a_out[1], blocked));
        expect(!graph.edge_matches(a_out[0], blocked));
        // has_path: the reachability probe honours the same filter.
        expect(graph.has_path(dag.a->guid(), dag.d->guid(), Constraint{},
                              EdgeOrientation::outgoing))
            << "a -> b -> d exists over every edge";
        expect(!graph.has_path(dag.a->guid(), dag.d->guid(), blocked, EdgeOrientation::outgoing))
            << "a -> c matches but c -> d does not, so d is unreachable over blocked edges";
        expect(graph.has_path(dag.a->guid(), dag.c->guid(), blocked, EdgeOrientation::outgoing))
            << "the blocked edge itself is reachable";
        expect(!graph.has_path(unknown, dag.d->guid(), Constraint{}, EdgeOrientation::outgoing))
            << "an unknown endpoint is unreachable, not an error";
        expect(graph.has_path(dag.b->guid(), dag.b->guid(), blocked, EdgeOrientation::outgoing))
            << "from == to on an existing vertex is a zero-length path";
        expect(!graph.has_path(unknown, unknown, Constraint{}, EdgeOrientation::outgoing));
    };

    // -----------------------------------------------------------------------
    // The BATCHED edge filter (a visit of kWideVisitLinks >= kEdgeFilterMinLinks
    // links): the walk filters a visit's tail out of order — one lock per touched
    // edge shard — but must still CONSUME the candidates in original link order, so
    // the first matching parallel edge carries the hop exactly as the per-link path
    // would. These are the suite's only wide visits, and they place the interesting
    // links inside a drained chunk on purpose (the bench's F4 hub, the only other
    // wide visit in the tree, has no parallel edges at all).
    // -----------------------------------------------------------------------
    "edge_filtered_wide_visit_first_match_wins"_test = [] {
        Graph graph;
        Vertex *start = graph.create_vertex();
        Vertex *target = graph.create_vertex();
        EdgeId first_link{};  // link index 130: open AND blocked
        EdgeId second_link{}; // link index 131: open only
        size_t made = 0;
        for (size_t i = 0; i < kWideVisitLinks; ++i) {
            if (i == kWideVisitPairAt) {
                // The two parallel links to the SAME target are ADJACENT candidates
                // of one drained chunk, so the batch's verdict array and its consume
                // loop — not the snapshot — decide which edge the hop reports.
                LabelId const labels[] = {kOpenEdgeLabel, kBlockedEdgeLabel};
                if (wide_link(graph, start->guid(), target->guid(),
                              kimix::span<const LabelId>{labels, 2}, first_link)) {
                    ++made;
                }
                continue;
            }
            if (i == kWideVisitPairAt + 1) {
                LabelId const labels[] = {kOpenEdgeLabel};
                if (wide_link(graph, start->guid(), target->guid(),
                              kimix::span<const LabelId>{labels, 1}, second_link)) {
                    ++made;
                }
                continue;
            }
            Vertex *decoy = graph.create_vertex();
            LabelId const labels[] = {kDecoyEdgeLabel};
            EdgeId unused{};
            if (wide_link(graph, start->guid(), decoy->guid(),
                          kimix::span<const LabelId>{labels, 1}, unused)) {
                ++made;
            }
        }
        expect(eq(made, kWideVisitLinks)) << "the wide visit fixture built every link";
        expect(first_link != second_link) << "a parallel edge is its own id";
        Constraint const open_only =
            Constraint{}.with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::equal));
        // Both parallel links match: the FIRST in link order wins.
        kimix::vector<PathStep> path;
        expect(eq(graph.shortest_path(start->guid(), target->guid(), open_only,
                                      EdgeOrientation::outgoing, 1, path),
                  GdiStatus::ok));
        expect(eq(path.size(), size_t{2})) << "the fixture is flat: one hop reaches the target";
        if (path.size() == 2) {
            expect(path[1].vertex == target->guid());
            expect(path[1].edge == first_link)
                << "the batched drain reports the FIRST matching parallel edge";
        }
        expect_path_is_sound(graph, path, open_only, EdgeOrientation::outgoing, start->guid(),
                             target->guid(), "the wide visit's hop is a real, matching edge");
        // Refusing the FIRST of the pair reroutes the same hop onto the second: a
        // refused candidate does not consume the neighbour.
        Constraint const open_and_not_blocked =
            Constraint{}
                .with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::equal))
                .with(ConstraintCondition::on_label(kBlockedEdgeLabel, GdiOp::not_equal));
        kimix::vector<PathStep> rerouted;
        expect(eq(graph.shortest_path(start->guid(), target->guid(), open_and_not_blocked,
                                      EdgeOrientation::outgoing, 1, rerouted),
                  GdiStatus::ok));
        expect(eq(rerouted.size(), size_t{2}));
        if (rerouted.size() == 2) {
            expect(rerouted[1].edge == second_link)
                << "the second parallel edge carries the hop once the first is refused";
        }
        // The same two parallel links in a visit NARROWER than kEdgeFilterMinLinks
        // take the per-link path; it must report the same edge, so the batch is a
        // cost change and never a rule change. (The unfiltered form cannot be the
        // reference here: it reports vertices, not the edge each hop took.)
        {
            Graph narrow;
            Vertex *narrow_start = narrow.create_vertex();
            Vertex *narrow_target = narrow.create_vertex();
            EdgeId narrow_first{};
            EdgeId narrow_second{};
            LabelId const both_labels[] = {kOpenEdgeLabel, kBlockedEdgeLabel};
            LabelId const open_label[] = {kOpenEdgeLabel};
            expect(wide_link(narrow, narrow_start->guid(), narrow_target->guid(),
                             kimix::span<const LabelId>{both_labels, 2}, narrow_first));
            expect(wide_link(narrow, narrow_start->guid(), narrow_target->guid(),
                             kimix::span<const LabelId>{open_label, 1}, narrow_second));
            kimix::vector<PathStep> narrow_path;
            expect(eq(narrow.shortest_path(narrow_start->guid(), narrow_target->guid(),
                                           open_only, EdgeOrientation::outgoing, 1, narrow_path),
                      GdiStatus::ok));
            expect(eq(narrow_path.size(), size_t{2}));
            if (narrow_path.size() == 2) {
                expect(narrow_path[1].edge == narrow_first)
                    << "the per-link path picks the first parallel edge, like the batch";
            }
        }
    };

    // A DUPLICATE neighbour inside ONE drained chunk (two links of the same chunk
    // pointing at the same vertex), next to a rival vertex that reaches the target
    // too: the consume-time visited check must drop the duplicate, so the path's
    // second step is the FIRST of the two links and the third step comes from the
    // duplicated vertex, not from the rival discovered a candidate later.
    "edge_filtered_wide_visit_duplicate_neighbour_skipped"_test = [] {
        Graph graph;
        Vertex *start = graph.create_vertex();
        Vertex *shared = graph.create_vertex(); // the duplicated neighbour
        Vertex *rival = graph.create_vertex();  // discovered AFTER `shared`, same chunk
        Vertex *target = graph.create_vertex();
        EdgeId shared_to_target{};
        EdgeId rival_to_target{};
        LabelId const open_labels[] = {kOpenEdgeLabel};
        expect(wide_link(graph, shared->guid(), target->guid(),
                         kimix::span<const LabelId>{open_labels, 1}, shared_to_target))
            << "the duplicated neighbour reaches the target";
        expect(wide_link(graph, rival->guid(), target->guid(),
                         kimix::span<const LabelId>{open_labels, 1}, rival_to_target))
            << "the rival reaches the target as well, so discovery order decides";
        EdgeId first_of_pair{};
        EdgeId second_of_pair{};
        EdgeId start_to_rival{};
        size_t made = 0;
        for (size_t i = 0; i < kWideVisitLinks; ++i) {
            if (i == kWideVisitPairAt) {
                if (wide_link(graph, start->guid(), shared->guid(),
                              kimix::span<const LabelId>{open_labels, 1}, first_of_pair)) {
                    ++made;
                }
                continue;
            }
            if (i == kWideVisitPairAt + 1) {
                // The duplicate: the same neighbour, later in the SAME chunk.
                if (wide_link(graph, start->guid(), shared->guid(),
                              kimix::span<const LabelId>{open_labels, 1}, second_of_pair)) {
                    ++made;
                }
                continue;
            }
            if (i == kWideVisitPairAt + 2) {
                if (wide_link(graph, start->guid(), rival->guid(),
                              kimix::span<const LabelId>{open_labels, 1}, start_to_rival)) {
                    ++made;
                }
                continue;
            }
            Vertex *decoy = graph.create_vertex();
            LabelId const decoy_labels[] = {kDecoyEdgeLabel};
            EdgeId unused{};
            if (wide_link(graph, start->guid(), decoy->guid(),
                          kimix::span<const LabelId>{decoy_labels, 1}, unused)) {
                ++made;
            }
        }
        expect(eq(made, kWideVisitLinks)) << "the duplicate-neighbour fixture built every link";
        expect(first_of_pair != second_of_pair)
            << "the duplicate neighbour is reached by two distinct edges";
        Constraint const open_only =
            Constraint{}.with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::equal));
        kimix::vector<PathStep> path;
        expect(eq(graph.shortest_path(start->guid(), target->guid(), open_only,
                                      EdgeOrientation::outgoing, 4, path),
                  GdiStatus::ok));
        expect(eq(path.size(), size_t{3}))
            << "start -> the duplicated neighbour -> target: three steps, not four";
        if (path.size() == 3) {
            expect(path[1].vertex == shared->guid())
                << "the first of the two duplicate links discovered the neighbour";
            expect(path[1].edge == first_of_pair)
                << "and that first link is the hop the path reports";
            expect(path[2].vertex == target->guid());
            expect(path[2].edge == shared_to_target)
                << "the target is reached from the duplicated neighbour, not from the rival";
        }
        expect_path_is_sound(graph, path, open_only, EdgeOrientation::outgoing, start->guid(),
                             target->guid(), "every hop of the duplicate-neighbour path is real");
        // The same answer as the brute force, which dedups by construction and keeps
        // the first matching link of a neighbour.
        expect_same_path(path,
                         ref_filtered_path(graph, start->guid(), target->guid(), open_only,
                                           EdgeOrientation::outgoing, 4),
                         "the duplicate neighbour collapses exactly like the reference");
        expect(graph.has_path(start->guid(), target->guid(), open_only,
                              EdgeOrientation::outgoing))
            << "reachability agrees with the path form on the same fixture";
        // A second shape of the same duplicate, this time with the duplicated
        // neighbour leading NOWHERE: the answer must then come from the rival, so a
        // duplicate the drain re-enqueued can no longer be hidden behind an early hit
        // — which is what makes the visit count below a proof rather than a guess.
        Graph nowhere;
        Vertex *n_start = nowhere.create_vertex();
        Vertex *n_shared = nowhere.create_vertex();
        Vertex *n_rival = nowhere.create_vertex();
        Vertex *n_target = nowhere.create_vertex();
        EdgeId n_rival_to_target{};
        expect(wide_link(nowhere, n_rival->guid(), n_target->guid(),
                         kimix::span<const LabelId>{open_labels, 1}, n_rival_to_target))
            << "the rival reaches the target in the second fixture too";
        EdgeId n_first_of_pair{};
        EdgeId n_second_of_pair{};
        EdgeId n_start_to_rival{};
        size_t n_made = 0;
        for (size_t i = 0; i < kWideVisitLinks; ++i) {
            if (i == kWideVisitPairAt) {
                if (wide_link(nowhere, n_start->guid(), n_shared->guid(),
                              kimix::span<const LabelId>{open_labels, 1}, n_first_of_pair)) {
                    ++n_made;
                }
                continue;
            }
            if (i == kWideVisitPairAt + 1) {
                if (wide_link(nowhere, n_start->guid(), n_shared->guid(),
                              kimix::span<const LabelId>{open_labels, 1}, n_second_of_pair)) {
                    ++n_made;
                }
                continue;
            }
            if (i == kWideVisitPairAt + 2) {
                if (wide_link(nowhere, n_start->guid(), n_rival->guid(),
                              kimix::span<const LabelId>{open_labels, 1}, n_start_to_rival)) {
                    ++n_made;
                }
                continue;
            }
            Vertex *decoy = nowhere.create_vertex();
            LabelId const decoy_labels[] = {kDecoyEdgeLabel};
            EdgeId unused{};
            if (wide_link(nowhere, n_start->guid(), decoy->guid(),
                          kimix::span<const LabelId>{decoy_labels, 1}, unused)) {
                ++n_made;
            }
        }
        expect(eq(n_made, kWideVisitLinks)) << "the nowhere fixture built every link";
        kimix::vector<PathStep> nowhere_path;
        expect(eq(nowhere.shortest_path(n_start->guid(), n_target->guid(), open_only,
                                        EdgeOrientation::outgoing, 4, nowhere_path),
                  GdiStatus::ok));
        expect(eq(nowhere_path.size(), size_t{3}))
            << "start -> the rival -> target: the duplicated neighbour leads nowhere";
        if (nowhere_path.size() == 3) {
            expect(nowhere_path[1].vertex == n_rival->guid())
                << "the rival carries the hop because the duplicated neighbour cannot";
            expect(nowhere_path[1].edge == n_start_to_rival);
            expect(nowhere_path[2].edge == n_rival_to_target);
        }
#if defined(KIMIX_GRAPH_DB_WALK_COUNTERS)
        // The consume-time visited check, made OBSERVABLE (only ever compiled in the
        // instrumented build; the answers above are configuration-independent).
        // In the FIRST fixture both candidates reach the target, so a duplicate the
        // drain re-enqueued would add a node the early hit then skips — invisible. In
        // the `nowhere` shape the duplicated neighbour leads nowhere, so every
        // duplicate enqueued costs one extra VERTEX SNAPSHOT at the next level:
        // exactly three visits (the start, the duplicated neighbour, the rival) prove
        // the duplicate was dropped where it is consumed, and a missing check reads 4.
        graph_db::reset_walk_counters();
        kimix::vector<PathStep> counted;
        expect(eq(nowhere.shortest_path(n_start->guid(), n_target->guid(), open_only,
                                        EdgeOrientation::outgoing, 4, counted),
                  GdiStatus::ok));
        expect(eq(graph_db::walk_counters().vertex_locks, uint64_t{3}))
            << "the duplicated neighbour was visited once: start, it, and the rival";
#endif
    };

    // -----------------------------------------------------------------------
    // The O6 split (E3): has_path is the REACHABILITY mode of the same walk, so its
    // bool must equal "the path walk found something" at the same bound. E3 left
    // that equivalence guarded only by hand-written cases; these seeded draws cover
    // endpoint x constraint x orientation, and compare both forms against the brute
    // force as well.
    // -----------------------------------------------------------------------
    "edge_filtered_has_path_agrees_with_the_path_walk"_test = [] {
        Graph graph;
        auto vertices = gtest::build_layered_dag(graph, 120, 9091u);
        // Label by POSITION (not by edge_ids() order, which is a hash order): the
        // same scheme the layered path test uses, so the filters select a stable
        // subset and the fixture is reproducible.
        kimix::unordered_map<kimix::Guid, size_t> rank;
        rank.reserve(vertices.size());
        for (size_t i = 0; i < vertices.size(); ++i) {
            rank.emplace(vertices[i]->guid(), i);
        }
        for (auto const &id : graph.edge_ids()) {
            EdgeRecord record;
            expect(graph.edge_record(id, record));
            bool const blocked =
                ((rank.find(record.origin)->second + rank.find(record.target)->second) % 3u) ==
                0u;
            expect(eq(graph.edge_add_label(id, blocked ? kBlockedEdgeLabel : kOpenEdgeLabel),
                      GdiStatus::ok));
            expect(eq(graph.edge_put_property(id, kCostName,
                                              PropertyValue::make_double(blocked ? 9.0 : 1.0)),
                      GdiStatus::ok));
        }
        kimix::vector<Constraint> constraints;
        constraints.push_back(Constraint{}); // no filter: every edge
        constraints.push_back(
            Constraint{}.with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::equal)));
        constraints.push_back(
            Constraint{}.with(ConstraintCondition::on_label(kBlockedEdgeLabel, GdiOp::equal)));
        constraints.push_back(Constraint{}.with(ConstraintCondition::on_property(
            kCostName, GdiOp::less, PropertyValue::make_double(5.0))));
        std::mt19937 rng(4711u);
        size_t reachable_count = 0;
        size_t unreachable_count = 0;
        size_t bounded_count = 0;
        for (int round = 0; round < 120; ++round) {
            kimix::Guid const from = vertices[rng() % vertices.size()]->guid();
            kimix::Guid const to = vertices[rng() % vertices.size()]->guid();
            Constraint const &constraint = constraints[rng() % constraints.size()];
            EdgeOrientation const orientation = static_cast<EdgeOrientation>(rng() % 3u);
            bool const reachable = graph.has_path(from, to, constraint, orientation);
            RefPath const at_the_bound =
                ref_filtered_path(graph, from, to, constraint, orientation,
                                  Graph::kMaxNativeHopDepth);
            expect(eq(reachable, !at_the_bound.vertices.empty()))
                << "has_path agrees with the brute force at the reachability bound";
            kimix::vector<PathStep> full;
            expect(eq(graph.shortest_path(from, to, constraint, orientation,
                                          Graph::kMaxNativeHopDepth, full),
                      GdiStatus::ok));
            expect(eq(reachable, !full.empty()))
                << "has_path == !shortest_path(..., path).empty() at the same bound";
            expect_same_path(full, at_the_bound,
                             "the full-bound path matches the brute force");
            // A tighter bound can only REMOVE paths: a non-empty one still implies
            // reachability, and the tighter answer matches its own reference.
            uint32_t const depth = 1 + static_cast<uint32_t>(rng() % 6);
            kimix::vector<PathStep> tight;
            expect(eq(graph.shortest_path(from, to, constraint, orientation, depth, tight),
                      GdiStatus::ok));
            expect_same_path(tight,
                             ref_filtered_path(graph, from, to, constraint, orientation, depth),
                             "the tighter bound's path matches the brute force");
            if (!tight.empty()) {
                expect(reachable) << "a path inside the tighter bound proves reachability";
                ++bounded_count;
            }
            if (reachable) {
                ++reachable_count;
            } else {
                ++unreachable_count;
            }
        }
        expect(gt(reachable_count, size_t{0})) << "the draws produced reachable pairs";
        expect(gt(unreachable_count, size_t{0})) << "the draws produced unreachable pairs";
        expect(gt(bounded_count, size_t{0})) << "the draws produced paths inside the tighter bound";
    };

    // Database facade: the edge filter is a registered constraint like every
    // other read filter, so the facade must resolve it, forward the same status
    // codes and agree with the raw Graph API.
    // Database facade: the edge filter is a registered constraint like every
    // other read filter, so the facade must resolve it, forward the same status
    // codes and agree with the raw Graph API.
    "database_edge_filtered_shortest_path"_test = [] {
        Database db;
        auto dag = gtest::small_dag(db.graph());
        // Every edge gets a label, and the b -> d hinge gets the blocking one, so
        // the two-hop route a -> c -> d is the only one an open-only filter
        // leaves (a -> b would be unlabeled and therefore unmatched).
        for (auto const &id : db.graph().edge_ids()) {
            EdgeRecord record;
            expect(db.graph().edge_record(id, record));
            bool const hinge = record.origin == dag.b->guid() && record.target == dag.d->guid();
            expect(eq(db.graph().edge_add_label(
                          id, hinge ? kBlockedEdgeLabel : kOpenEdgeLabel),
                      GdiStatus::ok));
        }
        Constraint const open_only =
            Constraint{}.with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::equal));
        kimix::vector<PathStep> via_facade;
        expect(eq(db.shortest_path(dag.a->guid(), dag.d->guid(), open_only,
                                   EdgeOrientation::outgoing, 8, via_facade),
                  GdiStatus::ok));
        expect(eq(via_facade.size(), size_t{3})) << "a -> c -> d";
        if (via_facade.size() == 3) {
            expect(via_facade[1].vertex == dag.c->guid() && via_facade[2].vertex == dag.d->guid())
                << "the blocked hinge is routed around";
            expect(via_facade[1].edge != via_facade[2].edge)
                << "the two hops are two distinct edges";
        }
        // The facade agrees with the raw Graph API on the same definition.
        kimix::vector<PathStep> via_graph;
        expect(eq(db.graph().shortest_path(dag.a->guid(), dag.d->guid(), open_only,
                                           EdgeOrientation::outgoing, 8, via_graph),
                  GdiStatus::ok));
        expect(eq(via_graph.size(), via_facade.size()));
        for (size_t i = 0; i < via_graph.size() && i < via_facade.size(); ++i) {
            expect(via_graph[i].vertex == via_facade[i].vertex &&
                   via_graph[i].edge == via_facade[i].edge)
                << "the facade forwards the Graph result verbatim";
        }
        // A registered HANDLE resolves to the same definition (the C ABI shape).
        graph_db::ConstraintHandle handle{};
        expect(eq(db.create_constraint(open_only, handle), GdiStatus::ok));
        Constraint resolved;
        expect(db.schema().constraint(handle, resolved));
        kimix::vector<PathStep> via_handle;
        expect(eq(db.shortest_path(dag.a->guid(), dag.d->guid(), resolved,
                                   EdgeOrientation::outgoing, 8, via_handle),
                  GdiStatus::ok));
        expect(eq(via_handle.size(), via_facade.size()));
        // The facade validates the definition BEFORE walking (_resolve_constraint).
        Constraint const ordering =
            Constraint{}.with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::less));
        kimix::vector<PathStep> rejected;
        expect(eq(db.shortest_path(dag.a->guid(), dag.d->guid(), ordering,
                                   EdgeOrientation::outgoing, 8, rejected),
                  GdiStatus::error_op));
        expect(rejected.empty());
        // Reachability through the facade, over matching edges only.
        bool reachable = true;
        expect(eq(db.has_path(dag.a->guid(), dag.d->guid(), open_only,
                              EdgeOrientation::outgoing, reachable),
                  GdiStatus::ok));
        expect(reachable);
        Constraint const blocked =
            Constraint{}.with(ConstraintCondition::on_label(kBlockedEdgeLabel, GdiOp::equal));
        expect(eq(db.has_path(dag.a->guid(), dag.d->guid(), blocked,
                              EdgeOrientation::outgoing, reachable),
                  GdiStatus::ok));
        expect(!reachable) << "a -> b is open, so the blocked-only walk leaves a and dies at b";
        expect(eq(db.has_path(dag.a->guid(), dag.d->guid(), ordering,
                              EdgeOrientation::outgoing, reachable),
                  GdiStatus::error_op))
            << "the reachability probe validates the definition too";
    };

    // 4 readers traverse (filtered and unfiltered) while one writer mutates a
    // disjoint tail region: no crash, invariants hold on every observed walk,
    // and a post-run quiescent traversal matches the brute force.
    // -----------------------------------------------------------------------
    // Database facade: the GDI-shaped entry points resolve a registered
    // constraint handle exactly like the read commands, forward the exact
    // status codes, and agree with the raw Graph API.
    // -----------------------------------------------------------------------
    "database_traverse_bfs_facade"_test = [] {
        Database db;
        LabelId person{};
        PropNameId age{};
        expect(eq(db.create_label("person", person), GdiStatus::ok));
        graph_db::PropertyTypeInfo age_info;
        age_info.name = "age";
        age_info.data = graph_db::GdiDataType::int64_;
        age_info.count = 1;
        age_info.container = graph_db::GdiContainerType::single;
        expect(eq(db.create_property_type(age_info, age), GdiStatus::ok));

        // hub -> leaves (alternating ages), each leaf -> one far vertex.
        kimix::vector<std::byte> hub_key = gtest::to_bytes("hub");
        graph_db::VertexCreateResult hub;
        expect(eq(db.create_vertex(graph_db::VertexKeyView{
                      person, kimix::span<const std::byte>{hub_key.data(), hub_key.size()}},
                                  {}, nullptr, hub),
                  GdiStatus::ok));
        kimix::vector<kimix::Guid> leaves;
        kimix::vector<kimix::Guid> fars;
        for (size_t i = 0; i < 8; ++i) {
            kimix::vector<std::byte> key = gtest::to_bytes(kimix::format("leaf-{}", i));
            graph_db::PropertyList props;
            props.push_back({age, PropertyValue::make_int64(i % 2 == 0 ? 70 : 20)});
            graph_db::LabelId labels[]{person};
            graph_db::VertexCreateResult created;
            expect(eq(db.create_vertex(graph_db::VertexKeyView{
                          person, kimix::span<const std::byte>{key.data(), key.size()}},
                                      kimix::span<const graph_db::LabelId>{labels, 1}, &props,
                                      created),
                      GdiStatus::ok));
            leaves.push_back(created.uid);
            kimix::Guid edge;
            expect(eq(db.edge_create(hub.uid, created.uid, graph_db::EdgeDirectionType::directed,
                                     {}, nullptr, nullptr, edge),
                      GdiStatus::ok));
            kimix::vector<std::byte> far_key = gtest::to_bytes(kimix::format("far-{}", i));
            graph_db::VertexCreateResult far_created;
            expect(eq(db.create_vertex(graph_db::VertexKeyView{
                          person, kimix::span<const std::byte>{far_key.data(), far_key.size()}},
                                      {}, nullptr, far_created),
                      GdiStatus::ok));
            fars.push_back(far_created.uid);
            expect(eq(db.edge_create(created.uid, far_created.uid,
                                     graph_db::EdgeDirectionType::directed, {}, nullptr, nullptr, edge),
                      GdiStatus::ok));
        }

        // Unfiltered facade traversal == the raw Graph API.
        {
            kimix::vector<TraversalVertex> facade;
            expect(eq(db.traverse_bfs(hub.uid, {}, EdgeOrientation::outgoing, 100, 2, facade),
                      GdiStatus::ok));
            kimix::vector<TraversalVertex> raw;
            expect(eq(db.graph().traverse_bfs(hub.uid, {}, EdgeOrientation::outgoing, 100, 2,
                                              raw),
                      GdiStatus::ok));
            expect(eq(facade.size(), raw.size()));
            for (size_t i = 0; i < facade.size(); ++i) {
                expect(facade[i].guid == raw[i].guid && facade[i].parent == raw[i].parent &&
                       facade[i].depth == raw[i].depth)
                    << "the facade forwards the Graph result verbatim";
            }
            expect(eq(facade.size(), size_t{1 + 8 + 8}));
        }
        // Registered-handle constraint (the C ABI's shape): seniors only.
        {
            Constraint definition;
            definition.conditions.push_back(ConstraintCondition::on_property(
                age, GdiOp::greater_or_equal, PropertyValue::make_int64(60)));
            graph_db::ConstraintHandle handle{};
            expect(eq(db.create_constraint(definition, handle), GdiStatus::ok));
            expect(handle.valid());
            Constraint resolved;
            expect(db.schema().constraint(handle, resolved));
            kimix::vector<TraversalVertex> walk;
            expect(eq(db.traverse_bfs(hub.uid, resolved, EdgeOrientation::outgoing, 100, 2,
                                      walk),
                      GdiStatus::ok));
            expect(eq(walk.size(), size_t{1 + 4}))
                << "the handle-resolved filter gates the discovery";
        }
        // Facade error surface: unknown start, bad cap, bad bound.
        {
            kimix::vector<TraversalVertex> walk;
            kimix::Guid unknown(true);
            expect(eq(db.traverse_bfs(unknown, {}, EdgeOrientation::outgoing, 10, 2, walk),
                      GdiStatus::error_vertex));
            expect(eq(db.traverse_bfs(hub.uid, {}, EdgeOrientation::outgoing, 0, 2, walk),
                      GdiStatus::error_argument));
            kimix::vector<kimix::Guid> path;
            expect(eq(db.shortest_path(hub.uid, leaves[0], 0, path),
                      GdiStatus::error_argument));
            expect(eq(db.shortest_path(unknown, leaves[0], 2, path),
                      GdiStatus::error_vertex));
            expect(eq(db.shortest_path(hub.uid, fars[0], 4, path), GdiStatus::ok));
            expect(eq(path.size(), size_t{3})) << "hub -> leaf -> far";
        }
    };

    "traverse_bfs_reader_writer_stress"_test = [] {
        Graph graph;
        auto core = gtest::build_layered_dag(graph, 400, 5150u);
        // Label the core so the filtered readers have something to match.
        for (auto *vertex : core) {
            vertex->add_label(kPersonLabel);
            vertex->put_property(kAgeName, PropertyValue::make_int64(42));
        }
        Constraint persons;
        persons.conditions.push_back(
            ConstraintCondition::on_label(kPersonLabel, GdiOp::equal));

        std::atomic<bool> stop{false};
        std::atomic<uint64_t> walks{0};
        std::atomic<uint64_t> invariant_violations{0};

        // The writer owns a disjoint vertex set it creates itself (the
        // documented reset: the bench thread built the core).
        std::thread writer([&] {
            graph.reset_writer_thread();
            kimix::vector<Vertex *> mine;
            uint64_t round = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                Vertex *vertex = graph.create_vertex();
                if (vertex == nullptr) {
                    break;
                }
                vertex->add_label(kRobotLabel);
                mine.push_back(vertex);
                if (mine.size() > 1) {
                    graph.connect(mine[mine.size() - 2]->guid(), vertex->guid());
                }
                // Cascade-remove every other batch from the tail: exercises the
                // concurrent-cascade memory-safety argument mid-traversal.
                if (mine.size() >= 64) {
                    if ((round++ % 2u) == 0u) {
                        for (auto *v : mine) {
                            graph.remove_vertex(v->guid());
                        }
                        mine.clear();
                    }
                }
            }
        });

        auto reader = [&] {
            std::mt19937 rng(static_cast<unsigned>(std::hash<std::thread::id>{}(
                                 std::this_thread::get_id())));
            while (!stop.load(std::memory_order_relaxed)) {
                kimix::Guid const start =
                    core[rng() % core.size()]->guid();
                kimix::vector<TraversalVertex> walk;
                GdiStatus const status =
                    rng() % 2u
                        ? graph.traverse_bfs(start, {}, EdgeOrientation::outgoing, 12, 3,
                                             walk)
                        : graph.traverse_bfs(start, persons, EdgeOrientation::both, 12, 2,
                                             walk);
                if (status != GdiStatus::ok) {
                    continue; // the start may have been removed? core is never removed
                }
                if (!walk_is_consistent(walk, start, 12)) {
                    invariant_violations.fetch_add(1, std::memory_order_relaxed);
                }
                walks.fetch_add(1, std::memory_order_relaxed);
            }
        };
        kimix::vector<std::thread> readers;
        for (int i = 0; i < 4; ++i) {
            readers.emplace_back(reader);
        }
        // Let the mix run briefly.
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        stop.store(true, std::memory_order_relaxed);
        for (auto &thread : readers) {
            thread.join();
        }
        writer.join();

        expect(gt(walks.load(), uint64_t{0})) << "the readers did work";
        expect(eq(invariant_violations.load(), uint64_t{0}))
            << "no observed walk violated the BFS invariants under concurrent writes";

        // Quiescent: the traversal matches the brute force on the final state.
        kimix::vector<TraversalVertex> walk;
        expect(eq(graph.traverse_bfs(core[0]->guid(), {}, EdgeOrientation::outgoing, 16,
                                     3, walk),
                  GdiStatus::ok));
        auto expected = brute_force_bfs(graph, core[0]->guid(), nullptr,
                                        EdgeOrientation::outgoing, 16, 3);
        expect(same_walk(walk, expected)) << "post-stress traversal matches the brute force";
    };

    // -----------------------------------------------------------------------
    // The EDGE-filtered BFS (plan §1.3): the Graph-level walk against a brute
    // force that shares no code with it (public edge reads for the hops, the
    // whole edge table for the internal-edge sweep), over the constraint x
    // orientation x depth x cap x start matrix on the known 6-vertex DAG.
    // -----------------------------------------------------------------------
    "traverse_bfs_edges_matches_brute_force"_test = [] {
        Graph graph;
        auto dag = gtest::small_dag(graph);
        // Label every edge by its ORIGIN (the same fixture shape the filtered
        // shortest_path legs use), so a label filter changes the reachable set.
        for (auto const &id : graph.edge_ids()) {
            EdgeRecord record;
            expect(graph.edge_record(id, record));
            bool const blocked = record.origin == dag.b->guid() || record.origin == dag.f->guid();
            expect(eq(graph.edge_add_label(id, blocked ? kBlockedEdgeLabel : kOpenEdgeLabel),
                      GdiStatus::ok));
        }
        kimix::vector<Constraint> constraints;
        constraints.push_back(Constraint{}); // no filter: every edge
        constraints.push_back(
            Constraint{}.with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::equal)));
        constraints.push_back(
            Constraint{}.with(ConstraintCondition::on_label(kBlockedEdgeLabel, GdiOp::equal)));
        EdgeOrientation const orientations[] = {EdgeOrientation::outgoing,
                                                EdgeOrientation::incoming,
                                                EdgeOrientation::both};
        uint32_t const depths[] = {1u, 2u, 3u, Graph::kMaxNativeHopDepth};
        uint32_t const caps[] = {1u, 2u, 3u, 6u, 100u};
        Graph::BfsEdgesResult actual;
        size_t compared = 0;
        for (auto const &constraint : constraints) {
            for (EdgeOrientation orientation : orientations) {
                for (uint32_t depth : depths) {
                    for (uint32_t cap : caps) {
                        for (auto *vertex : dag.vertices) {
                            kimix::Guid const start = vertex->guid();
                            expect(eq(graph.traverse_bfs_edges(gtest::one_start(start), constraint, orientation,
                                                               depth, cap, actual),
                                      GdiStatus::ok));
                            RefBfs const expected =
                                ref_edge_bfs(graph, start, constraint, orientation, depth, cap);
                            expect_same_bfs_vertices(actual.vertices, expected.vertices,
                                                     "BFS: discovery order and hop edges match");
                            expect(eq(actual.truncated, expected.truncated))
                                << "BFS: the truncation flag matches the cap rule";
                            expect_same_bfs_edges(actual.edges, expected.edges,
                                                  "BFS: the internal-edge sweep matches");
                            expect_bfs_vertices_are_sound(graph, actual.vertices, constraint,
                                                          "BFS: every reported hop is real");
                            ++compared;
                        }
                    }
                }
            }
        }
        expect(gt(compared, size_t{200})) << "the matrix really ran";
    };

    // The Graph-level contract the C ABI legs cannot see: the error surface, the
    // self-edge shape, the BOTH-orientation dedupe, and the reusable-buffer rule.
    "traverse_bfs_edges_graph_contract"_test = [] {
        Graph graph;
        auto dag = gtest::small_dag(graph);
        Graph::BfsEdgesResult walk;
        // Arguments: the bound, the cap, the unknown start — and the label
        // ordering op, refused BEFORE anything else is even looked at.
        expect(eq(graph.traverse_bfs_edges(gtest::one_start(dag.a->guid()), {}, EdgeOrientation::outgoing, 0u, 10u,
                                           walk),
                  GdiStatus::error_argument));
        expect(eq(graph.traverse_bfs_edges(gtest::one_start(dag.a->guid()), {}, EdgeOrientation::outgoing,
                                           Graph::kMaxNativeHopDepth + 1u, 10u, walk),
                  GdiStatus::error_argument));
        expect(eq(graph.traverse_bfs_edges(gtest::one_start(dag.a->guid()), {}, EdgeOrientation::outgoing, 1u, 0u,
                                           walk),
                  GdiStatus::error_argument));
        expect(eq(graph.traverse_bfs_edges(gtest::one_start(kimix::Guid{true}), {}, EdgeOrientation::outgoing, 1u,
                                           10u, walk),
                  GdiStatus::error_vertex));
        Constraint const ordering =
            Constraint{}.with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::less));
        expect(eq(graph.traverse_bfs_edges(gtest::one_start(dag.a->guid()), ordering, EdgeOrientation::outgoing, 1u,
                                           10u, walk),
                  GdiStatus::error_op))
            << "a label condition with an ordering op is refused up front, like the walk's";
        // The isolated vertex: the walk's whole product is the start itself, and
        // the sweep has nothing to say.
        expect(eq(graph.traverse_bfs_edges(gtest::one_start(dag.e->guid()), {}, EdgeOrientation::both, 4u, 10u,
                                           walk),
                  GdiStatus::ok));
        expect(eq(walk.vertices.size(), size_t{1}));
        expect(walk.edges.empty());
        expect(!walk.truncated);
        expect(!static_cast<bool>(walk.vertices[0].edge));
        // A self edge under BOTH is listed twice in ONE snapshot (its out half and
        // its in half), so the endpoint test alone would emit it twice. Keep the
        // first sighting: the scan covers only this vertex's own staged candidates,
        // and self edges are the rare case. GraphOptions' defaults allow both self
        // edges and parallel edges, so a default Graph is the fixture here.
        Graph self;
        Vertex *s = self.create_vertex();
        Vertex *t = self.create_vertex();
        EdgeId loop{};
        expect(eq(self.edge_create(s->guid(), s->guid(), graph_db::EdgeDirectionType::undirected,
                                   {}, nullptr, nullptr, loop),
                  GdiStatus::ok));
        EdgeId out_edge{};
        expect(eq(self.edge_create(s->guid(), t->guid(), graph_db::EdgeDirectionType::directed, {},
                                   nullptr, nullptr, out_edge),
                  GdiStatus::ok));
        expect(eq(self.traverse_bfs_edges(gtest::one_start(s->guid()), {}, EdgeOrientation::both, 4u, 100u, walk),
                  GdiStatus::ok));
        expect(eq(walk.vertices.size(), size_t{2})) << "s and t";
        expect(eq(walk.edges.size(), size_t{2})) << "the self edge once, plus s -> t";
        if (walk.edges.size() == 2) {
            size_t loops = 0;
            for (auto const &edge : walk.edges) {
                if (edge.edge == loop) {
                    ++loops;
                    expect(edge.origin == s->guid() && edge.target == s->guid())
                        << "the self edge reports its own endpoints";
                }
            }
            expect(eq(loops, size_t{1})) << "deduplicated by edge uid, not by endpoint pair";
        }
        // The reusable buffer: a short walk after a long one leaves NOTHING of the
        // long one behind (both vectors are cleared and refilled in place).
        expect(eq(graph.traverse_bfs_edges(gtest::one_start(dag.a->guid()), {}, EdgeOrientation::outgoing, 8u, 100u,
                                           walk),
                  GdiStatus::ok));
        expect(eq(walk.vertices.size(), size_t{4}));
        expect(eq(graph.traverse_bfs_edges(gtest::one_start(dag.e->guid()), {}, EdgeOrientation::outgoing, 8u, 100u,
                                           walk),
                  GdiStatus::ok));
        expect(eq(walk.vertices.size(), size_t{1})) << "cleared, not appended";
        expect(walk.edges.empty());
    };

    // The WIDE visit of the new walk: a hub whose 200 links all match lands on the
    // batched drain (kEdgeFilterMinLinks) in discovery AND on the batched sweep in the
    // second pass, so the head/chunk split and the sweep's own batch are both covered
    // (the small-DAG matrix above only ever runs the per-link path). The parallel pair
    // sits at link index 130 — inside a drained chunk, past the head — so the batch's
    // verdict array and its consume loop, not the snapshot, decide which edge carries
    // the hop, exactly like the filtered shortest_path wide-visit leg.
    "traverse_bfs_edges_wide_visit_batches"_test = [] {
        Graph graph;
        Vertex *hub = graph.create_vertex();
        Vertex *shared = graph.create_vertex();
        EdgeId pair_first{};
        EdgeId pair_second{};
        kimix::vector<EdgeId> all_edges;
        size_t made = 0;
        for (size_t i = 0; i < kWideVisitLinks; ++i) {
            if (i == kWideVisitPairAt) {
                LabelId const both[]{kOpenEdgeLabel, kBlockedEdgeLabel};
                if (wide_link(graph, hub->guid(), shared->guid(),
                              kimix::span<const LabelId>{both, 2}, pair_first)) {
                    all_edges.push_back(pair_first);
                    ++made;
                }
                continue;
            }
            if (i == kWideVisitPairAt + 1) {
                LabelId const open[]{kOpenEdgeLabel};
                if (wide_link(graph, hub->guid(), shared->guid(),
                              kimix::span<const LabelId>{open, 1}, pair_second)) {
                    all_edges.push_back(pair_second);
                    ++made;
                }
                continue;
            }
            Vertex *leaf = graph.create_vertex();
            LabelId const open[]{kOpenEdgeLabel};
            EdgeId id{};
            if (wide_link(graph, hub->guid(), leaf->guid(), kimix::span<const LabelId>{open, 1},
                          id)) {
                all_edges.push_back(id);
                ++made;
            }
        }
        expect(eq(made, kWideVisitLinks)) << "the wide fixture built every link";
        Constraint const open_only =
            Constraint{}.with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::equal));
        Graph::BfsEdgesResult walk;
        // The whole 200-link neighbourhood, filtered: the batch is what tests the edges.
        expect(eq(graph.traverse_bfs_edges(gtest::one_start(hub->guid()), open_only, EdgeOrientation::outgoing, 2u,
                                           1000u, walk),
                  GdiStatus::ok));
        RefBfs const expected =
            ref_edge_bfs(graph, hub->guid(), open_only, EdgeOrientation::outgoing, 2u, 1000u);
        expect_same_bfs_vertices(walk.vertices, expected.vertices,
                                 "wide visit: the batched drain discovers the same set");
        expect_same_bfs_edges(walk.edges, expected.edges,
                              "wide sweep: the batched pass reports every internal edge");
        expect(!walk.truncated);
        // Every leaf is reached by ONE hop and every edge of the fixture is internal to
        // the discovered set — the sweep reports all 200 of them, the parallel pair
        // included, and the pair's two uids both appear (the spanning tree would show
        // only one).
        expect(eq(walk.vertices.size(), size_t{kWideVisitLinks}))
            << "hub + 198 leaves + the shared target = one vertex per distinct link";
        expect(eq(walk.edges.size(), size_t{kWideVisitLinks}));
        size_t first_seen = 0;
        size_t second_seen = 0;
        for (auto const &edge : walk.edges) {
            first_seen += edge.edge == pair_first ? 1u : 0u;
            second_seen += edge.edge == pair_second ? 1u : 0u;
        }
        expect(eq(first_seen, size_t{1})) << "the batched sweep reports each uid once";
        expect(eq(second_seen, size_t{1}));
        // The hop that discovered `shared` is the FIRST matching link of the drained
        // chunk it sits in, and refusing it reroutes the same hop onto its twin — the
        // wide path is a cost change, not a rule change.
        bool shared_step = false;
        for (auto const &vertex : walk.vertices) {
            if (vertex.vertex == shared->guid()) {
                shared_step = true;
                expect(vertex.edge == pair_first)
                    << "the batched drain reports the FIRST matching parallel edge";
                expect(eq(vertex.depth, uint32_t{1}));
            }
        }
        expect(shared_step) << "shared was discovered through the drained chunk";
        // The cap mid-drain: a wide visit stops the WHOLE walk the moment the array is
        // full, so the result is exactly max_vertices long and never a partial level.
        Graph::BfsEdgesResult capped;
        expect(eq(graph.traverse_bfs_edges(gtest::one_start(hub->guid()), open_only, EdgeOrientation::outgoing, 2u,
                                           40u, capped),
                  GdiStatus::ok));
        expect(eq(capped.vertices.size(), size_t{40})) << "exactly the cap, not one more";
        expect(capped.truncated);
        RefBfs const capped_expected =
            ref_edge_bfs(graph, hub->guid(), open_only, EdgeOrientation::outgoing, 2u, 40u);
        expect_same_bfs_vertices(capped.vertices, capped_expected.vertices,
                                 "the capped wide walk matches the capped brute force");
        expect(eq(capped.truncated, capped_expected.truncated));
    };

    // The Database facade: the constraint reference is resolved and validated BEFORE
    // the walk runs, and the Graph's status is forwarded verbatim.
    "database_traverse_bfs_edges_facade"_test = [] {
        Database db;
        auto dag = gtest::small_dag(db.graph());
        for (auto const &id : db.graph().edge_ids()) {
            EdgeRecord record;
            expect(db.graph().edge_record(id, record));
            bool const hinge = record.origin == dag.b->guid();
            expect(eq(db.graph().edge_add_label(id, hinge ? kBlockedEdgeLabel : kOpenEdgeLabel),
                      GdiStatus::ok));
        }
        Constraint const open_only =
            Constraint{}.with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::equal));
        Graph::BfsEdgesResult via_facade;
        expect(eq(db.traverse_bfs_edges(gtest::one_start(dag.a->guid()), open_only, EdgeOrientation::outgoing, 8u,
                                        100u, via_facade),
                  GdiStatus::ok));
        Graph::BfsEdgesResult via_graph;
        expect(eq(db.graph().traverse_bfs_edges(gtest::one_start(dag.a->guid()), open_only,
                                                EdgeOrientation::outgoing, 8u, 100u, via_graph),
                  GdiStatus::ok));
        expect(eq(via_facade.vertices.size(), via_graph.vertices.size()));
        expect(eq(via_facade.edges.size(), via_graph.edges.size()))
            << "the facade forwards the Graph result verbatim";
        // A registered HANDLE answers the same definition (the C ABI shape).
        graph_db::ConstraintHandle handle{};
        expect(eq(db.create_constraint(open_only, handle), GdiStatus::ok));
        Constraint resolved;
        expect(db.schema().constraint(handle, resolved));
        Graph::BfsEdgesResult via_handle;
        expect(eq(db.traverse_bfs_edges(gtest::one_start(dag.a->guid()), resolved, EdgeOrientation::outgoing, 8u,
                                        100u, via_handle),
                  GdiStatus::ok));
        expect(eq(via_handle.vertices.size(), via_facade.vertices.size()));
        // The facade validates the definition BEFORE walking, and an unknown start
        // is the Graph's error_vertex, not an invented empty answer.
        Constraint const ordering =
            Constraint{}.with(ConstraintCondition::on_label(kOpenEdgeLabel, GdiOp::greater));
        Graph::BfsEdgesResult rejected;
        expect(eq(db.traverse_bfs_edges(gtest::one_start(dag.a->guid()), ordering, EdgeOrientation::outgoing, 8u,
                                        100u, rejected),
                  GdiStatus::error_op));
        expect(rejected.vertices.empty() && rejected.edges.empty() && !rejected.truncated);
        expect(eq(db.traverse_bfs_edges(gtest::one_start(kimix::Guid{true}), open_only, EdgeOrientation::outgoing,
                                        8u, 100u, rejected),
                  GdiStatus::error_vertex));
    };

    // -----------------------------------------------------------------------
    // The ABI-7 C legs: kimix_gdb_traverse_bfs_edges, two-phase, through the
    // frozen POD shapes (40 B / 48 B, pinned by static_assert above).
    // -----------------------------------------------------------------------

    // Two parallel edges between A and B: BOTH are reported (the spanning tree
    // would carry one and lose the other), and the discovered vertex B names
    // exactly ONE of them as its discovering edge.
    "cabi_traverse_bfs_edges_parallel_edges"_test = [] {
        cabi_session db;
        uint32_t const node = db.label("node");
        uint32_t const first = db.label("first_route");
        uint32_t const second = db.label("second_route");
        kimix_gdb_guid const a = db.vertex(node, "a");
        kimix_gdb_guid const b = db.vertex(node, "b");
        kimix_gdb_guid const e1 = db.edge_labeled(a, b, first);
        kimix_gdb_guid const e2 = db.edge_labeled(a, b, second);

        two_phase_result const walk =
            two_phase_bfs(db.session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 4, 100);
        expect(eq(walk.probe_code, KIMIX_GDB_OK));
        expect(eq(walk.fill_code, KIMIX_GDB_OK));
        expect(eq(walk.fill.vertices.size(), size_t{2})) << "A then B";
        expect(guid_eq(walk.fill.vertices[0].vertex, a) &&
               guid_eq(walk.fill.vertices[1].vertex, b));
        expect(walk.fill.vertices[0].depth == 0u && walk.fill.vertices[0].edge.lo == 0ull &&
               walk.fill.vertices[0].edge.hi == 0ull)
            << "items[0] is the start at depth 0 with a zeroed edge";
        expect(walk.fill.vertices[1].depth == 1u);
        bool const by_first = guid_eq(walk.fill.vertices[1].edge, e1);
        bool const by_second = guid_eq(walk.fill.vertices[1].edge, e2);
        expect(by_first || by_second) << "B's discovering edge is ONE of the parallel pair";
        expect(eq(walk.fill.edges.size(), size_t{2})) << "BOTH parallel edges are reported";
        expect(eq(count_uid(walk.fill.edges, e1), size_t{1}));
        expect(eq(count_uid(walk.fill.edges, e2), size_t{1}));
        for (auto const &edge : walk.fill.edges) {
            expect(guid_eq(edge.origin, a) && guid_eq(edge.target, b))
                << "the record names the edge's own endpoints";
        }
        // A constraint that selects only the SECOND label: the first edge is not
        // traversable, so the walk still reaches B (over the second edge) and the
        // sweep reports only the matching one.
        uint32_t const only_second = db.constraint();
        expect(eq(kimix_gdb_constraint_add_label(db.session, only_second, second,
                                                 KIMIX_GDB_OP_EQUAL),
                  KIMIX_GDB_OK));
        two_phase_result const filtered =
            two_phase_bfs(db.session, a, only_second, KIMIX_GDB_ORIENTATION_OUTGOING, 4, 100);
        expect(eq(filtered.fill_code, KIMIX_GDB_OK));
        expect(eq(filtered.fill.vertices.size(), size_t{2}));
        expect(guid_eq(filtered.fill.vertices[1].edge, e2)) << "the hop is the matching edge";
        expect(eq(filtered.fill.edges.size(), size_t{1}));
        expect(guid_eq(filtered.fill.edges[0].edge, e2));
    };

    // The triangle A -> B, A -> C, B -> C: B and C are both discovered from A, so
    // the spanning tree is {A->B, A->C} — yet the C/B edge is in the edges output.
    // Under BOTH orientation the same internal edge is seen from both endpoints and
    // is still reported exactly once.
    "cabi_traverse_bfs_edges_internal_sweep"_test = [] {
        cabi_session db;
        uint32_t const node = db.label("node");
        kimix_gdb_guid const a = db.vertex(node, "a");
        kimix_gdb_guid const b = db.vertex(node, "b");
        kimix_gdb_guid const c = db.vertex(node, "c");
        kimix_gdb_guid const ab = db.edge(a, b);
        kimix_gdb_guid const ac = db.edge(a, c);
        kimix_gdb_guid const bc = db.edge(b, c);

        two_phase_result const out =
            two_phase_bfs(db.session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 4, 100);
        expect(eq(out.fill_code, KIMIX_GDB_OK));
        expect(eq(out.fill.vertices.size(), size_t{3})) << "A, then B and C at depth 1";
        expect(out.fill.vertices[1].depth == 1u && out.fill.vertices[2].depth == 1u);
        expect(eq(out.fill.edges.size(), size_t{3}))
            << "the third edge of the triangle is reported, tree edge or not";
        expect(eq(count_uid(out.fill.edges, bc), size_t{1})) << "B -> C is internal";
        expect(eq(count_uid(out.fill.edges, ab), size_t{1}));
        expect(eq(count_uid(out.fill.edges, ac), size_t{1}));

        two_phase_result const both =
            two_phase_bfs(db.session, a, 0, KIMIX_GDB_ORIENTATION_BOTH, 4, 100);
        expect(eq(both.fill_code, KIMIX_GDB_OK));
        expect(eq(both.fill.vertices.size(), size_t{3}));
        expect(eq(both.fill.edges.size(), size_t{3}))
            << "a BOTH read sees each internal edge twice and reports it once";
        expect(eq(count_uid(both.fill.edges, bc), size_t{1}));

        // Depth bound: with max_depth 1, C is reached from A directly, so B -> C is
        // still internal to the discovered set {A, B, C} and still reported — the
        // sweep does NOT depend on the walk having stepped over the edge.
        two_phase_result const shallow =
            two_phase_bfs(db.session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 1, 100);
        expect(eq(shallow.fill.edges.size(), size_t{3}));
        // Walking the same triangle BACKWARDS from C: the incoming read discovers A
        // and B (over C's two in-links), so all three edges are internal to the set
        // and each is reported once — from the endpoint the orientation puts the
        // edge's target at.
        two_phase_result const back =
            two_phase_bfs(db.session, c, 0, KIMIX_GDB_ORIENTATION_INCOMING, 4, 100);
        expect(eq(back.fill.vertices.size(), size_t{3})) << "C, then A and B over C's in-links";
        expect(eq(back.fill.edges.size(), size_t{3}));
        expect(eq(count_uid(back.fill.edges, ab), size_t{1}));
        expect(eq(count_uid(back.fill.edges, ac), size_t{1}));
        expect(eq(count_uid(back.fill.edges, bc), size_t{1}));
    };

    // The cap: a star with more leaves than max_vertices stops AT the cap and says
    // so; a generous cap discovers the same set and says nothing.
    "cabi_traverse_bfs_edges_truncation"_test = [] {
        cabi_session db;
        uint32_t const node = db.label("node");
        kimix_gdb_guid const hub = db.vertex(node, "hub");
        for (size_t i = 0; i < 6; ++i) {
            db.edge(hub, db.vertex(node, kimix::format("leaf-{}", i).c_str()));
        }
        // 6 leaves + the hub = 7 discoverable. A cap of 3 keeps the hub and two
        // leaves, and the third leaf that passed both tests is what sets the flag.
        two_phase_result const capped = two_phase_bfs(db.session, hub, 0,
                                                       KIMIX_GDB_ORIENTATION_OUTGOING, 4, 3);
        expect(eq(capped.probe_code, KIMIX_GDB_OK));
        expect(eq(capped.fill_code, KIMIX_GDB_OK));
        expect(eq(capped.probe.vertex_count, size_t{3}));
        expect(eq(capped.fill.vertex_count, size_t{3})) << "the cap holds, in both phases";
        expect(eq(capped.probe.truncated, uint8_t{1})) << "the UI must show 'part of it'";
        expect(eq(capped.fill.truncated, uint8_t{1})) << "probe and fill agree";
        // The sweep follows the DISCOVERED set, so a capped star reports the two
        // edges inside it and nothing of the leaves that were never discovered.
        expect(eq(capped.fill.edge_count, size_t{2}));

        two_phase_result const full = two_phase_bfs(db.session, hub, 0,
                                                     KIMIX_GDB_ORIENTATION_OUTGOING, 4, 100);
        expect(eq(full.fill.vertex_count, size_t{7}));
        expect(eq(full.fill.edge_count, size_t{6}));
        expect(eq(full.probe.truncated, uint8_t{0})) << "a generous cap truncates nothing";
        expect(eq(full.fill.truncated, uint8_t{0}));

        // A cap of 1 keeps only the start, and the first refused leaf flags it.
        two_phase_result const one = two_phase_bfs(db.session, hub, 0,
                                                    KIMIX_GDB_ORIENTATION_OUTGOING, 4, 1);
        expect(eq(one.fill.vertex_count, size_t{1}));
        expect(eq(one.fill.truncated, uint8_t{1}));
        expect(one.fill.edges.empty()) << "no edge is internal to a one-vertex set";
        // The exact-fit rule: a cap EQUAL to the reachable set discovers everything and
        // drops nothing, so the flag is 0 — the walk ends because the frontier ran out,
        // not because the cap refused a hop. (A cap of 1 on a vertex with no links is the
        // same case one vertex wide.)
        kimix_gdb_guid const small_hub = db.vertex(node, "small_hub");
        db.edge(small_hub, db.vertex(node, "s1"));
        db.edge(small_hub, db.vertex(node, "s2"));
        two_phase_result const exact = two_phase_bfs(db.session, small_hub, 0,
                                                      KIMIX_GDB_ORIENTATION_OUTGOING, 4, 3);
        expect(eq(exact.fill.vertex_count, size_t{3})) << "start + both leaves = the cap";
        expect(eq(exact.fill.truncated, uint8_t{0}))
            << "exact fit is NOT truncation: nothing readable was dropped";
        expect(eq(exact.fill.edge_count, size_t{2}));
        // An isolated start with a cap of 1 discovers everything there is: full, 0.
        kimix_gdb_guid const lonely = db.vertex(node, "lonely");
        two_phase_result const single = two_phase_bfs(db.session, lonely, 0,
                                                       KIMIX_GDB_ORIENTATION_BOTH, 4, 1);
        expect(eq(single.fill.vertex_count, size_t{1}));
        expect(eq(single.fill.truncated, uint8_t{0}))
            << "the flag means 'something was dropped', not 'the array is full'";
    };

    // The two-phase protocol itself: probe == fill for BOTH arrays and the flag, a
    // short capacity answers ERR_BUFFER with the sizes still reported, and the flag
    // agrees between the phases.
    "cabi_traverse_bfs_edges_two_phase"_test = [] {
        cabi_session db;
        uint32_t const node = db.label("node");
        kimix_gdb_guid const a = db.vertex(node, "a");
        kimix_gdb_guid const b = db.vertex(node, "b");
        kimix_gdb_guid const c = db.vertex(node, "c");
        db.edge(a, b);
        db.edge(a, c);
        db.edge(b, c);

        size_t vertex_count = 0;
        size_t edge_count = 0;
        uint8_t truncated = 0;
        expect(eq(bfs_edges_one(db.session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                               4, 2, nullptr, 0, nullptr, 0, &vertex_count,
                                               &edge_count, &truncated),
                  KIMIX_GDB_OK));
        expect(eq(vertex_count, size_t{2})) << "the probe runs the same walk";
        expect(eq(edge_count, size_t{1}));
        expect(eq(truncated, uint8_t{1}));
        expect(eq(bfs_edges_one(db.session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                               4, 100, nullptr, 0, nullptr, 0, &vertex_count,
                                               &edge_count, &truncated),
                  KIMIX_GDB_OK));
        expect(eq(vertex_count, size_t{3}));
        expect(eq(edge_count, size_t{3}));
        expect(eq(truncated, uint8_t{0}));

        // Fill with exactly the reported sizes: probe == fill, every record.
        two_phase_result const walk =
            two_phase_bfs(db.session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 4, 100);
        expect(eq(walk.fill_code, KIMIX_GDB_OK));
        expect(eq(walk.fill.vertex_count, walk.probe.vertex_count)) << "vertex counts agree";
        expect(eq(walk.fill.edge_count, walk.probe.edge_count)) << "edge counts agree";
        expect(eq(walk.fill.truncated, walk.probe.truncated)) << "the flag agrees";
        // Same, with a cap that truncates: probe and fill must both say 1.
        two_phase_result const capped =
            two_phase_bfs(db.session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 4, 2);
        expect(eq(capped.fill_code, KIMIX_GDB_OK));
        expect(eq(capped.fill.truncated, capped.probe.truncated));
        expect(eq(capped.fill.truncated, uint8_t{1}));

        // A short VERTEX capacity: ERR_BUFFER, sizes still reported, flag still
        // reported — the caller can resize and retry without a second probe.
        kimix_gdb_bfs_vertex vertices[4]{};
        kimix_gdb_bfs_edge edges[4]{};
        vertex_count = 999;
        edge_count = 999;
        truncated = 9;
        expect(eq(bfs_edges_one(db.session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                               4, 100, vertices, 2, edges, 8, &vertex_count,
                                               &edge_count, &truncated),
                  KIMIX_GDB_ERR_BUFFER));
        expect(eq(vertex_count, size_t{3})) << "the size the caller must provide";
        expect(eq(edge_count, size_t{3}));
        expect(eq(truncated, uint8_t{0}));
        // A short EDGE capacity fails the same way (the vertex array is big enough).
        vertex_count = 999;
        edge_count = 999;
        expect(eq(bfs_edges_one(db.session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                               4, 100, vertices, 4, edges, 2, &vertex_count,
                                               &edge_count, &truncated),
                  KIMIX_GDB_ERR_BUFFER));
        expect(eq(vertex_count, size_t{3}));
        expect(eq(edge_count, size_t{3}));
        // A fill that hands one array and NULL the other is NOT a probe: the NULL
        // array cannot hold its (non-empty) result, so it is the buffer error.
        expect(eq(bfs_edges_one(db.session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                               4, 100, vertices, 4, nullptr, 0, &vertex_count,
                                               &edge_count, &truncated),
                  KIMIX_GDB_ERR_BUFFER));
        expect(eq(edge_count, size_t{3}));
        // The exact-size fill succeeds and agrees record for record with the walk.
        expect(eq(bfs_edges_one(db.session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                               4, 100, vertices, walk.probe.vertex_count, edges,
                                               walk.probe.edge_count, &vertex_count, &edge_count,
                                               &truncated),
                  KIMIX_GDB_OK));
        expect(eq(vertex_count, walk.probe.vertex_count));
        expect(eq(edge_count, walk.probe.edge_count));
        for (size_t i = 0; i < vertex_count && i < walk.fill.vertices.size(); ++i) {
            expect(guid_eq(vertices[i].vertex, walk.fill.vertices[i].vertex))
                << "the fill is deterministic across calls";
            expect(eq(vertices[i].depth, walk.fill.vertices[i].depth));
            expect(guid_eq(vertices[i].edge, walk.fill.vertices[i].edge));
            expect(eq(vertices[i].reserved, uint32_t{0})) << "reserved stays zero";
        }
        for (size_t i = 0; i < edge_count && i < walk.fill.edges.size(); ++i) {
            expect(guid_eq(edges[i].edge, walk.fill.edges[i].edge)) << "same edge order";
        }
    };

    // The depth shape: max_depth bounds the HOPS, and the reported depths are the
    // level indices.
    "cabi_traverse_bfs_edges_depth_shape"_test = [] {
        cabi_session db;
        uint32_t const node = db.label("node");
        // hub -> m1, m2 -> t1 .. t4: two levels under the hub.
        kimix_gdb_guid const hub = db.vertex(node, "hub");
        kimix_gdb_guid const m1 = db.vertex(node, "m1");
        kimix_gdb_guid const m2 = db.vertex(node, "m2");
        db.edge(hub, m1);
        db.edge(hub, m2);
        for (char const *name : {"t1", "t2", "t3", "t4"}) {
            kimix_gdb_guid const tip = db.vertex(node, name);
            db.edge(m1, tip);
            db.edge(m2, tip);
        }
        two_phase_result const one =
            two_phase_bfs(db.session, hub, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 1, 100);
        expect(eq(one.fill.vertex_count, size_t{3})) << "the hub and its two direct neighbours";
        expect(one.fill.vertices[0].depth == 0u);
        expect(one.fill.vertices[1].depth == 1u && one.fill.vertices[2].depth == 1u)
            << "only direct neighbours appear at depth 1";
        expect(eq(one.fill.truncated, uint8_t{0})) << "the DEPTH bound is not truncation";
        expect(eq(one.fill.edge_count, size_t{2}));

        two_phase_result const two =
            two_phase_bfs(db.session, hub, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 2, 100);
        expect(eq(two.fill.vertex_count, size_t{7})) << "the whole 3-level star";
        size_t at_two = 0;
        for (auto const &item : two.fill.vertices) {
            if (item.depth == 2u) {
                ++at_two;
            }
            expect(item.depth <= 2u) << "no vertex is deeper than the bound";
        }
        expect(eq(at_two, size_t{4})) << "the four tips are all at depth 2";
        // At depth 2 every edge of the fixture is internal to the discovered set.
        expect(eq(two.fill.edge_count, size_t{10})) << "2 hub links + 8 links into the tips";
    };

    // The error surface, in the order the ABI validates: argument guards, then the
    // constraint handle, then the walk's own answers.
    "cabi_traverse_bfs_edges_errors"_test = [] {
        cabi_session db;
        uint32_t const node = db.label("node");
        kimix_gdb_guid const a = db.vertex(node, "a");
        kimix_gdb_guid const unknown{0xdeadbeefull, 0xfeedfaceull};
        size_t vertices = 0;
        size_t edges = 0;
        uint8_t truncated = 0;

        expect(eq(bfs_edges_one(nullptr, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 4,
                                               10, nullptr, 0, nullptr, 0, &vertices, &edges,
                                               &truncated),
                  KIMIX_GDB_ERR_ARGUMENT));
        expect(eq(bfs_edges_one(db.session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 4,
                                               10, nullptr, 0, nullptr, 0, nullptr, &edges,
                                               &truncated),
                  KIMIX_GDB_ERR_ARGUMENT));
        expect(eq(bfs_edges_one(db.session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 4,
                                               10, nullptr, 0, nullptr, 0, &vertices, nullptr,
                                               &truncated),
                  KIMIX_GDB_ERR_ARGUMENT));
        expect(eq(bfs_edges_one(db.session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 4,
                                               10, nullptr, 0, nullptr, 0, &vertices, &edges,
                                               nullptr),
                  KIMIX_GDB_ERR_ARGUMENT));
        // max_depth outside 1..KIMIX_GDB_MAX_HOP_DEPTH, and a cap below 1.
        expect(eq(bfs_edges_one(db.session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 0u,
                                               10, nullptr, 0, nullptr, 0, &vertices, &edges,
                                               &truncated),
                  KIMIX_GDB_ERR_ARGUMENT));
        expect(eq(bfs_edges_one(db.session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING,
                                               KIMIX_GDB_MAX_HOP_DEPTH + 1u, 10, nullptr, 0,
                                               nullptr, 0, &vertices, &edges, &truncated),
                  KIMIX_GDB_ERR_ARGUMENT))
            << "33 is outside the advertised depth";
        expect(eq(bfs_edges_one(db.session, a, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 4,
                                               0u, nullptr, 0, nullptr, 0, &vertices, &edges,
                                               &truncated),
                  KIMIX_GDB_ERR_ARGUMENT));
        expect(eq(KIMIX_GDB_MAX_HOP_DEPTH, 32u)) << "the bound the errors above use";
        // An unknown constraint id is ERR_CONSTRAINT, and it is checked BEFORE the
        // walk: an unknown start with an unknown constraint still answers constraint.
        expect(eq(bfs_edges_one(db.session, a, 424242u,
                                               KIMIX_GDB_ORIENTATION_OUTGOING, 4, 10, nullptr, 0,
                                               nullptr, 0, &vertices, &edges, &truncated),
                  KIMIX_GDB_ERR_CONSTRAINT));
        expect(eq(bfs_edges_one(db.session, unknown, 424242u,
                                               KIMIX_GDB_ORIENTATION_OUTGOING, 4, 10, nullptr, 0,
                                               nullptr, 0, &vertices, &edges, &truncated),
                  KIMIX_GDB_ERR_CONSTRAINT));
        // The unknown start, with a valid constraint, is ERR_VERTEX.
        uint32_t const reader = db.constraint();
        expect(eq(bfs_edges_one(db.session, unknown, reader,
                                               KIMIX_GDB_ORIENTATION_OUTGOING, 4, 10, nullptr, 0,
                                               nullptr, 0, &vertices, &edges, &truncated),
                  KIMIX_GDB_ERR_VERTEX));
        // A label condition carrying an ordering op never reaches the call: the ABI
        // refuses it when the condition is ADDED (the same rule the walk re-checks,
        // and the C++ leg `traverse_bfs_edges_graph_contract` pins that one).
        uint32_t const ordering = db.constraint();
        expect(eq(kimix_gdb_constraint_add_label(db.session, ordering, node, KIMIX_GDB_OP_GREATER),
                  KIMIX_GDB_ERR_OP));
        // The arguments a successful call needs are all present in the deep-leg
        // above; a valid-but-empty constraint id 0 means "every edge".
        expect(eq(bfs_edges_one(db.session, a, 0, KIMIX_GDB_ORIENTATION_BOTH, 32,
                                               1, nullptr, 0, nullptr, 0, &vertices, &edges,
                                               &truncated),
                  KIMIX_GDB_OK));
        expect(eq(vertices, size_t{1})) << "the isolated start is always discovered";
        expect(eq(edges, size_t{0}));
        expect(eq(truncated, uint8_t{0}));
    };

    // W007 / S3, the BFS half of the leak scenario (the shortest_path half is
    // `cabi_any_in_w007_s3_edge_filter` in test_graph_db_c_api.cpp): a reader whose
    // document set omits the edge's evidence must not see the link — not as an edge
    // record, and not through the vertex only it connects. The block is spelled here
    // byte by byte (the §1.1 layout) so the leg feeds the ABI, not a library helper.
    "cabi_traverse_bfs_edges_w007_s3"_test = [] {
        cabi_session db;
        uint32_t const node = db.label("node");
        uint32_t const docs = db.array_type("docs", 3);

        kimix_gdb_guid const alice = db.vertex(node, "alice");
        kimix_gdb_guid const w007 = db.vertex(node, "w007");
        kimix_gdb_guid const secret = db.vertex(node, "secret");
        kimix_gdb_guid const pub = db.vertex(node, "public_link_end");
        auto const alice_block = sa_block({"D-PUB-1", "D-PUB-2", ""});
        auto const w007_block = sa_block({"D-PUB-3", "", ""});
        auto const secret_block = sa_block({"W007", "S3", ""});
        auto const confidential_block = sa_block({"W007", "S3", "E-OTHER"});
        auto const readable_edge_block = sa_block({"D-PUB-2", "", ""});
        db.add_vertex_docs(alice, docs, sa_property(alice_block, 3));
        db.add_vertex_docs(w007, docs, sa_property(w007_block, 3));
        db.add_vertex_docs(secret, docs, sa_property(secret_block, 3));

        // Both confidential links: alice -> w007 is readable by its OWN evidence
        // nowhere, and alice -> secret is the only way to `secret`.
        kimix_gdb_guid const hidden_to_w007 =
            db.edge_with_property(alice, w007, docs, sa_property(confidential_block, 3));
        kimix_gdb_guid const hidden_to_secret =
            db.edge_with_property(alice, secret, docs, sa_property(confidential_block, 3));
        // One link the reader CAN see, so a non-empty answer proves the walk ran.
        kimix_gdb_guid const open_link =
            db.edge_with_property(alice, pub, docs, sa_property(readable_edge_block, 3));

        // The reader: docs ANY_IN {D-PUB-1, D-PUB-2, D-PUB-3}.
        auto const readable_block = sa_block({"D-PUB-1", "D-PUB-2", "D-PUB-3"});
        auto const readable_value = sa_property(readable_block, 3);
        uint32_t const reader = db.constraint();
        expect(eq(kimix_gdb_constraint_add_property(db.session, reader, docs, KIMIX_GDB_OP_ANY_IN,
                                                   &readable_value, 3),
                  KIMIX_GDB_OK));

        // The unfiltered baseline: everything is there, so what follows is the
        // filter's doing and not a missing edge.
        two_phase_result const baseline =
            two_phase_bfs(db.session, alice, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 8, 100);
        expect(eq(baseline.fill.vertex_count, size_t{4})) << "alice, w007, secret, public end";
        expect(eq(baseline.fill.edge_count, size_t{3}));

        // The reader's BFS from alice: the two confidential edges are neither
        // traversed nor reported, and w007 / secret — reachable ONLY over them —
        // are not discovered at all.
        two_phase_result const walk = two_phase_bfs(db.session, alice, reader,
                                                    KIMIX_GDB_ORIENTATION_OUTGOING, 8, 100);
        expect(eq(walk.probe_code, KIMIX_GDB_OK));
        expect(eq(walk.fill_code, KIMIX_GDB_OK));
        expect(eq(walk.fill.edge_count, size_t{1})) << "only the open link survives the sweep";
        expect(guid_eq(walk.fill.edges[0].edge, open_link));
        expect(eq(count_uid(walk.fill.edges, hidden_to_w007), size_t{0}))
            << "the confidential edge is NOT in the edges output";
        expect(eq(count_uid(walk.fill.edges, hidden_to_secret), size_t{0}));
        bool saw_w007 = false;
        bool saw_secret = false;
        bool saw_pub = false;
        for (auto const &item : walk.fill.vertices) {
            saw_w007 = saw_w007 || guid_eq(item.vertex, w007);
            saw_secret = saw_secret || guid_eq(item.vertex, secret);
            saw_pub = saw_pub || guid_eq(item.vertex, pub);
        }
        expect(!saw_w007 && !saw_secret)
            << "a vertex reachable only over a filtered-out edge is not discovered";
        expect(saw_pub) << "the readable link's far end IS discovered (the walk ran)";
        expect(eq(walk.fill.vertex_count, size_t{2})) << "alice and the public end, no more";
        expect(eq(walk.fill.truncated, uint8_t{0}));

        // BOTH orientation does not leak it either: the reader cannot reach w007's
        // side over a hidden link in EITHER direction.
        two_phase_result const undirected =
            two_phase_bfs(db.session, alice, reader, KIMIX_GDB_ORIENTATION_BOTH, 8, 100);
        expect(eq(undirected.fill.vertex_count, size_t{2}));
        expect(eq(undirected.fill.edge_count, size_t{1}));
        expect(eq(count_uid(undirected.fill.edges, hidden_to_w007), size_t{0}));
        expect(eq(count_uid(undirected.fill.edges, hidden_to_secret), size_t{0}));
    };

    // -----------------------------------------------------------------------
    // ABI 8 change 2: SEVERAL start vertices (plan §2.1). The seeding rule is the
    // only new block, so these legs check the seeding and leave the walk's own
    // contract to the ABI-7 legs above.
    // -----------------------------------------------------------------------
    "traverse_bfs_edges_multi_start_overlapping"_test = [] {
        Graph graph;
        auto dag = gtest::small_dag(graph); // a->b, a->c, b->d, c->d, f->d, e isolated
        // Two starts with overlapping neighbourhoods: a reaches b, c, d; f reaches d.
        kimix::vector<kimix::Guid> starts{dag.a->guid(), dag.f->guid()};
        for (EdgeOrientation orientation :
             {EdgeOrientation::outgoing, EdgeOrientation::incoming, EdgeOrientation::both}) {
            for (uint32_t depth : {1u, 2u, 3u}) {
                Graph::BfsEdgesResult walk;
                expect(eq(graph.traverse_bfs_edges(
                              kimix::span<const kimix::Guid>{starts.data(), starts.size()}, {},
                              orientation, depth, 100u, walk),
                          GdiStatus::ok));
                RefBfs const expected =
                    ref_edge_bfs_multi(graph,
                                       kimix::span<const kimix::Guid>{starts.data(), starts.size()},
                                       {}, orientation, depth, 100u);
                expect_same_bfs_vertices(walk.vertices, expected.vertices,
                                         "multi-start: discovery order, edges and depths");
                expect_same_bfs_edges(walk.edges, expected.edges,
                                      "multi-start: the internal-edge sweep");
                expect(eq(walk.truncated, expected.truncated));
                // The seeding contract, spelled out: the starts are the first items, at
                // depth 0, each discovered by no edge, in the caller's order.
                expect(eq(walk.vertices.size() >= 2, true));
                expect(walk.vertices[0].vertex == dag.a->guid());
                expect(walk.vertices[1].vertex == dag.f->guid());
                expect(eq(walk.vertices[0].depth, uint32_t{0}));
                expect(eq(walk.vertices[1].depth, uint32_t{0}));
                expect(!static_cast<bool>(walk.vertices[0].edge));
                expect(!static_cast<bool>(walk.vertices[1].edge));
                // Every vertex appears EXACTLY once, and d — reachable from both — is
                // reported once at the smaller of its two distances (1 when the
                // orientation lets a->..->d run in one hop? it is 2 from a, 1 from f).
                kimix::unordered_set<kimix::Guid> seen;
                for (auto const &item : walk.vertices) {
                    expect(seen.insert(item.vertex).second)
                        << "a vertex reachable from several starts is reported once";
                    if (item.vertex == dag.d->guid() && depth >= 1u) {
                        expect(eq(item.depth, uint32_t{1}))
                            << "the depth is the distance to the NEAREST start";
                    }
                }
                expect(seen.find(dag.e->guid()) == seen.end())
                    << "a vertex no start reaches is not discovered";
            }
        }
    };

    "traverse_bfs_edges_multi_start_collapses_duplicates"_test = [] {
        Graph graph;
        auto dag = gtest::small_dag(graph);
        kimix::vector<kimix::Guid> duplicated{dag.a->guid(), dag.a->guid(), dag.b->guid(),
                                              dag.a->guid()};
        Graph::BfsEdgesResult walk;
        expect(eq(graph.traverse_bfs_edges(
                      kimix::span<const kimix::Guid>{duplicated.data(), duplicated.size()}, {},
                      EdgeOrientation::outgoing, 2u, 100u, walk),
                  GdiStatus::ok));
        expect(walk.vertices[0].vertex == dag.a->guid());
        expect(walk.vertices[1].vertex == dag.b->guid())
            << "a repeated start collapses onto its FIRST position";
        expect(eq(walk.vertices[1].depth, uint32_t{0}));
        size_t const a_items = static_cast<size_t>(
            std::count_if(walk.vertices.begin(), walk.vertices.end(),
                          [&dag](Graph::BfsVertex const &item) {
                              return item.vertex == dag.a->guid();
                          }));
        expect(eq(a_items, size_t{1})) << "one item per distinct start";
        // A one-start call through the multi-start entry point is the ABI-7 answer.
        Graph::BfsEdgesResult single;
        kimix::vector<kimix::Guid> one{dag.a->guid()};
        expect(eq(graph.traverse_bfs_edges(kimix::span<const kimix::Guid>{one.data(), 1}, {},
                                           EdgeOrientation::outgoing, 3u, 100u, single),
                  GdiStatus::ok));
        RefBfs const expected = ref_edge_bfs(graph, dag.a->guid(), {}, EdgeOrientation::outgoing,
                                            3u, 100u);
        expect_same_bfs_vertices(single.vertices, expected.vertices,
                                 "one start through the ABI-8 entry point is the ABI-7 walk");
        expect_same_bfs_edges(single.edges, expected.edges, "same sweep");
    };

    "traverse_bfs_edges_multi_start_errors"_test = [] {
        Graph graph;
        auto dag = gtest::small_dag(graph);
        Graph::BfsEdgesResult walk;
        // No start at all is an argument error (after the bounds, before the walk).
        expect(eq(graph.traverse_bfs_edges(kimix::span<const kimix::Guid>{}, {},
                                           EdgeOrientation::outgoing, 1u, 10u, walk),
                  GdiStatus::error_argument));
        expect(walk.vertices.empty());
        // Bounds still win, in the frozen order.
        kimix::vector<kimix::Guid> one{dag.a->guid()};
        expect(eq(graph.traverse_bfs_edges(kimix::span<const kimix::Guid>{one.data(), 1}, {},
                                           EdgeOrientation::outgoing, 0u, 10u, walk),
                  GdiStatus::error_argument));
        // An unknown start anywhere refuses the whole call and leaves `out` cleared —
        // even when the FIRST start is fine.
        kimix::vector<kimix::Guid> bad{dag.a->guid(), kimix::Guid{true}, dag.b->guid()};
        expect(eq(graph.traverse_bfs_edges(kimix::span<const kimix::Guid>{bad.data(), bad.size()},
                                           {}, EdgeOrientation::outgoing, 2u, 10u, walk),
                  GdiStatus::error_vertex));
        expect(walk.vertices.empty()) << "a refused call writes nothing";
        expect(walk.edges.empty());
        expect(!walk.truncated);
    };

    "traverse_bfs_edges_multi_start_cap_counts_the_starts"_test = [] {
        Graph graph;
        // 50 isolated vertices: distinct starts past the cap never expand a level.
        kimix::vector<kimix::Guid> starts;
        for (uint32_t i = 0; i < 50; ++i) {
            starts.push_back(graph.create_vertex(gtest::to_bytes(kimix::format("s{}", i)))->guid());
        }
        Graph::BfsEdgesResult walk;
        expect(eq(graph.traverse_bfs_edges(
                      kimix::span<const kimix::Guid>{starts.data(), starts.size()}, {},
                      EdgeOrientation::outgoing, 4u, 30u, walk),
                  GdiStatus::ok));
        expect(eq(walk.vertices.size(), size_t{30})) << "the first max_vertices starts are kept";
        for (size_t i = 0; i < walk.vertices.size(); ++i) {
            expect(walk.vertices[i].vertex == starts[i]) << "kept in the caller's order";
            expect(eq(walk.vertices[i].depth, uint32_t{0}));
        }
        expect(walk.truncated) << "the flag says the start list was cut";
        expect(walk.edges.empty()) << "the walk never started, so it reports no edges";
        // At the cap exactly, nothing was dropped.
        kimix::vector<kimix::Guid> exactly(starts.begin(), starts.begin() + 30);
        expect(eq(graph.traverse_bfs_edges(
                      kimix::span<const kimix::Guid>{exactly.data(), exactly.size()}, {},
                      EdgeOrientation::outgoing, 4u, 30u, walk),
                  GdiStatus::ok));
        expect(!walk.truncated) << "exactly max_vertices starts are not a truncation";
        expect(eq(walk.vertices.size(), size_t{30}));
    };

    "traverse_bfs_edges_multi_start_hides_edges_behind_an_unreadable_docs_list"_test = [] {
        // The permission shape (plan §2.1 + §5): docs lists of DIFFERENT lengths, and
        // an edge whose list shares no element with the readable set. The edge filter
        // must keep that edge — and the vertex behind it — out of the walk AND out of
        // the sweep.
        Graph graph;
        kimix::Guid origin = graph.create_vertex(gtest::to_bytes("origin"))->guid();
        kimix::Guid readable = graph.create_vertex(gtest::to_bytes("readable"))->guid();
        kimix::Guid hidden = graph.create_vertex(gtest::to_bytes("hidden"))->guid();
        PropNameId const docs{8001};
        auto strings = [](std::initializer_list<kimix::string_view> elements) {
            kimix::vector<kimix::string_view> views(elements);
            return PropertyValue::make_string_array(
                kimix::span<const kimix::string_view>{views.data(), views.size()});
        };
        kimix::vector<kimix::string_view> ok_docs{"reader-1"};
        kimix::vector<kimix::string_view> no_docs{"stranger", "outsider"};
        graph_db::PropertyList open_edge;
        open_edge.push_back(graph_db::PropertyEntry{docs, strings({"reader-1"})});
        graph_db::PropertyList closed_edge;
        closed_edge.push_back(graph_db::PropertyEntry{docs, strings({"stranger", "outsider"})});
        kimix::Guid visible_edge;
        kimix::Guid hidden_edge;
        expect(eq(graph.edge_create(origin, readable, graph_db::EdgeDirectionType::directed, {}, &open_edge,
                                    nullptr, visible_edge),
                  GdiStatus::ok));
        expect(eq(graph.edge_create(origin, hidden, graph_db::EdgeDirectionType::directed, {}, &closed_edge,
                                    nullptr, hidden_edge),
                  GdiStatus::ok));
        // The readable set is an ANY_IN over reader-1: a 1-element list matches, a
        // 2-element list that shares nothing does not.
        auto const set = strings({"reader-1", "reader-2"});
        Constraint const constraint = Constraint{}.with(
            graph_db::ConstraintCondition::on_property(docs, GdiOp::any_in, set,
                                                       static_cast<uint32_t>(set.element_count())));
        Graph::BfsEdgesResult walk;
        kimix::vector<kimix::Guid> starts{origin};
        expect(eq(graph.traverse_bfs_edges(kimix::span<const kimix::Guid>{starts.data(), 1},
                                           constraint, EdgeOrientation::outgoing, 4u, 100u, walk),
                  GdiStatus::ok));
        kimix::unordered_set<kimix::Guid> discovered;
        for (auto const &item : walk.vertices) {
            discovered.insert(item.vertex);
        }
        expect(discovered.find(readable) != discovered.end()) << "the readable edge is walked";
        expect(discovered.find(hidden) == discovered.end())
            << "a vertex reachable only over an unreadable edge is never discovered";
        expect(eq(walk.edges.size(), size_t{1}));
        expect(walk.edges[0].edge == visible_edge);
        for (auto const &item : walk.edges) {
            expect(item.edge != hidden_edge) << "and the sweep cannot leak it either";
        }
    };

    "cabi_traverse_bfs_edges_multi_start"_test = [] {
        cabi_session db;
        uint32_t const person = db.label("person");
        auto const a = db.vertex(person, "a");
        auto const b = db.vertex(person, "b");
        auto const c = db.vertex(person, "c");
        auto const d = db.vertex(person, "d");
        (void)db.edge(a, b);
        (void)db.edge(b, c);
        (void)db.edge(d, c);
        kimix::vector<kimix_gdb_guid> const starts{a, d};
        two_phase_result const result = two_phase_bfs_multi(
            db.session, starts, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 4u, 100u);
        expect(eq(result.probe_code, KIMIX_GDB_OK));
        expect(eq(result.fill_code, KIMIX_GDB_OK));
        expect(eq(result.probe.vertex_count, result.fill.vertex_count)) << "probe == fill";
        expect(eq(result.probe.edge_count, result.fill.edge_count));
        expect(eq(result.fill.vertex_count, size_t{4})) << "a, d, b, c";
        expect(guid_eq(result.fill.vertices[0].vertex, a)) << "items[0] is starts[0]";
        expect(guid_eq(result.fill.vertices[1].vertex, d)) << "items[1] is starts[1]";
        expect(eq(result.fill.vertices[0].depth, uint32_t{0}));
        expect(eq(result.fill.vertices[1].depth, uint32_t{0}));
        for (auto const &item : result.fill.vertices) {
            if (guid_eq(item.vertex, c)) {
                expect(eq(item.depth, uint32_t{1}))
                    << "c is 2 hops from a and 1 from d: the depth is the distance to the "
                       "nearest start";
            }
        }
        // Zero starts is the ABI's argument error, and nothing is written.
        size_t vertices_out = 99;
        size_t edges_out = 99;
        uint8_t truncated = 7;
        expect(eq(kimix_gdb_traverse_bfs_edges(db.session, starts.data(), 0, 0,
                                               KIMIX_GDB_ORIENTATION_OUTGOING, 4u, 100u, nullptr,
                                               0, nullptr, 0, &vertices_out, &edges_out,
                                               &truncated),
                  KIMIX_GDB_ERR_ARGUMENT));
        expect(eq(vertices_out, size_t{99})) << "a refused call writes nothing";
        expect(eq(truncated, uint8_t{7}));
        // An unknown start anywhere in the list is ERR_VERTEX, nothing written.
        kimix_gdb_guid const unknown{0xdeadbeefull, 0xfeedfaceull};
        kimix::vector<kimix_gdb_guid> const bad{a, unknown, d};
        expect(eq(kimix_gdb_traverse_bfs_edges(db.session, bad.data(), bad.size(), 0,
                                               KIMIX_GDB_ORIENTATION_OUTGOING, 4u, 100u, nullptr,
                                               0, nullptr, 0, &vertices_out, &edges_out,
                                               &truncated),
                  KIMIX_GDB_ERR_VERTEX));
        expect(eq(vertices_out, size_t{99}));
        // The bounds still win over the start list (the argument order is frozen).
        expect(eq(kimix_gdb_traverse_bfs_edges(db.session, starts.data(), starts.size(), 0,
                                               KIMIX_GDB_ORIENTATION_OUTGOING, 0u, 100u, nullptr,
                                               0, nullptr, 0, &vertices_out, &edges_out,
                                               &truncated),
                  KIMIX_GDB_ERR_ARGUMENT));
        expect(eq(kimix_gdb_traverse_bfs_edges(db.session, starts.data(), starts.size(), 0,
                                               KIMIX_GDB_ORIENTATION_OUTGOING, 4u, 0u, nullptr, 0,
                                               nullptr, 0, &vertices_out, &edges_out, &truncated),
                  KIMIX_GDB_ERR_ARGUMENT));
        // The cap counts the STARTS: two starts against max_vertices = 1 keeps the
        // first one, flags the truncation and reports no edge.
        two_phase_result const capped = two_phase_bfs_multi(
            db.session, starts, 0, KIMIX_GDB_ORIENTATION_OUTGOING, 4u, 1u);
        expect(eq(capped.probe_code, KIMIX_GDB_OK));
        expect(eq(capped.probe.vertex_count, size_t{1}));
        expect(eq(capped.probe.edge_count, size_t{0})) << "the walk never started";
        expect(eq(capped.probe.truncated, uint8_t{1}));
        expect(eq(capped.fill_code, KIMIX_GDB_OK));
        expect(eq(capped.fill.vertex_count, size_t{1}));
        expect(guid_eq(capped.fill.vertices[0].vertex, a)) << "the FIRST start survives";
        expect(eq(capped.fill.truncated, uint8_t{1}));
    };
}
