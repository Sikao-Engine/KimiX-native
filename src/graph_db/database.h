/*
 * database.h — kimix::graph_db::Database: the GDI-shaped backend facade.
 *
 * One Database owns one in-memory Graph, one Schema, one ColdStorage and one
 * GraphOptions set, and exposes the GDI command vocabulary as
 * GdiStatus-returning methods. It is the type a Semevia.Gdi.Native adapter binds
 * to (the C ABI in graph_c_api.h is a thin wrapper over exactly this class), and
 * it is what the report's §2.16 "session / multi-graph object" asks for: a named
 * graph catalog slots in here later without changing this surface.
 *
 * WHY A SEPARATE FACADE: Graph stays the container (sharded vertex map, edge
 * table, key index, traversals, its own lock contract). Everything that needs
 * the SCHEMA — name interning, declaration checks, error semantics, the
 * (label, external_id) uniqueness policy, filtered reads, transactions — lives
 * here, because it is policy, not storage. That keeps graph.h reviewable and
 * makes the GDI error surface exist in exactly one place.
 *
 * IDs VS NAMES: the entity-level commands take INTERNED IDS (LabelId /
 * PropNameId) because that is what an entity stores and it keeps a write
 * allocation-free; names are resolved once, through the schema
 * (Schema::resolve_label / resolve_property_type), by whoever has the names in
 * hand — the C ABI does, so the managed side keeps speaking names. Read-backs
 * return NAMES (the schema owns the name table), which is what a caller sees.
 *
 * ERROR SEMANTICS are copied from Semevia.Gdi's in-memory reference backend
 * (Backends/InMemory/InMemoryGdiBackend.cs), one method at a time:
 *   create_vertex    ErrorEmptyName / ErrorNonUniqueId (or WarningNonUniqueId +
 *                    the existing uid in warn mode) / ErrorLabel / ErrorPropertyType
 *   get_or_create    match returns the uid UNTOUCHED (created=false)
 *   translate        ok + found=false for a miss (never an error)
 *   free_vertex      ErrorVertex when unknown; guarded also ErrorEdge
 *   add_property     ErrorPropertyExists / ErrorPropertyType / ErrorWrongType
 *   set_property     ErrorNoProperty (+ the same type checks)
 *   update_property  UPSERT, no error for a miss
 *   remove_props     absent names ignored
 *   add_label        ErrorLabel when already present or undeclared
 *   remove_label     ErrorLabel when absent
 *   create_edge      ErrorVertex / ErrorEdge (self-loop when forbidden)
 *   get_edges        ErrorArgument for cap < 0; cap == 0 is the count probe
 *   transactions     ErrorTransaction / ErrorReadOnlyTransaction / ErrorSizeLimit
 *
 * PERSISTENCE is the save/dirty model of serialize.h, not a per-commit fsync:
 * persist(full=false) writes exactly the dirty records. Transaction commit only
 * persists when GraphOptions::persist_on_commit is set (a GDI caller that wants
 * durability per commit asks for it explicitly).
 *
 * Example:
 *   kimix::graph_db::Database db;
 *   db.open("graph.cold", kimix::graph_db::GraphOptions{});
 *   kimix::graph_db::LabelId person{};
 *   db.schema().create_label("person", person);
 *   kimix::graph_db::Database::CreateResult created;
 *   db.create_vertex(kimix::graph_db::VertexKeyView{person, id_bytes}, {}, nullptr, created);
 *   db.persist(false);   // incremental save
 */
#pragma once

#include "capability.h"
#include "cold_store.h"
#include "edge.h"
#include "file_cold_store.h"
#include "graph.h"
#include "graph_db_api.h"
#include "graph_db_options.h"
#include "key.h"
#include "property.h"
#include "query.h"
#include "schema.h"
#include "serialize.h"
#include "status.h"
#include "utils.h"

#include <core/guid.h>
#include <core/memory.h>
#include <core/stl/filesystem.h>
#include <core/stl/optional.h>
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <cstddef>
#include <cstdint>

