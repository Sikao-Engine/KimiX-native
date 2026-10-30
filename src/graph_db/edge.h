/*
 * edge.h — first-class edges (GDI §2.4 / §2.14).
 *
 * Before this header an edge WAS its two endpoints: a vertex kept a
 * unordered_set<Vertex*> plus a guid vector, and "an edge" was the fact that
 * both lists contained each other. That cannot express what GDI needs — a stable
 * edge uid, a per-edge direction, labels, properties, endpoint mutation, parallel
 * edges between the same ordered pair — so an edge is now an object:
 *
 *   Edge        the record: uid, origin, target, direction, labels, properties,
 *               dirty flag.
 *   EdgeTable   the sharded uid -> Edge map (same idiom as Graph's vertex map).
 *   EdgeLink    one entry of a vertex's adjacency: (edge uid, neighbour uid).
 *   EdgeRecord  the plain read-back a GDI read returns (uid + endpoints +
 *               direction), copyable and independent of any lock.
 *
 * Both adjacency directions are kept on the vertex ({edge, neighbour} in
 * _out_links / _in_links), so an oriented read is a scan of ONE list and needs
 * no second lookup to decide incidence.
 *
 * LOCKING (see README.md for the full hierarchy)
 *   An Edge's fields and payload are guarded by its OWN SHARD (the EdgeTable
 *   shard of its uid), NOT by a per-edge mutex: an edge record is small and a
 *   mutex would cost a cache line per edge on a multi-million-edge store. The
 *   consequence is documented and accepted — two writes to two different edges
 *   that land in the same shard serialize on that shard's lock — because every
 *   structural write is single-writer by contract anyway; only READS run
 *   concurrently, and a read takes the shard shared.
 *   Endpoint mutation takes the affected vertex shards AND the edge shard, in
 *   the hierarchy's order (vertex shards ascending, then edge shards), which is
 *   what keeps the adjacency lists consistent with the record.
 *
 * LOCK KIND: std::shared_mutex — KEPT (graph.h's LOCK KIND POLICY is the
 * admission test). Three holders are NOT bounded, so the shard object cannot move
 * to a busy-waiting lock for its readers either:
 *   - the EDGE-filtered walk holds ONE edge-shard SHARED lock across up to
 *     kEdgeFilterChunk (1024) candidate probes of one visit, which is on the read
 *     path of the filtered traversal;
 *   - record() / properties() / labels() / dirty_edges() copy whole payloads inside
 *     the shared lock (a PropertyValue may own strings or arrays);
 *   - reserve() allocates a whole per-shard table under the unique lock (that one is
 *     loader-only, i.e. quiescent, so it alone would be acceptable).
 * Shortening the first two is a separate refactor (a smaller chunk would trade the
 * measured batch win), so the edge shards stay on the blocking lock.
 *
 * DIRTY INDEX (the incremental save's edge write set)
 *   The table keeps the ids of the dirty edges in _dirty_edge_index, fed by the
 *   EdgeDirtySink protocol instead of an O(E) rescan (dirty_edges() was the
 *   dominant cost of an incremental save of a tiny change). Its mutex is the
 *   INNERMOST lock of the graph_db hierarchy: it may be taken while holding a
 *   shard lock (insert/erase/clear_dirty under the shard, mark_dirty from the
 *   Graph paths that hold vertex shards), and NOTHING is acquired while it is
 *   held. A kimix::spin_mutex for the same measured reason as the vertex index
 *   (graph.h, CONCURRENCY CONTRACT level 4): one hash insert/erase, taken on a
 *   write path, and structurally unable to deadlock because it never holds
 *   anything else.
 *
 * MULTI-EDGES: create_edge allows several edges between one ordered pair (and a
 * self-loop, both options-gated). Reachability is unaffected: a vertex's
 * neighbour-dedup sets are maintained separately, so has_path/shortest_path still
 * see each neighbour once.
 *
 * Example:
 *   kimix::graph_db::EdgeTable edges;
 *   kimix::graph_db::EdgeId id{true};
 *   edges.create(id, origin_guid, target_guid,
 *                kimix::graph_db::EdgeDirectionType::directed);
 *   kimix::graph_db::EdgeRecord record;
 *   edges.record(id, record);            // copy under the shard's shared lock
 */
