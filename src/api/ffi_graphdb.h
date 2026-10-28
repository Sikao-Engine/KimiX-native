/*
 * ffi_graphdb.h -- the kimix-graph-db C ABI (src/graph_db/graph_c_api.h),
 * re-exported so a foreign caller reaches the graph database from kimix_api.
 *
 * WHAT THIS FAMILY IS
 * -------------------
 * A pure RE-EXPORT: every kimix_graphdb_* entry point below has the signature
 * of its kimix_gdb_* twin in <graph_db/graph_c_api.h> -- same arguments, same
 * POD types, same return type -- and forwards to it one line at a time (see
 * api/ffi_graphdb.cpp).  The reason it exists: graph_c_api.cpp is compiled
 * into the STATIC archive kimix-graph-db with no export macro, so nothing of
 * it leaves a shared image; these wrappers put the house-conventional KIMIX_FFI
 * surface on kimix_api.dll itself, indexed in docs/ffi.md like every other
 * family, without changing src/graph_db in any way.
 *
 * The POD structs (kimix_gdb_guid, kimix_gdb_property, kimix_gdb_vertex_spec,
 * ...) are NOT re-declared here: this header includes <graph_db/graph_c_api.h>
 * and reuses its declarations, so the layout a caller sees is the one the
 * archive was compiled against.  Names keep the `kimix_gdb_` prefix on the
 * types and macros (KIMIX_GDB_OK, ...) and take the `kimix_graphdb_` prefix on
 * the functions -- one prefix per feature area, exactly like the rest of the
 * surface (api/ffi_common.h, contract 2).
 *
 * STATUS CONTRACT
 * ---------------
 * Every fallible entry point returns `int32_t`, one of the KIMIX_GDB_* status
 * codes -- NOT the kimix_status enum of the other areas.  The numbers are the
 * GDI status codes: 0 ok, 1..2 warnings, 3..62 errors, numerically identical
 * to kimix::graph_db::GdiStatus and to the managed Semevia.Gdi.GdiError, so a
 * binding switches on the same integers on both sides of its own seam.
 * KIMIX_GDB_OK is 0, so `if (kimix_graphdb_...(...))` reads as a failure.
 * An out-parameter is written only on OK (or on a warning); a failed call
 * leaves the caller's memory untouched.  A NULL session or a NULL required
 * pointer is answered with KIMIX_GDB_ERR_ARGUMENT -- never with undefined
 * behaviour.
 *
 * TWO-PHASE, CALLER-OWNED BUFFERS
 * -------------------------------
 * There is NO library allocation on read paths.  Every variable-length result
 * follows one protocol: call once with the array pointer NULL (and/or the
 * buffer length 0) to learn the required counts through the `out_*`
 * parameters, then call again with caller-owned storage.  A buffer that is
 * too small returns KIMIX_GDB_ERR_BUFFER and still reports the required
 * sizes.  Variable-length payloads (property values, schema names) land in a
 * caller-provided SCRATCH buffer; each record carries (offset, length) into
 * it.
 *
 * OWNERSHIP
 * ---------
 * The ONLY object this area ever allocates is the session
 * (kimix_graphdb_open); it is released by exactly one kimix_graphdb_close.
 * There is no cross-heap free anywhere in the family: a caller that links
 * kimix_api.dll (or dlopens it with KIMIX_API_STATIC) never hands a pointer
 * back to the library, so the one-heap rule of api/ffi_common.h holds
 * trivially.  `version_string` and `status_name` return static storage owned
 * by the library -- never free it.
 *
 * TRANSACTIONS
 * ------------
 * ONE open transaction per session (the IGraphBackend contract): begin,
 * commit or abort before beginning the next.  A collective transaction
 * (kimix_graphdb_tx_begin_collective) is read-only -- every write answered
 * with KIMIX_GDB_ERR_READ_ONLY_TRANSACTION -- and is closed with
 * kimix_graphdb_tx_close_collective.
 *
 * Threading, keys, and the (pointer, length) byte convention follow
 * <graph_db/graph_c_api.h> contract 5..7: one session per worker, nothing
 * NUL-terminated or NUL-free on the payload paths, a length of 0 is a valid
 * empty value.
 *
 * Valid for a C99/C11 compiler and for C++17 or newer; see api/ffi_common.h
 * for the rules shared by the whole surface.  The index is docs/ffi.md.
 */
#pragma once
#ifndef KIMIX_API_FFI_GRAPHDB_H
#define KIMIX_API_FFI_GRAPHDB_H

#include <api/ffi_common.h>

/* The POD structs, the status codes and every constant this family speaks are
 * graph_c_api.h's -- included, never re-declared (see the header note above). */
#include <graph_db/graph_c_api.h>

KIMIX_FFI_BEGIN

/* ===========================================================================
 * 1. Library / ABI queries
 * ======================================================================== */

/* KIMIX_GDB_ABI_VERSION as compiled into the image; a binding asserts on it
 * at load time.  Never fails. */
KIMIX_FFI uint32_t kimix_graphdb_abi_version(void);

/* The graph-db version string ("graph db 1.2.3" shape).  Static storage;
 * never free it.  Never fails. */
KIMIX_FFI const char *kimix_graphdb_version_string(void);