namespace kimix::graph_db {

class Transaction;

// One typed property with its NAME resolved (what a read-back returns, and what
// a write takes when the caller already has the name).
struct KIMIX_GRAPH_DB_API NamedProperty {
    kimix::string name;
    PropertyValue value;
};
using NamedPropertyList = kimix::vector<NamedProperty>;

// Result of create_vertex: GDI's GdiVertexCreateResult.
struct KIMIX_GRAPH_DB_API VertexCreateResult {
    kimix::Guid uid;
    // GDI's NonUniqueIdWarning: set only on the warning_non_unique_id path, where
    // `uid` is the EXISTING vertex (GraphOptions::key_mode == report_warning).
    bool non_unique_id_warning{false};
};

// Result of get_or_create_vertex: GDI's GdiVertexMergeResult.
struct KIMIX_GRAPH_DB_API VertexMergeResult {
    kimix::Guid uid;
    bool created{false};
};

// One row of create_vertices_bulk. The key owns its id bytes, so a caller may
// build the rows and let the values go out of scope.
struct KIMIX_GRAPH_DB_API VertexBulkRow {
    VertexKey key;
    kimix::vector<LabelId> labels;
    PropertyList properties;
    kimix::span<const std::byte> content;
};

// One row of create_edges_bulk.
struct KIMIX_GRAPH_DB_API EdgeBulkRow {
    kimix::Guid origin;
    kimix::Guid target;
    EdgeDirectionType direction{EdgeDirectionType::directed};
    kimix::vector<LabelId> labels;
    PropertyList properties;
};

// What a bulk call did (GDI's failed_rows / counts in one value).
struct KIMIX_GRAPH_DB_API BulkStats {
    size_t created{0};
    size_t failed{0};
    size_t warning{0};
};

// A label / property type as a caller sees it (name + its interned id).
struct KIMIX_GRAPH_DB_API NamedLabel {
    LabelId id{kNoLabel};
    kimix::string name;
};
using NamedLabelList = kimix::vector<NamedLabel>;

/*
 * The GDI backend. Single-writer by contract (Graph's rule, surfaced as
 * error_state on a refused write); any number of threads may read concurrently.
 * One Database per engine worker, like Semevia.Gdi's IGraphBackend.
 */
class KIMIX_GRAPH_DB_API Database : public kimix::IOperatorNewBase {
public:
    Database() noexcept;
    explicit Database(GraphOptions const &options) noexcept;
    Database(Database const &) = delete;
    Database &operator=(Database const &) = delete;
    Database(Database &&) = delete;
    Database &operator=(Database &&) = delete;
    ~Database() noexcept;

    // --- store lifecycle ----------------------------------------------------
    // Open (and create) the cold store in `dir` and load what it holds into this
    // database's graph. An empty path keeps the database purely in memory
    // (persist() then returns error_no_such_file). error_bad_file when the store
    // announces a newer format, error_state when it is already open.
    [[nodiscard]] GdiStatus open(kimix::filesystem::path const &dir,
                                 GraphOptions const &options);
    [[nodiscard]] GdiStatus open(kimix::filesystem::path const &dir);
    void close() noexcept;
    // True while a store is open, whichever backend GraphOptions::store_backend
    // selected for it.
    [[nodiscard]] bool is_open() const noexcept { return _store.valid(); }
    // Write the graph into the store: full (every record) or incremental (only
    // the dirty ones). error_io when no store is open.
    [[nodiscard]] GdiStatus persist(bool full, GraphSerializeStats *out = nullptr);
    // Re-read the store into the (already open) database.
    [[nodiscard]] GdiStatus load_store(GraphSerializeStats *out = nullptr);
    [[nodiscard]] GraphSerializeStats const &last_stats() const noexcept { return _last_stats; }

