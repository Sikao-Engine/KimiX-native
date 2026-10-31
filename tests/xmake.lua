-- Tests xmake.lua
-- Test helper: test_proj(name, source, callable)
--   callable: optional config callback for extra deps/includes/defines

local kimix_enable_tests = has_config("kimix_enable_tests")
if not kimix_enable_tests then
    return
end

-- Cached Python install directory. runtime_py.pyd links python314.dll, so any
-- test that loads it as a shared library needs Python's directory on PATH.
local _python_dir = nil

-- ---------------------------------------------------------------------------
-- Slow suites — measured, not guessed
-- ---------------------------------------------------------------------------
-- Seconds each suite takes on the RELEASE build (bin/release), measured with one
-- process per binary; the median suite is 0.2 s, the total is ~350 s serial, and
-- these 9 rows are ~190 s of it. They are real-process / large-corpus e2e runs
-- (bash/python subprocess lifecycles, a many-iteration crash repro, the grep
-- engine matrices, the glob and diff corpora, the MCP client).
--
-- They are NOT part of `xmake test` unless the project is configured with
-- `xmake f --kimix_slow_tests=y`:  else they are built and registered, they just
-- do not run, so the default sweep stays a fast gate. `xmake run <target>` always
-- runs them (the switch only decides what `xmake test` executes).  Re-measure
-- before adding a row — the table is the evidence, not an opinion.
local slow_suites = {
    test_builtin_process_runner = 74,
    test_subagent_bg_startup_stress = 20,
    test_builtin_bug_tool_report = 19,
    test_native_glob = 16,
    test_builtin_workflow = 14,
    test_builtin_grep_engine_matrix = 13,
    test_builtin_grep_engine = 12,
    test_mcp_client = 11,
    test_native_diff = 10
}

local _slow_skipped = {}

