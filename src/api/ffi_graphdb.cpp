/*
 * ffi_graphdb.cpp -- implementation of the graph-db re-export family
 * (api/ffi_graphdb.h).
 *
 * NOTHING HAPPENS HERE.  Every function is a one-line forward to its
 * kimix_gdb_* twin in src/graph_db/graph_c_api.h, so every semantic (the
 * int32_t GDI status codes, the two-phase caller-owned buffer protocol, the
 * one-open-transaction rule, the scratch-buffer payloads, the session-owned
 * constraint handles) is exactly the archive's -- this file is what makes the
 * archive's ABI VISIBLE from kimix_api.dll, and nothing more.  Keeping the
 * bodies a single line each is what lets the whole family be reviewed as a
 * rename table: the only thing that may drift is a signature, and the compiler
 * catches that against the two headers.
 *
 * UNITY BUILD: like every src/api TU, no file-local helpers exist here at all
 * (there is nothing to factor out), so no anonymous-namespace names are added
 * to the merged batch either.
 */
#include <api/ffi_graphdb.h>

KIMIX_FFI_BEGIN

// ===========================================================================
// 1. Library / ABI queries
// ===========================================================================

uint32_t kimix_graphdb_abi_version(void) {
    return kimix_gdb_abi_version();
}

const char *kimix_graphdb_version_string(void) {
    return kimix_gdb_version_string();
}

int32_t kimix_graphdb_layout_info(kimix_gdb_layout *out) {
    return kimix_gdb_layout_info(out);
}

const char *kimix_graphdb_status_name(int32_t status) {
    return kimix_gdb_status_name(status);
}

// ===========================================================================
// 2. Session lifecycle
// ===========================================================================

int32_t kimix_graphdb_open(const kimix_gdb_open_options *options, kimix_gdb_session **out) {
    return kimix_gdb_open(options, out);
}

void kimix_graphdb_close(kimix_gdb_session *session) {
    kimix_gdb_close(session);
}

int32_t kimix_graphdb_capabilities(kimix_gdb_session *session, kimix_gdb_capability *out) {
 return kimix_gdb_capabilities(session, out);
}

int32_t kimix_graphdb_catalog_contains(const char *name, size_t name_len, uint8_t *out_exists) {
 return kimix_gdb_catalog_contains(name, name_len, out_exists);
}

int32_t kimix_graphdb_catalog_count(size_t *out_count) {
 return kimix_gdb_catalog_count(out_count);
}

int32_t kimix_graphdb_session_graph_name(kimix_gdb_session *session, char *buffer, size_t capacity, size_t *out_len) {
 return kimix_gdb_session_graph_name(session, buffer, capacity, out_len);
}

int32_t kimix_graphdb_persist(kimix_gdb_session *session, uint8_t full) {
    return kimix_gdb_persist(session, full);
}

int32_t kimix_graphdb_load(kimix_gdb_session *session) {
    return kimix_gdb_load(session);
}

// ===========================================================================
// 3. Vertices
// ===========================================================================

int32_t kimix_graphdb_vertex_create(kimix_gdb_session *session, const kimix_gdb_vertex_spec *spec, kimix_gdb_guid *out_uid, uint8_t *out_warning) {
    return kimix_gdb_vertex_create(session, spec, out_uid, out_warning);
}

int32_t kimix_graphdb_vertex_get_or_create(kimix_gdb_session *session, const kimix_gdb_vertex_spec *spec, kimix_gdb_guid *out_uid, uint8_t *out_created) {
    return kimix_gdb_vertex_get_or_create(session, spec, out_uid, out_created);
}

int32_t kimix_graphdb_vertex_translate(kimix_gdb_session *session, const kimix_gdb_vertex_key *key, kimix_gdb_guid *out_uid, uint8_t *out_found) {
    return kimix_gdb_vertex_translate(session, key, out_uid, out_found);
}

int32_t kimix_graphdb_vertex_free(kimix_gdb_session *session, kimix_gdb_guid vertex) {
    return kimix_gdb_vertex_free(session, vertex);
}

int32_t kimix_graphdb_vertex_free_guarded(kimix_gdb_session *session, kimix_gdb_guid vertex) {
    return kimix_gdb_vertex_free_guarded(session, vertex);
}

