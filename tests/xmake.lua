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

-- unit/native (kimix runtime scaffold)
test_proj("test_native_module", "unit/native/test_module.cpp", function()
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

-- >>> BEGIN builtin_tools test registrations (per-tool lines go here) >>>
builtin_tools_test("test_builtin_glob", "unit/builtin_tools/test_glob_tool.cpp")
builtin_tools_test("test_builtin_grep", "unit/builtin_tools/test_grep_tool.cpp")
builtin_tools_test("test_builtin_read", "unit/builtin_tools/test_read_tool.cpp")
builtin_tools_test("test_builtin_read_image", "unit/builtin_tools/test_read_image_tool.cpp")
builtin_tools_test("test_builtin_write", "unit/builtin_tools/test_write_tool.cpp")
builtin_tools_test("test_builtin_edit", "unit/builtin_tools/test_edit_tool.cpp")
builtin_tools_test("test_builtin_bash", "unit/builtin_tools/test_bash_tool.cpp")
builtin_tools_test("test_builtin_pwsh", "unit/builtin_tools/test_pwsh_tool.cpp")
builtin_tools_test("test_builtin_python", "unit/builtin_tools/test_python_tool.cpp")
builtin_tools_test("test_builtin_fetch_url", "unit/builtin_tools/test_fetch_url_tool.cpp")
builtin_tools_test("test_builtin_web_search", "unit/builtin_tools/test_web_search_tool.cpp")
-- <<< END builtin_tools test registrations <<<
