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
    if is_plat("windows") then
        add_syslinks("advapi32", "bcrypt", {public = true})
        add_defines("_CRT_SECURE_NO_WARNINGS")
    elseif is_plat("linux") then
        add_syslinks("pthread", "atomic", {public = true})
        add_defines("MI_NO_THP")
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
-- cpp-httplib (HTTP/HTTPS client & server) — header-only
-- ============================================================================
target("kimix-cpp-httplib")
    set_kind("headeronly")
    on_load(function(target)
        target:add("includedirs", path.join(os.scriptdir(), "cpp-httplib"), {
            public = true
        })
    end)
target_end()

-- ============================================================================
-- OpenSSL (TLS/crypto) — vendored submodule built from source.
--
-- OpenSSL has its own build system (perl Configure + nmake/make), so instead
-- of compiling its sources with xmake we run that build once in before_build
-- (no-asm, no-shared, no-tests) into <buildir>/openssl/<plat>/<arch>/, then
-- expose the generated include/lib dirs publicly. A tiny stub source keeps
-- this a static target so public config propagates to consumers. The build is
-- cached via a marker file and skipped when already built.
-- ============================================================================

-- Locate the MSVC x64 developer environment script (Windows only).
local function find_vcvars64()
    local roots = {
        os.getenv("ProgramFiles(x86)"),
        os.getenv("ProgramFiles"),
        "C:/Program Files (x86)",
        "C:/Program Files",
    }
    for _, root in ipairs(roots) do
        if root and root ~= "" then
            for _, edition in ipairs({"Community", "Professional", "Enterprise", "BuildTools"}) do
                local p = path.join(root, "Microsoft Visual Studio", "2022", edition,
                                    "VC", "Auxiliary", "Build", "vcvars64.bat")
                if os.isfile(p) then
                    return p
                end
            end
        end
    end
    return nil
end

-- Locate a perl with the modules OpenSSL's Configure needs. A Strawberry Perl
-- extracted under .deps/perl-extract is preferred; otherwise fall back to
-- whatever `perl` resolves to on PATH.
local function find_perl()
    local deps_perl = path.join(os.projectdir(), ".deps", "perl-extract", "perl",
                                "bin", "perl.exe")
    if os.isfile(deps_perl) then
        return deps_perl
    end
    return "perl"
end

target("kimix-openssl")
    set_kind("static")
    add_files(path.join(os.scriptdir(), "openssl_stub.c"))
    on_load(function(target)
        local openssl_dir = path.join(os.scriptdir(), "openssl")
        local bd = get_config("buildir") or "build"
        local out = path.join(os.projectdir(), bd, "openssl", target:plat(), target:arch())
        target:data_set("openssl_dir", openssl_dir)
        target:data_set("openssl_out", out)
        target:add("includedirs", path.join(out, "include"), {public = true})
        target:add("linkdirs", path.join(out, "lib"), {public = true})
        if target:is_plat("windows") then
            -- Static OpenSSL on Windows needs these system libs at link time.
            target:add("links", "libssl", "libcrypto", {public = true})
            target:add("syslinks", "crypt32", "ws2_32", "user32", "advapi32", {public = true})
        else
            target:add("links", "ssl", "crypto", {public = true})
            if target:is_plat("linux") then
                target:add("syslinks", "dl", "pthread", {public = true})
            end
        end
    end)
    before_build(function(target)
        local dir = target:data("openssl_dir")
        local out = target:data("openssl_out")
        local marker = path.join(out, ".kimix-openssl-built")
        if os.isfile(marker) then
            return
        end
        local libdir = path.join(out, "lib")
        if os.isfile(path.join(libdir, "libssl.a"))
           or os.isfile(path.join(libdir, "libssl.lib")) then
            io.writefile(marker, "ok")
            return
        end

        os.mkdir(out)

        local conf
        if target:is_plat("windows") then
            conf = target:arch() == "x64" and "VC-WIN64A" or "VC-WIN32"
        elseif target:is_plat("linux") then
            conf = target:arch() == "arm64" and "linux-aarch64" or "linux-x86_64"
        elseif target:is_plat("macosx") then
            conf = target:arch() == "arm64" and "darwin64-arm64" or "darwin64-x86_64"
        else
            utils.error("kimix-openssl: unsupported platform " .. target:plat())
            return
        end

        -- Normalize to forward slashes: Windows makefiles are happier with them.
        local prefix = path.absolute(out):gsub("\\", "/")

        local ok = os.execv(find_perl(), {
            "Configure", conf, "no-asm", "no-shared", "no-tests",
            "--prefix=" .. prefix, "--openssldir=" .. prefix,
        }, {curdir = dir})
        -- os.execv returns the exit code (0 == success) and raises on failure.
        if ok ~= 0 then
            utils.error("kimix-openssl: Configure failed (perl=" .. find_perl()
                        .. ", exit=" .. tostring(ok) .. ")")
            return
        end

        if target:is_plat("windows") then
            local vcvars = find_vcvars64()
            if not vcvars then
                utils.error("kimix-openssl: cannot locate vcvars64.bat")
                return
            end
            -- os.execv escapes embedded quotes in args, so passing the whole
            -- `call "vcvars" && ...` line inline breaks cmd parsing. Write a
            -- batch file (path has no spaces) and execute that instead.
            local bat = path.join(out, "build_openssl.bat")
            io.writefile(bat, string.format(
                "@echo off\r\n"
                .. "call \"%s\"\r\n"
                .. "cd /d \"%s\"\r\n"
                .. "nmake install_sw\r\n", vcvars, dir))
            ok = os.execv("cmd.exe", {"/c", bat})
        else
            local jobs = os.getenv("NUMBER_OF_PROCESSORS") or "4"
            ok = os.execv("make", {"-j" .. jobs, "install_sw"}, {curdir = dir})
        end
        if ok ~= 0 then
            utils.error("kimix-openssl: build failed (exit=" .. tostring(ok) .. ")")
            return
        end

        io.writefile(marker, "ok")
        print("kimix-openssl: built OpenSSL into " .. prefix)
    end)
target_end()
