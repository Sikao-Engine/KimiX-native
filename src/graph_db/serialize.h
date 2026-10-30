/*
 * serialize.h — parallel incremental save and dual-stage load for kimix-graph-db.
 *
 * Two classes over the cold-store seam of cold_store.h — a non-owning handle
 * over the LMDB environment of utils.h or over the flat file of
 * file_cold_store.h:
 *
 *   GraphSerializer    a Graph      -> the cold store (full / incremental save)
 *   GraphDeserializer  the cold store -> a Graph     (dual-stage, cycle- and
 *                      corruption-proof)
 *
 * The byte layout is entirely utils.h's: a value is a v2 ("GDB2") VertexRecordV2
 * record written by encode_vertex_record_v2() — or a legacy "GDB1" VertexRecord,
 * which decode_any_vertex_record() still reads — and the key is guid_key()'s "v/"
 * + 16 binary guid bytes. An edge is a record of its own under the "e/" key space.
 * Nothing here re-implements little-endian encoding — both classes go through those
 * helpers.
 *
 * ===========================================================================
 * SAVE — two stages, one transaction
 * ===========================================================================
 *   stage 1 (collect)  Graph::dirty_vertices() / Graph::dirty_edges() — O(dirty):
 *                      the guids/ids come from the dirty index the flag protocol
 *                      maintains (one short index lock for the snapshot), then each
 *                      one is resolved under one shard SHARED lock and the cached
 *                      hash is read under that vertex's SHARED lock. The result is
 *                      the exact set this call will write; nothing else is ever
 *                      encoded.
 * stage A (encode) kimix::fiber::parallel over that set: each job copies the
 * content (Vertex::content_copy(), vertex SHARED lock) and encodes the record
 * straight into its own LMDBWriteCommand slot. A record carries no adjacency —
 * the "e/" records are the authority, and the two LEGACY projection lists stay
 * in the layout but are written EMPTY (utils.h, VertexRecordV2), which keeps one
 * O(degree) copy + allocation per dirty vertex out of this stage. Jobs write
 * DISJOINT slots, so the fan-out needs no lock.
 *   stage B (write)    ONE ColdStorage::save_batch() call — a single LMDB
 *                      transaction, therefore atomic. It runs on the CALLING
 *                      thread, never inside a fiber job: write_all() is a batch
 *                      of blocking LMDB syscalls and would park a pool worker
 *                      for its whole duration (graph_db_plan.md §9 risk note).
 *                      A caller that already runs inside a fiber should wrap the
 *                      whole save in kimix::fiber::blocking_call().
 *                      EPOCH 2 sorts the batch into LMDB key order first (see
 *                      stage A' below); that changes only the order in which
 *                      mdb_put walks the B-tree, never the record set, the byte
 *                      image of a record, or the single-transaction atomicity.
 *   stage A' (order)   std::sort of the finished batch by its "v/"+guid key,
 *                      i.e. by the exact bytes LMDB compares. A random-insert
 *                      B-tree build becomes a bulk-load one: measured here, 100k
 *                      records went from 11.9k to 9.2k dirty pages and the commit
 *                      from ~1.04 s to ~0.80 s (sort cost 14-16 ms, one thread,
 *                      against a ~200 ms commit win).
 *   then the dirty flag of every written record is cleared —
 *   CONDITIONALLY on the record's dirty EPOCH (Vertex::dirty_epoch /
 *   Edge::dirty_epoch, see Graph::clear_vertex_dirty_if and
 *   EdgeTable::clear_dirty_if). A level-2 write is allowed to land
 *   while a save runs and bumps that stamp, so a record whose stamp
 *   moved between the collect and this clear stays dirty (and indexed)
 *   for the next save instead of being marked clean and lost. The
 *   epoch covers every kind of write; the old content-hash comparison
 *   could not see a property/label/link change at all and raced the
 *   clear itself.
 *
 * A REFUSED batch (the backend reports false) wrote nothing, so the save
 * changes nothing either: no dirty flag is cleared, the pending meta record
 * stays pending, and the deletion tombstones go BACK into the graph
 * (restore_removed_vertices/edges) so the next save still deletes them.
 * The failure path is reachable only through a broken store, which is why
 * the suite drives it through the debug-only test_fail_next_batch() seam.
 *
 * save()             FULL save: marks every vertex dirty first, so every vertex
 *                    is re-encoded and rewritten (checkpoint semantics). It does
 *                    NOT delete the records of vertices that are gone from the
 *                    graph — a full save is one atomic write transaction, so
 *                    removal stays an explicit ColdStorage::remove_record() /
 *                    copy_to() step.
 * save_incremental() the primary path: ONLY the dirty vertices are encoded and
 *                    written; a clean vertex is neither re-encoded nor rewritten,
 *                    so its stored bytes stay bit-identical.
 * copy_to(path)      passthrough to ColdStorage::copy_to() (compacted
 *                    snapshot/checkpoint of the whole environment).
 *
 * The record's content hash is recomputed from the bytes actually copied, so a
 * content update racing with a save can never produce a record whose stored MD5
 * disagrees with its own content.
 *
* CALLER CONTRACT (inherited from Graph, see graph.h):
 *  - No structural write (create/remove vertex, connect/disconnect) may run
 * concurrently with a save: the graph is structurally quiescent while it is
 * saved. Concurrent level-2 content updates ARE allowed (see below); the
 * dependency lists are read through the shard-locked Graph accessors, so each
 * vertex's pair of lists is consistent even if a reader races them.
 *  - Graph::connect/disconnect change BOTH endpoints' serialized dependency
 *    lists but do not set their dirty flags (the dirty flag is content
 *    bookkeeping). After a structural change the caller must mark the affected
 *    vertices dirty (Vertex::mark_dirty()) or call Graph::mark_dirty(); a full
 *    save() does that for the whole graph. This is the same "bookkeeping is the
 *    caller's" rule dirty_vertices() documents.
 *  - Removing a vertex does not delete its record and no save prunes one: a save
 *    is a write transaction, so dropping a record stays an explicit
 *    ColdStorage::remove_record() step (copy_to() then compacts the store).
 *
 * ===========================================================================
 * LOAD — the dual-stage guarantee
 * ===========================================================================
 * stage 1 (collect) ColdStorage::load_all_records() — ONE sequential scan,
 * because kimix::LMDB's iterator holds a single cursor
 * (graph_db_plan.md §9: scan first, shard later). Every
 * "v/" key becomes a collection slot; then a fiber-parallel
 * pass runs decode_any_vertex_record() on the copied bytes,
 * filling (guid, content, GDI payload, validity) plus — for a
 * v1 record only — its outgoing dependency guids. Everything
 * else is decoded (so a bad buffer is caught) but not retained:
 * the stored content hash, the reverse list, and the projection
 * of a v2 record (the "e/" records are authoritative), since the
 * Vertex constructor re-hashes and the link pass rebuilds the
 * reverse edges. Corrupt values are flagged, never fatal.
 *   stage 2 (materialize) A fiber-parallel pass builds a bare
 *                       kimix::unique_ptr<Vertex> per valid slot — guid plus
 *                       content — and NOTHING else: no dependency list, no
 *                       pointer, no graph mutation. The single-threaded insert
 *                       then adopts them into the target Graph through
 *                       Graph::add_vertex (a structural write, so one thread);
 *                       the target graph itself is the staging container, since
 *                       it already owns the guid -> Vertex mapping a second index
 *                       would only duplicate.
 * link pass Only AFTER every vertex is in the graph are the edges
 * resolved, through the graph's edge API — which fills both the
 * guid lists and the Vertex* sets on both endpoints. The "e/"
 * records go first (they are authoritative for a v2 store: their
 * uid, direction, labels and properties are adopted verbatim),
 * and a V1 record's outgoing guid list is the one fallback that
 * can still create an edge, because a v1 store has no "e/"
 * records at all. A v2 record's projection is IGNORED even when
 * an older v2 store carries one: the trade-off is that a v2
 * store whose edge record went missing no longer gets a
 * synthesized edge, while a v1 store is unaffected and still
 * migrates. This ordering is what makes a dependency
 * cycle (A<->B) harmless and a missing guid harmless: an
 * edge is never the reason a vertex fails to load. A
 * dependency that cannot be resolved (missing target, self
 * edge, refused edge) is counted in
 * GraphSerializeStats::skipped_edge_count and reported on
 * stderr; the load continues.
 * (The next two epoch notes describe the PRE-edge-record link pass, when a vertex
 * record's dependency list was the only input: this build adopts each "e/" record
 * one by one through Graph::adopt_edge, so it reads no degree count from a record
 * and neither Graph::reserve_links nor Graph::connect_bulk is part of the load any
 * more — they still serve the legacy connect path.)
 * EPOCH 2 runs it in two sweeps so the four per-vertex edge
 * containers are sized once from the degree counts the
 * record already carries (Graph::reserve_links): this pass
 * was 87 % of a 100k load, and roughly half of that was the
 * geometric growth of those containers. The reservation
 * only ever raises capacity, so it cannot add, drop or
 * reorder a single edge.
 *                       EPOCH 3 builds the whole edge list and hands it to ONE
 *                       Graph::connect_bulk call: every distinct endpoint is
 *                       resolved once (one shard shared lock per unique endpoint,
 *                       instead of one Graph::find per edge), every touched shard's
 *                       unique lock is taken once for the batch in ascending order,
 *                       and the single-writer check runs once. Semantics are
 *                       identical to the per-edge Graph::connect loop it replaced —
 *                       same shared _connect_resolved_locked body, same duplicate
 *                       probe, same order, same skipped-edge arithmetic (each of the
 *                       n>created outcomes from EdgeOutcome is one skip) — and the
 *                       edge phase of a 100k load went 183 ms -> ~110 ms.
 *
 *   Corrupt record (bad magic, truncated, impossible dependency count, or a
 *   record whose embedded guid disagrees with its key) => the vertex is skipped,
 *   warned about, counted in skipped_count, and the rest of the store loads.
 *
 * Loaded vertices end up CLEAN, not dirty: their content and edges are exactly
 * what the store holds, so an immediate save_incremental() writes no vertex record
 * — and for a v2 store nothing at all, since every edge adopted from an "e/" record
 * is cleared before adoption. The one deliberate exception is a v1 load: an edge
 * synthesized from a v1 dependency list stays DIRTY, because a v1 store has no
 * "e/" record for it and this build no longer writes a projection a later load
 * could rebuild it from — so the next save persists it once as a real edge record,
 * and the save after that writes nothing. A loaded vertex also has its content hash
 * recomputed on materialization (the Vertex constructor always hashes), so a store
 * with a stale stored hash is repaired rather than propagated.
 *
 *   Edges resolve against the LOADED vertices plus whatever the target graph
 *   already holds: a dependency that names a guid present in the graph resolves
 *   to it, so a partially loaded store still wires into an existing graph. Load
 *   is not a MERGE, though: a record whose guid is already present in the target
 *   graph is skipped (counted, warned) instead of overwriting the live vertex,
 *   and any edge pointing at a guid that exists in neither place is skipped and
 *   counted in skipped_edge_count.
 *
 *   load() performs structural writes, so it must run on the graph's writer
 *   thread (graph.h's write policy) and on a graph no other thread is touching.
 *
 * FIBER NOTES
 *   Every fan-out is kimix::fiber::parallel over JOB IDS (0 .. slot_count) with an
 *   explicit per-claim size and task_limit = the ambient pool width capped at 8, so
 *   the serializer never claims more workers than it can use; each job fills the
 *   pre-sized slot its id names, so no two jobs touch the same memory. On a thread
 *   with no scheduler bound (worker_thread_count() == 1) parallel() degrades to
 *   running the bodies inline — the results and the order are identical, which is
 *   why the load is deterministic either way. Job bodies never block, never assert
 *   and never print: warnings are emitted single-threaded after the parallel pass,
 *   so stderr stays readable. The blocking LMDB batch runs outside every fiber.
 *
 * Example:
 *   kimix::graph_db::ColdStorage cold;
 *   cold.open(kimix::graph_db::default_cold_dir());
 *   kimix::graph_db::GraphSerializer serializer{cold};
 *   kimix::graph_db::GraphSerializeStats saved = serializer.save_incremental(graph);
 *   serializer.copy_to(checkpoint_dir);          // optional checkpoint
 *
 *   kimix::graph_db::Graph restored;
 *   kimix::graph_db::GraphDeserializer deserializer;
 *   kimix::graph_db::GraphSerializeStats loaded = deserializer.load(cold, restored);
 *   std::printf("%zu saved, %zu loaded, %zu skipped, %zu bad edges\n",
 *               saved.saved_count, loaded.loaded_count, loaded.skipped_count,
 *               loaded.skipped_edge_count);
 */
