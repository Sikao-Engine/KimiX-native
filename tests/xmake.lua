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
        -- kimix-core is built with KIMIX_CORE_EXPORT_DLL (so runtime_py.pyd
        -- re-exports core API like hash64); tests keep plain references and
        -- link the static copy through kimix-core.
        add_defines("KIMIX_CORE_STATIC")
        -- The Boost.UT harness runs a test body only inside
        -- `#if defined(__cpp_exceptions)` (tests/ut/ut.hpp, runner::
        -- on(events::test<...>) wraps `test()` in try/catch and has no
        -- exception-free branch). With exceptions disabled the suite silently
        -- registers every test but never executes one ("0 asserts in N tests"),
        -- so every test binary keeps exceptions enabled. The kimix library
        -- targets remain exception-free (kimix_enable_exception=false).
        set_values("kimix_enable_exception", true)
        _config_project({batch_size = 8})
        add_tests("default")
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

test_proj("test_stl_allocator", "unit/core/test_stl_allocator.cpp")
test_proj("test_stl_string", "unit/core/test_stl_string.cpp")
test_proj("test_stl_vector", "unit/core/test_stl_vector.cpp")
test_proj("test_stl_hash", "unit/core/test_stl_hash.cpp")
test_proj("test_basic_types", "unit/core/test_basic_types.cpp")
test_proj("test_pool", "unit/core/test_pool.cpp")
test_proj("test_first_fit", "unit/core/test_first_fit.cpp")
test_proj("test_string_scratch", "unit/core/test_string_scratch.cpp")
test_proj("test_binary_file_stream", "unit/core/test_binary_file_stream.cpp")
test_proj("test_clock", "unit/core/test_clock.cpp")
test_proj("test_format", "unit/core/test_format.cpp")
test_proj("test_json_repair", "unit/core/test_json_repair.cpp")

-- unit/ext
test_proj("test_yyjson", "unit/ext/test_yyjson.cpp")
test_proj("test_xxhash", "unit/ext/test_xxhash.cpp")
test_proj("test_pybind11", "unit/ext/test_pybind11.cpp")
test_proj("test_mbedtls", "unit/ext/test_mbedtls.cpp", function()
    add_deps("kimix-cpp-httplib", "kimix-mbedtls")
    add_defines("CPPHTTPLIB_MBEDTLS_SUPPORT")
end)

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
    add_deps("kimix-llm", "kimix-cpp-httplib")
    add_defines("CPPHTTPLIB_MBEDTLS_SUPPORT")
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
-- cpp-httplib, mbedtls and the vendored reproc process library.
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
