--[[
    xmake_luisa.lua - the external third-party leg (option kimix_external_thirdparty)
                       and its embedding closure (option kimix_luisa_embedding)

    What this file is for
    ---------------------
    kimix-native normally builds the vendored src/ext libraries and declares them
    on kimix-core (mimalloc, xxHash, yyjson, marl, pdqsort -- src/ext/xmake.lua).
    With `xmake f --kimix_external_thirdparty=true` those same facilities come
    from an external third-party tree instead: ../link.lua names it
    (kimix_external_config.external, currently LuisaCompute) and this file
    declares the MINIMAL subgraph of that tree which lc-core needs -- EASTL with
    its bundled mimalloc, spdlog, the BTree header target, lc-yyjson, the
    lc-check-winsdk compatibility target, and src/core/xmake.lua itself.

    TWO SWITCHES, not one.  The graph above IS the external leg; it is what the
    dual-configuration gate asserts (scripts/native_dual_config_gate.py: kimix-core
    has exactly one third-party dep, and no LC backend, test, example or tutorial
    target exists).  LuisaCompute's own embedding example needs far more -- the
    runtime/DSL/tile targets, the DX/VK backend subgraph with LMDB, volk, glslang
    and SPIRV-Tools, and examples/embedding's six targets -- so that closure sits
    behind a SECOND option, kimix_luisa_embedding (default false), instead of
    being part of every external configuration:

        kimix_external_thirdparty=false                          -> standalone
        kimix_external_thirdparty=true,  embedding=false         -> minimal graph
        kimix_external_thirdparty=true,  embedding=true          -> minimal +
                                                                    embedding closure
        kimix_external_thirdparty=false, embedding=true          -> standalone
            (the second option has no effect at all: there is no external tree
             to embed from, so nothing here is read or probed)

    LuisaCompute's root xmake.lua, monolithic src/ext/xmake.lua, tests, other
    examples and tutorials stay out of BOTH configurations: in particular no CUDA,
    GUI, Python, fallback, TVM or test dependency is pulled into this project.

    Contract
    --------
    * kimix_luisa_include() is called from xmake.lua, once, INSIDE the
      _kimix_check_env pass, AFTER set_targetdir() and BEFORE includes("src"):
      kimix-core's add_deps("lc-core") has to find the target already declared,
      and every target declared here inherits the project targetdir
      (bin/<mode>) from that root set_targetdir().
    * kimix_luisa_include_embedding() is called from src/xmake.lua, after every
      kimix target has been declared; it declares examples/embedding and nothing
      else.  It requires BOTH options (see kimix_luisa_embedding_enabled).
    * Option kimix_external_thirdparty off (the default) means ZERO effect:
      both entry points return before they probe the filesystem, so a standalone
      build is exactly the build it was.  The vendored src/ext targets are
      declared as before.  With kimix_luisa_embedding=true and the external leg
      off the behaviour is identical -- the embedding switch alone does nothing.
    * kimix_luisa_embedding off (the default) means the external leg is exactly
      the minimal graph above: no embedding-only probe runs (no runtime/DSL/tile
      /backends/lmdb/volk/glslang/SPIRV/examples path is even stat()ed) and no
      embedding-only rule, target or include is declared.  The lc_* options those
      LC files read are DECLARED (an option registration, see
      kimix_luisa_embedding_options) but nothing reads them in this graph, and
      their defaults are only published into the configuration by
      kimix_luisa_publish_embedding_option_defaults, which the closure calls.
    * External leg on means FAIL CLOSED: a missing ../link.lua, a missing/malformed
      kimix_external_config (no `external`), a missing external root, or any
      missing LC input aborts the configuration with a non-zero exit and names
      the path that is missing (see kimix_luisa_fail_closed).  Nothing of the
      external tree is included in that case, and there is no silent fallback
      to the vendored libraries -- a configuration that asks for the external
      leg and cannot get it must not build a second, different library graph
      under the same name.  The embedding inputs are probed the same way, and
      only when kimix_luisa_embedding is on; their failure names that option.
    * The external targets go through kimix's _config_project(), so they are
      compiled with this project's flags (cxx20, no RTTI, no exceptions,
      warnings off, the same MSVC runtime) instead of LC's lc_basic_settings,
      which is not loaded here.

    Why re-declaring three targets instead of including LC files
    -----------------------------------------------------------
    lc-yyjson, btree and lc-check-winsdk are declared inline in LC's monolithic
    src/ext/xmake.lua and scripts/xmake_func.lua, which this bridge must not
    include.  The declarations below are transcriptions of those three blocks
    with every path anchored at os.scriptdir() (this file), because LC's own
    relative paths would otherwise resolve against scripts/.
--]]