    [[nodiscard]] Graph &graph() noexcept { return _graph; }
    [[nodiscard]] Graph const &graph() const noexcept { return _graph; }
    [[nodiscard]] Schema &schema() noexcept { return _schema; }
    [[nodiscard]] Schema const &schema() const noexcept { return _schema; }
    // The LMDB backend instance — CLOSED (and unused) while the flat_file
    // backend is the live one. Kept as the typed escape hatch for the LMDB
    // store; a caller that needs the file store asks for its backend through
    // GraphOptions instead.
    [[nodiscard]] ColdStorage &storage() noexcept { return _cold; }
    // Which backend the currently open store is (lmdb while closed/in-memory).
    [[nodiscard]] ColdStoreBackend store_backend() const noexcept { return _backend; }
    [[nodiscard]] GraphOptions const &options() const noexcept { return _options; }
    void set_options(GraphOptions const &options);
    [[nodiscard]] Capability capabilities() const noexcept { return current_capability(); }

    // --- vertices (§2.3, §2.8, §2.10) ---------------------------------------
    // Create a keyed (or, with key_label == kNoLabel, an unkeyed) vertex.
    // `labels` and `properties` are interned ids; `key_label` must be registered.
    [[nodiscard]] GdiStatus create_vertex(VertexKeyView key, kimix::span<const LabelId> labels,
                                          PropertyList const *properties,
                                          VertexCreateResult &out);
    [[nodiscard]] GdiStatus get_or_create_vertex(VertexKeyView key,
                                                 kimix::span<const LabelId> labels,
                                                 PropertyList const *properties,
                                                 VertexMergeResult &out);
    // Key -> uid. ok + found=false for a miss (never an error).
    [[nodiscard]] GdiStatus translate_vertex_id(VertexKeyView key, kimix::Guid &out_uid,
                                                bool &found) const;
    // Cascading delete (GDI FreeVertex): every incident edge goes with it.
    [[nodiscard]] GdiStatus free_vertex(kimix::Guid const &uid);
    // Guarded delete (GDI FreeVertexGuarded): error_edge while any edge exists.
    [[nodiscard]] GdiStatus free_vertex_guarded(kimix::Guid const &uid);
    // N keyed creates in ONE transaction. continue_on_error=false makes the batch
    // all-or-nothing (the first failure aborts it and the remaining rows are
    // marked error_state); true isolates the rows (per-row statuses, matching
    // CreateVerticesBulkAsync + failed_rows).
    [[nodiscard]] GdiStatus create_vertices_bulk(kimix::span<VertexBulkRow const> rows,
                                                bool continue_on_error,
                                                kimix::vector<VertexCreateResult> &results,
                                                kimix::vector<GdiStatus> *row_status,
                                                BulkStats &stats);
    [[nodiscard]] GdiStatus create_edges_bulk(kimix::span<EdgeBulkRow const> rows,
                                              bool continue_on_error,
                                              kimix::vector<kimix::Guid> &edge_ids,
                                              kimix::vector<GdiStatus> *row_status,
                                              BulkStats &stats);

    // --- vertex labels (§2.2) ----------------------------------------------
    [[nodiscard]] GdiStatus add_label(kimix::Guid const &uid, LabelId label);
    [[nodiscard]] GdiStatus remove_label(kimix::Guid const &uid, LabelId label);
    // Labels of a vertex, as NAMES: the key label first, then the rest (the
    // reference backend's GetLabelsOfVertex order).
    [[nodiscard]] GdiStatus get_labels(kimix::Guid const &uid,
                                       kimix::vector<kimix::string> &out) const;

    // --- vertex properties (§2.1) ------------------------------------------
    [[nodiscard]] GdiStatus add_property(kimix::Guid const &uid, PropNameId name,
                                         PropertyValue value);
    [[nodiscard]] GdiStatus set_property(kimix::Guid const &uid, PropNameId name,
                                         PropertyValue value);
    [[nodiscard]] GdiStatus update_property(kimix::Guid const &uid, PropNameId name,
                                            PropertyValue value);
    [[nodiscard]] GdiStatus remove_properties(kimix::Guid const &uid,
                                              kimix::span<const PropNameId> names);
    [[nodiscard]] GdiStatus get_properties(kimix::Guid const &uid,
                                           NamedPropertyList &out) const;
    // The declarations of the properties a vertex actually carries, sorted by name.
    [[nodiscard]] GdiStatus get_property_types(kimix::Guid const &uid,
                                               PropertyTypeList &out) const;