#pragma once

#include "cold_store.h"
#include "edge.h"
#include "graph_db_api.h"
#include "graph.h"
#include "schema.h"
#include "status.h"
#include "utils.h"
#include "vertex.h"

#include <core/guid.h>
#include <core/memory.h>
#include <core/md5.h>
#include <core/stl/filesystem.h>
#include <core/stl/memory.h>
#include <core/stl/vector.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdio>

namespace kimix::graph_db {

/*
 * Outcome of one save or one load. Counts are exact; the timings are wall-clock
 * milliseconds of the whole call (fan-out included) and are informational only —
 * nothing in the library or the tests asserts on them.
 */
struct KIMIX_GRAPH_DB_API GraphSerializeStats {
    // Records successfully written by this save.
    size_t saved_count{0};
    // Vertices adopted into the target graph by this load.
    size_t loaded_count{0};
    // Records dropped instead of materialized: corrupt value, or a guid that is
    // already present in the target graph.
    size_t skipped_count{0};
    // Dependency edges that could not be resolved: missing target vertex, self
    // edge, or an edge Graph::connect refused.
    size_t skipped_edge_count{0};
    // Bytes of encoded records handed to the LMDB batch by this save.
    size_t bytes_written{0};
    // Bytes of scanned record values read from the store by this load (corrupt
    // ones included, since they were read before being rejected).
    size_t bytes_read{0};
    // Fan-out width used by the fiber parallel stages (1 == ran inline because
    // the calling thread had no scheduler bound).
    size_t worker_count{0};
    // Whole-call wall-clock time.
    double save_ms{0.0};
    double load_ms{0.0};