/* The struct sizes of the running library into `out` (see kimix_gdb_layout),
 * so a binding can verify its mirrored layouts before trusting one.
 * KIMIX_GDB_ERR_ARGUMENT for a NULL `out`. */
KIMIX_FFI int32_t kimix_graphdb_layout_info(KIMIX_OUT kimix_gdb_layout *out);

/* The textual name of a status code ("ok", "error_vertex", ...); NEVER NULL,
 * an unknown code answers "status_unknown".  Static storage.  Never fails. */
KIMIX_FFI const char *kimix_graphdb_status_name(KIMIX_IN int32_t status);

/* ===========================================================================
 * 2. Session lifecycle
 * ======================================================================== */

/* Open a database session -- the ONLY allocation of this family.  A NULL or
 * empty `options->directory` opens a purely in-memory database (persist() and
 * load() then answer KIMIX_GDB_ERR_NO_SUCH_FILE).  `*out` is written only on
 * KIMIX_GDB_OK.  Release with exactly one kimix_graphdb_close().
 * KIMIX_GDB_ERR_ARGUMENT for a NULL `out`; a NULL `options` uses the library
 * defaults. */
KIMIX_FFI int32_t kimix_graphdb_open(KIMIX_IN const kimix_gdb_open_options *options,
                                     KIMIX_OUT kimix_gdb_session **out);

/* Close and destroy the session (an open transaction is aborted first).  A
 * NULL `session` is ignored; a second close is undefined behaviour. */
KIMIX_FFI void kimix_graphdb_close(KIMIX_TRANSFER kimix_gdb_session *session);

/* The honest capability advertisement of this backend into `out`.
 * KIMIX_GDB_ERR_ARGUMENT for a NULL `session` or `out`. */
KIMIX_FFI int32_t kimix_graphdb_capabilities(KIMIX_IN kimix_gdb_session *session,
 KIMIX_OUT kimix_gdb_capability *out);

/* The named-graph catalog (ABI 5+): the process-wide registry of the LIVE
 * named graphs.  A second live open of one name is KIMIX_GDB_ERR_NAME_EXISTS;
 * close releases the entry.  Anonymous opens never touch it. */
KIMIX_FFI int32_t kimix_graphdb_catalog_contains(KIMIX_IN const char *name,
 KIMIX_IN size_t name_len,
 KIMIX_OUT uint8_t *out_exists);
KIMIX_FFI int32_t kimix_graphdb_catalog_count(KIMIX_OUT size_t *out_count);
/* The session's graph name into a caller buffer ("" when anonymous);
 * KIMIX_GDB_ERR_BUFFER with the required size when the buffer is too small. */
KIMIX_FFI int32_t kimix_graphdb_session_graph_name(KIMIX_IN kimix_gdb_session *session,
 KIMIX_OUT KIMIX_NULLABLE char *buffer,
 KIMIX_IN size_t capacity,
 KIMIX_OUT size_t *out_len);

/* Save the store.  `full != 0` rewrites every record, 0 only the dirty ones.
 * KIMIX_GDB_ERR_NO_SUCH_FILE when the session has no store (in-memory). */
KIMIX_FFI int32_t kimix_graphdb_persist(KIMIX_IN kimix_gdb_session *session,
                                        KIMIX_IN uint8_t full);

/* Re-read the store into the open session.
 * KIMIX_GDB_ERR_NO_SUCH_FILE when the session has no store. */
KIMIX_FFI int32_t kimix_graphdb_load(KIMIX_IN_OUT kimix_gdb_session *session);

/* ===========================================================================
 * 3. Vertices
 * ======================================================================== */

/* Create one keyed vertex.  `*out_uid` is written on OK and on the warning;
 * `*out_warning` is 1 with KIMIX_GDB_WARNING_NON_UNIQUE_ID (key_mode =
 * report_warning and a duplicate key). */
KIMIX_FFI int32_t kimix_graphdb_vertex_create(KIMIX_IN_OUT kimix_gdb_session *session,
                                              KIMIX_IN const kimix_gdb_vertex_spec *spec,
                                              KIMIX_OUT kimix_gdb_guid *out_uid,
                                              KIMIX_OUT KIMIX_NULLABLE uint8_t *out_warning);

/* MERGE: like vertex_create but an existing key is adopted, never an error;
 * `*out_created` is 1 when a vertex was made, 0 when an existing one matched. */
KIMIX_FFI int32_t kimix_graphdb_vertex_get_or_create(KIMIX_IN_OUT kimix_gdb_session *session,
                                                     KIMIX_IN const kimix_gdb_vertex_spec *spec,
                                                     KIMIX_OUT kimix_gdb_guid *out_uid,
                                                     KIMIX_OUT uint8_t *out_created);

/* Resolve the (label, external_id) key to a uid.  A miss is NOT an error:
 * KIMIX_GDB_OK with `*out_found` 0 and `*out_uid` untouched. */
KIMIX_FFI int32_t kimix_graphdb_vertex_translate(KIMIX_IN kimix_gdb_session *session,
                                                 KIMIX_IN const kimix_gdb_vertex_key *key,
                                                 KIMIX_OUT kimix_gdb_guid *out_uid,
                                                 KIMIX_OUT uint8_t *out_found);