    // --- edges (§2.4, §2.14) ------------------------------------------------
    [[nodiscard]] GdiStatus edge_create(kimix::Guid const &origin, kimix::Guid const &target,
                                        EdgeDirectionType direction,
                                        kimix::span<const LabelId> labels,
                                        PropertyList const *properties, kimix::Guid &out_edge);
    [[nodiscard]] GdiStatus edge_free(kimix::Guid const &edge);
    [[nodiscard]] GdiStatus set_edge_origin(kimix::Guid const &edge, kimix::Guid const &origin);
    [[nodiscard]] GdiStatus set_edge_target(kimix::Guid const &edge, kimix::Guid const &target);
    [[nodiscard]] GdiStatus set_edge_direction(kimix::Guid const &edge,
                                               EdgeDirectionType direction);
    [[nodiscard]] GdiStatus get_edge_endpoints(kimix::Guid const &edge, kimix::Guid &origin,
                                               kimix::Guid &target) const;
    [[nodiscard]] GdiStatus add_edge_label(kimix::Guid const &edge, LabelId label);
    [[nodiscard]] GdiStatus remove_edge_label(kimix::Guid const &edge, LabelId label);
    [[nodiscard]] GdiStatus get_edge_labels(kimix::Guid const &edge,
                                            kimix::vector<kimix::string> &out) const;
    [[nodiscard]] GdiStatus add_edge_property(kimix::Guid const &edge, PropNameId name,
                                              PropertyValue value);
    [[nodiscard]] GdiStatus set_edge_property(kimix::Guid const &edge, PropNameId name,
                                              PropertyValue value);
    [[nodiscard]] GdiStatus update_edge_property(kimix::Guid const &edge, PropNameId name,
                                                 PropertyValue value);
    [[nodiscard]] GdiStatus remove_edge_properties(kimix::Guid const &edge,
                                                   kimix::span<const PropNameId> names);
    [[nodiscard]] GdiStatus get_edge_properties(kimix::Guid const &edge,
                                                NamedPropertyList &out) const;
    [[nodiscard]] GdiStatus get_edge_property_types(kimix::Guid const &edge,
                                                    PropertyTypeList &out) const;

    // --- filtered, oriented, paged reads (§2.6) -----------------------------
    // `constraint` may be an empty Constraint (matches everything) or a registered
    // handle. Edge reads filter the EDGES (their labels/properties); neighbour
    // reads filter the NEIGHBOUR VERTICES (GDI §5.10). cap == 0 is the count
    // probe: no items, the real total_count. cap < 0 is error_argument.
    [[nodiscard]] GdiStatus get_edges_of_vertex(kimix::Guid const &uid,
                                                Constraint const &constraint,
                                                EdgeOrientation orientation, int64_t cap,
                                                EdgePage &out) const;
    [[nodiscard]] GdiStatus get_neighbor_vertices(kimix::Guid const &uid,
                                                  Constraint const &constraint,
                                                  EdgeOrientation orientation, int64_t cap,
                                                  VertexPage &out) const;
    // Exact count (no cap). ExistsNeighbor == count_neighbors(...) > 0 with a
    // cap-1 short circuit.
    [[nodiscard]] GdiStatus count_neighbors(kimix::Guid const &uid,
                                            Constraint const &constraint,
                                            EdgeOrientation orientation, size_t &out) const;
    [[nodiscard]] GdiStatus exists_neighbor(kimix::Guid const &uid,
                                            Constraint const &constraint,
                                            EdgeOrientation orientation, bool &out) const;

