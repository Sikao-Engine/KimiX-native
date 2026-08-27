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

-- ============================================================================
-- reproc (cross-platform C99/C++11 subprocess library) — vendored at
-- src/ext/reproc (https://github.com/LuisaGroup/reproc.git, same fork + pin as
-- C:/dev/LuisaCompute). Built as a static lib named "kimix-reproc" with the
-- same layout as LuisaCompute/src/ext/reproc/xmake.lua: the C sources are
-- selected by platform suffix (".posix" vs ".windows") and the C++ wrapper
-- (reproc++) is compiled from reproc++/src.  Static (not shared) so neither
-- REPROC_SHARED nor REPROC_BUILDING is defined; export.h then expands
-- REPROC_EXPORT to nothing, which is exactly what a static link needs.
-- ============================================================================
local reproc_dir = path.join(os.scriptdir(), "reproc")
target("kimix-reproc")
    _config_project({
        project_kind = "static"
    })
    -- NOTE: this target lives in src/ext/xmake.lua (not in the submodule's own
    -- xmake.lua), so every relative path must be anchored at os.scriptdir()
    -- (= src/ext); plain "reproc/..." would resolve against src/ext as well
    -- but add_files/add_includedirs are evaluated at parse time, so build them
    -- explicitly with path.join for clarity.
    add_headerfiles(path.join(reproc_dir, "reproc/include/**.h"))
    add_headerfiles(path.join(reproc_dir, "reproc++/include/**.hpp"))
    add_includedirs(path.join(reproc_dir, "reproc/include/"),
                    path.join(reproc_dir, "reproc++/include/"), {
        public = true
    })
    add_rules("c++.build", "c.build")
    on_config(function(target)
        local src_path = path.join(reproc_dir, "reproc/src")
        local keyword
        if is_plat("windows") then
            keyword = ".windows"
        else
            keyword = ".posix"
        end
        for _, filepath in ipairs(os.files(path.join(src_path, "*.c"))) do
            local file_name = path.filename(filepath)
            file_name = file_name:sub(1, #file_name - 2)
            local ext = path.extension(file_name)
            if (#ext == 0 or ext == keyword) then
                target:add("files", filepath)
            end
        end
        if is_plat("windows") then
            target:add("links", "ws2_32", {public = true})
        else
            target:add("syslinks", "pthread", {public = true})
        end
    end)
    add_files(path.join(reproc_dir, "reproc++/src/reproc.cpp"))
target_end()

-- ============================================================================
-- Mbed TLS (TLS/crypto) — vendored; compiled purely by xmake (no perl/scripts).
-- ============================================================================
target("kimix-mbedtls")
    set_kind("static")
    add_rules("kimix_basic_settings") -- project-wide flags, but NO unity build
    on_load(function(target)
        local dir = path.join(os.scriptdir(), "mbedtls")
        target:add("files", path.join(dir, "library/*.c"))
        target:add("includedirs", path.join(dir, "include"), {public = true})
        if target:is_plat("windows") then
            target:add("defines", "_CRT_SECURE_NO_WARNINGS")
            target:add("syslinks", "ws2_32", "crypt32", {public = true})
        end
    end)
target_end()