// ===========================================================================
// 4. Labels on vertices and edges
// ===========================================================================

int32_t kimix_graphdb_vertex_label_add(kimix_gdb_session *session, kimix_gdb_guid vertex, uint32_t label) {
    return kimix_gdb_vertex_label_add(session, vertex, label);
}

int32_t kimix_graphdb_vertex_label_remove(kimix_gdb_session *session, kimix_gdb_guid vertex, uint32_t label) {
    return kimix_gdb_vertex_label_remove(session, vertex, label);
}

int32_t kimix_graphdb_vertex_labels(kimix_gdb_session *session, kimix_gdb_guid vertex, uint32_t *labels, size_t capacity, size_t *out_count) {
    return kimix_gdb_vertex_labels(session, vertex, labels, capacity, out_count);
}

int32_t kimix_graphdb_edge_label_add(kimix_gdb_session *session, kimix_gdb_guid edge, uint32_t label) {
    return kimix_gdb_edge_label_add(session, edge, label);
}

int32_t kimix_graphdb_edge_label_remove(kimix_gdb_session *session, kimix_gdb_guid edge, uint32_t label) {
    return kimix_gdb_edge_label_remove(session, edge, label);
}

int32_t kimix_graphdb_edge_labels(kimix_gdb_session *session, kimix_gdb_guid edge, uint32_t *labels, size_t capacity, size_t *out_count) {
    return kimix_gdb_edge_labels(session, edge, labels, capacity, out_count);
}

// ===========================================================================
// 5. Properties on vertices and edges
// ===========================================================================

int32_t kimix_graphdb_vertex_property_add(kimix_gdb_session *session, kimix_gdb_guid vertex, uint32_t name, const kimix_gdb_property *value) {
    return kimix_gdb_vertex_property_add(session, vertex, name, value);
}

int32_t kimix_graphdb_vertex_property_set(kimix_gdb_session *session, kimix_gdb_guid vertex, uint32_t name, const kimix_gdb_property *value) {
    return kimix_gdb_vertex_property_set(session, vertex, name, value);
}

int32_t kimix_graphdb_vertex_property_update(kimix_gdb_session *session, kimix_gdb_guid vertex, uint32_t name, const kimix_gdb_property *value) {
    return kimix_gdb_vertex_property_update(session, vertex, name, value);
}

int32_t kimix_graphdb_vertex_properties_remove(kimix_gdb_session *session, kimix_gdb_guid vertex, const uint32_t *names, size_t name_count) {
    return kimix_gdb_vertex_properties_remove(session, vertex, names, name_count);
}

int32_t kimix_graphdb_vertex_properties(kimix_gdb_session *session, kimix_gdb_guid vertex, kimix_gdb_property_record *items, size_t capacity, uint8_t *scratch, size_t scratch_capacity, size_t *out_count, size_t *out_scratch_bytes) {
    return kimix_gdb_vertex_properties(session, vertex, items, capacity, scratch, scratch_capacity, out_count, out_scratch_bytes);
}

int32_t kimix_graphdb_vertex_property_types(kimix_gdb_session *session, kimix_gdb_guid vertex, kimix_gdb_property_type *items, size_t capacity, uint8_t *scratch, size_t scratch_capacity, size_t *out_count, size_t *out_scratch_bytes) {
    return kimix_gdb_vertex_property_types(session, vertex, items, capacity, scratch, scratch_capacity, out_count, out_scratch_bytes);
}

int32_t kimix_graphdb_edge_property_add(kimix_gdb_session *session, kimix_gdb_guid edge, uint32_t name, const kimix_gdb_property *value) {
    return kimix_gdb_edge_property_add(session, edge, name, value);
}

int32_t kimix_graphdb_edge_property_set(kimix_gdb_session *session, kimix_gdb_guid edge, uint32_t name, const kimix_gdb_property *value) {
    return kimix_gdb_edge_property_set(session, edge, name, value);
}

int32_t kimix_graphdb_edge_property_update(kimix_gdb_session *session, kimix_gdb_guid edge, uint32_t name, const kimix_gdb_property *value) {
    return kimix_gdb_edge_property_update(session, edge, name, value);
}