#pragma once

#include "graph_db_api.h"
#include "binary_le.h"
#include "property.h"
#include "status.h"

#include <core/guid.h>
#include <core/memory.h>
#include <core/spin_mutex.h>
#include <core/stl/memory.h>
#include <core/stl/unordered_map.h>
#include <core/stl/vector.h>

#include <array>
#include <atomic> // std::atomic<size_t> _size: MSVC pulls it in transitively, gcc does not
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>

namespace kimix::graph_db {

// A stable edge identity: a 16-byte guid, exactly like a vertex uid, and it
// survives save/load verbatim (which is what makes GDI's
// SupportsEdgeByStableUid honest).
using EdgeId = kimix::Guid;

// GDI_EDGE_DIRECTED / GDI_EDGE_UNDIRECTED (gdi.h: 259 / 260).
enum class EdgeDirectionType : uint8_t {
    directed = 0,
    undirected = 1,
};

// Read orientation of a neighborhood query (GDI_GetEdgesOfVertex). Deliberately a
// DIFFERENT type from EdgeDirectionType, like the C# side, so mixing up a write
// direction and a read orientation is a compile error.
enum class EdgeOrientation : uint8_t {
    outgoing = 0,
    incoming = 1,
    both = 2,
};

[[nodiscard]] KIMIX_GRAPH_DB_API char const *edge_direction_name(EdgeDirectionType direction) noexcept;
[[nodiscard]] KIMIX_GRAPH_DB_API char const *edge_orientation_name(EdgeOrientation orientation) noexcept;

// Shards of the edge table (independent constant; graph.h static_asserts it
// matches the vertex shard count so the lock-order story stays one story).
inline constexpr size_t kEdgeShardCount = 64;
static_assert((kEdgeShardCount & (kEdgeShardCount - 1)) == 0,
              "kEdgeShardCount must be a power of two (the shard index is a mask)");

/*
 * One adjacency entry of a vertex: the edge's uid plus the vertex at its other
 * end. 32 bytes, which is what the adjacency of a vertex costs per incident edge
 * (the epoch-2 guid list was 16) — measured and recorded in README.md.
 */
struct KIMIX_GRAPH_DB_API EdgeLink {
    EdgeId edge;
    kimix::Guid neighbour;

    [[nodiscard]] bool operator==(EdgeLink const &other) const noexcept {
        return edge == other.edge && neighbour == other.neighbour;
    }
};

/*
 * The read-back form of an edge: what GDI's GdiEdgeRecord carries (uid, both
 * endpoints, direction) and nothing the reader has to lock for. A copy is
 * independent of the table's state.
 */
struct KIMIX_GRAPH_DB_API EdgeRecord {
    EdgeId id;
    kimix::Guid origin;
    kimix::Guid target;
    EdgeDirectionType direction{EdgeDirectionType::directed};
    kimix::vector<LabelId> labels;
    PropertyList properties;

