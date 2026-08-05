-- Extensions xmake.lua
-- Includes all vendored third-party extensions.

-- ============================================================================
-- reproc (subprocess library)
-- ============================================================================
includes("reproc")

-- ============================================================================
-- spdlog (logging library)
-- ============================================================================
includes("spdlog")

-- ============================================================================
-- mimalloc (memory allocator) — compiled directly into yyjson
-- ============================================================================
local mimalloc_dir = path.join(os.scriptdir(), "mimalloc")
target("mimalloc")
    _config_project({
        project_kind = "object",
        no_rtti = true
    })
    add_files(path.join(mimalloc_dir, "src/static.c"))
    add_includedirs(path.join(mimalloc_dir, "include"), {public = true})
    -- Both MI_SHARED_LIB and MI_SHARED_LIB_EXPORT must be public so dependents
    -- get __declspec(dllexport) (works for static linking) not __declspec(dllimport)
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
-- yyjson (JSON library) — uses mimalloc as memory allocator
-- ============================================================================
target("kimix-yyjson")
    _config_project({
        project_kind = "static"
    })
    add_deps("mimalloc")
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
-- marl (fiber-based task scheduler) — compiled as a static library
-- ============================================================================
target("kimix-marl")
    _config_project({
        project_kind = "static"
    })
    on_load(function(target)
        local marl_dir = path.join(os.scriptdir(), "marl")
        -- Amalgamated translation unit (includes all non-Windows-specific sources)
        target:add("files", path.join(marl_dir, "src/build.marl.cpp"))
        -- Only include architecture-specific osfiber sources for the target arch.
        -- Globbing all *.c and *.S causes cross-arch compile errors.
        if not target:is_plat("windows") then
            local arch = target:arch()
            if arch == "arm64" or arch == "aarch64" or arch == "arm64-v8a" then
                target:add("files", path.join(marl_dir, "src/osfiber_aarch64.c"))
                target:add("files", path.join(marl_dir, "src/osfiber_asm_aarch64.S"))
            elseif arch == "x86_64" or arch == "x64" then
                target:add("files", path.join(marl_dir, "src/osfiber_x64.c"))
                target:add("files", path.join(marl_dir, "src/osfiber_asm_x64.S"))
            elseif arch == "x86" or arch == "i386" then
                target:add("files", path.join(marl_dir, "src/osfiber_x86.c"))
                target:add("files", path.join(marl_dir, "src/osfiber_asm_x86.S"))
            elseif arch == "arm" or arch == "armv7" or arch == "armeabi-v7a" then
                target:add("files", path.join(marl_dir, "src/osfiber_arm.c"))
                target:add("files", path.join(marl_dir, "src/osfiber_asm_arm.S"))
            elseif arch == "mips64" or arch == "mips64el" then
                target:add("files", path.join(marl_dir, "src/osfiber_mips64.c"))
                target:add("files", path.join(marl_dir, "src/osfiber_asm_mips64.S"))
            elseif arch == "loongarch64" then
                target:add("files", path.join(marl_dir, "src/osfiber_loongarch64.c"))
                target:add("files", path.join(marl_dir, "src/osfiber_asm_loongarch64.S"))
            elseif arch == "riscv64" or arch == "rv64" then
                target:add("files", path.join(marl_dir, "src/osfiber_rv64.c"))
                target:add("files", path.join(marl_dir, "src/osfiber_asm_rv64.S"))
            elseif arch == "ppc64" or arch == "ppc64le" then
                target:add("files", path.join(marl_dir, "src/osfiber_ppc64.c"))
                target:add("files", path.join(marl_dir, "src/osfiber_asm_ppc64.S"))
            end
        end
        target:add("includedirs", path.join(marl_dir, "include"), {
            public = true
        })
        -- This project does not vendor EASTL; marl must use the system STL.
        target:add("defines", "MARL_USE_SYSTEM_STL", {public = true})
        if target:is_plat("linux") then
            target:add("syslinks", "pthread", {public = true})
        end
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