int32_t kimix_graphdb_edge_properties_remove(kimix_gdb_session *session, kimix_gdb_guid edge, const uint32_t *names, size_t name_count) {
    return kimix_gdb_edge_properties_remove(session, edge, names, name_count);
}

int32_t kimix_graphdb_edge_properties(kimix_gdb_session *session, kimix_gdb_guid edge, kimix_gdb_property_record *items, size_t capacity, uint8_t *scratch, size_t scratch_capacity, size_t *out_count, size_t *out_scratch_bytes) {
    return kimix_gdb_edge_properties(session, edge, items, capacity, scratch, scratch_capacity, out_count, out_scratch_bytes);
}

int32_t kimix_graphdb_edge_property_types(kimix_gdb_session *session, kimix_gdb_guid edge, kimix_gdb_property_type *items, size_t capacity, uint8_t *scratch, size_t scratch_capacity, size_t *out_count, size_t *out_scratch_bytes) {
    return kimix_gdb_edge_property_types(session, edge, items, capacity, scratch, scratch_capacity, out_count, out_scratch_bytes);
}

// ===========================================================================
// 6. Edges
// ===========================================================================

int32_t kimix_graphdb_edge_create(kimix_gdb_session *session, kimix_gdb_guid origin, kimix_gdb_guid target, uint8_t direction, const uint32_t *labels, size_t label_count, const uint32_t *property_names, const kimix_gdb_property *properties, size_t property_count, kimix_gdb_guid *out_edge) {
 return kimix_gdb_edge_create(session, origin, target, direction, labels, label_count, property_names, properties, property_count, out_edge);
}

int32_t kimix_graphdb_edge_free(kimix_gdb_session *session, kimix_gdb_guid edge) {
    return kimix_gdb_edge_free(session, edge);
}

int32_t kimix_graphdb_edge_set_origin(kimix_gdb_session *session, kimix_gdb_guid edge, kimix_gdb_guid origin) {
    return kimix_gdb_edge_set_origin(session, edge, origin);
}

int32_t kimix_graphdb_edge_set_target(kimix_gdb_session *session, kimix_gdb_guid edge, kimix_gdb_guid target) {
    return kimix_gdb_edge_set_target(session, edge, target);
}

int32_t kimix_graphdb_edge_set_direction(kimix_gdb_session *session, kimix_gdb_guid edge, uint8_t direction) {
    return kimix_gdb_edge_set_direction(session, edge, direction);
}

int32_t kimix_graphdb_edge_endpoints(kimix_gdb_session *session, kimix_gdb_guid edge, kimix_gdb_guid *out_origin, kimix_gdb_guid *out_target) {
    return kimix_gdb_edge_endpoints(session, edge, out_origin, out_target);
}

// ===========================================================================
// 7. Filtered, oriented, paged reads
// ===========================================================================

int32_t kimix_graphdb_edges_of_vertex(kimix_gdb_session *session, kimix_gdb_guid vertex, uint32_t constraint, uint8_t orientation, int64_t cap, kimix_gdb_edge_record *items, size_t capacity, size_t *out_count, size_t *out_total) {
    return kimix_gdb_edges_of_vertex(session, vertex, constraint, orientation, cap, items, capacity, out_count, out_total);
}

int32_t kimix_graphdb_neighbors_of_vertex(kimix_gdb_session *session, kimix_gdb_guid vertex, uint32_t constraint, uint8_t orientation, int64_t cap, kimix_gdb_guid *items, size_t capacity, size_t *out_count, size_t *out_total) {
    return kimix_gdb_neighbors_of_vertex(session, vertex, constraint, orientation, cap, items, capacity, out_count, out_total);
}

int32_t kimix_graphdb_count_neighbors(kimix_gdb_session *session, kimix_gdb_guid vertex, uint32_t constraint, uint8_t orientation, size_t *out_count) {
    return kimix_gdb_count_neighbors(session, vertex, constraint, orientation, out_count);
}

int32_t kimix_graphdb_exists_neighbor(kimix_gdb_session *session, kimix_gdb_guid vertex, uint32_t constraint, uint8_t orientation, uint8_t *out_exists) {
 return kimix_gdb_exists_neighbor(session, vertex, constraint, orientation, out_exists);
}

