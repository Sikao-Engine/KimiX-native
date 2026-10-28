/*
 * graph_c_api.h -- the C ABI of kimix-graph-db (a Semevia.Gdi.Native adapter's
 * binding surface).
 *
 * This header is pure 7-bit ASCII and valid for BOTH a C99/C11 compiler and C++.
 * It uses no templates, no namespaces, no `enum class`, no references, no
 * default arguments, no `bool` (1-byte `uint8_t` instead) and no C++ types:
 * opaque handles, fixed-size POD structs, `<stdint.h>` scalars, `size_t` and
 * `const char *`, plus PLAIN C enums for the status-code and property-kind
 * vocabularies (kimix_gdb_err / kimix_gdb_kind -- plain enums stay valid C99
 * AND C++, and a debugger can name their values; every enumerator's value is
 * spelled out because the numbers are the ABI). Include it as
 * <graph_db/graph_c_api.h>; it is NOT part of the C++ umbrella (graph_db.h),
 * exactly like src/api's headers.
 *
 * CONTRACT (read before binding)
 * 1. Every entry point returns `int32_t`, one of the kimix_gdb_status codes
 *    below. The numbers are IDENTICAL to kimix::graph_db::GdiStatus and to
 *    Semevia.Gdi.GdiError (0..2 warnings, 3..62 errors), so a managed wrapper can
 *    switch on the same integers. KIMIX_GDB_OK is 0, so `if (call(...))` reads as
 *    failure.
 * 2. An out-parameter is written ONLY on KIMIX_GDB_OK (or a warning). A failed
 *    call leaves the caller's memory untouched.
 * 3. NO LIBRARY ALLOCATION ON READ PATHS. Every variable-length result uses the
 *    two-phase pattern: call once with the array pointer NULL (or the buffer
 *    length 0) and the `out_*` parameters filled with the required counts, then
 *    call again with caller-owned storage. A buffer that is too small returns
 *    KIMIX_GDB_ERR_BUFFER and still reports the required sizes. The ONLY object
 *    the library ever allocates is the session (open/close) -- there is no
 *    cross-heap free.
 * 4. Variable-length payloads (property values, names) are delivered into a
 *    caller-provided SCRATCH buffer: each record carries (offset, length) into
 *    it. That keeps one flat allocation per read instead of one per value.
 * 5. Threading: one session per engine worker, single-threaded per session (the
 *    IGraphBackend contract). Reads inside one session may not run concurrently
 *    with writes to the same session.
 * 6. Strings and byte blobs are (pointer, length) pairs of UTF-8 / binary data.
 *    Nothing is NUL-terminated or NUL-free; a length of 0 is a valid empty value.
 * 7. ABI changes bump KIMIX_GDB_ABI_VERSION and the values reported by
 *    kimix_gdb_layout_info().
 *
 * IO ERRORS: kimix::LMDB is abort-on-error by design, so KIMIX_GDB_ERR_IO is
 * only produced for RECOVERABLE preconditions this library can detect itself
 * (store not open, no store configured). A disk failure inside LMDB remains
 * fatal: the adapter must treat it as process death, not as a return code.
 */
#ifndef KIMIX_GDB_GRAPH_C_API_H
#define KIMIX_GDB_GRAPH_C_API_H

#include <stddef.h> /* size_t */
#include <stdint.h> /* uint8_t ... uint64_t, int32_t, int64_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Versioning / linkage
 * ------------------------------------------------------------------------- */

  #define KIMIX_GDB_ABI_VERSION 7u

/* Status codes: a plain C enum, one enumerator per code, with the EXACT values
 * of kimix::graph_db::GdiStatus and Semevia.Gdi.GdiError, so a managed wrapper
 * can switch on the same integers (and a debugger can name them). Only the ones
 * a caller of this surface can actually receive are listed; the rest exist so a
 * wrapper can name them. Every value is spelled out: the NUMBERS are the ABI,
 * not the ordering. KIMIX_GDB_OK is 0, so `if (call(...))` reads as failure. */
