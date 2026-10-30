/*
 * vertex.h — kimix::graph_db::Vertex: one node of the graph.
 *
 * A Vertex owns
 * - its identity (_guid),
 * - an opaque content blob (_content) plus the cached content digest,
 * - its dependency links: one EdgeLink vector per direction (edge uid + neighbour
 *   uid), the SINGLE in-memory adjacency representation (plan v3 §5.2: the
 *   guid-list and Vertex* set projections this file used to keep alongside the
 *   links were triple storage of the same fact and are gone),
 * - a per-vertex read/write lock and a dirty flag for incremental save.
 *
 * The dirty flag is not a bare bool: while the vertex belongs to a Graph, every
 * flip is reported to that Graph through VertexDirtySink (see the comment on the
 * interface below), which is how the save's collect stage stays O(dirty) instead
 * of rescanning the graph. The flag is private and reachable only through the
 * mark_dirty()/clear_dirty() protocol, so no write can escape the report.
 *
 * LOCKING (see graph.h for the full contract)
 * _mutex (kimix::shared_atomic_mutex, a shared spin lock) guards _content,
 * _content_hash, _properties, _labels and _dirty only: content/payload reads take
 * it shared, writes take it unique. A spin lock is admissible here because every
 * section is BOUNDED (the rule is written down in core/shared_atomic_mutex.h and
 * in graph.h's LOCK KIND POLICY): a container operation, an assignment, a copy of
 * an already-sized buffer. The ONE deliberately accepted exception is content()
 * / content_copy() / properties(), which copy the whole blob under the SHARED
 * lock — O(content size) — because the alternative (a snapshot or COW scheme) is
 * a much larger change; readers of the same vertex are rare by design (the
 * intended parallelism is over DISJOINT vertices), and no writer can be starved
 * by them: the writer preference of the lock blocks new readers as soon as one
 * writer waits. Hashing never runs under the lock: set_content() computes the
 * digest of the incoming bytes first, touch() copies and hashes outside (see
 * vertex.cpp).
 * The link vectors are GRAPH structure: they are guarded by the
 * SHARD lock of this vertex's shard (graph.h — hash(guid) & (kGraphShardCount -
 * 1)), which a Graph holds while it mutates them (unique) or reads them
 * (shared).
 * The raw out_links()/in_links() accessors below therefore do NOT lock: they
 * are for a caller that already owns the structural state — Graph's own
 * internals — which is the only production consumer. Everything else must go
 * through Graph::dependencies()/dependents()/neighbors(), which take the shard
 * lock themselves.
 * Lock ordering is always shard-lock before vertex-lock, never the inverse.
 *
 * CACHE-LINE SEPARATION
 *   _mutex is alignas(64): a vertex therefore starts on a cache line and its
 *   lock bitfield + dirty flag occupy a cache line of their own, so writing the
 *   content of vertex A never invalidates the line another thread spins on for
 *   vertex B (no false sharing between adjacent vertices). This makes Vertex
 *   over-aligned (alignof(Vertex) == 64), which is why the class provides
 *   aligned operator new/delete below — the plain mimalloc allocation inherited
 *   from kimix::IOperatorNewBase only guarantees 16-byte alignment.
 *
 * CONTENT MODEL — TODO
 *   _content is deliberately an opaque, already-serialized byte blob. The target
 *   model is the typed property graph of graph_db.md §1: a PropertyValue tagged
 *   union (int64 / double / string / blob / int64[] / double[]), a label set and
 *   a (label, external_id) primary key. That design is intentionally NOT done
 *   here: the container, the codec and the lock discipline must work before the
 *   property model is chosen, and every consumer above this layer only needs a
 *   stable blob with a content hash.
 *
 * Example:
 *   kimix::graph_db::Vertex v;                       // fresh guid, empty content
 *   v.set_content(bytes);                            // unique vertex lock
 *   kimix::vector<std::byte> copy = v.content_copy(); // shared vertex lock
 *   kimix::MD5 const hash = v.content_hash();        // shared vertex lock
 */
#pragma once

  #include "graph_db_api.h"
  #include "edge.h"
  #include "graph_db_options.h"
  #include "key.h"
  #include "property.h"