    // --- format negotiation (P0) -------------------------------------------
    // Outcome of the call as a status code. `ok` for every ordinary save/load;
    // error_bad_file when the store's meta record announces a format this build
    // cannot read (nothing is materialized in that case — a partial load of a
    // newer store would be worse than a refusal);
    // error_io when the storage is not open, and — for a save — when the backend
    // REFUSED the write batch (a store closed mid-save, a key it will not store):
    // such a save wrote nothing, cleared no dirty flag and left the pending meta
    // record queued for the next save.
    GdiStatus status{GdiStatus::ok};
    // Format version this load/write saw: 1 when the store carried no meta
    // record (a v1 store), kFormatVersion for a store this build wrote.
    uint32_t format_version{0};
    // True when a load read a v1 store and migrated it in memory: the vertices
    // come from "GDB1" records with no key, no labels, no properties, and their
    // dependency guids became edges with synthesized, deterministic ids.
    bool migrated_from_v1{false};
    // Edges materialized from a v1 dependency list (no edge record existed). They
    // are adopted DIRTY, so the next save writes them as real "e/" records once.
    size_t synthesized_edge_count{0};
    // Schema tables restored by this load (0 for a v1 store).
    size_t restored_label_count{0};
    size_t restored_property_type_count{0};
    // Edges written / loaded by this call (a save writes the dirty ones).
    size_t saved_edge_count{0};
    size_t loaded_edge_count{0};
    // Tombstones written by this save: an entity that was deleted since the last
    // save is stored as an EMPTY value under its key (a save is a write
    // transaction and cannot remove a key), and the loader treats an empty value
    // as "gone". Without them a reload would resurrect the deleted entity.
    size_t deleted_vertex_count{0};
    size_t deleted_edge_count{0};
    // Tombstones read (and skipped) by this load.
    size_t tombstone_count{0};
};

/*
 * Writes a Graph into a cold store: parallel encode (stage A), one atomic store
 * transaction (stage B). The store is referenced, not owned — it must outlive
 * the serializer. A concrete ColdStorage converts to the ColdStore seam
 * implicitly, so every existing call site keeps its spelling.
 */
class KIMIX_GRAPH_DB_API GraphSerializer : public kimix::IOperatorNewBase {
public:
    explicit GraphSerializer(ColdStore cold) noexcept
        : _cold(cold) {
    }
    GraphSerializer(GraphSerializer const &) = delete;
    GraphSerializer &operator=(GraphSerializer const &) = delete;
    GraphSerializer(GraphSerializer &&) = delete;
    GraphSerializer &operator=(GraphSerializer &&) = delete;
    ~GraphSerializer() noexcept = default;