int32_t kimix_graphdb_traverse_bfs(kimix_gdb_session *session, kimix_gdb_guid start, uint32_t constraint, uint8_t orientation, int64_t cap_per_level, uint32_t max_depth, kimix_gdb_traversal_vertex *items, size_t capacity, size_t *out_count) {
 return kimix_gdb_traverse_bfs(session, start, constraint, orientation, cap_per_level, max_depth, items, capacity, out_count);
}

int32_t kimix_graphdb_shortest_path(kimix_gdb_session *session, kimix_gdb_guid from, kimix_gdb_guid to, uint32_t max_depth, kimix_gdb_guid *items, size_t capacity, size_t *out_count) {
    return kimix_gdb_shortest_path(session, from, to, max_depth, items, capacity, out_count);
}

int32_t kimix_graphdb_shortest_path_edges(kimix_gdb_session *session, kimix_gdb_guid from, kimix_gdb_guid to, uint32_t edge_constraint, uint8_t orientation, uint32_t max_depth, kimix_gdb_path_step *items, size_t capacity, size_t *out_count) {
    return kimix_gdb_shortest_path_edges(session, from, to, edge_constraint, orientation, max_depth, items, capacity, out_count);
}

// ABI 7: the edge-filtered BFS (step 0 stubs the callee; the implementation is
// a later change — the symbol and its contract are frozen from the ABI-7 commit).
int32_t kimix_graphdb_traverse_bfs_edges(kimix_gdb_session *session, kimix_gdb_guid start, uint32_t edge_constraint, uint8_t orientation, uint32_t max_depth, uint32_t max_vertices, kimix_gdb_bfs_vertex *items, size_t vertex_capacity, kimix_gdb_bfs_edge *edges, size_t edge_capacity, size_t *out_vertex_count, size_t *out_edge_count, uint8_t *out_truncated) {
    return kimix_gdb_traverse_bfs_edges(session, start, edge_constraint, orientation, max_depth, max_vertices, items, vertex_capacity, edges, edge_capacity, out_vertex_count, out_edge_count, out_truncated);
}

int32_t kimix_graphdb_scan_vertices(kimix_gdb_session *session, uint32_t constraint, int64_t cap, kimix_gdb_guid *items, size_t capacity, size_t *out_count, size_t *out_total) {
 return kimix_gdb_scan_vertices(session, constraint, cap, items, capacity, out_count, out_total);
}

int32_t kimix_graphdb_aggregate(kimix_gdb_session *session, uint32_t constraint, uint32_t property, int32_t function, uint8_t *scratch, size_t scratch_capacity, kimix_gdb_aggregate_result *out) {
 return kimix_gdb_aggregate(session, constraint, property, function, scratch, scratch_capacity, out);
}

// ===========================================================================
// 8. Transactions (one open transaction per session)
// ===========================================================================

int32_t kimix_graphdb_tx_begin(kimix_gdb_session *session) {
    return kimix_gdb_tx_begin(session);
}

int32_t kimix_graphdb_tx_commit(kimix_gdb_session *session) {
    return kimix_gdb_tx_commit(session);
}

int32_t kimix_graphdb_tx_abort(kimix_gdb_session *session) {
    return kimix_gdb_tx_abort(session);
}

int32_t kimix_graphdb_tx_begin_collective(kimix_gdb_session *session) {
    return kimix_gdb_tx_begin_collective(session);
}

int32_t kimix_graphdb_tx_close_collective(kimix_gdb_session *session, uint8_t commit) {
    return kimix_gdb_tx_close_collective(session, commit);
}

// ===========================================================================
// 9. Schema: labels and property types
// ===========================================================================

int32_t kimix_graphdb_label_create(kimix_gdb_session *session, const char *name, size_t name_len, uint32_t *out_label) {
    return kimix_gdb_label_create(session, name, name_len, out_label);
}

int32_t kimix_graphdb_label_find(kimix_gdb_session *session, const char *name, size_t name_len, uint32_t *out_label, uint8_t *out_found) {
    return kimix_gdb_label_find(session, name, name_len, out_label, out_found);
}