#include <core/guid.h>
#include <core/md5.h>
#include <core/memory.h>
#include <core/shared_atomic_mutex.h>
#include <core/stl/memory.h>
#include <core/stl/unordered_map.h>
#include <core/stl/vector.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

namespace kimix::graph_db {

// Cache line the per-vertex atomic state is padded to (see the class comment).
inline constexpr size_t kCacheLineSize = 64;

/*
 * RAII guards for the per-vertex kimix::shared_atomic_mutex. They are ALIASES of
 * the core guards (core/shared_atomic_mutex.h) rather than guards defined here:
 * this file used to hand-roll the pair because the core mutex had no adapter, and
 * two copies of the same guard would drift. The names stay because they are the
 * vertex layer's public spelling (the serializer and README.md name them);
 * KIMIX_GRAPH_DB_API is deliberately NOT put on an alias — the guarded class is
 * kimix-core's.
 *
 * The mutex is `mutable`, so every guard can be constructed in a const member
 * function without a const_cast (the guards themselves take a non-const
 * reference, exactly like std::lock_guard/std::unique_lock).
 */
using VertexUniqueLock = kimix::shared_atomic_unique_lock;
using VertexSharedLock = kimix::shared_atomic_shared_lock;

class Graph;
class Vertex;

/*
 * The dirty-flag SINK: what an OWNED Vertex calls when its dirty flag flips, so
 * the owner (Graph) can keep its dirty index in step.
 *
 * WHY THE NOTIFICATION SITS IN THE FLAG SETTER. The incremental save writes
 * exactly the records whose dirty flag is set, so a flag that flips without the
 * owner hearing about it silently DROPS A RECORD from the next save — a lost
 * update, not a wrong number. The flag write therefore has to funnel through one
 * place per object (Vertex::_mark_dirty / Vertex::_clear_dirty), and that place
 * has to tell the owner. Vertex cannot hold a Graph pointer: vertex.h must not
 * depend on graph.h, and the ownership has to be CLEARABLE (a vertex returned by
 * Graph::remove_vertex belongs to no graph any more, and a caller may keep
 * marking it dirty). So the owner is an interface with two calls, implemented by
 * Graph and attached only while the vertex sits in a graph's shard map. A missed
 * call site is then structurally impossible rather than a review item: _dirty is
 * private and this file is the only code that stores into it (see vertex.cpp).
 */
class VertexDirtySink {
public:
    // The flag went false -> true: the record must be in the next save.
    virtual void note_vertex_dirty(Vertex *vertex) noexcept = 0;
    // The flag went true -> false: the record was written (or restored), so it
    // has to leave the index.
    virtual void clear_vertex_dirty(Vertex *vertex) noexcept = 0;

protected:
    // Never deleted through this pointer: the sink is a part of its owner.
    ~VertexDirtySink() = default;
};

/*
 * One graph node. Move-only (the lock is pinned), created through
 * Graph::create_vertex / Graph::add_vertex or directly on the stack/in a
 * unique_ptr by the tests and the later deserializer.
 */
class KIMIX_GRAPH_DB_API Vertex : public kimix::IOperatorNewBase {
    friend class Graph;

public:
      Vertex() noexcept;
      explicit Vertex(kimix::Guid const &guid) noexcept;
      Vertex(kimix::Guid const &guid,
             kimix::span<const std::byte> initial_content);
      // The content digest policy (plan v3 §4.5) is fixed per vertex by whoever
      // materializes it (Graph/loader) — the Graph's GraphOptions value.
      Vertex(kimix::Guid const &guid,
             kimix::span<const std::byte> initial_content, ContentDigest content_digest);
    Vertex(Vertex const &) = delete;
    Vertex &operator=(Vertex const &) = delete;
    Vertex(Vertex &&) = delete;
    Vertex &operator=(Vertex &&) = delete;
    ~Vertex() noexcept = default;

    // --- identity -----------------------------------------------------------
    [[nodiscard]] kimix::Guid const &guid() const noexcept { return _guid; }
    void set_guid(kimix::Guid const &guid) noexcept { _guid = guid; }

