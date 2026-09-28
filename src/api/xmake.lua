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
--   ffi_yyjson.h    the yyjson read/build/write surface with the mimalloc
--                   allocator BAKED IN (no yyjson_alc ever crosses the ABI)
--                                                         -> ffi_yyjson.cpp
--   ffi_repair.h    kimix::repair() (src/core/json_repair.h), returning the
--                   repaired bytes into a kimix_vec placeholder
--                                                        -> ffi_repair.cpp
--
-- Dependency rule: kimix_api depends on kimix-core only — never on a src/ext
-- target directly (mimalloc/yyjson arrive through core, exactly like every
-- other kimix library; see the note at the top of the kimix-core target).
--
-- Heap ownership: the mimalloc objects live INSIDE this DLL (MI_SHARED_LIB /
-- MI_SHARED_LIB_EXPORT are propagated by the mimalloc target, so the mi_*
-- definitions are compiled into kimix_api and every buffer the FFI hands out is
-- allocated from that single heap).  A caller must therefore free FFI buffers
-- with kimix_mem_free() / kimix_yyjson_str_free() / kimix_vec_*() and never
-- with the C runtime malloc/free pair of its own module.
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
    -- The static archives this library links (kimix-core, mimalloc objects) are
    -- compiled with -fPIC for exactly that reason (see src/xmake.lua).
    if is_plat("linux") then
        add_cxflags("-fPIC", {public = true})
    end
    add_deps("kimix-core")
    on_load(function(target)
        -- Our own symbols are dllexport'd from this image; a consumer without
        -- either macro sees dllimport (src/core/dll_export.h, KIMIX_API_API).
        -- A dlopen/LoadLibrary user compiles the headers with KIMIX_API_STATIC
        -- instead to get plain extern declarations.
        target:add("defines", "KIMIX_API_EXPORT_DLL")
        -- kimix-core is consumed as the static copy that lives inside this DLL
                  -- (same convention as kimix-test and the unit tests), and
        -- publicly so a dependent target keeps resolving core as a static copy
        -- rather than as an import of this library's re-exports.
        target:add("defines", "KIMIX_CORE_STATIC", {public = true})
        if target:is_plat("windows") then
            target:add("defines", "NOMINMAX", "_CRT_SECURE_NO_WARNINGS", {public = true})
        end
    end)
    _config_project({batch_size = 8, project_kind = "shared"})
target_end()
