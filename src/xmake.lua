-- Kimix library
target("kimix-core")
    set_kind("static")
    add_files("core/*.cpp")
    add_files("core/stl/*.cpp")       -- NEW: stl implementations
    add_headerfiles("core/*.h")
    add_headerfiles("core/stl/*.h")   -- NEW: stl headers
    add_headerfiles("core/stl/*.inl") -- if any .inl files
    add_includedirs(".", {public = true})
    add_deps("mimalloc", "spdlog", "kimix-xxhash", "kimix-marl")  -- NEW dependencies
    kimix_set_pcxxheader("core/pch.h")
    _config_project({batch_size = 8})
    on_load(function(target)
        -- kimix-core is a static library: KIMIX_CORE_STATIC (public, inherited by
        -- dependents) makes KIMIX_CORE_API expand to nothing on both sides.
        target:add("defines", "KIMIX_CORE_STATIC", {public = true})
        if has_config('kimix_disable_win_message_box') and target:is_plat('windows') then
            target:add('defines', 'KIMIX_DISABLE_WIN_MESSAGE_BOX', {public = true})
        end
        if target:is_plat("windows") then
            target:add("defines", "NOMINMAX", "_CRT_SECURE_NO_WARNINGS", {public = true})
            target:add("defines", "KIMIX_PLATFORM_WINDOWS", {public = true})  -- NEW
            if is_mode("debug") then
                target:add("syslinks", "Dbghelp")  -- for backtrace
            end
        elseif target:is_plat("linux") then
            target:add("defines", "KIMIX_PLATFORM_UNIX", {public = true})
            target:add("syslinks", "dl", "pthread")
        elseif target:is_plat("macosx") then
            target:add("defines", "KIMIX_PLATFORM_UNIX", "KIMIX_PLATFORM_APPLE", {public = true})
        end
    end)
target_end()

-- Kimix test executable
target("kimix-test")
    set_kind("binary")
    add_files("test/main.cpp")
    add_deps("kimix-core")
    _config_project({batch_size = 8})

    add_tests("basic", {
        runargs = {},
        group = "unit",
    })
target_end()

-- Include extensions
includes("ext")




-- ============================================================================
-- Kimix Runtime — shared library: kernels + C-FFI (runtime.dll)
-- ============================================================================
target("runtime")
    set_kind("shared")
    add_files("runtime/**.cpp")
    remove_files("runtime/py/**.cpp")   -- pybind bindings belong to runtime_py
    add_headerfiles("runtime/**/*.h")
    add_includedirs("..", {public = true}) -- expose src/ so <runtime/runtime.h> works
    add_deps("kimix-core", "kimix-yyjson", "kimix-xxhash")
    kimix_set_pcxxheader("runtime/pch.h")
    _config_project({batch_size = 8})
    on_load(function(target)
        -- exporting DLL: private define (consumers see dllimport by default)
        target:add("defines", "KIMIX_RUNTIME_EXPORT_DLL")
        target:add("defines", "KIMIX_CORE_STATIC", {public = true})
    end)
target_end()

-- ============================================================================
-- Kimix Runtime Python module (runtime_py.pyd)
-- ============================================================================
target("runtime_py")
    set_kind("shared")
    set_basename("runtime_py")
    add_rules("kimix_basic_settings")      -- RTTI-off etc., but NO unity build
    add_files("runtime/py/**.cpp")
    add_deps("runtime", "kimix-pybind11")
    on_load(function(target)
        if target:is_plat("windows") then
            target:set("filename", "runtime_py.pyd")
        end
        local function py_conf(var)
            local out = os.iorunv("python", {"-c", "import sysconfig; print(sysconfig." .. var .. ")"})
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
            target:add("links", "python314")
        end
        target:add("defines", "KIMIX_CORE_STATIC")
    end)
target_end()