-- The kimix-native root, anchored at THIS file: every path below derives from
-- it, never from the process cwd.  xmake runs description-scope scripts with
-- the cwd set to the script's own directory, so a bare "../link.lua" would
-- resolve differently here, in xmake.lua and inside an on_load.
local kimix_luisa_root = path.normalize(path.absolute(path.join(os.scriptdir(), "..")))
local kimix_luisa_thirdparty = path.normalize(path.join(kimix_luisa_root, ".."))

-- Resolved once per xmake run by kimix_luisa_resolve(); nil until then.
local kimix_luisa_paths = nil

-- Fail-closed flag: set by the first failure, so a second resolve() call in the
-- same description run does not declare the failure target twice.
local kimix_luisa_failed = false

-- LuisaCompute target files read these options directly, so they have to exist
-- before the file that reads them is included.  They come in two groups below:
-- the CORE-LEVEL ones (read by lc-core and its dependencies, i.e. both external
-- configurations) and the EMBEDDING-ONLY ones (read only by the closure behind
-- option kimix_luisa_embedding, which is off by default).
local function kimix_luisa_option(name, default)
    option(name)
        set_default(default)
        set_showmenu(false)
    option_end()
end

-- The CORE-LEVEL options: read by the target files BOTH external configurations
-- include -- LC's own src/core/xmake.lua (lc-core) and the EASTL / spdlog /
-- mimalloc files below.  They are declared unconditionally because lc-core reads
-- them whether or not the embedding closure is built; they are never a probe.
kimix_luisa_option("lc_enable_mimalloc", true)
kimix_luisa_option("lc_enable_custom_malloc", false)
kimix_luisa_option("lc_external_marl", false)
kimix_luisa_option("lc_use_system_stl", false)
kimix_luisa_option("lc_disable_win_message_box", true)
kimix_luisa_option("lc_check_unity_build", false)
kimix_luisa_option("lc_spdlog_use_xrepo", false)
-- lc_enable_dsl is a core-level switch too: src/core/xmake.lua reads it and
-- publishes LUISA_ENABLE_DSL on lc-core itself, so it is not part of the
-- embedding-only set below even though the DSL target is.
kimix_luisa_option("lc_enable_dsl", true)

-- The EMBEDDING-ONLY options: read exclusively by the runtime / DSL / tile /
-- backends / LMDB / volk / glslang / SPIRV target files that only exist in the
-- embedding closure.  One table, declared here at FILE LOAD, and NOT inside
-- kimix_luisa_include(): an option() call made from a function invoked during the
-- description pass is not registered by xmake's option collection, so the
-- declaration silently never reaches the configuration (measured: the twenty
-- lc_* keys were absent from .xmake/<plat>/<arch>/xmake.conf and every
-- has_config() in the closure answered false, which dropped the DX/VK halves of
-- the very closure this option exists for).
--
-- Declaration is not a probe: no path is touched here, the defaults are the
-- values LC's own files read, and nothing in the minimal graph or a standalone
-- build consults them (the closure is the only reader, and it needs BOTH
-- kimix_external_thirdparty and kimix_luisa_embedding).
--
-- Defaults deliberately exclude every facility the embedding example can do
-- without (CUDA, Metal, the fallback backend, TVM/TIR-X, interop, SDK
-- overrides).
local kimix_luisa_embedding_options = {
    {"lc_yyjson_use_xrepo", false},
    {"lc_lmdb_use_xrepo", false},
    {"lc_enable_xir", true},
    {"lc_safe_mode", false},
    {"lc_tile_tirx_bridge", false},
    {"lc_dx_backend", is_plat("windows")},
    {"lc_vk_backend", true},
    {"lc_cuda_backend", false},
    {"lc_metal_backend", false},
    {"lc_fallback_backend", false},
    {"lc_dx_cuda_interop", false},
    {"lc_vk_cuda_interop", false},
    {"lc_enable_win_pix", false},
    {"lc_enable_dxagsdk", false},
    {"lc_vk_backend_use_xir_spirv", false},
    {"lc_vk_backend_use_ast_llvm_spirv", false},
    {"lc_vk_backend_enable_dxc_compatibility", true},
    {"lc_vk_bindless_heap_capacity", 262144},
    {"lc_vk_min_api_version", "auto"},
    {"lc_sdk_dir", false},
}

