/*
 * graph.h — kimix::graph_db::Graph: the sharded in-memory vertex container.
 *
 * A Graph owns every Vertex of one graph and every dependency edge between
 * them. The vertex map is SHARDED: kGraphShardCount (64) buckets, each a
 * kimix::HashMap<kimix::Guid, kimix::unique_ptr<Vertex>> guarded by its own
 * std::shared_mutex. A vertex lives in shard hash(guid) & (kGraphShardCount - 1)
 * of its OWN GUID, so a reader of vertex V only ever contends with readers and
 * writers of V's shard, and structural reads of different shards run fully in
 * parallel (epoch 1: this replaced the single graph-wide shared_mutex that made
 * every structural read serialize on one lock word).
 *
 * Edges are stored twice per vertex — as GUIDs (the serialized source of truth,
 * see vertex.h) and as resolved Vertex* sets (the in-memory form traversals
 * use). BOTH forms of one vertex are guarded by that vertex's SHARD lock, and
 * are always consistent with each other while it is held.
 *
 * ===========================================================================
 * CONCURRENCY CONTRACT — read this before touching any method
 * ===========================================================================
 *
 * Two lock levels, deliberately independent, plus a strict order INSIDE the
 * lower level:
 *
 *   Level 1 — graph structure: kGraphShardCount shard locks. Each shard owns
 *   one `std::shared_mutex` (on its own cache line) and one vertex map bucket.
 *     UNIQUE lock for anything that changes the vertex set or the edge set of a
 *     vertex in that shard: create_vertex, add_vertex, remove_vertex, connect,
 *     disconnect, connect_bulk (epoch 3: every touched shard, once, ascending),
 *     reserve_links (and the private _connect_locked / _connect_resolved_locked /
 *     _disconnect_locked helpers).
 *     SHARED lock for pure reads and whole traversals: find, contains, roots,
 *     neighbors, dependencies, dependents, has_path, shortest_path,
 *     dirty_vertices, mark_dirty, clear_dirty, and the guid -> Vertex*
 *     resolution inside update_content. Any number of threads may hold one
 *     shard's shared lock at the same time; a structural writer of that shard
 *     waits for them and blocks new ones — but only for THAT shard.
 *     A reader of vertex V holds exactly one shard lock, and only long enough to
 *     copy what it needs (see NON-ALLOCATING TRAVERSALS below).
 *
 *   Level 2 — vertex content: one kimix::shared_atomic_mutex per Vertex
 *     (vertex.h). SHARED lock to read content/content_hash, UNIQUE lock to
 *     write content. Two different vertices never contend with each other, and a
 *     content write takes NO shard lock at all (update_content releases the
 *     shard lock before the vertex lock), so N threads updating N distinct
 *     vertices run fully in parallel.
 *
 * LOCK ORDER
 *   1. Shard locks: strictly ASCENDING shard index whenever more than one is
 *      taken. connect/disconnect take two (an edge's two endpoints) and
 *      connect_bulk (epoch 3) takes as many as its batch touches — but collects
 *      the set first and acquires it in ascending index, which is the same rule.
 *      The single-writer contract means at most one thread takes a multi-shard
 *      set at a time; the ascending order makes that safe anyway. remove_vertex
 *      takes exactly ONE shard unique lock at a time and releases it before
 *      moving to the next neighbour shard, so it never holds two.
 *   2. Shard lock first, vertex lock second — NEVER the other way round, and
 *      never acquire a shard lock while holding a vertex lock.
 *      connect/disconnect/connect_bulk (shard unique(s) + the two endpoints'
 *      vertex uniques), dirty_vertices (shard shared + vertex shared per vertex)
 *      and update_content (shard shared for the lookup, RELEASED, then one vertex
 *      unique) are the only places both levels meet; every other method holds
 *      exactly one level.
 *   3. The roots() cache lock (_roots_mutex) is OUTSIDE both levels and is never
 *      taken by a structural writer: writers only bump _structure_version with a
 *      relaxed atomic. roots() takes _roots_mutex and then one shard shared lock
 *      at a time while recomputing, so it can never be part of a shard-level
 *      cycle — no path takes _roots_mutex while holding a shard lock.
 *   4. EDGE shards (level 3, EdgeTable) are the one other level a READ takes: the
 *      EDGE-filtered walk tests ONE VISIT's candidate edges in a BATCH, which takes
 *      one edge-shard SHARED lock per TOUCHED shard in ASCENDING index — and only
 *      after that visit's vertex-shard (level 1) lock has been released. The batch
 *      never runs while a vertex is being visited, so it holds no vertex-shard
 *      lock: a level 1 -> level 3 acquisition is not a violation, it is the
 *      direction the hierarchy and the ascending shard numbering already permit
 *      (the structural write paths take it too — _create_edge_locked and
 *      _move_edge_endpoint in graph.cpp). What keeps the wait-for graph acyclic is
 *      the ABSENCE of the inverse, level 3 -> level 1: no edge-shard lock is ever
 *      held while a vertex-shard lock is acquired. Within level 3 the batch obeys
 *      the same ascending rule. See the multi-hop API block below for the walk's
 *      shape.
 *
 * DEADLOCK FREEDOM (the written-down argument)
 *   Every acquisition respects the hierarchy "shard level, then vertex level",
 *   and inside the shard level the strict ascending index order. A thread
 *   therefore only ever WAITS for a lock that is either (a) at a lower level than
 *   every lock it holds, or (b) at the same level but a strictly higher index. A
 *   wait-for cycle would need one edge that goes the other way; there is none,
 *   because no code path acquires a shard lock while holding a vertex lock, and
 *   none acquires a lower-index shard while holding a higher-index one. The
 *   vertex locks are additionally safe because the only multi-vertex path
 *   (connect/disconnect) is single-writer by contract, so its two uniques are
 *   never taken in opposite orders by two threads.
 *
 * READ ATOMICITY (what a traversal may observe)
 *   Per-vertex atomic, not whole-graph atomic. One vertex's guid list and
 *   pointer set are read under one shard lock and are therefore mutually
 *   consistent; a traversal that walks several vertices can interleave with a
 *   structural writer and may see the mutation on one visit and not on the next
 *   (a single BFS may, for example, miss an edge that was added while it ran).
 *   That is the price of the parallel BFS, and it is why epoch 1 is an
 *   optimization and not a free lunch: the epoch-0 whole-graph snapshot
 *   guaranteed every reader a consistent picture of the WHOLE graph, and no
 *   scheme that lets readers of different shards run in parallel can. The
 *   guarantees that DO hold are the ones a caller can act on:
 *     (a) a single call returns a self-consistent answer for its subject vertex
 *         (find returns the vertex that guid names, dependencies returns exactly
 *         one snapshot of that vertex's outgoing list);
 *     (b) structural WRITES remain single-threaded by contract, so at most one
 *         structural mutation is ever in flight and no traversal can observe an
 *         edge half-added by two competing writers;
 *     (c) it is MEMORY-SAFE against a concurrent remove_vertex. remove_vertex
 *         detaches the victim from the map, then unlinks it from each neighbour
 *         under that neighbour's shard lock, and only then returns the victim to
 *         its caller — the unique_ptr stays alive for the whole cascade, and the
 *         caller owns it afterwards. A reader that already resolved the victim
 *         therefore reads live memory (its guid, its content); a reader that
 *         visits it afterwards simply does not find it. No path dereferences a
 *         freed Vertex.
 *     (d) the ONE exception to "per visit" is the EDGE-filtered walk's edge
 *         TESTS: read atomicity is PER VISIT BATCH for edge tests (see the
 *         multi-hop API block below). That walk still copies one visited vertex's
 *         link list under one shard lock and releases it before the next visit —
 *         its vertex reads are per visit — but the candidate edges that ONE visit
 *         produced may be tested together, one edge-shard SHARED lock per touched
 *         shard, after that visit's vertex-shard lock is gone. A writer observed
 *         between the visit and its batch is the same anomaly as one observed
 *         between two visits: the batch changes WHEN an edge of that visit is
 *         tested and which state the test sees. In a structurally quiescent graph
 *         it never changes which vertices the walk discovers; in a graph that is
 *         NOT quiescent it can, because a wide visit's tests span the per-link head
 *         plus up to seven drains (64 + 256 + 1 024 x 4 + 520 on the 5 000-link
 *         hub), so a writer change landing between two drains can flip a verdict
 *         and with it a discovery. That is the SAME anomaly class as the
 *         inter-visit skew above — the batch introduces no new one — and neither
 *         memory safety nor determinism in a structurally quiescent graph is
 *         affected by it.
 *   Verified by a 8-reader / 1-writer stress harness (create + connect + cascade
 *   remove, 80k removals, readers resolving and reading vertices mid-removal):
 *   zero invariant violations, zero dangling edges, and a post-run sweep found
 *   every surviving edge resolving to a live vertex. ASan runs clean.
 *   Callers that need a consistent whole-graph picture must hold the graph
 *   structurally quiescent — which is exactly what the serializer's CALLER
 *   CONTRACT already requires (see serialize.h).
 *
 * WRITE POLICY — single writer by contract. Every structural write documents
 * "call from one thread at a time" and the implementation records the id of the
 * thread that performed the first structural write. A later structural write
 * from a DIFFERENT thread is REFUSED (it returns false / nullptr) and, in debug
 * builds only (-m debug / -m releasedbg, i.e. when NDEBUG is not defined), also
 * prints a diagnostic to stderr and trips an assert. Refusing rather than
 * proceeding is what keeps a contract violation a clean no-op instead of a racy
 * half-applied mutation; release builds stay silent about it. The recording
 * itself is always compiled (two atomics), so is_writer_thread() answers
 * correctly in every build mode. Reads are unrestricted: any number of threads
 * may read concurrently with each other.
 *
 * THE ONE EXCEPTION to "writers are a single thread": update_content is a
 * level-2 operation, so any number of threads may call it at the same time for
 * DIFFERENT vertices (that is exactly what the write benchmark exercises). It
 * does not touch the structure and therefore does not participate in the writer
 * contract; it takes one shard shared lock to resolve the guid and releases it
 * before touching the vertex.
 *
 * ===========================================================================
 * EPOCH-1 RESULT (why this scheme, measured)
 * ===========================================================================
 * A/B against the SAME code compiled with KIMIX_GRAPH_DB_SHARD_COUNT=1, which is
 * bit-for-bit the epoch-0 structure (one map, one shared_mutex); min-of-N over
 * the whole bench suite, interleaved to cancel host drift:
 *
 *   read  1T user op        +/-0 %      (sharding is not a 1-thread win; the
 *                                       costs and savings cancel)
 *   read 10/20/40T user op  -82/-92/-91 %  (aggregate 474k/416k/414k -> 2.23M/
 *                                       4.14M/3.95M ops/s)
 *   find-only 10/20/40T     -93/-96/-94 %  (65M 1T -> 103M/166M/138M aggregate,
 *                                       i.e. per-call 147ns -> 9.7/6.0/7.3ns: the
 *                                       contended-lock plateau is GONE)
 *   update_content 10/20/40T -80/-91/-90 %  (7.3M/6.1M/6.3M -> 34.6M/66.0M/60.6M)
 *   structural 1T            +7 %       (two shard locks vs one: the accepted
 *                                       cost, still single-threaded by contract)
 *   serialization            +/-8 %     (LMDB-bound; no regression)
 *
 * Correctness was not traded for it: the guarantee that narrowed is read
 * atomicity, from whole-graph snapshot to per-vertex atomicity, and that is
 * documented below with the memory-safety argument that still holds. Every test
 * in tests/unit/graph_db passes unmodified in debug, release and under ASan.
 *
 * ===========================================================================
 * EPOCH-2 RESULT (bulk-build hook, measured)
 * ===========================================================================
 * Epoch 2 attacked the serialization path; the only thing it changed in this
 * class is reserve_links() (below), which the deserializer's link pass uses to
 * size each vertex's four edge containers once from the degree counts the record
 * already carries. The measurement that motivated it: a 100k load spent 272 ms of
 * its 313 ms in the link pass, and of those 272 ms roughly 120 ms were connect()
 * plus 150 ms of per-vertex container growth. Interleaved same-binary A/B with the
 * reservation disabled, load of a 100k store, ms:
 *
 *   reserve on   241 / 237 / 248 / 250 / 253     (min 237)
 *   reserve off  262 / 267 / 293 / 301 / 328     (min 262)
 *
 * i.e. -10 % on the load with no change to what load produces (reserve() only
 * raises capacity). It is NOT a single-thread structural-write win: the
 * create+connect bench measures 951-1155 ns/op with and without a first-edge
 * reservation, i.e. the containers were never the structural cost (the MD5 in
 * create_vertex and the two shard locks are).
 *
 * ===========================================================================
 * EPOCH-3 RESULT (bulk edge creation + a cached roots(), measured)
 * ===========================================================================
 * The remaining load lever was the LINK pass, and profiling confirmed it was still
 * the whole load: a 100k store's load decomposed as scan 9-22 ms, slot build 2 ms,
 * decode 4 ms, materialize 6 ms, insert 10 ms, reserve sweep 16-28 ms, and the
 * EDGE SWEEP 183-283 ms. Inside that sweep, the six graph calls per edge cost
 * Graph::find ~77-80 ms and Graph::connect ~106-180 ms.
 *
 * connect_bulk() (below) collapses both: every distinct endpoint is resolved ONCE
 * (one shard SHARED lock per unique endpoint instead of one find per edge), every
 * touched shard's UNIQUE lock is taken ONCE for the whole batch in ascending order,
 * and the single-writer check runs once instead of per edge. Measured on the same
 * 100k store, min-of-N, the edge phase went 183 ms -> ~110 ms and the whole load
 * 234 ms -> 168-178 ms (see README §6 for the row).
 *
 * It changes NO result: _connect_resolved_locked is the ONE implementation of
 * "this edge exists", shared by connect() and connect_bulk(), so the duplicate
 * probe, the two-sided bookkeeping and the vertex-lock nesting are literally the
 * same code. A one-off equivalence harness re-linked a 1M store (6 001 843 edges)
 * through the old per-edge connect loop in a second graph and compared every
 * vertex's sorted outgoing and incoming guid lists: 0 adjacency mismatches, and the
 * same skipped-edge count. It was also the reason for one real fix: the first
 * version stored Vertex* into the staging map, and ankerl::unordered_dense moves
 * its values on rehash, which crashed.
 *
 * roots() is now cached behind _structure_version (below): 0.59 ms -> 0.003 ms at
 * 100k vertices (152-172x) with NO measurable structural-write cost (the bump is
 * one relaxed atomic increment; interleaved same-binary A/B, bump on vs off:
 * -0.06 % / +0.02 %).
 *
 * ===========================================================================
 * NON-ALLOCATING TRAVERSALS (epoch 1, item 2)
 * ===========================================================================
 * The pool of a nanosecond-scale read is the allocation, not the copy: the
 * depth-3/64-node bounded expansion of the benchmark issues one dependencies()
 * call per visited vertex, and the allocating form returns a fresh kimix::vector
 * every time. The traversal-friendly forms instead fill a CALLER-OWNED,
 * reusable buffer under one shard shared lock:
 *
 *   AdjacencyBuffer scratch;                       // one per thread / traversal
 *   if (graph.dependencies(guid, scratch)) {        // no allocation at all for
 *       for (auto &next : scratch) { ... }          // a vertex of degree <= 16
 *   }
 *
 * AdjacencyBuffer keeps 16 guids inline and reuses one heap vector beyond that,
 * so a graph of any shape costs at most one allocation per traversal thread
 * instead of one per visited vertex. size_t dependencies(guid, span<Guid>) is
 * the flat, caller-sized variant: it copies up to out.size() guids and returns
 * the TOTAL edge count, so count > out.size() means "your buffer was too small"
 * (the same convention as snprintf). has_path / shortest_path / neighbors use the
 * same discipline internally, and neighbors() dedups with a 32-entry stack
 * scratch instead of building a hash set per call.
 * The allocating dependencies() / dependents() / neighbors() forms stay for
 * compatibility and for callers that want an owning copy.
 *
 * ===========================================================================
 * EDGE DIRECTION
 * ===========================================================================
 * connect(from, to) records "from depends on to" (from -> to). Traversal
 * follows that direction: has_path/shortest_path walk outgoing dependency
 * edges, dependencies(guid) returns the outgoing links' neighbour guids, and
 * neighbors(guid) returns the union of both directions (an undirected
 * adjacency view).
 *
 * ROOTS
 * ===========================================================================
 * A root is a vertex nothing depends on, i.e. one with no incoming links
 * (the plan's §9 definition: sinks in dependency order — NOT the "vertex with
 * no outgoing edges" reading). EPOCH 3 made the set a VERSION-KEYED CACHE: the
 * O(n) shard walk (0.59 ms at 100k) happens only when a structural write has
 * happened since it was computed, and a warm call is a shared-lock version check
 * plus one vector copy (0.003 ms, 152-172x). The invalidation is a single relaxed
 * atomic increment inside the structural writer that already holds its shard
 * lock, so the write path pays no extra lock; level-2 content writes do NOT
 * invalidate (a root is defined by its incoming EDGES, which they cannot change).
 * The accessor still returns a snapshot vector rather than a reference, so the
 * cache cannot be observed mutating.
 *
 * ITERATION ORDER
 * ===========================================================================
 * roots(), dirty_vertices() and mark_dirty()/clear_dirty() walk the shards in
 * index order and each shard's map in insertion order, so an iteration is
 * deterministic (the same graph always produces the same order) — but it is now
 * shard order first, not one global insertion order, because the map is sharded.
 *
 * ===========================================================================
 * EPOCH 4 — FIRST-CLASS EDGES (the GDI alignment)
 * ===========================================================================
 * An edge is now an OBJECT (edge.h): uid, origin, target, direction, labels,
 * properties. Vertex adjacency became
 *
 *     kimix::vector<EdgeLink> _out_links, _in_links;   // {edge uid, neighbour}
 *
 * i.e. one entry per EDGE instead of one per neighbour, which is what makes
 * parallel edges, per-edge direction and delete-by-uid expressible at all.
 * PLAN V3 §5.2 made these link lists the SINGLE adjacency representation: the
 * neighbour guid lists (_depending_guids / _depended_guids) and the resolved
 * Vertex* sets (_depending / _depended) that used to sit alongside them as
 * maintained projections were triple storage of the same fact (~45 MB on the
 * graphbench 50k/300k dataset) and are gone; the guid-view reads
 * (dependencies()/dependents()/has_path()/neighbors()) now copy the neighbour
 * field out of the links under the same one-shard-lock snapshot, preserving
 * both the read shape and the outgoing-first order.
 *
 * The legacy connect()/disconnect()/connect_bulk() semantics are UNCHANGED and
 * are now implemented THROUGH the edge table: connect() creates one DIRECTED
 * edge, refuses a second edge between the same ordered pair, and refuses
 * from == to. create_edge() is the GDI entry point: it allows parallel edges and
 * (option-gated) self-loops, and it is the only place that does.
 *
 * LOCK HIERARCHY v2 (the full version is in README.md):
 *   1. Schema::_mutex          (owned by Database, never taken by Graph)
 *   2. vertex shards           (this class, ascending index)
 *   3. edge shards             (EdgeTable, ascending index)
 *   4. key-index shards        (KeyIndex, one at a time)
 *   5. leaf locks              (Vertex::_mutex — content/props/labels)
 * A structural write holds 2 -> 3 (or 2 -> 4) in that order and never a leaf
 * lock; a payload write takes 2 shared to resolve, RELEASES it, then a leaf
 * lock; a payload read takes the edge shard shared (edge payloads are guarded by
 * their shard, not by a per-edge mutex).
 *
 * Example:
 *   kimix::graph_db::Graph graph;
 *   auto *a = graph.create_vertex(bytes_a);
 *   auto *b = graph.create_vertex(bytes_b);
 *   graph.connect(a->guid(), b->guid());   // a depends on b
 *   graph.update_content(b->guid(), bytes_b2); // vertex lock, no shard lock held
 *   auto roots = graph.roots();            // {a}
 *   bool reachable = graph.has_path(a->guid(), b->guid()); // true
* kimix::graph_db::AdjacencyBuffer deps;
 * if (graph.dependencies(a->guid(), deps)) {
 *   for (auto const &next : deps) { ... }   // next is one dependency guid
 * }
 */