/* Remove a vertex and its edges (cascade).  KIMIX_GDB_ERR_VERTEX for an
 * unknown uid. */
KIMIX_FFI int32_t kimix_graphdb_vertex_free(KIMIX_IN_OUT kimix_gdb_session *session,
                                            KIMIX_IN kimix_gdb_guid vertex);

/* The guarded delete: KIMIX_GDB_ERR_EDGE while the vertex still carries any
 * edge, the cascade otherwise. */
KIMIX_FFI int32_t kimix_graphdb_vertex_free_guarded(KIMIX_IN_OUT kimix_gdb_session *session,
                                                    KIMIX_IN kimix_gdb_guid vertex);

/* ===========================================================================
 * 4. Labels on vertices and edges
 * ======================================================================== */

/* Attach / detach one registered label.  Unknown label: KIMIX_GDB_ERR_LABEL;
 * unknown vertex/edge: KIMIX_GDB_ERR_VERTEX / KIMIX_GDB_ERR_EDGE. */
KIMIX_FFI int32_t kimix_graphdb_vertex_label_add(KIMIX_IN_OUT kimix_gdb_session *session,
                                                 KIMIX_IN kimix_gdb_guid vertex,
                                                 KIMIX_IN uint32_t label);
KIMIX_FFI int32_t kimix_graphdb_vertex_label_remove(KIMIX_IN_OUT kimix_gdb_session *session,
                                                    KIMIX_IN kimix_gdb_guid vertex,
                                                    KIMIX_IN uint32_t label);

/* Two-phase read: `labels == NULL` reports `*out_count` only.  The FIRST
 * entry is the vertex's key label (the GDI GetLabelsOfVertex order). */
KIMIX_FFI int32_t kimix_graphdb_vertex_labels(KIMIX_IN kimix_gdb_session *session,
                                              KIMIX_IN kimix_gdb_guid vertex,
                                              KIMIX_OUT KIMIX_NULLABLE uint32_t *labels,
                                              KIMIX_IN size_t capacity,
                                              KIMIX_OUT size_t *out_count);

KIMIX_FFI int32_t kimix_graphdb_edge_label_add(KIMIX_IN_OUT kimix_gdb_session *session,
                                               KIMIX_IN kimix_gdb_guid edge,
                                               KIMIX_IN uint32_t label);
KIMIX_FFI int32_t kimix_graphdb_edge_label_remove(KIMIX_IN_OUT kimix_gdb_session *session,
                                                  KIMIX_IN kimix_gdb_guid edge,
                                                  KIMIX_IN uint32_t label);

/* Two-phase read, same shape as the vertex labels (no key-label rule here). */
KIMIX_FFI int32_t kimix_graphdb_edge_labels(KIMIX_IN kimix_gdb_session *session,
                                            KIMIX_IN kimix_gdb_guid edge,
                                            KIMIX_OUT KIMIX_NULLABLE uint32_t *labels,
                                            KIMIX_IN size_t capacity,
                                            KIMIX_OUT size_t *out_count);

/* ===========================================================================
 * 5. Properties on vertices and edges
 * ======================================================================== */

/* add: fails with KIMIX_GDB_ERR_PROPERTY_EXISTS when the name is already
 * carried.  set: requires it (KIMIX_GDB_ERR_NO_PROPERTY otherwise).  update:
 * an upsert -- insert or overwrite, never an existence error.  The name must
 * be a registered property type or KIMIX_GDB_ERR_PROPERTY_TYPE comes back,
 * and the value must match its declaration (container/dtype/count) or
 * KIMIX_GDB_ERR_WRONG_TYPE / KIMIX_GDB_ERR_COUNT. */
KIMIX_FFI int32_t kimix_graphdb_vertex_property_add(KIMIX_IN_OUT kimix_gdb_session *session,
                                                    KIMIX_IN kimix_gdb_guid vertex,
                                                    KIMIX_IN uint32_t name,
                                                    KIMIX_IN const kimix_gdb_property *value);
KIMIX_FFI int32_t kimix_graphdb_vertex_property_set(KIMIX_IN_OUT kimix_gdb_session *session,
                                                    KIMIX_IN kimix_gdb_guid vertex,
                                                    KIMIX_IN uint32_t name,
                                                    KIMIX_IN const kimix_gdb_property *value);
KIMIX_FFI int32_t kimix_graphdb_vertex_property_update(KIMIX_IN_OUT kimix_gdb_session *session,
                                                       KIMIX_IN kimix_gdb_guid vertex,
                                                       KIMIX_IN uint32_t name,
                                                       KIMIX_IN const kimix_gdb_property *value);

/* Remove the named properties (unknown names in the list: the batch still
 * succeeds -- this is GDI's remove-as-many-as-there-are shape). */
KIMIX_FFI int32_t kimix_graphdb_vertex_properties_remove(KIMIX_IN_OUT kimix_gdb_session *session,
                                                         KIMIX_IN kimix_gdb_guid vertex,
                                                         KIMIX_IN const uint32_t *names,
                                                         KIMIX_IN size_t name_count);

