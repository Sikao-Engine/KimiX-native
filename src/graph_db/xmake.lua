-- Kimix graph database (src/graph_db)
--
-- Target: kimix-graph-db — a STATIC library holding the in-memory graph layer,
-- its cold-storage glue and the GDI backend facade, on top of kimix-core only:
--
--   graph_db_api.h   the KIMIX_GRAPH_DB_API linkage-macro family (three states,
--                    same shape as KIMIX_CORE_API in src/core/dll_export.h).
--                    The target is static, so it defines KIMIX_GRAPH_DB_STATIC
--                    publicly and the macro expands to nothing for the library
--                    and for every consumer.
--   graph_db.h       umbrella include + KIMIX_GRAPH_DB_VERSION +
--                    KIMIX_GRAPH_DB_FORMAT_VERSION.
--   utils.h/.cpp     key spaces ("v/", "e/", "s/", "meta/root", "i/" reserved),
--                    the v1 (GDB1) and v2 (GDB2) vertex-record codecs, the meta
--                    record codec and ColdStorage — a thin RAII front end over
--                    kimix::LMDB (src/core/lmdb.h) with batched transactional
--                    writes, prefix scans and copy_to() checkpoints.
--   vertex.h/.cpp    one vertex: guid, opaque content blob + cached MD5, the
--                    typed payload (labels, properties), the immutable
--                    (label, external_id) key, the edge-link adjacency, a
--                    64-byte-separated kimix::shared_atomic_mutex and the dirty
--                    flag.
-- Lock KIND: one kimix::shared_atomic_mutex per vertex, one per key-index
-- shard and one for the schema registry; the vertex/edge shards and the
-- roots cache stay on the blocking std::shared_mutex: graph.h's LOCK KIND
-- POLICY is the admission test, README.md §3b the per-object summary
--   edge.h/.cpp      first-class edges: uid, endpoints, direction, labels,
--                    properties, the sharded EdgeTable and the "GDE2" codec.
--   key.h/.cpp       ExternalId (16-byte SSO), VertexKey and the sharded KeyIndex.
--   (kimix::shared_atomic_mutex per shard: one dense-map op per holder).
--   graph.h/.cpp     the container: structural CRUD with a cascading remove,
--                    connect/disconnect/connect_bulk through the edge table,
--                    the edge + key API, roots, neighbors/dependencies,
--                    cycle-safe BFS, dirty bookkeeping, and the five-level lock
--                    hierarchy (see README.md §5b).
--   serialize.h/.cpp GraphSerializer / GraphDeserializer (v2 records, the edge
--                    family, the schema tables and the meta record).
--   status.h/.cpp    GdiStatus: the 63 GDI status codes, pinned by a test.
--   property.h/.cpp  PropertyValue / PropertyBag, the GDI type vocabulary,
--                    evaluate() and the record-level property codec.
--   schema.h/.cpp    label / property-type / constraint registries + codecs.
--   (one kimix::shared_atomic_mutex; lookups build their probe before the
--   lock and _version is a relaxed atomic).
--   query.h/.cpp     Page<T> and the constraint evaluation.
--   transaction.h/.cpp  the undo journal.
--   database.h/.cpp  Database: the GDI backend facade (one per session).
--   capability.h/.cpp   the honest capability advertisement.
--   graph_db_options.h  GraphOptions / KeyMode.
--   graph_c_api.h/.cpp  the C ABI (ASCII, C99-clean, two-phase buffers).
--   binary_le.h      INTERNAL little-endian helpers (not in the umbrella).
--   reports/gdi_alignment.md  the graph_report.md §2.1-§2.16 checklist.
--
-- Dependency rule: kimix-core only. Third-party code is reached through it
-- (LMDB's C sources are already compiled into kimix-core, mimalloc backs
-- kimix::allocator), so this target never add_deps() a src/ext target — the
-- same rule kimix-llm and kimix_api follow.
--
-- New sources need no change to this file (`add_files("*.cpp")` picks them up),
-- and neither does a new header (`add_headerfiles("*.h")`); only the test
-- registrations in tests/xmake.lua.
--
-- Build:   xmake build kimix-graph-db   ->  bin/<mode>/libkimix-graph-db.a
target("kimix-graph-db")
    set_kind("static")
    add_files("*.cpp")
    add_headerfiles("*.h")
    -- `src/` on the (public) include path: this module's own sources include
    -- its headers as "graph.h"/"utils.h" (same directory) and core as
    -- <core/lmdb.h>, and a dependent target gets the identical spelling —
    -- #include <graph_db/graph_db.h>.
    add_includedirs("..", { public = true })
    -- `.` public as well, so a consumer may also include the flat spelling
    -- "graph_db.h" (documented in graph_db.h's own example).
    add_includedirs(".", { public = true })
    add_deps("kimix-core")
    -- Standalone kimix-core propagates KIMIX_CORE_STATIC to this archive/users.
    -- Static linkage of this library's own symbols: KIMIX_GRAPH_DB_API
    -- expands to nothing (src/graph_db/graph_db_api.h).
    add_defines("KIMIX_GRAPH_DB_STATIC", { public = true })
      -- Optional walk instrumentation (root xmake.lua option
      -- graph_db_walk_counters, optimize_plan.md §3.2). PUBLIC, so the
      -- benchmark that reads the counters — and only it — compiles against the
      -- same record as the library. OFF by default: the structure, the accessor
      -- and every increment in graph.cpp are inside #if, so a default build has
      -- no counter in any timing path.
      if has_config("graph_db_walk_counters") then
          add_defines("KIMIX_GRAPH_DB_WALK_COUNTERS", { public = true })
      end
    on_load(function(target)
        -- KIMIX_GRAPH_DB_VERSION comes from version.txt, the single source of
        -- truth for the project version (same file src/xmake.lua turns into
        -- kimix_version.h). A missing/unreadable file falls back to "0.0.0",
        -- which is also the header's #ifndef fallback.
        local version_txt = path.join(os.projectdir(), "version.txt")
        local text = os.isfile(version_txt) and io.readfile(version_txt) or nil
        local version = text and text:gsub("^%s*(.-)%s*$", "%1") or "0.0.0"
        target:add("defines", 'KIMIX_GRAPH_DB_VERSION="' .. version .. '"', { public = true })
        if target:is_plat("windows") then
            target:add("defines", "NOMINMAX", "_CRT_SECURE_NO_WARNINGS", { public = true })
        end
    end)
    _config_project({ batch_size = 8, project_kind = "static" })
target_end()