#pragma once

#include "graph_db_api.h"
#include "edge.h"
#include "graph_db_options.h"
#include "key.h"
#include "property.h"
#include "query.h"
#include "schema.h"
#include "status.h"
#include "vertex.h"

#include <core/guid.h>
#include <core/hash_map.h>
#include <core/memory.h>
#include <core/md5.h>
#include <core/stl/memory.h>
#include <core/stl/optional.h>
#include <core/stl/string.h>
#include <core/stl/unordered_map.h>
#include <core/stl/vector.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>

namespace kimix::graph_db {

// Number of vertex-map shards. A power of two so the shard index is a mask, and
// large enough that 20 reader threads hit mostly distinct shard locks (64 lines
// of contention instead of one). See the contract in the header comment.
//
// KIMIX_GRAPH_DB_SHARD_COUNT overrides it for diagnostics only: building with
// -DKIMIX_GRAPH_DB_SHARD_COUNT=1 yields exactly the epoch-0 single-lock structure
// (one map, one shared_mutex) on the current code, which is how an epoch's
// lock-sharding change is A/B measured on one machine in one machine state.
// Nothing may depend on the value: a shard is an implementation detail, and the
// only observable effect of a different count is contention, never a result.
#ifndef KIMIX_GRAPH_DB_SHARD_COUNT
  #define KIMIX_GRAPH_DB_SHARD_COUNT 64
#endif
inline constexpr size_t kGraphShardCount = KIMIX_GRAPH_DB_SHARD_COUNT;
static_assert(kGraphShardCount > 0 &&
                  (kGraphShardCount & (kGraphShardCount - 1)) == 0,
              "kGraphShardCount must be a power of two (the shard index is a mask)");
// log2 of kGraphShardCount: the shard index is the top that many bits of the
// mixed guid hash (see Graph::_shard_of).
static_assert(kGraphShardCount == kEdgeShardCount && kGraphShardCount == kKeyShardCount,
              "vertex, edge and key shard counts must match: the lock-order argument "
              "(level 2 -> 3 -> 4, ascending index) is written down once, for one count");
inline constexpr unsigned kGraphShardShift =
    kGraphShardCount == 1    ? 0u
    : kGraphShardCount == 2  ? 1u
    : kGraphShardCount == 4  ? 2u
    : kGraphShardCount == 8  ? 3u
    : kGraphShardCount == 16 ? 4u
    : kGraphShardCount == 32 ? 5u
    : kGraphShardCount == 64 ? 6u
    : kGraphShardCount == 128 ? 7u
    : kGraphShardCount == 256 ? 8u
                             : 9u;
static_assert(kGraphShardCount <= 512,
              "kGraphShardShift handles up to 512 shards; extend the table");

// One entry of Graph::dirty_vertices(): everything the incremental serializer
// needs about a vertex that changed since the last save.
struct KIMIX_GRAPH_DB_API DirtyVertex {
    Vertex *vertex{nullptr};
    kimix::Guid guid;
    kimix::MD5 content_hash;
};

/*
 * One requested edge for Graph::connect_bulk(): "from depends on to", the same
 * meaning an individual Graph::connect(from, to) has.
 */
struct KIMIX_GRAPH_DB_API EdgeRequest {
    kimix::Guid from;
    kimix::Guid to;
};

/*
 * What Graph::connect_bulk() did with one EdgeRequest. Every value other than
 * `created` means "no edge was added", and a bulk caller counts them exactly the
 * way it would have counted a false return from Graph::connect():
 *
 *   created          the edge now exists in both directions (both forms)
 *   duplicate_edge   from already depends on to, i.e. the very edge this request
 *                    names is present (the SAME outgoing probe connect() uses —
 *                    a stray incoming-only bookkeeping entry does not refuse it,
 *                    matching connect()'s behaviour to the letter)
 *   missing_source   `from` is not in this graph
 *   missing_target   `to` is not in this graph
 *   self_edge        from == to, which is never an edge
 *   refused          the whole call was refused (single-writer contract), so
 *                    nothing at all was applied
 */
enum class EdgeOutcome : uint8_t {
    created = 0,
    duplicate_edge,
    missing_source,
    missing_target,
    self_edge,
    refused,
};

/*
 * Reusable adjacency scratch for the non-allocating traversal API. Declare one
 * per traversal thread and pass it to Graph::dependencies(guid, buffer) /
 * dependents(guid, buffer):
 *
 *   kimix::graph_db::AdjacencyBuffer deps;
 *   while (...) {
 *       if (!graph.dependencies(guid, deps)) { continue; }
 *       for (auto const &next : deps) { ... }   // begin()/end() range-for
 *   }
 *
 * The buffer keeps kInlineCapacity guids inline and only falls back to a heap
 * vector (whose capacity is retained across calls) for a vertex with a larger
 * degree, so a traversal of a normal-degree graph allocates nothing at all. It
 * is non-copyable and non-movable on purpose: it points at its own inline array.
 */
class KIMIX_GRAPH_DB_API AdjacencyBuffer {
public:
    static constexpr size_t kInlineCapacity = 16;