/* Two-phase read of every property of the vertex: `items == NULL && scratch
 * == NULL` reports `*out_count` and `*out_scratch_bytes`; the fetch fills
 * each record and appends its payload into the caller's scratch buffer
 * (record payload_offset / payload_len).  Too-small storage answers
 * KIMIX_GDB_ERR_BUFFER with the full sizes still reported. */
KIMIX_FFI int32_t kimix_graphdb_vertex_properties(KIMIX_IN kimix_gdb_session *session,
                                                  KIMIX_IN kimix_gdb_guid vertex,
                                                  KIMIX_OUT KIMIX_NULLABLE kimix_gdb_property_record *items,
                                                  KIMIX_IN size_t capacity,
                                                  KIMIX_OUT KIMIX_NULLABLE uint8_t *scratch,
                                                  KIMIX_IN size_t scratch_capacity,
                                                  KIMIX_OUT size_t *out_count,
                                                  KIMIX_OUT size_t *out_scratch_bytes);

/* The DECLARATION (kimix_gdb_property_type) of every property the vertex
 * carries, in the same two-phase scratch shape: name_offset / name_len point
 * into the scratch buffer. */
KIMIX_FFI int32_t kimix_graphdb_vertex_property_types(KIMIX_IN kimix_gdb_session *session,
                                                      KIMIX_IN kimix_gdb_guid vertex,
                                                      KIMIX_OUT KIMIX_NULLABLE kimix_gdb_property_type *items,
                                                      KIMIX_IN size_t capacity,
                                                      KIMIX_OUT KIMIX_NULLABLE uint8_t *scratch,
                                                      KIMIX_IN size_t scratch_capacity,
                                                      KIMIX_OUT size_t *out_count,
                                                      KIMIX_OUT size_t *out_scratch_bytes);

KIMIX_FFI int32_t kimix_graphdb_edge_property_add(KIMIX_IN_OUT kimix_gdb_session *session,
                                                  KIMIX_IN kimix_gdb_guid edge,
                                                  KIMIX_IN uint32_t name,
                                                  KIMIX_IN const kimix_gdb_property *value);
KIMIX_FFI int32_t kimix_graphdb_edge_property_set(KIMIX_IN_OUT kimix_gdb_session *session,
                                                  KIMIX_IN kimix_gdb_guid edge,
                                                  KIMIX_IN uint32_t name,
                                                  KIMIX_IN const kimix_gdb_property *value);
KIMIX_FFI int32_t kimix_graphdb_edge_property_update(KIMIX_IN_OUT kimix_gdb_session *session,
                                                     KIMIX_IN kimix_gdb_guid edge,
                                                     KIMIX_IN uint32_t name,
                                                     KIMIX_IN const kimix_gdb_property *value);
KIMIX_FFI int32_t kimix_graphdb_edge_properties_remove(KIMIX_IN_OUT kimix_gdb_session *session,
                                                       KIMIX_IN kimix_gdb_guid edge,
                                                       KIMIX_IN const uint32_t *names,
                                                       KIMIX_IN size_t name_count);
KIMIX_FFI int32_t kimix_graphdb_edge_properties(KIMIX_IN kimix_gdb_session *session,
                                                KIMIX_IN kimix_gdb_guid edge,
                                                KIMIX_OUT KIMIX_NULLABLE kimix_gdb_property_record *items,
                                                KIMIX_IN size_t capacity,
                                                KIMIX_OUT KIMIX_NULLABLE uint8_t *scratch,
                                                KIMIX_IN size_t scratch_capacity,
                                                KIMIX_OUT size_t *out_count,
                                                KIMIX_OUT size_t *out_scratch_bytes);
KIMIX_FFI int32_t kimix_graphdb_edge_property_types(KIMIX_IN kimix_gdb_session *session,
                                                    KIMIX_IN kimix_gdb_guid edge,
                                                    KIMIX_OUT KIMIX_NULLABLE kimix_gdb_property_type *items,
                                                    KIMIX_IN size_t capacity,
                                                    KIMIX_OUT KIMIX_NULLABLE uint8_t *scratch,
                                                    KIMIX_IN size_t scratch_capacity,
                                                    KIMIX_OUT size_t *out_count,
                                                    KIMIX_OUT size_t *out_scratch_bytes);

/* ===========================================================================
 * 6. Edges
 * ======================================================================== */

/* Connect origin -> target (a vertex uid each, or KIMIX_GDB_ERR_VERTEX) WITH
 * the initial payload in the same call: `labels` may be NULL/0 and
 * `property_names[i]` names `properties[i]` (a property-type id; ABI 4 — the
 * create-time degrade of dropping unnamed property values is gone).  The edge
 * uid lands in `*out_edge`. */
KIMIX_FFI int32_t kimix_graphdb_edge_create(KIMIX_IN_OUT kimix_gdb_session *session,
 KIMIX_IN kimix_gdb_guid origin,
 KIMIX_IN kimix_gdb_guid target,
 KIMIX_IN uint8_t direction,
 KIMIX_IN KIMIX_NULLABLE const uint32_t *labels,
 KIMIX_IN size_t label_count,
 KIMIX_IN KIMIX_NULLABLE const uint32_t *property_names,
 KIMIX_IN KIMIX_NULLABLE const kimix_gdb_property *properties,
 KIMIX_IN size_t property_count,
 KIMIX_OUT kimix_gdb_guid *out_edge);