typedef enum kimix_gdb_err {
    KIMIX_GDB_OK = 0,
    KIMIX_GDB_WARNING_NON_UNIQUE_ID = 1,
    KIMIX_GDB_WARNING_OTHER = 2,
    KIMIX_GDB_ERR_ACCESS = 3,
    KIMIX_GDB_ERR_ARGUMENT = 4,
    KIMIX_GDB_ERR_BAD_FILE = 5,
    KIMIX_GDB_ERR_BUFFER = 6,
    KIMIX_GDB_ERR_CONSTRAINT = 7,
    KIMIX_GDB_ERR_CONVERSION = 8,
    KIMIX_GDB_ERR_COUNT = 9,
    KIMIX_GDB_ERR_DATABASE = 10,
    KIMIX_GDB_ERR_DATATYPE = 11,
    KIMIX_GDB_ERR_INVALID_DATE = 52,
    KIMIX_GDB_ERR_EDGE = 15,
    KIMIX_GDB_ERR_EDGE_ORIENTATION = 16,
    KIMIX_GDB_ERR_EMPTY_NAME = 17,
    KIMIX_GDB_ERR_LABEL = 23,
    KIMIX_GDB_ERR_NAME_EXISTS = 24,
    KIMIX_GDB_ERR_NO_PROPERTY = 26,
    KIMIX_GDB_ERR_NO_SUCH_FILE = 28,
    KIMIX_GDB_ERR_NON_UNIQUE_ID = 29,
    KIMIX_GDB_ERR_OP = 32,
    KIMIX_GDB_ERR_OP_DATATYPE_MISMATCH = 33,
    KIMIX_GDB_ERR_PROPERTY_EXISTS = 34,
    KIMIX_GDB_ERR_PROPERTY_TYPE = 35,
    KIMIX_GDB_ERR_PROPERTY_TYPE_EXISTS = 36,
    KIMIX_GDB_ERR_READ_ONLY_PROPERTY_TYPE = 39,
    KIMIX_GDB_ERR_READ_ONLY_TRANSACTION = 40,
    KIMIX_GDB_ERR_SIZE = 42,
    KIMIX_GDB_ERR_SIZE_LIMIT = 43,
    KIMIX_GDB_ERR_STATE = 45,
    KIMIX_GDB_ERR_SUBCONSTRAINT = 46,
    KIMIX_GDB_ERR_TRANSACTION = 48,
    KIMIX_GDB_ERR_UID = 49,
    KIMIX_GDB_ERR_VERTEX = 50,
    KIMIX_GDB_ERR_WRONG_TYPE = 51,
    KIMIX_GDB_ERR_IO = 60,
    KIMIX_GDB_ERR_OTHER = 61,
    KIMIX_GDB_ERR_UNKNOWN = 62
} kimix_gdb_err;

/* ---------------------------------------------------------------------------
 * POD types
 * ------------------------------------------------------------------------- */

/* A 128-bit id (a vertex or edge uid). `lo` is the first 64 bits of
 * kimix::Guid::to_binary().data0/data1, `hi` the second. */
typedef struct kimix_gdb_guid {
    uint64_t lo;
    uint64_t hi;
} kimix_gdb_guid;

/* A borrowed byte range. Never owned by the library on a read path. */
typedef struct kimix_gdb_bytes {
    const uint8_t *data;
    size_t len;
} kimix_gdb_bytes;

/* Property kinds: a plain C enum, one enumerator per kind, mirroring
 * kimix::graph_db::PropertyKind (Semevia.Gdi ValueKind). Values are spelled out:
 * the numbers are the ABI. */
typedef enum kimix_gdb_kind {
    KIMIX_GDB_KIND_NONE = 0,
    KIMIX_GDB_KIND_INT64 = 1,
    KIMIX_GDB_KIND_DOUBLE = 2,
    KIMIX_GDB_KIND_STRING = 3,
    KIMIX_GDB_KIND_BLOB = 4,
    KIMIX_GDB_KIND_INT64_ARRAY = 5,
    KIMIX_GDB_KIND_DOUBLE_ARRAY = 6,
    KIMIX_GDB_KIND_STRING_ARRAY = 7
} kimix_gdb_kind;

/* Property container types (GDI "etype"). */
#define KIMIX_GDB_CONTAINER_SINGLE 0
#define KIMIX_GDB_CONTAINER_ARRAY 1

/* GDI datatypes (gdi_datatype.h values). */
#define KIMIX_GDB_DTYPE_CHAR 100
#define KIMIX_GDB_DTYPE_BYTE 104
#define KIMIX_GDB_DTYPE_INT64 112
#define KIMIX_GDB_DTYPE_UINT64 113
#define KIMIX_GDB_DTYPE_DOUBLE 114

/* Comparison operators (gdi_operation.h). */
#define KIMIX_GDB_OP_EQUAL 0
#define KIMIX_GDB_OP_NOT_EQUAL 1
#define KIMIX_GDB_OP_GREATER 2
#define KIMIX_GDB_OP_GREATER_OR_EQUAL 3
#define KIMIX_GDB_OP_LESS 4
#define KIMIX_GDB_OP_LESS_OR_EQUAL 5
#define KIMIX_GDB_OP_ANY_IN 6

/* Edge direction (GDI_EDGE_DIRECTED / GDI_EDGE_UNDIRECTED). */
#define KIMIX_GDB_EDGE_DIRECTED 0
#define KIMIX_GDB_EDGE_UNDIRECTED 1

/* Read orientation (a distinct set of values from the direction above). */
#define KIMIX_GDB_ORIENTATION_OUTGOING 0
#define KIMIX_GDB_ORIENTATION_INCOMING 1
#define KIMIX_GDB_ORIENTATION_BOTH 2

/* One property value as an INPUT: the same struct is both the input and the
 * output shape. `payload` is only read for the string/blob/array kinds (for the
 * numeric array kinds it is the little-endian element data; for KIND_STRING_ARRAY
 * it is the string-array block of ABI 7, sectionstring-array below). `count` is the
 * element count of an array value (exact, uint32_t since ABI 7 -- it was uint16_t
 * before) and informational for scalars.
 *
 * STRING-ARRAY (ABI 7). A KIMIX_GDB_KIND_STRING_ARRAY value carries its elements
 * in ONE contiguous block, the same layout the record codec persists:
 *
 *   [u32 element_count] { [u32 byte_len][UTF-8 bytes] } * element_count
 *
 * little-endian, no padding. `count` == element_count; `payload` is the block.
 * A malformed block (truncated element, length past the end, count inconsistent
 * with the block) is refused at the write path with KIMIX_GDB_ERR_COUNT. */
