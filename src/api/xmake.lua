-- Kimix C-FFI library (src/api)
--
-- Target: kimix_api — a SHARED library that exposes a plain C ABI on top of
-- kimix-core, so a foreign caller (C, C#, Zig, Rust, Python ctypes/cffi, Julia,
-- Node ffi-napi, ...) can reach the kimix base facilities without any C++ or
-- pybind11 in the picture.  The surface is documented function-by-function in
-- docs/ffi.md and declared in the pure-C headers under src/api.
--
-- What it exports (one header + one translation unit per group):
--   ffi_common.h    the C-FFI basement: KIMIX_FFI linkage macro (from
--                   src/core/dll_export.h), extern "C" wrappers, ABI constants,
--                   library/version queries                 -> kimix_api.cpp
--   ffi_mem.h       the mimalloc allocation functions (src/ext/mimalloc,
--                   mimalloc.h) behind stable kimix_mem_* names
--                                                          -> ffi_mem.cpp
--   ffi_vec.h       kimix::vector<std::byte> as an inline "placeholder" object
--                   the caller stores in its own memory: placement new,
--                   move construction and destruction across the ABI
--                                                          -> ffi_vec.cpp
--   ffi_map.h       an opaque uint64 -> uint64 map handle over
--                   kimix::unordered_map (core/stl/unordered_dense.h) with the
--                   dense-order walk and per-instance byte accounting
--                                                          -> ffi_map.cpp
--   ffi_parallel.h  kimix::fiber (core/fiber.h over the vendored marl pool):
--                   run N C-callable jobs over the workers, with a task cap and
--                   a caller-owned cancel flag; transiently binds the shared
--                   pool for a foreign thread (never creates one)
--                                                          -> ffi_parallel.cpp
--   ffi_yyjson.h    the yyjson read/build/write surface with the mimalloc
--                   allocator BAKED IN (no yyjson_alc ever crosses the ABI)
--                                                         -> ffi_yyjson.cpp
--   ffi_repair.h    kimix::repair() (src/core/json_repair.h), returning the
--                   repaired bytes into a kimix_vec placeholder
--                                                        -> ffi_repair.cpp
-- ffi_canonical.h deterministic canonical JSON: strict yyjson parse (raw
--                   numbers) + key-sorted, minified re-emit into a kimix_vec
--                                                        -> ffi_canonical.cpp
--   ffi_sqlite.h     the SQLite3 database C API re-exported over the vendored
--                   amalgamation (kimix-sqlite3): open/close, one-shot exec,
--                   prepare/step/finalize with parameter binding and typed
--                   column access; the only caller-owned buffer is the exec
--                   error message (mimalloc, freed with kimix_mem_free)
--                                                        -> ffi_sqlite.cpp
--   ffi_graphdb.h    the kimix-graph-db C ABI re-exported one line per entry
--                   point (kimix-graph-db): the graph_c_api.h surface under
--                   house KIMIX_FFI names, so a foreign caller gets the whole
--                   GDI graph vocabulary (int32_t status codes, two-phase
--                   caller-owned buffers) from this image; the session is the
--                   only allocated object, released by kimix_graphdb_close
--                                                        -> ffi_graphdb.cpp
--   ffi_lua.h        the Lua script sandbox (kimix-script): one opaque handle
--                   per worker thread, create -> run -> read the result ->
--                   destroy, plus the HOST SURFACE (kimix_lua_host_bind /
--                   kimix_lua_global_set / kimix_lua_globals_clear /
--                   kimix_lua_run_call) that lets a C caller extend a run with a
--                   callback and drive a named Lua function without splicing
--                   text into the source.  A chunk cannot touch the file system,
--                   spawn a process, load a module, reach debug introspection,
--                   obtain an address or run unbounded; a denial aborts the run
--                   and comes back as KIMIX_LUA_DENIED plus the documented
--                   message                              -> ffi_lua.cpp
--
-- Dependency rule: kimix_api depends on kimix-core for everything core-ish —
-- never on a src/ext target directly for those (mimalloc/yyjson, and the marl
-- objects that ffi_parallel.h fans jobs over, arrive through core, exactly
-- like every other kimix library; see the note at the top of the kimix-core
-- target).  The exceptions are kimix-sqlite3 and kimix-graph-db: ffi_sqlite.h
-- wraps the amalgamation itself and ffi_graphdb.h forwards into the graph
-- archive, so both targets are declared here the same way kimix-llm declares
-- its own extra third-party deps (kimix-mbedtls / kimix-reproc /
-- kimix-sqlite3).  The sqlite3 include directory arrives through the
-- kimix-sqlite3 target's public includedirs; <graph_db/graph_c_api.h> through
-- kimix-graph-db's.
--
-- Heap ownership: standalone kimix-core and mimalloc are statically linked
-- into this shared image; the external leg uses their shared libraries. Every
-- buffer the FFI hands out belongs to that allocation path. A caller must
-- therefore free FFI buffers with kimix_mem_free()
-- / kimix_yyjson_str_free() / kimix_vec_*(), never with its own malloc/free.
--
-- Feature switch: --kimix_enable_api=false.  It is a FILE-LEVEL skip:
-- src/xmake.lua does not even includes("api") then, so this target and its
-- headers leave the configuration entirely (test_kimix_api in
-- tests/xmake.lua repeats the same guard because it links this library).
--
-- Build / use:
--   xmake build kimix_api                    -> bin/<mode>/kimix_api.dll (or
--                                               libkimix_api.so on Linux)
--   xmake f --kimix_enable_api=false -c      -> the target disappears
target("kimix_api")
    set_kind("shared")
    add_files("*.cpp")
    add_headerfiles("*.h")
    -- `src/` on the (public) include path: sources include their own headers as
    -- <api/ffi_*.h> and core as <core/dll_export.h>, and a dependent target gets
    -- the same spelling.
    add_includedirs("..", {public = true})
    -- Static archives linked into this shared image are compiled with -fPIC.
    if is_plat("linux") then
        add_cxflags("-fPIC", {public = true})
    end
    add_deps("kimix-core")
    -- The SQLite FFI wraps the amalgamation directly (see the header comment
    -- above); kimix-sqlite3 is always built (it is an input, like
    -- kimix-core), so this dep gates nothing and costs nothing when the
    -- option that disables this target is off.
    add_deps("kimix-sqlite3")
    -- The graphdb family forwards into the graph archive (its C ABI has no
    -- export macro of its own, so linking the archive is what makes the
    -- kimix_graphdb_* re-exports callable).  kimix-graph-db is likewise
    -- always built.
    add_deps("kimix-graph-db")
    -- ffi_lua.h owns the sandbox handle over kimix::script::Sandbox, so the
    -- archive (and, transitively, the vendored `lua` target it links) becomes
    -- part of this image.  kimix-script is always built.
    add_deps("kimix-script")
    on_load(function(target)
        -- Our own symbols are dllexport'd from this image; a consumer without
        -- either macro sees dllimport (src/core/dll_export.h, KIMIX_API_API).
        -- A dlopen/LoadLibrary user compiles the headers with KIMIX_API_STATIC
        -- instead to get plain extern declarations.
        target:add("defines", "KIMIX_API_EXPORT_DLL")
        -- Standalone kimix-core propagates KIMIX_CORE_STATIC; the external leg
        -- instead imports the shared core and its process-wide state.
        if target:is_plat("windows") then
            target:add("defines", "NOMINMAX", "_CRT_SECURE_NO_WARNINGS", {public = true})
        elseif target:is_plat("linux") then
            -- $ORIGIN resolves the external leg's shared kimix/luisa-core
            -- companions when this library is dlopen'd. Lua is static.
            target:add("rpathdirs", "$ORIGIN")
        end
    end)
    _config_project({batch_size = 8, project_kind = "shared"})
target_end()