    // --- content (vertex lock: shared read / unique write) ------------------
    // Copy of the content. Takes the vertex lock in SHARED mode, so any number
    // of readers may run while a writer waits for its unique lock.
    void content(kimix::vector<std::byte> &out) const;
    [[nodiscard]] kimix::vector<std::byte> content_copy() const;
    [[nodiscard]] size_t content_size() const;
    // MD5 cached at the last content write (recomputed under the unique lock).
    [[nodiscard]] kimix::MD5 content_hash() const;
    // Replace the content, recompute the cached hash and mark the vertex dirty.
    void set_content(kimix::span<const std::byte> content);
    // Recompute the cached hash from the current content and mark it dirty.
    // Used by the codec/loader after a record is materialized.
    void touch();
    // --- dirty flag (the incremental save's write set) -----------------------
    // FLAG PROTOCOL: every write of the flag goes through _mark_dirty /
    // _clear_dirty below, which forward to the attached VertexDirtySink (Graph)
    // when there is one and store directly when there is not (a vertex no graph
    // owns). mark_dirty()/clear_dirty() are the public spellings of that protocol
    // and are what every caller — Graph, the serializer, the tests — uses; the
    // inline flag writes inside vertex.cpp call _mark_dirty() too, so there is no
    // path that flips the flag behind the sink's back.
    void mark_dirty() noexcept { _mark_dirty(); }
    void clear_dirty() noexcept { _clear_dirty(); }
    [[nodiscard]] bool is_dirty() const noexcept {
        return _dirty.load(std::memory_order_acquire);
    }

    // --- typed payload: labels and properties (VERTEX lock) ------------------
    // The vertex lock (_mutex, the same one content uses) guards these: a
    // property read takes it shared, a write unique. Two threads updating two
    // DIFFERENT vertices never contend, which is the whole point of keeping the
    // payload off the shard lock.
    //   add_*    inserts only; false when the name/label is already present
    //   set_*    replaces only; false when it is absent
    //   put_*    upsert (GDI's UpdateProperty): true when a value was replaced
    //   remove_* ignores an absent name and returns how many were removed
    // The schema's declaration checks live one layer up (Database): a Vertex
    // stores what it is given, which keeps the container usable without a schema.
    bool add_property(PropNameId name, PropertyValue value);
    bool set_property(PropNameId name, PropertyValue value);
    bool put_property(PropNameId name, PropertyValue value);
    size_t remove_properties(kimix::span<PropNameId const> names);
    [[nodiscard]] bool has_property(PropNameId name) const;
    [[nodiscard]] bool property(PropNameId name, PropertyValue &out) const;
      // Every property, copied. Order: the bag's iteration order (insertion order
      // for ankerl's dense map), so an unfiltered read-back is stable.
      void properties(PropertyList &out) const;
      [[nodiscard]] PropertyList property_list() const;
      [[nodiscard]] size_t property_count() const;
      // The ZERO-COPY visitor form of properties() (plan v3 §4.3): `visitor` is
      // invoked as visitor(name_id, value) under ONE leaf shared lock, in the
      // bag's iteration order, until it returns false. The value is borrowed:
      // it dies with the call, so a visitor that keeps it must copy. Use this
      // on hot read paths instead of properties(), which deep-copies every
      // value (a string/blob property costs an allocation per copy).
      // Zero-copy property probe (plan v3 §6.6): the BORROWED value or
      // nullptr, via one leaf shared lock and no copies. The aggregate
      // fan-out, filtered scan and filtered traversal paths use this instead
      // of property_list() — a deep copy of every value (a 256-char string
      // property was a heap alloc + memcpy PER VERTEX PER READ).
      [[nodiscard]] PropertyValue const *find_property(PropNameId name) const {
          PropertyValue const *found = nullptr;
          visit_properties([name, &found](PropNameId n, PropertyValue const &value) {
              if (n == name) {
                  found = &value;
                  return false;
              }
              return true;
          });
          return found;
      }

      template <typename Visitor>
      void visit_properties(Visitor &&visitor) const {
          VertexSharedLock guard(_mutex); // mutable lock: no const_cast needed
          for (auto const &entry : _properties) {
              if (!visitor(entry.first, entry.second)) {
                  return;
              }
          }
      }

    bool add_label(LabelId label);
    bool remove_label(LabelId label);
    [[nodiscard]] bool has_label(LabelId label) const;
    void labels(kimix::vector<LabelId> &out) const;

