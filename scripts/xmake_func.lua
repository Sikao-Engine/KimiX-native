--[[
    xmake_func.lua - Build configuration functions for Kimix
--]]

-- ============================================================================
-- SECTION 1: Internal Options
-- ============================================================================

-- Environment validation option
option("_kimix_check_env")
set_showmenu(false)
set_default(false)
after_check(function(option)
    -- Validate architecture (only x64 and arm64 are supported)
    if not is_arch("x64", "x86_64", "arm64") then
        option:set_value(false)
        utils.error("Illegal environment. Please check your compiler, architecture or platform.")
        return nil
    end
    -- Validate build mode
    if not (is_mode("debug") or is_mode("release") or is_mode("releasedbg")) then
        option:set_value(false)
        utils.error("Illegal mode. set mode to 'release', 'debug' or 'releasedbg'.")
        return nil
    end
    option:set_value(true)
end)
option_end()

-- Binary output directory configuration option
option("_kimix_bin_dir")
set_default(false)
set_showmenu(false)
add_deps("kimix_bin_dir")

before_check(function(option)
    -- Set binary directory based on build mode
    local bin_dir = option:dep("kimix_bin_dir"):enabled()
    if is_mode("debug") then
        bin_dir = path.join(bin_dir, "debug")
    elseif is_mode("releasedbg") then
        bin_dir = path.join(bin_dir, "releasedbg")
    else
        bin_dir = path.join(bin_dir, "release")
    end
    option:set_value(bin_dir)
end)
option_end()

-- ============================================================================
-- SECTION 2: Build Rules
-- ============================================================================

-- Basic settings rule applied to all targets
rule("kimix_basic_settings")
on_config(function(target)
    -- Linux-specific: Use libc++ with Clang
    if target:is_plat("linux") then
        if target:has_tool("cxx", "clang", "clangxx") then
            target:add("cxflags", "-stdlib=libc++", {
                force = true
            })
            target:add("syslinks", "c++")
        end
    end
end)

