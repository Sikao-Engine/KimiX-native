--[[
    xmake_func.lua - Build configuration functions for Kimix
--]]

-- ============================================================================
-- SECTION 0: Target feature gates
-- ============================================================================
--
-- The switches declared in xmake.lua (kimix_enable_llm, kimix_enable_cli,
-- kimix_enable_runtime) decide which targets a configuration contains:
--
--   kimix-core              always built -- the base library everything links
--   kimix-llm               kimix_enable_llm
--   kimix-cli, kimix_cli    kimix_enable_cli     (also needs kimix_enable_llm)
-- runtime_py kimix_enable_runtime (also needs kimix_enable_llm)
--
-- and a target that links a skipped target is skipped with it: the unit tests
-- that add_deps() one of the libraries above inherit their state
-- from that list, so not one of them needs a per-target condition.  `xmake` on
-- a reduced configuration therefore builds what is left instead of failing on a
-- library that was never produced (xmake drops a disabled target from the job
-- graph: modules/private/action/build/target.lua checks target:is_enabled()
-- before it adds any file or link job).
--
-- The third-party targets under src/ext are dependency *inputs* of kimix-core /
-- kimix-llm and never depend on a kimix target, so they stay available for
-- every combination.
--
-- This lives here rather than in xmake.lua because the maps have to be
-- file-scope locals: a scope script (this rule's on_load) runs in a forked
-- sandbox environment that only sees its upvalues and the xmake builtins, not
-- the globals of another xmake.lua file.
--
-- How the rule reaches the targets: _config_project() applies the rules of
-- _config_rules (see SECTION 4) and every kimix target plus most src/ext
-- targets go through it; runtime_py, which lists its rules by hand, names
-- kimix_feature_gate next to kimix_basic_settings in its add_rules() call.
-- The src/ext targets that skip the gate (kimix-mbedtls) are dependency
-- inputs, never gated, so they do not need it.
--

-- feature name -> the option that switches it
local kimix_feature_option = {
    llm     = "kimix_enable_llm",
    cli     = "kimix_enable_cli",
    runtime = "kimix_enable_runtime",
    api     = "kimix_enable_api",
}

-- feature name -> the features it needs on top of its own option; mirrors the
-- add_deps() graph of the gated targets (kimix-cli -> kimix-llm,
-- runtime_py -> kimix-llm).  `api` needs nothing: kimix_api links only
-- kimix-core, which is always built.
local kimix_feature_requires = {
    llm     = {},
    cli     = {"llm"},
    runtime = {"llm"},
    api     = {},
}

-- target name -> the feature it provides.  Targets absent from this map
-- (kimix-core and the src/ext libraries) are built for every combination.
-- kimix_api is normally switched off by skipping src/api/xmake.lua altogether
-- (a file-level skip, see xmake.lua); listing it here as well makes the gate
-- drop any future dependent target too, so the option never leaves a dangling
-- add_deps("kimix_api") behind.
local kimix_target_feature = {
    ["kimix-llm"]  = "llm",
    ["kimix-cli"]  = "cli",
    ["kimix_cli"]  = "cli",
    ["runtime_py"] = "runtime",
    ["kimix_api"]  = "api",
}

-- Is a feature enabled?  Its own option and the options of every feature it
-- requires have to be on; the check recurses over kimix_feature_requires.
local function kimix_feature_enabled(name)
    local opt = name and kimix_feature_option[name]
    if not opt then
        return true
    end
    if not has_config(opt) then
        return false
    end
    for _, need in ipairs(table.wrap(kimix_feature_requires[name])) do
        if not kimix_feature_enabled(need) then
            return false
        end
    end
    return true
end

-- Should this target be built?  Skipped when the feature it provides is off,
-- or when a target it depends on provides a feature that is off.  Walking only
-- the direct dependencies is enough: kimix_feature_enabled() already expands
-- the requires map, so a test that links kimix-cli is covered for cli and llm.
local function kimix_target_gate(target)
    local own = kimix_target_feature[target:name()]
    if own and not kimix_feature_enabled(own) then
        return false
    end
    for _, dep in ipairs(table.wrap(target:get("deps"))) do
        local feature = kimix_target_feature[dep]
        if feature and not kimix_feature_enabled(feature) then
            return false
        end
    end
    return true
end

rule("kimix_feature_gate")
on_load(function(target)
    if not kimix_target_gate(target) then
        target:set("enabled", false)
    end
end)
rule_end()

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
    -- Gated off by kimix_feature_gate (SECTION 0): the target is not built, so
    -- none of the flags below apply either.
    if not target:is_enabled() then
        return
    end

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

      -- Linux: Position independent code for static libraries. Static
      -- archives are linked into the runtime_py shared module, whose objects
      -- must all be -fPIC (the linker refuses otherwise: "relocation
      -- R_X86_64_PC32 ... can not be used when making a shared object").
      if target:is_plat("linux") then
          if project_kind == "static" or project_kind == "object"
              or target:kind() == "static" or target:kind() == "object" then
              target:add("cxflags", "-fPIC", {force = true})
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

        -- ...but never let the compiler CONTRACT a*b+c into an FMA behind our
        -- back.  GCC/clang default to -ffp-contract=fast (and -ffp-contract=on
        -- also contracts inside one expression), so `jaro + prefix * scale *
        -- (1 - jaro)` or a BM25 `idf * tf + ...` chain rounds once instead of
        -- twice and lands 1 ulp away from the float64 the Python reference
        -- (sequential IEEE ops) produces.  Every float kernel here is
        -- bit-exact-tested against that reference -- the python/tests
        -- parity suites assert with `abs=0.0 `, and the C++ goldens were
        -- recorded from the uncontracted result -- so Linux built with FMA
        -- used to disagree with the same source on MSVC (which does not
        -- contract at /fp:precise).  Keeping -mfma (the instructions are still
        -- emitted for explicit std::fma and for non-contractable code) while
        -- turning contraction off keeps both platforms bit-identical.
        target:add("cxflags", "-ffp-contract=off", {
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

    -- Configure exception handling.
    --
    -- kimix_enable_exception defaults to false: the project is compiled
    -- WITHOUT C++ exceptions.  No `throw` / `try` / `catch` is allowed in
    -- kimix code; every failure is reported through a return value (a bool or
    -- an error-code enum plus a message out-parameter).
    --
    -- Exception: the pybind11 binding translation units must keep exceptions
    -- enabled, because pybind11 (src/ext/pybind11, third-party code we never
    -- modify) is built on them: PYBIND11_MODULE expands to a try/catch block
    -- and every pybind11_fail()/PYBIND11_RUNTIME_EXCEPTION() is a `throw`.
    -- Those targets are listed in the `kimix_exceptions_targets` option
    -- (comma separated; defaults to runtime_py,test_pybind11).
    local enable_exception = _get_or("enable_exception")
    local keep_exceptions = get_config("kimix_exceptions_targets")
    if type(keep_exceptions) == "table" then
        -- allow both `--kimix_exceptions_targets=a,b` (string) and a table
        for _, name in ipairs(keep_exceptions) do
            if name == target:name() then
                enable_exception = true
            end
        end
    elseif type(keep_exceptions) == "string" and #keep_exceptions > 0 then
        for name in keep_exceptions:gmatch("[^,%s]+") do
            if name == target:name() then
                enable_exception = true
            end
        end
    end
    if not empty_str(enable_exception) then
        if enable_exception then
            target:set("exceptions", "cxx")
            -- Belt and braces: the flag form is explicit so the intent is
            -- visible in the compile commands regardless of the tool mapping.
            target:add("cxflags", "/EHsc", {
                tools = {"clang_cl", "cl"}
            })
            target:add("cxflags", "-fexceptions", {
                tools = {"clang", "gcc"}
            })
        else
            target:set("exceptions", "no-cxx")
            -- Belt and braces: `throw`/`try`/`catch` must be a hard error, not
            -- a warning, on every toolchain (MSVC only warns about C4530).
            target:add("cxflags", "/EHs-c-", {
                tools = {"clang_cl", "cl"}
            })
            target:add("cxflags", "-fno-exceptions", {
                tools = {"clang", "gcc"}
            })
            if target:is_plat('windows') then
                target:add('defines', '_HAS_EXCEPTIONS=0')
            end
            -- Same idea for the vendored moodycamel queue
            -- (src/core/detail/concurrent_queue.h), which auto-detects
            -- exceptions from _CPPUNWIND/__EXCEPTIONS and honours this switch
            -- instead.  Private: targets that keep exceptions enabled (the
            -- pybind11 binding layer) must keep the queue's real try/catch.
            target:add('defines', 'KIMIX_NO_EXCEPTIONS')
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
      -- GCC/Clang map xmake's optimize "aggressive" to -Ofast, which implies
      -- -ffast-math: NaN/Inf detection (std::isfinite/isnan/signbit and
      -- !(x >= 0) guards) is then constant-folded away, silently breaking
      -- the IEEE-semantics guards the codebase relies on (observed on a
      -- Linux release build: json_repair accepted a 400-digit overflow, the
      -- agent tool number renderer stopped mapping inf to null, and the
      -- job_output/read_image NaN-Inf rejections never fired). MSVC's /O2
      -- has no fast-math, so Windows keeps "aggressive"; POSIX toolchains
      -- drop to "fastest" (-O3), matching the Windows semantics.
      local default_opt = "aggressive"
      if not is_plat("windows") and not is_plat("mingw") then
          default_opt = "fastest"
      end
      opt = _get_or("optimize", default_opt)
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

    -- Source and execution character set: UTF-8, always.
    --
    -- Without it MSVC reads a BOM-less UTF-8 source as the system ANSI codepage
    -- and re-encodes narrow string literals into that same codepage. On a GBK
    -- (ACP 936) host the 3-byte UTF-8 em dash E2 80 94 therefore decodes as one
    -- GBK character plus a dangling lead byte, and is written back as E2 80 3F -
    -- invalid UTF-8 in the compiled binary. Every embedded non-ASCII literal is
    -- affected: the prompt templates of src/agent/system_prompt.cpp (which are
    -- documented as byte-identical to the Python reference), the tool
    -- descriptions, the CLI help text. Those strings go into the request JSON,
    -- yyjson's writer refuses invalid UTF-8, and the whole turn died with
    -- "chat failed: failed to build request body".
    --
    -- GCC and Clang already assume UTF-8 source and execution charsets, so this
    -- only pins the MSVC-family toolchains to the same behaviour. clang-cl needs
    -- the separate switches: it maps /utf-8 incompletely (see clang PR for
    -- /source-charset), so both halves are given explicitly.
    target:add("cxflags", "/utf-8", {
        tools = "cl",
        public = true
    })
    target:add("cxflags", "/source-charset:utf-8", "/execution-charset:utf-8", {
        tools = "clang_cl",
        public = true
    })

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

    -- RTTI (Run-Time Type Information) configuration.
    --
    -- kimix_rtti defaults to false: no `dynamic_cast`, no `typeid`, and no
    -- "read the type id out of the vtable pointer" in kimix code.  Runtime
    -- type dispatch uses a manual tag instead (a virtual tag getter or an
    -- explicit tag member) plus static_cast.
    local use_rtti = _get_or("rtti", false)
    -- pybind11 3.x (src/ext/pybind11, third-party code we never modify) uses
    -- typeid() unconditionally in its cast machinery (pytypes.h, detail/
    -- descr.h), so the pybind11 binding targets must keep RTTI as well as
    -- exceptions even when the project disables both.  Those are exactly the
    -- kimix_exceptions_targets (runtime_py, test_pybind11).  MSVC compiles
    -- typeid under /GR- so this only bites GCC/Clang, but apply it everywhere
    -- for consistency.
    if not use_rtti then
        local keep_rtti = get_config("kimix_exceptions_targets")
        local names = {}
        if type(keep_rtti) == "table" then
            names = keep_rtti
        elseif type(keep_rtti) == "string" and #keep_rtti > 0 then
            for name in keep_rtti:gmatch("[^,%s]+") do
                table.insert(names, name)
            end
        end
        for _, name in ipairs(names) do
            if name == target:name() then
                use_rtti = true
                break
            end
        end
    end
    if not empty_str(use_rtti) then
        if use_rtti then
            -- Enable RTTI
            target:add("cxflags", "/GR", {
                tools = {"clang_cl", "cl"}
            })
            target:add("cxflags", "-frtti", {
                tools = {"clang", "gcc"}
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

    -- Exec the real platform output (e.g. `kimix_cli.exe` on Windows).  The
    -- extension-less `targetdir/name` form used to be shadowed by foreign
    -- artifacts that share the directory -- a WSL/Linux build leaves a bare
    -- `kimix_cli` ELF next to `kimix_cli.exe`, and Windows then refused to run
    -- it with "%1 is not a valid Win32 application".  Prefer targetfile()
    -- (always carries the platform extension); fall back to name-based lookup
    -- only for custom `name` overrides or when the target file is missing.
    local program = target:targetfile()
    if not program or program == "" or not os.isfile(program) then
        program = path.join(tar_dir, name)
        if is_plat("windows") and not os.isfile(program) and os.isfile(program .. ".exe") then
            program = program .. ".exe"
        end
    end

    os.execv(program, arguments, {
        curdir = tar_dir
    })
end)
rule_end()

-- ============================================================================
-- SECTION 4: Global Configuration Functions
-- ============================================================================

-- Initialize default config rules.  kimix_feature_gate (SECTION 0) decides
-- whether a target is built at all; kimix_basic_settings then returns early for
-- a target it disabled.  The order does not carry any correctness: a rule
-- setting flags on a target that is skipped anyway is simply wasted work.
if _config_rules == nil then
    _config_rules = {"kimix_feature_gate", "kimix_basic_settings"}
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

-- ============================================================================
-- SECTION 5: Python interpreter probe (runtime_py)
-- ============================================================================
-- runtime_py is a pybind11 extension module: it compiles against ONE CPython
-- installation's headers and links that installation's import library.  Which
-- installation that is belongs to the host, so nothing about it is written into
-- a build file -- the `kimix_python` option (xmake.lua) selects it, either per
-- invocation
--
--     xmake f --kimix_python=C:/Python315/python.exe
--
-- or per machine in the gitignored local options.lua
--
--     kimix_options = { kimix_python = "C:/Python315" }
--
-- and while it is empty the rule uses the host's DEFAULT interpreter: `python`
-- on Windows, `python3` then `python` elsewhere, all resolved through PATH
-- first and then through the standard install prefixes (/usr/bin,
-- /usr/local/bin) so a bare environment still resolves.
--
-- Why this is a rule and not plain code in runtime_py's on_load: only a script
-- field may spawn a process.  The project scope an xmake.lua file is evaluated
-- in exposes os.isdir/os.getenv/os.files but neither io nor os.execv/iorunv
-- (verified), and a rule's script field is the one place that runs with the
-- full sandbox AND can hand the result to the target.  Rule on_load runs
-- BEFORE the target's own on_load (core/project/target.lua: _load() calls
-- _load_rules() and then the target script), so runtime_py's on_load only
-- reads the values published here.  The probe result is cached in the
-- file-scope table below, so an interpreter is spawned once per xmake
-- invocation however many targets carry the rule.
--
-- Published target values ("" when the probe found nothing):
--   kimix_python          interpreter that answered (`python` or an abs path)
--   kimix_python_version  sysconfig VERSION -- "315" on Windows, "3.15" else
--   kimix_python_include  directory holding Python.h
--   kimix_python_libdir   directory holding the import library / shared object
--   kimix_python_linkname name for links(): "python315", "python3.15", ...
--
-- Everything below is a file-scope local because a rule script runs in a
-- forked sandbox that sees its upvalues, not the globals of this file.
-- probe result per configured value; see the note in the rule about the several
-- project loads of a single xmake invocation
local _kimix_python_probe_cache = {}

-- One interpreter round trip: all five facts the target needs, one per line, in
-- the order the parser below expects.
--   VERSION    dotless on Windows ("315"), dotted elsewhere ("3.15")
--   include    directory holding Python.h
--   libdir     directory that REALLY holds the import library / shared object
--   LDLIBRARY  the library file name the interpreter reports
--   ABIFLAGS   free-threaded / debug suffix ("t", "d"), shared by DLL and .lib
--
-- The library directory is resolved inside the interpreter on purpose: a
-- relocatable CPython build (the standalone linux tarballs, a `make altinstall`
-- under /usr/local, a venv base) reports its BUILD-time LIBDIR, e.g.
-- /install/lib, which does not exist on the machine that installed it.  So the
-- script tries LIBDIR, LIBPL, <prefix>/lib, <prefix>/libs, <bindir>/../lib and
-- the same next to the resolved executable, and prints the first one that
-- actually contains the library -- "" when none does (the build then links
-- without it and fails loudly if the symbols are really needed).
local _kimix_python_probe_code = [[
import os
import sys
import sysconfig


def cv(name):
    return sysconfig.get_config_var(name) or ""


version = cv("VERSION")
abiflags = cv("ABIFLAGS")
include = sysconfig.get_paths().get("include") or ""
ldlib = cv("LDLIBRARY")
if os.name == "nt":
    libfile = "python" + version.replace(".", "") + abiflags + ".lib"
else:
    libfile = ldlib or ("libpython" + version + ".so")

candidates = []
for key in ("LIBDIR", "LIBPL"):
    value = cv(key)
    if value:
        candidates.append(value)
for key in ("prefix", "exec_prefix", "base", "platbase", "installed_base"):
    value = cv(key)
    if value:
        candidates.append(os.path.join(value, "lib"))
        candidates.append(os.path.join(value, "libs"))
exe_root = os.path.dirname(os.path.dirname(os.path.realpath(sys.executable)))
candidates.append(os.path.join(exe_root, "lib"))
candidates.append(os.path.join(exe_root, "libs"))

libdir = ""
for candidate in candidates:
    if not candidate or not os.path.isdir(candidate):
        continue
    if os.path.exists(os.path.join(candidate, libfile)) or any(
        name.startswith(libfile) for name in os.listdir(candidate)
    ):
        libdir = candidate
        break

print(version)
print(include)
print(libdir)
print(ldlib)
print(abiflags)
]]

rule("kimix_python_probe")
    on_load(function(target)
        -- A disabled target (kimix_enable_runtime or kimix_enable_llm off) must
        -- not spawn an interpreter: a host without Python still configures a
        -- build that does not contain the module.
        if not target:is_enabled() then
            return
        end

        -- Query one interpreter.  A missing or broken interpreter is a normal
        -- state (the user may have configured an installation this host cannot
        -- reach), so iorunv's raise is turned into a nil here.
        local function query(exe)
            local out
            try {
                function()
                    out = os.iorunv(exe, {"-c", _kimix_python_probe_code})
                end,
                catch {
                    function(errors)
                        out = nil
                    end
                }
            }
            if type(out) ~= "string" or out == "" then
                return nil
            end
            local lines = {}
            for line in (out .. "\n"):gmatch("([^\n]*)\n") do
                lines[#lines + 1] = line:gsub("\r$", ""):gsub("^%s+", ""):gsub("%s+$", "")
            end
            if #lines < 4 then
                return nil
            end
            return {
                version = lines[1] or "",
                include = lines[2] or "",
                libdir = lines[3] or "",
                ldlib = lines[4] or "",
                abiflags = lines[5] or "",
            }
        end

        -- Read the configuration before the cache, and key the cache by it: one
        -- xmake invocation loads the project more than once (a configure pass
        -- runs before the platform is resolved and the config is committed), and
        -- a probe cached under the wrong key would keep the earlier, still
        -- unconfigured answer for the rest of the run (measured: caching on a
        -- single slot made a configured interpreter look unset).
        local configured = get_config("kimix_python")
        if type(configured) ~= "string" then
            configured = ""
        end
        configured = configured:gsub("^%s+", ""):gsub("%s+$", "")
        local probe = _kimix_python_probe_cache[configured]
        if not probe then
            -- Candidate order: the configured interpreter first (option or local
            -- options.lua), then the host's DEFAULT python.
            local candidates = {}
            if configured ~= "" then
                -- A directory names the installation.  Windows keeps the
                -- interpreter at the root of the install dir (and beside it the
                -- DLL/import lib); a Unix prefix keeps it in bin/ and may name it
                -- python3, python or python3.15 -- take whatever is there, newest
                -- name last, and never guess silently.
                if os.isdir(configured) then
                    local dirs = is_plat("windows") and {configured}
                        or {path.join(configured, "bin"), configured}
                    local names = is_plat("windows") and {"python.exe"} or {"python3", "python"}
                    for _, dir in ipairs(dirs) do
                        for _, name in ipairs(names) do
                            local exe = path.join(dir, name)
                            if os.isfile(exe) then
                                candidates[#candidates + 1] = exe
                            end
                        end
                        if not is_plat("windows") then
                            -- a prefix installed beside its minor version
                            -- (bin/python3.15, no bin/python3 symlink)
                            for _, exe in ipairs(os.files(path.join(dir, "python3.*"))) do
                                if not exe:find("%-config$") then
                                    candidates[#candidates + 1] = exe
                                end
                            end
                        end
                    end
                    if #candidates == 0 then
                        -- nothing found: keep one candidate so the "cannot be
                        -- queried" note names the path the user configured
                        candidates[1] = path.join(dirs[1], names[1])
                    end
                else
                    candidates[#candidates + 1] = configured
                end
            end
            -- The default: whatever interpreter the host provides.  The bare
            -- names come first, so it is literally `python` (Windows) / `python3`
            -- resolved through PATH -- the interpreter a plain `python3 -c ...` in
            -- a shell would use.  The absolute paths after them are the standard
            -- install prefixes, so a sandboxed/bare environment whose PATH does
            -- not contain /usr/bin still resolves; they are only probed when the
            -- file exists.  Windows knows one name (its installer ships
            -- python.exe and no `python3`); on Linux `python` itself is frequently
            -- absent and `python3` is the contract.
            local defaults
            if is_plat("windows") then
                defaults = {"python"}
            else
                defaults = {"python3", "python",
                            "/usr/bin/python3", "/usr/local/bin/python3", "/usr/bin/python"}
            end
            for _, name in ipairs(defaults) do
                if name:find("[/\\]") and not os.isfile(name) then
                    -- absent standard location: skip it instead of spawning
                else
                    candidates[#candidates + 1] = name
                end
            end

            local exe, info = nil, nil
            local configured_failed = false
            for index, candidate in ipairs(candidates) do
                info = query(candidate)
                if info then
                    exe = candidate
                    break
                end
                if index == 1 and configured ~= "" then
                    configured_failed = true
                end
            end
            if configured_failed then
                -- Not fatal on purpose: a configure run that cannot reach the
                -- configured installation (a Linux leg reading a Windows path out
                -- of options.lua, a pruned install, a typo) still has to work,
                -- and a module built without Python.h fails loudly at compile
                -- time.  The note is what makes the fallback visible.
                print("note: kimix_python=" .. configured .. " cannot be queried on this host;"
                    .. (exe and (" using the host's default interpreter (" .. exe .. ")") or ""))
            end
            if not exe and configured == "" then
                print("note: no Python interpreter found for runtime_py (kimix_python is empty and"
                    .. " neither `" .. table.concat(defaults, "`, `") .. "` answered);"
                    .. " building without Python headers/library")
            elseif not exe then
                print("note: no Python interpreter found for runtime_py; building without"
                    .. " Python headers/library")
            end

            local linkname = ""
            if exe then
                if is_plat("windows") then
                    -- <prefix>\libs\python315.lib -> links "python315"; the
                    -- version comes from sysconfig, never from a literal here.
                    if info.version ~= "" then
                        linkname = "python" .. info.version:gsub("%.", "") .. info.abiflags
                    end
                else
                    -- libpython3.15.so -> links "python3.15" (the -l name)
                    linkname = info.ldlib:gsub("^lib", ""):gsub("%.so.*$", "")
                end
            end
            probe = {
                exe = exe or "",
                version = (info and info.version) or "",
                include = (info and info.include) or "",
                libdir = (info and info.libdir) or "",
                linkname = linkname,
            }
            _kimix_python_probe_cache[configured] = probe
        end

        target:values_set("kimix_python", probe.exe)
        target:values_set("kimix_python_version", probe.version)
        target:values_set("kimix_python_include", probe.include)
        target:values_set("kimix_python_libdir", probe.libdir)
        target:values_set("kimix_python_linkname", probe.linkname)
    end)
rule_end()
