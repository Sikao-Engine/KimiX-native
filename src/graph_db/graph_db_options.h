/*
 * graph_db_options.h — GraphOptions: the tunables of one database instance.
 *
 * Everything a caller might reasonably want to change lives here from the first
 * phase on, even where a later phase is the one that starts honouring a field.
 * That is deliberate: the options travel through the C ABI's open() struct as
 * well, so adding a field later would be an ABI change; adding it now (with the
 * documented default) is not.
 *
 * FIELDS
 *   auto_create_schema        implicit label / property-type creation on write.
 *                             true (the container-friendly default): adding a
 *                             label that is not registered creates it. false (the
 *                             GDI-strict posture): an unregistered name is
 *                             error_label / error_property_type.
 *   key_mode                  what create_vertex does with a duplicate
 *                             (label, external_id) key: refuse it
 *                             (error_non_unique_id, the GDI default) or return
 *                             the existing vertex with warning_non_unique_id
 *                             (the SimulateNonUniqueId path).
 *   allow_parallel_edges      applies to create_edge only, never to the legacy
 *                             connect(): the container API keeps its
 *                             one-edge-per-ordered-pair rule.
 *   allow_self_edges          same scoping: create_edge honours it, connect()
 *                             still refuses from == to.
 *   max_journal_bytes         cap on one transaction's undo journal; exceeding
 *                             it yields error_size_limit with the transaction
 *                             still abortable.
 *   cold_map_size                 LMDB map size handed to the backend
 *                                 (both: ColdStorage and the second backend).
 *   store_backend                 which cold-storage backend open() builds: lmdb
 *                                 (the default, unchanged behavior) or flat_file
 *                                 (the SECOND backend: its own LMDB environment at
 *                                 <dir>\graph.xdb, see file_cold_store.h). C++-only:
 *                                 the C ABI has no field for it, so its default
 *                                 keeps ABI behavior identical.
 *   persist_on_commit         commit() runs GraphSerializer::save_incremental.
 *   graph_name                the name recorded in the store's meta record and
 *                             used by the (later) named-graph catalog.
 *
 * Example:
 *   kimix::graph_db::GraphOptions options;
 *   options.auto_create_schema = false;   // GDI-strict: names must exist
 *   options.key_mode = kimix::graph_db::KeyMode::report_warning;
 */
#pragma once

#include "graph_db_api.h"

#include <core/stl/string.h>

#include <cstddef>
#include <cstdint>

namespace kimix::graph_db {

  // What a duplicate (label, external_id) key does.
  enum class KeyMode : uint8_t {
      // Refuse the create: error_non_unique_id (GDI's EnforcesUniqueExternalIds).
      enforce_unique = 0,
      // Return the EXISTING vertex's uid plus warning_non_unique_id (GDI-RMA's
      // under-enforced uniqueness, surfaced as a warning).
      report_warning = 1,
  };
  // Which digest caches a vertex's content (plan v3 §4.5). Both produce a
  // 128-bit value stored in the same 16-byte record field, so the on-disk
  // format is identical — the choice only changes the VALUES. Cross-policy
  // use is benign: the loader recomputes the cached hash at materialization,
  // loaded vertices stay clean, and a dirty vertex is saved under the
  // graph's own policy, so the post-write clear-dirty comparison keeps
  // matching.
  enum class ContentDigest : uint8_t {
      md5 = 0,   // the historical default (RFC 1321, ~3 block transforms any size)
      xxh3 = 1,  // XXH3-128 (~20x faster on small blobs; non-cryptographic)
  };
  // Which cold-storage backend Database::open builds (cold_store.h is the seam
  // both backends implement). C++-only: the C ABI's kimix_gdb_open_options has no
  // field for it, so a caller through graph_c_api.h always gets the default (lmdb)
  // — the ABI's observable behavior is unchanged.
  enum class ColdStoreBackend : uint8_t {
      // The LMDB environment under the store directory (utils.h ColdStorage):
      // the historical default, 64 GiB map, per-transaction write-through pages.
      lmdb = 0,
        // The SECOND backend of file_cold_store.h: an LMDB environment of its own at
        // <dir>\graph.xdb, so one directory can hold it next to an lmdb store and a
        // checkpoint is one directory copy. Opt-in; see that header for the layout
        // and the limits.
        flat_file = 1,
  };
  struct KIMIX_GRAPH_DB_API GraphOptions {
      bool auto_create_schema{true};
      KeyMode key_mode{KeyMode::enforce_unique};
      bool allow_parallel_edges{true};
      bool allow_self_edges{true};
        ContentDigest content_digest{ContentDigest::md5};
        // 64 MiB of undo journal per transaction (see max_journal_bytes).
    size_t max_journal_bytes{64ull * 1024ull * 1024ull};
      // ColdStorage::default_map_size (64 GiB); 0 means "keep the storage default".
      // Both backends take it: the second one opens its own LMDB environment with it.
      size_t cold_map_size{0};
      // Which cold-store backend Database::open builds. lmdb keeps every existing
      // behavior byte for byte; flat_file swaps in the second backend (its own LMDB
      // environment at <dir>\graph.xdb).
      ColdStoreBackend store_backend{ColdStoreBackend::lmdb};
    bool persist_on_commit{false};
    kimix::string graph_name;
};

} // namespace kimix::graph_db