local function _registers_tests(name)
    local seconds = slow_suites[name]
    if seconds == nil or has_config("kimix_slow_tests") then
        return true
    end
    _slow_skipped[#_slow_skipped + 1] = name .. " (" .. seconds .. "s)"
    return false
end

local function get_python_dir(target)
    if _python_dir then
        return _python_dir
    end
    -- Some xmake versions do not expose os.iorunv inside before_run hooks, so
    -- guard the call and fall back to locating the python executable's
    -- directory from PATH (it contains python314.dll on Windows).
    if type(os.iorunv) == "function" then
        local out = os.iorunv("python", {"-c", "import sysconfig; print(sysconfig.get_config_var('LIBDIR'))"})
        if out then
            out = out:gsub("%s+$", "")
            if out ~= "" then
                -- On Windows LIBDIR is <python>\libs but the DLL is in the parent
                -- directory; on Linux the .so usually lives in LIBDIR itself.
                if target:is_plat("windows") then
                    _python_dir = path.directory(out)
                else
                    _python_dir = out
                end
                return _python_dir
            end
        end
    end
    local sep = path.envsep()
    for dir in (os.getenv("PATH") or ""):gmatch("[^" .. sep .. "]+") do
        if dir ~= "" and os.isfile(path.join(dir, "python.exe")) then
            _python_dir = dir
            return _python_dir
        end
    end
    return nil
end

local function test_proj(name, source, callable)
    target(name)
        set_kind("binary")
        add_files(source)
        add_includedirs("./")
        add_deps("kimix-core")
        -- No KIMIX_CORE_STATIC: KIMIX_CORE_API resolves as dllimport and the test
        -- binary links kimix-core.dll (the single mi_* heap lives there too). All
        -- targets share bin/<mode>, so the loader resolves it without a PATH hack.
        -- The Boost.UT harness runs a test body only inside
        -- `#if defined(__cpp_exceptions)` (tests/ut/ut.hpp, runner::
        -- on(events::test<...>) wraps `test()` in try/catch and has no
        -- exception-free branch). With exceptions disabled the suite silently
        -- registers every test but never executes one ("0 asserts in N tests"),
        -- so every test binary keeps exceptions enabled. The kimix library
        -- targets remain exception-free (kimix_enable_exception=false).
        set_values("kimix_enable_exception", true)
        _config_project({batch_size = 8})
        -- Registering the test entry is what puts a target into `xmake test`; a
        -- slow suite keeps its target (and `xmake run`) but skips this line
        -- unless --kimix_slow_tests=y (see the slow_suites table above).
        if _registers_tests(name) then
            add_tests("default")
        end
        before_run(function(target)
            local pydir = get_python_dir(target)
            if pydir and os.isdir(pydir) then
                local old = os.getenv("PATH") or ""
                os.setenv("PATH", pydir .. path.envsep() .. old)
            end
        end)
        if callable then
            callable()
        end
    target_end()
end

    -- unit/core tests
    test_proj("test_kimix_core", "unit/core/test_kimix_core.cpp")

    -- The kimix::allocate / kimix::deallocate gateway (core/memory.h) and, in a
    -- debug build configured with --kimix_memory_strict=true, its leak tracker.
    -- The suite is runtime-guarded (kimix::memory_tracking_enabled()), so it runs
    -- and passes in every configuration.
    test_proj("test_memory", "unit/core/test_memory.cpp")
    -- The CORE CONTRACT of the two build legs (xmake option kimix_external_thirdparty):
    -- the container/allocator semantics both legs share, the container ABI across the
    -- kimix-core dll boundary, the leg discriminator asserted at compile time in both
    -- directions, and (external leg only) the Luisa logging / magic_enum doors.
    -- Registered unconditionally: a leg that stopped mapping its containers fails
    -- here, not 200 suites later.
    test_proj("test_core_leg_contract", "unit/core/test_core_leg_contract.cpp")
    test_proj("test_stl_allocator", "unit/core/test_stl_allocator.cpp")
test_proj("test_stl_functional", "unit/core/test_stl_functional.cpp")
test_proj("test_stl_string", "unit/core/test_stl_string.cpp")
test_proj("test_stl_vector", "unit/core/test_stl_vector.cpp")
test_proj("test_stl_hash", "unit/core/test_stl_hash.cpp")
test_proj("test_stl_filesystem", "unit/core/test_stl_filesystem.cpp")
-- kimix::sort (pdqsort) dispatch + correctness; the perf comparison against
-- std::sort is the env-gated benchmark on the next line.
test_proj("test_stl_pdqsort", "unit/core/test_stl_pdqsort.cpp")
-- Env-gated pdqsort vs std::sort benchmark (runs nothing unless KIMIX_PDQSORT_BENCH=1).
test_proj("test_stl_pdqsort_bench", "unit/core/test_pdqsort_bench.cpp")
test_proj("test_platform", "unit/core/test_platform.cpp")
test_proj("test_basic_types", "unit/core/test_basic_types.cpp")
test_proj("test_pool", "unit/core/test_pool.cpp")
test_proj("test_first_fit", "unit/core/test_first_fit.cpp")
test_proj("test_string_scratch", "unit/core/test_string_scratch.cpp")
test_proj("test_binary_file_stream", "unit/core/test_binary_file_stream.cpp")
test_proj("test_clock", "unit/core/test_clock.cpp")
test_proj("test_format", "unit/core/test_format.cpp")
test_proj("test_json_repair", "unit/core/test_json_repair.cpp")
-- unit/core (kimix::fiber over the vendored marl: scheduler, events, counters,
-- futures, parallel/async_parallel, SharedFunction)
test_proj("test_fiber", "unit/core/test_fiber.cpp")
-- unit/core (md5/guid ported from LuisaCompute vstl, plus kimix::HashMap)
test_proj("test_md5", "unit/core/test_md5.cpp")
test_proj("test_guid", "unit/core/test_guid.cpp")
test_proj("test_hash_map", "unit/core/test_hash_map.cpp")
-- unit/core (kimix::shared_atomic_mutex: the shared/exclusive spin lock and its
-- RAII guards — mutual exclusion, reader inclusion, writer preference, the
-- try/upgrade forms, guard moves and the BasicLockable contract)
test_proj("test_shared_atomic_mutex", "unit/core/test_shared_atomic_mutex.cpp")
-- unit/core (kimix::LMDB: the wrapper's read path — a refused get reports absent
-- instead of a garbage span, and read_into copies the value out of the mapping so
-- no decode ever runs against pages a commit can reuse)
test_proj("test_lmdb", "unit/core/test_lmdb.cpp")

-- unit/ext
test_proj("test_yyjson", "unit/ext/test_yyjson.cpp")
test_proj("test_xxhash", "unit/ext/test_xxhash.cpp")
-- unit/ext (vendored Lua 5.5.1 as the shared `lua` target: C API, error
-- handling, C callbacks, GC/allocator stress over the mimalloc-backed luaL_alloc)
test_proj("test_lua", "unit/ext/test_lua.cpp", function()
    add_deps("lua")
end)
      
-- unit/script (kimix-script): the Lua script sandbox (src/script).
-- test_script_sandbox -- lifecycle, isolation, budgets, marshalling
-- test_script_hostile -- the abort contract: 17 forbidden APIs x 7 wrappers
-- test_script_surface -- golden capability allowlist (fails on any new global)
-- test_script_host -- the host surface: bind_host_function / set_global /
--   run(RunSpec), the three host budgets, and the reserved-name rule
test_proj("test_script_sandbox", "unit/script/test_script_sandbox.cpp", function()
    add_deps("kimix-script")
end)

-- unit/ext (the kimix Lua memory arena, src/ext/lua/larena.c): size classes,
-- segment/free-list accounting, the huge tier, trim/purge, dry-run, and the
-- allocator's conservation invariants. Compiled only when the arena is on.
if has_config("lua_arena") then
    test_proj("test_lua_arena", "unit/ext/test_lua_arena.cpp", function()
        add_deps("lua")
    end)
-- unit/ext (table memory policy): array/node block sizes against
-- concretesize()/sizehash(), table.new/table.shrink, compaction plan+move
    test_proj("test_lua_table_mem", "unit/ext/test_lua_table_mem.cpp", function()
        add_deps("lua")
    end)
-- bench_lua_mem.cpp is deliberately NOT registered as a Boost.UT suite:
-- it is a benchmark with thresholds (its own main(), prints bands, exits
-- non-zero when a band is missed), built as a plain binary so CI can run
-- it without turning a measurement into a unit test.
    target("bench_lua_mem")
        set_kind("binary")
        add_rules("kimix_basic_settings")
        add_files("unit/ext/bench_lua_mem.cpp")
        add_includedirs(".", {
            public = true
        })
        add_deps("lua")
        set_values("kimix_enable_exception", true)
        target_end()
    end

test_proj("test_script_hostile", "unit/script/test_script_hostile.cpp", function()
    add_deps("kimix-script")
end)
test_proj("test_script_surface", "unit/script/test_script_surface.cpp", function()
    add_deps("kimix-script")
end)
test_proj("test_script_host", "unit/script/test_script_host.cpp", function()
    add_deps("kimix-script")
end)
-- Env-gated sandbox benchmark (runs nothing unless KIMIX_SCRIPT_BENCH=1): fresh vs
-- reused state, count-hook granularity, marshalling cost, the denial path.
test_proj("test_script_bench", "unit/script/test_script_bench.cpp", function()
    add_deps("kimix-script")
end)
-- pybind11 is not inherited from kimix-core any more (it is a binding-layer
-- facility, not a core one), so the two targets that include <pybind11/...>:
-- runtime_py's src/runtime/py layer and this compile gate, declare it
-- themselves.  A successful compile IS the assertion.
test_proj("test_pybind11", "unit/ext/test_pybind11.cpp", function()
    add_deps("kimix-pybind11")
end)
test_proj("test_mbedtls", "unit/ext/test_mbedtls.cpp", function()
    add_deps("kimix-llm", "kimix-mbedtls")
end)
-- The vendored SQLite amalgamation (kimix-sqlite3): header/source version sync,
-- FTS5 compiled in, and an FTS5 round-trip. kimix-sqlite3 is a dependency input
-- (always built), so there is no feature-gate concern.
test_proj("test_sqlite", "unit/ext/test_sqlite.cpp", function()
    add_deps("kimix-sqlite3")
end)

-- unit/api — the kimix_api C FFI library (src/api).
--
-- Behind the same file-level switch as the library itself: with
-- --kimix_enable_api=false src/api/xmake.lua is not included at all, so the
-- kimix_api target does not exist and this test must not name it as a dep.
-- The test drives the library exactly like a foreign binding does: only the
-- public C headers are included, and everything (buffers, JSON documents,
-- placeholder vectors) is allocated and released inside the DLL.
if has_config("kimix_enable_api") then
    test_proj("test_kimix_api", "unit/api/test_kimix_api.cpp", function()
        add_deps("kimix_api")
    end)
    test_proj("test_kimix_canonical", "unit/api/test_kimix_canonical.cpp", function()
        add_deps("kimix_api")
    end)
    -- unit/api (the Lua script sandbox C ABI): the ABI/layout gates, the
    -- mimalloc-owned handle, and the HOST SURFACE -- kimix_lua_host_bind /
    -- kimix_lua_global_set / kimix_lua_globals_clear / kimix_lua_run_call /
    -- kimix_lua_last_host_calls, with tables crossing BOTH ways as canonical
    -- JSON, the non-sticky host-failure RUNTIME a pcall catches, and the
    -- host-misuse KIMIX_ERR_INVALID_ARG paths.
    test_proj("test_ffi_lua", "unit/api/test_ffi_lua.cpp", function()
        add_deps("kimix_api")
    end)
end

-- unit/openai (SSE stream parser for OpenAI-compatible chat completions)
test_proj("test_openai_stream", "unit/openai/test_openai_stream.cpp")

-- unit/openai_responses (SSE stream parser for OpenAI Responses API)
test_proj("test_responses_stream", "unit/openai_responses/test_responses_stream.cpp")

-- unit/anthropic (SSE stream parser for Anthropic Messages API)
test_proj("test_anthropic_stream", "unit/anthropic/test_anthropic_stream.cpp")

-- unit/llm (unified LLM interface + create_llm dispatch)
test_proj("test_llm", "unit/llm/test_llm.cpp", function()
    add_deps("kimix-llm")
end)
-- unit/llm (capability pre-flight: thinking parts vs provider capabilities,
-- LLMNotSupported wording, "capabilities" config key parsing)
test_proj("test_capabilities", "unit/llm/test_capabilities.cpp", function()
    add_deps("kimix-llm")
end)
-- unit/llm (tool-call argument sanitizing for history echo-back)
test_proj("test_tool_arguments_sanitize", "unit/llm/test_tool_arguments_sanitize.cpp", function()
    add_deps("kimix-llm")
end)
-- unit/llm (invalid-JSON responses from the server must not be silent successes)
test_proj("test_invalid_server_json", "unit/llm/test_invalid_server_json.cpp", function()
    add_deps("kimix-llm")
end)
-- unit/llm (kimix::net HTTP(S) client: framing, chunked decoding, redirects,
-- cancel, timeouts - against the loopback one-shot server helper)
test_proj("test_http_client", "unit/llm/test_http_client.cpp", function()
    add_deps("kimix-llm")
end)
-- unit/llm (request-body building: UTF-8 policy of the embedded prompt-- templates + invalid-UTF-8 tolerance of the three providers' body builders)
test_proj("test_request_body", "unit/llm/test_request_body.cpp", function()
 add_deps("kimix-llm")
end)
-- unit/llm (E4 tool-call pairing repair: normalize_tool_call_ids - charset,
-- 64-char truncation, _2/_3 collision suffixes, empty-id repair - plus the
-- Anthropic merge of consecutive tool-result-only user messages)
test_proj("test_tool_call_ids", "unit/llm/test_tool_call_ids.cpp", function()
 add_deps("kimix-llm")
end)
-- unit/llm (provider wire options: E5 anthropic cache_control placement,
-- E6 max_tokens/max_completion_tokens/max_output_tokens, E7 thinking off,
-- E11 temperature/top_p passthrough)
test_proj("test_wire_options", "unit/llm/test_wire_options.cpp", function()
 add_deps("kimix-llm")
end)
-- unit/llm (E11 KIMI_* environment fallback chain from kimi_cli/llm.py:275-300)
test_proj("test_env_overrides", "unit/llm/test_env_overrides.cpp", function()
    add_deps("kimix-llm")
end)
-- unit/llm (lossless shell lexer: token/part tiling, quote/escape/substitution
-- regions, operator tables, comments, heredoc markers, oracle equivalence)
test_proj("test_bash_parser_lexer", "unit/llm/test_bash_parser_lexer.cpp", function() add_deps("kimix-llm") end)
-- unit/llm (lossless shell IR: models/parser round-trip, command-position
-- classification, heredoc semantics, nested contexts, wrapper hints, embedded
-- scripts, iterative-depth lossless reconstruction)
test_proj("test_bash_parser_shell_ir", "unit/llm/test_bash_parser_shell_ir.cpp", function() add_deps("kimix-llm") end)
-- unit/llm (native Git Bash kernel public API: fix_bash_command /
-- process_unquoted differential goldens vs. the Python reference)
test_proj("test_bash_parser", "unit/llm/test_bash_parser.cpp", function() add_deps("kimix-llm") end)
-- unit/llm (native Git Bash kernel: pure path/null-device primitive goldens)
test_proj("test_bash_parser_paths", "unit/llm/test_bash_parser_paths.cpp", function() add_deps("kimix-llm") end)
-- unit/llm (native Git Bash kernel: pure wrapper/cmd rule-function goldens)
test_proj("test_bash_parser_rules", "unit/llm/test_bash_parser_rules.cpp", function() add_deps("kimix-llm") end)
-- unit/llm (native Git Bash kernel: host-gated real Git Bash execution e2e)
test_proj("test_bash_parser_e2e", "unit/llm/test_bash_parser_e2e.cpp", function() add_deps("kimix-llm") end)
-- unit/kimi (Kimi/Moonshot provider wire: message conversion, reasoning
-- round-trip + preserved-thinking backfill, thinking/keep shapes, tool
-- schema normalization, token caps, prompt_cache_key, cached_tokens usage)
test_proj("test_kimi_wire", "unit/kimi/test_kimi_wire.cpp", function()
    add_deps("kimix-llm")
end)



-- NOTE: tests/unit/native/test_fts5_cjk.cpp was deleted (gap-closure phase 0,
-- D1): it included <native/fts5_cjk/fts5_cjk_core.h>, which does not exist
-- anywhere in the tree, and it was never registered as a test target here.
-- unit/native (kimix runtime scaffold)
test_proj("test_native_module", "unit/native/test_module.cpp", function()
    add_deps("runtime_py")
end)

-- unit/native (kimix runtime common kernels — utf8 / json_pretty / text_util)
test_proj("test_native_utf8", "unit/native/test_utf8.cpp", function()
    add_deps("runtime_py")
end)

-- unit/native (kimix runtime text/stream kernels — plans 001/002/003)
test_proj("test_native_token_count", "unit/native/test_token_count.cpp", function()
    add_deps("runtime_py")
end)
test_proj("test_native_sanitize", "unit/native/test_sanitize.cpp", function()
    add_deps("runtime_py")
end)
test_proj("test_native_ansi", "unit/native/test_ansi.cpp", function()
    add_deps("runtime_py")
end)
test_proj("test_native_line_processor", "unit/native/test_line_processor.cpp", function()
    add_deps("runtime_py")
end)
test_proj("test_native_print_stream", "unit/native/test_print_stream.cpp", function()
    add_deps("runtime_py")
end)

-- unit/native (kimix runtime index kernels — plan 004)
test_proj("test_native_ngram_tokenizer", "unit/native/test_ngram_tokenizer.cpp", function()
    add_deps("runtime_py")
end)
test_proj("test_native_inverted_index", "unit/native/test_inverted_index.cpp", function()
    add_deps("runtime_py")
end)
  test_proj("test_native_history_index", "unit/native/test_history_index.cpp", function()
      add_deps("runtime_py")
  end)


-- unit/native (kimix runtime search kernels — plan 005)
test_proj("test_native_bm25", "unit/native/test_bm25.cpp", function()
    add_deps("runtime_py")
end)
test_proj("test_native_distance", "unit/native/test_distance.cpp", function()
    add_deps("runtime_py")
end)
test_proj("test_native_fuzzy", "unit/native/test_fuzzy.cpp", function()
    add_deps("runtime_py")
end)
test_proj("test_native_hash_kernels", "unit/native/test_hash_kernels.cpp", function()
    add_deps("runtime_py")
end)
test_proj("test_native_rerank", "unit/native/test_rerank.cpp", function()
    add_deps("runtime_py")
end)

-- unit/native (kimix runtime codec kernels - plans 007/008/009/010)
test_proj("test_native_wire_envelope", "unit/native/test_wire_envelope.cpp", function()
    add_deps("runtime_py")
end)
test_proj("test_native_merge_buffer", "unit/native/test_merge_buffer.cpp", function()
    add_deps("runtime_py")
end)
test_proj("test_native_frame_writer", "unit/native/test_frame_writer.cpp", function()
    add_deps("runtime_py")
end)
test_proj("test_native_recv_buffer", "unit/native/test_recv_buffer.cpp", function()
    add_deps("runtime_py")
end)
test_proj("test_native_sse", "unit/native/test_sse.cpp", function()
    add_deps("runtime_py")
end)

  -- unit/native (kimix runtime parse kernels - plans 011/012)
  test_proj("test_native_comment_scanner", "unit/native/test_comment_scanner.cpp", function()
      add_deps("runtime_py")
  end)
  test_proj("test_native_shell_scanner", "unit/native/test_shell_scanner.cpp", function()
      add_deps("runtime_py")
      -- the deep-nesting abort test exercises 1024 levels of scanner
      -- recursion (the reference's _MAX_NESTING_DEPTH); give it a big stack
      add_ldflags("/STACK:16777216", {tools = {"cl", "clang_cl"}})
      add_ldflags("-Wl,-z,stack-size=16777216", {tools = {"gcc", "clang"}})
  end)

  -- unit/native (kimix runtime tools kernels - plan 013)
  test_proj("test_native_line_hash", "unit/native/test_line_hash.cpp", function()
      add_deps("runtime_py")
  end)
  test_proj("test_native_find_str", "unit/native/test_find_str.cpp", function()
      add_deps("runtime_py")
  end)
  test_proj("test_native_grep_scan", "unit/native/test_grep_scan.cpp", function()
      add_deps("runtime_py")
  end)

  -- unit/tools (kimix runtime compression kernels - plan 016)
  test_proj("test_compress", "unit/tools/test_compress.cpp", function()
      add_deps("runtime_py")
  end)

  -- unit/native (kimix runtime security/shell-safety kernels - plan 0582e09)
  test_proj("test_native_security", "unit/native/test_security.cpp", function()
      add_deps("runtime_py")
  end)
  test_proj("test_native_shell_safety", "unit/native/test_shell_safety.cpp", function()
      add_deps("runtime_py")
  end)
  test_proj("test_native_grep_pattern", "unit/native/test_grep_pattern.cpp", function()
      add_deps("runtime_py")
  end)

    -- unit/native (kimix runtime soul kernels - plans 014/015/016)
      test_proj("test_native_export_builder", "unit/native/test_export_builder.cpp", function()
          add_deps("runtime_py")
      end)
      -- Durable SQLite FTS5 history index (report.md section D, rows
      -- D3/D4/D6/D7/D8/D9): links kimix-llm (carries the index kernels +
      -- the fuzzy kernel) and kimix-sqlite3 (raw SQL for schema/corruption
      -- assertions). No runtime_py dependency.
      test_proj("test_native_sqlite_history_index", "unit/native/test_sqlite_history_index.cpp", function()
          add_deps("kimix-llm", "kimix-sqlite3")
      end)

    -- unit/native (kimix runtime diff kernels - plan 018)
    test_proj("test_native_diff", "unit/native/test_diff.cpp", function()
        add_deps("runtime_py")
    end)

  -- unit/native (kimix runtime glob kernels - plan 019)
  test_proj("test_native_glob", "unit/native/test_glob.cpp", function()
      add_deps("runtime_py")
  end)


-- ============================================================================
-- unit/builtin_tools: C++ ports of the kimi-agent built-in tools
-- (C:/dev/kimi-agent/plans/*.md). Every project links the kimix-llm static
-- library (src/builtin_tools/*), which transitively pulls kimix-core,
-- the hand-written HTTP(S) client (llm/http_client.h), mbedtls and the
-- vendored reproc process library.
-- ============================================================================
local function builtin_tools_test(name, source)
    test_proj(name, source, function()
        add_deps("kimix-llm")
    end)
end

builtin_tools_test("test_builtin_tool_types", "unit/builtin_tools/test_tool_types.cpp")
-- Generic Tool / ToolParams infrastructure tests (incl. fuzzy alias matching).
builtin_tools_test("test_builtin_tool", "unit/builtin_tools/test_tool.cpp")
-- The CLI display line contract of every built-in tool (Tool::operator()'s
-- display_str out-parameter, builtin_tools/tool.h).
builtin_tools_test("test_builtin_tool_display", "unit/builtin_tools/test_tool_display.cpp")
-- The oversized-result spill: a payload past kToolOutputSpillMaxBytes is
-- dumped to .kimix_cache and exchanged for a pointer payload
-- (tool_output_spill_scope, builtin_tools/tool.h).
builtin_tools_test("test_builtin_tool_output_spill", "unit/builtin_tools/test_tool_output_spill.cpp")
-- Parses the live kimi-agent agent_*.json manifests from KIMI_AGENT_ROOT and
-- resolves every tool entry through the registry (yyjson + fuzzy resolution).
builtin_tools_test("test_agent_manifests", "unit/builtin_tools/test_agent_manifests.cpp")

-- >>> BEGIN builtin_tools test registrations (per-tool lines go here) >>>
-- The bash-fix nesting cases exercise deep scanner recursion; the scanner
-- abandons the scan at its own stack bound, and these flags mirror
-- test_native_shell_scanner for toolchains that honour them (the current msvc
-- toolset keeps the 1 MiB default, which the bound is sized for).
test_proj("test_builtin_bash", "unit/builtin_tools/test_bash_tool.cpp", function()
    add_deps("kimix-llm")
    add_ldflags("/STACK:16777216", {tools = {"cl", "clang_cl"}})
    add_ldflags("-Wl,-z,stack-size=16777216", {tools = {"gcc", "clang"}})
end)
  builtin_tools_test("test_builtin_compact", "unit/builtin_tools/test_compact_tool.cpp")
  builtin_tools_test("test_builtin_edit", "unit/builtin_tools/test_edit_tool.cpp")
  -- Bug Tool Report reproductions (bug_tool.md): shell out to bash (execute /
  -- interactive tasks), so the same large stack as test_builtin_bash.
  test_proj("test_builtin_bug_tool_report", "unit/builtin_tools/test_bug_tool_report.cpp", function()
      add_deps("kimix-llm", "kimix-cli") -- cli_default_plan_path (plan tools wiring)
      add_ldflags("/STACK:16777216", {tools = {"cl", "clang_cl"}})
      add_ldflags("-Wl,-z,stack-size=16777216", {tools = {"gcc", "clang"}})
  end)
builtin_tools_test("test_builtin_fetch_url", "unit/builtin_tools/test_fetch_url_tool.cpp")
builtin_tools_test("test_builtin_glob", "unit/builtin_tools/test_glob_tool.cpp")
builtin_tools_test("test_builtin_grep", "unit/builtin_tools/test_grep_tool.cpp")
builtin_tools_test("test_builtin_grep_engine", "unit/builtin_tools/test_grep_engine.cpp")
-- Argument-combination matrix: engine vs an independent oracle across
-- pattern x mode x -i x head_limit x -B/-A x include, replayed serial and
-- parallel.
builtin_tools_test("test_builtin_grep_engine_matrix", "unit/builtin_tools/test_grep_engine_matrix.cpp")
-- Env-gated grep engine benchmark (runs nothing unless KIMIX_GREP_BENCH=1).
builtin_tools_test("test_builtin_grep_engine_bench", "unit/builtin_tools/test_grep_engine_bench.cpp")
builtin_tools_test("test_builtin_pwsh", "unit/builtin_tools/test_pwsh_tool.cpp")
builtin_tools_test("test_builtin_python", "unit/builtin_tools/test_python_tool.cpp")
builtin_tools_test("test_builtin_read", "unit/builtin_tools/test_read_tool.cpp")
builtin_tools_test("test_builtin_read_image", "unit/builtin_tools/test_read_image_tool.cpp")
builtin_tools_test("test_builtin_retrieve", "unit/builtin_tools/test_retrieve_tool.cpp")
-- Retrieve view served by the durable SQLite history index (rows D3/D4/D7).
builtin_tools_test("test_builtin_retrieve_sqlite", "unit/builtin_tools/test_retrieve_sqlite_index.cpp")
builtin_tools_test("test_builtin_todo", "unit/builtin_tools/test_todo_tool.cpp")
builtin_tools_test("test_builtin_web_search", "unit/builtin_tools/test_web_search_tool.cpp")
builtin_tools_test("test_builtin_write", "unit/builtin_tools/test_write_tool.cpp")
      builtin_tools_test("test_agent", "unit/agent/test_agent.cpp")
      builtin_tools_test("test_system_prompt", "unit/agent/test_system_prompt.cpp")
        -- Sub-agent scratch session dirs (src/agent/agent_host.cpp): the child
        -- session persists under <work_dir>/.kimix_cache/<session_id> and an
        -- anonymous session's temp dir is deleted when the session closes.
        builtin_tools_test("test_subagent_session_dir", "unit/agent/test_subagent_session_dir.cpp")
        -- Empty-prompt sub-agent run (agent_host runner): the soul's
        -- empty-input guard must surface as a successful "(no text output)"
        -- outcome, not a bare <system>ERROR:</system> (pass-7 finding).
        builtin_tools_test("test_subagent_empty_prompt", "unit/agent/test_subagent_empty_prompt.cpp")
    -- Phase-1 loop-resilience modules (src/agent/token_ledger.*,
    -- context_overflow.*, step_retry.*, errors.h) and the turn-level
    -- retry/overflow/restart/escalation behaviour driven by scripted fakes.
      builtin_tools_test("test_token_ledger", "unit/agent/test_token_ledger.cpp")
      builtin_tools_test("test_context_overflow", "unit/agent/test_context_overflow.cpp")
      builtin_tools_test("test_step_retry", "unit/agent/test_step_retry.cpp")
      builtin_tools_test("test_turn_resilience", "unit/agent/test_turn_resilience.cpp")
      -- Phase-1 loop-control modules (src/agent/tool_taxonomy.h,
      -- tool_loop_guard.*, verification_gate.*) and the turn-level detector /
      -- loop-recovery / reasoning-reset / verification-gate behaviour driven
      -- by scripted fakes.
        builtin_tools_test("test_tool_taxonomy", "unit/agent/test_tool_taxonomy.cpp")
        builtin_tools_test("test_tool_loop_guard", "unit/agent/test_tool_loop_guard.cpp")
        builtin_tools_test("test_verification_gate", "unit/agent/test_verification_gate.cpp")
        builtin_tools_test("test_loop_control", "unit/agent/test_loop_control.cpp")
        -- G9 dynamic-injection framework + the five providers
        -- (src/agent/dynamic_injection.*, src/agent/dynamic_injections/*) and
        -- the turn-level delivery/strip/normalize wiring in the soul.
        -- Phase 4 (part 2): the ContextPruner engine (src/agent/context_pruning.*):
    -- Tier A/B/C detectors, the protected set, the gates, the dry-run
    -- estimate, prune_with_policy, and the prune_N reserve/archive round trip
    -- through AgentSession (rows D1/D5).
    builtin_tools_test("test_context_pruning", "unit/agent/test_context_pruning.cpp")
    -- D2: the context_prune agent tool (src/builtin_tools/context_prune_tool.*):
    -- modes, validation refusals, dry-run idempotency, history application and
    -- the prune_N archiving through the bound soul.
    builtin_tools_test("test_context_prune_tool", "unit/builtin_tools/test_context_prune_tool.cpp")
    -- D12 + B4/B6 (src/cli): the /prune slash command (summary line, cooldown
    -- no-op, disabled-config refusal) and the session-store context.db
    -- backend (Python-session round trip, JSONL->DB migration, corrupt DB).
    test_proj("test_cli_prune", "unit/cli/test_cli_prune.cpp", function()
        add_deps("kimix-llm", "kimix-cli", "kimix-sqlite3")
    end)
    -- D11: step-1 auto-retrieval memory injection (src/agent/auto_retrieve.*):
    -- the three citation tiers, thresholds, last-2 exclusion, the token
    -- budget and the dedup set capped at 10.
    builtin_tools_test("test_auto_retrieve", "unit/agent/test_auto_retrieve.cpp")
    builtin_tools_test("test_dynamic_injection", "unit/agent/test_dynamic_injection.cpp")
        builtin_tools_test("test_compact_reminder", "unit/agent/test_compact_reminder.cpp")
        builtin_tools_test("test_todo_reminder", "unit/agent/test_todo_reminder.cpp")
        builtin_tools_test("test_budget_reminder", "unit/agent/test_budget_reminder.cpp")
        builtin_tools_test("test_context_meter", "unit/agent/test_context_meter.cpp")
        builtin_tools_test("test_target_churn", "unit/agent/test_target_churn.cpp")
        builtin_tools_test("test_turn_injections", "unit/agent/test_turn_injections.cpp")
  -- SQLite context store (src/agent/context_db.*, the context_db.py port):
  -- schema/WAL, JSONL record byte shape, rowid pagination, JSONL->DB
  -- migration (lenient parse), meta tables, cross-reopen persistence and a
  -- two-connection concurrency sanity check.
  builtin_tools_test("test_context_db", "unit/agent/test_context_db.cpp")

-- Phase 3 part 1 (G7/G8/B7): cancellation token + steer queue + Steer API,
-- the turn-level abort/wake-interrupt/stale-flush wiring, and the live
-- wire.jsonl writer's record byte shapes (src/agent/cancel.*, steer.*,
-- wire.*, agent_host.* and the turn-loop producers in soul.cpp).
builtin_tools_test("test_cancel_steer", "unit/agent/test_cancel_steer.cpp")
builtin_tools_test("test_wire", "unit/agent/test_wire.cpp")
-- F8 (audit G03): hallucinated tool-name recovery - normalize/redirect/fuzzy
-- kernels, argument-fit disambiguation and the soul-level auto-correct echo +
-- the typed not-found error.
builtin_tools_test("test_tool_name_resolver",
                   "unit/agent/test_tool_name_resolver.cpp")
-- F9 (audit G16/G17/G18/G24): the argument anti-hallucination repair
-- pipeline (unwrap/stringified/schema coercion/todo shape/long-param temp
-- files) + the F11 canonical call key.
builtin_tools_test("test_tool_argument_repair",
                   "unit/agent/test_tool_argument_repair.cpp")
-- F6 (audit G07): the tool lifecycle hooks engine (payloads, matchers,
-- fail-open isolation) and the PreToolUse / PostToolUse /
-- PostToolUseFailure wiring in execute_tool_call.
builtin_tools_test("test_hooks_engine", "unit/agent/test_hooks_engine.cpp")
-- F11 (audit G12) same-step duplicate short-circuit, F10 (kimisoul.py
-- 1983-1993) the rejection-stops-turn rule and G13 hide()/unhide() runtime
-- visibility - the turn-level dispatch policies.
builtin_tools_test("test_dispatch_policies",
                   "unit/agent/test_dispatch_policies.cpp")
-- Reflection mode (options::reflection): a failed tool call is appended to
-- <work_dir>/.kimix_cache/error_log/<session_id>.jsonl (original arguments,
-- Clock-measured elapsed time, message, output); off by default.
builtin_tools_test("test_reflection_error_log",
                   "unit/agent/test_reflection_error_log.cpp")
-- Media capability gate (read_media.py:532-539 parity): a tool result
-- carrying media (read_image's data_url) is refused with the reference's
-- ToolError wording for a model without image_in - a regular error tool
-- result, no image_url part in the history, and the turn continues.
builtin_tools_test("test_media_capability_gate",
                   "unit/agent/test_media_capability_gate.cpp")
-- Phase 3 part 2 (G1-G4/G10/G11): the approval gate (approval.*), the LLM
-- request recorder (llm_recorder.*) and the /btw side channel (btw.* +
-- KimiSoul::run_side_question).
builtin_tools_test("test_approval", "unit/agent/test_approval.cpp")
builtin_tools_test("test_llm_recorder", "unit/agent/test_llm_recorder.cpp")
builtin_tools_test("test_btw", "unit/agent/test_btw.cpp")
builtin_tools_test("test_builtin_plan", "unit/builtin_tools/test_plan_tool.cpp")
builtin_tools_test("test_builtin_job_output", "unit/builtin_tools/test_job_output_tool.cpp")
builtin_tools_test("test_builtin_agent", "unit/builtin_tools/test_agent_tool.cpp")
builtin_tools_test("test_builtin_workflow", "unit/builtin_tools/test_workflow_tool.cpp")
-- Cross-tool fuzzy alias matching tests (ToolParams::alias_map + the per-tool
-- param_alias literals in tool.h).
builtin_tools_test("test_builtin_param_aliases", "unit/builtin_tools/test_param_aliases.cpp")
-- Tool::valid() (environment / session availability) + the KimiSoul validity
-- gate and the bash -> pwsh shell fallback. Nothing here spawns a process, so
-- it is independent of the reproc-backed suites.
builtin_tools_test("test_builtin_tool_valid", "unit/builtin_tools/test_tool_valid.cpp")
-- Real subprocess lifecycle through the vendored reproc runner (the single
-- spawn path of the bash / pwsh / python tools): spawn, drain, stdin,
-- timeout kill, interactive task registry start/stop/wait and the stop-vs-drain
-- thread race. Skips cleanly when no bash/python is installed.
builtin_tools_test("test_builtin_process_runner",
                   "unit/builtin_tools/test_process_runner.cpp")
-- F12 (audit G26): the JSON-Schema meta-validation run at registration + the
-- F14 runtime register_external_tool API (host-answered wire tools).
builtin_tools_test("test_tool_registry_schema",
                   "unit/builtin_tools/test_tool_registry_schema.cpp")
-- H8: the persistent /code exec context (a long-lived interpreter child
-- speaking newline-JSON; state persists across /code calls). Skips the live
-- part when no python interpreter is installed.
builtin_tools_test("test_python_code_session",
                   "unit/builtin_tools/test_python_code_session.cpp")
-- <<< END builtin_tools test registrations <<<

-- ============================================================================
-- unit/cli: the native CLI (src/cli).  S3 registers the session-store tests;
-- S6 extends this file with the stream / REPL / command coverage.
-- ============================================================================
test_proj("test_cli", "unit/cli/test_cli.cpp", function()
    add_deps("kimix-llm", "kimix-cli")
end)
-- cli_skills: skill dir pipeline (COMMON_SKILL_DIRS + .kimix/skill.json + -s),
-- the two discover_skills layouts + frontmatter parsing, and the
-- format_skills_for_prompt rendering.
test_proj("test_cli_skills", "unit/cli/test_cli_skills.cpp", function()
    add_deps("kimix-llm", "kimix-cli")
end)

-- G8: the cross-platform Ctrl-C module (src/cli/cli_signal.*): handler
-- installation, the shared atomic flag, and the no-signal test hook.
test_proj("test_cli_signal", "unit/cli/test_cli_signal.cpp", function()
 add_deps("kimix-cli")
end)

-- ============================================================================
-- Audit follow-ups (context store / compaction hardening)
-- ============================================================================
-- Store-level audit rows (B5 meta write-through, B8 checkpoints, B9 structured
-- export + the markdown golden, B10 lenient record parse, B11 stale-reminder
-- strip, B12 replace_history / restore guard / backend detect).
test_proj("test_cli_session_store", "unit/cli/test_cli_session_store.cpp", function()
    -- kimix-cli carries cli_init_wizard.cpp, whose ShellExecuteW "open"
    -- (os.startfile parity) needs shell32 at link time.
    add_deps("kimix-llm", "kimix-cli")
    if is_plat("windows") then
        add_syslinks("shell32")
    end
end)
-- Compaction audit rows (C10 CompactionLedger + its soul emit sites, C12 the
-- agent-side mode->guidance mapping, C13 the aligned summarization transport
-- and estimated_token_count_for_model).
test_proj("test_compaction_ledger", "unit/agent/test_compaction_ledger.cpp", function()
    add_deps("kimix-llm")
end)

-- ============================================================================
-- CLI gap-closure suites (G5/I1-I9/H1-H11/E7)
-- ============================================================================
-- The extended slash layer + session commands: /yolo /afk, /reset alias,
-- list_command_infos, /add-dir /import /refresh-env, the /export resolved-path
-- forms, the /sessions cache table, /store release+recovery, /exit temp-folder
-- cleanup, the /plan pipeline, the prompt() closing loop, the >64 KB temp-file
-- rule, escape_file_paths and the "Prompt failed: {e}" wording.
test_proj("test_cli_slash_layer", "unit/cli/test_cli_slash_layer.cpp", function()
    add_deps("kimix-llm", "kimix-cli")
    if is_plat("windows") then
        add_syslinks("shell32")
    end
end)
-- The ANSI terminal markdown renderer (render_markdown + the theme table +
-- ANSI-aware wrap + terminal width) and the stream renderer's markdown buffer.
test_proj("test_cli_markdown", "unit/cli/test_cli_markdown.cpp", function()
    add_deps("kimix-llm", "kimix-cli")
end)
-- The /init wizard, the non-TTY boot auto-init, sub_provider parsing, the
-- .kimix/mcp.json diagnostics, print_error -> stdout, the native-acceleration
-- log, --no_think -> enable_thinking and the Python-only front-end refusal.
test_proj("test_cli_init_boot", "unit/cli/test_cli_init_boot.cpp", function()
    add_deps("kimix-llm", "kimix-cli")
    if is_plat("windows") then
        add_syslinks("shell32")
    end
end)

-- ============================================================================
-- Audit gap-closure suites (E1/E2 media parts, E10 coalescing, A7 recovery
-- seam, A9 parallel dispatch) - appended registrations only.
-- ============================================================================
-- E10 (soul/message.py): the system()/system_reminder() wrappers and the
-- Layer-1 coalesce passes applied in normalize_history.
builtin_tools_test("test_system_block_coalesce",
                   "unit/agent/test_system_block_coalesce.cpp")
-- E1/E2 (kosong ContentPart registry): the media adjunct of llm::Message,
-- the message_parts/message_set_parts sync helpers, the image_in/video_in
-- capability pre-flight and the three providers' part wire shapes.
test_proj("test_content_parts", "unit/llm/test_content_parts.cpp", function()
    add_deps("kimix-llm")
end)
-- E1/E2: the session-store round trip of media content parts (reference
-- record shape, restore into ContentPart, transcript/export placeholders).
test_proj("test_media_session_roundtrip",
          "unit/cli/test_media_session_roundtrip.cpp", function()
    add_deps("kimix-llm", "kimix-cli")
    if is_plat("windows") then
        add_syslinks("shell32")
    end
end)
-- A7 (kimisoul.py _run_with_connection_recovery): the on_retryable_error /
-- refresh_auth recovery seam wired into the step-retry loop.
builtin_tools_test("test_connection_recovery",
                   "unit/agent/test_connection_recovery.cpp")
-- A9 (bounded parallel tool dispatch): the dispatch_concurrency knob - the
-- rendezvous probe proves parallel execution, serial mode stays serial, the
-- duplicate short-circuit and original call order hold.
builtin_tools_test("test_parallel_dispatch",
                   "unit/agent/test_parallel_dispatch.cpp")
-- Crash repro (real CLI died dispatching bash + todo_list + glob): the
-- production bounded dispatch runs the three real builtin tools on fresh
-- instances over many scripted turns. Skips when no real Git Bash is
-- available (nothing to spawn then).
builtin_tools_test("test_parallel_real_tools",
                   "unit/agent/test_parallel_real_tools.cpp")
-- Crash repro (real CLI died dispatching bash + todo_list + glob): the
-- production app + REPL stack runs the three real builtin tools with a
-- scripted backend and a stdin pipe whose reader blocks during the turn
-- (the crashed session's exact shape); pins all three tool results
-- reaching the session history. Skips when no real Git Bash is available.
test_proj("test_cli_bash_todo_glob",
          "unit/cli/test_cli_bash_todo_glob.cpp", function()
    add_deps("kimix-llm", "kimix-cli")
    if is_plat("windows") then
        add_syslinks("shell32")
    end
end)
  builtin_tools_test("test_mcp_client",
                     "unit/native/test_mcp_client.cpp")
  builtin_tools_test("test_stream_filter",
                     "unit/llm/test_stream_filter.cpp")
-- Regression (0xC0000409, real CLI "execv(bin\release\kimix_cli.exe ...)
-- failed(-1073740791)"): resuming a SETTLED sub-agent session with
-- subagent(session_id=..., run_in_background=true) after send_message queued
-- a payload for it crashed the process.  The scripted-backend app fixture
-- drives the production install_subagent_runner path.
  test_proj("test_subagent_resume_crash",
            "unit/cli/test_subagent_resume_crash.cpp", function()
      add_deps("kimix-llm", "kimix-cli")
  end)
  -- Crash repro (real CLI "execv(bin\debug\kimix_cli.exe ...) failed
  -- (-1073740791)" after the model emitted subagent + send_message +
  -- list_agents in one step): the crashed session's sub-agent scratch wire
  -- holds only the metadata record, i.e. the process died while the
  -- background child was starting its turn overlapped with the parent turn.
  -- The scripted-backend app fixture stresses that exact overlap (slow
  -- parent continuation + reader-thread steers) over many iterations.
  test_proj("test_subagent_bg_startup_stress",
            "unit/cli/test_subagent_bg_startup_stress.cpp", function()
      add_deps("kimix-llm", "kimix-cli")
  end)

-- unit/graph_db -- the graph database (src/graph_db, kimix-graph-db).
--
-- S1 (in-memory container + LMDB cold-storage glue): test_graph_db is the real
-- suite. The other two are registered with minimal placeholder suites so the
-- build stays green until their subtasks land -- S2 fills in
-- test_graph_db_serialize.cpp (round-trip / incremental / dual-stage /
-- corruption) and S3 fills in test_graph_db_bench.cpp (read/write benchmarks
-- over bench_util.h). Their test_proj lines need no change then, and neither
-- does this comment.
test_proj("test_graph_db", "unit/graph_db/test_graph_db.cpp", function()
    add_deps("kimix-graph-db")
end)
test_proj("test_graph_db_serialize", "unit/graph_db/test_graph_db_serialize.cpp", function()
    add_deps("kimix-graph-db")
end)
-- Behaviour-parity suite for the two cold-store backends behind cold_store.h (plan
-- Phase 3): every persistence scenario of test_graph_db_serialize.cpp runs against
-- BOTH the LMDB ColdStorage and the flat-file FileColdStore, plus the file backend's
-- own contract (open-failure degradation, the windowed scan of a store past 4 MiB,
-- determinism, Database backend selection).
test_proj("test_graph_db_file_store",
  "unit/graph_db/test_graph_db_file_store.cpp", function()
    add_deps("kimix-graph-db")
end)
test_proj("test_graph_db_bench", "unit/graph_db/test_graph_db_bench.cpp", function()
    add_deps("kimix-graph-db")
end)
-- Edge-filtered traversal benchmark (README §5h / optimize_plan.md phase E0): the EDGE
-- half of the §5.10 filter. Split from test_graph_db_bench because that suite's fixture
-- is shared but its serialization rows take minutes; this one must iterate in seconds.
-- Gated on KIMIX_GRAPH_DB_BENCH like the other benches (a plain xmake test runs nothing),
-- and it prints [ctr] rows only when the root option graph_db_walk_counters is on
-- (KIMIX_GRAPH_DB_WALK_COUNTERS arrives through kimix-graph-db's public define).
test_proj("test_graph_db_bench_edges", "unit/graph_db/test_graph_db_bench_edges.cpp", function()
    add_deps("kimix-graph-db")
end)
-- GDI alignment (src/graph_db/reports/gdi_alignment.md): one suite per phase.
-- status pins every GdiStatus numeric value and the capability advertisement;
-- migrate covers the meta envelope and the v1 -> v2 store migration.
test_proj("test_graph_db_status", "unit/graph_db/test_graph_db_status.cpp", function()
    add_deps("kimix-graph-db")
end)
test_proj("test_graph_db_migrate", "unit/graph_db/test_graph_db_migrate.cpp", function()
    add_deps("kimix-graph-db")
end)
test_proj("test_graph_db_property", "unit/graph_db/test_graph_db_property.cpp", function()
    add_deps("kimix-graph-db")
end)
test_proj("test_graph_db_key", "unit/graph_db/test_graph_db_key.cpp", function()
    add_deps("kimix-graph-db")
end)
test_proj("test_graph_db_edge", "unit/graph_db/test_graph_db_edge.cpp", function()
    add_deps("kimix-graph-db")
end)
test_proj("test_graph_db_schema", "unit/graph_db/test_graph_db_schema.cpp", function()
    add_deps("kimix-graph-db")
end)
test_proj("test_graph_db_read", "unit/graph_db/test_graph_db_read.cpp", function()
    add_deps("kimix-graph-db")
end)
-- Native multi-hop traversal (plan v3 step 1.2): traverse_bfs / bounded
-- shortest_path vs a brute-force reference, cap/depth/orientation matrix,
-- §5.10 filter gating, determinism, 4-reader/1-writer stress.
  test_proj("test_graph_db_traverse", "unit/graph_db/test_graph_db_traverse.cpp", function()
        add_deps("kimix-graph-db")
    end)
  -- GDI alignment (src/graph_db/reports/gdi_alignment.md): one suite per phase.
  -- test_graph_db_scan.cpp covers the G1 global scan: Graph::for_each_vertex
  -- (shard-ascending iterator, early stop) and Database::scan_vertices over
  -- the paging matrix, the constraint matrix, handle equivalence,
  -- determinism and the empty graph.
  test_proj("test_graph_db_scan", "unit/graph_db/test_graph_db_scan.cpp", function()
      add_deps("kimix-graph-db")
  end)
  test_proj("test_graph_db_tx", "unit/graph_db/test_graph_db_tx.cpp", function()
    add_deps("kimix-graph-db")
end)
-- Phase-5 memory budget (plan v3 step 5.1): sizeof probes pin the hot
-- structs and a scaled 10k/60k store asserts the per-entity RSS ceilings,
-- so the 425 -> 300 MB peak-working-set target has a per-PR guard between
-- full graphbench gate runs (step 5.6).
test_proj("test_graph_db_memory", "unit/graph_db/test_graph_db_memory.cpp", function()
    add_deps("kimix-graph-db")
end)
test_proj("test_graph_db_bulk", "unit/graph_db/test_graph_db_bulk.cpp", function()
    add_deps("kimix-graph-db")
end)
-- The C ABI suite: it includes ONLY <graph_db/graph_c_api.h>, like
-- tests/unit/api/test_kimix_api.cpp does for the kimix_api FFI.
test_proj("test_graph_db_c_api", "unit/graph_db/test_graph_db_c_api.cpp", function()
    add_deps("kimix-graph-db")
end)

-- Configure-time record of the slow suites `xmake test` will NOT run, so the
-- omission is visible instead of silent (see the slow_suites table above).
if #_slow_skipped > 0 and is_config() then
    print("note: %d slow suite(s) built but not run by `xmake test`;"
        .. " configure with `xmake f --kimix_slow_tests=y` to include them: %s",
        #_slow_skipped, table.concat(_slow_skipped, ", "))
end