on_load(function(target)
    -- Helper function to get configuration value from multiple sources
    local function _get_or(name, default_value)
        local v = target:extraconf("rules", "kimix_basic_settings", name)
        name = 'kimix_' .. name
        if v == nil then
            v = target:values(name)
        end
        if v == nil then
            v = get_config(name)
        end
        if v then
            return v
        end
        return default_value or false
    end

    local function empty_str(value)
        return type(value) == 'string' and #value == 0
    end

    -- Apply toolchain configuration
    local toolchain = _get_or("toolchain")
    if toolchain and not empty_str(toolchain) then
        target:set("toolchains", toolchain)
    end

    -- Apply project type (static/shared library, executable, etc.)
    local project_kind = _get_or("project_kind")
    if project_kind and not empty_str(project_kind) then
        target:set("kind", project_kind)
    end

    -- Linux: Position independent code for static libraries
    if target:is_plat("linux") then
        if project_kind == "static" or project_kind == "object" then
            target:add("cxflags", "-fPIC")
        end
    end

    -- macOS-specific flags
    if target:is_plat("macosx") then
        target:add("cxflags", "-no-pie")
        target:add("cxflags", "-Wno-invalid-specialization", {
            tools = {"clang"}
        })
    end

    -- Enable FMA (Fused Multiply-Add) on x64 platforms
    if target:is_arch("x64", "x86_64") then
        target:add("cxflags", "-mfma", {
            tools = {"clang", "gcc"}
        })
    end

    -- Set C/C++ language standards
    local c_standard = _get_or("c_standard")
    local cxx_standard = _get_or("cxx_standard")
    if c_standard and not empty_str(c_standard) then
        target:set("languages", c_standard, {
            public = true
        })
    end
    if cxx_standard and not empty_str(cxx_standard) then
        target:set("languages", cxx_standard, {
            public = true
        })
    end

    -- Configure exception handling
    local enable_exception = _get_or("enable_exception")
    if not empty_str(enable_exception) then
        if enable_exception then
            target:set("exceptions", "cxx")
        else
            target:set("exceptions", "no-cxx")
            if target:is_plat('windows') then
                target:add('defines', '_HAS_EXCEPTIONS=0')
            end
        end
    end

    -- Mode-specific configurations
    local win_runtime
    local opt
    if is_mode("debug") then
        win_runtime = _get_or('win_runtime', 'MDd')
        opt = _get_or("optimize", "none")
        target:add("cxflags", "/GS", "/Gd", {
            tools = {"clang_cl", "cl"},
            public = true
        })
    elseif is_mode("releasedbg") then
        win_runtime = _get_or('win_runtime', 'MD')
        opt = _get_or("optimize", "none")
        target:add("cxflags", "/GS-", "/Gd", {
            tools = {"clang_cl", "cl"},
            public = true
        })
    else
        win_runtime = _get_or('win_runtime', 'MD')
        opt = _get_or("optimize", "aggressive")
        target:add("cxflags", "/GS-", "/Gd", {
            tools = {"clang_cl", "cl"},
            public = true
        })
    end

    if not empty_str(opt) then
        target:set("optimize", opt)
    end

    local warnings = _get_or("warnings", "none")
    if not empty_str(warnings) then
        target:set("warnings", warnings)
    end

    if not empty_str(win_runtime) then
        target:set("runtimes", win_runtime, {
            public = true
        })
    end

    -- MSVC-specific preprocessor settings
    target:add("cxflags", "/Zc:preprocessor", "/wd4244", {
        tools = "cl",
        public = true
    });

    -- SIMD extensions configuration
    if _get_or("enable_simd") then
        if is_arch("arm64") then
            -- NEON is always available on aarch64
            if not target:is_plat("macosx", "linux") then
                target:add("vectorexts", "neon", {
                    public = true
                })
            end
        else
            target:add("vectorexts", "avx", "avx2", {
                public = true
            })
        end
    end

    -- Link Time Optimization (LTO) configuration
    local use_lto = _get_or("lto", false)
    if not empty_str(use_lto) then
        target:set("policy", "build.optimization.lto", use_lto)
        if use_lto then
            -- Use LLVM tools when using Clang toolchain with LTO
            if toolchain and (toolchain:find("clang") or toolchain:find("llvm")) then
                target:set("toolset", "ld", "lld-link")
                target:set("toolset", "ar", "llvm-ar")
            end
        end
    end

    -- RTTI (Run-Time Type Information) configuration
    local use_rtti = _get_or("rtti", false)
    if not empty_str(use_rtti) then
        if use_rtti then
            -- Enable RTTI
            target:add("cxflags", "/GR", {
                tools = {"clang_cl", "cl"}
            })
        else
            -- Disable RTTI
            target:add("cxflags", "/GR-", {
                tools = {"clang_cl", "cl"}
            })
            target:add("cxflags", "-fno-rtti", "-fno-rtti-data", {
                tools = {"clang"}
            })
            target:add("cxflags", "-fno-rtti", {
                tools = {"gcc"}
            })
        end
    end
end)
rule_end()

-- ============================================================================
-- SECTION 3: Target Execution Rule
-- ============================================================================

-- Rule for running built targets with proper working directory
rule("kimix_run_target")
on_run(function(target)
    import("core.base.option")

    -- Get target name from rule config or use target name
    local name = target:extraconf("rules", "kimix_run_target", "name")
    if not name then
        name = target:name()
    end

    local arguments = option.get("arguments")
    local tar_dir = path.absolute(target:targetdir())

    os.execv(path.join(tar_dir, name), arguments, {
        curdir = tar_dir
    })
end)
rule_end()

-- ============================================================================
-- SECTION 4: Global Configuration Functions
-- ============================================================================

-- Initialize default config rules
if _config_rules == nil then
    _config_rules = {"kimix_basic_settings"}
end

-- Unity build configuration
if _disable_unity_build == nil then
    local unity_build = get_config("kimix_enable_unity_build")
    if unity_build ~= nil then
        _disable_unity_build = not unity_build
    end
end

-- Main project configuration function
if not _config_project then
    function _config_project(config)
        -- Apply unity build if enabled and batch size is valid
        local batch_size = config["batch_size"]
        if type(batch_size) == "number" and batch_size > 1 and (not _disable_unity_build) then
            add_rules("c.unity_build", {
                batchsize = batch_size
            })
            add_rules("c++.unity_build", {
                batchsize = batch_size
            })
        end

        -- Apply configuration rules
        if type(_config_rules) == "table" then
            add_rules(_config_rules, config)
        end
    end
end