      // --- native multi-hop traversal (§5.12 composites — the G3 capability) ---
      // One native call for the whole level-by-level walk the engine would
      // otherwise compose from per-hop reads.  `constraint` is an empty
      // Constraint or a registered handle, resolved exactly like the read
      // commands; a non-empty one filters every discovered vertex (§5.10), so
      // a filtered-out vertex's neighbourhood is never visited.  Read-only:
      // usable inside any transaction scope, journaled by nothing.
      // See Graph::traverse_bfs for the cap/depth/error contract.
      [[nodiscard]] GdiStatus traverse_bfs(kimix::Guid const &start, Constraint const &constraint,
                                           EdgeOrientation orientation, int64_t cap_per_level,
                                           uint32_t max_depth,
                                           kimix::vector<Graph::TraversalVertex> &out) const;
      // Bounded shortest path along outgoing edges (the SHORTEST_PATH composite).
      // Empty result + ok = unreachable within `max_depth`; error_vertex for an
      // unknown endpoint; error_argument for a bound outside 1..kMaxNativeHopDepth.
      [[nodiscard]] GdiStatus shortest_path(kimix::Guid const &from, kimix::Guid const &to,
                                            uint32_t max_depth,
                                            kimix::vector<kimix::Guid> &out) const;
      // EDGE-filtered forms of the two above (the EDGE half of §5.10: the walk
      // keeps a hop only when the EDGE it steps over satisfies `edge_constraint`,
      // §2.6 semantics — the edge's own labels AND-ed with its properties).
      // `edge_constraint` is an empty Constraint (every edge) or a registered
      // handle, resolved and validated exactly like the read commands (a label
      // condition with an ordering op is error_op). `orientation` is the direction
      // the walk follows; `out[i].edge` names the edge each hop traversed, which
      // is what a vertex-only path cannot report. Same status/result contract as
      // the vertex-only bounded form.
      [[nodiscard]] GdiStatus shortest_path(kimix::Guid const &from, kimix::Guid const &to,
                                            Constraint const &edge_constraint,
                                            EdgeOrientation orientation, uint32_t max_depth,
                                            kimix::vector<Graph::PathStep> &out) const;
      // Edge-filtered reachability, bounded by Graph::kMaxNativeHopDepth. `out` is
      // false for an unknown endpoint (never an error) and for a walk that finds
      // no matching chain of edges.
      [[nodiscard]] GdiStatus has_path(kimix::Guid const &from, kimix::Guid const &to,
                                       Constraint const &edge_constraint,
                                       EdgeOrientation orientation, bool &out) const;

      // EDGE-filtered BFS (plan §1.3, the ABI-7 `traverse_bfs_edges`): the whole
      // readable neighbourhood of `start` in ONE call — every discovered vertex with its
      // discovering edge and depth, EVERY constraint-satisfying edge whose two endpoints
      // are both in that set (the sweep that makes a spanning tree's omissions
      // impossible), and the truncation flag. `edge_constraint` is an empty Constraint
      // (every edge) or a registered handle, resolved and validated exactly like the
      // reads above (a label condition with an ordering op is error_op, and an unknown
      // handle reaches this call only as error_constraint from the C ABI layer).
      // `max_depth` bounds the hop count (1..Graph::kMaxNativeHopDepth) and
      // `max_vertices` the discovered set; the cap never partially expands a level, so
      // `out.truncated` is true exactly when the cap refused a hop that had passed both
      // its tests. See Graph::traverse_bfs_edges for the hop rule, the sweep's
      // O(V * degree) cost and the full error table. `out` is a REUSABLE result record:
      // both vectors are cleared and refilled in place, so a caller that hoists one
      // across calls (the C ABI keeps it in the session) allocates nothing on a warm
      // read path.
      [[nodiscard]] GdiStatus traverse_bfs_edges(kimix::Guid const &start,
                                                 Constraint const &edge_constraint,
                                                 EdgeOrientation orientation,
                                                 uint32_t max_depth, uint32_t max_vertices,
                                                 Graph::BfsEdgesResult &out) const;