typedef struct kimix_gdb_property {
    uint8_t kind;  /* KIMIX_GDB_KIND_* */
    uint8_t dtype; /* KIMIX_GDB_DTYPE_*, informational on input */
    uint32_t count;
    int64_t i64;
    double f64;
    kimix_gdb_bytes payload;
} kimix_gdb_property;

/* One property as an OUTPUT record: the value plus where its variable-length
 * payload landed inside the caller's scratch buffer. */
typedef struct kimix_gdb_property_record {
    uint32_t name_id;
    uint8_t kind;
    uint8_t dtype;
    uint32_t count;
    int64_t i64;
    double f64;
    uint32_t payload_offset;
    uint32_t payload_len;
} kimix_gdb_property_record;

/* The (label, external_id) primary key of a vertex. */
typedef struct kimix_gdb_vertex_key {
    uint32_t label;
    kimix_gdb_bytes external_id;
} kimix_gdb_vertex_key;

/* One edge record as a read returns it (GDI's GdiEdgeRecord). */
typedef struct kimix_gdb_edge_record {
    kimix_gdb_guid uid;
    kimix_gdb_guid origin;
    kimix_gdb_guid target;
    uint8_t direction;
} kimix_gdb_edge_record;

/* One vertex of a traverse_bfs result (the native multi-hop shape): the
 * vertex, the BFS-tree parent that discovered it (zeroed at depth 0), and
 * its depth.  items[0] is always the start vertex at depth 0. */
typedef struct kimix_gdb_traversal_vertex {
    kimix_gdb_guid vertex;
    kimix_gdb_guid parent;
    uint32_t depth;
    uint32_t reserved;
} kimix_gdb_traversal_vertex;

/* One hop of an EDGE-FILTERED shortest path (kimix_gdb_shortest_path_edges):
 * the vertex the walk reached, the EDGE it traversed to get there (zeroed at
 * index 0 -- the start is reached by no edge) and the hop count from the start,
 * which is also the step's index in the returned path.  Carrying the edge is
 * what a vertex-only path cannot express: with parallel edges between the same
 * ordered pair, this says WHICH one carried the hop. */
typedef struct kimix_gdb_path_step {
    kimix_gdb_guid vertex;
    kimix_gdb_guid edge;
    uint32_t depth;
    uint32_t reserved;
} kimix_gdb_path_step;

/* One discovered vertex of a kimix_gdb_traverse_bfs_edges result (ABI 7): the
 * vertex, the EDGE that discovered it (zeroed at index 0 -- the start is reached
 * by no edge) and its BFS depth.  items[0] is always the start at depth 0. */
typedef struct kimix_gdb_bfs_vertex {
    kimix_gdb_guid vertex;
    kimix_gdb_guid edge;
    uint32_t depth;
    uint32_t reserved;
} kimix_gdb_bfs_vertex;

/* One constraint-satisfying edge among the discovered vertices of a
 * kimix_gdb_traverse_bfs_edges result (ABI 7): the edge uid and both endpoints.
 * Parallel edges between the same ordered pair are reported as separate records. */
typedef struct kimix_gdb_bfs_edge {
    kimix_gdb_guid edge;
    kimix_gdb_guid origin;
    kimix_gdb_guid target;
} kimix_gdb_bfs_edge;

  /* The deepest native traversal this build honours (the advertised
   * max_native_hop_depth); larger max_depth requests are ERR_ARGUMENT. */
  #define KIMIX_GDB_MAX_HOP_DEPTH 32u
  /* The reduction a kimix_gdb_aggregate call applies. */
  #define KIMIX_GDB_AGGREGATE_COUNT 0
  #define KIMIX_GDB_AGGREGATE_MIN 1
  #define KIMIX_GDB_AGGREGATE_MAX 2
  #define KIMIX_GDB_AGGREGATE_SUM 3
  #define KIMIX_GDB_AGGREGATE_AVG 4
  /* The payload kind carried by kimix_gdb_aggregate_result. */
  #define KIMIX_GDB_VALUE_NONE 0
  #define KIMIX_GDB_VALUE_INT64 1
  #define KIMIX_GDB_VALUE_DOUBLE 2
  #define KIMIX_GDB_VALUE_STRING 3
  /* The result of one kimix_gdb_aggregate call. `matched` counts the vertices
   * satisfying the constraint; `valued` the subset carrying the property.
   * `has_value` is false exactly when the reduction had no input rows (the
   * count zero is read from `matched`). A string payload (min/max over char_)
   * lands in the call's scratch buffer: `string_offset`/`string_len` are valid
   * only when `value_kind == KIMIX_GDB_VALUE_STRING`. */
  typedef struct kimix_gdb_aggregate_result {
      uint64_t matched;
      uint64_t valued;
      uint8_t has_value;
      uint8_t value_kind;
      uint8_t reserved[6];
      int64_t int64_value;
      double double_value;
      uint32_t string_offset;
      uint32_t string_len;
  } kimix_gdb_aggregate_result;

/* A property-type declaration. `name_offset`/`name_len` point into the scratch
 * buffer of the call that returned it. */
typedef struct kimix_gdb_property_type {
    uint32_t id;
    uint8_t container;
    uint8_t dtype;
    uint32_t reserved; /* widened to uint32_t in ABI 7; stays zero-filled */
    uint32_t count;
    uint32_t name_offset;
    uint32_t name_len;
} kimix_gdb_property_type;

