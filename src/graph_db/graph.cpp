/*
 * graph.cpp — sharded, lock-acquiring implementations of the Graph container.
 *
 * Every public method acquires the shard lock(s) it needs EXACTLY once per
 * vertex it visits and then calls the private _locked/_find helpers: a
 * std::shared_mutex is not recursive (the Windows SRW lock behind it does not
 * support re-acquisition from the same thread), so re-acquiring the lock of a
 * shard that is already held would deadlock. The helpers below are therefore
 * private and never lock.
 *
 * Lock ordering (see the contract in graph.h, whose item 4 is authoritative for
 * the traversal's EDGE-shard read phase):
 *   1. shard locks in strict ASCENDING index order when more than one is taken;
 *   2. shard lock first, vertex lock second, never the inverse.
 * connect/disconnect take at most two shard uniques (ascending) plus the two
 * endpoints' vertex uniques; remove_vertex takes ONE shard unique at a time and
 * releases it before moving to the next neighbour shard; update_content takes
 * one shard shared lock, RELEASES it, then the vertex unique; dirty_vertices
 * takes one shard shared lock plus the visited vertices' shared locks.
 *   3. vertex shard (level 1) before EDGE shard (level 3), never the inverse: the
 * EDGE-filtered walk releases a visit's vertex-shard lock before it batches that
 * visit's edge tests, and a batch takes one edge-shard SHARED lock per touched
 * shard in ascending index. A 1 -> 3 acquisition is the direction the ascending
 * numbering permits (the write paths take it too); what keeps the wait-for graph
 * acyclic is the absence of 3 -> 1.
 *
 * Traversals copy the guid list of each visited vertex under that vertex's
 * shard shared lock into a reused AdjacencyBuffer and release the lock before
 * walking to the next vertex, so a BFS holds no lock between visits and
 * traversals of different shards proceed fully in parallel.
 *
 * Cycle safety: has_path and shortest_path are BFS over an explicit visited
 * set, so a dependency cycle (A -> B -> A) terminates instead of looping.
 */
  #include "graph.h"

  #include <core/fiber.h> // current_execution_id(): the write lease keys on the fiber, not the OS thread

  #include <algorithm>
  #include <cassert>
  #include <cstdio>
  #include <functional>
  #include <mutex>
  #include <thread>
  #include <utility>

