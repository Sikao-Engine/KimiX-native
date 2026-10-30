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
-- ============================================================================
-- Lua Language Server (submodule at src/ext/lua-language-server,
-- https://github.com/Sikao-Engine/lua-language-server) — the server's own
-- executable, plus (under kimix_enable_tests) a target that runs its Lua test
-- suites.
--
-- Upstream builds this with luamake (make.lua + 3rd/bee.lua/compile/*.lua); the
-- targets below transcribe those five source sets rather than inventing a
-- build:
--   * source_lua       3rd/bee.lua/3rd/lua55/onelua.c — Lua 5.5 as ONE
--                      translation unit that defines LUA_CORE + LUA_LIB
--                      itself, so MAKE_LIB is all it needs to not emit a
--                      main().
--   * source_bee       3rd/bee.lua — bee (filesystem/thread/socket/subprocess/
--                      filewatch), 3rd/lua-seri, 3rd/fmt, binding/*.cpp (the
--                      Lua module bindings) and 3rd/lua-patch/bee_newstate.c.
--   * source_bootstrap 3rd/bee.lua/bootstrap/main.cpp — the interpreter entry
--                      point that makes this binary a Lua script host.
--   * lpeglabel        3rd/lpeglabel — the LPeg parser the Lua parser runs on
--                      (C, MAXRECLEVEL=1000).
--   * code_format      3rd/EmmyLuaCodeStyle — the formatter with its LuaParser
--                      and Util, exported as the `code_format` module.
-- make/modules.cpp is the glue: it registers `lpeglabel` and `code_format` as
-- preloaded modules, both symbols coming from the sets above.
--
-- Deliberate deviations from upstream, each for a reason we can name:
--   * No BEE_FAST_SETJMP — that define selects an MSVC-only assembly setjmp
--     (3rd/lua-patch/fast_setjmp_<arch>.s). Without it lua55/lprefix.h keeps
--     the plain setjmp/longjmp pair every other platform uses: same
--     semantics, one less hand-written .s to compile.
--   * No make/lua-language-server.rc — the resource is the release icon plus
--     version block, and it references make/icon.ico through the repo root.
--     Neither belongs to a build/test artifact.
--   * No -Wall -Werror (upstream's POSIX flags for code_format): third-party
--     code is built with this project's warnings=none (kimix_basic_settings),
--     so a new compiler version cannot fail the build on a vendored warning.
--   * EmmyLuaCodeStyle is compiled against the EMBEDDED lua 5.5 headers, not
--     upstream's hardcoded 3rd/bee.lua/3rd/lua54. Upstream's source sets are
--     separate compilations, which hides the mismatch, but
--     CodeFormatLib.cpp's luaopen_code_format runs inside the 5.5 state this
--     executable embeds and has to see the same declarations.
--   * C++ exceptions stay ON for the executable (set_values
--     "kimix_enable_exception"): 3rd/EmmyLuaCodeStyle/LuaParser throws
--     LuaParseException and bee's filesystem binding catches
--     std::filesystem_error. RTTI has to stay ON as well: 3rd/EmmyLuaCodeStyle
--     casts its format/name-style nodes with dynamic_cast and
--     std::dynamic_pointer_cast (FormatState.h, NameStyleRuleMatcher.cpp), which
--     upstream's global `lm:conf { rtti = "off" }` only gets away with because
--     its msvc toolchain ignores the flag.
--   * The executable is written to <submodule>/bin/ — the upstream layout, and
--     gitignored there (`/bin*`). The server derives its ROOT from the
--     executable path (bootstrap/main.lua: the directory of main.lua; test.lua:
--     parent of parent of the exe), so bin/lua-language-server resolves
--     script/, meta/, locale/ and test/ inside the submodule and the suites can
--     run with the submodule root as the working directory.
-- ============================================================================
local lls_dir = path.join(os.scriptdir(), "lua-language-server")
local lls_bee = path.join(lls_dir, "3rd/bee.lua")
local lls_lua55 = path.join(lls_bee, "3rd/lua55")
local lls_emmy = path.join(lls_dir, "3rd/EmmyLuaCodeStyle")
-- The file name has to stay `lua-language-server` (+ .exe): it is what the
-- release archive ships and what the ROOT detection of the server expects.
local lls_exe = "lua-language-server"

target(lls_exe)
    _config_project({
        project_kind = "binary"
    })
    -- See the header comment: the formatter and the filesystem binding require
    -- exceptions, and EmmyLuaCodeStyle's node casts require RTTI — both are
    -- kimix options that default to off.
    set_values("kimix_enable_exception", true)
    set_values("kimix_rtti", true)
    -- Standards exactly as upstream pins them: make.lua does
    -- `lm.c = msvc and 'c89' or 'c11'` and `lm.cxx = 'c++17'`.
    set_values("kimix_c_standard", is_plat("windows") and "c89" or "c11")
    set_values("kimix_cxx_standard", "cxx17")
    -- The executable goes to <submodule>/bin (the upstream layout, see the
    -- header comment). On Windows the link step also drops an import library
    -- (.lib) + .exp next to it — exporting the lua C API (LUA_BUILD_AS_DLL
    -- below) makes xmake pass `/implib:` for every Windows binary, and its
    -- value is always the target dir. They are inert (`/bin*` is gitignored in
    -- the submodule) and only differ from upstream's luamake, which sends the
    -- same pair into its object dir.
    set_targetdir(path.join(lls_dir, "bin"))
    -- bee's include root ("bee/...", "3rd/...", "binding/...") plus the lua 5.5
    -- headers that onelua.c and the bindings share.
    add_includedirs(lls_bee, lls_lua55)
    -- EmmyLuaCodeStyle's own include roots (make/code_format.lua).
    add_includedirs(path.join(lls_emmy, "Util/include"),
                    path.join(lls_emmy, "CodeFormatCore/include"),
                    path.join(lls_emmy, "LuaParser/include"),
                    path.join(lls_emmy, "3rd/wildcards/include"))
    -- An executable, not the bee DLL: bee's module openers become plain
    -- `extern "C"` instead of dllexport stubs (bee/lua/module.h).
    add_defines("BEE_STATIC")
    -- make/modules.cpp registers the formatter as `code_format` under this.
    add_defines("CODE_FORMAT")
    -- Lua 5.5 in a single TU: MAKE_LIB keeps the standalone interpreter out.
    -- The platform define is the one compile/common.lua gives its source_lua
    -- set: LUA_BUILD_AS_DLL on Windows (LUA_CORE/LUA_LIB are defined inside
    -- onelua.c, so LUA_API stays dllexport there and plain `extern` in every
    -- other TU), LUA_USE_LINUX / LUA_USE_MACOSX for dlopen + the POSIX
    -- features.
    local lua55_defines = {"MAKE_LIB"}
    if is_plat("windows") then
        table.join2(lua55_defines, {"LUA_BUILD_AS_DLL"})
    elseif is_plat("macosx") then
        table.join2(lua55_defines, {"LUA_USE_MACOSX"})
    else
        table.join2(lua55_defines, {"LUA_USE_LINUX"})
    end
    add_files(path.join(lls_lua55, "onelua.c"), {
        defines = lua55_defines
    })
    add_files(path.join(lls_bee, "3rd/lua-patch/bee_newstate.c"))
    add_files(path.join(lls_bee, "3rd/lua-seri/lua-seri.cpp"),
              path.join(lls_bee, "3rd/fmt/format.cc"))
    add_files(path.join(lls_bee, "binding/*.cpp"))
    add_files(path.join(lls_bee, "bootstrap/main.cpp"),
              path.join(lls_bee, "bootstrap/bootstrap_init.cpp"))
    -- LPeg: pure C, and MAXRECLEVEL is the recursion guard upstream sets.
    add_files(path.join(lls_dir, "3rd/lpeglabel/*.c"), {
        defines = "MAXRECLEVEL=1000"
    })
    -- The module table of the executable.
    add_files(path.join(lls_dir, "make/modules.cpp"))
    -- code_format: CodeFormatLib (the lua binding) + LuaParser + the parts of
    -- Util/ and CodeFormatCore/ they link against. CommandLine.cpp,
    -- FileFinder.cpp and Url.cpp are NOT here — they pull uriparser, which the
    -- server never uses.
    add_files(path.join(lls_emmy, "CodeFormatLib/src/*.cpp"))
    add_files(path.join(lls_emmy, "LuaParser/src/**.cpp"))
    add_files(path.join(lls_emmy, "Util/src/StringUtil.cpp"),
              path.join(lls_emmy, "Util/src/Utf8.cpp"))
    add_files(path.join(lls_emmy, "Util/src/SymSpell/*.cpp"))
    add_files(path.join(lls_emmy, "Util/src/InfoTree/*.cpp"))
    add_files(path.join(lls_emmy, "CodeFormatCore/src/**.cpp"))
    on_load(function(target)
        -- bee splits per-platform code over a filename suffix (*_win.cpp,
        -- *_linux.cpp, ...) and a per-platform directory (bee/win/,
        -- bee/linux/, ...). Upstream's `need()` in compile/common.lua keeps
        -- exactly the entries of the current platform; the same filter, spelled
        -- out per file so a rename upstream cannot silently change the set.
        local tags = {"win", "posix", "osx", "linux", "bsd"}
        local keep = {}
        if target:is_plat("windows") then
            keep.win = true
        elseif target:is_plat("macosx") then
            keep.osx = true
            keep.posix = true
        else
            keep.linux = true
            keep.posix = true
        end
        local bee_root = path.translate(path.join(lls_bee, "bee"), "/")
        for _, filepath in ipairs(os.files(path.join(lls_bee, "bee/**.cpp"))) do
            local rel = path.translate(filepath, "/"):sub(#bee_root + 2)
            local wanted = true
            local dir = rel:match("^([^/]+)/")
            if dir and table.contains(tags, dir) and not keep[dir] then
                wanted = false
            end
            local tag = rel:match("_([a-z]+)%.cpp$")
            if tag and table.contains(tags, tag) and not keep[tag] then
                wanted = false
            end
            -- Upstream never builds bee's Linux crash handler
            -- ("!bee/crash/linux/**/" in the linux source set).
            if not target:is_plat("windows") and rel:find("^crash/linux/") then
                wanted = false
            end
            if wanted then
                target:add("files", filepath)
            end
        end

        if target:is_plat("windows") then
            target:add("defines", "_WIN32_WINNT=0x0602", "_CRT_SECURE_NO_WARNINGS")
            -- The AFD-based socket poller, the unicode/unicode-CRT helpers and
            -- the crash handler: ntdll (NtCreateFile), ws2_32 (sockets),
            -- version (GetFileVersionInfo), synchronization (WaitOnAddress),
            -- plus the Win32 shells bee/win/unicode.cpp, lua_windows.cpp and
            -- the crash handler call into. Same list as compile/common.lua.
            target:add("syslinks", "ntdll", "ws2_32", "ole32", "user32",
                                    "version", "synchronization")
            -- UTF-8 command line shim and the Windows-only luapath port.
            target:add("files", path.join(lls_bee, "3rd/lua-patch/bee_utf8_crt.cpp"),
                                path.join(lls_bee, "binding/port/lua_windows.cpp"))
        elseif target:is_plat("macosx") then
            -- compile/common.lua's macos sections: LUA_USE_MACOSX plus the
            -- Foundation trio bee's socket/thread layers use. Transcribed from
            -- upstream — this project builds Windows and Linux, so this branch
            -- is not exercised by any test here.
            target:add("defines", "LUA_USE_MACOSX")
            target:add("syslinks", "m", "dl")
            target:add("frameworks", "Foundation", "CoreFoundation", "CoreServices")
        else
            -- dlopen (Lua's loadlib), the libm math functions, clock_gettime
            -- and the epoll/thread primitives behind bee.thread.
            target:add("syslinks", "dl", "m", "pthread")
            -- Upstream exports the executable's symbols (-Wl,-E / -rdynamic) so
            -- bee's backtrace resolver can name frames, and pins libgcc
            -- (crt = "static" in compile/common.lua).
            target:add("ldflags", "-rdynamic", "-static-libgcc")
        end
    end)
    -- bin/main.lua is what the server loads when it is started WITHOUT a script
    -- argument: bootstrap/main.cpp falls back to <exedir>/main.lua, and
    -- make/bootstrap.lua is the Lua half of that entry point (it installs
    -- <cwd>/script/?.lua and dispatches -e/script arguments). Upstream's luamake
    -- copies it in its "copy_bootstrap" step — without it the binary only works
    -- when handed an explicit script (e.g. test.lua).
    after_build(function(target)
        local dst = path.join(target:targetdir(), "main.lua")
        os.cp(path.join(lls_dir, "make/bootstrap.lua"), dst)
        print("copied:", dst)
    end)
target_end()

-- ---------------------------------------------------------------------------
-- lua-language-server-tests — the server's own Lua test suite
-- ---------------------------------------------------------------------------
-- Upstream drives it from luamake (make.lua, rule "run-unit-test") as
-- `bin/lua-language-server test.lua` with the repository root as the working
-- directory. Nothing is compiled — the 175 files under test/ are Lua fixtures
-- and assertions, and the C++ half is the executable built above — so this
-- target is phony and only depends on it. Declared under kimix_enable_tests
-- only: it is a gate, not a shipped artifact.
--
-- `test.lua` runs 27 top-level suites (basic, parser_test, definition,
-- type_inference, implementation, references, hover, completion, diagnostics,
-- crossfile, highlight, inlay_hint, rename, signature, command,
-- document_symbol, code_action, other, tclient, full, plugins, cli) whose bodies
-- are the upstream assertion corpus; it exits non-zero on the first failed
-- assert.
--
-- Line endings: the fixtures are committed with LF, and one suite
-- (test/cli/visualize) compares generated output byte-for-byte against a
-- committed -expected.txt. A Windows checkout with core.autocrlf=true rewrites
-- both to CRLF, which the Windows build cannot notice (Lua's io.open is in text
-- mode there, so CRLF is read back as LF) but a Linux build over that same
-- working tree reports as an output mismatch. If
-- `git -C src/ext/lua-language-server ls-files --eol test/cli/visualize/testdata`
-- shows `w/crlf`, fix the checkout rather than the test:
--   git -C src/ext/lua-language-server config core.autocrlf false
--   rm -f src/ext/lua-language-server/test/cli/visualize/testdata/*
--   git -C src/ext/lua-language-server checkout -- test/cli/visualize/testdata
--
-- bee's runtime suite (3rd/bee.lua/test/test.lua, upstream's "run-bee-test",
-- which luamake makes a dependency of the unit test) is deliberately NOT part
-- of this target. 110 of its 112 tests pass against this executable, but
-- `subprocess.test_args` and `subprocess.test_cwd` assert lua.c's `-e` argument
-- table (arg[0] = the script name, arg[1..] = the script's arguments), which
-- only luamake's generated `_bee_main` provides. A lua-language-server build
-- uses bee's Lua fallback bootstrap instead — `bin/lua-language-server -e ...
-- _ foo` leaves arg as {-1 = exe, 0 = "!main.lua", 1 = "-e", 2 = <code>,
-- 3 = "_", 4 = "foo"} — so both fail in the host's CLI emulation, before any
-- code of this project runs. Run it by hand to see the other 110 pass:
--   bin/lua-language-server 3rd/bee.lua/test/test.lua
if has_config("kimix_enable_tests") then
    -- Run one suite with the executable built above. `sbx_os` is the SCRIPT
    -- field's own `os` module (passed in, not the file-scope one: the project
    -- description sandbox this file is parsed in has no execv, every script
    -- field does). `script` is relative to the submodule root, which is also
    -- the working directory: when the server is handed a script path its
    -- bootstrap takes `root = "."` and installs `./script/?.lua`, and test.lua
    -- derives ROOT/TESTROOT from the executable path — both expect to start in
    -- the submodule root.
    local function lls_run_suite(sbx_os, exe, script, extra_args)
        local argv = {script}
        for _, arg in ipairs(extra_args or {}) do
            argv[#argv + 1] = arg
        end
        print("running:", exe, table.concat(argv, " "))
        local ok, errors = sbx_os.execv(exe, argv, {
            curdir = lls_dir,
            try = true
        })
        if ok ~= 0 then
            print("suite failed with exit code", tostring(ok), errors or "")
            return false
        end
        return true
    end

    target("lua-language-server-tests")
        set_kind("phony")
        add_rules("kimix_basic_settings")
        add_deps(lls_exe)
        -- `xmake run lua-language-server-tests [-n=<suite>]`: the extra argument
        -- is forwarded, and test.lua accepts -n= / --name= to run one suite.
        on_run(function(target)
            import("core.base.option")
            local exe = target:dep(lls_exe):targetfile()
            if not lls_run_suite(os, exe, "test.lua", option.get("arguments")) then
                raise("lua-language-server test suite failed")
            end
        end)
        -- 46 s for the whole suite on the Windows release build (one process,
        -- 27 suites; 55 s inside WSL) is over the ~10 s budget of the default
        -- `xmake test` sweep, so — exactly like the slow_suites table in
        -- tests/xmake.lua — the test is registered only when
        -- `--kimix_slow_tests=y` is configured. The target still builds, and
        -- `xmake run lua-language-server-tests` always runs it.
        if has_config("kimix_slow_tests") then
            add_tests("suite")
            on_test(function(target)
                return lls_run_suite(os, target:dep(lls_exe):targetfile(), "test.lua")
            end)
        else
            on_config(function(target)
                print("note: lua-language-server-tests is kept out of `xmake test`"
                    .. " (46 s); use --kimix_slow_tests=y or"
                    .. " `xmake run lua-language-server-tests`")
            end)
        end
    target_end()
end

-- SQLite3 amalgamation (vendored pregenerated amalgamation at
-- sqlite_amalgamation/) — see the header comment in sqlite_xmake.lua.
includes("sqlite_xmake.lua")