for _, entry in ipairs(kimix_luisa_embedding_options) do
    kimix_luisa_option(entry[1], entry[2])
end

-- xmake persists an option's DEFAULT into xmake.conf only at the END of
-- `xmake f`, so in the pass that is configuring a tree for the first time
-- has_config() still answers "unset" even for a declared option -- and LC's own
-- target files read these switches themselves (src/core/xmake.lua,
-- src/backends/common/xmake.lua, src/backends/vk/xmake.lua).  Publishing the
-- declared defaults into the in-memory configuration before the closure is
-- included makes the graph the same on the first configuration and the tenth;
-- a value the user actually set is never overwritten.
local function kimix_luisa_publish_embedding_option_defaults()
    for _, entry in ipairs(kimix_luisa_embedding_options) do
        if get_config(entry[1]) == nil then
            set_config(entry[1], entry[2])
        end
    end
end

-- The ONE predicate the whole embedding closure hangs off, so both entry points
-- and the probe split below cannot drift apart: the embedding needs an external
-- tree to embed from, so kimix_luisa_embedding alone does nothing.
function kimix_luisa_embedding_enabled()
    return has_config("kimix_external_thirdparty") and has_config("kimix_luisa_embedding")
end

-- Imported LC target files call lc_set_pcxxheader(); map it to kimix-native's
-- PCH switch rather than loading LC's root option/rule file.
function lc_set_pcxxheader(...)
    if get_config("kimix_enable_pch") then
        set_pcxxheader(...)
    end
end

-- Fail-closed helper: every complaint below goes through here, so a broken
-- external leg always names the option that asked for the missing input and the
-- offending path.  `option_name` defaults to the external leg; the embedding
-- inputs pass "kimix_luisa_embedding" so a configuration that only asked for the
-- minimal graph can never be blamed for a backend tree it never wanted.
--
-- WHY A TARGET AND NOT raise(): this file runs at DESCRIPTION scope (the
-- _kimix_check_env pass), and the description-scope sandbox exports neither
-- raise() nor assert()/error()/utils -- those exist in script scopes only.  So
-- the failure is declared as a target whose on_load raises: `xmake f` and
-- `xmake build` both abort with
--     error: <option>: <what>
-- and a non-zero exit, and NOTHING of the external tree is included -- which is
-- the point: no half-declared graph, and no silent fallback to the vendored
-- libraries.
function kimix_luisa_fail_closed(what, option_name)
    if kimix_luisa_failed then
        return
    end
    kimix_luisa_failed = true
    local message = (option_name or "kimix_external_thirdparty") .. ": " .. what
    target("kimix-external-thirdparty-fail-closed")
        set_kind("phony")
        on_load(function(target)
            raise(message)
        end)
    target_end()
end