    AdjacencyBuffer() noexcept = default;
    AdjacencyBuffer(AdjacencyBuffer const &) = delete;
    AdjacencyBuffer &operator=(AdjacencyBuffer const &) = delete;
    AdjacencyBuffer(AdjacencyBuffer &&) = delete;
    AdjacencyBuffer &operator=(AdjacencyBuffer &&) = delete;
    ~AdjacencyBuffer() noexcept = default;

    [[nodiscard]] size_t size() const noexcept { return _size; }
    [[nodiscard]] bool empty() const noexcept { return _size == 0; }
    [[nodiscard]] kimix::Guid const *data() const noexcept { return _data; }
    [[nodiscard]] kimix::Guid const *begin() const noexcept { return _data; }
    [[nodiscard]] kimix::Guid const *end() const noexcept { return _data + _size; }
    [[nodiscard]] kimix::Guid const &operator[](size_t index) const noexcept {
        return _data[index];
    }

private:
    friend class Graph;

    void _clear() noexcept {
        _size = 0;
        _data = _local;
    }

    // Called by Graph under the owning shard's SHARED lock, so `src` cannot
    // change while it is copied. Never allocates while src.size() <=
    // kInlineCapacity.
    void _assign(kimix::vector<kimix::Guid> const &src) {
        _size = src.size();
        if (_size <= kInlineCapacity) {
            _data = _local;
            std::copy(src.begin(), src.end(), _local);
        } else {
            _heap.assign(src.begin(), src.end()); // reuses the retained capacity
            _data = _heap.data();
        }
    }

    // The undirected-visit form: both edge lists concatenated (outgoing first).
    // Duplicates between the two lists are left in — the caller's dedup removes
    // them — because the one-lock snapshot matters more than the copy size.
    void _assign_two(kimix::vector<kimix::Guid> const &first,
                     kimix::vector<kimix::Guid> const &second) {
        _size = first.size() + second.size();
        if (_size <= kInlineCapacity) {
            _data = _local;
            std::copy(first.begin(), first.end(), _local);
            std::copy(second.begin(), second.end(), _local + first.size());
        } else {
            _heap.clear(); // retains capacity across calls
            _heap.reserve(_size);
            _heap.insert(_heap.end(), first.begin(), first.end());
            _heap.insert(_heap.end(), second.begin(), second.end());
            _data = _heap.data();
        }
    }

    // The link-list forms (plan v3 §5.2): the EdgeLink vectors are the one
    // adjacency representation, and the guid view this buffer exposes is
    // their neighbour field — copied under the same single shard lock, same
    // order, same inline/heap split as the guid-list forms above.
    void _assign_links(kimix::vector<EdgeLink> const &src) {
        _size = src.size();
        if (_size <= kInlineCapacity) {
            _data = _local;
            for (size_t i = 0; i < _size; ++i) {
                _local[i] = src[i].neighbour;
            }
        } else {
            _heap.clear(); // retains capacity across calls
            _heap.reserve(_size);
            for (auto const &link : src) {
                _heap.push_back(link.neighbour);
            }
            _data = _heap.data();
        }
    }

    void _assign_two_links(kimix::vector<EdgeLink> const &first,
                           kimix::vector<EdgeLink> const &second) {
        _size = first.size() + second.size();
        if (_size <= kInlineCapacity) {
            _data = _local;
            kimix::Guid *cursor = _local;
            for (auto const &link : first) {
                *cursor++ = link.neighbour;
            }
            for (auto const &link : second) {
                *cursor++ = link.neighbour;
            }
        } else {
            _heap.clear(); // retains capacity across calls
            _heap.reserve(_size);
            for (auto const &link : first) {
                _heap.push_back(link.neighbour);
            }
            for (auto const &link : second) {
                _heap.push_back(link.neighbour);
            }
            _data = _heap.data();
        }
    }

    kimix::Guid _local[kInlineCapacity]{};
    kimix::Guid const *_data{_local};
    size_t _size{0};
    kimix::vector<kimix::Guid> _heap; // only for a vertex exceeding kInlineCapacity
};

// Candidates one batched edge-filter DRAIN stages at a time. The batch may be handed
// a whole visit's link list (a hub's is thousands of links) and drains it in chunks
// of this size, so the scratch it needs stays at "order 4 B + match 1 B per
// candidate" (~5 KB) however wide the visit is. 1024 is the documented default from
// the measured batching curve (6.65x at 1024 candidates, 8.39x at 4096): the curve
// has flattened, so 4x less staging is worth the last 20 % — a deliberate
// peak-for-time trade, recorded in README §5h.
inline constexpr size_t kEdgeFilterChunk = 1024;

/*
 * Reusable scratch for the batched edge filter (Graph::_filter_edges_batched — the
 * filtered walk's level-3 phase). One per walk, passed in and reused across every
 * drain of every level, so a walk whose drains fit the inline budget allocates
 * nothing for it:
 *
 *   kimix::graph_db::EdgeFilterScratch scratch;   // a stack local of the walk
 *
 * A drain counting-sorts its candidates by EdgeTable::shard_of() in ONE pass that
 * also records the TOUCHED shards (it never scans all 64, which is what makes the
 * batch a win even at a handful of candidates), prefixes over that touched list
 * only, and then takes ONE edge-shard SHARED lock per touched shard in ascending
 * index. `_order` holds one candidate index (4 B) per candidate of the drain; the
 * first kInlineCapacity live in the object itself, a wider drain falls back to a
 * heap vector whose capacity is retained for the rest of the walk.
 *
 * Level-3 scratch only: it carries no vertex-shard state and gives the caller no
 * way to hold an edge-shard lock across a visit.
 */
class KIMIX_GRAPH_DB_API EdgeFilterScratch {
public:
    // Candidate indices staged inline (256 B). A drain of a normal level fits; the
    // hub fixtures (degree 5 000) grow the heap array once per walk.
    static constexpr size_t kInlineCapacity = 64;
    EdgeFilterScratch() noexcept = default;
    EdgeFilterScratch(EdgeFilterScratch const &) = delete;
    EdgeFilterScratch &operator=(EdgeFilterScratch const &) = delete;
    EdgeFilterScratch(EdgeFilterScratch &&) = delete;
    EdgeFilterScratch &operator=(EdgeFilterScratch &&) = delete;
    ~EdgeFilterScratch() noexcept = default;