/* Remove the edge.  Unknown uid: KIMIX_GDB_ERR_EDGE. */
KIMIX_FFI int32_t kimix_graphdb_edge_free(KIMIX_IN_OUT kimix_gdb_session *session,
                                          KIMIX_IN kimix_gdb_guid edge);

/* Re-point or re-orient one edge (the endpoint must be a live vertex). */
KIMIX_FFI int32_t kimix_graphdb_edge_set_origin(KIMIX_IN_OUT kimix_gdb_session *session,
                                                KIMIX_IN kimix_gdb_guid edge,
                                                KIMIX_IN kimix_gdb_guid origin);
KIMIX_FFI int32_t kimix_graphdb_edge_set_target(KIMIX_IN_OUT kimix_gdb_session *session,
                                                KIMIX_IN kimix_gdb_guid edge,
                                                KIMIX_IN kimix_gdb_guid target);
KIMIX_FFI int32_t kimix_graphdb_edge_set_direction(KIMIX_IN_OUT kimix_gdb_session *session,
                                                   KIMIX_IN kimix_gdb_guid edge,
                                                   KIMIX_IN uint8_t direction);

/* The two endpoints of one edge (GDI's GetVerticesOfEdge). */
KIMIX_FFI int32_t kimix_graphdb_edge_endpoints(KIMIX_IN kimix_gdb_session *session,
                                               KIMIX_IN kimix_gdb_guid edge,
                                               KIMIX_OUT kimix_gdb_guid *out_origin,
                                               KIMIX_OUT kimix_gdb_guid *out_target);

/* ===========================================================================
 * 7. Filtered, oriented, paged reads
 * ======================================================================== */

/* Paged edge read over one vertex.  `constraint` is an id from
 * kimix_graphdb_constraint_create (0 = none); `orientation` a
 * KIMIX_GDB_ORIENTATION_* value; `cap` limits the PAGE (-1/0 semantics: see
 * graph_c_api.h -- with `items == NULL` the call only counts).  Two-phase
 * like the buffers: `items == NULL` reports `*out_count` and `*out_total`
 * without touching any storage; the fetch fills up to `capacity` rows and
 * `*out_total` always carries the full unfiltered-by-capacity count.
 * Too-small storage: KIMIX_GDB_ERR_BUFFER. */
KIMIX_FFI int32_t kimix_graphdb_edges_of_vertex(KIMIX_IN kimix_gdb_session *session,
                                                KIMIX_IN kimix_gdb_guid vertex,
                                                KIMIX_IN uint32_t constraint,
                                                KIMIX_IN uint8_t orientation,
                                                KIMIX_IN int64_t cap,
                                                KIMIX_OUT KIMIX_NULLABLE kimix_gdb_edge_record *items,
                                                KIMIX_IN size_t capacity,
                                                KIMIX_OUT size_t *out_count,
                                                KIMIX_OUT size_t *out_total);

/* The same read returning the neighbour VERTEX uids instead of edge records. */
KIMIX_FFI int32_t kimix_graphdb_neighbors_of_vertex(KIMIX_IN kimix_gdb_session *session,
                                                    KIMIX_IN kimix_gdb_guid vertex,
                                                    KIMIX_IN uint32_t constraint,
                                                    KIMIX_IN uint8_t orientation,
                                                    KIMIX_IN int64_t cap,
                                                    KIMIX_OUT KIMIX_NULLABLE kimix_gdb_guid *items,
                                                    KIMIX_IN size_t capacity,
                                                    KIMIX_OUT size_t *out_count,
                                                    KIMIX_OUT size_t *out_total);

/* How many neighbours the (constraint, orientation) pair has. */
KIMIX_FFI int32_t kimix_graphdb_count_neighbors(KIMIX_IN kimix_gdb_session *session,
                                                KIMIX_IN kimix_gdb_guid vertex,
                                                KIMIX_IN uint32_t constraint,
                                                KIMIX_IN uint8_t orientation,
                                                KIMIX_OUT size_t *out_count);

/* Whether the pair has at least one neighbour (`*out_exists` 0/1). */
KIMIX_FFI int32_t kimix_graphdb_exists_neighbor(KIMIX_IN kimix_gdb_session *session,
                                                KIMIX_IN kimix_gdb_guid vertex,
                                                KIMIX_IN uint32_t constraint,
                                                  KIMIX_IN uint8_t orientation,
                                                  KIMIX_OUT uint8_t *out_exists);

  /* ===========================================================================
   * 7b. Native multi-hop traversal (the G3 capability — ABI 2)
   * ======================================================================== */

  /* ONE call for the whole level-by-level BFS the engine would otherwise
   * compose from per-hop neighbour reads.  `constraint` filters every
   * discovered vertex (GDI section 5.10); `cap_per_level` bounds each level
   * (>= 1); `max_depth` is 1..KIMIX_GDB_MAX_HOP_DEPTH.  Two-phase like the
   * reads: items == NULL reports *out_count, then caller-owned storage of
   * *out_count records (items[0] is the start at depth 0).  ERR_BUFFER when
   * capacity < *out_count (the count is still reported). */
  KIMIX_FFI int32_t kimix_graphdb_traverse_bfs(KIMIX_IN kimix_gdb_session *session,
                                               KIMIX_IN kimix_gdb_guid start,
                                               KIMIX_IN uint32_t constraint,
                                               KIMIX_IN uint8_t orientation,
                                               KIMIX_IN int64_t cap_per_level,
                                               KIMIX_IN uint32_t max_depth,
                                               KIMIX_OUT KIMIX_NULLABLE kimix_gdb_traversal_vertex *items,
                                               KIMIX_IN size_t capacity,
                                               KIMIX_OUT size_t *out_count);

  /* Bounded shortest path along outgoing edges, endpoints included, in one
   * native call.  Two-phase like traverse_bfs.  OK with *out_count == 0 is
   * "unreachable within max_depth" (not an error); an unknown endpoint is
   * KIMIX_GDB_ERR_VERTEX; a bound outside 1..KIMIX_GDB_MAX_HOP_DEPTH is
   * KIMIX_GDB_ERR_ARGUMENT. */