      // --- global scan (the G1 capability) ------------------------------------
      // One native pass over EVERY vertex (Graph::for_each_vertex: shard 0..63
      // ascending, one shard SHARED lock at a time, map order within a shard),
      // filtered by `constraint` exactly like a neighbourhood read filters the
      // neighbour vertex (§5.10: the scanned vertex itself, labels AND-ed with
      // properties). `constraint` is an empty Constraint (match everything) or
      // a registered handle, resolved like the read commands. cap == 0 is the
      // count probe (no items, real total_count); cap < 0 is error_argument.
      // Deterministic: the result order is shard order then map order, so the
      // same graph yields the same page regardless of scheduling. Read-only.
      [[nodiscard]] GdiStatus scan_vertices(Constraint const &constraint, int64_t cap,
                                            VertexPage &out) const;

      // --- global aggregates (the G4 capability) ------------------------------
      // One native pass reducing one property over every vertex matching
      // `spec.constraint` (empty = all). The fan-out runs one fiber per shard
      // over the shard ranges (SHARED locks only), and the per-shard partials
      // merge in SHARD ORDER, so the float sums and the min/max tie-breaks are
      // bit-identical to the serial scan regardless of scheduling; the parallel
      // path needs a bound fiber scheduler, and the unbound fast path runs the
      // same shards inline (C-ABI safe: the call blocks either way). See
      // AggregateSpec/AggregateResult in query.h for the legality matrix
      // (error_op / error_property_type are decided BEFORE the scan) and the
      // payload kinds. Read-only.
      [[nodiscard]] GdiStatus aggregate_vertices(AggregateSpec const &spec,
                                                 AggregateResult &out) const;

    // --- constraints (§2.5) -------------------------------------------------
    [[nodiscard]] GdiStatus create_constraint(Constraint const &definition,
                                              ConstraintHandle &out);
    [[nodiscard]] GdiStatus add_label_condition(ConstraintHandle handle, LabelId label, GdiOp op);
    [[nodiscard]] GdiStatus add_property_condition(ConstraintHandle handle, PropNameId property,
                                                   GdiOp op, PropertyValue value,
                                                   uint32_t count = 1);
    [[nodiscard]] GdiStatus free_constraint(ConstraintHandle handle);

    // --- schema commands (§2.5) --------------------------------------------
    [[nodiscard]] GdiStatus create_label(kimix::string_view name, LabelId &out);
    [[nodiscard]] GdiStatus get_label_by_name(kimix::string_view name, LabelId &out,
                                              bool &found) const;
    [[nodiscard]] GdiStatus get_all_labels(NamedLabelList &out) const;
    // Rename. Entities are untouched (they store ids), so this is O(1).
    [[nodiscard]] GdiStatus update_label(LabelId id, kimix::string_view new_name);
    // Unregister AND strip the label from every vertex and edge (GDI FreeLabel).
    // Refuses (error_state) the key label of a live vertex: freeing it would
    // orphan the key index entry.
    [[nodiscard]] GdiStatus free_label(LabelId id);

    [[nodiscard]] GdiStatus create_property_type(PropertyTypeInfo const &info, PropNameId &out);
    [[nodiscard]] GdiStatus get_property_type_by_name(kimix::string_view name, PropNameId &out,
                                                      bool &found) const;
    [[nodiscard]] GdiStatus get_all_property_types(PropertyTypeList &out) const;
    [[nodiscard]] GdiStatus update_property_type(PropNameId id, PropertyTypeInfo const &info);
    // Unregister AND remove every value of that type from every vertex and edge.
    [[nodiscard]] GdiStatus free_property_type(PropNameId id);