/* A registered label. Same scratch convention as above. */
typedef struct kimix_gdb_label {
    uint32_t id;
    uint32_t name_offset;
    uint32_t name_len;
} kimix_gdb_label;

/* What the backend can do (mirrors Semevia.Gdi.Capability, same names). */
typedef struct kimix_gdb_capability {
    uint8_t supports_global_scan;
    uint8_t supports_edge_by_stable_uid;
    uint8_t supports_edge_property_read;
    uint8_t supports_native_multi_hop;
    int32_t max_native_hop_depth;
    uint8_t supports_global_aggregates;
    uint8_t supports_collective_transactions;
    uint8_t enforces_unique_external_ids;
} kimix_gdb_capability;

/* Struct sizes, reported so a binding can verify it was built against the same
 * layout it is calling. */
typedef struct kimix_gdb_layout {
    uint32_t abi_version;
    uint32_t sizeof_property;
    uint32_t sizeof_property_record;
    uint32_t sizeof_vertex_key;
    uint32_t sizeof_edge_record;
    uint32_t sizeof_property_type;
    uint32_t sizeof_capability;
    uint32_t sizeof_vertex_spec;
    uint32_t sizeof_size_t;
      uint32_t alignof_guid;
      uint32_t sizeof_traversal_vertex; /* ABI 2+: native multi-hop traversal */
      uint32_t sizeof_aggregate_result; /* ABI 3+: global scan/aggregate */
      uint32_t sizeof_bfs_vertex; /* ABI 7+: edge-filtered BFS (traverse_bfs_edges) */
      uint32_t sizeof_bfs_edge;   /* ABI 7+: edge-filtered BFS (traverse_bfs_edges) */
  } kimix_gdb_layout;

/* An open() request. A NULL/empty `directory` opens a purely in-memory database
 * (persist() then reports KIMIX_GDB_ERR_NO_SUCH_FILE). */
typedef struct kimix_gdb_open_options {
 const char *directory; /* UTF-8, NUL-terminated (this is a path, not payload) */
 uint8_t auto_create_schema;
 uint8_t key_mode; /* 0 = enforce unique, 1 = report warning */
 uint8_t allow_parallel_edges;
 uint8_t allow_self_edges;
  uint8_t persist_on_commit;
  /* The content digest (ABI 6+, plan v3 section4.5): 0 = MD5 (default, the historical
   * behavior; a zeroed options struct therefore keeps it), 1 = XXH3-128. Both
   * fill the same 16-byte record field, so the on-disk format is unchanged.
   * Cross-policy use is benign: the loader recomputes the cached hash, loaded
   * vertices stay clean, and a dirty vertex is saved under the graph's own
   * policy (the post-write clear-dirty comparison keeps matching). */
  uint8_t content_digest;
  uint8_t reserved[2];
  size_t max_journal_bytes;
 size_t cold_map_size;
 /* The named-graph catalog name (ABI 5+, plan v3 step 3.7): NULL/"" opens an
  * ANONYMOUS graph that never touches the catalog.  A non-empty name enters
  * the process-wide catalog of LIVE graphs: a second live open of the same
  * name is KIMIX_GDB_ERR_NAME_EXISTS (one live graph per name -- the
  * one-session-per-worker contract, named), and kimix_gdb_close releases the
  * entry.  The name is also recorded in the store's meta record (persisted),
  * so a reopened store keeps its identity.  When NULL, the directory path
  * remains the recorded name (the pre-ABI-5 behavior, unchanged). */
 const char *graph_name; /* UTF-8, NUL-terminated */
} kimix_gdb_open_options;

/* One keyed vertex to create in a bulk call. */
typedef struct kimix_gdb_vertex_spec {
    kimix_gdb_vertex_key key;
    const uint32_t *labels;
    size_t label_count;
    /* `properties[i]` is named by `property_names[i]` (a property type id). */
    const kimix_gdb_property *properties;
    const uint32_t *property_names;
    size_t property_count;
    kimix_gdb_bytes content;
} kimix_gdb_vertex_spec;

/* One edge to create in a bulk call. */
typedef struct kimix_gdb_edge_spec {
    kimix_gdb_guid origin;
    kimix_gdb_guid target;
    uint8_t direction;
    uint8_t reserved[3];
    const uint32_t *labels;
    size_t label_count;
    const kimix_gdb_property *properties;
    const uint32_t *property_names;
    size_t property_count;
} kimix_gdb_edge_spec;

/* A constraint condition (an INPUT; constraints are built incrementally). */
typedef struct kimix_gdb_constraint_spec {
    uint32_t id;      /* returned by kimix_gdb_constraint_create */
    uint32_t reserved;
    uint32_t label;   /* label condition when is_property == 0 */
    uint32_t property;/* property condition when is_property == 1 */
    uint8_t is_property;
    uint8_t op;
    uint32_t reserved2; /* widened to uint32_t in ABI 7; stays zero-filled */
    kimix_gdb_property value;
    uint32_t count;
} kimix_gdb_constraint_spec;

/* Opaque session handle (one per database; see contract point 5). */
typedef struct kimix_gdb_session kimix_gdb_session;

/* ---------------------------------------------------------------------------
 * Library / ABI queries
 * ------------------------------------------------------------------------- */