    // How many candidate indices the order array can hold without reallocating.
    [[nodiscard]] size_t order_capacity() const noexcept { return _capacity; }

private:
    friend class Graph;
    // The order array for a drain of `count` candidates, growing on demand. The
    // heap array keeps its capacity, so only the first wide drain of a walk pays.
    [[nodiscard]] uint32_t *_order_for(size_t count) {
        if (count > _capacity) {
            _order_heap.resize(count);
            _order = _order_heap.data();
            _capacity = _order_heap.size();
        }
        return _order;
    }
    uint32_t _order_inline[kInlineCapacity]{};
    uint32_t *_order{_order_inline};
    size_t _capacity{kInlineCapacity};
    kimix::vector<uint32_t> _order_heap; // only for a drain wider than kInlineCapacity
    // Per-drain bookkeeping, all indexed by shard and touched only for the shards
    // the drain actually touches: the per-shard counts, each bucket's first slot,
    // each bucket's next free slot, and the touched list itself (kept in ascending
    // index, which is the contract's lock order).
    uint32_t _count[kEdgeShardCount]{};
    uint32_t _begin[kEdgeShardCount]{};
    uint32_t _cursor[kEdgeShardCount]{};
    uint32_t _touched[kEdgeShardCount]{};
    uint32_t _touched_count{0};
};

// --- optional walk instrumentation (measurement scaffolding, not API) --------
// The EDGE-filtered walk's cost is dominated by two things a wall clock cannot
// attribute: how many vertices it locked, and how many candidate links it pushed
// through the edge filter. This is the counter record that attributes them — the
// input of tests/unit/graph_db/test_graph_db_bench_edges.cpp's [ctr] rows.
//
// OFF BY DEFAULT and compiled out ENTIRELY: the structure, the accessor and every
// increment live behind KIMIX_GRAPH_DB_WALK_COUNTERS (xmake option
// `graph_db_walk_counters`), so a default build carries no counter, no
// thread_local access and no hot-path cost at all — the [bench] numbers of a
// default build are the record, and the [ctr] numbers exist only in an
// instrumented build. Nothing here changes a result, a lock order or a return
// value; a walk that early-returns (error_argument / error_op / error_vertex /
// zero-length path) leaves the record untouched, which is itself an assertion the
// benchmark makes.
//
// Thread-local, so the CALLING thread reads right after its own call: reset with
// reset_walk_counters(), call the walk, read walk_counters(). Several threads may
// each hold their own record; the library never merges them.
#if defined(KIMIX_GRAPH_DB_WALK_COUNTERS)
struct KIMIX_GRAPH_DB_API WalkCounters {
    // One oriented_links snapshot per VISITED vertex (the level-1 shard lock).
    uint64_t vertex_locks{0};
    // Edge-shard SHARED lock acquisitions on the level-3 paths: one per
    // edge_matches() call, and — in the filtered walk's BATCH — one per TOUCHED
    // shard per drain, i.e. fewer than one per candidate. This is the counter that
    // proves the batching: edge_locks <= edge_tests, and after the batch
    // edge_locks <= touched shards per drain <= 64 per drain.
    uint64_t edge_locks{0};
    // Candidate EDGES actually pushed through the filter, i.e. one matches_probe
    // evaluation each — NOT every iterated link. The two paths differ here, by
    // design: the PER-LINK path gates the filter behind the visited check (O1), so its
    // edge_tests is a strict subset of the links it iterated (that denominator is
    // visited_checks below); a drained CHUNK stages and probes EVERY one of its
    // candidates and only then runs the visited check at consume time, so a candidate
    // whose neighbour an earlier candidate of the same chunk discovered is still
    // probed — one probe the per-link path would have skipped. That is why [ctr] must
    // be read per row, and why kEdgeFilterMinLinks keeps the common path off the batch.
    uint64_t edge_tests{0};
    // Links copied out of the vertices' adjacencies (the snapshot volume).
    uint64_t links_snapshotted{0};
    // Visited-set probes, ONE PER ITERATED LINK: this is the "candidates" of the
    // cost model (it equals the pre-O1 `candidates` count on every row). The batch's
    // re-check of a matching hop at consume time is internal bookkeeping and is not
    // counted again, so the denominator stays comparable across the epochs.
    uint64_t visited_checks{0};
    // Bitmask of the edge shards whose lock was taken at least once — its
    // popcount is the "shards" of the model, the denominator of any batching
    // efficiency claim.
    uint64_t edge_shards{0};
};
// The calling thread's record. The library never clears it: the caller resets
// before the call it wants to attribute.
[[nodiscard]] KIMIX_GRAPH_DB_API WalkCounters &walk_counters() noexcept;
inline void reset_walk_counters() noexcept { walk_counters() = WalkCounters{}; }
static_assert(kEdgeShardCount <= 64,
              "WalkCounters::edge_shards is a uint64_t bitmask of the edge shards");
#endif // KIMIX_GRAPH_DB_WALK_COUNTERS

class KIMIX_GRAPH_DB_API Graph : public kimix::IOperatorNewBase {
public:
    Graph() noexcept = default;
    Graph(Graph const &) = delete;
    Graph &operator=(Graph const &) = delete;
    Graph(Graph &&) = delete;
    Graph &operator=(Graph &&) = delete;
    ~Graph() noexcept;

    // --- structure (one or two shard UNIQUE locks; single writer by contract) -

    // Create a vertex with a freshly generated guid and store it. The returned
    // pointer stays valid until the vertex is removed; nullptr is only possible
    // for a (astronomically unlikely) guid collision or a refused foreign
    // writer thread.
    Vertex *create_vertex(kimix::span<const std::byte> initial_content = {});
    // Adopt an already built vertex. Returns false (leaving `vertex` with the
    // caller) when its guid is already present or `vertex` is null.
    bool add_vertex(kimix::unique_ptr<Vertex> vertex);
    // Remove `guid` and CASCADE: the removed vertex is unlinked from every
    // neighbour's pointer set and guid list, so no dangling edge survives. The
    // cascade takes the neighbours' shard locks one at a time (never two), and
    // the returned vertex stays alive for the whole cascade, so a concurrent
    // reader can never observe freed memory. The removed vertex is returned
    // (nullptr when `guid` is unknown) so the caller can persist or resurrect it.
    kimix::unique_ptr<Vertex> remove_vertex(kimix::Guid const &guid);
    // Transaction-aware removal: the cascade hands back the EDGE objects it
    // deleted (in `removed_edges`, when non-null) instead of discarding them, so
    // an aborting transaction can re-adopt them verbatim.
    kimix::unique_ptr<Vertex>
    remove_vertex(kimix::Guid const &guid,
                  kimix::vector<kimix::unique_ptr<Edge>> *removed_edges);
    // Record "from depends on to". False when either guid is unknown, when
    // from == to, or when the edge already exists. Takes the one or two
    // endpoint shards' unique locks in ascending shard order.
    bool connect(kimix::Guid const &from, kimix::Guid const &to);
    // Drop the "from depends on to" edge. False when it does not exist.
    bool disconnect(kimix::Guid const &from, kimix::Guid const &to);

    // --- bulk edge creation (epoch 3, the loader's link pass) ----------------
    /*
     * Create a whole batch of edges in ONE structural pass. Semantically this is
     * exactly `for (auto &edge : edges) connect(edge.from, edge.to);` — same
     * resulting state, same per-edge accept/refuse decisions — but it eliminates
     * the two costs that made a per-edge loop 78 % of a cold load:
     *
     *   1. ENDPOINT RESOLUTION ONCE. Every distinct endpoint guid is resolved to
     *      its Vertex* a single time, at the start, under one SHARED lock per
     *      touched shard (a caller passing the vertices it just inserted pays
     *      "one lookup per unique endpoint" instead of "one Graph::find per
     *      edge"); the resolved (Vertex*, shard) pairs are then reused by every
     *      edge of the batch, so the per-edge resolution steps disappear.
     *   2. LOCK CHOREOGRAPHY HOISTED. The batch takes each touched shard's UNIQUE
     *      lock exactly ONCE, in strict ascending shard order, and keeps every
     *      one of them for the whole batch — so an edge whose two endpoints share
     *      a shard needs no order question at all, and no edge pays an
     *      acquire/release pair. The single-writer check runs ONCE for the batch
     *      instead of per edge (a bulk load is a single-threaded phase by
     *      contract, see WRITE POLICY).
     *
     * Everything else is unchanged: the same duplicate probe on the source,
     * the same two vertex UNIQUE locks per created edge (shard before
     * vertex, never the inverse), and the same _add_out_link/_add_in_link
     * bookkeeping, so both directions of the link lists are filled.
     * `outcomes` receives one verdict per request,
     * in request order, when its size is at least `count`; it is OPTIONAL (the
     * default empty span discards the verdicts) because a caller that only needs
     * the created count should not pay for them.
     *
     * The batch is ATOMIC with respect to the writer contract but NOT a
     * transaction: an edge that is refused for its own reason (self edge,
     * missing endpoint, already present) leaves the others applied, exactly as
     * the per-edge connect loop would have. A `refused` outcome (the calling
     * thread is not the graph's writer thread) applies NOTHING — the whole batch
     * is skipped, and `count` is returned as applied.
     *
     * DEADLOCK FREEDOM: same argument as connect/disconnect, only wider. Every
     * shard lock the batch needs is taken on entry in ascending index order and
     * released on exit, so a bulk caller waits only for locks at its own level
     * with a strictly higher index, and the single-writer contract keeps a
     * second bulk caller (or a connect) out of the same set anyway. No vertex
     * lock is ever held while a shard lock is taken, so the level order holds
     * too. Return: how many edges were CREATED.
     */
    size_t connect_bulk(kimix::span<EdgeRequest const> edges,
                        kimix::span<EdgeOutcome> outcomes = {}) noexcept;

    // Epoch-2 bulk-build hook: reserve room for `outgoing` outgoing and `incoming`
    // incoming edges on `guid` BEFORE they are created. A caller that knows the
    // final degree of its vertices (the deserializer's link pass — every record
    // carries both counts) can size the four edge containers once instead of
    // paying a geometric reallocation per edge: every vertex otherwise starts at
    // the 16-slot minimum that kimix::unordered_set, kimix::vector and
    // kimix::HashMap round a first insert up to. No-op (returns false) for an
    // unknown guid. Only ever RAISES capacity, never shrinks and never touches an
    // element, so it cannot change a single edge or traversal result.
    // A structural write: takes that ONE shard's UNIQUE lock and is subject to the
    // single-writer contract like every other structural mutation.
    bool reserve_links(kimix::Guid const &guid, size_t outgoing, size_t incoming);

    // --- options (key mode, edge policy) ------------------------------------
    void set_options(GraphOptions const &options) noexcept { _options = options; }
    [[nodiscard]] GraphOptions const &options() const noexcept { return _options; }

    // --- GDI-shaped vertex creation (P1/P2) ---------------------------------
    struct VertexSpec {
        // Primary key. `key_label` == kNoLabel creates an unkeyed vertex (the
        // container API's shape); a keyed one is indexed and must be unique.
        LabelId key_label{kNoLabel};
        kimix::span<const std::byte> external_id;
        kimix::span<const LabelId> labels;
        // Optional payload (already schema-validated by the Database).
        PropertyList const *properties{nullptr};
        kimix::span<const std::byte> content;
    };

    // Create a vertex with a key, labels and properties in ONE structural write.
    // error_empty_name for an empty external id on a keyed vertex,
    // error_non_unique_id for a taken key (GraphOptions::key_mode ==
    // report_warning turns that into warning_non_unique_id plus the EXISTING
    // vertex's uid, which is GDI's under-enforced-uniqueness path).
    [[nodiscard]] GdiStatus create_vertex(VertexSpec const &spec, kimix::Guid &out_guid);
    // MERGE: match an existing key (no mutation at all, per the C# reference) or
    // create a fresh vertex. `created` says which happened.
    [[nodiscard]] GdiStatus get_or_create_vertex(VertexSpec const &spec, kimix::Guid &out_guid,
                                                 bool &created);
    // Key -> uid. `found == false` is NOT an error (GDI's TranslateVertexID
    // returns a nullable uid).
    [[nodiscard]] GdiStatus translate_vertex_id(VertexKeyView const &key, kimix::Guid &out_guid,
                                                bool &found) const;
    // Delete a vertex. `guarded` (GDI's FreeVertexGuarded) refuses with
    // error_edge while any incident edge exists; the cascading form (GDI's
    // FreeVertex, DETACH DELETE) removes every incident edge first, in either
    // direction. error_vertex when the guid is unknown.
    [[nodiscard]] GdiStatus free_vertex(kimix::Guid const &guid, bool guarded);