    // Payload assignment for a vertex that is NOT published yet: the loader and
    // Graph::create_vertex fill a fresh vertex through this before it becomes
    // visible, which is why it needs no lock. Calling it on a vertex the graph
    // already holds is a contract violation (and would race every reader).
    void assign_payload(LabelId key_label, kimix::span<const std::byte> external_id,
                        kimix::span<const LabelId> labels, PropertyList const &properties);

    // Overwrite a PUBLISHED vertex's labels and properties in one step (vertex
    // unique lock). The transaction rollback path: it restores a payload exactly,
    // without pretending to be a sequence of add/remove calls.
    void restore_payload(kimix::span<const LabelId> labels, PropertyList const &properties);

    // --- key (IMMUTABLE: set before insert, read-only afterwards) -----------
    // A vertex's (label, external id) key never changes — GDI has no rename
    // command for it, and a change would have to re-index the vertex. Like
    // guid(), both accessors are therefore lock-free.
    [[nodiscard]] LabelId key_label() const noexcept { return _key_label; }
    [[nodiscard]] ExternalId const &external_id() const noexcept { return _external_id; }
    [[nodiscard]] bool has_key() const noexcept { return _key_label != kNoLabel; }
    // Same key in its owned form (a copy).
    [[nodiscard]] VertexKey key() const;

    // --- dependencies (GRAPH structure: caller holds this vertex's SHARD lock,
    // or runs under the single-writer rule; see the LOCKING comment above) ----
    // The adjacency is a list of (edge uid, neighbour uid) pairs, one entry per
    // EDGE: parallel edges are representable, an oriented read is a scan of one
    // list, and the neighbour guid is right there in the entry — the separate
    // guid projections and Vertex* sets this class used to maintain alongside
    // (plan v3 §5.2) were triple storage of the same fact: reads now derive the
    // guid view from these lists (Graph::_read_oriented_adjacency et al).
    [[nodiscard]] kimix::vector<EdgeLink> const &out_links() const noexcept { return _out_links; }
    [[nodiscard]] kimix::vector<EdgeLink> const &in_links() const noexcept { return _in_links; }

    // --- dependencies (GRAPH structure: caller holds this vertex's shard lock,
    // or runs under the single-writer rule; see the LOCKING comment above) ----
    // Epoch-2 sizing hook: reserve room for `outgoing` + `incoming` edges in the
    // two containers those edges live in. A bulk builder (the loader's link
    // pass, Graph::connect's caller) knows its vertex's final degree up front, so
    // reserving once removes the per-edge growth reallocation - and the 16-slot
    // minimum `kimix::vector` starts with. Never shrinks: it only ever raises
    // capacity, so calling it with a smaller count than the current capacity is
    // a no-op, and it never touches the elements. Same locking rule as the
    // accessors above: graph structure, so caller holds this vertex's SHARD
    // lock (unique).
    void reserve_links(size_t outgoing, size_t incoming);
    // Outgoing-edge count (one per edge, parallel edges counted individually).
    [[nodiscard]] size_t depending_count() const noexcept { return _out_links.size(); }
    // Incoming-edge count (one per edge, parallel edges counted individually).
    [[nodiscard]] size_t depended_count() const noexcept { return _in_links.size(); }
    [[nodiscard]] bool depends_on(kimix::Guid const &guid) const noexcept;

    // --- locking (exposed for callers that already own a Vertex*, e.g. the
    // serializer, which wants a long-lived content snapshot) -----------------
    void lock_shared() const noexcept { _mutex.lock_shared(); }
    void unlock_shared() const noexcept { _mutex.unlock_shared(); }
    void lock_unique() noexcept { _mutex.lock(); }
    void unlock_unique() noexcept { _mutex.unlock(); }

    // Per-vertex lock, exposed for the tests' cache-line sanity checks.
    [[nodiscard]] kimix::shared_atomic_mutex const &mutex() const noexcept { return _mutex; }

