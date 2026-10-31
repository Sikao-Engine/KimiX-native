-- Kimix library
-- Standalone publishing links one private core into each final image. The
-- external leg crosses C++ boundaries between those images, so keep ONE shared
-- kimix-core there: its scheduler, strict allocation tracker and other mutable
-- function-local/static state must not be duplicated per DLL/executable.
local kimix_core_kind = has_config("kimix_external_thirdparty") and "shared" or "static"
target("kimix-core")
    set_kind(kimix_core_kind)
    if kimix_core_kind == "static" then
        add_defines("KIMIX_CORE_STATIC", {public = true})
    elseif is_plat("windows") then
        -- The external shared core exports its full C++ surface, including the
        -- STL wrappers that do not carry KIMIX_CORE_API individually.
        add_rules("utils.symbols.export_all")
    end
    -- The standalone archive is linked into runtime_py, kimix_api and lua.
    if kimix_core_kind == "static" and is_plat("linux") then
        add_cxflags("-fPIC", {public = true})
    end
    add_files("core/*.cpp")
    add_files("core/stl/*.cpp")       -- NEW: stl implementations
    -- Optional allocation tracking (root xmake.lua option kimix_memory_strict).
    -- PUBLIC: src/core/memory.h declares kimix::allocate/deallocate in every
    -- build, but the tracker inside src/core/memory.cpp only exists with this
    -- define -- and consumers that assert on the tracker (tests, kimix_api) must
    -- observe the same shape as the library that implements it.  Off by default
    -- and off means compiled out, exactly like graph_db_walk_counters
    -- (src/graph_db/xmake.lua) and kimix_mi_track_asan (src/ext/xmake.lua).
    if has_config("kimix_memory_strict") then
        add_defines("KIMIX_MEMORY_STRICT", { public = true })
    end
    -- Vendored LMDB C sources (third-party, byte-exact copy under core/lmdb/).
    -- Pin: LMDB 1.0.2, tag LMDB_1.0.2 (commit c279e047, 2026-09-08), taken from
    -- <https://github.com/LMDB/lmdb> libraries/liblmdb/. The bump procedure is:
    -- clone upstream, `git checkout LMDB_1.0.2` (or the newer 1.x tag), copy
    -- mdb.c + midl.c + lmdb.h + midl.h into core/lmdb/ UNCHANGED, then rebuild and
    -- re-check the two format constants derived from MDB_page_header/MDB_DATA_VERSION
    -- (see below). Only these four files are vendored: lmdb.cpp uses nothing outside
    -- mdb.c's database engine, so upstream's tools (mdb_stat/mdb_dump/...), tests
    -- (mtest*) and module.c (the dlopen-based crypto module loader -- mdb.c never
    -- calls it) are intentionally not copied.
    -- 1.x, not the 0.9 line: LMDB 1.0 maps the environment through
    -- NtCreateSection/NtMapViewOfSection on Windows, so the map exists over a file
    -- that grows page by page. The 0.9 release line instead SetEndOfFile's the WHOLE
    -- map size at mdb_env_open, which cannot work with the 64 GiB map size graph_db
    -- opens its stores with (it fails with ENOSPC). 1.0 also changed the on-disk
    -- format (MDB_DATA_VERSION 3, the writing txnid in every page header), so a store
    -- written by an earlier 0.9-format kimix build is not readable here: graph_db's
    -- data_file_is_a_store() pre-check (src/graph_db/file_cold_store.cpp, which pins
    -- both constants) refuses it as a plain false instead of aborting, and the way to
    -- migrate such a store is dump with a 0.9 build, load with this one.
    -- Added explicitly: the "core/*.cpp" glob above is non-recursive and only
    -- matches C++, so these .c files are never auto-picked-up. unity_ignored keeps
    -- them out of the c.unity_build batch that _config_project applies (mdb.c and
    -- midl.c define their own statics/macros and must not be #include-merged).
    add_files("core/lmdb/mdb.c", "core/lmdb/midl.c", {unity_ignored = true})
    add_headerfiles("core/*.h")
    add_headerfiles("core/stl/*.h")   -- NEW: stl headers
    add_headerfiles("core/stl/*.inl") -- if any .inl files
    add_headerfiles("core/lmdb/lmdb.h", "core/lmdb/midl.h")
    add_includedirs(".", {public = true})
    -- Dependency rule: all third-party deps are declared HERE (on kimix-core)
    -- only; every other target depends on kimix-core and never on a third-party
    -- target directly. In the standalone leg the mimalloc and marl object
    -- targets are archived into static kimix-core and then into each final
    -- image; KIMIX_CORE_STATIC propagates publicly. In the external leg both
    -- facilities come from shared lc-core and kimix-core itself stays shared.
    --
    -- TWO LEGS, one declaration site (the external leg is declared by
    -- scripts/xmake_luisa.lua, see the kimix_external_thirdparty option):
    --
    -- * standalone (default): the vendored libraries of src/ext/xmake.lua, as
    --   above.
    -- * external (kimix_external_thirdparty=true): lc-core (LuisaCompute's core
    --   library) is the ONE direct third-party dep.  It re-exports the same
    --   facilities through its public interface -- EASTL plus its mimalloc,
    --   spdlog, BTree, lc-yyjson and marl -- so kimix-core keeps compiling
    --   against the same headers, but the vendored targets of src/ext/xmake.lua
    --   are not declared at all in this leg (the bridge owns them) and the
    --   KIMIX_EXTERNAL_THIRDPARTY define lets the C++ side see which leg it is.
    --
    -- kimix-pybind11 is deliberately NOT here in either leg: pybind11 is a
    -- binding-layer facility, not a core one.  The consumers that include
    -- <pybind11/...> (runtime_py's src/runtime/py layer and test_pybind11)
    -- declare the dep themselves.
    if has_config("kimix_external_thirdparty") then
        add_deps("lc-core")
        add_defines("KIMIX_EXTERNAL_THIRDPARTY=1", {public = true})
    else
        add_deps("mimalloc", "kimix-xxhash", "kimix-yyjson", "kimix-marl", "kimix-pdqsort")
    end
    kimix_set_pcxxheader("core/pch.h")
    _config_project({batch_size = 8, project_kind = kimix_core_kind})
    on_load(function(target)
        if kimix_core_kind == "shared" then
            target:add("defines", "KIMIX_CORE_EXPORT_DLL")
        end
        -- ================================================================
        -- Version header: generated from the single config file
        -- (version.txt in the project root). No C++ source hard-codes the
        -- version; bumping the version means editing version.txt only.
        --
        -- The header is rewritten ONLY when its content actually changes.
        -- on_load runs on every single xmake invocation, so an unconditional
        -- io.writefile bumps the mtime every time; kimix_version.h is included
        -- (via kimix_core.h / runtime.h) by essentially every translation unit,
        -- so each of them then looks stale and the whole project recompiles
        -- instead of incrementally.
        -- ================================================================
        local version_txt = path.join(os.projectdir(), "version.txt")
        -- io.readfile raises on a missing file, so probe with os.isfile first.
        local text = os.isfile(version_txt) and io.readfile(version_txt) or nil
        local version = text and text:gsub("^%s*(.-)%s*$", "%1") or "0.0.0"
        local bd = get_config("buildir")
        if bd == nil or bd == "" then
            bd = "build"
        end
        local gen_dir = path.join(os.projectdir(), bd, "gen")
        if not os.isdir(gen_dir) then
            os.mkdir(gen_dir)
        end
        local version_h = path.join(gen_dir, "kimix_version.h")
        local version_h_content =
              "#pragma once\n"
            .. "// GENERATED by xmake from version.txt - DO NOT EDIT.\n"
            .. string.format("#define KIMIX_CORE_VERSION \"%s\"\n", version)
            .. string.format("#define KIMIX_RUNTIME_VERSION \"%s\"\n", version)
        -- io.readfile returns the file byte-exactly (no trim, no newline
        -- translation), so this equality is a real content diff: an unchanged
        -- rerun keeps the old mtime, a version.txt bump still rewrites.
        local current = os.isfile(version_h) and io.readfile(version_h) or nil
        if current ~= version_h_content then
            io.writefile(version_h, version_h_content)
        end
        target:add("includedirs", gen_dir, {public = true})
        if has_config('kimix_disable_win_message_box') and target:is_plat('windows') then
            target:add('defines', 'KIMIX_DISABLE_WIN_MESSAGE_BOX', {public = true})
        end
        if target:is_plat("windows") then
            target:add("defines", "NOMINMAX", "_CRT_SECURE_NO_WARNINGS", {public = true})
            target:add("defines", "KIMIX_PLATFORM_WINDOWS", {public = true})  -- NEW
        -- Vendored LMDB (core/lmdb): its only Windows API beyond kernel32 is
        -- InitializeSecurityDescriptor/SetSecurityDescriptorDacl, which live in
        -- Advapi32.lib (no bcrypt/Crypto API is used). Public so consumers that
        -- link kimix-core also resolve those imports.
        target:add("syslinks", "Advapi32", {public = true})
        -- core/guid.cpp: Guid::remake() calls CoCreateGuid on Windows.
        target:add("syslinks", "Ole32", {public = true})
            if is_mode("debug") then
                target:add("syslinks", "Dbghelp")  -- for backtrace
            end
        elseif target:is_plat("linux") then
            target:add("defines", "KIMIX_PLATFORM_UNIX", {public = true})
        -- Vendored LMDB (core/lmdb): on Linux mdb.c selects MDB_USE_POSIX_MUTEX,
        -- i.e. robust process-shared pthread mutexes in the shared lock mapping, so
        -- -lpthread is what it really needs (no SysV semaphores, no sem_* from -lrt);
        -- -ldl is for core/platform.cpp's dlopen/dlsym and -lrt is a harmless stub on
        -- glibc >= 2.34 (kept for older toolchains).
        target:add("syslinks", "dl", "pthread", "rt")
        elseif target:is_plat("macosx") then
            target:add("defines", "KIMIX_PLATFORM_UNIX", "KIMIX_PLATFORM_APPLE", {public = true})
        end
    end)
target_end()

-- Kimix test executable
local kimix_enable_tests = has_config("kimix_enable_tests")
if kimix_enable_tests then
    target("kimix-test")
    set_kind("binary")
    add_files("test/main.cpp")
    add_deps("kimix-core")
    -- Standalone kimix-core propagates KIMIX_CORE_STATIC to this executable.
    _config_project({batch_size = 8})

    add_tests("basic", {
        runargs = {},
        group = "unit",
    })
    target_end()
end

  -- Kimix LLM providers static library: OpenAI Chat, OpenAI Responses, Anthropic.
  -- Compiled into one static lib; kimix-cli and the unit tests link it.
  --
  -- Also carries the built-in agent tool kernels (src/builtin_tools/*): pure C++
  -- ports of the kimi-agent tools documented in C:/dev/kimi-agent/plans/*.md
  -- (bash, pwsh, python, glob, grep, read, read_image, write, edit, fetch_url,
  -- web_search). Process spawning for the shell/python tools goes through the
  -- vendored reproc library (src/ext/reproc, same fork + pin as LuisaCompute).
    target("kimix-llm")
        set_kind("static")
        -- linked into runtime_py (see kimix-core above)
        if is_plat("linux") then
            add_cxflags("-fPIC", {public = true})
        end
      add_files("llm/*.cpp")
              add_files("llm/openai/*.cpp", "llm/openai_responses/*.cpp", "llm/anthropic/*.cpp", "llm/kimi/*.cpp")
    -- Native Git Bash compatibility kernel (src/llm/bash_parser): the
    -- kimi-agent shell_compat fix_bash_command port; see its README.md.
    add_files("llm/bash_parser/*.cpp")
      add_headerfiles("llm/**/*.h")
        -- Built-in tools: one header/source pair per tool plus the shared kernels.
                add_files("builtin_tools/*.cpp")
      -- http_fetch.cpp / web_search_tool.cpp pull llm/http_client.h (winsock2);
      -- the winsock2-before-windows.h order is guaranteed project-wide by
      -- core/kimix_core.h, so they can share a unity batch like the llm/ TUs.
        -- F7: the minimal MCP stdio client (bridges servers' tools into the
        -- ToolRegistry as external tools; see src/mcp/mcp_client.h).
        add_files("mcp/*.cpp")
        add_headerfiles("mcp/*.h")
        -- Agent soul (src/agent/*): session management + turn loop + compaction
        -- on top of the built-in tools and the unified LLM facade.
    add_files("agent/*.cpp")
    add_files("agent/dynamic_injections/*.cpp") -- G9 dynamic-injection providers
    add_headerfiles("agent/*.h")
    add_headerfiles("agent/dynamic_injections/*.h")
        -- pwsh_tool calls kimix::runtime::parse::scan_shell and
        -- kimix::runtime::tools::check_hardline_blocked. The kernel DEFINITIONS are
        -- compiled into kimix-llm itself (shell_scanner.cpp / shell_safety.cpp
        -- below); KIMIX_RUNTIME_EXPORT_DLL makes them dllexport (so runtime_py
        -- re-exports them and its pybind bindings resolve them as imports) and
        -- makes pwsh_tool's calls plain references resolved from this lib. This
                    -- keeps the unit tests independent of runtime_py (the runtime
          -- kernels live in this static lib and are re-exported by the pyd, so a
          -- test that links kimix-llm resolves them without the pyd).
      add_files("runtime/parse/shell_scanner.cpp", "runtime/tools/shell_safety.cpp")
      -- AgentSession embeds a kimix::runtime::index::HistoryIndex (the
      -- retrieve tool's in-memory index, gap D4), so the index kernels are
      -- compiled into kimix-llm as well; runtime_py removes these files to
      -- avoid duplicate definitions (same arrangement as shell_scanner).
      -- The durable SQLite FTS5 index (rows D3/D6/D7/D8/D9) and the fuzzy
      -- kernel it wires in (D9) join them; kimix-sqlite3 (dep below) carries
      -- the amalgamation.
      add_files("runtime/index/history_index.cpp", "runtime/index/inverted_index.cpp",
                "runtime/index/ngram_tokenizer.cpp", "runtime/index/sqlite_history_index.cpp",
                "runtime/search/bm25.cpp", "runtime/search/fuzzy.cpp",
                "runtime/search/distance.cpp",
                "runtime/common/utf8.cpp")
      -- KimiSoul's dispatch path calls sanitize_for_tokenizer on every tool
      -- output (F4) and exports the pre-compaction history through
      -- build_export_markdown (C8), so those runtime kernels are compiled
      -- into kimix-llm as well; runtime_py removes them like shell_safety.
              add_files("runtime/text/sanitize.cpp", "runtime/tools/export_builder.cpp",
                  -- Tier C micro-compress kernels the context pruner
                  -- (src/agent/context_pruning.cpp) calls, plus the ansi.cpp
                  -- strip_ansi kernel compress.cpp reuses; runtime_py removes
                  -- them like sanitize.
                  "runtime/tools/compress.cpp", "runtime/stream/ansi.cpp")
      add_headerfiles("builtin_tools/*.h")
      add_includedirs(".", {public = true}) -- keeps `#include "llm/..."` working from `src/` root
      add_deps("kimix-core", "kimix-mbedtls", "kimix-reproc",
               -- SQLite context store (src/agent/context_db.*, the context_db.py
               -- port): amalgamation static lib, FTS5 enabled.
               "kimix-sqlite3")
        add_defines("KIMIX_LLM_STATIC", "KIMIX_RUNTIME_EXPORT_DLL",
                    {public = true})
        _config_project({batch_size = 8, project_kind = "static"})
  target_end()


-- Include extensions
includes("ext")

-- Include the native CLI (src/cli): kimix-cli (static) + kimix_cli (binary).
-- It depends on kimix-llm, which already carries the agent, the unified LLM
-- facade and the built-in tools.  See src/cli/PLAN.md.
includes("cli")

-- Include the C-FFI library (src/api): kimix_api (shared).
--
-- File-level feature switch: with --kimix_enable_api=false this whole include
-- is skipped, so the target, its headers and everything built on top of it are
-- not part of the configuration at all (see tests/xmake.lua, which repeats the
-- same condition for test_kimix_api).  kimix_api depends only on kimix-core,
-- so it is independent of kimix_enable_llm / _cli / _runtime.
if has_config("kimix_enable_api") then
    includes("api")
end
-- Include the graph database (src/graph_db): kimix-graph-db (static), the
-- in-memory graph container + LMDB cold-storage glue (kimix-core only).
includes("graph_db")

-- Include the Lua script sandbox (src/script): kimix-script (static) — one
-- throw-away, budgeted lua_State per run, a vetted environment, loud tripwires
-- and an aborted run on any denied capability.  See src/script/README.md.
includes("script")




-- ============================================================================
-- Kimix Runtime Python module (runtime_py.pyd)
--
-- Merged target: all runtime kernels (runtime/**.cpp) plus the pybind11
-- binding layer (runtime/py/**.cpp) are compiled into a single Python
-- extension module. Native C++ tests link against this module as a shared
-- library; the runtime symbols are exported via KIMIX_RUNTIME_API.
-- ============================================================================
target("runtime_py")
    set_kind("shared")
    set_extension(".pyd")
    if is_plat("linux") then
        -- publish.py packages bin/release/runtime_py.pyd; on Linux xmake would
        -- otherwise prepend 'lib' to the shared-library filename.
        set_prefixname("")
    end
    -- gate + RTTI-off etc., WITH unity build (batch_size = 8): the runtime
    -- kernels and the pybind11 binding TUs merge into jumbo batches like every
    -- other src target (src/ext excluded). The remove_files list above is
    -- respected by the unity batcher, so the kimix-llm-shared kernels stay
    -- single-TU excluded here.
    _config_project({batch_size = 8, project_kind = "shared"})

    add_files("runtime/**.cpp")
        -- shell_scanner.cpp / shell_safety.cpp are compiled into kimix-llm (which
        -- needs them for its built-in pwsh tool) and re-exported here; excluding
        -- them avoids duplicate definitions in this module.  The history-index
        -- kernels moved into kimix-llm for the same reason (AgentSession embeds
        -- an in-memory HistoryIndex for the retrieve tool).
          remove_files("runtime/parse/shell_scanner.cpp", "runtime/tools/shell_safety.cpp",
                       "runtime/tools/compress.cpp", "runtime/stream/ansi.cpp",
                       "runtime/index/history_index.cpp", "runtime/index/inverted_index.cpp",
                       "runtime/index/ngram_tokenizer.cpp", "runtime/index/sqlite_history_index.cpp",
                       "runtime/search/bm25.cpp", "runtime/search/fuzzy.cpp",
                       "runtime/search/distance.cpp",
                       "runtime/common/utf8.cpp",
                       "runtime/text/sanitize.cpp", "runtime/tools/export_builder.cpp")
    add_headerfiles("runtime/**/*.h")
    add_includedirs("..", {public = true}) -- expose src/ so <runtime/runtime.h> works
    -- runtime_py depends on kimix-llm for real: the binding layer in
    -- runtime/py/py_builtin_*.cpp (and py_tools/py_glob/py_search/py_index/
    -- py_stream/py_text) links the builtin_tools::* kernels, and the shared
    -- runtime kernels listed in remove_files above (bm25/fuzzy/distance,
    -- utf8, sanitize, compress/ansi, shell_scanner/shell_safety, the history
    -- index, export_builder) are compiled only into kimix-llm and re-exported
    -- from this module.  Removing this dep leaves hundreds of unresolved
    -- externals (verified: LNK2019 on Bm25Scorer / strip_ansi /
    -- sanitize_for_tokenizer / the glob/bash/compact/... tool kernels), so it
    -- stays until those kernels move to a lib shared by runtime_py and
    -- kimix-llm.
    add_deps("kimix-core", "kimix-llm",
             -- pybind11 moved off kimix-core (it is a binding-layer facility,
             -- not a core one): the module's runtime/py/**.cpp TUs are the
             -- consumer that includes <pybind11/...>, so they declare it here.
             "kimix-pybind11")
    on_load(function(target)
        -- Disabled by kimix_enable_runtime (or by kimix_enable_llm, which
        -- runtime_py links): skip the Python interpreter probe below entirely,
        -- so a configuration without the module also works on a host without
        -- Python.  The kimix_basic_settings feature gate has already run for
        -- this target when on_load starts.
        if not target:is_enabled() then
            return
        end
        -- Export runtime symbols from this module; consumers see dllimport.
        target:add("defines", "KIMIX_RUNTIME_EXPORT_DLL")
        -- Standalone kimix-core is statically linked into this module. The
        -- external leg uses shared kimix-core/luisa-core companions, so keep
        -- same-directory dependency resolution on Linux.
        if is_plat("linux") then
            target:add("rpathdirs", "$ORIGIN")
        end

        -- Python interpreter discovery.  Windows keeps the original direct
        -- call (python is on PATH).  On Linux the bare name "python" usually
        -- does not exist (only python3) and os.iorunv RAISES when the program
        -- is missing, so resolve the interpreter path through the shell first.
        local function py_conf(var)
            local exe = "python"
            if not is_plat("windows") then
                local probe = os.iorunv("sh", {"-c", "command -v python3 2>/dev/null || command -v python 2>/dev/null"})
                if not probe then
                    return nil
                end
                exe = probe:gsub("%s+$", ""):gsub("^%s+", "")
                if exe == "" then
                    return nil
                end
            end
            local out = os.iorunv(exe, {"-c", "import sysconfig; print(sysconfig." .. var .. ")"})
            if out then
                out = out:gsub("%s+$", "")
                if out ~= "" then return out end
            end
            return nil
        end
        local inc = py_conf("get_paths()['include']")
        if inc and os.isdir(inc) then
            target:add("includedirs", inc)
        end
        local libdir = py_conf("get_config_var('LIBDIR')")
        if libdir and os.isdir(libdir) then
            target:add("linkdirs", libdir)
            if is_plat("windows") then
                target:add("links", "python314")
            else
                -- Linux: link the interpreter's real shared library
                -- (libpython3.10.so -> "python3.10").
                local ldlib = py_conf("get_config_var('LDLIBRARY')")
                local linkname = ldlib and ldlib:gsub("^lib", ""):gsub("%.so$", "")
                if linkname and linkname ~= "" then
                    target:add("links", linkname)
                end
            end
        end
    end)
    after_link(function(target)
        -- Linux: the test executables link this module with -lruntime_py;
        -- provide libruntime_py.so pointing at the actual module file so the
        -- linker can resolve it (the module itself keeps the .pyd extension).
        if target:is_plat("linux") then
            local so = path.join(target:targetdir(), "libruntime_py.so")
            -- remove any pre-existing link (incl. stale/broken symlinks)
            os.rm(so)
            -- same-dir relative link: libruntime_py.so -> <module filename>
            os.ln(target:filename(), so)
            -- CPython on Linux only imports extension modules named *.so, so
            -- publish a real copy under the importable name next to the
            -- module (the module file itself keeps the .pyd extension).
              local imp = path.join(target:targetdir(), "runtime_py.so")
              os.rm(imp)
              os.cp(target:targetfile(), imp)
          end
      end)
  target_end()


-- LuisaCompute embedding example (BOTH options required: kimix_external_thirdparty
-- and kimix_luisa_embedding, both off by default).
-- scripts/xmake_luisa.lua resolves ../link.lua and declares just this example's
-- runtime/DSL/DX/VK dependency closure; no LC tests or other examples are
-- included. All targets share bin/<mode>, so the interactive executable finds
-- luisa-backend-dx.dll and luisa-backend-vk.dll beside itself at runtime.
-- The call is unconditional on purpose -- the bridge owns both switches, so a
-- standalone build and a plain external build (kimix_luisa_embedding=false) take
-- the early return inside it and this file stays free of a second option read.
kimix_luisa_include_embedding()