    [[nodiscard]] bool is_self_loop() const noexcept { return origin == target; }
};

class Edge;

/*
 * The dirty-flag SINK for an Edge — the same protocol vertex.h documents for
 * Vertex, one level down: while an edge belongs to an EdgeTable, every flip of
 * its dirty flag is reported here, which is what lets the incremental save
 * collect its edge records in O(dirty) instead of rescanning every shard. The
 * notification sits in the flag setter (Edge::_mark_dirty / _clear_dirty) because
 * a flag write that skipped it would silently drop that edge's record from the
 * next save. The flag is private and only reachable through
 * mark_dirty()/clear_dirty(), so there is no other write to miss.
 */
class EdgeDirtySink {
public:
    // The flag went false -> true: the record must be in the next save.
    virtual void note_edge_dirty(Edge *edge) noexcept = 0;
    // The flag went true -> false: the record was written (or restored).
    virtual void clear_edge_dirty(Edge *edge) noexcept = 0;

protected:
    // Never deleted through this pointer: the sink is a part of its owner.
    ~EdgeDirtySink() = default;
};

/*
 * One edge. Fields are public to Graph/EdgeTable (which own the locking); there
 * is no meaningful "partially valid" state, so there is no setter soup either.
 */
class KIMIX_GRAPH_DB_API Edge {
public:
    Edge() noexcept = default;
    Edge(EdgeId id, kimix::Guid origin, kimix::Guid target,
         EdgeDirectionType direction) noexcept
        : _id(id), _origin(origin), _target(target), _direction(direction) {
    }
    Edge(Edge const &) = delete;
    Edge &operator=(Edge const &) = delete;
    Edge(Edge &&) = delete;
    Edge &operator=(Edge &&) = delete;
    ~Edge() noexcept = default;

    [[nodiscard]] EdgeId const &id() const noexcept { return _id; }
    [[nodiscard]] kimix::Guid const &origin() const noexcept { return _origin; }
    [[nodiscard]] kimix::Guid const &target() const noexcept { return _target; }
    [[nodiscard]] EdgeDirectionType direction() const noexcept { return _direction; }
    // --- dirty flag (the incremental save's edge write set) ------------------
    // Same protocol as Vertex: every write goes through the private
    // _mark_dirty/_clear_dirty, which report to the attached EdgeDirtySink
    // (EdgeTable) when there is one and store directly when there is not.
    [[nodiscard]] bool is_dirty() const noexcept { return _dirty; }
    void mark_dirty() noexcept { _mark_dirty(); }
    void clear_dirty() noexcept { _clear_dirty(); }

    // Payload assignment for an edge that is not published yet (the loader and
    // Graph::edge_create fill a fresh edge through this). Same contract as
    // Vertex::assign_payload: never on an edge the table already holds.
    void assign_payload(kimix::span<const LabelId> labels, PropertyList const &properties) {
        _labels.clear();
        for (auto label : labels) {
            if (label != kNoLabel) {
                _labels.push_back(label);
            }
        }
        _properties.clear();
        for (auto const &entry : properties) {
            _properties.emplace(entry.name, entry.value);
        }
    }

    // A "neighbour" of `vertex`: the other endpoint. `vertex` must be one of the
    // two endpoints (the callers only ask that way).
    [[nodiscard]] kimix::Guid neighbour_of(kimix::Guid const &vertex) const noexcept {
        return _origin == vertex ? _target : _origin;
    }

private:
    friend class EdgeTable;
    friend class Graph;

    // The dirty-flag protocol (see the EdgeDirtySink comment above). Defined in
    // edge.cpp: the ONLY code that writes _dirty.
    void _mark_dirty() noexcept;
    void _clear_dirty() noexcept;

    EdgeId _id;
    kimix::Guid _origin;
    kimix::Guid _target;
    EdgeDirectionType _direction{EdgeDirectionType::directed};
    kimix::vector<LabelId> _labels;
    PropertyBag _properties;
    bool _dirty{true};
    // The EdgeTable this edge currently belongs to, or nullptr. Set and cleared
    // only by EdgeTable::_attach_dirty / _detach_dirty, always together with the
    // index entry it implies.
    EdgeDirtySink *_dirty_sink{nullptr};
};

/*
 * The sharded edge store. Public methods take exactly one shard lock and copy
 * what they return; the mutating entry points that need several locks (an
 * endpoint move) live on Graph, which owns the vertex shards too.
 */
class KIMIX_GRAPH_DB_API EdgeTable : public kimix::IOperatorNewBase, public EdgeDirtySink {
public:
    EdgeTable() noexcept = default;
    EdgeTable(EdgeTable const &) = delete;
    EdgeTable &operator=(EdgeTable const &) = delete;
    EdgeTable(EdgeTable &&) = delete;
    EdgeTable &operator=(EdgeTable &&) = delete;
    ~EdgeTable() noexcept;