    // --- first-class edges (P3) ---------------------------------------------
    // Create an edge between two EXISTING vertices. `allow_parallel` and
    // `allow_self_loop` come from GraphOptions for the GDI entry point; the legacy
    // connect() passes false for both, which is exactly its old contract.
    // error_vertex for a missing endpoint, error_edge for a refused self-loop,
    // error_edge_orientation for an unknown direction value.
    [[nodiscard]] GdiStatus edge_create(kimix::Guid const &origin, kimix::Guid const &target,
                                        EdgeDirectionType direction,
                                        kimix::span<const LabelId> labels,
                                        PropertyList const *properties, kimix::Guid &out_edge);
    [[nodiscard]] GdiStatus edge_free(kimix::Guid const &edge);
    [[nodiscard]] GdiStatus edge_set_origin(kimix::Guid const &edge, kimix::Guid const &origin);
    [[nodiscard]] GdiStatus edge_set_target(kimix::Guid const &edge, kimix::Guid const &target);
    [[nodiscard]] GdiStatus edge_set_direction(kimix::Guid const &edge,
                                               EdgeDirectionType direction);
    // Payload of one edge. Labels/properties are guarded by the edge's shard, so
    // these take exactly that one lock (shared to read, unique to write).
    [[nodiscard]] GdiStatus edge_add_label(kimix::Guid const &edge, LabelId label);
    [[nodiscard]] GdiStatus edge_remove_label(kimix::Guid const &edge, LabelId label);
    [[nodiscard]] bool edge_labels(kimix::Guid const &edge, kimix::vector<LabelId> &out) const;
    [[nodiscard]] GdiStatus edge_add_property(kimix::Guid const &edge, PropNameId name,
                                              PropertyValue value);
    [[nodiscard]] GdiStatus edge_set_property(kimix::Guid const &edge, PropNameId name,
                                              PropertyValue value);
    [[nodiscard]] GdiStatus edge_put_property(kimix::Guid const &edge, PropNameId name,
                                              PropertyValue value);
    [[nodiscard]] GdiStatus edge_remove_properties(kimix::Guid const &edge,
                                                   kimix::span<PropNameId const> names);
    [[nodiscard]] bool edge_properties(kimix::Guid const &edge, PropertyList &out) const;
    [[nodiscard]] bool edge_property(kimix::Guid const &edge, PropNameId name,
                                     PropertyValue &out) const;
    // Read-backs (a copy each, one shard lock per call).
    [[nodiscard]] bool edge_record(kimix::Guid const &edge, EdgeRecord &out) const;
    [[nodiscard]] bool edge_endpoints(kimix::Guid const &edge, kimix::Guid &origin,
                                      kimix::Guid &target) const;
    // Incident edge ids of `vertex`, oriented. One vertex-shard shared lock; the
    // ids are then resolved by the caller (never two shard locks at once).
    void incident_edges(kimix::Guid const &vertex, EdgeOrientation orientation,
                        kimix::vector<EdgeId> &out) const;
    // True when an edge origin -> target exists (one vertex shard shared lock).
    [[nodiscard]] bool has_edge(kimix::Guid const &origin, kimix::Guid const &target) const;
    // True when `vertex` has at least one incident edge in `orientation` — the
    // guarded-delete probe. Takes ONE vertex shard shared lock.
    [[nodiscard]] bool has_incident_edges(kimix::Guid const &vertex, EdgeOrientation orientation) const;
    [[nodiscard]] EdgeTable const &edges() const noexcept { return _edges; }
    [[nodiscard]] EdgeTable &edges() noexcept { return _edges; }
    // Dirty EDGE records for the incremental serializer (one shard shared lock at
    // a time, deterministic order).
    [[nodiscard]] kimix::vector<EdgeRecord> dirty_edges() const;
    // --- deletion tombstones (the save path's purge) -------------------------
    // A save is a WRITE transaction: it never removes a stored key (see
    // serialize.h). A DELETED entity therefore has to be tombstoned, or the next
    // load would resurrect it. Graph records the ids this graph removed since the
    // last save; GraphSerializer writes an EMPTY value under each key in the same
    // transaction as the records, and the loader treats an empty value as "this
    // entity is gone" (silently, not as a corrupt record).
    // Writer-only state (single-writer contract), so no lock is taken.
    // take_*() moves the lists out and empties them; adopt_vertex / adopt_edge
    // REMOVE an id from them again, so an aborted transaction that re-adopts a
    // deleted entity cannot tombstone a live one.
    [[nodiscard]] kimix::vector<kimix::Guid> take_removed_vertices();
    [[nodiscard]] kimix::vector<EdgeId> take_removed_edges();
    void restore_removed_vertex(kimix::Guid const &guid);
    void restore_removed_edge(EdgeId const &edge);
    [[nodiscard]] size_t removed_vertex_count() const noexcept { return _removed_vertices.size(); }
      [[nodiscard]] size_t removed_edge_count() const noexcept { return _removed_edges.size(); }

    // Every vertex guid / every edge id, copied one shard at a time (shard index,
    // then map insertion order — deterministic). O(n); used by the schema
    // cascades and by the statistics the tests assert on, never on a hot path.
    [[nodiscard]] kimix::vector<kimix::Guid> vertex_guids() const;
    [[nodiscard]] kimix::vector<EdgeId> edge_ids() const;
    // Overwrite an edge's labels and properties in one step (the transaction
    // rollback path: it restores a payload exactly, without pretending to be a
    // sequence of add/remove calls).
    void edge_restore_payload(EdgeId const &edge, kimix::span<const LabelId> labels,
                              PropertyList const &properties);
    // The key index (level 4). Exposed for the database facade's statistics and
    // for the tests' leak checks.
    [[nodiscard]] KeyIndex const &key_index() const noexcept { return _keys; }
    // Degree hints of one vertex (its out/in link counts), copied under one
    // shard shared lock — what a save writes as the vertex record's reserve hint.
    [[nodiscard]] bool link_counts(kimix::Guid const &guid, size_t &outgoing,
                                   size_t &incoming) const;

    // --- loader-only adoption (P1/P3) ---------------------------------------
    // Adopt a vertex that is ALREADY fully built (key, labels, properties and
    // content on it) and index its key. error_vertex when the guid is present,
    // error_non_unique_id when the key is (a store whose records disagree — the
    // load skips such a record and counts it). Structural write.
    [[nodiscard]] GdiStatus adopt_vertex(kimix::unique_ptr<Vertex> vertex);
    // Adopt an already built edge record and link both endpoints. error_vertex
    // when an endpoint is not in the graph (a dangling edge: the load skips and
    // counts it), error_edge for a self loop the options forbid.
    [[nodiscard]] GdiStatus adopt_edge(kimix::unique_ptr<Edge> edge);

    // --- level-2 write (VERTEX unique lock only, no shard lock held) --------
    // Replace one vertex's content. The vertex's shard is taken SHARED only to
    // resolve guid -> Vertex* and released before the vertex lock, so /N/
    // threads writing /N/ distinct vertices never serialize on a shard lock —
    // the no-false-sharing payoff. False when `guid` is unknown.
    // Callers must not remove a vertex concurrently with an update of it (the
    // single-writer contract forbids it); see the contract section above.
    bool update_content(kimix::Guid const &guid,
                        kimix::span<const std::byte> content);

    // --- reads and traversals (ONE shard SHARED lock per vertex visit) ------

