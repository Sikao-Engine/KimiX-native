set_xmakever("3.0.6")
add_rules("mode.release", "mode.debug", "mode.releasedbg")
set_policy("build.ccache", not is_plat("windows"))
set_policy("check.auto_ignore_flags", false)

-- ============================================================================
-- Pre-defined options
-- ============================================================================

-- enable unity(jumbo) build, enable this option will optimize compile speed
option("kimix_enable_unity_build", {
    default = true
})
-- enable Pre-Compiled header
option("kimix_enable_pch", {
    default = true
})
-- enable SSE and SSE2 SIMD
option("kimix_enable_simd", {
    default = true
})
-- C++ standard version (e.g., cxx17, cxx20, cxx23)
option("kimix_cxx_standard", {
    default = 'cxx20'
})
-- C standard version (e.g., c11, clatest)
option("kimix_c_standard", {
    default = 'clatest'
})
-- enable C++ Run-Time Type Information (RTTI)
-- Default false: every kimix target is compiled with RTTI disabled, so
-- `dynamic_cast` and `typeid` (and therefore "read the type id out of the
-- vtable pointer" tricks) are forbidden in kimix code.  Use `static_cast`
-- plus an explicit tag (a virtual tag getter / enum member) instead.
option("kimix_rtti", {
    default = false
})
-- custom binary output directory
option("kimix_bin_dir", {
    default = "bin"
})
-- custom toolchain path or name
option("kimix_toolchain", {
    default = false
})
-- Windows runtime library (MT/MD/MTd/MDd)
option("kimix_win_runtime", {
    default = false
})
-- additional optimization flags
option("kimix_optimize", {
    default = false
})
-- enable Link Time Optimization (LTO) for smaller binary size
option("kimix_use_lto", {
    default = false
})
-- enable C++ exceptions
-- Default false: the whole project is built WITHOUT C++ exceptions.  No
-- `throw` / `try` / `catch` is allowed in kimix code; failures travel through
-- return values (bool + error message / error codes).  The only exceptions are
-- the pybind11 binding translation units, which pybind11 (third-party,
-- src/ext/pybind11) itself requires -- see kimix_exceptions_targets below.
option("kimix_enable_exception", {
    default = false
})
-- Targets that keep C++ exceptions enabled even though the project disables
-- them (comma separated).  pybind11 cannot be compiled without exceptions: its
-- PYBIND11_MODULE macro, pybind11_fail() and every PYBIND11_RUNTIME_EXCEPTION()
-- are implemented with try/catch/throw, and src/ext/pybind11 is third-party
-- code this project never modifies.  runtime_py is the pybind11 extension
-- module and test_pybind11 is the unit test for that binding layer.
option("kimix_exceptions_targets", {
    default = "runtime_py,test_pybind11"
})
-- enable tests module
option("kimix_enable_tests", {
    default = true
})
-- enable the kimix-llm library: the LLM providers (src/llm), the agent turn
-- loop (src/agent), the built-in tools (src/builtin_tools) and the MCP client
-- (src/mcp) as one static library.  Everything that links it is skipped with
-- it: kimix-cli, kimix_cli, runtime_py, the LLM/agent demos and the unit tests
-- on top of them (see the kimix_feature_gate rule in scripts/xmake_func.lua).
option("kimix_enable_llm", {
    default = true
})
-- enable the native CLI: the kimix-cli static library and the kimix_cli
-- executable.  It links kimix-llm, so kimix_enable_llm has to be on as well;
-- the after_check below only reports that, the gate does the skipping.
option("kimix_enable_cli")
    set_default(true)
    set_description("build kimix-cli / kimix_cli (requires kimix_enable_llm)")
    add_deps("kimix_enable_llm")
    after_check(function(option)
        if option:enabled() and not option:dep("kimix_enable_llm"):enabled() then
            print("note: kimix_enable_cli is ignored while kimix_enable_llm is off"
                .. " (skipping kimix-cli, kimix_cli)")
        end
    end)
option_end()
-- enable the Python extension module runtime_py (the src/runtime kernels plus
-- the pybind11 binding layer in src/runtime/py).  It links kimix-llm, because
-- part of the kernels is compiled into that library and re-exported from the
-- module, so kimix_enable_llm has to be on as well.
option("kimix_enable_runtime")
    set_default(true)
    set_description("build the runtime_py python module (requires kimix_enable_llm)")
    add_deps("kimix_enable_llm")
    after_check(function(option)
        if option:enabled() and not option:dep("kimix_enable_llm"):enabled() then
            print("note: kimix_enable_runtime is ignored while kimix_enable_llm is off"
                .. " (skipping runtime_py)")
        end
    end)
option_end()

-- enable the C-FFI library: kimix_api (src/api), a shared library that exposes
-- a plain C ABI (mimalloc allocation, the yyjson JSON surface with the mimalloc
-- allocator baked in, the kimix::vector<std::byte> placeholder, and
-- kimix::repair) for foreign-language callers.  Only kimix-core is needed, so
-- this switch is independent of kimix_enable_llm / _cli / _runtime.
-- Turning it off skips the whole src/api/xmake.lua file: the target, its
-- headers and the test on top of it disappear from the configuration.
option("kimix_enable_api")
    set_default(true)
    set_showmenu(true)
    set_description("build the kimix_api C FFI shared library (src/api)")
option_end()

-- disable Windows message box (redirect asserts/errors to stderr instead)
option("kimix_disable_win_message_box", {
    default = true
})


-- ============================================================================
-- Local user options (options.lua, gitignored)
-- ============================================================================
-- Optional user config file; each entry is applied to the config via
-- set_config(). Same pattern as C:/dev/LuisaCompute/xmake.lua
-- (lc_options + set_config loop).
if os.exists("options.lua") then
    includes("options.lua")
end
if kimix_options then
    for k, v in pairs(kimix_options) do
        set_config(k, v)
    end
end

-- ============================================================================
-- PCH helper
-- ============================================================================

function kimix_set_pcxxheader(...)
    if get_config('kimix_enable_pch') then
        set_pcxxheader(...)
    end
end

-- ============================================================================
-- Target feature gates
-- ============================================================================
--
-- kimix_enable_llm / kimix_enable_cli / kimix_enable_runtime decide which
-- targets a build contains, and a target that links a skipped target is
-- skipped too.  The gate itself (the maps, kimix_feature_enabled(),
-- kimix_target_gate() and the kimix_feature_gate rule that applies them) lives
-- in scripts/xmake_func.lua, because that is the scope every kimix target
-- shares through _config_project().
--
-- kimix_enable_api uses the other mechanism: it is a file-level skip, so
-- src/api/xmake.lua is not even included when the option is off (see src/
-- xmake.lua).  Nothing links kimix_api except its own test, which is skipped
-- with the same condition.
--

-- ============================================================================
-- Internal options
-- ============================================================================

-- internal: xmake scripts directory path
option("kimix_scripts_path")
set_showmenu(false)
set_default(false)
after_check(function(option)
    option:set_value(path.join(os.scriptdir(), 'scripts'))
end)
option_end()

-- ============================================================================
-- Include build functions
-- ============================================================================

includes("scripts/xmake_func.lua")
-- ============================================================================
-- Build targets
-- ============================================================================

if has_config('_kimix_check_env') then
    local kimix_bin_dir = get_config("_kimix_bin_dir")
    if kimix_bin_dir then
        set_targetdir(kimix_bin_dir)
    end
    includes("src")
    -- Include test targets
    includes("tests/xmake.lua")
end