KIMIX_FFI int32_t kimix_graphdb_shortest_path(KIMIX_IN kimix_gdb_session *session,
KIMIX_IN kimix_gdb_guid from,
KIMIX_IN kimix_gdb_guid to,
KIMIX_IN uint32_t max_depth,
KIMIX_OUT KIMIX_NULLABLE kimix_gdb_guid *items,
KIMIX_IN size_t capacity,
KIMIX_OUT size_t *out_count);

/* EDGE-FILTERED shortest path (the edge-searching form): same contract as
 * kimix_graphdb_shortest_path, but the walk keeps a hop only when the EDGE it
 * steps over satisfies `edge_constraint` (0 = every edge), follows
 * `orientation` edges, and each returned step reports the edge that carried the
 * hop (zeroed on the first step).  Two-phase like traverse_bfs.  OK with
 * *out_count == 0 is "unreachable within max_depth over matching edges". */
KIMIX_FFI int32_t kimix_graphdb_shortest_path_edges(KIMIX_IN kimix_gdb_session *session,
KIMIX_IN kimix_gdb_guid from,
KIMIX_IN kimix_gdb_guid to,
KIMIX_IN uint32_t edge_constraint,
KIMIX_IN uint8_t orientation,
KIMIX_IN uint32_t max_depth,
KIMIX_OUT KIMIX_NULLABLE kimix_gdb_path_step *items,
KIMIX_IN size_t capacity,
KIMIX_OUT size_t *out_count);

/* EDGE-FILTERED BFS (ABI 7): the readable neighbourhood of `start` over
 * MATCHING edges in ONE call, with EVERY constraint-satisfying edge among the
 * discovered vertices (not just the spanning tree) and a truncation flag.
 * Same hop rule as kimix_graphdb_shortest_path_edges: an edge failing
 * `edge_constraint` is never traversed; `orientation` selects the followed
 * links.  THREE outputs, all two-phase: probe with items == NULL &&
 * edges == NULL reports *out_vertex_count / *out_edge_count /
 * *out_truncated; the fill call re-runs the walk into caller storage
 * (ERR_BUFFER with the sizes still reported when a capacity is short).
 * items[0] is the start at depth 0 with a zeroed discovering edge; expansion
 * stops at `max_vertices` vertices (*out_truncated = 1 then).  Error surface
 * like shortest_path_edges: ERR_VERTEX / ERR_CONSTRAINT / ERR_ARGUMENT
 * (bad depth or max_vertices < 1); a label ordering op is ERR_OP at
 * constraint-creation time. */
KIMIX_FFI int32_t kimix_graphdb_traverse_bfs_edges(KIMIX_IN kimix_gdb_session *session,
KIMIX_IN kimix_gdb_guid start,
KIMIX_IN uint32_t edge_constraint,
KIMIX_IN uint8_t orientation,
KIMIX_IN uint32_t max_depth,
KIMIX_IN uint32_t max_vertices,
KIMIX_OUT KIMIX_NULLABLE kimix_gdb_bfs_vertex *items,
KIMIX_IN size_t vertex_capacity,
KIMIX_OUT KIMIX_NULLABLE kimix_gdb_bfs_edge *edges,
KIMIX_IN size_t edge_capacity,
KIMIX_OUT size_t *out_vertex_count,
KIMIX_OUT size_t *out_edge_count,
KIMIX_OUT KIMIX_NULLABLE uint8_t *out_truncated);

/* Global scan (the G1 capability): ONE native pass over EVERY vertex matching
* `constraint` (0 = none), in the deterministic shard-then-map order.
* Two-phase like the reads: items == NULL reports *out_count and *out_total,
* then caller-owned storage of *out_count guids. */
KIMIX_FFI int32_t kimix_graphdb_scan_vertices(KIMIX_IN kimix_gdb_session *session,
KIMIX_IN uint32_t constraint,
KIMIX_IN int64_t cap,
KIMIX_OUT KIMIX_NULLABLE kimix_gdb_guid *items,
KIMIX_IN size_t capacity,
KIMIX_OUT size_t *out_count,
KIMIX_OUT size_t *out_total);