uint32_t kimix_gdb_abi_version(void);
const char *kimix_gdb_version_string(void);
int32_t kimix_gdb_layout_info(kimix_gdb_layout *out);
/* Never returns NULL. Unknown codes answer "status_unknown". */
const char *kimix_gdb_status_name(int32_t status);

/* ---------------------------------------------------------------------------
 * Session lifecycle
 * ------------------------------------------------------------------------- */

int32_t kimix_gdb_open(const kimix_gdb_open_options *options, kimix_gdb_session **out);
void kimix_gdb_close(kimix_gdb_session *session);
int32_t kimix_gdb_capabilities(kimix_gdb_session *session, kimix_gdb_capability *out);
/* The named-graph catalog (ABI 5+, plan v3 step 3.7): the process-wide
 * registry of the LIVE named graphs.  open() with a non-empty graph_name
 * enters it; a second live open of the same name is KIMIX_GDB_ERR_NAME_EXISTS;
 * kimix_gdb_close releases the entry.  Anonymous opens never touch it.
 * Both probes are status-coded (no exceptions); catalog_count answers 0 for
 * an empty catalog. */
int32_t kimix_gdb_catalog_contains(const char *name, size_t name_len, uint8_t *out_exists);
int32_t kimix_gdb_catalog_count(size_t *out_count);
/* The session's graph name into a caller buffer ("" / length 0 when the
 * session is anonymous); ERR_ARGUMENT when the buffer is NULL or too small
 * (the required size is still reported). */
int32_t kimix_gdb_session_graph_name(kimix_gdb_session *session, char *buffer, size_t capacity,
                                     size_t *out_len);
/* full != 0 rewrites every record, 0 writes only the dirty ones. */
int32_t kimix_gdb_persist(kimix_gdb_session *session, uint8_t full);
/* Re-read the store into the open session. */
int32_t kimix_gdb_load(kimix_gdb_session *session);

/* ---------------------------------------------------------------------------
 * Vertices
 * ------------------------------------------------------------------------- */

/* out_uid and out_warning (0/1) are written on OK and on the warning. */
int32_t kimix_gdb_vertex_create(kimix_gdb_session *session,
                                const kimix_gdb_vertex_spec *spec,
                                kimix_gdb_guid *out_uid, uint8_t *out_warning);
/* MERGE: out_created is 1 when a vertex was made, 0 when an existing one matched. */
int32_t kimix_gdb_vertex_get_or_create(kimix_gdb_session *session,
                                       const kimix_gdb_vertex_spec *spec,
                                       kimix_gdb_guid *out_uid, uint8_t *out_created);
int32_t kimix_gdb_vertex_translate(kimix_gdb_session *session,
                                   const kimix_gdb_vertex_key *key,
                                   kimix_gdb_guid *out_uid, uint8_t *out_found);
int32_t kimix_gdb_vertex_free(kimix_gdb_session *session, kimix_gdb_guid vertex);
int32_t kimix_gdb_vertex_free_guarded(kimix_gdb_session *session, kimix_gdb_guid vertex);

/* ---------------------------------------------------------------------------
 * Labels on vertices and edges
 * ------------------------------------------------------------------------- */

int32_t kimix_gdb_vertex_label_add(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                   uint32_t label);
int32_t kimix_gdb_vertex_label_remove(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                      uint32_t label);
/* Two-phase: labels == NULL reports *out_count. The FIRST label is the vertex's
 * key label (GDI's GetLabelsOfVertex order). */
int32_t kimix_gdb_vertex_labels(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                uint32_t *labels, size_t capacity, size_t *out_count);

int32_t kimix_gdb_edge_label_add(kimix_gdb_session *session, kimix_gdb_guid edge,
                                 uint32_t label);
int32_t kimix_gdb_edge_label_remove(kimix_gdb_session *session, kimix_gdb_guid edge,
                                    uint32_t label);
int32_t kimix_gdb_edge_labels(kimix_gdb_session *session, kimix_gdb_guid edge,
                              uint32_t *labels, size_t capacity, size_t *out_count);

/* ---------------------------------------------------------------------------
 * Properties on vertices and edges
 * ------------------------------------------------------------------------- */

int32_t kimix_gdb_vertex_property_add(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                      uint32_t name, const kimix_gdb_property *value);
int32_t kimix_gdb_vertex_property_set(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                      uint32_t name, const kimix_gdb_property *value);
int32_t kimix_gdb_vertex_property_update(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                         uint32_t name, const kimix_gdb_property *value);
int32_t kimix_gdb_vertex_properties_remove(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                           const uint32_t *names, size_t name_count);
/* Two-phase read: items/scratch == NULL reports *out_count and *out_scratch_bytes. */
int32_t kimix_gdb_vertex_properties(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                    kimix_gdb_property_record *items, size_t capacity,
                                    uint8_t *scratch, size_t scratch_capacity,
                                    size_t *out_count, size_t *out_scratch_bytes);
int32_t kimix_gdb_vertex_property_types(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                        kimix_gdb_property_type *items, size_t capacity,
                                        uint8_t *scratch, size_t scratch_capacity,
                                        size_t *out_count, size_t *out_scratch_bytes);

int32_t kimix_gdb_edge_property_add(kimix_gdb_session *session, kimix_gdb_guid edge,
                                    uint32_t name, const kimix_gdb_property *value);
int32_t kimix_gdb_edge_property_set(kimix_gdb_session *session, kimix_gdb_guid edge,
                                    uint32_t name, const kimix_gdb_property *value);
