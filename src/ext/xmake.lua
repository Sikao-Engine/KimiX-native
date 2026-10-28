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
    -- MI_TRACK_ASAN (root option `kimix_mi_track_asan`, default OFF): hand every
    -- mi_malloc/mi_free block to ASan via mi_track_malloc_size/mi_track_free_size
    -- (include/mimalloc/track.h:63) so an --policies=build.sanitizer.address
    -- build sees this project's real heap. Without it, MI_WIN_NOREDIRECT above
    -- means the CRT allocator is untouched and ASan only covers stack/globals/
    -- CRT blocks. It is a PRIVATE define on purpose: it changes how mimalloc
    -- itself is compiled (and pulls in <sanitizer/asan_interface.h>), so it must
    -- not propagate to dependents.
    if has_config("kimix_mi_track_asan") then
        add_defines("MI_TRACK_ASAN=1")
    end
    if is_plat("windows") then
        add_syslinks("advapi32", "bcrypt", {public = true})
        add_defines("_CRT_SECURE_NO_WARNINGS")
    elseif is_plat("linux") then
        add_syslinks("pthread", "atomic", {public = true})
        -- MI_NO_THP must NOT be defined here: vendored mimalloc 3.6.0
        -- (src/ext/mimalloc, prim/unix/prim.c) leaves a dangling `else` when
        -- MI_NO_THP is combined with MADV_NOHUGEPAGE (always defined on Linux
        -- glibc), so the unix primitive translation unit fails to compile.
        -- The THP madvise path stays runtime-gated by mi_option_allow_thp
        -- (default off) and MADV_NOHUGEPAGE prevention still applies, so
        -- dropping the define does not change allocator behavior.
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
-- ============================================================================
-- pdqsort (Orson Peters' pattern-defeating quicksort, vendored copy as shipped
-- by LuisaCompute) — header-only; kimix-core re-exports it through
-- core/stl/pdqsort.h, consumers call it via `pdqsort(begin, end)`.
-- ============================================================================
target("kimix-pdqsort")
    set_kind("headeronly")
    on_load(function(target)
        target:add("includedirs", path.join(os.scriptdir(), "pdqsort"), {
            public = true
        })
    end)