int32_t kimix_graphdb_label_get_all(kimix_gdb_session *session, kimix_gdb_label *items, size_t capacity, uint8_t *scratch, size_t scratch_capacity, size_t *out_count, size_t *out_scratch_bytes) {
    return kimix_gdb_label_get_all(session, items, capacity, scratch, scratch_capacity, out_count, out_scratch_bytes);
}

int32_t kimix_graphdb_label_update(kimix_gdb_session *session, uint32_t label, const char *name, size_t name_len) {
    return kimix_gdb_label_update(session, label, name, name_len);
}

int32_t kimix_graphdb_label_free(kimix_gdb_session *session, uint32_t label) {
    return kimix_gdb_label_free(session, label);
}

int32_t kimix_graphdb_label_name(kimix_gdb_session *session, uint32_t label, char *out, size_t capacity, size_t *out_required) {
    return kimix_gdb_label_name(session, label, out, capacity, out_required);
}

int32_t kimix_graphdb_property_type_create(kimix_gdb_session *session, const char *name, size_t name_len, uint8_t container, uint8_t dtype, uint32_t count, uint32_t *out_type) {
    return kimix_gdb_property_type_create(session, name, name_len, container, dtype, count, out_type);
}

int32_t kimix_graphdb_property_type_find(kimix_gdb_session *session, const char *name, size_t name_len, uint32_t *out_type, uint8_t *out_found) {
    return kimix_gdb_property_type_find(session, name, name_len, out_type, out_found);
}

int32_t kimix_graphdb_property_type_get_all(kimix_gdb_session *session, kimix_gdb_property_type *items, size_t capacity, uint8_t *scratch, size_t scratch_capacity, size_t *out_count, size_t *out_scratch_bytes) {
    return kimix_gdb_property_type_get_all(session, items, capacity, scratch, scratch_capacity, out_count, out_scratch_bytes);
}

int32_t kimix_graphdb_property_type_update(kimix_gdb_session *session, uint32_t type, const char *name, size_t name_len, uint8_t container, uint8_t dtype, uint32_t count) {
    return kimix_gdb_property_type_update(session, type, name, name_len, container, dtype, count);
}

int32_t kimix_graphdb_property_type_free(kimix_gdb_session *session, uint32_t type) {
    return kimix_gdb_property_type_free(session, type);
}

// ===========================================================================
// 10. Constraints (session-owned handles, dense ids)
// ===========================================================================

int32_t kimix_graphdb_constraint_create(kimix_gdb_session *session, uint32_t *out_constraint) {
    return kimix_gdb_constraint_create(session, out_constraint);
}

int32_t kimix_graphdb_constraint_add_label(kimix_gdb_session *session, uint32_t constraint, uint32_t label, uint8_t op) {
    return kimix_gdb_constraint_add_label(session, constraint, label, op);
}

int32_t kimix_graphdb_constraint_add_property(kimix_gdb_session *session, uint32_t constraint, uint32_t property, uint8_t op, const kimix_gdb_property *value, uint32_t count) {
    return kimix_gdb_constraint_add_property(session, constraint, property, op, value, count);
}

int32_t kimix_graphdb_constraint_free(kimix_gdb_session *session, uint32_t constraint) {
    return kimix_gdb_constraint_free(session, constraint);
}

// ===========================================================================
// 11. Bulk
// ===========================================================================

int32_t kimix_graphdb_vertices_create_bulk(kimix_gdb_session *session, const kimix_gdb_vertex_spec *specs, size_t count, uint8_t continue_on_error, kimix_gdb_guid *out_uids, int32_t *out_status, uint8_t *out_warning) {
    return kimix_gdb_vertices_create_bulk(session, specs, count, continue_on_error, out_uids, out_status, out_warning);
}

int32_t kimix_graphdb_edges_create_bulk(kimix_gdb_session *session, const kimix_gdb_edge_spec *specs, size_t count, uint8_t continue_on_error, kimix_gdb_guid *out_uids, int32_t *out_status) {
    return kimix_gdb_edges_create_bulk(session, specs, count, continue_on_error, out_uids, out_status);
}

KIMIX_FFI_END
