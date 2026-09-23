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