    // --- aligned allocation -------------------------------------------------
    // Vertex is alignas(kCacheLineSize), i.e. over-aligned, so the compiler
    // selects the align_val_t overloads of operator new/delete for every
    // `new Vertex` / `std::make_unique<Vertex>()`; the plain forms are declared
    // too so no path falls back to the global aligned operators (which are not
    // mimalloc). Both flavours route through mi_malloc_aligned / mi_free, the
    // same heap family kimix::IOperatorNewBase uses.
    static void *operator new(size_t size) noexcept;
    static void *operator new(size_t size, std::align_val_t align) noexcept;
    static void operator delete(void *ptr) noexcept;
    static void operator delete(void *ptr, size_t size) noexcept;
    static void operator delete(void *ptr, std::align_val_t align) noexcept;
    static void operator delete(void *ptr, size_t size, std::align_val_t align) noexcept;

private:
    // --- the dirty-flag protocol (the ONE place the flag is written) ---------
    // Defined in vertex.cpp. With a sink attached the owner does the write AND
    // its index bookkeeping under its own mutex (Graph::note_vertex_dirty /
    // Graph::clear_vertex_dirty), so the flag and the index entry can never
    // disagree; without one (a vertex no graph owns yet, or any more) the flag is
    // stored directly.
    void _mark_dirty() noexcept;
    void _clear_dirty() noexcept;

    // Pre-insert population: called by Graph on a vertex no reader can see yet,
    // so no lock is taken (and none could be: the lock protects published state).
    void _labels_unlocked_push(LabelId label) {
        if (label != kNoLabel) {
            _labels.push_back(label);
        }
    }
    void _properties_unlocked_set(PropertyList const &properties) {
        for (auto const &entry : properties) {
            _properties.emplace(entry.name, entry.value);
        }
    }

    // Key assignment: called by Graph BEFORE the vertex is inserted (and by the
    // loader before add_vertex), i.e. while no reader can observe the vertex.
    void _set_key(LabelId label, ExternalId external_id) noexcept {
        _key_label = label;
        _external_id = std::move(external_id);
    }

    // Edge-link mutators. The caller holds this vertex's shard UNIQUE lock (and
    // nothing else), which is what makes the link list, the guid projection and
    // the neighbour sets mutually consistent.
    // `neighbour` may be nullptr (the neighbour is already being removed): the
    // link and its guid projection are dropped either way, only the resolved
    // pointer set is skipped.
    void _add_out_link(kimix::Guid const &edge, Vertex *neighbour);
    void _add_in_link(kimix::Guid const &edge, Vertex *neighbour);
    bool _remove_out_link(kimix::Guid const &edge, Vertex *neighbour);
    bool _remove_in_link(kimix::Guid const &edge, Vertex *neighbour);
    // Drop every edge of this vertex without touching the neighbours (the
    // cascade in Graph::remove_vertex does the neighbour side).
    void _clear_links() noexcept;

    kimix::Guid _guid;
      kimix::MD5 _content_hash; // guarded by _mutex
      kimix::vector<std::byte> _content; // guarded by _mutex
      // Which digest _content_hash uses (GraphOptions::content_digest at
      // materialization time; immutable afterwards).
      ContentDigest _content_digest{ContentDigest::md5};
    kimix::vector<EdgeLink> _out_links;   // graph lock (shard unique/shared)
    kimix::vector<EdgeLink> _in_links;    // graph lock
    PropertyBag _properties;              // guarded by _mutex
    kimix::vector<LabelId> _labels;       // guarded by _mutex
    LabelId _key_label{kNoLabel};         // immutable after construction
    ExternalId _external_id;              // immutable after construction

    // Dedicated cache line: the lock bitfield and the dirty flag live alone so
    // neighbouring vertices never invalidate each other's line (see the class
    // comment). The remaining members above are cold relative to this line.
    alignas(kCacheLineSize) mutable kimix::shared_atomic_mutex _mutex;
    // PRIVATE on purpose: the only writers are _mark_dirty/_clear_dirty (and the
    // sink they call), so a stray direct store cannot bypass the dirty index.
    std::atomic<bool> _dirty{true};
    // The sink of the Graph this vertex currently belongs to, or nullptr. Set and
    // cleared only by Graph::_attach_dirty / Graph::_detach_dirty (Graph is a
    // friend), always together with the index entry it implies. It lands in the
    // padding of the dedicated lock/dirty cache line above, so it costs no extra
    // line and sizeof(Vertex) does not grow.
    VertexDirtySink *_dirty_sink{nullptr};
};

} // namespace kimix::graph_db