    // --- transactions (§2.7) ------------------------------------------------
    // Auto-transaction: every command above applies immediately (the container's
    // own behaviour) and marks what it touched dirty.
    // An explicit transaction brackets several commands; abort() restores the
    // pre-begin state exactly. read_only (a GDI collective transaction) makes
    // every write return error_read_only_transaction.
    [[nodiscard]] GdiStatus begin_transaction(bool read_only = false);
    [[nodiscard]] GdiStatus commit();
    [[nodiscard]] GdiStatus abort();
    [[nodiscard]] GdiStatus begin_collective();
    [[nodiscard]] GdiStatus close_collective(bool commit_it);
    [[nodiscard]] bool in_transaction() const noexcept { return _tx != nullptr; }
    [[nodiscard]] bool transaction_is_read_only() const noexcept;

private:
    friend class Transaction;

    // --- internals ----------------------------------------------------------
    // Resolve a guid to a vertex, or error_vertex.
    [[nodiscard]] GdiStatus _require_vertex(kimix::Guid const &uid, Vertex *&out) const;
    // Validate a property VALUE against its declared type: error_property_type
    // when undeclared, error_wrong_type when the kind/container/dtype disagree,
    // error_count when an array's length does not match the declaration.
    [[nodiscard]] GdiStatus _validate_property(PropNameId name, PropertyValue const &value) const;
    // Resolve the registered constraint (or an empty one for a zero handle) and
    // hand it back BY REFERENCE: `out` is set to `&constraint` on success, so a
    // read pays no copy of the condition vector (plan O9), and the C ABI path —
    // whose handle is already resolved — pays nothing at all. The validation rules
    // are unchanged (a label condition with an ordering op is error_op) and `out`
    // is written only on success.
    [[nodiscard]] GdiStatus _resolve_constraint(Constraint const &constraint,
                                                Constraint const *&out) const;
    // True when any live entity carries a value of this property type.
    [[nodiscard]] bool _property_values_exist(PropNameId name) const;
    // Strip a label / property type from every vertex and every edge. When a
    // transaction is open each affected entity's previous payload is journaled
    // first, so an abort puts the values back (a cascade is undoable).
    void _strip_label(LabelId label);
    void _strip_property_type(PropNameId name);
    // Journal the current payload of a vertex / edge (the undo of any payload
    // edit). Returns false when the journal is full.
    bool _journal_vertex_payload(kimix::Guid const &uid);
    bool _journal_edge_state(kimix::Guid const &edge);
    // True when a live vertex uses `label` as its key label.
    [[nodiscard]] bool _label_is_a_key(LabelId label) const;
    // Replay a transaction's journal in reverse (the abort path, and the bulk
    // batch's all-or-nothing rollback). Consumes the journal.
    [[nodiscard]] GdiStatus _rollback(Transaction &tx);

    Graph _graph;
    Schema _schema;
    // The two cold-store backends, in declaration order so the handle below can
    // be initialised from _cold. Exactly ONE of them is open at a time; which one
    // is decided by GraphOptions::store_backend in open(), and both stay members
    // (never heap-allocated) so the non-owning handle always points at live
    // storage.
    ColdStorage _cold;
    FileColdStore _cold_file;
    // The seam handle over whichever backend is live (cold_store.h). open()
    // re-points it; the serializer below is rebuilt from it, because a ColdStore
    // is a copy of two pointers and re-pointing this one would not move the
    // serializer's.
    ColdStore _store;
    GraphOptions _options;
    // Built by open() from _store (GraphSerializer has no default constructor and
    // its backend is chosen at open time), reset by close(). Empty while the
    // database is closed or purely in-memory, which is exactly when persist() and
    // load_store() report error_no_such_file anyway.
    kimix::optional<GraphSerializer> _serializer;
    GraphDeserializer _deserializer;
    // The backend the live store is; lmdb while closed.
    ColdStoreBackend _backend{ColdStoreBackend::lmdb};
    GraphSerializeStats _last_stats{};
    // The one open transaction (nullptr outside one). Created and owned by
    // begin_transaction(); the Database only tracks it so a second begin is an
    // error and so a write can check the read-only flag.
    Transaction *_tx{nullptr};
};

} // namespace kimix::graph_db