    // True when the referenced cold store is open; every save returns empty
    // stats otherwise.
    [[nodiscard]] bool valid() const noexcept { return _cold.valid(); }

    /*
     * FULL save: mark every vertex dirty, then encode and write every vertex in
     * one transaction. Use it for a first save or a checkpoint. Records of
     * vertices that were removed from the graph are left in the store (see the
     * file header). Returns the stats of this call (also kept in last_stats()).
     */
    GraphSerializeStats save(Graph &graph);

    /*
     * INCREMENTAL save: encode and write ONLY the vertices whose dirty flag is
     * set. A clean vertex is never re-encoded and never rewritten, so the bytes
     * already in the store stay untouched. Returns the stats of this call.
     */
    GraphSerializeStats save_incremental(Graph &graph);

    // The schema whose labels / property types are written alongside the records
    // (borrowed; may be null, in which case the two "s/" tables are not written).
    // A full save always writes them; an incremental save only when the schema's
    // version differs from the one in the store's meta record.
    void set_schema(Schema *schema) noexcept { _schema = schema; }
    [[nodiscard]] Schema *schema() const noexcept { return _schema; }

    // The metadata a save writes into "meta/root" (format version, store uuid,
    // timestamps, schema version, id counters, graph name). A caller that owns a
    // Schema hands its counters over here so a reload continues the id spaces;
    // the Database does. When no meta has been set the serializer writes a
    // default one (fresh store uuid, kFormatVersion) on its first save that has
    // anything to write, so a store is never left half-versioned.
    void set_meta(MetaRecord const &meta) { _meta = meta; }
    [[nodiscard]] MetaRecord const &meta() const noexcept { return _meta; }