int32_t kimix_gdb_edge_property_update(kimix_gdb_session *session, kimix_gdb_guid edge,
                                       uint32_t name, const kimix_gdb_property *value);
int32_t kimix_gdb_edge_properties_remove(kimix_gdb_session *session, kimix_gdb_guid edge,
                                         const uint32_t *names, size_t name_count);
int32_t kimix_gdb_edge_properties(kimix_gdb_session *session, kimix_gdb_guid edge,
                                  kimix_gdb_property_record *items, size_t capacity,
                                  uint8_t *scratch, size_t scratch_capacity,
                                  size_t *out_count, size_t *out_scratch_bytes);
int32_t kimix_gdb_edge_property_types(kimix_gdb_session *session, kimix_gdb_guid edge,
                                      kimix_gdb_property_type *items, size_t capacity,
                                      uint8_t *scratch, size_t scratch_capacity,
                                      size_t *out_count, size_t *out_scratch_bytes);

/* ---------------------------------------------------------------------------
 * Edges
 * ------------------------------------------------------------------------- */

/* Create one edge between two existing vertices WITH its initial payload in
 * the same call: labels plus (name id, value) properties.  ONE journal entry,
 * ONE validation pass -- the create-time degrade (property values accepted but
 * dropped for want of their names) is gone as of ABI 4.
 * `property_names[i]` names `properties[i]` (a property-type id from
 * kimix_gdb_property_type_find / _create); property_count > 0 with a null
 * names or values array is ERR_ARGUMENT.  Properties are validated BEFORE
 * anything is created, so a refusal leaves nothing behind. */
int32_t kimix_gdb_edge_create(kimix_gdb_session *session, kimix_gdb_guid origin,
                              kimix_gdb_guid target, uint8_t direction,
                              const uint32_t *labels, size_t label_count,
                              const uint32_t *property_names, const kimix_gdb_property *properties,
                              size_t property_count, kimix_gdb_guid *out_edge);
int32_t kimix_gdb_edge_free(kimix_gdb_session *session, kimix_gdb_guid edge);
int32_t kimix_gdb_edge_set_origin(kimix_gdb_session *session, kimix_gdb_guid edge,
                                  kimix_gdb_guid origin);
int32_t kimix_gdb_edge_set_target(kimix_gdb_session *session, kimix_gdb_guid edge,
                                  kimix_gdb_guid target);
int32_t kimix_gdb_edge_set_direction(kimix_gdb_session *session, kimix_gdb_guid edge,
                                     uint8_t direction);
int32_t kimix_gdb_edge_endpoints(kimix_gdb_session *session, kimix_gdb_guid edge,
                                 kimix_gdb_guid *out_origin, kimix_gdb_guid *out_target);

/* ---------------------------------------------------------------------------
 * Filtered, oriented, paged reads
 * ------------------------------------------------------------------------- */

/* `constraint` is a constraint id from kimix_gdb_constraint_create (0 = none). */
int32_t kimix_gdb_edges_of_vertex(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                  uint32_t constraint, uint8_t orientation, int64_t cap,
                                  kimix_gdb_edge_record *items, size_t capacity,
                                  size_t *out_count, size_t *out_total);
int32_t kimix_gdb_neighbors_of_vertex(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                      uint32_t constraint, uint8_t orientation, int64_t cap,
                                      kimix_gdb_guid *items, size_t capacity,
                                      size_t *out_count, size_t *out_total);
int32_t kimix_gdb_count_neighbors(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                  uint32_t constraint, uint8_t orientation, size_t *out_count);
int32_t kimix_gdb_exists_neighbor(kimix_gdb_session *session, kimix_gdb_guid vertex,
                                  uint32_t constraint, uint8_t orientation, uint8_t *out_exists);

/* Native multi-hop traversal (the G3 capability): ONE call for the whole
 * level-by-level BFS the engine would otherwise compose from per-hop reads.
 * `constraint` is a constraint id (0 = none) and filters every discovered
 * vertex (GDI section 5.10: a traversal filter applies to vertices, not
 * edges), so a filtered-out vertex's neighbourhood is never visited.
 * `cap_per_level` bounds the discoveries kept per level (>= 1; a full level
 * stops expanding).  `max_depth` is 1..KIMIX_GDB_MAX_HOP_DEPTH.
 * Two-phase like the reads: items == NULL reports *out_count (the traversal
 * itself runs in both phases -- no library allocation may hold the result),
 * then caller-owned storage of *out_count records.  A too-small capacity is
 * ERR_BUFFER with *out_count still reported. */
int32_t kimix_gdb_traverse_bfs(kimix_gdb_session *session, kimix_gdb_guid start,
                               uint32_t constraint, uint8_t orientation, int64_t cap_per_level,
                               uint32_t max_depth, kimix_gdb_traversal_vertex *items,
                               size_t capacity, size_t *out_count);