/* Global aggregate (the G4 capability): reduce `property` over every vertex
* matching `constraint` (0 = none), in one blocking native call (a fiber per
* shard when a scheduler is bound, inline otherwise). `function` is one of the
* KIMIX_GDB_AGGREGATE_* values; a string payload lands in `scratch`
* (KIMIX_GDB_ERR_BUFFER when it does not fit, *out still reported). */
KIMIX_FFI int32_t kimix_graphdb_aggregate(KIMIX_IN kimix_gdb_session *session,
KIMIX_IN uint32_t constraint,
KIMIX_IN uint32_t property,
KIMIX_IN int32_t function,
KIMIX_OUT KIMIX_NULLABLE uint8_t *scratch,
KIMIX_IN size_t scratch_capacity,
KIMIX_OUT kimix_gdb_aggregate_result *out);

/* ===========================================================================
* 8. Transactions (one open transaction per session)
* ======================================================================== */

/* Begin / commit / abort the session's ONE transaction.  A second begin is
 * KIMIX_GDB_ERR_STATE; a commit or abort with none open is
 * KIMIX_GDB_ERR_TRANSACTION.  An abort rolls back everything the
 * transaction touched, in reverse order. */
KIMIX_FFI int32_t kimix_graphdb_tx_begin(KIMIX_IN_OUT kimix_gdb_session *session);
KIMIX_FFI int32_t kimix_graphdb_tx_commit(KIMIX_IN_OUT kimix_gdb_session *session);
KIMIX_FFI int32_t kimix_graphdb_tx_abort(KIMIX_IN_OUT kimix_gdb_session *session);

/* The read-only collective scope: any write inside answers
 * KIMIX_GDB_ERR_READ_ONLY_TRANSACTION.  Closed with
 * kimix_graphdb_tx_close_collective (`commit != 0` commits the scope, 0
 * aborts it); closing a writable transaction that way is
 * KIMIX_GDB_ERR_TRANSACTION. */
KIMIX_FFI int32_t kimix_graphdb_tx_begin_collective(KIMIX_IN_OUT kimix_gdb_session *session);
KIMIX_FFI int32_t kimix_graphdb_tx_close_collective(KIMIX_IN_OUT kimix_gdb_session *session,
                                                    KIMIX_IN uint8_t commit);

/* ===========================================================================
 * 9. Schema: labels and property types
 * ======================================================================== */

/* Register / look up / rename / remove a label (a NUL-terminated UTF-8 name
 * given with its byte length; a blank name is KIMIX_GDB_ERR_EMPTY_NAME, a
 * duplicate KIMIX_GDB_ERR_NAME_EXISTS).  label_find: a miss is NOT an error
 * -- OK with `*out_found` 0.  label_get_all: two-phase, in id order, names
 * into the scratch buffer.  label_name writes ONE label's name into the
 * caller's `out` buffer and reports the required bytes (including no NUL:
 * exactly name_len bytes are written; capacity 0 probes). */
KIMIX_FFI int32_t kimix_graphdb_label_create(KIMIX_IN_OUT kimix_gdb_session *session,
                                             KIMIX_IN const char *name,
                                             KIMIX_IN size_t name_len,
                                             KIMIX_OUT uint32_t *out_label);
KIMIX_FFI int32_t kimix_graphdb_label_find(KIMIX_IN kimix_gdb_session *session,
                                           KIMIX_IN const char *name,
                                           KIMIX_IN size_t name_len,
                                           KIMIX_OUT uint32_t *out_label,
                                           KIMIX_OUT uint8_t *out_found);
KIMIX_FFI int32_t kimix_graphdb_label_get_all(KIMIX_IN kimix_gdb_session *session,
                                              KIMIX_OUT KIMIX_NULLABLE kimix_gdb_label *items,
                                              KIMIX_IN size_t capacity,
                                              KIMIX_OUT KIMIX_NULLABLE uint8_t *scratch,
                                              KIMIX_IN size_t scratch_capacity,
                                              KIMIX_OUT size_t *out_count,
                                              KIMIX_OUT size_t *out_scratch_bytes);
KIMIX_FFI int32_t kimix_graphdb_label_update(KIMIX_IN_OUT kimix_gdb_session *session,
                                             KIMIX_IN uint32_t label,
                                             KIMIX_IN const char *name,
                                             KIMIX_IN size_t name_len);
KIMIX_FFI int32_t kimix_graphdb_label_free(KIMIX_IN_OUT kimix_gdb_session *session,
                                           KIMIX_IN uint32_t label);
KIMIX_FFI int32_t kimix_graphdb_label_name(KIMIX_IN kimix_gdb_session *session,
                                           KIMIX_IN uint32_t label,
                                           KIMIX_OUT KIMIX_NULLABLE char *out,
                                           KIMIX_IN size_t capacity,
                                           KIMIX_OUT size_t *out_required);

/* The same lifecycle for property types: `container` KIMIX_GDB_CONTAINER_*,
 * `dtype` KIMIX_GDB_DTYPE_*, `count` the element count of an array container
 * (ignored for a single).  A name collision is KIMIX_GDB_ERR_PROPERTY_TYPE_EXISTS. */
KIMIX_FFI int32_t kimix_graphdb_property_type_create(KIMIX_IN_OUT kimix_gdb_session *session,
                                                     KIMIX_IN const char *name,
                                                     KIMIX_IN size_t name_len,
                                                     KIMIX_IN uint8_t container,
                                                     KIMIX_IN uint8_t dtype,
                                                     KIMIX_IN uint32_t count,
                                                     KIMIX_OUT uint32_t *out_type);