    // Compacted snapshot of the whole store into `target` (checkpoint).
    [[nodiscard]] bool copy_to(kimix::filesystem::path const &target) const noexcept {
        return _cold.copy_to(target);
    }

    [[nodiscard]] GraphSerializeStats const &last_stats() const noexcept {
        return _last_stats;
    }

#ifndef NDEBUG
    /*
     * TEST SEAM (DEBUG ONLY — compiled out of release builds, see AGENTS.md's
     * "debugging code" rule): make the NEXT save batch behave exactly as if the
     * backend REFUSED it, without touching a real store. The refusal path is
     * otherwise unreachable — kimix::LMDB aborts on a genuine failure and the seam
     * reports "not open" before that — so this is the only way to prove what a
     * refused batch leaves behind: nothing written, no dirty flag cleared and the
     * deletion tombstones still queued for the next save.
     *
     * Armed for ONE call and consumed by it (a test arms, saves, then asserts). Never
     * call from production code.
     */
    void test_fail_next_batch(bool fail) noexcept {
        _test_fail_next_batch.store(fail, std::memory_order_release);
    }
#endif // NDEBUG

private:
    // The shared pipeline behind save() and save_incremental(). `full` decides
    // whether the meta record is written even when nothing else is.
    GraphSerializeStats _write(Graph &graph, bool full);

    // ONE batch through the seam. Factored out so BOTH write sites (the
    // nothing-dirty stamp and the record batch) share the refusal handling and the
    // debug injection point.
    [[nodiscard]] bool _save_batch(kimix::vector<kimix::LMDBWriteCommand> &&commands) noexcept;

    // A non-owning handle over the backend (cold_store.h): it refers to the
    // caller's store, so the serializer never copies, closes or moves it.
    ColdStore _cold;
    Schema *_schema{nullptr};
    MetaRecord _meta{};
    // The meta record of the call in flight (written in the same transaction as
    // the records it describes) and whether it still has to go out. ATOMIC: it is
    // save-path state a caller may inspect/drive from several threads, and a plain
    // bool there is a formal data race (see the dirty-flag protocol in graph.h).
    MetaRecord _pending_meta{};
    std::atomic<bool> _meta_dirty{false};
#ifndef NDEBUG
    // See test_fail_next_batch(). Atomic so arming it cannot race the save itself.
    std::atomic<bool> _test_fail_next_batch{false};
#endif // NDEBUG
    GraphSerializeStats _last_stats{};
};

/*
 * Reads a cold store into a Graph. Stateless: the store is passed per call, so
 * one deserializer may load several stores (or the same store into several
 * graphs) without any reset between calls.
 */
class KIMIX_GRAPH_DB_API GraphDeserializer : public kimix::IOperatorNewBase {
public:
    GraphDeserializer() noexcept = default;
    GraphDeserializer(GraphDeserializer const &) = delete;
    GraphDeserializer &operator=(GraphDeserializer const &) = delete;
    GraphDeserializer(GraphDeserializer &&) = delete;
    GraphDeserializer &operator=(GraphDeserializer &&) = delete;
    ~GraphDeserializer() noexcept = default;

    /*
     * Dual-stage load of every "v/" record of `cold` into `graph` (see the file
     * header for the stage split and the guarantees). Returns the stats of this
     * call (also kept in last_stats()). Never fails because of a dependency
     * problem and never fails because of a corrupt record: both are skipped and
     * counted. It returns empty stats only when the store is not open.
     */
    // `schema` (optional) receives the store's label / property-type tables, so a
    // reload keeps its names and its id spaces. Without one the ids in the
    // records are still restored verbatim (they are just un-nameable).
    GraphSerializeStats load(ColdStore cold, Graph &graph, Schema *schema = nullptr);

    [[nodiscard]] GraphSerializeStats const &last_stats() const noexcept {
        return _last_stats;
    }

private:
    GraphSerializeStats _last_stats{};
};

} // namespace kimix::graph_db