/* Bounded shortest path along outgoing edges, endpoints INCLUDED, in one
 * native call.  Two-phase like traverse_bfs.  OK with *out_count == 0 means
 * `to` is unreachable within `max_depth` (GDI's nullable-list answer, not an
 * error); an unknown endpoint is ERR_VERTEX; a bound outside
 * 1..KIMIX_GDB_MAX_HOP_DEPTH is ERR_ARGUMENT. */
  int32_t kimix_gdb_shortest_path(kimix_gdb_session *session, kimix_gdb_guid from,
                                  kimix_gdb_guid to, uint32_t max_depth, kimix_gdb_guid *items,
                                  size_t capacity, size_t *out_count);
  /* EDGE-FILTERED shortest path: exactly kimix_gdb_shortest_path, except the
   * walk keeps a hop only when the EDGE it steps over satisfies `edge_constraint`
   * (0 = every edge; section2.6 semantics -- the edge's own labels AND-ed with its
   * properties, NOT the neighbour vertex's), it follows `orientation` edges
   * (OUTGOING / INCOMING / BOTH) instead of outgoing only, and each returned
   * record reports the edge that carried the hop.  That is the edge-searching
   * form the vertex-only call cannot express (a traversal filter in GDI section5.10
   * gates vertices; this gates relations).
   * Two-phase like traverse_bfs: items == NULL reports *out_count only, then
   * caller-owned storage of *out_count kimix_gdb_path_step records.  OK with
   * *out_count == 0 means `to` is unreachable within `max_depth` over MATCHING
   * edges (GDI's nullable-list answer, not an error); an unknown endpoint is
   * ERR_VERTEX; a bound outside 1..KIMIX_GDB_MAX_HOP_DEPTH is ERR_ARGUMENT; an
   * unknown constraint id is ERR_CONSTRAINT.  A too-small capacity is
   * ERR_BUFFER with *out_count still reported.  A label condition carrying an
   * ordering op never reaches this call: kimix_gdb_constraint_add_label refuses
   * it with ERR_OP at creation time, which is the same rule.
   * ADDED AFTER ABI 6 (that precedent kept the ABI at 6; the bump to 7 came
   * later, with the string-array kind and the widened count fields -- see the
   * traverse_bfs_edges declaration below for the ABI-7 additions). */
  int32_t kimix_gdb_shortest_path_edges(kimix_gdb_session *session, kimix_gdb_guid from,
                                        kimix_gdb_guid to, uint32_t edge_constraint,
                                        uint8_t orientation, uint32_t max_depth,
                                        kimix_gdb_path_step *items, size_t capacity,
                                        size_t *out_count);
  /* EDGE-FILTERED BFS (ABI 7): ONE native call discovering the readable
   * neighbourhood of `start` over MATCHING edges, with ALL the edges that graph
   * uses -- the shape traverse_bfs (vertex-filtered, tree-only) and
   * shortest_path_edges (one path) cannot express.  The hop rule is exactly
   * kimix_gdb_shortest_path_edges': an edge that fails `edge_constraint`
   * (0 = every edge) is never traversed, and `orientation` selects the followed
   * links (OUTGOING / INCOMING / BOTH).
   * THREE outputs, all two-phase (probe with items == NULL && edges == NULL
   * reports *out_vertex_count / *out_edge_count / *out_truncated; the fill call
   * re-runs the walk into caller storage, ERR_BUFFER (6) with the sizes still
   * reported when either capacity is short):
   *   1. items: the discovered vertices in BFS order -- (vertex, depth, the edge
   *      that discovered it; zeroed at index 0).  Expansion stops once the vertex
   *      count reaches `max_vertices`.
   *   2. edges: EVERY constraint-satisfying edge whose BOTH endpoints are among
   *      the discovered vertices (a second sweep, deduplicated by edge uid -- with
   *      parallel edges between two entities, or A and B both linked to C, a mere
   *      spanning tree would drop edges; this does not).
   *   3. *out_truncated: 1 when discovery stopped at the `max_vertices` cap (the
   *      UI shows "only part displayed").
   * Error surface mirrors shortest_path_edges: an unknown start is ERR_VERTEX (50),
   * an unknown constraint id is ERR_CONSTRAINT (7), max_depth outside
   * 1..KIMIX_GDB_MAX_HOP_DEPTH is ERR_ARGUMENT (4), max_vertices < 1 is
   * ERR_ARGUMENT, and a label condition carrying an ordering op never reaches this
   * call (ERR_OP (32) at constraint-creation time, the same rule).  OK with a zero
   * vertex count cannot happen (the start is always discovered). */
  int32_t kimix_gdb_traverse_bfs_edges(kimix_gdb_session *session, kimix_gdb_guid start,
                                       uint32_t edge_constraint, uint8_t orientation,
                                       uint32_t max_depth, uint32_t max_vertices,
                                       kimix_gdb_bfs_vertex *items, size_t vertex_capacity,
                                       kimix_gdb_bfs_edge *edges, size_t edge_capacity,
                                       size_t *out_vertex_count, size_t *out_edge_count,
                                       uint8_t *out_truncated);
  /* Global scan (the G1 capability): ONE native pass over EVERY vertex matching
   * `constraint` (0 = none), in the deterministic shard-then-map order.
   * Two-phase like the reads: items == NULL runs the count probe and reports
   * *out_count (capped) and *out_total (the full match count), then caller-owned
   * storage of *out_count guids. `cap` follows the read contract: 0 = count
     * probe (no items), < 0 = ERR_ARGUMENT. A too-small capacity is ERR_BUFFER
     * with out_count/out_total still reported. */
  int32_t kimix_gdb_scan_vertices(kimix_gdb_session *session, uint32_t constraint, int64_t cap,
                                  kimix_gdb_guid *items, size_t capacity, size_t *out_count,
                                  size_t *out_total);
  /* Global aggregate (the G4 capability): reduce `property` over every vertex
   * matching `constraint` (0 = none), in one native call (a fiber per shard
   * when a scheduler is bound, inline otherwise -- the call blocks either way).
   * `function` is one of the KIMIX_GDB_AGGREGATE_* values; `property` is ignored
   * by COUNT. The legality matrix is validated up front: an unknown function or
   * a missing property id for a property reduction is ERR_ARGUMENT, an
   * unregistered property name is ERR_PROPERTY_TYPE, and a reduction the
   * declared dtype cannot serve (sum/avg on a non-numeric, min/max on a
   * non-orderable) is ERR_OP. A string payload (min/max over char_) is copied
   * into `scratch` (ERR_BUFFER when it does not fit, *out still reported). */
  int32_t kimix_gdb_aggregate(kimix_gdb_session *session, uint32_t constraint,
                              uint32_t property, int32_t function, uint8_t *scratch,
                                size_t scratch_capacity, kimix_gdb_aggregate_result *out);
  /* ---------------------------------------------------------------------------
   * Transactions (one open transaction per session)
   * ------------------------------------------------------------------------- */

  int32_t kimix_gdb_tx_begin(kimix_gdb_session *session);