namespace kimix::graph_db {

#if defined(KIMIX_GRAPH_DB_WALK_COUNTERS)
// The instrumented build's per-thread walk record (graph.h). Defined here, not in
// the header, so only the library's own translation unit owns the storage; the
// increments below are compiled ONLY when xmake's graph_db_walk_counters option
// is on, so a default build has neither the symbol nor the touch.
namespace src_graph_db_graph {
thread_local WalkCounters g_walk_counters;
} // namespace src_graph_db_graph
using namespace src_graph_db_graph;

WalkCounters &walk_counters() noexcept { return g_walk_counters; }
#endif // KIMIX_GRAPH_DB_WALK_COUNTERS

namespace src_graph_db_graph {

// Distinguishes "graph edge endpoints are the same shard" without comparing
// pointers (std::array elements are contiguous, so indices are the identity).
inline constexpr size_t kNoShard = static_cast<size_t>(-1);

// "no endpoint id" for connect_bulk's staging index (real ids start at 0).
inline constexpr uint32_t kNoEndpoint = static_cast<uint32_t>(-1);


} // namespace src_graph_db_graph
using namespace src_graph_db_graph;

// ---------------------------------------------------------------------------
// Shard
// ---------------------------------------------------------------------------

Graph::Shard::Shard() noexcept = default;

Vertex *Graph::Shard::_find(kimix::Guid const &guid) const noexcept {
    auto index = vertices.find(guid);
    if (!index) {
        return nullptr;
    }
    return index.value().get();
}

Graph::~Graph() noexcept = default;

// ---------------------------------------------------------------------------
// Write-contract bookkeeping (see the WRITE POLICY comment in graph.h)
// ---------------------------------------------------------------------------

Graph::WriteLease Graph::_write_lease() noexcept {
    // kimix::fiber::current_execution_id(), NOT std::this_thread::get_id(): a
    // fiber migrates across the pool's worker OS threads at every yield, so the
    // thread id can change MID-LEASE — the holder would be refused by its own
    // inner call, and a second flow on the former thread would compare "equal"
    // and be let in while the first write is still in flight. The fiber identity
    // is stable for the whole logical flow, migrated or not. An unbound thread
    // cannot yield, so its id is its flow's identity (same helper).
    uint64_t const mine = kimix::fiber::current_execution_id();
    std::lock_guard<std::mutex> guard(_write_mutex);
    if (_write_depth == 0) {
        _write_owner = mine;
        _write_depth = 1;
        return WriteLease(*this, true);
    }
    if (_write_owner == mine) {
        // Same-flow reentry: a public write composing another public write
        // (free_vertex -> remove_vertex is the canonical pair). Depth counting
        // is what keeps the inner call from being refused by the outer one.
        ++_write_depth;
        return WriteLease(*this, true);
    }
    // A DIFFERENT execution flow arrived while a structural write is in flight:
    // the single-writer-at-a-time refusal. In debug this is a bug in the CALLER,
    // so say so loudly; a release build keeps the documented refusal.
#ifndef NDEBUG
    std::fprintf(stderr,
                 "[kimix][graph_db] single-writer contract violated: structural "
                 "write from one execution flow while another flow's write is in flight\n");
    assert(false && "kimix::graph_db::Graph: structural writes must not overlap");
#endif
    return WriteLease(*this, false);
}

void Graph::_release_write() noexcept {
    std::lock_guard<std::mutex> guard(_write_mutex);
    if (--_write_depth == 0) {
        _write_owner = 0; // 0 = no owner; execution ids are never 0
    }
}

bool Graph::is_writer_thread() const noexcept {
    std::lock_guard<std::mutex> guard(_write_mutex);
    return _write_depth == 0 || _write_owner == kimix::fiber::current_execution_id();
}

void Graph::reset_writer_thread() noexcept {
    // The overlap-based rule records no owner between writes, so there is
    // nothing to forget. Retained for source compatibility (older tests and
    // benches call it before taking over a graph they did not build).
}

// ---------------------------------------------------------------------------
// Structure (shard UNIQUE locks, ascending order)
// ---------------------------------------------------------------------------

Vertex *Graph::create_vertex(kimix::span<const std::byte> initial_content) {
    auto const write_lease = _write_lease();
    if (!write_lease) {
        return nullptr;
    }
    // `new Vertex` picks Vertex's own over-aligned operator new (mimalloc), so
    // the cache-line aligned lock region is guaranteed; a unique_ptr built by
    // hand rather than kimix::make_unique keeps that choice explicit.
          kimix::unique_ptr<Vertex> vertex{
          new Vertex(kimix::Guid{true}, initial_content, _options.content_digest)};
    Vertex *raw = vertex.get();
    kimix::Guid guid = raw->guid();
    Shard &shard = _shards[_shard_of(guid)];
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    auto inserted = shard.vertices.try_emplace(guid, std::move(vertex));
    if (!inserted.second) {
        // A guid collision is astronomically unlikely; report it as "not added"
        // rather than corrupting the map. Retry semantics belong to the caller.
        return nullptr;
    }
    _size.fetch_add(1, std::memory_order_relaxed);
    _bump_structure(); // a new vertex is (transiently) a root: invalidate roots()
    return raw;
}

bool Graph::add_vertex(kimix::unique_ptr<Vertex> vertex) {
    if (!vertex) {
        return false;
    }
    auto const write_lease = _write_lease();
    if (!write_lease) {
        return false;
    }
    kimix::Guid guid = vertex->guid();
    Shard &shard = _shards[_shard_of(guid)];
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    auto inserted = shard.vertices.try_emplace(guid, std::move(vertex));
    if (!inserted.second) {
        return false;
    }
    _size.fetch_add(1, std::memory_order_relaxed);
    _bump_structure(); // an adopted vertex is (transiently) a root
    return true;
}

Vertex *Graph::_resolve(kimix::Guid const &guid) const noexcept {
    size_t const shard_index = _shard_of(guid);
    std::shared_lock<std::shared_mutex> lock(_shards[shard_index].mutex);
    return _shards[shard_index]._find(guid);
}

kimix::unique_ptr<Vertex> Graph::remove_vertex(kimix::Guid const &guid) {
    return remove_vertex(guid, nullptr);
}

kimix::unique_ptr<Vertex>
Graph::remove_vertex(kimix::Guid const &guid,
                     kimix::vector<kimix::unique_ptr<Edge>> *removed_edges) {
    auto const write_lease = _write_lease();
    if (!write_lease) {
        return nullptr;
    }
    size_t const own_shard = _shard_of(guid);
    kimix::unique_ptr<Vertex> victim;
    // The cascade's work list: one entry per incident EDGE, holding the edge id,
    // the neighbour guid and whether the victim was its origin (so the neighbour's
    // list to clean is decided without touching the edge record again).
    struct Cascade {
        kimix::Guid edge;
        kimix::Guid neighbour;
        bool victim_is_origin;
    };
    kimix::vector<Cascade> cascade;
    {
        // 1. Detach the victim from its own shard's map, copying out every
        //    incident edge. Only the OWN shard is locked, so no second shard can
        //    be held here.
        std::unique_lock<std::shared_mutex> lock(_shards[own_shard].mutex);
        auto index = _shards[own_shard].vertices.find(guid);
        if (!index) {
            return nullptr;
        }
        victim = std::move(index.value());
        if (!victim) {
            return nullptr;
        }
        cascade.reserve(victim->_out_links.size() + victim->_in_links.size());
        // The link lists are authoritative: one entry per EDGE, so parallel
        // edges and self loops are covered exactly once each (plan v3 §5.2 made
        // them the single adjacency representation).
        for (auto const &link : victim->_out_links) {
            cascade.push_back(Cascade{link.edge, link.neighbour, true});
        }
        for (auto const &link : victim->_in_links) {
            cascade.push_back(Cascade{link.edge, link.neighbour, false});
        }
        _shards[own_shard].vertices.remove(guid);
        _size.fetch_sub(1, std::memory_order_relaxed);
        // Tombstone: a save never deletes a stored key, so the removal has to be
        // recorded for the next save to write (see graph.h).
        _removed_vertices.push_back(guid);
        _bump_structure(); // the victim is gone: it is not a root (nor anything) now
    }

    // 2. Drop the incident edge RECORDS. One edge shard lock at a time, taken
    //    AFTER the vertex shard lock above has been released (level 2 -> 3). A
    //    transaction keeps the removed objects so an abort can put them back.
    for (auto const &item : cascade) {
        kimix::unique_ptr<Edge> removed = _edges.erase(item.edge);
        if (removed) {
            // Tombstone every cascaded edge too, or the next load would bring it
            // back with a dangling endpoint.
            _removed_edges.push_back(item.edge);
            if (removed_edges != nullptr) {
                removed_edges->push_back(std::move(removed));
            }
        }
    }

    // 2b. The key index entry (level 4, one shard, after every higher level is
    //     released). A vertex without a key owns no entry.
    if (victim->has_key()) {
        kimix::Guid removed;
        _keys.erase(victim->key(), removed);
    }

    // 3. Cascade: unlink the victim from every neighbour's link list and guid
    //    projection, ONE shard lock at a time (never two), which is what keeps the
    //    deadlock-freedom argument trivial. The victim stays alive (it is the
    //    unique_ptr above, and it is returned to the caller), so even a reader
    //    that raced the detach can never touch freed memory: at worst it sees the
    //    edge for one more visit.
    for (auto const &item : cascade) {
        if (item.neighbour == guid) {
            // A self loop lives entirely on the victim, which is already detached.
            continue;
        }
        size_t const other_shard = _shard_of(item.neighbour);
        // A neighbour may well live in the victim's OWN shard
        // (1/kGraphShardCount of them do): the lock above was released, so
        // re-taking it here is a fresh acquisition, not recursion.
        std::unique_lock<std::shared_mutex> lock(_shards[other_shard].mutex);
        Vertex *other = _shards[other_shard]._find(item.neighbour);
        if (other == nullptr || other == victim.get()) {
            continue;
        }
        // The neighbour's OUT list holds an edge whose origin is the victim; its
        // IN list an edge whose target is the victim.
        if (item.victim_is_origin) {
            other->_remove_in_link(item.edge, victim.get());
        } else {
            other->_remove_out_link(item.edge, victim.get());
        }
    }
    victim->_clear_links();
    // The cascade's LAST bump. Every neighbour above lost an edge to a vertex that
    // no longer exists, so the root set changed in both directions; this bump is
    // what makes the whole cascade again visible to roots() (the detach's own bump
    // only covered the removal). A reader of the cache can therefore never stash a
    // half-cascaded answer under the final version: the version it stores is the
    // one it read BEFORE its scan, which is older than this one.
    _bump_structure();
    return victim;
}

// ---------------------------------------------------------------------------
// Keyed vertex creation / translation (P2)
// ---------------------------------------------------------------------------

GdiStatus Graph::create_vertex(VertexSpec const &spec, kimix::Guid &out_guid) {
    auto const write_lease = _write_lease();
    if (!write_lease) {
        return GdiStatus::error_state;
    }
    bool const keyed = spec.key_label != kNoLabel;
    if (keyed && spec.external_id.empty()) {
        // GDI's VertexKey.Validate: the label must not be empty, and an id of
        // zero length names nothing.
        return GdiStatus::error_empty_name;
    }
    if (!keyed && !spec.external_id.empty()) {
        return GdiStatus::error_argument; // an id without a label names nothing
    }

    // The key probe happens BEFORE the structural write, under one key shard
    // shared lock (level 4 alone — the hierarchy forbids taking it while a
    // higher level is held, and nothing is held here).
    if (keyed) {
        VertexKeyView const view{spec.key_label, spec.external_id};
        kimix::Guid existing;
        if (_keys.find(view, existing)) {
            if (_options.key_mode == KeyMode::report_warning) {
                // GDI-RMA's under-enforced uniqueness: the caller gets the
                // EXISTING vertex plus the warning, and nothing is created.
                out_guid = existing;
                return GdiStatus::warning_non_unique_id;
            }
            return GdiStatus::error_non_unique_id;
        }
    }

          kimix::unique_ptr<Vertex> vertex{
          new Vertex(kimix::Guid{true}, spec.content, _options.content_digest)};
    if (keyed) {
        vertex->_set_key(spec.key_label, ExternalId::from_bytes(spec.external_id));
    }
    for (auto label : spec.labels) {
        vertex->_labels_unlocked_push(label);
    }
    if (spec.properties != nullptr) {
        vertex->_properties_unlocked_set(*spec.properties);
    }
    Vertex *raw = vertex.get();
    kimix::Guid const guid = raw->guid();
    Shard &shard = _shards[_shard_of(guid)];
    {
        std::unique_lock<std::shared_mutex> lock(shard.mutex);
        auto inserted = shard.vertices.try_emplace(guid, std::move(vertex));
        if (!inserted.second) {
            return GdiStatus::error_intern; // guid collision: astronomically unlikely
        }
        _size.fetch_add(1, std::memory_order_relaxed);
        _bump_structure();
    }
    if (keyed) {
        // Level 4 AFTER the vertex shard has been released.
        GdiStatus const status = _keys.insert(raw->key(), guid);
        if (is_error(status)) {
            // A concurrent create won the race between the probe and now: undo the
            // insertion instead of leaving an unindexed vertex behind.
            std::unique_lock<std::shared_mutex> lock(shard.mutex);
            shard.vertices.remove(guid);
            _size.fetch_sub(1, std::memory_order_relaxed);
            _bump_structure();
            return status;
        }
    }
    out_guid = guid;
    return GdiStatus::ok;
}

GdiStatus Graph::get_or_create_vertex(VertexSpec const &spec, kimix::Guid &out_guid,
                                      bool &created) {
    created = false;
    if (spec.key_label != kNoLabel) {
        VertexKeyView const view{spec.key_label, spec.external_id};
        kimix::Guid existing;
        if (_keys.find(view, existing)) {
            // MERGE semantics: a match is returned untouched — no label, no
            // property is applied (the C# reference does exactly this).
            out_guid = existing;
            return GdiStatus::ok;
        }
    }
    GdiStatus const status = create_vertex(spec, out_guid);
    if (is_error(status)) {
        return status;
    }
    created = true;
    return GdiStatus::ok;
}

GdiStatus Graph::translate_vertex_id(VertexKeyView const &key, kimix::Guid &out_guid,
                                     bool &found) const {
    found = false;
    if (key.label == kNoLabel) {
        return GdiStatus::error_empty_name;
    }
    kimix::Guid existing;
    if (!_keys.find(key, existing)) {
        // GDI's TranslateVertexID answers "no such key" with a null uid, not an
        // error; the caller distinguishes the two through `found`.
        return GdiStatus::ok;
    }
    out_guid = existing;
    found = true;
    return GdiStatus::ok;
}

GdiStatus Graph::free_vertex(kimix::Guid const &guid, bool guarded) {
    if (guarded) {
        if (!contains(guid)) {
            return GdiStatus::error_vertex;
        }
        if (has_incident_edges(guid, EdgeOrientation::both)) {
            // GDI's FreeVertexGuarded: ErrorEdge and NOTHING removed.
            return GdiStatus::error_edge;
        }
    }
    kimix::unique_ptr<Vertex> removed = remove_vertex(guid);
    if (!removed) {
        return GdiStatus::error_vertex;
    }
    return GdiStatus::ok;
}

// ---------------------------------------------------------------------------
// First-class edges (P3)
// ---------------------------------------------------------------------------

bool Graph::_has_out_edge_locked(Vertex const *origin,
                                 kimix::Guid const &target) noexcept {
    for (auto const &link : origin->_out_links) {
        if (link.neighbour == target) {
            return true;
        }
    }
    return false;
}

GdiStatus Graph::_create_edge_locked(Vertex *origin, Vertex *target,
                                     EdgeDirectionType direction,
                                     kimix::span<const LabelId> labels,
                                     PropertyList const *properties, bool via_gdi_api,
                                     kimix::Guid const *chosen_uid, EdgeId &out_edge) {
    if (origin == nullptr || target == nullptr) {
        return GdiStatus::error_vertex;
    }
    bool const self_loop = origin == target;
    if (self_loop && !(via_gdi_api && _options.allow_self_edges)) {
        // The legacy contract: from == to is never an edge.
        return GdiStatus::error_edge;
    }
    if (!via_gdi_api || !_options.allow_parallel_edges) {
        if (_has_out_edge_locked(origin, target->guid())) {
            return GdiStatus::error_edge;
        }
    }
    // ABI 8 (plan §3.1): the caller may name the uid. A minted guid cannot collide,
    // so the insert's refusal on the minted path stays error_intern; a CHOSEN uid
    // that a live edge already holds is the duplicate-id answer, and the check runs
    // before the record is built so a refusal leaves nothing behind.
    EdgeId const id{chosen_uid != nullptr ? *chosen_uid : kimix::Guid{true}};
    if (chosen_uid != nullptr && _edges.contains(id)) {
        return GdiStatus::error_non_unique_id;
    }
    kimix::unique_ptr<Edge> edge{new Edge(id, origin->guid(), target->guid(), direction)};
    for (auto label : labels) {
        if (label == kNoLabel) {
            return GdiStatus::error_label;
        }
        edge->_labels.push_back(label);
    }
    if (properties != nullptr) {
        for (auto const &entry : *properties) {
            edge->_properties.emplace(entry.name, entry.value);
        }
    }
    // Record first, then the adjacency: an adjacency entry that outlives its
    // record would be a dangling edge for every reader, while a record inserted a
    // moment earlier is invisible until a link names it.
    if (!_edges.insert(std::move(edge))) {
        return GdiStatus::error_intern;
    }
    if (chosen_uid != nullptr) {
        // The chosen uid may sit in _removed_edges from a free earlier in the
        // same session: the create re-validates it, so its tombstone must not
        // shadow the new record in the next save. "Free U, then create U again"
        // inside one transaction is the GDI "replace the edge for fact X" shape
        // (ABI-8 feedback, bug 2 — the abort path always did this via
        // adopt_edge's restore_removed_edge).
        restore_removed_edge(id);
    }
    // The two vertex uniques flush a reader that is in the middle of a payload
    // read; for a self loop origin == target and the mutex is taken ONCE
    // (shared_atomic_mutex is not recursive).
    origin->lock_unique();
    if (!self_loop) {
        target->lock_unique();
    }
    origin->_add_out_link(id, target);
    target->_add_in_link(id, origin);
    _bump_structure(); // `target` gained an incoming edge, so it left the root set
    if (!self_loop) {
        target->unlock_unique();
    }
    origin->unlock_unique();
    out_edge = id;
    return GdiStatus::ok;
}

GdiStatus Graph::edge_create(kimix::Guid const &origin, kimix::Guid const &target,
                             EdgeDirectionType direction, kimix::span<const LabelId> labels,
                             PropertyList const *properties, kimix::Guid const *chosen_uid,
                             kimix::Guid &out_edge) {
    auto const write_lease = _write_lease();
    if (!write_lease) {
        return GdiStatus::error_state;
    }
    if (direction != EdgeDirectionType::directed &&
        direction != EdgeDirectionType::undirected) {
        return GdiStatus::error_edge_orientation;
    }
    size_t const origin_shard = _shard_of(origin);
    size_t const target_shard = _shard_of(target);
    if (!_options.allow_self_edges && origin == target) {
        return GdiStatus::error_edge;
    }
    if (origin_shard == target_shard) {
        std::unique_lock<std::shared_mutex> lock(_shards[origin_shard].mutex);
        Vertex *from_vertex = _shards[origin_shard]._find(origin);
        Vertex *to_vertex = _shards[origin_shard]._find(target);
        return _create_edge_locked(from_vertex, to_vertex, direction, labels, properties,
                                   /*via_gdi_api=*/true, chosen_uid, out_edge);
    }
    // Two shards: UNIQUE in ascending shard order (level 2), then the edge shard
    // inside _create_edge_locked (level 3).
    size_t const first = origin_shard < target_shard ? origin_shard : target_shard;
    size_t const second = origin_shard < target_shard ? target_shard : origin_shard;
    std::unique_lock<std::shared_mutex> first_lock(_shards[first].mutex);
    std::unique_lock<std::shared_mutex> second_lock(_shards[second].mutex);
    Vertex *from_vertex = _shards[origin_shard]._find(origin);
    Vertex *to_vertex = _shards[target_shard]._find(target);
    return _create_edge_locked(from_vertex, to_vertex, direction, labels, properties,
                               /*via_gdi_api=*/true, chosen_uid, out_edge);
}

GdiStatus Graph::_free_edge_locked(EdgeId const &edge) {
    EdgeRecord record_copy;
    if (!_edges.record(edge, record_copy)) {
        return GdiStatus::error_uid;
    }
    kimix::unique_ptr<Edge> removed = _edges.erase(edge);
    if (!removed) {
        return GdiStatus::error_uid;
    }
    _removed_edges.push_back(edge);
    // Unlink both sides, one vertex shard at a time (ascending when they differ;
    // a self loop touches its single shard once).
    size_t const origin_shard = _shard_of(record_copy.origin);
    size_t const target_shard = _shard_of(record_copy.target);
    if (origin_shard == target_shard) {
        std::unique_lock<std::shared_mutex> lock(_shards[origin_shard].mutex);
        Vertex *origin = _shards[origin_shard]._find(record_copy.origin);
        Vertex *target = _shards[origin_shard]._find(record_copy.target);
        if (origin != nullptr) {
            origin->_remove_out_link(edge, target);
        }
        if (target != nullptr) {
            target->_remove_in_link(edge, origin);
        }
    } else {
        size_t const first = origin_shard < target_shard ? origin_shard : target_shard;
        size_t const second = origin_shard < target_shard ? target_shard : origin_shard;
        {
            std::unique_lock<std::shared_mutex> first_lock(_shards[first].mutex);
            std::unique_lock<std::shared_mutex> second_lock(_shards[second].mutex);
            Vertex *origin = _shards[origin_shard]._find(record_copy.origin);
            Vertex *target = _shards[target_shard]._find(record_copy.target);
            if (origin != nullptr) {
                origin->_remove_out_link(edge, target);
            }
            if (target != nullptr) {
                target->_remove_in_link(edge, origin);
            }
        }
    }
    _bump_structure(); // the target may be a root now
    return GdiStatus::ok;
}

GdiStatus Graph::edge_free(kimix::Guid const &edge) {
    auto const write_lease = _write_lease();
    if (!write_lease) {
        return GdiStatus::error_state;
    }
    return _free_edge_locked(edge);
}

// One endpoint move, shared by edge_set_origin and edge_set_target. Everything
// happens under ONE ascending lock set: the shards of every vertex involved
// (level 2) and then the edge's own shard (level 3), so a reader can never see
// the record and the adjacency disagree.
GdiStatus Graph::_move_edge_endpoint(EdgeId const &edge, kimix::Guid const &endpoint,
                                     bool move_origin) {
    EdgeRecord record_copy;
    if (!_edges.record(edge, record_copy)) {
        return GdiStatus::error_uid;
    }
    kimix::Guid const new_origin = move_origin ? endpoint : record_copy.origin;
    kimix::Guid const new_target = move_origin ? record_copy.target : endpoint;
    if (new_origin == record_copy.origin && new_target == record_copy.target) {
        return GdiStatus::ok; // already there: no mutation, no dirty flag
    }

    // The distinct vertex shards this move touches, by ascending index.
    std::array<uint8_t, kGraphShardCount> touched{};
    touched[_shard_of(record_copy.origin)] = 1;
    touched[_shard_of(record_copy.target)] = 1;
    touched[_shard_of(new_origin)] = 1;
    touched[_shard_of(new_target)] = 1;
    std::array<std::unique_lock<std::shared_mutex>, kGraphShardCount> held;
    for (size_t shard = 0; shard < kGraphShardCount; ++shard) {
        if (touched[shard] != 0) {
            held[shard] = std::unique_lock<std::shared_mutex>(_shards[shard].mutex);
        }
    }
    Vertex *old_origin = _shards[_shard_of(record_copy.origin)]._find(record_copy.origin);
    Vertex *old_target = _shards[_shard_of(record_copy.target)]._find(record_copy.target);
    Vertex *origin_vertex = _shards[_shard_of(new_origin)]._find(new_origin);
    Vertex *target_vertex = _shards[_shard_of(new_target)]._find(new_target);
    if (origin_vertex == nullptr || target_vertex == nullptr) {
        // Endpoint mutation is validated against the live graph: GDI raises
        // ErrorVertex for a uid that does not exist, and nothing is moved.
        return GdiStatus::error_vertex;
    }

    // Level 3: the edge's own shard, taken AFTER every vertex shard.
    size_t const edge_shard = EdgeTable::shard_of(edge);
    std::unique_lock<std::shared_mutex> edge_lock(_edges.shard_mutex(edge_shard));
    Edge *record = _edges._find_locked(edge_shard, edge);
    if (record == nullptr) {
        return GdiStatus::error_uid;
    }

    // Move the adjacency: an edge contributes exactly one out link (on its
    // origin) and one in link (on its target). Removing by edge id and re-adding
    // keeps parallel edges and self loops correct without special cases.
    if (old_origin != nullptr) {
        old_origin->_remove_out_link(edge, old_target);
    }
    if (old_target != nullptr) {
        old_target->_remove_in_link(edge, old_origin);
    }
    origin_vertex->_add_out_link(edge, target_vertex);
    target_vertex->_add_in_link(edge, origin_vertex);

    record->_origin = new_origin;
    record->_target = new_target;
    record->mark_dirty();
    _bump_structure(); // the new target gained an incoming edge (a former root may differ)
    return GdiStatus::ok;
}

GdiStatus Graph::edge_set_origin(kimix::Guid const &edge, kimix::Guid const &origin) {
    auto const write_lease = _write_lease();
    if (!write_lease) {
        return GdiStatus::error_state;
    }
    return _move_edge_endpoint(edge, origin, /*move_origin=*/true);
}

GdiStatus Graph::edge_set_target(kimix::Guid const &edge, kimix::Guid const &target) {
    auto const write_lease = _write_lease();
    if (!write_lease) {
        return GdiStatus::error_state;
    }
    return _move_edge_endpoint(edge, target, /*move_origin=*/false);
}

// ---------------------------------------------------------------------------
// Legacy edge API (connect / disconnect / connect_bulk), now implemented
// THROUGH the edge table so the two worlds cannot drift apart
// ---------------------------------------------------------------------------

bool Graph::_connect_locked(Shard &from_shard, Shard &to_shard,
                            kimix::Guid const &from, kimix::Guid const &to) {
    Vertex *from_vertex = from_shard._find(from);
    Vertex *to_vertex = to_shard._find(to);
    EdgeId created;
    // via_gdi_api = false: one directed edge per ordered pair, no self loop —
    // exactly the epoch-1..3 contract. connect() never names a uid (ABI 8's
    // chosen-uid path belongs to Graph::edge_create alone).
    return _create_edge_locked(from_vertex, to_vertex, EdgeDirectionType::directed, {}, nullptr,
                               /*via_gdi_api=*/false, nullptr, created) == GdiStatus::ok;
}

bool Graph::_disconnect_locked(Shard &from_shard, Shard &to_shard,
                               kimix::Guid const &from, kimix::Guid const &to) {
    Vertex *from_vertex = from_shard._find(from);
    if (from_vertex == nullptr) {
        return false;
    }
    kimix::Guid const to_guid = to;
    for (auto const &link : from_vertex->_out_links) {
        if (link.neighbour != to_guid) {
            continue;
        }
        // The caller holds both endpoint shards UNIQUE, so the unlink below is
        // performed on the resolved vertices directly; _free_edge_locked would
        // re-acquire them (not recursive).
        EdgeId const edge = link.edge;
        kimix::unique_ptr<Edge> removed = _edges.erase(edge);
        if (!removed) {
            continue;
        }
        // Tombstone the erased record, exactly as _free_edge_locked does: a save
        // never deletes a stored key, so without this the edge record survives
        // the next incremental save and the edge comes back on reopen.
        _removed_edges.push_back(edge);
        from_vertex->_remove_out_link(edge, to_shard._find(to_guid));
        Vertex *to_vertex = to_shard._find(to_guid);
        if (to_vertex != nullptr) {
            to_vertex->_remove_in_link(edge, from_vertex);
        }
        _bump_structure(); // `to` lost an incoming edge, so it may be a root now
        return true;
    }
    return false;
}

bool Graph::connect(kimix::Guid const &from, kimix::Guid const &to) {
    if (from == to) {
        return false;
    }
    auto const write_lease = _write_lease();
    if (!write_lease) {
        return false;
    }
    size_t const from_shard = _shard_of(from);
    size_t const to_shard = _shard_of(to);
    if (from_shard == to_shard) {
        std::unique_lock<std::shared_mutex> lock(_shards[from_shard].mutex);
        return _connect_locked(_shards[from_shard], _shards[from_shard], from, to);
    }
    size_t const first = from_shard < to_shard ? from_shard : to_shard;
    size_t const second = from_shard < to_shard ? to_shard : from_shard;
    std::unique_lock<std::shared_mutex> first_lock(_shards[first].mutex);
    std::unique_lock<std::shared_mutex> second_lock(_shards[second].mutex);
    return _connect_locked(_shards[from_shard], _shards[to_shard], from, to);
}

bool Graph::disconnect(kimix::Guid const &from, kimix::Guid const &to) {
    auto const write_lease = _write_lease();
    if (!write_lease) {
        return false;
    }
    size_t const from_shard = _shard_of(from);
    size_t const to_shard = _shard_of(to);
    if (from_shard == to_shard) {
        std::unique_lock<std::shared_mutex> lock(_shards[from_shard].mutex);
        return _disconnect_locked(_shards[from_shard], _shards[from_shard], from, to);
    }
    size_t const first = from_shard < to_shard ? from_shard : to_shard;
    size_t const second = from_shard < to_shard ? to_shard : from_shard;
    std::unique_lock<std::shared_mutex> first_lock(_shards[first].mutex);
    std::unique_lock<std::shared_mutex> second_lock(_shards[second].mutex);
    return _disconnect_locked(_shards[from_shard], _shards[to_shard], from, to);
}

size_t Graph::connect_bulk(kimix::span<EdgeRequest const> edges,
                           kimix::span<EdgeOutcome> outcomes) noexcept {
    size_t const count = edges.size();
    bool const want_outcomes = outcomes.size() >= count;
    if (count == 0) {
        return 0;
    }
    auto const write_lease = _write_lease();
    if (!write_lease) {
        if (want_outcomes) {
            for (size_t i = 0; i < count; ++i) {
                outcomes[i] = EdgeOutcome::refused;
            }
        }
        return 0;
    }

    // Resolve every DISTINCT endpoint once (one shard shared lock per unique
    // endpoint instead of one Graph::find per edge). Ids stay valid under growth
    // because the dense table only moves its own values (epoch-3 lesson).
    struct Endpoint {
        Vertex *vertex;
        uint32_t shard;
    };
    kimix::vector<Endpoint> endpoints;
    endpoints.reserve(count / 4 + 16);
    kimix::unordered_map<kimix::Guid, uint32_t> resolved;
    resolved.reserve(count / 4 + 16);
    kimix::Guid last_guid{};
    uint32_t last_id = kNoEndpoint;
    auto resolve = [this, &endpoints, &resolved, &last_guid,
                    &last_id](kimix::Guid const &guid) -> uint32_t {
        if (last_id != kNoEndpoint && last_guid == guid) {
            return last_id;
        }
        auto known = resolved.find(guid);
        if (known != resolved.end()) {
            last_guid = guid;
            last_id = known->second;
            return last_id;
        }
        size_t const shard_index = _shard_of(guid);
        Vertex *vertex = nullptr;
        {
            std::shared_lock<std::shared_mutex> lock(_shards[shard_index].mutex);
            vertex = _shards[shard_index]._find(guid);
        }
        if (vertex == nullptr) {
            return kNoEndpoint;
        }
        uint32_t const id = static_cast<uint32_t>(endpoints.size());
        endpoints.push_back(Endpoint{vertex, static_cast<uint32_t>(shard_index)});
        resolved.emplace(guid, id);
        last_guid = guid;
        last_id = id;
        return id;
    };

    kimix::vector<uint32_t> from_ends(count, kNoEndpoint);
    kimix::vector<uint32_t> to_ends(count, kNoEndpoint);
    kimix::vector<EdgeOutcome> verdicts(count, EdgeOutcome::created);
    kimix::vector<uint8_t> pending(count, 0);
    for (size_t i = 0; i < count; ++i) {
        if (edges[i].from == edges[i].to) {
            verdicts[i] = EdgeOutcome::self_edge;
            continue;
        }
        uint32_t const from_id = resolve(edges[i].from);
        if (from_id == kNoEndpoint) {
            verdicts[i] = EdgeOutcome::missing_source;
            continue;
        }
        uint32_t const to_id = resolve(edges[i].to);
        if (to_id == kNoEndpoint) {
            verdicts[i] = EdgeOutcome::missing_target;
            continue;
        }
        from_ends[i] = from_id;
        to_ends[i] = to_id;
        pending[i] = 1;
    }

    std::array<uint8_t, kGraphShardCount> touched{};
    for (size_t i = 0; i < count; ++i) {
        if (pending[i] == 0) {
            continue;
        }
        touched[endpoints[from_ends[i]].shard] = 1;
        touched[endpoints[to_ends[i]].shard] = 1;
    }
    std::array<std::unique_lock<std::shared_mutex>, kGraphShardCount> held;
    for (size_t shard = 0; shard < kGraphShardCount; ++shard) {
        if (touched[shard] != 0) {
            held[shard] = std::unique_lock<std::shared_mutex>(_shards[shard].mutex);
        }
    }

    size_t created = 0;
    for (size_t i = 0; i < count; ++i) {
        if (pending[i] == 0) {
            continue;
        }
        EdgeId edge_id;
        if (_create_edge_locked(endpoints[from_ends[i]].vertex,
                                endpoints[to_ends[i]].vertex,
                                EdgeDirectionType::directed, {}, nullptr,
                                /*via_gdi_api=*/false, nullptr, edge_id) == GdiStatus::ok) {
            ++created;
        } else {
            verdicts[i] = EdgeOutcome::duplicate_edge; // already present (or refused)
        }
    }
    if (want_outcomes) {
        for (size_t i = 0; i < count; ++i) {
            outcomes[i] = verdicts[i];
        }
    }
    return created;
}

GdiStatus Graph::edge_set_direction(kimix::Guid const &edge, EdgeDirectionType direction) {
    auto const write_lease = _write_lease();
    if (!write_lease) {
        return GdiStatus::error_state;
    }
    if (direction != EdgeDirectionType::directed &&
        direction != EdgeDirectionType::undirected) {
        return GdiStatus::error_edge_orientation;
    }
    size_t const shard = EdgeTable::shard_of(edge);
    std::unique_lock<std::shared_mutex> lock(_edges.shard_mutex(shard));
    Edge *record = _edges._find_locked(shard, edge);
    if (record == nullptr) {
        return GdiStatus::error_uid;
    }
    record->_direction = direction;
    record->mark_dirty();
    return GdiStatus::ok;
}

bool Graph::reserve_links(kimix::Guid const &guid, size_t outgoing, size_t incoming) {
    auto const write_lease = _write_lease();
    if (!write_lease) {
        return false;
    }
    // One shard UNIQUE lock (the vertex's own), i.e. exactly the locking level the
    // edge lists live at: the containers being reserved are graph structure, and
    // no second shard is ever involved, so the ascending-index rule is vacuous
    // here. Taking it unique rather than shared is deliberate - reserve() mutates
    // the containers, so a concurrent reader must not walk them meanwhile.
    Shard &shard = _shards[_shard_of(guid)];
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    Vertex *vertex = shard._find(guid);
    if (vertex == nullptr) {
        return false;
    }
    vertex->reserve_links(outgoing, incoming);
    return true;
}

// ---------------------------------------------------------------------------
// Level-2 write: vertex lock ONLY (one shard shared lock for the lookup)
// ---------------------------------------------------------------------------

bool Graph::update_content(kimix::Guid const &guid,
                           kimix::span<const std::byte> content) {
    // Epoch 1: the lookup takes the SHARED lock of the vertex's OWN shard and
    // releases it before the vertex unique lock is taken and before the MD5 is
    // recomputed. Two threads updating two different vertices therefore contend
    // only when their guids happen to share a shard (1/64 of the pairs) — epoch 0
    // serialized all of them on the one graph lock, which is what made this path
    // 17x slower than Vertex::set_content on the same disjoint partitions.
    // Removal is a structural write and the single-writer contract excludes it
    // from running concurrently with an update (see graph.h), so the Vertex*
    // resolved here cannot be freed underneath the vertex lock below.
    Shard &shard = _shards[_shard_of(guid)];
    Vertex *vertex = nullptr;
    {
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        vertex = shard._find(guid);
    }
    if (vertex == nullptr) {
        return false;
    }
    vertex->set_content(content); // vertex UNIQUE lock, shared readers excluded
    return true;
}

// ---------------------------------------------------------------------------
// Reads (one shard SHARED lock each)
// ---------------------------------------------------------------------------

bool Graph::_is_root(Vertex const &vertex) noexcept {
    // A root is a vertex nothing depends on: no incoming links.
    return vertex._in_links.empty();
}

Vertex *Graph::find(kimix::Guid const &guid) const {
    Shard const &shard = _shards[_shard_of(guid)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    return shard._find(guid);
}

bool Graph::contains(kimix::Guid const &guid) const {
    Shard const &shard = _shards[_shard_of(guid)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    return shard._find(guid) != nullptr;
}

kimix::vector<Vertex *> Graph::_compute_roots() const {
    kimix::vector<Vertex *> out;
    for (auto const &shard : _shards) {
        // One shard shared lock at a time: shards are visited sequentially, so
        // this never holds two and never blocks a writer of another shard.
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        for (auto const &entry : shard.vertices) {
            Vertex *vertex = entry.second.get();
            if (vertex != nullptr && _is_root(*vertex)) {
                out.push_back(vertex);
            }
        }
    }
    return out;
}

kimix::vector<Vertex *> Graph::roots() const {
    // Two-level read, mirroring the graph's own design:
    //   1. Take the cache SHARED and compare the structural version. That is the
    //      whole cost of a warm call: one shared lock (readers never block each
    //      other) plus a copy of the cached vector, which is what the caller
    //      wants anyway. The version is read AFTER the lock, so a writer that
    //      bumped it (under its shard lock, before releasing it) is visible.
    //   2. Only a stale cache takes it UNIQUE and recomputes. The version stored
    //      is the one read at step 1, i.e. the state _compute_roots() is about to
    //      observe from the start: a structural write landing mid-scan bumps the
    //      counter again (the writer's bump does not need this mutex), so the
    //      cache is left stale and the next call recomputes instead of caching a
    //      half-updated answer.
    {
        std::shared_lock<std::shared_mutex> lock(_roots_mutex);
        if (_roots_valid) {
            uint64_t const version = _structure_version.load(std::memory_order_relaxed);
            if (_cached_root_version == version) {
                return _cached_roots;
            }
        }
    }
    std::unique_lock<std::shared_mutex> lock(_roots_mutex);
    uint64_t const version = _structure_version.load(std::memory_order_relaxed);
    if (_roots_valid && _cached_root_version == version) {
        return _cached_roots; // another reader recomputed while we waited
    }
    _cached_roots = _compute_roots();
    _cached_root_version = version;
    _roots_valid = true;
    return _cached_roots;
}

// ---------------------------------------------------------------------------
// Adjacency (allocating and non-allocating forms)
// ---------------------------------------------------------------------------

kimix::vector<kimix::Guid> Graph::_copy_adjacency(kimix::Guid const &guid,
                                                  bool outgoing) const {
    // The allocating form copies the guid list straight into the returned vector
    // under one shard shared lock: one allocation, no intermediate buffer.
    kimix::vector<kimix::Guid> out;
    Shard const &shard = _shards[_shard_of(guid)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    Vertex *vertex = shard._find(guid);
    if (vertex == nullptr) {
        return out;
    }
    // The link list is the one adjacency representation (plan v3 §5.2); the
    // guid view is its neighbour field, copied under the same single lock.
    auto const &links = outgoing ? vertex->_out_links : vertex->_in_links;
    out.reserve(links.size());
    for (auto const &link : links) {
        out.push_back(link.neighbour);
    }
    return out;
}

kimix::vector<kimix::Guid> Graph::dependencies(kimix::Guid const &guid) const {
    return _copy_adjacency(guid, true);
}

kimix::vector<kimix::Guid> Graph::dependents(kimix::Guid const &guid) const {
    return _copy_adjacency(guid, false);
}

bool Graph::_read_adjacency(kimix::Guid const &guid, bool outgoing,
                            AdjacencyBuffer &out) const noexcept {
    out._clear();
    Shard const &shard = _shards[_shard_of(guid)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    Vertex *vertex = shard._find(guid);
    if (vertex == nullptr) {
        return false;
    }
    out._assign_links(outgoing ? vertex->_out_links : vertex->_in_links);
    return true;
}

size_t Graph::_read_adjacency(kimix::Guid const &guid, bool outgoing,
                              kimix::span<kimix::Guid> out) const noexcept {
    Shard const &shard = _shards[_shard_of(guid)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    Vertex *vertex = shard._find(guid);
    if (vertex == nullptr) {
        return 0;
    }
    auto const &source = outgoing ? vertex->_out_links : vertex->_in_links;
    size_t const copy = std::min(source.size(), out.size());
    for (size_t i = 0; i < copy; ++i) {
        out[i] = source[i].neighbour;
    }
    return source.size();
}

bool Graph::_read_oriented_adjacency(kimix::Guid const &guid, EdgeOrientation orientation,
                                     AdjacencyBuffer &out) const noexcept {
    out._clear();
    Shard const &shard = _shards[_shard_of(guid)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    Vertex *vertex = shard._find(guid);
    if (vertex == nullptr) {
        return false;
    }
    if (orientation == EdgeOrientation::both) {
        // One lock, one snapshot: the undirected union's two halves are copied
        // together (duplicates between them are the caller's dedup problem).
        out._assign_two_links(vertex->_out_links, vertex->_in_links);
    } else {
        out._assign_links(orientation == EdgeOrientation::outgoing ? vertex->_out_links
                                                                   : vertex->_in_links);
    }
    return true;
}

bool Graph::dependencies(kimix::Guid const &guid, AdjacencyBuffer &out) const noexcept {
    return _read_adjacency(guid, true, out);
}

bool Graph::dependents(kimix::Guid const &guid, AdjacencyBuffer &out) const noexcept {
    return _read_adjacency(guid, false, out);
}

size_t Graph::dependencies(kimix::Guid const &guid,
                           kimix::span<kimix::Guid> out) const noexcept {
    return _read_adjacency(guid, true, out);
}

size_t Graph::dependents(kimix::Guid const &guid,
                         kimix::span<kimix::Guid> out) const noexcept {
    return _read_adjacency(guid, false, out);
}

kimix::vector<kimix::Guid> Graph::neighbors(kimix::Guid const &guid) const {
    // Undirected adjacency = outgoing + incoming, deduplicated. Epoch 0 built a
    // kimix::unordered_set per call (an allocation + one XXH3 per guid) and copied
    // both lists into the result; the depth-3 expansion of the benchmark issues one
    // such call per user op, so this was a visible share of the op.
    //
    // Epoch 1: dedup into a STACK array first and materialise the vector once with
    // its exact size, i.e. exactly one allocation for a vertex whose combined
    // degree fits the array — which covers the whole 4..8 out-degree read graph and
    // any realistic hub-free shape. Only a vertex with a larger combined degree
    // falls back to the hash set, where the quadratic linear scan would start to
    // cost more than the hashing. Both paths produce the identical order: the
    // outgoing list first, then the incoming list, each skipping a guid already
    // emitted.
    constexpr size_t kStackDedup = 16;
    Shard const &shard = _shards[_shard_of(guid)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    Vertex *vertex = shard._find(guid);
    if (vertex == nullptr) {
        return {};
    }
    // Guid view of the one adjacency representation (plan v3 §5.2): the link
    // lists' neighbour fields, same outgoing-first order the projections had.
    auto const &depending = vertex->_out_links;
    auto const &depended = vertex->_in_links;
    size_t const total = depending.size() + depended.size();
    if (total <= kStackDedup) {
        kimix::Guid deduped[kStackDedup];
        size_t count = 0;
        for (auto const &link : depending) {
            if (std::find(deduped, deduped + count, link.neighbour) == deduped + count) {
                deduped[count++] = link.neighbour;
            }
        }
        for (auto const &link : depended) {
            if (std::find(deduped, deduped + count, link.neighbour) == deduped + count) {
                deduped[count++] = link.neighbour;
            }
        }
        return kimix::vector<kimix::Guid>{deduped, deduped + count};
    }
    kimix::vector<kimix::Guid> out;
    out.reserve(total);
    kimix::unordered_set<kimix::Guid> seen;
    seen.reserve(total);
    for (auto const &link : depending) {
        if (seen.insert(link.neighbour).second) {
            out.push_back(link.neighbour);
        }
    }
    for (auto const &link : depended) {
        if (seen.insert(link.neighbour).second) {
            out.push_back(link.neighbour);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Traversals (one shard SHARED lock per VISIT, none held between visits)
// ---------------------------------------------------------------------------

namespace src_graph_db_graph {

// Small-BFS visited set: the discovery container itself IS the visited set (every
// discovered vertex is pushed exactly once in discovery order), so a linear scan
// of it is both the cheapest and the smallest form for a bounded traversal. Only
// when the walk grows past kLinearVisitedLimit does `hash` take over, primed with
// the entries already discovered, so the membership test stays O(1) from then on.
// This removes one hash-set allocation + one hash per hop from has_path's inner
// loop, which is the common case (the benchmark's pairs are 1-2 hops apart).
// The discovery container is the guid vector of the two unfiltered callers
// (has_path's `queue`, traverse_bfs' `discovery`), so the scan takes a projection
// instead of a second mirror vector of guids — the projection is inlined, so it costs
// a byte offset, not a call. The Container/Key generality is HISTORY, not a live
// requirement: the EDGE-filtered walk used to pass its node array here (PathWalkNode /
// ReachabilityWalkNode, both of which keep the discovered guid in their first field),
// but it has its own exact table now (WalkVisited, plan O13), so `guid_identity` —
// the identity — is the only projection any caller passes. Kept as-is: a template is not
// dead code and narrowing it to a Guid vector would change nothing but the shape.
constexpr size_t kLinearVisitedLimit = 32;

// The identity projection, for a discovery container that already stores guid
// keys (has_path's `queue`, traverse_bfs' `discovery`): the only projection in use.
// Not named guid_key: utils.h declares guid_key(kimix::Guid const &) for the storage
// key, and the unity build puts both in one translation unit.
inline kimix::Guid const &guid_identity(kimix::Guid const &guid) noexcept {
    return guid;
}

class VisitedSet {
public:
    // `linear_limit` is how many discovered entries the linear scan covers before
    // the hash set takes over. It is a defaulted KNOB with no live caller that passes
    // anything but kLinearVisitedLimit: the EDGE-filtered walk, which used to raise it
    // to 64, no longer constructs this class at all (it uses WalkVisited, plan O13),
    // so the parameter survives for the unfiltered callers' default behaviour and the
    // value it was raised to survives as kFilteredVisitedLinearLimit. The measurement
    // that settled the 32-entry default still holds and is why it was not lowered: on
    // the S2/F1 row a bounded 3-hop walk discovers 38-45 nodes — just past a 32-entry
    // limit — so such a walk would allocate a hash set, prime 38-45 entries into it
    // and throw it away (two allocations for a 1.7 us call), where a 64-entry scan
    // costs ~1 ns per compare over a contiguous 1 KB array and allocates nothing. The
    // unfiltered pairs are 1-2 hops apart and stay inside the scan, so 32 is right
    // for them.
    explicit VisitedSet(size_t linear_limit = kLinearVisitedLimit) noexcept
        : _linear_limit(linear_limit) {}

    // A HINT only: the bucket array belongs to the hash set, so it is allocated at
    // the switch-over below and never before. A walk that stays inside the linear
    // scan — every bounded read of the 4..8 out-degree graph — therefore allocates
    // no hash table at all, which is what its own comment always claimed.
    void reserve(size_t n) {
        _hint = n < _linear_limit ? _linear_limit : n;
    }

    // Was `key(entry)` already discovered? `queue` is the discovery order.
    template <typename Container, typename Key>
    [[nodiscard]] bool contains(Container const &queue, Key const &key,
                                kimix::Guid const &guid) {
        if (!_hashed) {
            for (auto const &entry : queue) {
                if (key(entry) == guid) {
                    return true;
                }
            }
            return false;
        }
        return _hash.find(guid) != _hash.end();
    }

    [[nodiscard]] bool contains(kimix::vector<kimix::Guid> const &queue,
                                kimix::Guid const &guid) {
        return contains(queue, guid_identity, guid);
    }

    // Insert `guid` (the caller has already pushed it onto `queue`).
    template <typename Container, typename Key>
    void insert(Container const &queue, Key const &key, kimix::Guid const &guid) {
        if (_hashed) {
            _hash.insert(guid);
            return;
        }
        if (queue.size() > _linear_limit) {
            // Switch over: prime the hash with everything discovered so far, and
            // size its bucket array here — the first point at which it is used, so
            // the caller's reserve hint is finally worth an allocation.
            size_t const prime = queue.size() * 2;
            _hash.reserve(prime > _hint ? prime : _hint);
            for (auto const &known : queue) {
                _hash.insert(key(known));
            }
            _hashed = true;
        }
    }

    void insert(kimix::vector<kimix::Guid> const &queue, kimix::Guid const &guid) {
        insert(queue, guid_identity, guid);
    }

private:
    kimix::unordered_set<kimix::Guid> _hash;
    size_t _linear_limit{kLinearVisitedLimit};
    size_t _hint{0};
    bool _hashed{false};
};

/*
 * A vector whose first N elements live in the object itself, with a heap vector
 * taking over on growth (plan O5). This is AdjacencyBuffer's idiom, generalised so
 * the EDGE-filtered walk can keep its whole per-call state in its stack frame:
 * `nodes`, the two level frontiers and the link snapshot are all inline-backed, and
 * a walk that stays inside its budget allocates NOTHING — no node array, no
 * frontier, no hash table.
 *
 * The inline storage is deliberately RAW (bytes + placement new, no `{}` array):
 * zero-initialising a few kilobytes of stack on every call measured at ~100 ns on
 * this host, which is the whole cost of a 1-hop walk. The elements are created by
 * push_back() and are destroyed nowhere, because every T used here is trivially
 * copyable — the same reason AdjacencyBuffer needs no element bookkeeping.
 *
 * Growth is the documented fallback, not a failure: the heap vector keeps its
 * capacity (so a re-grown walk re-uses it instead of allocating again), and the walk
 * is a plain local with NO recursion, so the stack cost is one frame of
 * N * sizeof(T) per buffer. The `_data` pointer is the only state that
 * distinguishes the two forms, exactly like AdjacencyBuffer: `_heap` outlives
 * clear(), so a buffer that has grown once stays grown.
 *
 * Not copyable and not movable on purpose: `_data` points at its own inline array.
 */
template <typename T, size_t N>
class InlineBuffer {
public:
    static constexpr size_t kInlineCapacity = N;
    InlineBuffer() noexcept : _data(reinterpret_cast<T *>(_storage)) {}
    InlineBuffer(InlineBuffer const &) = delete;
    InlineBuffer &operator=(InlineBuffer const &) = delete;
    InlineBuffer(InlineBuffer &&) = delete;
    InlineBuffer &operator=(InlineBuffer &&) = delete;
    ~InlineBuffer() noexcept = default;

    [[nodiscard]] size_t size() const noexcept { return _size; }
    [[nodiscard]] bool empty() const noexcept { return _size == 0; }
    [[nodiscard]] T *data() noexcept { return _data; }
    [[nodiscard]] T const *data() const noexcept { return _data; }
    [[nodiscard]] T *begin() noexcept { return _data; }
    [[nodiscard]] T *end() noexcept { return _data + _size; }
    [[nodiscard]] T const *begin() const noexcept { return _data; }
    [[nodiscard]] T const *end() const noexcept { return _data + _size; }
    [[nodiscard]] T &operator[](size_t index) noexcept { return _data[index]; }
    [[nodiscard]] T const &operator[](size_t index) const noexcept { return _data[index]; }

    // Drop the elements, KEEP the storage: a walk that clears a level's state
    // between levels must not pay for it again.
    void clear() noexcept {
        if (_data != _local_ptr()) {
            _heap.clear();
        }
        _size = 0;
    }

    void push_back(T const &value) {
        T *const local = _local_ptr();
        if (_data == local && _size < N) {
            ::new (static_cast<void *>(local + _size)) T(value);
            ++_size;
            return;
        }
        if (_data == local) {
            // First growth: adopt what is inline into the heap vector, which then
            // owns the storage for the rest of the walk.
            _heap.assign(local, local + _size);
        }
        _heap.push_back(value);
        _data = _heap.data();
        _size = _heap.size();
    }

private:
    [[nodiscard]] T *_local_ptr() noexcept { return reinterpret_cast<T *>(_storage); }
    [[nodiscard]] T const *_local_ptr() const noexcept {
        return reinterpret_cast<T const *>(_storage);
    }

    alignas(T) unsigned char _storage[N * sizeof(T)];
    T *_data;
    size_t _size{0};
    kimix::vector<T> _heap; // only once the walk grows past kInlineCapacity
};

} // namespace src_graph_db_graph
using namespace src_graph_db_graph;

bool Graph::has_path(kimix::Guid const &from, kimix::Guid const &to) const {
    // Epoch 0 did two upfront lookups ("unknown endpoint => false"). Neither is
    // needed for the walk: a BFS from an absent `from` discovers nothing, and an
    // absent `to` can never be reached. Only the zero-length case has to ask
    // whether `from` exists, so the common path pays ONE shard shared lock fewer
    // than epoch 0 (and the walk's first visit already re-resolves `from`).
    if (from == to) {
        return contains(from);
    }
    // Breadth-first walk over outgoing edges with an explicit visited set: a
    // dependency cycle is entered once per vertex, so the walk always
    // terminates. `cursor` walks the queue instead of erasing from its front,
    // which keeps the queue a plain vector.
    kimix::vector<kimix::Guid> queue;
    queue.reserve(16);
    VisitedSet visited;
    visited.reserve(16);
    AdjacencyBuffer adjacency; // reused: no per-visit allocation
    queue.push_back(from);
    visited.insert(queue, from);
    for (size_t cursor = 0; cursor < queue.size(); ++cursor) {
        kimix::Guid const current = queue[cursor];
        if (!dependencies(current, adjacency)) {
            continue; // removed between the enqueue and the visit
        }
        for (auto const &next : adjacency) {
            if (next == to) {
                return true;
            }
            if (!visited.contains(queue, next)) {
                queue.push_back(next);
                visited.insert(queue, next);
            }
        }
    }
    return false;
}

kimix::vector<kimix::Guid>
Graph::shortest_path(kimix::Guid const &from, kimix::Guid const &to) const {
    // Same reasoning as has_path: an absent endpoint cannot appear in the
    // predecessor table, so a walk from an unknown `from` and a walk towards an
    // unknown `to` both return {} on their own. Only from == to needs a lookup.
    if (from == to) {
        return contains(from) ? kimix::vector<kimix::Guid>{from}
                              : kimix::vector<kimix::Guid>{};
    }
    // BFS with a predecessor table; the visited set doubles as the cycle guard,
    // so a cyclic graph terminates with the shortest simple path (or {}).
    kimix::vector<kimix::Guid> frontier;
    frontier.push_back(from);
    kimix::vector<kimix::Guid> next_frontier;
    kimix::unordered_map<kimix::Guid, kimix::Guid> predecessor;
    predecessor.reserve(32);
    predecessor.emplace(from, from);
    AdjacencyBuffer adjacency; // reused: no per-visit allocation
    bool found = false;
    while (!frontier.empty() && !found) {
        next_frontier.clear();
        for (auto const &current : frontier) {
            if (!dependencies(current, adjacency)) {
                continue;
            }
            for (auto const &next : adjacency) {
                if (predecessor.find(next) != predecessor.end()) {
                    continue;
                }
                predecessor.emplace(next, current);
                if (next == to) {
                    found = true;
                    break;
                }
                next_frontier.push_back(next);
            }
            if (found) {
                break;
            }
        }
        frontier.swap(next_frontier);
    }
    if (!found) {
        return {};
    }
    kimix::vector<kimix::Guid> path;
    kimix::Guid cursor = to;
    while (cursor != from) {
        path.push_back(cursor);
        auto it = predecessor.find(cursor);
        if (it == predecessor.end()) {
            return {};
        }
        cursor = it->second;
    }
    path.push_back(from);
    std::reverse(path.begin(), path.end());
    return path;
}

GdiStatus Graph::traverse_bfs(kimix::Guid const &start, Constraint const &constraint,
                              EdgeOrientation orientation, int64_t cap_per_level,
                              uint32_t max_depth, kimix::vector<TraversalVertex> &out) const {
    out.clear();
    if (cap_per_level < 1) {
        return GdiStatus::error_argument;
    }
    if (max_depth < 1 || max_depth > kMaxNativeHopDepth) {
        return GdiStatus::error_argument;
    }
    // The _resolve_constraint rule, validated here so the container API refuses
    // a label ordering op exactly like the Database facade does (the facade
    // re-runs it through _resolve_constraint, which is idempotent).
    for (auto const &condition : constraint.conditions) {
        if (condition.is_label() && condition.op != GdiOp::equal &&
            condition.op != GdiOp::not_equal) {
            return GdiStatus::error_op;
        }
    }
    if (!contains(start)) {
        return GdiStatus::error_vertex;
    }
    bool const filtered = !constraint.conditions.empty();
    kimix::vector<TraversalVertex> result;
    result.reserve(static_cast<size_t>(cap_per_level) + 1);
    kimix::vector<kimix::Guid> frontier;
    kimix::vector<kimix::Guid> next;
    frontier.push_back(start);
    result.push_back(TraversalVertex{start, kimix::Guid{}, 0});
    // The has_path discipline: `discovery` (every guid ever enqueued, in order)
    // doubles as the small-BFS visited set — a linear scan up to
    // kLinearVisitedLimit, then the hash set primes from it. A 2-hop read with
    // cap 100 rarely pays for a hash table.
    kimix::vector<kimix::Guid> discovery;
    discovery.reserve(64);
    discovery.push_back(start);
    VisitedSet visited;
    visited.reserve(32);
    AdjacencyBuffer adjacency;
    kimix::vector<LabelId> candidate_labels;    // filtered path only, reused
    uint32_t depth = 0;
    while (!frontier.empty() && depth < max_depth) {
        ++depth;
        next.clear();
        int64_t level_count = 0;
        bool level_full = false; // per level: a full level still feeds the next one
        for (size_t i = 0; i < frontier.size() && !level_full; ++i) {
            kimix::Guid const current = frontier[i];
            if (!_read_oriented_adjacency(current, orientation, adjacency)) {
                continue; // removed between the enqueue and the visit
            }
            for (auto const &candidate : adjacency) {
                if (level_count >= cap_per_level) {
                    level_full = true; // the level stops expanding at its cap
                    break;
                }
                if (visited.contains(discovery, candidate)) {
                    continue;
                }
                if (filtered) {
                    // §5.10: the filter applies to the discovered VERTEX, so a
                    // failed candidate is not enqueued and its neighbourhood is
                    // never visited.  Payload read under the candidate's leaf
                    // SHARED lock (labels/properties take it internally); a
                    // vanished endpoint is skipped like the composite's find().
                    Vertex *vertex = _resolve(candidate);
                    if (vertex == nullptr) {
                        continue;
                    }
                    candidate_labels.clear();
                    vertex->labels(candidate_labels);
                    // §6.6 zero-copy: probe the borrowed values through one
                    // leaf shared lock instead of deep-copying every property
                    // of every candidate vertex.
                    auto probe = [vertex](PropNameId name) -> PropertyValue const * {
                        return vertex->find_property(name);
                    };
                    if (!matches_probe(constraint,
                                       kimix::span<const LabelId>{candidate_labels.data(),
                                                                  candidate_labels.size()},
                                       probe)) {
                        continue;
                    }
                }
                discovery.push_back(candidate);
                visited.insert(discovery, candidate);
                result.push_back(TraversalVertex{candidate, current, depth});
                next.push_back(candidate);
                ++level_count;
            }
        }
        frontier.swap(next);
    }
    out = std::move(result);
    return GdiStatus::ok;
}

GdiStatus Graph::shortest_path(kimix::Guid const &from, kimix::Guid const &to,
                               uint32_t max_depth, kimix::vector<kimix::Guid> &out) const {
    out.clear();
    if (max_depth < 1 || max_depth > kMaxNativeHopDepth) {
        return GdiStatus::error_argument;
    }
    if (!contains(from) || !contains(to)) {
        return GdiStatus::error_vertex;
    }
    if (from == to) {
        out.push_back(from);
        return GdiStatus::ok;
    }
    // The unbounded shortest_path's BFS, with a level counter: a frontier at
    // max_depth is examined for `to` but not expanded past it.  Unreachable
    // within the bound is an EMPTY result with status ok (GDI's "no path"
    // answer is a nullable list, not an error).
    kimix::vector<kimix::Guid> frontier;
    frontier.push_back(from);
    kimix::vector<kimix::Guid> next_frontier;
    kimix::unordered_map<kimix::Guid, kimix::Guid> predecessor;
    predecessor.reserve(32);
    predecessor.emplace(from, from);
    AdjacencyBuffer adjacency;
    bool found = false;
    uint32_t depth = 0;
    while (!frontier.empty() && !found && depth < max_depth) {
        ++depth;
        next_frontier.clear();
        for (auto const &current : frontier) {
            if (!dependencies(current, adjacency)) {
                continue;
            }
            for (auto const &next : adjacency) {
                if (predecessor.find(next) != predecessor.end()) {
                    continue;
                }
                predecessor.emplace(next, current);
                if (next == to) {
                    found = true;
                    break;
                }
                next_frontier.push_back(next);
            }
            if (found) {
                break;
            }
        }
        frontier.swap(next_frontier);
    }
    if (!found) {
        return GdiStatus::ok; // out stays empty
    }
    kimix::Guid cursor = to;
    while (cursor != from) {
        out.push_back(cursor);
        auto it = predecessor.find(cursor);
        if (it == predecessor.end()) {
            out.clear();
            return GdiStatus::ok; // defensive: cannot happen after `found`
        }
        cursor = it->second;
    }
    out.push_back(from);
    std::reverse(out.begin(), out.end());
    return GdiStatus::ok;
}

// ---------------------------------------------------------------------------
// EDGE-filtered traversals (the EDGE half of the §5.10 filter story)
// ---------------------------------------------------------------------------

namespace src_graph_db_graph {

// One DISCOVERED vertex of the edge-filtered BFS, as the walk's `nodes` array records
// it (discovery order == BFS order). The record is the WALK MODE's record (plan O6,
// option (a)): that array is the walk's discovery container AND the projection its
// small-BFS visited set scans, so both modes keep it, but only the path mode needs the
// predecessor chain the PathStep assembly walks back. Sizes are the lever's whole
// point — 48 B vs 16 B per discovered vertex — so the two records are separate types
// and `_filtered_walk`'s body asks `Node::builds_path` what to do, never a runtime flag.
//
// The predecessor is an INDEX into `nodes`, not a guid: the walk already stores every
// discovered vertex in discovery order, so a second map from guid to predecessor would
// be a redundant structure (the vertex-only BFS above needs one because it keeps no
// node array at all).
// `edge` is the edge that discovered this node, i.e. the hop to report in the PathStep
// — with parallel edges between one ordered pair, that is the only place the choice is
// observable.
struct PathWalkNode {
    static constexpr bool builds_path = true;
    kimix::Guid vertex;
    EdgeId edge;
    size_t parent; // index of the discovering node; 0 for the start (itself)
    uint32_t depth;

    PathWalkNode(kimix::Guid const &vertex_, EdgeId const &edge_, size_t parent_,
                 uint32_t depth_) noexcept
        : vertex(vertex_), edge(edge_), parent(parent_), depth(depth_) {}
};

// The reachability record: the vertex alone. `edge`, `parent` and `depth` exist ONLY to
// rebuild a PathStep sequence, so the mode that assembles none does not carry them —
// has_path's discovery array is 16 B/vertex instead of 48 B/vertex. The four-argument
// constructor mirrors PathWalkNode's so the walk's `discover` has ONE spelling for both
// modes (nothing is a branch here: the discarded arguments are compile-time absent).
struct ReachabilityWalkNode {
    static constexpr bool builds_path = false;
    kimix::Guid vertex;

    ReachabilityWalkNode(kimix::Guid const &vertex_, EdgeId const &, size_t,
                         uint32_t) noexcept
        : vertex(vertex_) {}
};

constexpr size_t kNoNode = static_cast<size_t>(-1);

// The walk's INLINE budgets (plan O5). They are the stack cost, because each one is
// a stack array in InlineBuffer / LinkBuffer: 64 nodes (the path mode's 64 x 48 B =
// 3 072 B, the node array being 62 % of the state; the reachability mode's record is
// 16 B, so its array is 64 x 16 B = 1 024 B) + 2 x 64 frontier indices (1 024 B) + 16
// links per snapshot (512 B) = 4 608 B of buffer for the path mode, in one stack frame,
// with no recursion and nothing that grows with the graph.
//
// Initialisation, precisely: the walk's own three buffers are RAW storage written by
// placement new (InlineBuffer) or by copy_oriented_links' placement new (LinkBuffer),
// so they cost stack SPACE and no stores. Only the level-3 scratch is {}-initialised,
// and only once it is ENGAGED (a wide visit), where its 4 x 64-entry shard tables must
// start zeroed — that is how a drain detects a touched shard: +1 280 B, so the worst
// case is ~5.9 KB, not 4.6 KB. The visited TABLE (WalkVisited, plan O13) adds its own
// 512 x 4 B = 2 KB of raw slot storage to the frame — also written only when engaged
// (a walk that discovers more than kFilteredVisitedLinearLimit vertices), so it too is
// stack space and no stores for a short walk: ~7.9 KB worst case, i.e. 0.77 % of the
// 1 MiB marl fiber stack (src/ext/marl/include/marl/scheduler.h) once both are engaged.
//
// The sizes are the smallest that keep the SHORT filtered walks allocation-free, as
// MEASURED on the S1/S2/F1 rows: 1 hop is 7 discovered nodes, 3 hops discover 49-56
// of them with a level up to 43 wide (48 was one node short), out-degree 4..8 and an
// in-degree of the same order. Nothing here is sized for the hub fixtures (degree
// 5 000) or for the 32-level bound: growing to the heap IS the fallback, and a short
// walk pays nothing for it.
inline constexpr size_t kFilteredVisitedLinearLimit = 64;
inline constexpr size_t kInlineWalkNodes = 64;
inline constexpr size_t kInlineWalkFrontier = 64;

// A visit whose oriented link list is at least this wide takes the BATCHED filter
// (plan O2) for its TAIL; a narrower one stays per-link throughout. Two rules decide
// the split, and both come from measurement rather than from the plan's model:
//
//   * the LOCK is a small part of the per-candidate cost. `edge_matches` measures
//     20.4 ns on the S11 micro-row, but cutting a drain's lock count by 52 % moved
//     `edges/S2_3hop_hub/F4` by ~1.06x, i.e. an edge-shard SHARED lock is only
//     ~5-7 ns of that 20.4 and the rest is the shard-map probe, which batching
//     cannot remove. A drain of n candidates over s touched shards therefore saves
//     (n - s) * ~6 ns and pays a few ns per candidate to stage, sort and scatter; at
//     n ~ s (a small drain spread over the 64 shards) it is a pure loss, which is what
//     this bound keeps a normal-degree visit out of;
//   * the EARLY EXIT dominates a wide visit. A drain stages before it consumes, so it
//     cannot pay off the visited gate the way the per-link path does: on
//     `edges/S1_1hop_label100/F4` a 1-hop lookup that finds its target at link index 7
//     of a 5 000-link hub probed 5 000 edges and took 320 edge locks where 8 probes
//     and 8 locks answer it (96 888 ns vs 2 958) — hence kEdgeFilterHeadLinks below.
//
// Neither number is a sharp measured crossover, and both are recorded as CONSERVATIVE
// choices rather than as a fitted knee. 128 sits above the width at which draining a
// WHOLE visit measured ~2.4-2.5x slower on `edges/S3_8hop_label100/F1` in the
// intermediate build that implemented it (139-149 us against 51-58 us for the shipped
// visit-wide shape). That build was NOT retained, which is why the ratio is quoted as a
// band and not as a number; the MECHANISM is reproducible from the shipped tree and is
// what makes the deviation sound: deferring a level's discovery makes it re-probe every
// link the level re-encounters, `[ctr]` 515 -> 2 460 probes, i.e. exactly
// links_snapshotted. Above 128 the hub rows lose ~51 % of their edge locks with the time
// axis unchanged. Both paths are semantically identical: same discovery order, same
// verdicts, first matching parallel edge still wins.
inline constexpr size_t kEdgeFilterMinLinks = 128;

// How many of a wide visit's links take the per-link path before the tail is drained
// (see above), and the size of the FIRST tail drain: the tail ESCALATES
// 64 -> 256 -> 1024 (kEdgeFilterChunk) and stays there. 64 x edge_matches' ~20 ns
// bounds what an early hit can cost right after the head at ~1.3 us and covers the
// measured early-hit row (`S1_1hop_label100/F4`, link index 7) by 8x, and the escalation
// keeps the next drains small too. A hit inside a drain is only noticed when that drain
// is consumed, so the CURRENT drain is the real bound: ~1.3 us just past the head, ~5 us
// in the 256 drain, ~21 us (1 024 x 20.4 ns) once the visit has reached the steady-state
// chunk. That is a bound, not a per-link guarantee.
inline constexpr size_t kEdgeFilterHeadLinks = 64;

/*
 * The EDGE-filtered walk's visited set (plan O13): a WALK-LOCAL, EXACT,
 * open-addressing table whose slots hold a 32-bit INDEX into the walk's `nodes`
 * array (kEmptySlot == empty), never a hash. Two consequences, and they are the
 * whole design:
 *
 *   * MEMBERSHIP STAYS EXACT. A collision is resolved by comparing the candidate
 *     guid with `nodes[index].vertex` — the full 16-byte comparison the linear scan
 *     already runs — so a slot can only ever report a vertex that was really
 *     discovered. No Bloom/fingerprint filter, no false positive that would silently
 *     drop a path: the walk's answer stays bit-identical to the brute force's.
 *   * NO CROSS-DLL HASH. The key is a cheap inline mix of the guid's two words, so a
 *     probe costs one multiply-and-shift plus a slot read and (usually) one 16-byte
 *     compare. The set it replaces (VisitedSet above, used past its linear bound) paid
 *     kimix::hash64 — an exported XXH3 call — plus an ankerl bucket probe for every
 *     candidate, which the E2 record measured as the DOMINANT remaining term of the
 *     deep rows: 2 460 / 17 460 / 9 000 membership probes on S3, S4-wide and the hub.
 *
 * SHAPE. `kInlineSlots` (2 KB) live in the walk's stack frame as RAW storage: the
 * array is written only by _engage(), i.e. only by a walk that discovers more than
 * kFilteredVisitedLinearLimit vertices, so a short walk stores nothing and zeroes
 * nothing (the same reason InlineBuffer's storage is raw). Below that bound `nodes`
 * IS the visited set — the contiguous scan of the walk's discovery array, which is
 * exactly the code path every sub-64 row ran before this table existed, so the change
 * can only be paid by (and is only measured on) the walks that used to cross into the
 * XXH3 hash set. Past it: an open-addressing table with linear probing over a
 * power-of-two capacity, kept at or below a 3/4 load factor, growing 4x into the heap
 * vector (capacity retained for the rest of the walk). 512 slots carry 384 discovered
 * vertices, 2 048 carry 1 536, 8 192 carry 6 144, so the widest rows of the benchmark
 * rehash once or twice: the hub's walks visit 1 001 vertices (fits 2 048) and
 * S4-wide's visit 2 907 (one more growth to 8 192). Growth is the documented fallback,
 * and it is CHEAPER than the set it replaces, as measured: the [mem] rows fell on every
 * walk that crosses the bound (S3 21 -> 14 allocs/op, the hub 63 -> 47, S4-wide
 * 31 -> 19, S2/F3 14 -> 9), because at most two slots arrays cost less than the ankerl
 * set's bucket array plus value-array growths.
 *
 * The index, not a guid, is what makes this cheap and self-consistent: `nodes` is the
 * walk's discovery order (index == discovery rank), it is stable for the whole walk
 * (InlineBuffer may move its storage, the INDICES survive), and every membership
 * question the walk asks is about a vertex that is either in that array or not in the
 * graph at all. So the table needs no key storage of its own — 4 B per slot, no guid
 * copy, no rehash of keys, and it works unchanged for both walk modes (PathWalkNode
 * and ReachabilityWalkNode both begin with `vertex`).
 */
class WalkVisited {
public:
    static constexpr size_t kInlineSlots = 512; // 2 KB of the walk's stack frame
    static constexpr uint32_t kEmptySlot = 0xFFFFFFFFu;
      // Above this many discovered vertices the contiguous scan stops paying and the
      // table is built. It is DELIBERATELY the bound the previous VisitedSet was given
      // (kFilteredVisitedLinearLimit), not a new fitted knee: the switch point stays
      // where it was, so the rows that never crossed it are byte-for-byte unchanged
      // and the rows that move can only have moved because of the table itself.
      //
      // MEASURED, because the alternative (engaging at 16, i.e. keeping the scan only
      // for the genuinely tiny walks) was tried rather than assumed: two interleaved
      // runs per arm (ref -> engage-at-16) gave S2/F3 9 112.0/9 313.7 -> 8 622.3/8 539.3,
      // S7_in 6 962.0/6 914.3 -> 6 193.0/6 195.8, S5_hastrue/F1 1 165.9/1 147.5 ->
      // 1 085.6/1 101.9, while S2/F1, S4/F5 and S7_out were flat. Two reasons not to
      // take it: ~5 % is inside this host's cross-process band (the same-binary control
      // row S6 moved 2.3 % between the two arms), and `[ctr] shards` is a per-process
      // random variable (plan 3.7, 27-31 observed), so a single-process pair cannot
      // attribute a 5 % shift on a few-hundred-candidate row. Keeping 64 leaves every
      // sub-64 walk on exactly the code path E2 shipped, which is what makes the deep
      // rows' 1.2-1.5x attributable to the table; the earlier-engage variant is
      // recorded as neutral-to-+5 % and not shipped.
      static constexpr size_t kLinearLimit = kFilteredVisitedLinearLimit;


    WalkVisited() noexcept {} // raw slot storage: nothing to zero, nothing to allocate
    WalkVisited(WalkVisited const &) = delete;
    WalkVisited &operator=(WalkVisited const &) = delete;
    WalkVisited(WalkVisited &&) = delete;
    WalkVisited &operator=(WalkVisited &&) = delete;
    ~WalkVisited() noexcept = default;

    // Was `guid` discovered? `nodes` is the walk's discovery array (discovery order).
    // A missing start vertex is not in it, so an absent `from` still discovers nothing.
    template <typename Nodes>
    [[nodiscard]] bool contains(Nodes const &nodes, kimix::Guid const &guid) const noexcept {
        if (!_live) {
            // The linear phase: the discovery array itself is the set, scanned from
            // the start (the walk asks about its whole reachable set, in any order).
            for (size_t i = 0; i < nodes.size(); ++i) {
                if (nodes[i].vertex == guid) {
                    return true;
                }
            }
            return false;
        }
        size_t const mask = _capacity - 1;
        for (size_t pos = _home(guid);; pos = (pos + 1) & mask) {
            uint32_t const slot = _slots[pos];
            if (slot == kEmptySlot) {
                return false;
            }
            if (nodes[slot].vertex == guid) {
                return true;
            }
        }
    }

    // Record `nodes[index]` as discovered (discovery index, NOT a guid). The caller
    // has already pushed it onto `nodes`, so the linear scan sees it immediately.
    template <typename Nodes>
    void insert(Nodes const &nodes, size_t index) {
        if (!_live) {
            if (nodes.size() <= kLinearLimit) {
                return; // still inside the scan
            }
            _engage(nodes); // builds the table from nodes[0 .. size)
            return;
        }
        if ((_count + 1) * 4 > _capacity * 3) {
            _grow(nodes);
        }
        _place(nodes, index);
    }

private:
    // Inside a shard: `_home` is index 0 of the probe sequence.
    [[nodiscard]] size_t _home(kimix::Guid const &guid) const noexcept {
        // Fibonacci hashing on (data0 ^ data1), the same shape Graph::_shard_of uses
        // and for the same reason: the multiply spreads even the constant bits a v4
        // guid carries (version/variant in data1), and the TOP bits of the product
        // depend on every input bit — which is what a power-of-two mask needs.
        // Correctness never depends on the spread: a collision is resolved by the
        // full guid compare in contains() (this is an exact set, not a filter).
        auto const &words = guid.to_binary();
        uint64_t const mixed = (words.data0 ^ words.data1) * 0x9E3779B97F4A7C15ull;
        return static_cast<size_t>(mixed >> _shift);
    }

    // Empty a (possibly new) slot array and adopt it. `capacity` is a power of two.
    void _reset(size_t capacity) {
        if (capacity > kInlineSlots) {
            _heap.resize(capacity);
            _slots = _heap.data();
        } else {
            _slots = _slots_inline;
        }
        _capacity = capacity;
        _shift = 64;
        for (size_t n = capacity; n > 1; n >>= 1) {
            --_shift;
        }
        std::fill_n(_slots, capacity, kEmptySlot);
        _count = 0;
    }

    template <typename Nodes>
    void _engage(Nodes const &nodes) {
        _live = true;
        size_t capacity = kInlineSlots;
        while (capacity * 3 / 4 < nodes.size()) {
            capacity *= 2;
        }
        _reset(capacity);
        for (size_t i = 0; i < nodes.size(); ++i) {
            _place(nodes, i);
        }
    }

    // Rehash into 4x the slots. Re-inserting from `nodes` (all of them, in discovery
    // order) instead of from the old slots is the cheap form: it needs no key
    // storage, and the amortised cost is one extra pass over the discovered set per
    // growth, of which a walk has at most two.
    template <typename Nodes>
    void _grow(Nodes const &nodes) {
        size_t const placed = _count;
        _reset(_capacity * 4);
        for (size_t i = 0; i < placed; ++i) {
            _place(nodes, i);
        }
    }

    template <typename Nodes>
    void _place(Nodes const &nodes, size_t index) noexcept {
        size_t const mask = _capacity - 1;
        size_t pos = _home(nodes[index].vertex);
        while (_slots[pos] != kEmptySlot) {
            pos = (pos + 1) & mask;
        }
        _slots[pos] = static_cast<uint32_t>(index);
        ++_count;
    }

    // Declared FIRST so the `_slots` initialiser below can point at it. Raw storage,
    // like InlineBuffer's: never zeroed until the table is engaged. (The natural name
    // `_inline` is a legacy keyword alias in MSVC's CRT headers, which the platforms
    // this library ships on still define — hence `_slots_inline`, the same spelling
    // EdgeFilterScratch uses for its own inline array.)
    uint32_t _slots_inline[kInlineSlots];
    uint32_t *_slots{_slots_inline};
    size_t _capacity{kInlineSlots};
    unsigned _shift{64 - 9}; // 64 - log2(capacity); 512 slots == 9 bits
    size_t _count{0};
    bool _live{false};
    kimix::vector<uint32_t> _heap; // only once the walk grows past kInlineSlots
};

// The ONE oriented-link copy: `vertex`'s out list, its in list, or both
// concatenated outgoing-first (the order oriented_links() produces), into a
// caller-sized buffer. Returns the TOTAL link count, i.e. the snprintf convention
// the span form of dependencies() already uses here. Both halves are read from one
// instant of the vertex — that is what a `both` snapshot means, and it is why the
// copy cannot be split across two locks.
//
// The destination is written with PLACEMENT NEW rather than assignment, so a caller
// may hand it raw storage: LinkBuffer's inline array is then never zero-initialised
// (512 B of per-call stores for nothing), and EdgeLink is trivially copyable, so the
// placement new compiles to a plain store.
inline size_t copy_oriented_links(Vertex const &vertex, EdgeOrientation orientation,
                                  EdgeLink *out, size_t capacity) noexcept {
    size_t total = 0;
    auto const take = [&](kimix::vector<EdgeLink> const &links) noexcept {
        for (size_t i = 0; i < links.size(); ++i) {
            if (total < capacity) {
                ::new (static_cast<void *>(out + total)) EdgeLink(links[i]);
            }
            ++total;
        }
    };
    if (orientation != EdgeOrientation::incoming) {
        take(vertex.out_links());
    }
    if (orientation != EdgeOrientation::outgoing) {
        take(vertex.in_links());
    }
    return total;
}

// How many links the oriented snapshot would copy. Two O(1) size reads — used to
// pick the inline or the heap path BEFORE copying, so that a wide vertex costs one
// pass over its link list and not a probe pass plus a real one.
inline size_t oriented_link_count(Vertex const &vertex, EdgeOrientation orientation) noexcept {
    size_t total = 0;
    if (orientation != EdgeOrientation::incoming) {
        total += vertex.out_links().size();
    }
    if (orientation != EdgeOrientation::outgoing) {
        total += vertex.in_links().size();
    }
    return total;
}

// Append `vertex`'s oriented link list to an owning container (the heap fallback of
// LinkBuffer): same order and same one-instant snapshot as copy_oriented_links, but
// written through the container's own append so nothing is value-initialised first.
template <typename Container>
inline void append_oriented_links(Vertex const &vertex, EdgeOrientation orientation,
                                  Container &out) {
    if (orientation != EdgeOrientation::incoming) {
        out.insert(out.end(), vertex.out_links().begin(), vertex.out_links().end());
    }
    if (orientation != EdgeOrientation::outgoing) {
        out.insert(out.end(), vertex.in_links().begin(), vertex.in_links().end());
    }
}

} // namespace src_graph_db_graph
using namespace src_graph_db_graph;

/*
 * The filtered walk's link snapshot (Graph::LinkBuffer, declared in graph.h) —
 * AdjacencyBuffer's shape applied to EdgeLink: kInlineCapacity links inline, a heap
 * vector beyond, so a normal-degree visit allocates nothing and a hub pays ONE
 * allocation per call (the capacity is retained for the rest of the walk). Filled
 * by Graph::_snapshot_links() while the vertex's own shard SHARED lock is held, and
 * growing inside that lock on purpose: the alternative is a second lock to re-read
 * a degree the first lock already saw.
 */
class Graph::LinkBuffer {
public:
    static constexpr size_t kInlineCapacity = 16;
    LinkBuffer() noexcept = default;
    LinkBuffer(LinkBuffer const &) = delete;
    LinkBuffer &operator=(LinkBuffer const &) = delete;
    LinkBuffer(LinkBuffer &&) = delete;
    LinkBuffer &operator=(LinkBuffer &&) = delete;
    ~LinkBuffer() noexcept = default;

    [[nodiscard]] size_t size() const noexcept { return _size; }
    [[nodiscard]] bool empty() const noexcept { return _size == 0; }
    [[nodiscard]] EdgeLink const *data() const noexcept { return _data; }
    [[nodiscard]] EdgeLink const *begin() const noexcept { return _data; }
    [[nodiscard]] EdgeLink const *end() const noexcept { return _data + _size; }
    [[nodiscard]] EdgeLink const &operator[](size_t index) const noexcept { return _data[index]; }

private:
    friend class Graph;
    void _clear() noexcept {
        _size = 0;
        _data = _local_ptr();
    }
    // Called by Graph under the owning shard's SHARED lock, so the link lists
    // cannot change while they are copied. Never allocates at degree <= 16, and the
    // inline path is ONE pass over the lists (the count the copy returns IS the
    // size); only a vertex wider than the inline array is counted first, so that a
    // hub pays a single copy into a correctly sized heap array.
    void _assign(Vertex const &vertex, EdgeOrientation orientation) {
        EdgeLink *const local = _local_ptr();
        size_t const total = oriented_link_count(vertex, orientation);
        if (total <= kInlineCapacity) {
            copy_oriented_links(vertex, orientation, local, kInlineCapacity);
            _size = total;
            _data = local;
            return;
        }
        // The wide path APPENDS instead of resize()-ing: resize value-initialises the
        // new elements first (two Guid stores each) and the copy then overwrites them,
        // i.e. 160 KB of needless zeroing per 5 000-link hub visit. What that is worth is
        // the A/B that motivated the change — two adjacent builds of one session whose
        // only difference was this line: `edges/S1_1hop_label100/F4` 7 144.7 ns ->
        // 4 370.7 ns, i.e. 2.77 us on a 2.7-2.8 us row. The 160 KB is the mechanism, the
        // 2.77 us is the measurement. Capacity is retained, so only the first wide visit
        // allocates.
        _heap.clear();
        _heap.reserve(total);
        append_oriented_links(vertex, orientation, _heap);
        _size = _heap.size();
        _data = _heap.data();
    }

    // Raw storage, like InlineBuffer's: the elements are created by
    // copy_oriented_links' placement new, so a normal-degree visit zeroes nothing.
    [[nodiscard]] EdgeLink *_local_ptr() noexcept {
        return reinterpret_cast<EdgeLink *>(_storage);
    }
    [[nodiscard]] EdgeLink const *_local_ptr() const noexcept {
        return reinterpret_cast<EdgeLink const *>(_storage);
    }

    // Declared FIRST so the `_data` initialiser below runs after it exists.
    alignas(EdgeLink) unsigned char _storage[kInlineCapacity * sizeof(EdgeLink)];
    EdgeLink const *_data{_local_ptr()};
    size_t _size{0};
    kimix::vector<EdgeLink> _heap; // only for a vertex exceeding kInlineCapacity
};

bool Graph::edge_matches(EdgeId const &edge, Constraint const &constraint) const {
#if defined(KIMIX_GRAPH_DB_WALK_COUNTERS)
    {
        // BOTH paths below take exactly one edge-shard SHARED lock, so counting
        // here (before the empty-constraint shortcut) keeps the counter equal to
        // the lock count rather than to the filter count.
        WalkCounters &counters = walk_counters();
        ++counters.edge_locks;
        counters.edge_shards |= (uint64_t{1} << EdgeTable::shard_of(edge));
    }
#endif
    if (constraint.conditions.empty()) {
        // An empty constraint matches everything — including "does this edge
        // still exist", which is the only thing left to answer.
        return _edges.contains(edge);
    }
    size_t const shard = EdgeTable::shard_of(edge);
    std::shared_lock<std::shared_mutex> lock(_edges.shard_mutex(shard));
    Edge *record = _edges._find_locked(shard, edge);
    if (record == nullptr) {
        return false;
    }
    return _record_matches(*record, constraint);
}

bool Graph::_record_matches(Edge const &record, Constraint const &constraint) {
    // §2.6: an EDGE filter reads the edge's own labels and properties, through the
    // same zero-copy probe the filtered vertex paths use (no payload copy: this
    // runs once per candidate link of a filtered walk). The caller holds the
    // record's shard SHARED lock, which is what keeps the borrowed payload valid
    // for the whole probe — the ONE evaluation of the rule, shared by edge_matches()
    // (single edge) and _filter_edges_batched() (a whole visit's links), so the two
    // cannot drift.
    auto probe = [&record](PropNameId name) -> PropertyValue const * {
        auto it = record._properties.find(name);
        return it == record._properties.end() ? nullptr : &it->second;
    };
    return matches_probe(
        constraint,
        kimix::span<const LabelId>{record._labels.data(), record._labels.size()}, probe);
}

void Graph::_filter_edges_batched(kimix::span<const EdgeId> edges, Constraint const &constraint,
                                  kimix::span<uint8_t> match_out, EdgeFilterScratch &scratch) const {
    size_t const total = edges.size();
    // Bounded scratch: a hub visit hands over thousands of links and is drained in
    // kEdgeFilterChunk-sized chunks, so the staging arrays never grow past "order
    // 4 B + match 1 B per candidate" (~5 KB). The chunk is invisible to the caller:
    // the verdicts come back indexed by the ORIGINAL candidate index either way.
    for (size_t base = 0; base < total; base += kEdgeFilterChunk) {
        size_t const count = total - base < kEdgeFilterChunk ? total - base : kEdgeFilterChunk;
        kimix::span<const EdgeId> const chunk = edges.subspan(base, count);
        kimix::span<uint8_t> const matches = match_out.subspan(base, count);
        uint32_t *const order = scratch._order_for(count);

        // 1. ONE pass over the drain: count the candidates per edge shard AND record
        // the TOUCHED-shard list (first appearance; sorted below). Scanning all 64
        // shards instead -- the naive shape -- costs more than the locks it saves
        // below ~32 candidates, so the touched list keeps this O(candidates) instead
        // of O(candidates + 64). What the batch still cannot beat, MEASURED: an
        // edge-shard lock is only ~5-7 ns of edge_matches()'s 20.4 ns, so a drain of
        // `n` candidates over `s` touched shards saves (n - s) * ~6 ns and pays a few
        // ns per candidate to stage it -- which is why the caller only hands over a
        // visit wide enough for (n - s) to be large (kEdgeFilterMinLinks).
        for (size_t i = 0; i < count; ++i) {
            size_t const shard = EdgeTable::shard_of(chunk[i]);
            if (scratch._count[shard] == 0) {
                scratch._touched[scratch._touched_count++] = static_cast<uint32_t>(shard);
            }
            ++scratch._count[shard];
        }

        // 2. Ascending shard index — the contract's lock order — and the bucket
        //    offsets prefixed over the TOUCHED list ONLY. Insertion sort: the list
        //    holds at most kEdgeShardCount entries and is usually 1..30.
        for (uint32_t i = 1; i < scratch._touched_count; ++i) {
            uint32_t const shard = scratch._touched[i];
            uint32_t slot = i;
            for (; slot > 0 && scratch._touched[slot - 1] > shard; --slot) {
                scratch._touched[slot] = scratch._touched[slot - 1];
            }
            scratch._touched[slot] = shard;
        }
        uint32_t next = 0;
        for (uint32_t i = 0; i < scratch._touched_count; ++i) {
            uint32_t const shard = scratch._touched[i];
            scratch._begin[shard] = next;
            scratch._cursor[shard] = next;
            next += scratch._count[shard];
        }

        // 3. Scatter the candidate INDICES into their shard's bucket, so the
        //    per-shard loop below walks exactly the candidates that shard owns. The
        //    original index is what gets stored: the caller consumes the batch in
        //    original link order, which is what keeps discovery order — and every
        //    determinism gate — bit-identical to the per-visit walk.
        for (size_t i = 0; i < count; ++i) {
            size_t const shard = EdgeTable::shard_of(chunk[i]);
            order[scratch._cursor[shard]++] = static_cast<uint32_t>(i);
        }

        // 4. LEVEL 3: one SHARED lock per TOUCHED shard, ascending, and the same
        //    _find_locked + _record_matches pair the single-edge probe runs. No
        //    vertex-shard lock is held here — the walk released every one of them
        //    before it called this — and nothing here may take one.
        for (uint32_t i = 0; i < scratch._touched_count; ++i) {
            uint32_t const shard = scratch._touched[i];
            std::shared_lock<std::shared_mutex> lock(_edges.shard_mutex(shard));
#if defined(KIMIX_GRAPH_DB_WALK_COUNTERS)
            // ONE lock, not one per candidate: this is the counter that proves the
            // batch (edge_locks <= touched shards per drain <= 64).
            ++walk_counters().edge_locks;
            walk_counters().edge_shards |= (uint64_t{1} << shard);
#endif
            uint32_t const end = scratch._begin[shard] + scratch._count[shard];
            for (uint32_t slot = scratch._begin[shard]; slot < end; ++slot) {
                size_t const index = order[slot];
                Edge *record = _edges._find_locked(shard, chunk[index]);
                matches[index] = record != nullptr && _record_matches(*record, constraint)
                                     ? uint8_t{1}
                                     : uint8_t{0};
            }
        }

        // 5. Reset only what this drain touched, so the next one starts clean
        //    without a 64-entry clear per batch.
        for (uint32_t i = 0; i < scratch._touched_count; ++i) {
            scratch._count[scratch._touched[i]] = 0;
        }
        scratch._touched_count = 0;
    }
}

bool Graph::_snapshot_links(kimix::Guid const &guid, EdgeOrientation orientation,
                            LinkBuffer &out) const {
    Shard const &shard = _shards[_shard_of(guid)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
#if defined(KIMIX_GRAPH_DB_WALK_COUNTERS)
    // One level-1 shard lock per visited vertex, taken or not — the same count
    // oriented_links() reports, so the [ctr] rows stay comparable across the two
    // walks.
    ++walk_counters().vertex_locks;
#endif
    Vertex *vertex = shard._find(guid);
    if (vertex == nullptr) {
        out._clear();
        return false;
    }
    // ONE lock, ONE snapshot: `both` copies the two halves together, so a link is
    // never paired with a neighbour from a different instant of the vertex (the
    // oriented_links() contract, into a buffer that keeps 16 links inline).
    out._assign(*vertex, orientation);
#if defined(KIMIX_GRAPH_DB_WALK_COUNTERS)
    walk_counters().links_snapshotted += out.size();
#endif
    return true;
}

bool Graph::oriented_links(kimix::Guid const &guid, EdgeOrientation orientation,
                           kimix::vector<EdgeLink> &out) const {
    out.clear();
    Shard const &shard = _shards[_shard_of(guid)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
#if defined(KIMIX_GRAPH_DB_WALK_COUNTERS)
    // One level-1 shard lock per visited vertex, taken or not.
    ++walk_counters().vertex_locks;
#endif
    Vertex *vertex = shard._find(guid);
    if (vertex == nullptr) {
        return false;
    }
    // ONE lock, ONE snapshot: the `both` orientation copies the two halves
    // together (like _read_oriented_adjacency), so a link is never paired with a
    // neighbour from a different instant of the vertex.
    switch (orientation) {
    case EdgeOrientation::outgoing:
        out.insert(out.end(), vertex->_out_links.begin(), vertex->_out_links.end());
        break;
    case EdgeOrientation::incoming:
        out.insert(out.end(), vertex->_in_links.begin(), vertex->_in_links.end());
        break;
    case EdgeOrientation::both:
        out.insert(out.end(), vertex->_out_links.begin(), vertex->_out_links.end());
        out.insert(out.end(), vertex->_in_links.begin(), vertex->_in_links.end());
        break;
    }
#if defined(KIMIX_GRAPH_DB_WALK_COUNTERS)
    walk_counters().links_snapshotted += out.size();
#endif
    return true;
}

/*
 * The ONE edge-filtered walk (declared in graph.h). Shortest_path's filtered forms ask
 * it for a PathStep sequence, the filtered has_path asks for a bool, and the mode is the
 * Node template argument, so the two products cannot drift: same depth bound, same
 * visited-check-then-filter ordering (O1), same discovery order, same batched filter
 * (O2), same refusal rules (a refused edge is not a hop, an unknown endpoint is
 * unreachable, from == to is the zero-length answer). What the reachability mode does
 * NOT pay for is everything that exists only to rebuild a path: the per-node
 * edge/parent/depth payload (48 B -> 16 B per discovered vertex), the predecessor
 * walk-back and the caller's buffer fill (plan O6).
 */
template <typename Node>
auto Graph::_filtered_walk(kimix::Guid const &from, kimix::Guid const &to,
                           Constraint const &edge_constraint, EdgeOrientation orientation,
                           uint32_t max_depth, kimix::vector<PathStep> *out) const
    -> FilteredWalkResult {
    // `out` is the path mode's buffer and is never touched by the reachability mode,
    // which has nothing to fill (and passes nullptr).
    if constexpr (Node::builds_path) {
        out->clear();
    } else {
        (void)out;
    }
    if (max_depth < 1 || max_depth > kMaxNativeHopDepth) {
        return FilteredWalkResult{GdiStatus::error_argument, false};
    }
    // The _resolve_constraint rule, validated here so the container API refuses a
    // label ordering op exactly like traverse_bfs and the Database facade do.
    for (auto const &condition : edge_constraint.conditions) {
        if (condition.is_label() && condition.op != GdiOp::equal &&
            condition.op != GdiOp::not_equal) {
            return FilteredWalkResult{GdiStatus::error_op, false};
        }
    }
    if (!contains(from) || !contains(to)) {
        return FilteredWalkResult{GdiStatus::error_vertex, false};
    }
    if (from == to) {
        // Zero-length path: it traverses no edge, so no edge constraint can
        // exclude it (the same rule the vertex-only bounded form follows).
        if constexpr (Node::builds_path) {
            out->push_back(PathStep{from, EdgeId{}, 0});
        }
        return FilteredWalkResult{GdiStatus::ok, true};
    }
    bool const filtered = !edge_constraint.conditions.empty();
    // --- the walk's per-call state: plain stack locals, heap ONLY on growth -----
    // (plan O5) `nodes`, the two level frontiers and the link snapshot are
    // inline-array-backed (the budgets and the whole stack arithmetic are documented
    // above with kInlineWalkNodes), so the short filtered walk allocates nothing at
    // all: no node array, no frontier, no link vector, no hash table. Growth past a
    // budget spills that one container to the heap — the hub fixtures and the
    // 32-level bound do that, and that is the point of the fallback.
    // `Node` is the mode's record (PathWalkNode 48 B / ReachabilityWalkNode 16 B), so
    // this array is the only thing the reachability mode sizes differently from the
    // path mode — same inline count, a third of the bytes, and therefore a third of
    // what growth allocates on a wide unreachable probe.
    InlineBuffer<Node, kInlineWalkNodes> nodes; // discovery order == BFS order
    nodes.push_back(Node{from, EdgeId{}, 0, 0});
    // The has_path discipline: `nodes` (every guid ever enqueued, in order, as the
    // record's `vertex` field — the only field the reachability mode has) doubles as
    // the small-BFS visited set, so a 2-hop search pays for no hash table; past
    // kLinearVisitedLimit the set primes itself from this array. This is why there
    // is no second `discovery` vector of guids to keep consistent.
    //
    // Past that bound the set is WalkVisited (plan O13), the walk-local EXACT table
    // above: 4 B slots holding a `nodes` index, keyed by an inline mix of the guid's
    // two words, colliding candidates resolved by the full guid compare. It replaces
    // the ankerl set the shared VisitedSet would switch to here, whose key hash is the
    // exported XXH3 `kimix::hash64` — a cross-DLL call plus a bucket probe for every
    // candidate, which the E2 record left as the dominant term of the deep rows.
    // Same switch point, same exactness, both walk modes: nothing here is probabilistic.
    WalkVisited visited;
    InlineBuffer<size_t, kInlineWalkFrontier> frontier_a; // node indices at this depth
    InlineBuffer<size_t, kInlineWalkFrontier> frontier_b;
    InlineBuffer<size_t, kInlineWalkFrontier> *frontier = &frontier_a;
    InlineBuffer<size_t, kInlineWalkFrontier> *next_frontier = &frontier_b;
    frontier->push_back(0);
    LinkBuffer links; // one visit's snapshot: allocates on degree growth only
    // The batched filter's staging (plan O2), used ONLY by a visit wide enough for
    // the batch to pay (kEdgeFilterMinLinks): a normal-degree visit never touches
    // these, so they stay unallocated on the short rows. One entry per candidate of
    // the visit, in ORIGINAL link order — the verdict array is indexed the same way,
    // because the walk filters them out of order (per touched edge shard) but
    // consumes them in this order.
    kimix::vector<EdgeId> batch_edges;
    kimix::vector<kimix::Guid> batch_neighbours;
    kimix::vector<uint8_t> batch_matches;
    // The level-3 scratch is ENGAGED on the first wide visit and never before: its
    // shard tables have to start zeroed (that is how a drain detects a touched
    // shard), so constructing it eagerly would cost a normal-degree walk ~1.3 KB of
    // stack zeroing — measured at ~15 ns on the 1-hop row, i.e. 13 % of it.
    kimix::optional<EdgeFilterScratch> filter_scratch;
    size_t hit = kNoNode;
    uint32_t depth = 0;
    // ONE place a hop is recorded: push the node, insert it into the visited set
    // (AFTER the push — the linear scan and the table's engage both read the
    // container the caller has just grown, and the index recorded is the node's own),
    // then either stop on the target or extend the next frontier. Returns true when
    // `to` was discovered, i.e. the walk is over.
    auto const discover = [&](size_t origin_index, kimix::Guid const &neighbour,
                              EdgeId const &edge, uint32_t level) {
        nodes.push_back(Node{neighbour, edge, origin_index, level});
        visited.insert(nodes, nodes.size() - 1);
        if (neighbour == to) {
            hit = nodes.size() - 1;
            return true;
        }
        next_frontier->push_back(nodes.size() - 1);
        return false;
    };
    // ONE link, ONE verdict: the per-link rule the walk is defined by. Visited check
    // first (O1), then the edge filter, then the hop. Returns true when `to` was
    // discovered, i.e. the walk is over. The wide visit's head calls this; the narrow
    // path below spells out the same three steps instead of calling it, for a measured
    // inlining reason (see its comment), so the two must be kept in step.
    auto const visit_one = [&](size_t origin_index, EdgeLink const &link) {
#if defined(KIMIX_GRAPH_DB_WALK_COUNTERS)
        // The "candidates" of the cost model: every link the walk actually looked at,
        // i.e. every visited-set probe.
        ++walk_counters().visited_checks;
#endif
        if (visited.contains(nodes, link.neighbour)) {
            // A discovered neighbour is not a hop whatever its edge says: discovery is
            // the only observable effect of a matching edge (a BFS discovers a vertex
            // once and never re-emits it), so the cheap membership test gates the edge
            // filter instead of the other way round. A NEW neighbour still takes the
            // filter in link order, so the first matching parallel edge wins.
            return false;
        }
        if (filtered) {
#if defined(KIMIX_GRAPH_DB_WALK_COUNTERS)
            ++walk_counters().edge_tests;
#endif
            if (!edge_matches(link.edge, edge_constraint)) {
                return false; // a refused EDGE is not a hop
            }
        }
        return discover(origin_index, link.neighbour, link.edge, depth);
    };
    while (!frontier->empty() && hit == kNoNode && depth < max_depth) {
        ++depth;
        next_frontier->clear();
        for (size_t pos = 0; pos < frontier->size() && hit == kNoNode; ++pos) {
            size_t const index = (*frontier)[pos];
            if (!_snapshot_links(nodes[index].vertex, orientation, links)) {
                continue; // removed between the enqueue and the visit
            }
            if (!filtered || links.size() < kEdgeFilterMinLinks) {
                // The per-visit path: ONE edge_matches() call per surviving link, and
                // the whole unfiltered form. This is what a normal-degree visit pays,
                // and it is why the batch below is not on the common path.
                //
                // The body is `visit_one` SPELLED OUT rather than called, on purpose:
                // this loop is the walk's hot path (every visit of every non-hub row).
                // The evidence for the duplication is WEAK, and recorded as such: a
                // single-window A/B two adjacent runs apart put the shared-lambda version
                // at 63.7 us against 57.6 us here, which does not meet §3.7's bar (that
                // session's own S3 range was 51.6-69.0 us, a 34 % band), so the cost of
                // sharing is UNPROVEN, not measured at 15 %. The duplication is kept as a
                // cheap precaution — the head below is at most kEdgeFilterHeadLinks
                // iterations wide, so calling the lambda there costs nothing — and the two
                // bodies are bound to be kept in step: a one-sided edit would be invisible
                // to every gate, the [ctr] rows included.
                for (auto const &link : links) {
#if defined(KIMIX_GRAPH_DB_WALK_COUNTERS)
                    ++walk_counters().visited_checks;
#endif
                    if (visited.contains(nodes, link.neighbour)) {
                        continue;
                    }
                    if (filtered) {
#if defined(KIMIX_GRAPH_DB_WALK_COUNTERS)
                        ++walk_counters().edge_tests;
#endif
                        if (!edge_matches(link.edge, edge_constraint)) {
                            continue; // a refused EDGE is not a hop
                        }
                    }
                    if (discover(index, link.neighbour, link.edge, depth)) {
                        break;
                    }
                }
                continue;
            }
            // --- the WIDE visit: HEAD per link, then the TAIL in drained chunks ------
            // The head exists because of the early exit: a wide visit usually stops at
            // the first matching link that reaches `to`, and a drain that stages the
            // whole list before consuming anything cannot prune itself — measured on
            // `edges/S1_1hop_label100/F4`, whose 1-hop lookup finds its target at link
            // index 7 of a 5 000-link hub: staging everything probed 5 000 edges and
            // took 320 edge locks where 8 probes and 8 locks answer it, i.e. 96 888 ns
            // instead of 2 958. So the first kEdgeFilterHeadLinks links take the
            // per-link path (the same `visit_one` rule) and the REST is drained
            // in kEdgeFilterChunk-sized chunks, each chunk consumed in original order
            // before the next is staged.
            //
            // Lock discipline (graph.h's LOCK ORDER 4): `_snapshot_links` released this
            // vertex's shard lock before returning, so every `_filter_edges_batched`
            // call below takes level-3 locks with no level-1 lock held, one per TOUCHED
            // shard, ascending. Discovery is deferred only WITHIN a chunk, which is why
            // the verdict array is indexed by the ORIGINAL candidate index and why the
            // visited check moves into the consume loop: a refused edge is not a hop,
            // and the first matching parallel edge still wins because the first
            // candidate for a neighbour is the first one to pass both tests.
            size_t const head = links.size() < kEdgeFilterHeadLinks ? links.size()
                                                                   : kEdgeFilterHeadLinks;
            for (size_t i = 0; i < head && hit == kNoNode; ++i) {
                (void)visit_one(index, links[i]);
            }
            // The tail ESCALATES 64 -> 256 -> 1024 (kEdgeFilterChunk) and stays there: a
            // hit inside a drain is only noticed when that drain is consumed, so the size
            // of the CURRENT drain is the real bound on what an early hit wastes — small
            // right after the head, saturating at 1 024 once the visit has shown it is not
            // going to hit early.
            size_t base = head;
            size_t step = kEdgeFilterHeadLinks;
            while (base < links.size() && hit == kNoNode) {
                size_t const tail = links.size() - base;
                size_t const count = tail < step ? tail : step;
                batch_edges.clear();
                batch_neighbours.clear();
                // Sized for the STEADY-STATE chunk up front, not for the escalating first
                // one: three growth steps (64, 256, 1 024) would be three allocations per
                // buffer per walk, where one reserve of the final size is one.
                size_t const room = links.size() - head < kEdgeFilterChunk
                                        ? links.size() - head
                                        : kEdgeFilterChunk;
                if (batch_edges.capacity() < room) {
                    batch_edges.reserve(room);
                    batch_neighbours.reserve(room);
                }
                if (batch_matches.capacity() < room) {
                    batch_matches.reserve(room);
                }
                for (size_t i = base; i < base + count; ++i) {
                    batch_edges.push_back(links[i].edge);
                    batch_neighbours.push_back(links[i].neighbour);
                }
                batch_matches.resize(count);
                if (!filter_scratch.has_value()) {
                    filter_scratch.emplace();
                }
                _filter_edges_batched(
                    kimix::span<const EdgeId>{batch_edges.data(), count}, edge_constraint,
                    kimix::span<uint8_t>{batch_matches.data(), count}, *filter_scratch);
#if defined(KIMIX_GRAPH_DB_WALK_COUNTERS)
                // One probe per iterated link — the model's candidate denominator stays
                // comparable with the per-visit path — and one filter evaluation per
                // staged candidate, which is what the batch actually cost.
                walk_counters().visited_checks += count;
                walk_counters().edge_tests += count;
#endif
                for (size_t i = 0; i < count; ++i) {
                    if (batch_matches[i] == 0) {
                        continue; // a refused EDGE is not a hop
                    }
                    if (visited.contains(nodes, batch_neighbours[i])) {
                        continue; // discovered by an earlier link of this same chunk
                    }
                    if (discover(index, batch_neighbours[i], batch_edges[i], depth)) {
                        break;
                    }
                }
                base += count;
                step = step * 4 < kEdgeFilterChunk ? step * 4 : kEdgeFilterChunk;
            }
        }
        // Pointer swap, not a buffer swap: both frontiers live here, and the buffers
        // are not movable (they point at their own inline storage).
        std::swap(frontier, next_frontier);
    }
    if (hit == kNoNode) {
        // Unreachable within the bound over MATCHING edges — the whole answer of the
        // reachability mode, and an empty `out` for the path forms.
        return FilteredWalkResult{GdiStatus::ok, false};
    }
    if constexpr (Node::builds_path) {
        // Fill the caller's buffer in place: `out` is documented as reusable and its
        // capacity survives between calls (graph.h, the bounded path forms), so writing
        // the steps at their final index (the parent chain walked backwards) costs
        // neither a second vector, nor the move that discarded that capacity, nor the
        // reverse pass. `depth` IS the step index, so the reverse-index fill needs no
        // separate path length. A reachability walk has none of this: no parent index,
        // no depth, no edge to report, and no buffer — which is exactly the payload the
        // 16 B record drops (plan O6).
        size_t const length = static_cast<size_t>(nodes[hit].depth) + 1;
        out->resize(length);
        for (size_t cursor = hit, step = length; step-- > 0;
             cursor = nodes[cursor].parent) {
            (*out)[step] =
                PathStep{nodes[cursor].vertex, nodes[cursor].edge, nodes[cursor].depth};
        }
    }
    return FilteredWalkResult{GdiStatus::ok, true};
}

GdiStatus Graph::shortest_path(kimix::Guid const &from, kimix::Guid const &to,
                               Constraint const &edge_constraint, EdgeOrientation orientation,
                               uint32_t max_depth, kimix::vector<PathStep> &out) const {
    // The PATH mode of the shared walk above. The status is the whole product for this
    // form: an empty `out` with ok means "unreachable within max_depth over MATCHING
    // edges", and {from} means from == to on an existing vertex.
    return _filtered_walk<PathWalkNode>(from, to, edge_constraint, orientation, max_depth,
                                        &out)
        .status;
}

bool Graph::has_path(kimix::Guid const &from, kimix::Guid const &to,
                     Constraint const &edge_constraint, EdgeOrientation orientation) const {
    // The SAME filtered walk as shortest_path, in its REACHABILITY mode (plan O6): one
    // implementation, so the bool cannot answer differently from the path form (same
    // kMaxNativeHopDepth bound, same zero-length rule, same unknown-endpoint => false
    // rule, same refused-edge rule, same discovery order) — but the walk then keeps no
    // predecessor chain and assembles no PathStep, and its per-node record is the guid
    // alone (16 B instead of 48 B). A false answer, which is the expensive one (it
    // walks the whole reachable set inside the bound), therefore no longer materialises
    // the path-building state it would only throw away. The vertex-only has_path above
    // stays the cheap adjacency walk for the unfiltered case.
    return _filtered_walk<ReachabilityWalkNode>(from, to, edge_constraint, orientation,
                                               kMaxNativeHopDepth, nullptr)
        .reached;
}

/*
 * The EDGE-filtered BFS (declared in graph.h). Its hop rule is the one
 * _filtered_walk runs — visited check first, then the EDGE filter, then the hop — on
 * the SAME primitives (_snapshot_links, edge_matches, _filter_edges_batched, the same
 * kEdgeFilterMinLinks / kEdgeFilterHeadLinks / kEdgeFilterChunk split), so a hop that
 * exists here exists there and a refused edge is not traversed in either. What differs
 * is the PRODUCT: this walk has no target and no early exit for one, it expands the
 * whole readable neighbourhood up to `max_depth` levels and `max_vertices` vertices,
 * and it then sweeps that set for every matching edge inside it, spanning-tree or not.
 *
 * The discovery array IS `out.vertices` — the walk's product is exactly "every
 * discovered vertex with its discovering edge and its depth", so there is no second
 * node array to build and no copy at the end. Two consequences, both intended: the
 * reusable-buffer contract (graph.h) is what makes the C ABI's warm read path
 * allocation-free, and the visited set can index this array directly, because
 * WalkVisited reads the `vertex` field of the element and BfsVertex keeps it first —
 * exactly like the walk's own PathWalkNode / ReachabilityWalkNode records do.
 */
GdiStatus Graph::traverse_bfs_edges(kimix::span<const kimix::Guid> starts,
                                    Constraint const &edge_constraint,
                                    EdgeOrientation orientation, uint32_t max_depth,
                                    uint32_t max_vertices, BfsEdgesResult &out) const {
    out.vertices.clear();
    out.edges.clear();
    out.truncated = false;
    if (max_depth < 1 || max_depth > kMaxNativeHopDepth) {
        return GdiStatus::error_argument;
    }
    if (max_vertices < 1) {
        return GdiStatus::error_argument;
    }
    // The _resolve_constraint rule, validated here exactly where _filtered_walk
    // validates it: a label condition with an ordering op cannot be evaluated, and the
    // container API must refuse it before touching a single shard — the Database facade
    // re-runs the same check, which is idempotent.
    for (auto const &condition : edge_constraint.conditions) {
        if (condition.is_label() && condition.op != GdiOp::equal &&
            condition.op != GdiOp::not_equal) {
            return GdiStatus::error_op;
        }
    }
    // ABI 8: no start is no walk. Checked after the bounds and the constraint rule so
    // the error precedence stays the frozen one (bounds -> constraint -> starts).
    if (starts.empty()) {
        return GdiStatus::error_argument;
    }
    // Validate EVERY start before anything is written, so an unknown one leaves the
    // caller's reusable buffers empty (the single-start contract, unchanged).
    for (auto const &start : starts) {
        if (!contains(start)) {
            return GdiStatus::error_vertex;
        }
    }
    bool const filtered = !edge_constraint.conditions.empty();

    // --- the walk's per-call state: the same shapes _filtered_walk keeps ---------
    kimix::vector<BfsVertex> &nodes = out.vertices; // discovery order == BFS order
    // WalkVisited (plan O13): the walk-local EXACT index table over `nodes`, linear
    // scan until kFilteredVisitedLinearLimit, no hash table and no cross-DLL hash for a
    // short neighbourhood. It is ALSO the discovered-set membership test of the sweep.
    WalkVisited visited;
    InlineBuffer<size_t, kInlineWalkFrontier> frontier_a; // node indices at this depth
    InlineBuffer<size_t, kInlineWalkFrontier> frontier_b;
    InlineBuffer<size_t, kInlineWalkFrontier> *frontier = &frontier_a;
    InlineBuffer<size_t, kInlineWalkFrontier> *next_frontier = &frontier_b;
    // SEEDING (ABI 8, plan §2.1): each DISTINCT start becomes one item at depth 0 with
    // a zeroed discovering edge, in the caller's order, and all of them seed the
    // depth-0 frontier — so the rest of the walk is untouched and `depth` below means
    // the distance to the nearest start. A duplicate start collapses onto the item the
    // first sighting created. When the distinct starts alone overflow `max_vertices`,
    // the first `max_vertices` of them are kept, the flag says something was dropped,
    // and the walk stops right there: no level was ever expanded, so `edges` stays
    // empty rather than describing a neighbourhood that was never read.
    bool starts_capped = false;
    for (auto const &start : starts) {
        if (visited.contains(nodes, start)) {
            continue; // a repeated start is one item, at its first position
        }
        if (nodes.size() >= static_cast<size_t>(max_vertices)) {
            starts_capped = true;
            break;
        }
        nodes.push_back(BfsVertex{start, EdgeId{}, 0});
        visited.insert(nodes, nodes.size() - 1);
        frontier->push_back(nodes.size() - 1);
    }
    if (starts_capped) {
        out.truncated = true;
        return GdiStatus::ok;
    }
    LinkBuffer links; // one visit's snapshot: allocates on degree growth only
    kimix::vector<EdgeId> batch_edges;
    kimix::vector<kimix::Guid> batch_neighbours;
    kimix::vector<uint8_t> batch_matches;
    kimix::optional<EdgeFilterScratch> filter_scratch; // engaged by the first wide visit
    // True exactly when the vertex cap refused a hop that had passed BOTH tests, i.e.
    // when something readable is not in the result. The walk ends at that instant (the
    // cap never partially expands a level), and a neighbourhood that fits the cap — or
    // fills it exactly and then runs out on its own — leaves the flag false.
    bool capped = false;
    uint32_t depth = 0;

    // ONE place a hop is recorded. Returns false when the cap refused it, which ends
    // the whole walk; the push-before-insert order is _filtered_walk's (the linear scan
    // and the table's engage both read the container the caller has just grown).
    auto const discover = [&](kimix::Guid const &neighbour, EdgeId const &edge,
                              uint32_t level) {
        if (nodes.size() >= static_cast<size_t>(max_vertices)) {
            capped = true;
            return false;
        }
        nodes.push_back(BfsVertex{neighbour, edge, level});
        visited.insert(nodes, nodes.size() - 1);
        next_frontier->push_back(nodes.size() - 1);
        return true;
    };
    // ONE link, ONE verdict — the walk's rule, and the wide visit below is its
    // chunked form, not a different rule: same tests, same order, same first matching
    // parallel edge wins.
    auto const visit_one = [&](EdgeLink const &link) {
        if (visited.contains(nodes, link.neighbour)) {
            return true; // a discovered neighbour is not a hop whatever its edge says
        }
        if (filtered && !edge_matches(link.edge, edge_constraint)) {
            return true; // a refused EDGE is not a hop
        }
        return discover(link.neighbour, link.edge, depth);
    };
    while (!frontier->empty() && !capped && depth < max_depth) {
        ++depth;
        next_frontier->clear();
        for (size_t pos = 0; pos < frontier->size() && !capped; ++pos) {
            size_t const index = (*frontier)[pos];
            if (!_snapshot_links(nodes[index].vertex, orientation, links)) {
                continue; // removed between the enqueue and the visit
            }
            if (!filtered || links.size() < kEdgeFilterMinLinks) {
                // The per-visit path: ONE edge_matches() per surviving link, and the
                // whole unfiltered form (graph.h's cost rule, identical to the walk's).
                for (auto const &link : links) {
                    if (!visit_one(link)) {
                        break; // the cap ended the level, the visit and the walk
                    }
                }
                continue;
            }
            // --- the WIDE visit: HEAD per link, then the TAIL in escalating drains ---
            // Same shape, same constants and the same lock discipline as the walk's
            // wide visit: _snapshot_links released this vertex's shard lock before
            // returning, so each drain takes level-3 locks only, one per TOUCHED shard,
            // ascending. The early exit this bounds is the cap (a path search has the
            // target instead), so the head is still what keeps a hub visit from
            // staging thousands of candidates it will never record.
            size_t const head = links.size() < kEdgeFilterHeadLinks
                                    ? links.size()
                                    : kEdgeFilterHeadLinks;
            for (size_t i = 0; i < head && !capped; ++i) {
                (void)visit_one(links[i]);
            }
            size_t base = head;
            size_t step = kEdgeFilterHeadLinks;
            while (base < links.size() && !capped) {
                size_t const tail = links.size() - base;
                size_t const count = tail < step ? tail : step;
                batch_edges.clear();
                batch_neighbours.clear();
                size_t const room = links.size() - head < kEdgeFilterChunk
                                        ? links.size() - head
                                        : kEdgeFilterChunk;
                if (batch_edges.capacity() < room) {
                    batch_edges.reserve(room);
                    batch_neighbours.reserve(room);
                }
                if (batch_matches.capacity() < room) {
                    batch_matches.reserve(room);
                }
                for (size_t i = base; i < base + count; ++i) {
                    batch_edges.push_back(links[i].edge);
                    batch_neighbours.push_back(links[i].neighbour);
                }
                batch_matches.resize(count);
                if (!filter_scratch.has_value()) {
                    filter_scratch.emplace();
                }
                _filter_edges_batched(
                    kimix::span<const EdgeId>{batch_edges.data(), count}, edge_constraint,
                    kimix::span<uint8_t>{batch_matches.data(), count}, *filter_scratch);
                for (size_t i = 0; i < count; ++i) {
                    if (batch_matches[i] == 0) {
                        continue; // a refused EDGE is not a hop
                    }
                    if (visited.contains(nodes, batch_neighbours[i])) {
                        continue; // discovered by an earlier link of this same chunk
                    }
                    if (!discover(batch_neighbours[i], batch_edges[i], depth)) {
                        break;
                    }
                }
                base += count;
                step = step * 4 < kEdgeFilterChunk ? step * 4 : kEdgeFilterChunk;
            }
        }
        // Pointer swap, not a buffer swap (the frontiers point at their own storage).
        std::swap(frontier, next_frontier);
    }
    out.truncated = capped;

    // --- the SECOND sweep: every matching edge internal to the discovered set -------
    // O(V * degree): one level-1 snapshot per discovered vertex, one level-3 pass over
    // that vertex's staged candidates (per-candidate edge_matches for a narrow staging,
    // the batched filter past kEdgeFilterMinLinks of them — the walk's own cost split),
    // and one level-3 endpoint read per staged candidate that survived (graph.h). The
    // walk's visited set IS the discovered-set membership test, so this needs no second
    // index, and the result is ordered by (discovery order of the visiting vertex, link
    // order in its snapshot). Both staging arrays are InlineBuffers (plan O5: 64 entries
    // inline, the heap only past that, capacity kept for the rest of the sweep), so a
    // normal-degree sweep allocates nothing — which is the point of the two-phase
    // protocol re-running the identical walk on a warm session.
    InlineBuffer<EdgeId, kInlineWalkFrontier> candidates; // one visit's internal edges
    InlineBuffer<uint8_t, kInlineWalkFrontier> flags;     // verdict per candidate
    kimix::Guid sweep_origin;
    kimix::Guid sweep_target;
    for (size_t i = 0; i < nodes.size(); ++i) {
        kimix::Guid const &visitor = nodes[i].vertex;
        if (!_snapshot_links(visitor, orientation, links)) {
            continue; // removed between its discovery and the sweep
        }
        candidates.clear();
        flags.clear(); // the two arrays stay index-aligned, one entry per candidate
        for (auto const &link : links) {
            // The endpoint test FIRST, the cheap half of the pair (the walk's own
            // O1 ordering): a link that leaves the discovered set is not an internal
            // edge whatever its record says, and a confidential edge never gets here
            // because its target was never discovered over it.
            if (!visited.contains(nodes, link.neighbour)) {
                continue;
            }
            if (link.neighbour == visitor) {
                // A self edge under BOTH is listed twice in ONE snapshot (once in its
                // out half, once in its in half), so the endpoint test alone would
                // stage it twice. Keep the first sighting: the scan covers only this
                // vertex's own staged candidates, and self edges are the rare case.
                bool again = false;
                for (size_t k = 0; k < candidates.size(); ++k) {
                    if (candidates[k] == link.edge) {
                        again = true;
                        break;
                    }
                }
                if (again) {
                    continue;
                }
            }
            candidates.push_back(link.edge);
            flags.push_back(0);
        }
        size_t const staged = candidates.size();
        if (staged == 0) {
            continue;
        }
        if (filtered) {
            if (staged < kEdgeFilterMinLinks) {
                for (size_t k = 0; k < staged; ++k) {
                    flags[k] = edge_matches(candidates[k], edge_constraint) ? uint8_t{1}
                                                                            : uint8_t{0};
                }
            } else {
                // One batched pass over the whole staging: one edge-shard SHARED lock
                // per TOUCHED shard, ascending, with no level-1 lock held (LOCK ORDER 4)
                // — _snapshot_links released this vertex's shard lock before returning.
                if (!filter_scratch.has_value()) {
                    filter_scratch.emplace();
                }
                _filter_edges_batched(kimix::span<const EdgeId>{candidates.data(), staged},
                                      edge_constraint, kimix::span<uint8_t>{flags.data(), staged},
                                      *filter_scratch);
            }
        }
        for (size_t k = 0; k < staged; ++k) {
            if (filtered && flags[k] == 0) {
                continue; // a refused EDGE is not an internal edge either
            }
            if (!edge_endpoints(candidates[k], sweep_origin, sweep_target)) {
                continue; // vanished: no record, so nothing to report (and no edge test
                          // was run for the unfiltered form, which this read covers)
            }
            // ONE record per edge uid. An internal edge of a BOTH read is staged from
            // both of its endpoints, so only the sighting on the edge's OWN origin side
            // emits; the two single-direction reads stage one side only, and the same
            // test is trivially true for them (outgoing links start at the origin,
            // incoming links end at the target).
            bool const origin_side = orientation == EdgeOrientation::incoming
                                         ? sweep_target == visitor
                                         : sweep_origin == visitor;
            if (!origin_side) {
                continue;
            }
            out.edges.push_back(BfsEdge{candidates[k], sweep_origin, sweep_target});
        }
    }
    return GdiStatus::ok;
}

// ---------------------------------------------------------------------------
// Dirty bookkeeping
// ---------------------------------------------------------------------------

void Graph::mark_dirty() noexcept {
    for (auto const &shard : _shards) {
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        for (auto const &entry : shard.vertices) {
            if (entry.second) {
                entry.second->mark_dirty();
            }
        }
    }
    // A full save rewrites the edge family too (they are separate records now).
    _edges.mark_dirty();
}

void Graph::clear_dirty() noexcept {
    for (auto const &shard : _shards) {
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        for (auto const &entry : shard.vertices) {
            if (entry.second) {
                entry.second->clear_dirty();
            }
        }
    }
    _edges.clear_dirty();
}

kimix::vector<DirtyVertex> Graph::dirty_vertices() const {
    kimix::vector<DirtyVertex> out;
    for (auto const &shard : _shards) {
        // Shard SHARED (map), then each vertex's SHARED lock for the hash: the
        // documented high-to-low order, never the inverse.
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        out.reserve(out.size() + shard.vertices.size());
        for (auto const &entry : shard.vertices) {
            Vertex *vertex = entry.second.get();
            if (vertex == nullptr || !vertex->is_dirty()) {
                continue;
            }
            DirtyVertex item;
            item.vertex = vertex;
            item.guid = vertex->guid();
            item.content_hash = vertex->content_hash(); // vertex SHARED lock
            out.push_back(item);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Loader-only adoption
// ---------------------------------------------------------------------------

GdiStatus Graph::adopt_vertex(kimix::unique_ptr<Vertex> vertex) {
    if (!vertex) {
        return GdiStatus::error_argument;
    }
    auto const write_lease = _write_lease();
    if (!write_lease) {
        return GdiStatus::error_state;
    }
    kimix::Guid const guid = vertex->guid();
    bool const keyed = vertex->has_key();
    VertexKey const key = keyed ? vertex->key() : VertexKey{};
    Shard &shard = _shards[_shard_of(guid)];
    {
        std::unique_lock<std::shared_mutex> lock(shard.mutex);
        if (!shard.vertices.try_emplace(guid, std::move(vertex)).second) {
            return GdiStatus::error_vertex;
        }
        _size.fetch_add(1, std::memory_order_relaxed);
        _bump_structure();
    }
    restore_removed_vertex(guid);
    if (keyed) {
        // Level 4, after the vertex shard is released. A store whose records
        // disagree about uniqueness leaves the vertex in place but unindexed:
        // the load counts it and carries on rather than dropping a vertex whose
        // edges are still resolvable.
        (void)_keys.insert(key, guid);
    }
    return GdiStatus::ok;
}

GdiStatus Graph::adopt_edge(kimix::unique_ptr<Edge> edge) {
    if (!edge) {
        return GdiStatus::error_argument;
    }
    auto const write_lease = _write_lease();
    if (!write_lease) {
        return GdiStatus::error_state;
    }
    EdgeId const id = edge->id();
    kimix::Guid const origin = edge->origin();
    kimix::Guid const target = edge->target();
    if (origin == target && !_options.allow_self_edges) {
        return GdiStatus::error_edge;
    }
    if (!_edges.insert(std::move(edge))) {
        return GdiStatus::error_vertex; // a duplicate edge uid: treated as a bad record
    }
    restore_removed_edge(id);
    // Link both sides, one ascending lock set for the two (or one) endpoint
    // shards. The endpoints were inserted by the vertex pass, so a missing one is
    // a dangling record, not a race.
    size_t const origin_shard = _shard_of(origin);
    size_t const target_shard = _shard_of(target);
    if (origin_shard == target_shard) {
        std::unique_lock<std::shared_mutex> lock(_shards[origin_shard].mutex);
        Vertex *origin_vertex = _shards[origin_shard]._find(origin);
        Vertex *target_vertex = _shards[origin_shard]._find(target);
        if (origin_vertex == nullptr || target_vertex == nullptr) {
            _edges.erase(id);
            return GdiStatus::error_vertex;
        }
        origin_vertex->_add_out_link(id, target_vertex);
        target_vertex->_add_in_link(id, origin_vertex);
    } else {
        size_t const first = origin_shard < target_shard ? origin_shard : target_shard;
        size_t const second = origin_shard < target_shard ? target_shard : origin_shard;
        std::unique_lock<std::shared_mutex> first_lock(_shards[first].mutex);
        std::unique_lock<std::shared_mutex> second_lock(_shards[second].mutex);
        Vertex *origin_vertex = _shards[origin_shard]._find(origin);
        Vertex *target_vertex = _shards[target_shard]._find(target);
        if (origin_vertex == nullptr || target_vertex == nullptr) {
            _edges.erase(id);
            return GdiStatus::error_vertex;
        }
        origin_vertex->_add_out_link(id, target_vertex);
        target_vertex->_add_in_link(id, origin_vertex);
    }
    _bump_structure();
    return GdiStatus::ok;
}

// ---------------------------------------------------------------------------
// Edge payload (labels / properties): guarded by the edge's OWN SHARD
// ---------------------------------------------------------------------------

namespace src_graph_db_graph {

// Build a PropertyList copy of a bag, so the edge read-back and the vertex
// read-back return the same shape.
void copy_bag(PropertyBag const &bag, PropertyList &out) {
    out.clear();
    out.reserve(bag.size());
    for (auto const &entry : bag) {
        PropertyEntry item;
        item.name = entry.first;
        item.value = entry.second;
        out.push_back(std::move(item));
    }
}

} // namespace src_graph_db_graph
using namespace src_graph_db_graph;

GdiStatus Graph::edge_add_label(kimix::Guid const &edge, LabelId label) {
    auto const write_lease = _write_lease();
    if (!write_lease) {
        return GdiStatus::error_state;
    }
    if (label == kNoLabel) {
        return GdiStatus::error_label;
    }
    size_t const shard = EdgeTable::shard_of(edge);
    std::unique_lock<std::shared_mutex> lock(_edges.shard_mutex(shard));
    Edge *record = _edges._find_locked(shard, edge);
    if (record == nullptr) {
        return GdiStatus::error_uid;
    }
    if (std::find(record->_labels.begin(), record->_labels.end(), label) !=
        record->_labels.end()) {
        return GdiStatus::error_label; // already present (GDI AddLabel semantics)
    }
    record->_labels.push_back(label);
    record->mark_dirty();
    return GdiStatus::ok;
}

GdiStatus Graph::edge_remove_label(kimix::Guid const &edge, LabelId label) {
    auto const write_lease = _write_lease();
    if (!write_lease) {
        return GdiStatus::error_state;
    }
    size_t const shard = EdgeTable::shard_of(edge);
    std::unique_lock<std::shared_mutex> lock(_edges.shard_mutex(shard));
    Edge *record = _edges._find_locked(shard, edge);
    if (record == nullptr) {
        return GdiStatus::error_uid;
    }
    auto it = std::find(record->_labels.begin(), record->_labels.end(), label);
    if (it == record->_labels.end()) {
        return GdiStatus::error_label; // absent (GDI RemoveLabel semantics)
    }
    record->_labels.erase(it);
    record->mark_dirty();
    return GdiStatus::ok;
}

bool Graph::edge_labels(kimix::Guid const &edge, kimix::vector<LabelId> &out) const {
    return _edges.labels(edge, out);
}

GdiStatus Graph::edge_add_property(kimix::Guid const &edge, PropNameId name,
                                   PropertyValue value) {
    auto const write_lease = _write_lease();
    if (!write_lease) {
        return GdiStatus::error_state;
    }
    size_t const shard = EdgeTable::shard_of(edge);
    std::unique_lock<std::shared_mutex> lock(_edges.shard_mutex(shard));
    Edge *record = _edges._find_locked(shard, edge);
    if (record == nullptr) {
        return GdiStatus::error_uid;
    }
    if (record->_properties.find(name) != record->_properties.end()) {
        return GdiStatus::error_property_exists;
    }
    record->_properties.emplace(name, std::move(value));
    record->mark_dirty();
    return GdiStatus::ok;
}

GdiStatus Graph::edge_set_property(kimix::Guid const &edge, PropNameId name,
                                   PropertyValue value) {
    auto const write_lease = _write_lease();
    if (!write_lease) {
        return GdiStatus::error_state;
    }
    size_t const shard = EdgeTable::shard_of(edge);
    std::unique_lock<std::shared_mutex> lock(_edges.shard_mutex(shard));
    Edge *record = _edges._find_locked(shard, edge);
    if (record == nullptr) {
        return GdiStatus::error_uid;
    }
    auto it = record->_properties.find(name);
    if (it == record->_properties.end()) {
        return GdiStatus::error_no_property;
    }
    it->second = std::move(value);
    record->mark_dirty();
    return GdiStatus::ok;
}

GdiStatus Graph::edge_put_property(kimix::Guid const &edge, PropNameId name,
                                   PropertyValue value) {
    auto const write_lease = _write_lease();
    if (!write_lease) {
        return GdiStatus::error_state;
    }
    size_t const shard = EdgeTable::shard_of(edge);
    std::unique_lock<std::shared_mutex> lock(_edges.shard_mutex(shard));
    Edge *record = _edges._find_locked(shard, edge);
    if (record == nullptr) {
        return GdiStatus::error_uid;
    }
    auto it = record->_properties.find(name);
    if (it != record->_properties.end()) {
        it->second = std::move(value);
    } else {
        record->_properties.emplace(name, std::move(value));
    }
    record->mark_dirty();
    return GdiStatus::ok;
}

GdiStatus Graph::edge_remove_properties(kimix::Guid const &edge,
                                        kimix::span<PropNameId const> names) {
    auto const write_lease = _write_lease();
    if (!write_lease) {
        return GdiStatus::error_state;
    }
    size_t const shard = EdgeTable::shard_of(edge);
    std::unique_lock<std::shared_mutex> lock(_edges.shard_mutex(shard));
    Edge *record = _edges._find_locked(shard, edge);
    if (record == nullptr) {
        return GdiStatus::error_uid;
    }
    for (auto name : names) {
        // Removing an absent property is not an error (GDI RemoveProperties).
        record->_properties.erase(name);
    }
    record->mark_dirty();
    return GdiStatus::ok;
}

bool Graph::edge_properties(kimix::Guid const &edge, PropertyList &out) const {
    size_t const shard = EdgeTable::shard_of(edge);
    std::shared_lock<std::shared_mutex> lock(_edges.shard_mutex(shard));
    Edge const *record = _edges._find_locked(shard, edge);
    if (record == nullptr) {
        return false;
    }
    copy_bag(record->_properties, out);
    return true;
}

bool Graph::edge_property(kimix::Guid const &edge, PropNameId name,
                          PropertyValue &out) const {
    return _edges.property(edge, name, out);
}

bool Graph::edge_record(kimix::Guid const &edge, EdgeRecord &out) const {
    return _edges.record(edge, out);
}

bool Graph::edge_endpoints(kimix::Guid const &edge, kimix::Guid &origin,
                           kimix::Guid &target) const {
    return _edges.endpoints(edge, origin, target);
}

void Graph::incident_edges(kimix::Guid const &vertex, EdgeOrientation orientation,
                           kimix::vector<EdgeId> &out) const {
    out.clear();
    Shard const &shard = _shards[_shard_of(vertex)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    Vertex const *record = shard._find(vertex);
    if (record == nullptr) {
        return;
    }
    // One list scan per requested direction: the link list already IS the
    // orientation, which is why an oriented read needs no incidence check.
    if (orientation == EdgeOrientation::outgoing || orientation == EdgeOrientation::both) {
        out.reserve(record->_out_links.size() + record->_in_links.size());
        for (auto const &link : record->_out_links) {
            out.push_back(link.edge);
        }
    }
    if (orientation == EdgeOrientation::incoming || orientation == EdgeOrientation::both) {
        out.reserve(record->_out_links.size() + record->_in_links.size());
        for (auto const &link : record->_in_links) {
            out.push_back(link.edge);
        }
    }
}

bool Graph::has_edge(kimix::Guid const &origin, kimix::Guid const &target) const {
    Shard const &shard = _shards[_shard_of(origin)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    Vertex const *record = shard._find(origin);
    if (record == nullptr) {
        return false;
    }
    return _has_out_edge_locked(record, target);
}

bool Graph::has_incident_edges(kimix::Guid const &vertex, EdgeOrientation orientation) const {
    Shard const &shard = _shards[_shard_of(vertex)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    Vertex const *record = shard._find(vertex);
    if (record == nullptr) {
        return false;
    }
    if ((orientation == EdgeOrientation::outgoing || orientation == EdgeOrientation::both) &&
        !record->_out_links.empty()) {
        return true;
    }
    if ((orientation == EdgeOrientation::incoming || orientation == EdgeOrientation::both) &&
        !record->_in_links.empty()) {
        return true;
    }
    return false;
}

kimix::vector<kimix::Guid> Graph::take_removed_vertices() {
    kimix::vector<kimix::Guid> out;
    out.swap(_removed_vertices);
    return out;
}

kimix::vector<EdgeId> Graph::take_removed_edges() {
    kimix::vector<EdgeId> out;
    out.swap(_removed_edges);
    return out;
}

void Graph::restore_removed_vertex(kimix::Guid const &guid) {
    auto it = std::find(_removed_vertices.begin(), _removed_vertices.end(), guid);
    if (it != _removed_vertices.end()) {
        _removed_vertices.erase(it);
    }
}

void Graph::restore_removed_edge(EdgeId const &edge) {
    auto it = std::find(_removed_edges.begin(), _removed_edges.end(), edge);
    if (it != _removed_edges.end()) {
        _removed_edges.erase(it);
    }
}

kimix::vector<kimix::Guid> Graph::vertex_guids() const {
    kimix::vector<kimix::Guid> out;
    for (auto const &shard : _shards) {
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        out.reserve(out.size() + shard.vertices.size());
        for (auto const &entry : shard.vertices) {
            out.push_back(entry.first);
        }
    }
    return out;
}

kimix::vector<EdgeId> Graph::edge_ids() const {
    kimix::vector<EdgeId> out;
    for (size_t shard_index = 0; shard_index < kEdgeShardCount; ++shard_index) {
        std::shared_lock<std::shared_mutex> lock(_edges.shard_mutex(shard_index));
        _edges.ids_of_shard(shard_index, out);
    }
    return out;
}

void Graph::edge_restore_payload(EdgeId const &edge, kimix::span<const LabelId> labels,
                                 PropertyList const &properties) {
    size_t const shard = EdgeTable::shard_of(edge);
    std::unique_lock<std::shared_mutex> lock(_edges.shard_mutex(shard));
    Edge *record = _edges._find_locked(shard, edge);
    if (record == nullptr) {
        return;
    }
    record->_labels.clear();
    for (auto label : labels) {
        record->_labels.push_back(label);
    }
    record->_properties.clear();
    for (auto const &entry : properties) {
        record->_properties.emplace(entry.name, entry.value);
    }
    record->mark_dirty();
}

kimix::vector<EdgeRecord> Graph::dirty_edges() const {
    return _edges.dirty_edges();
}

bool Graph::link_counts(kimix::Guid const &guid, size_t &outgoing, size_t &incoming) const {
    Shard const &shard = _shards[_shard_of(guid)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    Vertex const *record = shard._find(guid);
    if (record == nullptr) {
        return false;
    }
    outgoing = record->_out_links.size();
    incoming = record->_in_links.size();
    return true;
}

} // namespace kimix::graph_db