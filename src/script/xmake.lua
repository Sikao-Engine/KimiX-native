-- Kimix Lua script sandbox (src/script)
--
-- Target: kimix-script — a STATIC library that runs one Lua chunk per call in a
-- throw-away, tightly budgeted lua_State whose environment contains only a
-- vetted subset of the standard library.  A script cannot read or write a file,
-- spawn a process, load a module, reach debug introspection, obtain an address,
-- exit the process, read the environment, or allocate/run without a bound; an
-- attempt is a LOUD denial that ABORTS the run and comes back to the caller as a
-- typed Result.  See src/script/README.md for the five defence layers, the
-- capability matrix and the message format.
--
-- Files (one concern each):
--   script_sandbox.h   the public C++ surface (Sandbox, Limits, Result)
--   script_status.h    Status, the message prefixes, Violation, Result
--   script_value.h     the only value that crosses the boundary (no pointers);
--                      the canonical-JSON writer AND the strict JSON reader
--                      live in script_value.cpp (table text -> Lua table,
--                      one mimalloc yyjson_alc in both directions)
--   script_state.*     SandboxState + lua_State lifetime, panic, warnings
--   script_alloc.*     the budgeted lua_Alloc (memory cap + sticky OOM)
--   script_env.*       _ENV: allow list, replacements, tripwires, integrity
--   script_host.*      host functions/globals: bind_host_function, set_global,
--                      the host trampoline, Value -> stack (a table via the
--                      strict reader), set_global's host-time text validation
--   script_error.*     the one message format + the raise helpers
--   script_sticky.cpp  first-violation-wins + re-raise of the sticky message
--   script_hook.cpp    count hook: deadline, instruction budget, cancel
--   script_run.cpp     THE single lua_pcallk boundary + result marshalling
--   script_lua.h       the only include of the vendored Lua C API
--
-- Dependencies: kimix-core (kimix::string/vector, Clock, the mimalloc heap) and
-- the vendored `lua` target.  `lua` is the one third-party exception to the
-- "deps are declared on kimix-core only" rule — lua already depends on
-- kimix-core, so it cannot be bundled into it; src/api's kimix-sqlite3 dep is the
-- same shape.
--
-- New sources need no change to this file (`add_files("*.cpp")` picks them up),
-- and neither does a new header; only the test registrations in tests/xmake.lua.
--
-- Build: xmake build kimix-script -> bin/<mode>/kimix-script.lib (libkimix-script.a)
target("kimix-script")
    set_kind("static")
    -- Project-wide flags: C++ standard, RTTI/exceptions off, SIMD, LTO
    -- (scripts/xmake_func.lua).  Declared explicitly like every other target;
    -- _config_project() below adds the same rule, and xmake keeps target rules
    -- in a name-keyed set, so this is not applied twice.
    add_rules("kimix_basic_settings")
    add_files("*.cpp")
    add_headerfiles("*.h")
    -- `src/` on the (public) include path: this module's own sources include its
    -- headers as <script/script_env.h>, and a dependent target (kimix_api, the
    -- tests) gets the identical spelling.
    add_includedirs("..", { public = true })
    -- `.` public as well, so a consumer may also use the flat spelling
    -- "script_sandbox.h".
    add_includedirs(".", { public = true })
    -- Linked into the kimix_api shared image (api/ffi_lua.cpp), so the archive
    -- has to be position independent on the POSIX toolchains.
    if is_plat("linux") then
        add_cxflags("-fPIC", { public = true })
    end
    add_deps("kimix-core")
    -- See the header comment: `lua` cannot live inside kimix-core (it already
    -- depends on it), so this target declares it directly.
    add_deps("lua")
    -- Static linkage of this library's own symbols: KIMIX_SCRIPT_API expands to
    -- nothing (src/script/script_api.h).
    add_defines("KIMIX_SCRIPT_STATIC", { public = true })
    on_load(function(target)
        if target:is_plat("windows") then
            target:add("defines", "NOMINMAX", "_CRT_SECURE_NO_WARNINGS", { public = true })
        end
    end)
    _config_project({ batch_size = 8, project_kind = "static" })
target_end()