int32_t kimix_gdb_tx_commit(kimix_gdb_session *session);
int32_t kimix_gdb_tx_abort(kimix_gdb_session *session);
int32_t kimix_gdb_tx_begin_collective(kimix_gdb_session *session);
int32_t kimix_gdb_tx_close_collective(kimix_gdb_session *session, uint8_t commit);

/* ---------------------------------------------------------------------------
 * Schema: labels and property types
 * ------------------------------------------------------------------------- */

int32_t kimix_gdb_label_create(kimix_gdb_session *session, const char *name, size_t name_len,
                               uint32_t *out_label);
int32_t kimix_gdb_label_find(kimix_gdb_session *session, const char *name, size_t name_len,
                             uint32_t *out_label, uint8_t *out_found);
/* Two-phase, in id order. */
int32_t kimix_gdb_label_get_all(kimix_gdb_session *session, kimix_gdb_label *items,
                                size_t capacity, uint8_t *scratch, size_t scratch_capacity,
                                size_t *out_count, size_t *out_scratch_bytes);
int32_t kimix_gdb_label_update(kimix_gdb_session *session, uint32_t label, const char *name,
                               size_t name_len);
int32_t kimix_gdb_label_free(kimix_gdb_session *session, uint32_t label);
/* The name of one label into a caller buffer (two-phase via out_required). */
int32_t kimix_gdb_label_name(kimix_gdb_session *session, uint32_t label, char *out,
                             size_t capacity, size_t *out_required);

int32_t kimix_gdb_property_type_create(kimix_gdb_session *session, const char *name,
                                       size_t name_len, uint8_t container, uint8_t dtype,
                                       uint32_t count, uint32_t *out_type);
int32_t kimix_gdb_property_type_find(kimix_gdb_session *session, const char *name,
                                     size_t name_len, uint32_t *out_type, uint8_t *out_found);
int32_t kimix_gdb_property_type_get_all(kimix_gdb_session *session,
                                        kimix_gdb_property_type *items, size_t capacity,
                                        uint8_t *scratch, size_t scratch_capacity,
                                        size_t *out_count, size_t *out_scratch_bytes);
int32_t kimix_gdb_property_type_update(kimix_gdb_session *session, uint32_t type,
                                       const char *name, size_t name_len, uint8_t container,
                                       uint8_t dtype, uint32_t count);
int32_t kimix_gdb_property_type_free(kimix_gdb_session *session, uint32_t type);

/* ---------------------------------------------------------------------------
 * Constraints
 * ------------------------------------------------------------------------- */

int32_t kimix_gdb_constraint_create(kimix_gdb_session *session, uint32_t *out_constraint);
int32_t kimix_gdb_constraint_add_label(kimix_gdb_session *session, uint32_t constraint,
                                       uint32_t label, uint8_t op);
int32_t kimix_gdb_constraint_add_property(kimix_gdb_session *session, uint32_t constraint,
                                          uint32_t property, uint8_t op,
                                          const kimix_gdb_property *value, uint32_t count);
int32_t kimix_gdb_constraint_free(kimix_gdb_session *session, uint32_t constraint);

/* ---------------------------------------------------------------------------
 * Bulk
 * ------------------------------------------------------------------------- */

/* N vertex creates in ONE transaction (continue_on_error != 0 isolates the rows,
 * 0 makes the batch all-or-nothing). `out_uids`, `out_status` and `out_warning`
 * are caller arrays of `count` entries; any of them may be NULL. */
int32_t kimix_gdb_vertices_create_bulk(kimix_gdb_session *session,
                                       const kimix_gdb_vertex_spec *specs, size_t count,
                                       uint8_t continue_on_error, kimix_gdb_guid *out_uids,
                                       int32_t *out_status, uint8_t *out_warning);
/* The same shape for edges. */
int32_t kimix_gdb_edges_create_bulk(kimix_gdb_session *session,
                                    const kimix_gdb_edge_spec *specs, size_t count,
                                    uint8_t continue_on_error, kimix_gdb_guid *out_uids,
                                    int32_t *out_status);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KIMIX_GDB_GRAPH_C_API_H */