    // Resolve a guid. The pointer is valid until the vertex is removed; taking
    // it does NOT keep the vertex alive, and holding it across a structural
    // write is the caller's race to manage (the shard lock protects the map, not
    // the caller's raw pointer).
    [[nodiscard]] Vertex *find(kimix::Guid const &guid) const;
    [[nodiscard]] bool contains(kimix::Guid const &guid) const;
    // Number of vertices. Maintained as an atomic on every insert/remove, so this
    // takes no lock at all.
    [[nodiscard]] size_t size() const noexcept {
        return _size.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    // Vertices nothing depends on (no incoming edge), in shard order and, within
    // a shard, in map insertion order (see ITERATION ORDER).
    //
    // EPOCH 3 keeps the answer cached, because computing it is O(n) with one shard
    // lock acquisition each (1.0 ms at 100k vertices) and a read loop that asks for
    // it repeatedly paid that every time:
    //   - the cache is keyed by a `_structure_version` counter that every
    //     structural write (create/add/remove vertex, connect, disconnect,
    //     connect_bulk, reserve_links) bumps with ONE relaxed atomic increment —
    //     no extra lock, no extra acquisition on the write path;
    //   - a read compares that version against the cached one with a relaxed load
    //     (lock-free fast path, and a REUSE of the returned vector's storage);
    //   - only a stale cache takes the dedicated `_roots_mutex` and recomputes
    //     the set the old way (shard by shard, shared locks, no vertex lock), so
    //     concurrent readers of a stale cache compute it once and the rest reuse.
    // The returned vector is still a SNAPSHOT copy: the caller owns it.
    // Level-2 content writes and update_content do NOT bump the version, because
    // they cannot change the root set (a root is defined by its incoming EDGES).
    [[nodiscard]] kimix::vector<Vertex *> roots() const;

    // --- adjacency, allocating forms (compatibility) ------------------------
    // Each returns a freshly allocated copy; prefer the buffer forms below in a
    // traversal loop (epoch 1 measured ~30 such allocations per user op).
    // Undirected adjacency: outgoing + incoming edges, deduplicated by guid.
    [[nodiscard]] kimix::vector<kimix::Guid> neighbors(kimix::Guid const &guid) const;
    // Outgoing edges only: the vertices `guid` depends on.
    [[nodiscard]] kimix::vector<kimix::Guid> dependencies(kimix::Guid const &guid) const;
    // Incoming edges only: the vertices that depend on `guid`.
    [[nodiscard]] kimix::vector<kimix::Guid> dependents(kimix::Guid const &guid) const;

    // --- adjacency, NON-ALLOCATING forms (epoch 1) --------------------------
    // Fill a caller-owned, reusable buffer under ONE shard shared lock. Return
    // false when `guid` is unknown (the buffer is then empty). No allocation at
    // all while the degree stays <= AdjacencyBuffer::kInlineCapacity.
    [[nodiscard]] bool dependencies(kimix::Guid const &guid,
                                    AdjacencyBuffer &out) const noexcept;
    [[nodiscard]] bool dependents(kimix::Guid const &guid,
                                  AdjacencyBuffer &out) const noexcept;
    // Flat variant for a caller-sized buffer: copy up to out.size() guids into
    // `out` and return the TOTAL number of edges, so a return value greater than
    // out.size() means the buffer was too small (snprintf convention). Always
    // allocates nothing.
    [[nodiscard]] size_t dependencies(kimix::Guid const &guid,
                                      kimix::span<kimix::Guid> out) const noexcept;
  [[nodiscard]] size_t dependents(kimix::Guid const &guid,
                                  kimix::span<kimix::Guid> out) const noexcept;
  // Oriented form of the buffer reads above (plan v3 §4.1): ONE shard SHARED
  // lock snapshot of the neighbour-guid projection for `orientation` — the
  // out list, the in list, or both concatenated outgoing-first. False when
  // `guid` is unknown. Duplicates are left in (parallel edges, both-halves
  // overlap, self loops); the caller's dedup removes them, exactly like
  // traverse_bfs. The guids are reported as recorded at the anchor snapshot:
  // the neighbours' own records are NOT re-validated (the traverse_bfs
  // contract).
  [[nodiscard]] bool oriented_adjacency(kimix::Guid const &guid,
                                        EdgeOrientation orientation,
                                        AdjacencyBuffer &out) const noexcept {
      return _read_oriented_adjacency(guid, orientation, out);
  }

    // --- traversals ---------------------------------------------------------
    // Reachability along outgoing edges. Cycle-safe: a BFS over an explicit
    // visited set, so a dependency cycle terminates (and a == b is a
    // zero-length path). Each visited vertex costs one shard shared lock: the
    // walk itself holds no lock between visits, which is what lets several
    // traversals run in parallel across shards.
    [[nodiscard]] bool has_path(kimix::Guid const &from,
                                kimix::Guid const &to) const;
    // Shortest path along outgoing edges, endpoints INCLUDED ([from, ..., to]).
    // Empty when either guid is unknown or `to` is unreachable from `from`;
    // {from} for from == to on an existing vertex.
    [[nodiscard]] kimix::vector<kimix::Guid>
    shortest_path(kimix::Guid const &from, kimix::Guid const &to) const;

    // --- GDI multi-hop traversal (the G3 capability) -------------------------
    // The deepest traversal this build honours; a larger max_depth is refused
    // with error_argument so the capability advertisement stays honest.
    inline static constexpr uint32_t kMaxNativeHopDepth = 32;

    // One vertex of a traverse_bfs result, in discovery (BFS) order: `guid`,
    // the BFS-tree `parent` that discovered it (zeroed at depth 0), and its
    // `depth` (0 = the start vertex).
    struct KIMIX_GRAPH_DB_API TraversalVertex {
        kimix::Guid guid;
        kimix::Guid parent;
        uint32_t depth{0};
    };

    // One hop of an EDGE-FILTERED shortest path (the result shape of the
    // constrained shortest_path below): the vertex the walk reached, the EDGE it
    // traversed to get there, and the hop count from the start. `edge` is zeroed
    // on the first step — the start vertex is reached by no edge — and `depth` is
    // also the step's index in the returned path, because a BFS path is a simple
    // one-vertex-per-hop chain. Reporting the edge is what an edge SEARCH needs
    // and what a vertex-only path cannot express: with parallel edges between the
    // same ordered pair, the pair says which edge carried the hop.
    struct KIMIX_GRAPH_DB_API PathStep {
        kimix::Guid vertex;
        EdgeId edge;
        uint32_t depth{0};
    };

    // Breadth-first traversal from `start` over `orientation` edges, `max_depth`
    // levels deep, keeping at most `cap_per_level` vertices per level (level 0,
    // the start, is always exactly one vertex). A non-empty `constraint`
    // filters every DISCOVERED vertex the GDI §5.10 way (a traversal filter
    // applies to vertices, not edges): a vertex that fails the filter is not
    // enqueued, so its own neighbourhood is never visited. The start vertex
    // is returned unconditionally at depth 0 (it was chosen by the caller,
    // not discovered). Discovery order is deterministic (shard-adjacency list
    // order, levels in ascending order), so the same graph yields the same
    // result regardless of scheduling.
    //
    // READ ATOMICITY: per-vertex, exactly like has_path/shortest_path above —
    // one vertex's adjacency is copied under one shard SHARED lock, and a
    // filtered candidate's labels/properties are read under that vertex's
    // leaf SHARED lock; a concurrent structural writer may be observed on one
    // visit and not the next, memory safety is preserved by the cascade
    // argument in the header contract, and no lock is held between visits.
    //
    // Work is bounded: a level stops expanding the moment it reaches
    // `cap_per_level`, so a hub-heavy level exits early instead of walking
    // every remaining frontier vertex.
    //
    //   error_argument   cap_per_level < 1, max_depth < 1 or > kMaxNativeHopDepth
    //   error_op         a label condition carries an ordering op (the
    //                    _resolve_constraint rule — validated up front)
    //   error_vertex     `start` is unknown
    //   ok               otherwise (`out` holds every discovered vertex; the
    //                    start is always `out[0]`)
    [[nodiscard]] GdiStatus traverse_bfs(kimix::Guid const &start, Constraint const &constraint,
                                         EdgeOrientation orientation, int64_t cap_per_level,
                                         uint32_t max_depth,
                                         kimix::vector<TraversalVertex> &out) const;

    // Bounded shortest path along outgoing edges, endpoints INCLUDED. `out` is
    // empty (status ok) when `to` is unreachable within `max_depth` hops;
    // {from} when from == to and `from` exists. An UNKNOWN endpoint is
    // error_vertex (the unbounded shortest_path above returns {} there; the
    // GDI entry point must not silently invent an empty answer for a bad uid).
    // Same BFS shape and read-atomicity contract as the unbounded form, and `out`
    // is a REUSABLE buffer here too: it is cleared and refilled, so its capacity
    // survives the call.
    //
    //   error_argument   max_depth < 1 or > kMaxNativeHopDepth
    //   error_vertex     `from` or `to` is unknown
    [[nodiscard]] GdiStatus shortest_path(kimix::Guid const &from, kimix::Guid const &to,
                                        uint32_t max_depth,
                                        kimix::vector<kimix::Guid> &out) const;

    // --- EDGE-filtered traversals (the EDGE half of the §5.10 filter story) --
    // §5.10 filters VERTICES: a traversal constraint gates the discovered vertex,
    // which is what traverse_bfs and the reads above implement. Nothing above can
    // express the other half — "walk only over relations of this kind" — because
    // every walk there enumerates the neighbour-guid projection of a vertex and
    // never looks at the edge it steps over. The three entry points below are that
    // half: the walk enumerates (edge, neighbour) LINK entries and keeps a hop
    // only when the EDGE's own labels and properties satisfy `edge_constraint`
    // (the §2.6 edge-read rule, evaluated by the same matches() semantics).
    //
    // A refused edge is not a hop: its target is not discovered, so the walk
    // routes around it exactly like a refused vertex is never expanded. The walk
    // follows `orientation` edges (a filtered "incoming" path answers "which
    // relations point at me", which the outgoing-only forms cannot), so the
    // orientation is a parameter here and not a fixed outgoing read.
    //
    // READ ATOMICITY: PER VISIT BATCH for edge tests, per visit for everything else.
    // One vertex shard SHARED lock still copies one visited vertex's oriented LINK
    // list and is released before the next visit, so no vertex-shard lock is ever
    // held between visits; the candidate edges that ONE visit produced may then be
    // tested together — one edge-shard SHARED lock per TOUCHED shard, ascending
    // index, taken only after that visit's vertex-shard lock has been released
    // (Graph::_filter_edges_batched). A concurrent structural writer may therefore
    // be observed on one visit and not the next, and an edge of a visit may be
    // tested against a state a later visit would not have seen; memory safety holds
    // by the header's cascade argument either way, because the edge record stays
    // under its own shard's lock for the whole test. A link whose edge or neighbour
    // vanished between the snapshot and the test simply is not a hop; which
    // vertices the walk discovers, in which order, is unaffected — the batch filters
    // out of order but consumes in ORIGINAL link order.
    //
    // Which of the two paths a visit takes is a COST decision only, and both produce
    // the same path, the same discovery order and the same PathStep::edge: a visit
    // whose link list is shorter than kEdgeFilterMinLinks stays per-link, and a wider
    // one keeps its first kEdgeFilterHeadLinks links per-link and drains only the TAIL
    // in ESCALATING chunks (64 -> 256 -> kEdgeFilterChunk, then kEdgeFilterChunk), so
    // the bound on what an early hit past the head can waste is the current drain, not
    // the whole visit. The head is what preserves the early exit — a wide visit that
    // finds its target among its first few links never reaches the batch at all.
    //
    // Parallel edges: the link list holds one entry per edge, and the first
    // matching one wins (the target vertex is discovered once — a BFS visits a
    // vertex once by definition); the reported PathStep::edge names WHICH of them
    // carried the hop. The batch preserves that: candidates are filtered out of
    // order but consumed in ORIGINAL link order.
    //
    // Does ONE edge's own payload satisfy `constraint`? ONE edge-shard SHARED lock
    // and a zero-copy probe (no labels/properties copy: a filtered walk tests a
    // candidate edge per link and must not deep-copy its payload). False when the
    // edge is unknown — a vanished edge is skipped, not an error, exactly like the
    // read commands' post-collection resolution. This single-edge probe is public
    // and unchanged; the walk's batched form (below) evaluates the SAME rule
    // through the same _record_matches() code.
    [[nodiscard]] bool edge_matches(EdgeId const &edge, Constraint const &constraint) const;

    // The oriented LINK view of one vertex's adjacency: (edge uid, neighbour uid)
    // pairs from ONE shard SHARED lock, the outgoing half first when orientation
    // == both (duplicates between the halves are the caller's dedup problem, like
    // oriented_adjacency). `out` is replaced, not appended, so a traversal reuses
    // ONE vector across visits and allocates on degree growth only. False when
    // `guid` is unknown.
    [[nodiscard]] bool oriented_links(kimix::Guid const &guid, EdgeOrientation orientation,
                                      kimix::vector<EdgeLink> &out) const;

    // Edge-filtered reachability: is `to` reachable from `from` over edges that
    // satisfy `edge_constraint` in `orientation`? Bounded by kMaxNativeHopDepth
    // (the native ceiling — an "unbounded" filtered search is not offered, because
    // the answer is a walk, not a composite). A zero-length path (from == to on an
    // existing vertex) is true without testing any edge: it traverses none.
    // Unknown endpoints are simply unreachable (false), the unbounded has_path
    // convention above.
    // It runs the SAME walk as the bounded filtered shortest_path below, in the
    // reachability mode of _filtered_walk (plan O6): same bound, same
    // visited-check-then-filter ordering, same discovery order, same refusal rules —
    // but the walk keeps only what a bool answer needs, so the per-node predecessor
    // record (edge/parent/depth, 48 B) and the PathStep assembly do not exist for it
    // (16 B per discovered node). The answer is therefore identical by construction,
    // not by keeping two implementations in step.
    [[nodiscard]] bool has_path(kimix::Guid const &from, kimix::Guid const &to,
                                Constraint const &edge_constraint,
                                EdgeOrientation orientation) const;

    // Edge-filtered, oriented, bounded shortest path. `out` holds one PathStep per
    // vertex of the path, endpoints INCLUDED and in order from `from`; the status
    // and result contract is exactly the vertex-only bounded form's — empty `out`
    // with ok means `to` is unreachable within `max_depth` over MATCHING edges,
    // {from} when from == to on an existing vertex. Like every bounded path form
    // here, `out` is a REUSABLE buffer: it is resized in place and its CAPACITY
    // survives the call, so a caller that hoists one vector across calls pays no
    // allocation for a buffer that is already big enough (a warm call allocates
    // nothing at all for a short walk).
    //
    // The walk itself is allocation-free up to a documented inline budget (64
    // discovered nodes, a 64-wide level frontier, 16 links per visit, and a
    // linear-scan visited bound of 64) and falls back to the heap on growth — a
    // 5 000-degree hub or a 32-level walk allocates, a 1-8 hop filtered walk over a
    // normal-degree graph does not.
    //
    //   error_argument   max_depth < 1 or > kMaxNativeHopDepth
    //   error_op         a label condition carries an ordering op (the
    //                    _resolve_constraint rule, validated up front)
    //   error_vertex     `from` or `to` is unknown
    //   ok               otherwise
    [[nodiscard]] GdiStatus shortest_path(kimix::Guid const &from, kimix::Guid const &to,
                                          Constraint const &edge_constraint,
                                          EdgeOrientation orientation, uint32_t max_depth,
                                          kimix::vector<PathStep> &out) const;

    // --- EDGE-filtered BFS (plan §1.3, the ABI-7 `traverse_bfs_edges`) -------
    // One discovered vertex of traverse_bfs_edges, in BFS discovery order: the
    // vertex, the EDGE that discovered it (zeroed at index 0 -- the start is
    // reached by no edge) and its depth.  The record is PathStep's shape reused
    // for a WHOLE frontier instead of one path: the pair (vertex, edge) is what
    // makes a parallel-edge discovery observable, which a vertex-only list (the
    // TraversalVertex form above) cannot report.
    struct KIMIX_GRAPH_DB_API BfsVertex {
        kimix::Guid vertex;
        EdgeId edge;
        uint32_t depth{0};
    };

    // One constraint-satisfying edge whose BOTH endpoints are in the discovered
    // set: the edge uid plus the edge's OWN (origin, target) -- the direction the
    // walk happened to follow it in is not reported, the record stands for the edge.
    // Parallel edges between one ordered pair are separate records (distinct uids).
    struct KIMIX_GRAPH_DB_API BfsEdge {
        EdgeId edge;
        kimix::Guid origin;
        kimix::Guid target;
    };

    // The result record of traverse_bfs_edges. BOTH vectors are REUSABLE buffers:
    // they are cleared and refilled in place, so their capacity survives the call --
    // which is what lets the C ABI keep ONE of these per session (the path_staging
    // precedent) and allocate nothing on a warm read path. `truncated` is the walk's
    // third output (see below); it is written by every successful call, including an
    // empty-frontier one.
    struct KIMIX_GRAPH_DB_API BfsEdgesResult {
        kimix::vector<BfsVertex> vertices;
        kimix::vector<BfsEdge> edges;
        bool truncated{false};
    };

    // Edge-filtered breadth-first discovery of the whole readable neighbourhood of
    // `start`: THREE outputs -- (1) every discovered vertex with its discovering edge
    // and depth, `start` first at depth 0 with a zeroed edge; (2) EVERY edge that
    // satisfies `edge_constraint` and joins two discovered vertices, whether or not
    // the walk stepped over it (the internal-edge sweep below, which is the whole
    // point: a spanning tree would drop the triangle's third edge and hide one half
    // of a parallel pair); (3) the truncation flag.
    //
    // HOP RULE -- identical to the _filtered_walk above, and implemented on the SAME
    // primitives (_snapshot_links, edge_matches / _filter_edges_batched, the same
    // kEdgeFilterMinLinks / kEdgeFilterHeadLinks / kEdgeFilterChunk split): ONE visit
    // checks the visited set FIRST, then the EDGE filter, then discovers. A link whose
    // edge fails the constraint is never traversed, so its target is only reachable
    // around it -- exactly the routing the filtered shortest_path follows, and the
    // leak rule the permission filter depends on (an edge the reader may not see is
    // not in the walk AND not in the sweep, and a vertex reachable only over it is not
    // discovered at all). Which of several parallel matching links carries the hop is
    // decided by link order (the first one wins, the same rule as PathStep::edge); the
    // SWEEP still reports every one of them, because it lists edges, not hops.
    //
    // The walk has no target and no early exit: it expands level by level in BFS
    // order until the frontier is empty, `max_depth` levels are done, or the vertex
    // count reaches `max_vertices`. The cap never PARTIALLY expands anything: the
    // moment the count is at `max_vertices`, no further discovery is recorded on any
    // level, the walk ends, and `truncated` is true exactly when a discovery that had
    // passed both tests was refused by the full array. So a graph whose readable set
    // is exactly `max_vertices` big reports 0 (nothing was dropped), and a generous
    // cap reports 0 -- the flag means "the readable neighbourhood continued past what
    // is displayed here", not "the array is full". COST of that honesty: after the
    // array fills, the walk keeps scanning links until the first hop the cap refuses,
    // so a neighbourhood that ends up not being truncated costs one full scan of the
    // readable set -- which is exactly what the same call with a non-binding cap costs.
    // The refusal ends the walk on the spot, so the walk never expands a vertex past
    // the cap and never discovers more than `max_vertices` of them.
    //
    // SECOND SWEEP (output 2), after discovery stops: for every discovered vertex, in
    // discovery order, snapshot its oriented LINK list and keep the links whose
    // NEIGHBOUR is also discovered (the visiting side is by construction) — the
    // endpoint test runs FIRST, the walk's own O1 ordering. The survivors are probed
    // through the SAME filter the walk uses — one edge-shard SHARED lock per TOUCHED
    // shard, ascending, with no vertex-shard lock held (LOCK ORDER 4) for a wide
    // staging, ONE edge_matches() per candidate for a narrow one (the same
    // kEdgeFilterMinLinks cost split, the same _record_matches() rule either way) — and
    // the ones that answer "satisfied" get their endpoints read back (one more level-3
    // lock per surviving candidate), which is what makes the record the edge's OWN
    // origin/target rather than the direction the walk happened to come from. The
    // candidate staging is stack scratch with a 64-entry inline budget (the walk's own
    // O5 discipline), so a normal-degree sweep allocates nothing at all. Deduplication
    // is by edge uid, and it is structural, not a second hash set: a BOTH read stages
    // one internal edge from BOTH of its endpoints, so only the sighting on the edge's
    // own origin side is emitted; a self edge is staged twice inside ONE vertex's
    // snapshot (its out half and its in half), so the first sighting within that
    // snapshot wins. Parallel edges have distinct uids, so every one of them is a
    // separate record — the guarantee a spanning tree cannot give. An edge that vanished
    // between the snapshot and the probe or the endpoint read is simply not reported,
    // the walk's rule for a link whose edge is gone. Cost: O(V * degree) probes and
    // locks over the discovered set — bounded in the stated use by `max_vertices` (300),
    // documented rather than optimised.
    //
    // READ ATOMICITY: per visit, exactly like the filtered shortest_path above -- one
    // vertex-shard SHARED lock per snapshot (released before the next visit), then the
    // level-3 batch for that visit's candidates with NO level-1 lock held. The sweep
    // repeats the same per-vertex snapshot discipline, so a concurrent structural
    // writer may be observed on one visit and not the next; memory safety holds by the
    // header's cascade argument, and probe/fill determinism is by construction: the
    // same walk over the same quiescent graph yields the same vertices, the same edges
    // in the same order and the same flag.
    //
    //   error_argument   max_depth < 1 or > kMaxNativeHopDepth, or max_vertices < 1
    //   error_op         a label condition carries an ordering op (the
    //                    _resolve_constraint rule, validated up front like the walk's)
    //   error_vertex     `start` is unknown
    //   ok               otherwise: `out.vertices` holds the discovered set (the start
    //                    is always out.vertices[0]) and `out.edges` the internal ones
    [[nodiscard]] GdiStatus traverse_bfs_edges(kimix::Guid const &start,
                                               Constraint const &edge_constraint,
                                               EdgeOrientation orientation, uint32_t max_depth,
                                               uint32_t max_vertices,
                                               BfsEdgesResult &out) const;

      // --- GDI global scan (the G1 capability) -----------------------------
      // Calls `visit(vertex)` for EVERY vertex in the graph: shard 0..
      // kGraphShardCount-1 in ascending order, each shard's map order within a
      // shard. ONE shard SHARED lock at a time, taken and released per shard —
      // the same contract as _compute_roots(): a concurrent structural writer
      // may be observed on one shard and not the next (the scan is a
      // shard-by-shard snapshot, not a graph-wide point-in-time one), memory
      // safety holds by the header's cascade argument, and no lock is held
      // between shards. Per-vertex atomicity: the callback runs while its
      // shard's SHARED lock is held, so one call sees one consistent view of
      // that shard (same contract as roots()).
      // The callback MUST NOT mutate the graph — it runs under a read lock.
      // Returning false stops the scan early; the status is ok either way.
      // Template (header-defined) so a lambda costs no allocation and no
      // kimix::function indirection on the per-vertex hot path.
      template <typename Fn> GdiStatus for_each_vertex(Fn &&visit) const {
          return for_each_vertex(0u, static_cast<uint32_t>(kGraphShardCount),
                                 static_cast<Fn &&>(visit));
      }
      // The shard-range form: visits shards [shard_begin, shard_end) only —
      // the unit the aggregate fan-out parallelizes over. Each range obeys the
      // same contract as the full scan (one SHARED lock at a time, ascending);
      // DISJOINT ranges running on different fibers therefore never touch the
      // same shard lock, and a caller that merges per-range results in range
      // order gets the deterministic full-scan order. Indices are clamped to
      // [0, kGraphShardCount]; an empty range invokes nothing.
      template <typename Fn>
      GdiStatus for_each_vertex(uint32_t shard_begin, uint32_t shard_end, Fn &&visit) const {
          uint32_t const end = shard_end < kGraphShardCount ? shard_end
                                                            : static_cast<uint32_t>(kGraphShardCount);
          for (uint32_t i = shard_begin; i < end; ++i) {
              Shard const &shard = _shards[i];
              std::shared_lock<std::shared_mutex> lock(shard.mutex);
              for (auto const &entry : shard.vertices) {
                  Vertex *vertex = entry.second.get();
                  if (vertex != nullptr && !visit(*vertex)) {
                      return GdiStatus::ok;
                  }
              }
          }
          return GdiStatus::ok;
      }
      // --- dirty bookkeeping (for the incremental serializer) -----------------

    // Set/clear every vertex's AND every edge's dirty flag (a full save rewrites
    // both families). The vertex flags are atomics; one shard shared lock at a
    // time keeps each shard's map alive while it is walked.
    void mark_dirty() noexcept;
    void clear_dirty() noexcept;
    // Snapshot of the dirty vertices with their guid and cached content hash:
    // one shard shared lock at a time for the map, each vertex's SHARED lock for
    // the hash (shard before vertex, as the contract requires).
    [[nodiscard]] kimix::vector<DirtyVertex> dirty_vertices() const;

    // --- write-contract introspection ---------------------------------------

    // True when the calling thread is the recorded structural writer (or when
    // no structural write has happened yet, i.e. this thread would become it).
    [[nodiscard]] bool is_writer_thread() const noexcept;
    // Forget the recorded writer, so the next structural write may come from
    // any thread. Test-only helper for the single-writer contract check.
    void reset_writer_thread() noexcept;

private:
    /*
     * One vertex-map shard: a shared_mutex on its OWN cache line, then the map.
     * The split matters: the lock word is written by every reader, so sharing a
     * line with the map's node array would make each acquire invalidate the
     * bucket pointers the holder is about to read.
     */
    struct Shard {
        Shard() noexcept;
        Shard(Shard const &) = delete;
        Shard &operator=(Shard const &) = delete;
        Shard(Shard &&) = delete;
        Shard &operator=(Shard &&) = delete;

        // Lookup under the already-held shard lock (shared or unique).
        [[nodiscard]] Vertex *_find(kimix::Guid const &guid) const noexcept;

        alignas(kCacheLineSize) mutable std::shared_mutex mutex;
        kimix::HashMap<kimix::Guid, kimix::unique_ptr<Vertex>> vertices;
        // Pad so consecutive shards never share a cache line.
        alignas(kCacheLineSize) unsigned char _padding[kCacheLineSize]{};
    };

    // Shard index of a guid. INLINE (it sits in the dependency chain of every
    // single structural call, and an out-of-line call cost ~1.6 ns per op — more
    // than the lock it saves). One xor, one multiply and a shift: Fibonacci
    // hashing on the top kGraphShardShift bits.
    //
    // Why the multiply is not optional: the bits taken must NOT be the plain guid
    // high bits. Windows guids come from ::CoCreateGuid, whose version/variant
    // fields are CONSTANT in exactly the top bits — an A/B with a
    // `data0 ^ (data1 << 32)` index collapsed every guid onto a handful of shards
    // and lost all 12x of the read-scaling win (measured: 5.9 ns -> 142.8 ns per
    // contended find). Multiplying by an odd constant first spreads even those
    // constant high bits over the whole mask.
    //
    // Correctness does not depend on the spread at all: a poor index only costs
    // contention, never a wrong lookup, because the shard's own HashMap does the
    // full (XXH3) key comparison.
    [[nodiscard]] static size_t _shard_of(kimix::Guid const &guid) noexcept {
        if constexpr (kGraphShardCount == 1) {
            // The diagnostic single-shard build: no index at all, i.e. exactly the
            // epoch-0 "one map, one lock" structure on current code.
            (void)guid;
            return 0;
        } else {
            auto const &words = guid.to_binary();
            uint64_t const mixed = (words.data0 ^ words.data1) * 0x9E3779B97F4A7C15ull;
            constexpr unsigned kShift = 64 - kGraphShardShift;
            return static_cast<size_t>(mixed >> kShift);
        }
    }

    // Records/enforces the single-writer contract; always records, asserts in
    // debug builds when a second thread performs a structural write.
    bool _check_writer() noexcept;

    // True when `vertex` has no incoming dependency edge in either form (the
    // pointer set or the serialized guid list): that is the "root" predicate.
    // Reads structural state, so the caller holds that vertex's shard lock.
    [[nodiscard]] static bool _is_root(Vertex const &vertex) noexcept;

    // connect/disconnect body: both endpoints' shards are ALREADY locked unique
    // by the caller (the same shard twice when from/to share one).
    bool _connect_locked(Shard &from_shard, Shard &to_shard,
                         kimix::Guid const &from, kimix::Guid const &to);
    bool _disconnect_locked(Shard &from_shard, Shard &to_shard,
                            kimix::Guid const &from, kimix::Guid const &to);
    // The ONE place an edge is added. The caller has resolved both endpoints and
    // holds their shards UNIQUE (the same shard once when origin == target, which
    // is why the two vertex uniques are taken only when they differ). Performs
    // the duplicate probe against the origin's link list, inserts the record into
    // the edge table and links both sides. `via_gdi_api` selects the option-gated
    // policy (parallel edges + self loops) over the legacy one.
    GdiStatus _create_edge_locked(Vertex *origin, Vertex *target,
                                  EdgeDirectionType direction,
                                  kimix::span<const LabelId> labels,
                                  PropertyList const *properties, bool via_gdi_api,
                                  EdgeId &out_edge);
    // The shared body of edge_set_origin/edge_set_target: validate the new
    // endpoint, then move the record's endpoint AND both adjacency entries under
    // one ascending lock set (vertex shards, then the edge's own shard).
    GdiStatus _move_edge_endpoint(EdgeId const &edge, kimix::Guid const &endpoint,
                                  bool move_origin);
    // Duplicate probe over the link list: is there an edge origin->target?
    [[nodiscard]] static bool _has_out_edge_locked(Vertex const *origin,
                                                  kimix::Guid const &target) noexcept;
    // Remove one edge's record and BOTH adjacency links. The endpoint shards are
    // taken one at a time (ascending order when they differ), never nested with
    // the edge shard held.
    GdiStatus _free_edge_locked(EdgeId const &edge);
    // Resolve a guid under one shard SHARED lock. Nullptr when unknown.
    [[nodiscard]] Vertex *_resolve(kimix::Guid const &guid) const noexcept;

    // Allocating adjacency copy (dependencies / dependents): one shard shared
    // lock, one allocation, no intermediate buffer.
    [[nodiscard]] kimix::vector<kimix::Guid>
    _copy_adjacency(kimix::Guid const &guid, bool outgoing) const;

    // Fill `out` from one vertex's serialized edge list under one shard shared
    // lock. False when the guid is unknown.
    [[nodiscard]] bool _read_adjacency(kimix::Guid const &guid, bool outgoing,
                                       AdjacencyBuffer &out) const noexcept;
    [[nodiscard]] size_t _read_adjacency(kimix::Guid const &guid, bool outgoing,
                                         kimix::span<kimix::Guid> out) const noexcept;
    // The traversal form: fill `out` with the oriented adjacency (both edge
    // lists when orientation == both) under ONE shard shared lock, so the two
    // directions of one undirected visit come from one snapshot of that
    // vertex. False when the guid is unknown.
    [[nodiscard]] bool _read_oriented_adjacency(kimix::Guid const &guid,
                                                EdgeOrientation orientation,
                                                AdjacencyBuffer &out) const noexcept;

    // --- the EDGE-filtered walk's batch filter (plan O2) ---------------------
    // Does an ALREADY-RESOLVED edge record's own payload satisfy `constraint`? The
    // ONE evaluation of the §2.6 edge rule, shared by the public single-edge probe
    // (edge_matches) and the batched filter, so the two cannot drift. The caller
    // holds that edge's shard lock (level 3), which is what keeps the borrowed
    // labels/properties valid for the probe — no payload copy.
    [[nodiscard]] static bool _record_matches(Edge const &record,
                                             Constraint const &constraint);

    // The batched edge filter: `match_out[i] = 1` iff edges[i]'s own payload
    // satisfies `constraint`. ONE edge-shard SHARED lock per TOUCHED shard instead
    // of one per candidate — the measured shape: one pass counting-sorts the
    // candidates by EdgeTable::shard_of() and records the touched-shard list (never
    // a scan of all 64), a prefix over that touched list only, then per touched
    // shard in ASCENDING index one lock, `_find_locked` per candidate of that shard
    // and `_record_matches`. `match_out` must be exactly as long as `edges`;
    // `scratch` is caller-owned and reused across calls.
    //
    // LEVEL 3 ONLY. The caller must already have released the vertex-shard lock of
    // the visit it staged (the filtered walk snapshots one vertex, then batches its
    // candidates, then consumes them), and nothing here may take a vertex-shard
    // lock: that is what keeps the hierarchy's "level 1 before level 3, never
    // together" rule and its deadlock-freedom argument intact.
    void _filter_edges_batched(kimix::span<const EdgeId> edges, Constraint const &constraint,
                               kimix::span<uint8_t> match_out, EdgeFilterScratch &scratch) const;

    // The filtered walk's LINK snapshot storage: the AdjacencyBuffer idiom applied
    // to EdgeLink — kInlineCapacity links inline, a heap vector beyond (capacity
    // retained for the rest of the walk), so a normal-degree visit allocates
    // nothing. Defined in graph.cpp next to the walk that owns it; declared here
    // because that walk's state is a plain stack local.
    class LinkBuffer;
    // Fill `out` with `guid`'s oriented LINK list (outgoing half first when
    // orientation == both — the oriented_links() snapshot) from ONE vertex-shard
    // SHARED lock, growing `out` only past LinkBuffer::kInlineCapacity. False when
    // `guid` is unknown (a removed vertex is not a hop).
    [[nodiscard]] bool _snapshot_links(kimix::Guid const &guid, EdgeOrientation orientation,
                                       LinkBuffer &out) const;

    // --- the EDGE-filtered walk itself (plan O6) -----------------------------
    // ONE walk body with two products, selected at COMPILE time by the kind of record
    // it has to discover (`Node`, a graph.cpp-local struct: the mode IS what the
    // record needs, so a type parameter — not a runtime flag — is what keeps the two
    // from drifting and lets the unused half disappear):
    //   * PathWalkNode (48 B: vertex, discovering edge, parent index, depth) for the
    //     public shortest_path forms, which must assemble a PathStep sequence;
    //   * ReachabilityWalkNode (16 B: the vertex alone) for the filtered has_path,
    //     whose whole product is the bool below — no predecessor chain, no PathStep
    //     assembly, so the walk's discovery array is a third of the size.
    // Both modes keep the discovery array (it is also the small-BFS visited set's
    // projection), the per-visit link snapshot, the level frontiers, the batched edge
    // filter and every refusal rule, so the reachability answer cannot drift from the
    // path answer. `out` is the caller's PathStep buffer; it is NULL in the
    // reachability mode, which writes no path at all.
    struct FilteredWalkResult {
        GdiStatus status;
        // Did the walk reach `to`? The reachability mode's entire product; the path
        // mode reports through `out` and never reads this.
        bool reached;
    };
    template <typename Node>
    auto _filtered_walk(kimix::Guid const &from, kimix::Guid const &to,
                        Constraint const &edge_constraint, EdgeOrientation orientation,
                        uint32_t max_depth, kimix::vector<PathStep> *out) const
        -> FilteredWalkResult;

    // Bump the structural version. Called by every structural write, AFTER the
    // mutation and while its shard lock is still held, so a reader that sees the
    // new version also sees the mutation. Relaxed is enough: the counter carries
    // no data itself (the cached root vector is guarded by _roots_mutex and the
    // graph state by the shard locks), it only has to change.
    void _bump_structure() noexcept {
        _structure_version.fetch_add(1, std::memory_order_relaxed);
    }

    // The O(n) scan behind roots(): walks the shards, one SHARED lock at a time,
    // and is what a stale cache pays for. Never called with _roots_mutex held by
    // anyone else's account of the graph state — it takes the shard locks itself.
    [[nodiscard]] kimix::vector<Vertex *> _compute_roots() const;

    std::array<Shard, kGraphShardCount> _shards;
    // First-class edges (level 3) and the (label, external_id) index (level 4).
    EdgeTable _edges;
    KeyIndex _keys;
    GraphOptions _options;
    // Ids removed since the last save (the tombstones it writes). Writer-only.
    kimix::vector<kimix::Guid> _removed_vertices;
    kimix::vector<EdgeId> _removed_edges;
    // Vertex count, maintained by the structural writers, so size() is lock-free.
    std::atomic<size_t> _size{0};
    // Structural version + the roots cache (see roots()). The cache fields are
    // written only under _roots_mutex; _structure_version is bumped lock-free by
    // the writers.
    mutable std::atomic<uint64_t> _structure_version{0};
    // Dedicated cache lock, a shared_mutex so warm roots() calls run concurrently.
    // It is NEVER taken by a structural write (those only bump the atomic), so it
    // adds no acquisition to the write path and cannot participate in a
    // wait-for cycle with a shard lock: roots() takes it, then the shard locks
    // one at a time, and no path takes it while holding a shard lock.
    mutable std::shared_mutex _roots_mutex;
    mutable kimix::vector<Vertex *> _cached_roots;
    mutable uint64_t _cached_root_version{0};
    mutable bool _roots_valid{false};
    // Thread id (std::hash of std::thread::id) of the first structural writer.
    // std::thread::id itself is not guaranteed lock-free, so the recorded value
    // is its hash, and a separate flag says whether anything was recorded yet.
    std::atomic<bool> _writer_recorded{false};
    std::atomic<uint64_t> _writer_thread_hash{0};
};

} // namespace kimix::graph_db