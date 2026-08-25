-- Extensions xmake.lua
-- Includes all vendored third-party extensions.

-- ============================================================================
-- mimalloc (memory allocator)
-- ============================================================================
local mimalloc_dir = path.join(os.scriptdir(), "mimalloc")
target("mimalloc")
    _config_project({
        project_kind = "object",
        no_rtti = true
    })
    add_files(path.join(mimalloc_dir, "src/static.c"))
    add_includedirs(path.join(mimalloc_dir, "include"), {public = true})
    -- Shared-heap design: mimalloc is exported from runtime_py.pyd and imported
    -- by every test executable (MI_SHARED_LIB(_EXPORT) public gives plain
    -- __declspec(dllexport) declarations to dependents, never dllimport). This
    -- keeps ONE mimalloc instance across the pyd and its native C++ tests, so
    -- kimix::string values passed across the boundary free in the right heap.
    add_defines("MI_SHARED_LIB", {public = true})
    add_defines("MI_XMALLOC=1", "MI_WIN_NOREDIRECT", "MI_SHARED_LIB_EXPORT", {public = true})
    if is_plat("windows") then
        add_syslinks("advapi32", "bcrypt", {public = true})
        add_defines("_CRT_SECURE_NO_WARNINGS")
    elseif is_plat("linux") then
        add_syslinks("pthread", "atomic", {public = true})
        add_defines("MI_NO_THP")
    else
        add_syslinks("pthread", {public = true})
    end
target_end()

-- ============================================================================
-- yyjson (JSON library)
-- ============================================================================
target("kimix-yyjson")
    _config_project({
        project_kind = "static"
    })
    on_load(function(target)
        local src_path = path.join(os.scriptdir(), "yyjson/src")
        target:add("files", path.join(src_path, "yyjson.c"))
        target:add("includedirs", src_path, {
            public = true
        })
        target:add("cxflags", "/utf-8", {
            tools = "cl"
        })
    end)
target_end()

-- ============================================================================
-- xxHash (fast hash library) — header-only, included by kimix-core
-- ============================================================================
target("kimix-xxhash")
    set_kind("headeronly")
    on_load(function(target)
        target:add("includedirs", path.join(os.scriptdir(), "xxHash"), {
            public = true
        })
    end)
target_end()

-- ============================================================================
-- pybind11 (Python binding) — header-only
-- ============================================================================
target("kimix-pybind11")
    set_kind("headeronly")
    on_load(function(target)
        target:add("includedirs", path.join(os.scriptdir(), "pybind11/include"), {
            public = true
        })
    end)
target_end()

-- ============================================================================
-- cpp-httplib (HTTP/HTTPS client & server) — header-only
-- ============================================================================
target("kimix-cpp-httplib")
    set_kind("headeronly")
    on_load(function(target)
        target:add("includedirs", path.join(os.scriptdir(), "cpp-httplib"), {
            public = true
        })
    end)
target_end()