    // Insert `edge` (adopting it). False when the uid is already present (an
    // astronomically unlikely collision, reported instead of corrupting the map).
    // The single insertion point of the whole class: it attaches the dirty sink
    // and registers an already-dirty edge in the index, so no adopted edge can
    // escape the incremental save's write set.
    bool insert(kimix::unique_ptr<Edge> edge);
    // Remove an edge, returning it to the caller (the transaction/journal path
    // needs to resurrect it). nullptr when the uid is unknown. The sink is
    // DETACHED and the flag cleared before the edge is handed back: the edge is
    // not this table's any more, so a caller that marks it dirty afterwards must
    // not touch this table's index (a re-adoption through insert() re-attaches).
    kimix::unique_ptr<Edge> erase(EdgeId const &id);
    // Resolve a uid to its (still owned) Edge. The pointer is valid until the
    // edge is erased; taking it does not keep it alive.
    [[nodiscard]] Edge *find(EdgeId const &id) const;
    [[nodiscard]] bool contains(EdgeId const &id) const;
    [[nodiscard]] size_t size() const noexcept { return _size.load(std::memory_order_relaxed); }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    // --- reads that copy (one shard shared lock each) ------------------------

    // Full record: uid, endpoints, direction, labels, properties.
    [[nodiscard]] bool record(EdgeId const &id, EdgeRecord &out) const;
    // Endpoints + direction only (cheaper: no payload copy).
    [[nodiscard]] bool endpoints(EdgeId const &id, kimix::Guid &origin,
                                 kimix::Guid &target) const;
    [[nodiscard]] bool direction(EdgeId const &id, EdgeDirectionType &out) const;
    [[nodiscard]] bool labels(EdgeId const &id, kimix::vector<LabelId> &out) const;
    [[nodiscard]] bool properties(EdgeId const &id, PropertyList &out) const;
    [[nodiscard]] bool has_label(EdgeId const &id, LabelId label) const;
    [[nodiscard]] bool property(EdgeId const &id, PropNameId name, PropertyValue &out) const;

    // --- dirty bookkeeping (the incremental serializer) ----------------------

    // Every dirty edge record, copied out. The SET comes from the dirty index
    // (_dirty_edge_index, maintained by the sink) and each id is then resolved
    // under ONE edge-shard SHARED lock, so the cost is O(dirty) instead of
    // O(edges) — the old form scanned every shard.
    // ORDER: the index's order (the order the records were first marked dirty,
    // as that container iterates it), NOT the old shard-then-insertion order. No
    // caller depends on it: the save path sorts its batch into LMDB key order
    // before writing (serialize.h stage A'), and a test that compares must
    // compare sets.
    [[nodiscard]] kimix::vector<EdgeRecord> dirty_edges() const;
    void mark_dirty() noexcept;
    void clear_dirty() noexcept;
    // Clear the flag of exactly `id`, if it is still dirty. Used by the save path
    // after a successful transaction, mirroring the vertex rule ("only the
    // records this call wrote stop being dirty").
    void clear_dirty(EdgeId const &id) noexcept;

    // TEST SEAM: full O(E) scan, returns how many records are dirty. A test can
    // assert it equals dirty_edges().size() — the dirty index must never lose an
    // entry, or the next save silently drops a record. Never call from production
    // code.
    [[nodiscard]] size_t test_count_dirty_edges_by_scan() const noexcept;

    // Reserve capacity before a bulk load (per-shard; only ever raises).
    void reserve(size_t entries);