-- The external third-party leg, resolved and validated:
--   {external, root, ext, core, eastl, eastl_mimalloc, spdlog, btree_include,
--    yyjson, core_file}
-- Loads ../link.lua (the one place the external tree is named) on first use and
-- returns nil -- after declaring the fail-closed target -- instead of a
-- half-filled table.
function kimix_luisa_resolve()
    if kimix_luisa_paths then
        return kimix_luisa_paths
    end

    -- 1. ../link.lua, relative to the kimix-native root, loaded as a real Lua
    --    file (its globals land in this scope) -- not parsed by hand.
    local link_file = path.join(kimix_luisa_thirdparty, "link.lua")
    if not os.isfile(link_file) then
        kimix_luisa_fail_closed("- ../link.lua not found: " .. link_file ..
            " (it must define kimix_external_config = { external = 'LuisaCompute' })")
        return nil
    end
    includes(link_file)

    local config = kimix_external_config
    if type(config) ~= "table" then
        kimix_luisa_fail_closed("- " .. link_file ..
            " did not define kimix_external_config { external = ... }")
        return nil
    end
    local external = config.external
    if type(external) ~= "string" or #external == 0 then
        kimix_luisa_fail_closed("- " .. link_file ..
            ": kimix_external_config.external is missing or empty")
        return nil
    end

    -- 2. The external root: ../<external>, next to kimix-native.
    local ext_root = path.normalize(path.join(kimix_luisa_thirdparty, external))
    if not os.isdir(ext_root) then
        kimix_luisa_fail_closed("- external root of '" .. external .. "' not found: " ..
            ext_root .. " (../link.lua names it, so its checkout is expected as a " ..
            "sibling of kimix-native)")
        return nil
    end

    -- 3. The LC inputs this bridge declares, one entry per included file plus
    --    the two things a build cannot invent: the BTree headers and lc-core's
    --    own sources.
    --
    --    TWO LISTS, because they belong to two different graphs:
    --      * kimix_luisa_core_probes      -- always probed in the external leg:
    --        exactly the files lc-core and its four dependencies are made of.
    --      * kimix_luisa_embedding_probes -- probed ONLY when
    --        kimix_luisa_embedding is on: the runtime/DSL/tile/backend/example
    --        closure.  A plain external leg must not stat() a single one of
    --        them -- an LC checkout with no backends, no examples and no SDK
    --        helper script is a perfectly valid MINIMAL external tree, and
    --        requiring them would turn "least privilege" into "build everything".
    local lc_ext = path.join(ext_root, "src/ext")
    local lc_core = path.join(ext_root, "src/core")
    local kimix_luisa_core_probes = {
        {"LC EASTL target",           path.join(lc_ext, "EASTL/xmake.lua")},
        {"LC EASTL mimalloc target",  path.join(lc_ext, "EASTL/packages/mimalloc/xmake.lua")},
        {"LC EASTL mimalloc sources", path.join(lc_ext, "EASTL/packages/mimalloc/src/static.c")},
        {"LC spdlog target",          path.join(lc_ext, "spdlog/xmake.lua")},
        {"LC spdlog sources",         path.join(lc_ext, "spdlog/src")},
        {"LC BTree headers",          path.join(lc_ext, "BTree/include/fc")},
        {"LC yyjson source",          path.join(lc_ext, "yyjson/src/yyjson.c")},
        {"LC core target",            path.join(lc_core, "xmake.lua")},
        {"LC core headers",           path.join(ext_root, "include/luisa/core")},
    }
    local kimix_luisa_embedding_probes = {
        {"LC VSTL target",            path.join(ext_root, "src/vstl/xmake.lua")},
        {"LC runtime target",         path.join(ext_root, "src/runtime/xmake.lua")},
        {"LC DSL target",             path.join(ext_root, "src/dsl/xmake.lua")},
        {"LC tile target",            path.join(ext_root, "src/tile/xmake.lua")},
        {"LC backend targets",        path.join(ext_root, "src/backends/xmake.lua")},
        {"LC LMDB target",            path.join(lc_ext, "liblmdb/xmake.lua")},
        {"LC Volk target",            path.join(lc_ext, "volk/xmake.lua")},
        {"LC glslang target",         path.join(lc_ext, "glslang/xmake.lua")},
        {"LC SPIRV-Tools target",     path.join(lc_ext, "SPIRV-Tools/xmake.lua")},
        {"LC SPIRV-Headers",          path.join(lc_ext, "spirv-headers/include")},
        {"LC SDK helper",             path.join(ext_root, "scripts/find_sdk.lua")},
        {"LC embedding targets",      path.join(ext_root, "examples/embedding/xmake.lua")},
    }
    -- Collect into `missing` and report once per owner, so the message names the
    -- option a reader has to look at.
    local function collect(probes, missing)
        for _, probe in ipairs(probes) do
            if not os.exists(probe[2]) then
                missing[#missing + 1] = probe[1] .. " (" .. probe[2] .. ")"
            end
        end
        return missing
    end
    local function complain(missing, option_name)
        if #missing == 0 then
            return
        end
        kimix_luisa_fail_closed("- " .. external .. " is missing " .. #missing ..
            " input(s): " .. table.concat(missing, "; "), option_name)
    end

    local missing_core = collect(kimix_luisa_core_probes, {})
    -- lc-core compiles src/core/**.cpp; an empty set would produce an empty
    -- luisa-core.dll instead of failing, so probe it explicitly.
    if #os.files(path.join(lc_core, "*.cpp")) == 0 then
        missing_core[#missing_core + 1] = "LC core sources (" .. path.join(lc_core, "*.cpp") .. ")"
    end
    if #missing_core > 0 then
        complain(missing_core)
        return nil
    end

    -- The embedding closure's inputs, ONLY when the embedding was asked for.
    if kimix_luisa_embedding_enabled() then
        local missing_embedding = collect(kimix_luisa_embedding_probes, {})
        if #missing_embedding > 0 then
            complain(missing_embedding, "kimix_luisa_embedding")
            return nil
        end
    end

    -- Every remaining field is a PURE path join -- nothing below touches the
    -- filesystem -- so resolving them costs nothing in the minimal graph, and the
    -- probe lists above are the only filesystem reads in this whole file.
    kimix_luisa_paths = {
        external = external,
        root = ext_root,
        ext = lc_ext,
        core = lc_core,
        eastl = path.join(lc_ext, "EASTL/xmake.lua"),
        eastl_mimalloc = path.join(lc_ext, "EASTL/packages/mimalloc/xmake.lua"),
        spdlog = path.join(lc_ext, "spdlog/xmake.lua"),
        btree_include = path.join(lc_ext, "BTree/include"),
        yyjson = path.join(lc_ext, "yyjson/src"),
        core_file = path.join(lc_core, "xmake.lua"),
        scripts = path.join(ext_root, "scripts"),
        vstl = path.join(ext_root, "src/vstl/xmake.lua"),
        runtime = path.join(ext_root, "src/runtime/xmake.lua"),
        dsl = path.join(ext_root, "src/dsl/xmake.lua"),
        tile = path.join(ext_root, "src/tile/xmake.lua"),
        backends = path.join(ext_root, "src/backends/xmake.lua"),
        lmdb = path.join(lc_ext, "liblmdb/xmake.lua"),
        volk = path.join(lc_ext, "volk/xmake.lua"),
        glslang = path.join(lc_ext, "glslang/xmake.lua"),
        spirv_tools = path.join(lc_ext, "SPIRV-Tools/xmake.lua"),
        spirv_tools_root = path.join(lc_ext, "SPIRV-Tools"),
        spirv_headers = path.join(lc_ext, "spirv-headers/include"),
        embedding = path.join(ext_root, "examples/embedding/xmake.lua"),
    }
    return kimix_luisa_paths
end

-- Declare the MINIMAL external subgraph lc-core needs.  Called once from
-- xmake.lua's _kimix_check_env pass; a no-op while the option is off.
--
-- It declares lc-core, its four dependencies (EASTL + bundled mimalloc, spdlog,
-- btree, lc-yyjson) and lc-check-winsdk, and nothing else -- that is the graph
-- scripts/native_dual_config_gate.py asserts as the whole of the external leg.
-- The embedding closure is opt-in on top of it: see step 8 at the bottom.
function kimix_luisa_include()
    if not has_config("kimix_external_thirdparty") then
        return
    end
    local p = kimix_luisa_resolve()
    if not p then
        return
    end
    print("note: external third-party leg: kimix-thirdparty -> " .. p.external ..
        " (" .. p.root .. ")")

    -- 1. mimalloc, EASTL's bundled copy (EASTL/packages/mimalloc).
    --    In this leg it is THE mimalloc: the vendored `mimalloc` target of
    --    src/ext/xmake.lua is not declared, so the same target name, the same
    --    MI_SHARED_LIB object target and the same one-heap rule apply -- the
    --    objects are archived into luisa-core.dll and imported from there.
    includes(p.eastl_mimalloc)

    -- 2. EASTL with its mimalloc allocator enabled.  These two globals are what
    --    LC's own src/ext/xmake.lua sets immediately before includes("EASTL");
    --    EASTL/xmake.lua reads them at parse time (EASTL_MIMALLOC_ENABLED).
    lc_eastl_enable_mimalloc = true
    lc_eastl_enable_custom_malloc = false
    includes(p.eastl)
    -- EASTL/xmake.lua guards its own mimalloc dep with
    -- `os.exists("packages/mimalloc")`, which resolves against the project cwd
    -- (the kimix-native root) and is therefore false here.  Re-attach the same
    -- dep from outside instead of editing LC: a second target block extends the
    -- target EASTL just declared.
    target("eastl")
        add_deps("mimalloc")
    target_end()

    -- 3. spdlog (lc-core's logging backend; its own file, its own target).
    includes(p.spdlog)

    -- 4. BTree -- header-only, declared inline in LC's src/ext/xmake.lua.
    target("btree")
        set_kind("headeronly")
        add_includedirs(p.btree_include, {
            public = true
        })
    target_end()

    -- 5. lc-yyjson -- declared inline in LC's src/ext/xmake.lua; transcribed
    --    with the source dir anchored at the external tree.
    target("lc-yyjson")
        _config_project({
            project_kind = "static"
        })
        on_load(function(target)
            target:add("files", path.join(p.yyjson, "yyjson.c"))
            target:add("includedirs", p.yyjson, {
                public = true
            })
            target:add("cxflags", "/utf-8", {
                tools = "cl"
            })
        end)
    target_end()

    -- 6. lc-check-winsdk -- LC declares it in scripts/xmake_func.lua (not
    --    included, see the header comment), yet src/core/xmake.lua hard-depends
    --    on it.  Same name, same check: Windows SDK 10.0.22000.0 or later,
    --    skipped on every other platform.
    target("lc-check-winsdk")
        set_kind("phony")
        add_rules("kimix_basic_settings")
        on_config(function(target)
            if not target:is_plat("windows") then
                return nil
            end
            local toolchain_settings = target:toolchain("msvc")
            if not toolchain_settings then
                toolchain_settings = target:toolchain("clang-cl")
            end
            if not toolchain_settings then
                toolchain_settings = target:toolchain("llvm")
            end
            if not toolchain_settings then
                return nil
            end
            local sdk_version = toolchain_settings:runenvs().WindowsSDKVersion
            if not sdk_version then
                return nil
            end
            import("core.base.semver")
            local ver = semver.match(sdk_version)
            local legal_sdk = false
            if ver then
                if ver:major() > 10 then
                    legal_sdk = true
                elseif ver:major() == 10 and ver:patch() >= 22000 then
                    legal_sdk = true
                end
            end
            if not legal_sdk then
                raise("Illegal windows SDK version, requires 10.0.22000.0 or later")
            end
        end)
    target_end()

    -- 7. lc-core itself: LC's own target file, unchanged.  It pulls eastl,
    --    btree, spdlog and lc-check-winsdk itself and compiles marl + its own
    --    src/core sources.
    includes(p.core_file)
    -- kimix-core's external leg declares lc-core as its ONE third-party dep, so
    -- everything kimix-core uses has to be reachable through lc-core's public
    -- interface.  yyjson is the one LC facility lc-core does not depend on yet
    -- (LC's backends add it), so carry it here: kimix-core -- and later
    -- kimix_api's ffi_yyjson -- then use <yyjson.h> solely through lc-core.
    target("lc-core")
        add_deps("lc-yyjson")
    target_end()

    -- 8. The embedding closure -- OPT-IN, option kimix_luisa_embedding only.
    --    Off (the default) not one line of it runs: the external leg is exactly
    --    the seven targets above, no embedding input is probed and no embedding
    --    rule or target is declared.
    if kimix_luisa_embedding_enabled() then
        kimix_luisa_include_embedding_closure(p)
    end
end

-- The LuisaCompute embedding closure: everything examples/embedding needs on top
-- of lc-core.  Reached ONLY through kimix_luisa_include() with BOTH
-- kimix_external_thirdparty and kimix_luisa_embedding on -- i.e. never in a
-- default external configuration.  Its inputs were already probed by
-- kimix_luisa_resolve() (kimix_luisa_embedding_probes), its lc_* options are
-- declared here, and kimix_luisa_include_embedding() below adds the example
-- targets themselves.
--
-- Do not include LC's src/xmake.lua or src/ext/xmake.lua here: those umbrella
-- files also declare tests, unrelated third-party libraries and optional
-- backends.  Each include below is a target file required by
-- examples/embedding/xmake.lua or by the DX/VK backend aggregate.
function kimix_luisa_include_embedding_closure(p)
    -- The lc_* options the files below read: publish the declared defaults for
    -- this description pass first (see kimix_luisa_publish_embedding_option_defaults).
    kimix_luisa_publish_embedding_option_defaults()

    -- The backend aggregate uses LC's SDK rule to stage DXC/DStorage next to
    -- the executables and backend DLLs.  Capture the resolved path as an
    -- upvalue: rule script scopes do not inherit description-scope globals.
    rule("lc_install_sdk")
        before_build(function(target)
            import("find_sdk", {rootdir = p.scripts})
            find_sdk.on_install_sdk(target, "lc_install_sdk")
        end)
    rule_end()

    includes(p.lmdb)
    includes(p.volk)

    if has_config("lc_vk_backend") then
        includes(p.glslang)
        target("lc-glslang")
            add_defines("ENABLE_HLSL", {public = true})
            add_files(path.join(p.root, "src/ext/glslang/glslang/HLSL/*.cpp"))
        target_end()

        target("spirv-headers")
            set_kind("headeronly")
            add_includedirs(p.spirv_headers,
                path.join(p.spirv_headers, "spirv/unified1"), {public = true})
        target_end()
        includes(p.spirv_tools)
        -- SPIRV-Tools' optional mimalloc override is not part of an LC build
        -- (and its header is unavailable with the default allocator setup).
        target("spirv-tools")
            remove_files(path.join(p.spirv_tools_root, "source/mimalloc.cpp"))
        target_end()
    end

    includes(p.vstl)
    includes(p.runtime)
    if has_config("lc_enable_dsl") then
        includes(p.dsl)
    end
    includes(p.tile)

    -- Reproduce LC's small backend aggregate without its unrelated shader
    -- compiler tool (which would add reproc even though no embedding target
    -- depends on that tool).
    includes(path.join(p.root, "src/backends/common/xmake.lua"))
    target("lc_backend_sdk")
        set_kind("phony")
        local libnames = {}
        if os.host() == "windows" and
           (has_config("lc_dx_backend") or has_config("lc_vk_backend")) then
            table.insert(libnames, "dx_sdk")
        end
        if os.host() == "linux" and os.arch() == "x86_64" and
           has_config("lc_vk_backend") then
            table.insert(libnames, "vk_sdk")
        end
        if #libnames > 0 then
            add_rules("lc_install_sdk", {libnames = libnames})
        end
    target_end()
    if has_config("lc_dx_backend") then
        includes(path.join(p.root, "src/backends/dx/xmake.lua"))
    end
    if has_config("lc_vk_backend") then
        includes(path.join(p.root, "src/backends/vk/xmake.lua"))
    end
    includes(path.join(p.root, "src/backends/validation/xmake.lua"))

    rule("lc-backend-deps")
        on_load(function(target)
            target:add("deps", "lc-validation-layer", {inherit = false, links = false})
            target:add("deps", "lc_backend_sdk")
            if has_config("lc_dx_backend") then
                target:add("deps", "lc-backend-dx", {inherit = false, links = false})
            end
            if has_config("lc_vk_backend") then
                target:add("deps", "lc-backend-vk", {inherit = false, links = false})
            end
        end)
    rule_end()
    target("lc-backends-dummy")
        set_kind("phony")
        add_rules("lc-backend-deps")
    target_end()
end

-- src/xmake.lua calls this after all kimix targets have been declared.  Keeping
-- the embedding include separate makes the requested LC example file explicit
-- at the kimix-native target declaration site while this bridge owns its
-- dependency closure and path resolution.
--
-- GATED BY BOTH OPTIONS: kimix_external_thirdparty (there has to be an external
-- tree) AND kimix_luisa_embedding (the example is not part of the minimal
-- external graph, and the dual-configuration gate forbids it there).  With
-- either one off this returns before resolving anything, so a standalone build
-- and a plain external build never touch an LC/embedding path here.
function kimix_luisa_include_embedding()
    if not kimix_luisa_embedding_enabled() then
        return
    end
    local p = kimix_luisa_resolve()
    if p then
        includes(p.embedding)
        -- The example file anchors its public includes at $(projectdir).  In
        -- this embedding that is kimix-native, so re-anchor the same paths at
        -- the resolved LC root (without editing the external target file).
        local example_dir = path.join(p.root, "examples/embedding")
        for _, name in ipairs({
            "lc-example-embedding",
            "example_embed_pack_model",
            "example_embed_retrieval_lib",
            "example_embed_retrieval",
            "example_embed_dump_golden",
            "example_embed_text",
        }) do
            target(name)
                add_includedirs(path.join(p.root, "examples"), example_dir,
                    path.join(p.root, "src/ext/yyjson/src"), {public = true})
            target_end()
        end
        target("example_embed_retrieval_lib")
            add_includedirs(path.join(example_dir, "api"), {public = true})
        target_end()
    end
end