target_end()

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
    -- Linked into the runtime_py shared module: Linux requires -fPIC objects.
    if is_plat("linux") then
        add_cxflags("-fPIC", {public = true})
    end
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
-- marl (fibers + work-stealing scheduler) — vendored at src/ext/marl
-- (https://github.com/LuisaGroup/marl.git, same fork + pin as
  -- C:/dev/compute/src/ext/marl, commit 4ed34cc). Built as an OBJECT
  -- target (like mimalloc): its objects are merged into kimix-core, so the
  -- fiber scheduler and its thread_local state exist exactly ONCE per process.
  -- A static archive would be re-linked privately into every consumer dll/exe
  -- (each getting its own copy of marl's thread_local Scheduler::bound slot),
  -- which breaks fiber scheduling across the kimix-core.dll boundary
  -- (observed: deterministic test_fiber failure). The C++ sources are the
  -- single manual-unity TU
-- src/build.marl.cpp, which #includes debug/memory/scheduler/thread/trace.cpp
-- and - on _WIN32 only - osfiber_windows.cpp (the Windows Fiber API:
-- CreateFiberEx/SwitchToFiber, kernel32, already on the default link line). On
-- every other platform the context switch is hand-written assembly, so the
-- matching osfiber_<arch>.c + osfiber_asm_<arch>.S pair is added for the target
-- arch only (globbing all of them breaks cross-arch builds, e.g. -mfpu= on
-- ARM64 macOS).
--
-- Two defines carry the configuration:
--   * MARL_USE_SYSTEM_STL=1 - this fork's default STL is EASTL
--     (marl/memory.h, marl/future.h, marl/finally.h and src/memory.cpp all
--     guard their <EASTL/...> includes with it) and KimixBase does not vendor
--     EASTL, so marl is built against std::unique_ptr/shared_ptr/function/
--     optional - the mode CMake selects via LUISA_COMPUTE_USE_SYSTEM_STL.
--     Public: the define changes marl's own header declarations, so every
--     consumer must compile with it.
--   * NO MARL_DLL - a static link needs neither MARL_DLL nor
--     MARL_BUILDING_DLL; marl/export.h then expands MARL_EXPORT/MARL_NO_EXPORT
--     to nothing, which is exactly what an archived object wants.
-- ============================================================================
local marl_dir = path.join(os.scriptdir(), "marl")
  target("kimix-marl")
      _config_project({
          project_kind = "object"
      })
      -- Merged into kimix-core (shared): Linux requires -fPIC objects.
    if is_plat("linux") then
        add_cxflags("-fPIC", {public = true})
        -- osfiber_asm_<arch>.S carries no .note.GNU-stack section, so without
        -- this the linker marks every consumer (kimix_api.so) as needing an
        -- executable stack, and glibc >= 2.41 refuses to dlopen it ("cannot
        -- enable executable stack as shared object requires").
        add_asflags("-Wa,--noexecstack")
    end
    add_headerfiles(path.join(marl_dir, "include/**.h"))
    add_includedirs(path.join(marl_dir, "include"), {
        public = true
    })
    add_defines("MARL_USE_SYSTEM_STL=1", {
        public = true
    })
    add_files(path.join(marl_dir, "src/build.marl.cpp"))
    on_config(function(target)
        if target:is_plat("windows") then
            return
        end
        local arch = target:arch()
        local stem
        if arch == "arm64" or arch == "aarch64" or arch == "arm64-v8a" then
            stem = "aarch64"
        elseif arch == "x86_64" or arch == "x64" then
            stem = "x64"
        elseif arch == "x86" or arch == "i386" then
            stem = "x86"
        elseif arch == "arm" or arch == "armv7" or arch == "armeabi-v7a" then
            stem = "arm"
        elseif arch == "mips64" or arch == "mips64el" then
            stem = "mips64"
        elseif arch == "loongarch64" then
            stem = "loongarch64"
        elseif arch == "riscv64" or arch == "rv64" then
            stem = "rv64"
        elseif arch == "ppc64" or arch == "ppc64le" then
            stem = "ppc64"
        end
        if stem ~= nil then
            target:add("files", path.join(marl_dir, "src/osfiber_" .. stem .. ".c"),
                                path.join(marl_dir, "src/osfiber_asm_" .. stem .. ".S"))
        end
        -- marl spawns std::thread worker threads.
        target:add("syslinks", "pthread", {public = true})
    end)
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

-- ============================================================================
-- Lua (5.5.x, vendored at src/ext/lua) — built as the SHARED library `lua`
-- (lua.dll / liblua.so) exporting the plain C API (LUA_API) for FFI consumers.
-- Every *.c in the tree is compiled except lua.c (the standalone interpreter
-- main()), onelua.c (the single-TU distribution that #includes the others) and
-- ltests.c (Lua's internal test harness, gated by LUA_LIB + ltests.h). Three
-- guarded edits to the vendored sources carry the kimix wiring (see luaconf.h
-- and lauxlib.c); a stock build that defines none of them is unaffected:
--   * KIMIX_LUA_BUILD_SHARED (private, building this dll): luaconf.h emits
--     __declspec(dllexport) on Windows / visibility("default") elsewhere on
--     LUA_API (LUALIB_API and LUAMOD_API alias it).
--   * KIMIX_LUA_USE_SHARED (public, consumers): dllimport on Windows, plain
--     extern elsewhere — FFI/native consumers just need the declarations.
-- * KIMIX_LUA_USE_MIMALLOC (private): lauxlib.c's default allocator
-- luaL_alloc (what luaL_newstate passes to lua_newstate) routes through
-- mi_free/mi_realloc, so a Lua state shares the kimix mimalloc heap.
-- The two guarded source edits live as uncommitted changes inside the
-- src/ext/lua submodule (pinned at upstream 8cb93bad); their exported diff
-- is kept at src/ext/lua.kimix.patch — re-apply it after a submodule update.
-- NOTE for the consumers of this library: src/script (kimix-script, the Lua
-- script sandbox) NEVER calls luaL_openlibs — it builds its states with
-- lua_newstate + luaL_openselectedlibs and installs loud tripwires for the
-- libraries it does not open (see src/script/README.md). Nothing here changes
-- for the sandbox: KIMIX_LUA_SANDBOX (the optional source-side address
-- redaction of the plan) is intentionally NOT defined, because
-- src/script/script_env.cpp closes the same hole from the host side by wrapping
-- string.format. A new consumer must not open io/os/package/debug either;
-- scripts/check_lua_sandbox.py enforces that tree-wide.
-- The mimalloc include dir + the MI_SHARED_LIB import declarations arrive
-- transitively through kimix-core (this target's only dependency), matching
-- the project rule that third-party deps are declared on kimix-core, never
-- pulled directly.
-- ============================================================================
local lua_dir = path.join(os.scriptdir(), "lua")
target("lua")
    _config_project({
        batch_size = 8,
        project_kind = "shared",
        rtti = false,
        enable_exception = false
    })
    -- All library sources; the interpreter, single-TU and internal-test files
    -- are excluded explicitly so a glob never drags in a main() or a duplicate
    -- copy of every symbol.
    add_files(path.join(lua_dir, "*.c"))
    remove_files(path.join(lua_dir, "lua.c"),
                 path.join(lua_dir, "onelua.c"),
                 path.join(lua_dir, "ltests.c"))
    -- Public: consumers need lua.h / lauxlib.h / lualib.h (and luaconf.h).
    add_includedirs(lua_dir, {
        public = true
    })
    add_headerfiles(path.join(lua_dir, "lua.h"),
                    path.join(lua_dir, "luaconf.h"),
                    path.join(lua_dir, "lauxlib.h"),
                    path.join(lua_dir, "lualib.h"))
    -- mimalloc (mi_free/mi_realloc) and its include dir + MI_SHARED_LIB import
    -- declarations come transitively through kimix-core, per the project rule.
    add_deps("kimix-core")
    -- Building this dll: dllexport/visibility branch + the mimalloc allocator.
    add_defines("KIMIX_LUA_BUILD_SHARED", "KIMIX_LUA_USE_MIMALLOC")
      -- The memory arena is opt-in per configuration (root option lua_arena, default
      -- on). Off = the vendored sources compile unchanged: every edit in src/ext/lua
      -- is #if defined(KIMIX_LUA_ARENA)-guarded, so a stock build is unaffected.
      if has_config("lua_arena") then
          -- public: the arena API lives in lua.h / lauxlib.h under this guard, so
    -- every consumer must see the same define the library was built with.
    add_defines("KIMIX_LUA_ARENA", {
        public = true
    })
      end
    -- Consumers: dllimport/extern branch for the C API.
    add_defines("KIMIX_LUA_USE_SHARED", {
        public = true
    })
    on_load(function(target)
        if target:is_plat("windows") then
            -- C stdio/str functions (fopen, getenv, ...) otherwise warn as
            -- "deprecated" under MSVC; loadlib.c uses LoadLibraryExA.
            target:add("defines", "_CRT_SECURE_NO_WARNINGS")
        elseif target:is_plat("linux") then
            -- loadlib.c uses dlopen/dlsym/dlclose (-ldl); lmathlib.c uses the
            -- libm math functions (-lm); loslib.c's system()/clock_gettime and
            -- the pthread-backed scheduler hooks need -lpthread.
            target:add("syslinks", "dl", "m", "pthread", {
                public = true
            })
            -- liblua.so NEEDEDs libkimix-core.so; an $ORIGIN RUNPATH lets a
            -- consumer resolve the transitive dep without LD_LIBRARY_PATH
            -- (RUNPATH is not transitive, so the consumer's own $ORIGIN does
            -- not cover it).
            target:add("rpathdirs", "$ORIGIN")
        end
    end)
target_end()

-- lua-tests: the upstream interpreter WITH Lua's internal test library (T),
-- built exactly like upstream's "make test" -- lua.c + ltests.c + every library
-- source, compiled with LUA_USER_H='"ltests.h"'. It is what runs
-- src/ext/lua/testes/ (36 scripts: gc.lua, gengc.lua, heavy.lua, big.lua,
-- nextvar.lua, sort.lua, strings.lua, api.lua, ...) and it is the e2e gate for
-- any memory change: ltests.h turns on LUAI_ASSERT and routes luaL_newstate
-- through debug_realloc, so T.checkmemory() verifies block sizes and types at
-- the end of every suite. Declared only when the tests are enabled (a plain
-- interpreter is not a shipped artifact).
if has_config("kimix_enable_tests") then
    target("lua-tests")
        set_kind("binary")
        -- No unity build: ltests.h redefines luaL_newstate()/luai_openlibs()
        -- as macros and keeping every unit separate is what upstream tests.
        add_rules("kimix_basic_settings")
        add_files(path.join(lua_dir, "*.c"))
        remove_files(path.join(lua_dir, "onelua.c"))
        add_includedirs(lua_dir, {
            public = true
        })
        -- The hosted allocator is mimalloc (the same heap as kimix-core), and
        -- the memory arena is compiled in when the configuration asks for it,
        -- so both builds can run the same suite.
        add_defines("KIMIX_LUA_USE_MIMALLOC")
        add_defines("LUA_USER_H=\"ltests.h\"")
        if has_config("lua_arena") then
            add_defines("KIMIX_LUA_ARENA")
        end
        add_deps("kimix-core")
        -- testes/all.lua opens its sibling scripts by relative name.
        set_rundir(path.join(lua_dir, "testes"))
        if is_plat("windows") then
            add_defines("_CRT_SECURE_NO_WARNINGS")
        elseif is_plat("linux") then
            add_syslinks("dl", "m", "pthread")
        end
    target_end()
end
-- SQLite3 amalgamation (vendored pregenerated amalgamation at
-- sqlite_amalgamation/) — see the header comment in sqlite_xmake.lua.
includes("sqlite_xmake.lua")