    // Shard plumbing, exposed for Graph (which takes several locks at once) and
    // for the lock-order assertions in the tests. `shard_mutex` is LEVEL 3.
      // The returned object is the BLOCKING std::shared_mutex, not the atomic one:
      // an edge shard's longest holders are unbounded (see this file's LOCK KIND note
      // and graph.h's LOCK KIND POLICY), so a caller must not assume a spin lock here.
    [[nodiscard]] static size_t shard_of(EdgeId const &id) noexcept;
    [[nodiscard]] std::shared_mutex &shard_mutex(size_t shard) const noexcept {
        return _shards[shard].mutex;
    }

private:
    friend class Graph;

    struct Shard {
        Shard() noexcept = default;
        Shard(Shard const &) = delete;
        Shard &operator=(Shard const &) = delete;
        Shard(Shard &&) = delete;
        Shard &operator=(Shard &&) = delete;

          // LOCK KIND: std::shared_mutex — KEPT; see this file's LOCK KIND note.
        alignas(64) mutable std::shared_mutex mutex;
        kimix::unordered_map<EdgeId, kimix::unique_ptr<Edge>> edges;
    };

    // Lookup under the already-held shard lock (shared or unique).
    [[nodiscard]] Edge *_find_locked(size_t shard, EdgeId const &id) const noexcept;
    // Append one shard's ids (caller holds that shard's lock).
    void ids_of_shard(size_t shard, kimix::vector<EdgeId> &out) const;

    // --- the dirty index (its mutex is the INNERMOST lock of the hierarchy) --
    // The EdgeDirtySink half of the flag protocol: called by Edge::_mark_dirty /
    // _clear_dirty, i.e. while the caller already holds whatever lock guards that
    // edge record (an edge shard, a vertex shard for the adjacency paths, or no
    // lock at all between writes). Both take _dirty_edges_mutex, so the flag and
    // the index entry cannot disagree. NOTHING may be acquired while that mutex
    // is held: the critical section only touches a hash container.
    void note_edge_dirty(Edge *edge) noexcept override;
    void clear_edge_dirty(Edge *edge) noexcept override;

    // Attach the sink to an edge that has just entered a shard map (and register
    // it when it is already dirty — a fresh Edge starts dirty). One helper so
    // insert() and adopt_edge() cannot drift apart.
    void _attach_dirty(Edge *edge) noexcept;
    // Detach the sink and clear the flag: the edge is leaving this table. The
    // index entry is dropped in the same critical section, so a detached edge can
    // never leave a stale entry behind.
    void _detach_dirty(Edge *edge) noexcept;

    std::array<Shard, kEdgeShardCount> _shards;
    std::atomic<size_t> _size{0};
    // The dirty edges, i.e. exactly the records the next incremental save writes.
    // mutable because dirty_edges() is const. Guarded by _dirty_edges_mutex (a
    // spin lock — see the DIRTY INDEX note at the top of this file).
    mutable kimix::spin_mutex _dirty_edges_mutex;
    mutable kimix::unordered_set<EdgeId> _dirty_edge_index;
};

// ---------------------------------------------------------------------------
// Edge record codec ("e/" + edge uid -> "GDE2" record)
// ---------------------------------------------------------------------------

// Bytes an encoded edge record occupies.
[[nodiscard]] KIMIX_GRAPH_DB_API size_t
encoded_edge_record_size(EdgeRecord const &record) noexcept;
KIMIX_GRAPH_DB_API void encode_edge_record(EdgeRecord const &record,
                                           kimix::vector<std::byte> &out);
// Total decoder: a truncated / wrong-magic / impossible-count buffer is a
// `false`, never an out-of-bounds read.
[[nodiscard]] KIMIX_GRAPH_DB_API bool
decode_edge_record(kimix::span<const std::byte> bytes, EdgeRecord &out) noexcept;

} // namespace kimix::graph_db