KIMIX_FFI int32_t kimix_graphdb_property_type_find(KIMIX_IN kimix_gdb_session *session,
                                                   KIMIX_IN const char *name,
                                                   KIMIX_IN size_t name_len,
                                                   KIMIX_OUT uint32_t *out_type,
                                                   KIMIX_OUT uint8_t *out_found);
KIMIX_FFI int32_t kimix_graphdb_property_type_get_all(KIMIX_IN kimix_gdb_session *session,
                                                      KIMIX_OUT KIMIX_NULLABLE kimix_gdb_property_type *items,
                                                      KIMIX_IN size_t capacity,
                                                      KIMIX_OUT KIMIX_NULLABLE uint8_t *scratch,
                                                      KIMIX_IN size_t scratch_capacity,
                                                      KIMIX_OUT size_t *out_count,
                                                      KIMIX_OUT size_t *out_scratch_bytes);
KIMIX_FFI int32_t kimix_graphdb_property_type_update(KIMIX_IN_OUT kimix_gdb_session *session,
                                                     KIMIX_IN uint32_t type,
                                                     KIMIX_IN const char *name,
                                                     KIMIX_IN size_t name_len,
                                                     KIMIX_IN uint8_t container,
                                                     KIMIX_IN uint8_t dtype,
                                                     KIMIX_IN uint32_t count);
KIMIX_FFI int32_t kimix_graphdb_property_type_free(KIMIX_IN_OUT kimix_gdb_session *session,
                                                   KIMIX_IN uint32_t type);

/* ===========================================================================
 * 10. Constraints (session-owned handles, dense ids)
 * ======================================================================== */

/* Build a constraint incrementally: create takes the fresh id, add_label /
 * add_property attach one condition each (op: KIMIX_GDB_OP_*), and the id
 * feeds the paged reads above.  Freeing releases the session-side slot;
 * using an unknown id in a read answers KIMIX_GDB_ERR_CONSTRAINT. */
KIMIX_FFI int32_t kimix_graphdb_constraint_create(KIMIX_IN_OUT kimix_gdb_session *session,
                                                  KIMIX_OUT uint32_t *out_constraint);
KIMIX_FFI int32_t kimix_graphdb_constraint_add_label(KIMIX_IN_OUT kimix_gdb_session *session,
                                                     KIMIX_IN uint32_t constraint,
                                                     KIMIX_IN uint32_t label,
                                                     KIMIX_IN uint8_t op);
KIMIX_FFI int32_t kimix_graphdb_constraint_add_property(KIMIX_IN_OUT kimix_gdb_session *session,
                                                        KIMIX_IN uint32_t constraint,
                                                        KIMIX_IN uint32_t property,
                                                        KIMIX_IN uint8_t op,
                                                        KIMIX_IN const kimix_gdb_property *value,
                                                        KIMIX_IN uint32_t count);
KIMIX_FFI int32_t kimix_graphdb_constraint_free(KIMIX_IN_OUT kimix_gdb_session *session,
                                               KIMIX_IN uint32_t constraint);

/* ===========================================================================
 * 11. Bulk
 * ======================================================================== */

/* N vertex creates in ONE transaction (a local scope when the session has no
 * open transaction).  `continue_on_error != 0` isolates the failing row and
 * keeps going; 0 aborts the batch at the first error and every later row
 * reports KIMIX_GDB_ERR_STATE (never attempted).  `out_uids`, `out_status`
 * and `out_warning` are caller arrays of `count` entries; any of them may be
 * NULL.  The call's own status is OK when the batch ran, an error code when
 * the whole batch was refused (a read-only transaction answers
 * KIMIX_GDB_ERR_READ_ONLY_TRANSACTION). */
KIMIX_FFI int32_t kimix_graphdb_vertices_create_bulk(KIMIX_IN_OUT kimix_gdb_session *session,
                                                     KIMIX_IN const kimix_gdb_vertex_spec *specs,
                                                     KIMIX_IN size_t count,
                                                     KIMIX_IN uint8_t continue_on_error,
                                                     KIMIX_OUT KIMIX_NULLABLE kimix_gdb_guid *out_uids,
                                                     KIMIX_OUT KIMIX_NULLABLE int32_t *out_status,
                                                     KIMIX_OUT KIMIX_NULLABLE uint8_t *out_warning);

/* The same shape for edges (no per-row warning: a duplicate edge is not a
 * key collision when parallel edges are allowed). */
KIMIX_FFI int32_t kimix_graphdb_edges_create_bulk(KIMIX_IN_OUT kimix_gdb_session *session,
                                                  KIMIX_IN const kimix_gdb_edge_spec *specs,
                                                  KIMIX_IN size_t count,
                                                  KIMIX_IN uint8_t continue_on_error,
                                                  KIMIX_OUT KIMIX_NULLABLE kimix_gdb_guid *out_uids,
                                                  KIMIX_OUT KIMIX_NULLABLE int32_t *out_status);

KIMIX_FFI_END

#endif /* KIMIX_API_FFI_GRAPHDB_H */
