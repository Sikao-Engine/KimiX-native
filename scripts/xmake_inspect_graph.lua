--[[
    xmake_inspect_graph.lua - dump the EVALUATED xmake graph of one configuration.

    Run it as
        xmake l scripts/xmake_inspect_graph.lua
    from the project root, AFTER a configuration (xmake f ...). It loads the
    recorded configuration, evaluates the project description exactly as a build
    would, and prints one JSON object between the two markers below. Nothing is
    built and nothing is written: this is the read-only view the dual-configuration
    gate (scripts/native_dual_config_gate.py) asserts against.

    Why a Lua script instead of parsing the xmake files: the gate has to reason
    about what the configuration REALLY decided -- kimix-core's dependency list,
    which defines reach a dependent target, which targets exist and which files
    they compile -- and the difference between the standalone and the external leg
    is exactly the part text parsing gets wrong (has_config branches, on_load
    additions, inherited public defines).

    Shape of the emitted object:
        {
          "project_dir": "<abs>",
          "config": { "<key>": <value>, ... },        -- the evaluated configuration
          "targets": [
            { "name": "kimix-core", "kind": "shared", "enabled": true,
              "deps": ["lc-core"],
              "defines_self": ["KIMIX_EXTERNAL_THIRDPARTY=1", ...],
              "defines_from": [...],                  -- self + DEPS' public + options
              "files": ["src/core/kimix_core.cpp", ...],  -- expanded, project-relative
              "tests": ["default"],
              "targetdir": "<abs>",
              "targetfile": "<abs path of the artifact, '' for phony/headeronly>" },
            ...
          ]
        }

    `defines_from` is what makes the "KIMIX_EXTERNAL_THIRDPARTY is PUBLIC" check
    possible: a private define stops at the target that declared it, a public one
    shows up in a dependent target's get_from(). The gate asserts the former
    absence and the latter presence on a TEST target.
--]]

import("core.base.json")
import("core.project.config")
import("core.project.project")

config.load()

-- xmake stores most target fields as a list, but a single value sometimes comes
-- back unwrapped (get("tests") is "default", not {"default"}), and get_from()
-- returns nested tables. Reduce all of that to a flat, sorted array of strings.
local function flat_strings(value)
    local out = {}
    if type(value) == "string" then
        out[1] = value
    elseif type(value) == "number" then
        out[1] = tostring(value)
    elseif type(value) == "table" then
        local seen = {}
        local function walk(v)
            if type(v) == "string" then
                if not seen[v] then
                    seen[v] = true
                    out[#out + 1] = v
                end
            elseif type(v) == "table" then
                -- pairs, not ipairs: a dict-shaped entry (tests = {default = {...}})
                -- has string keys and no array part.
                for k, x in pairs(v) do
                    if type(k) == "string" and type(x) ~= "table" then
                        -- dict of scalars: plan-style maps name -> bool/number.
                        local text = k .. "=" .. tostring(x)
                        if not seen[text] then
                            seen[text] = true
                            out[#out + 1] = text
                        end
                    else
                        walk(x)
                    end
                end
            end
        end
        walk(value)
    end
    table.sort(out)
    return out
end

-- Test entries registered on a target (add_tests), as plain names.
local function target_tests(target)
    local raw = target:get("tests")
    local out = {}
    if type(raw) == "string" then
        out[1] = raw
    elseif type(raw) == "table" then
        for k, v in pairs(raw) do
            if type(k) == "string" then
                out[#out + 1] = k
            elseif type(v) == "string" then
                out[#out + 1] = v
            end
        end
    end
    table.sort(out)
    return out
end

-- Source files, EXPANDED (target:sourcefiles()) and normalised to forward slashes
-- so the gate can compare them on any platform.
local function target_files(target)
    local out = {}
    local files = target:sourcefiles()
    if type(files) == "table" then
        for _, filepath in ipairs(files) do
            out[#out + 1] = (path.translate(filepath, "/"))
        end
    end
    table.sort(out)
    return out
end

-- The configuration keys the gate asserts on. config.get() only answers one key
-- at a time (a bare config.get() is empty), so they are listed here explicitly;
-- every one of them is recorded in .xmake/<plat>/<arch>/xmake.conf by `xmake f`.
local KIMIX_CONFIG_KEYS = {
    "mode", "plat", "arch", "host", "builddir",
    "kimix_bin_dir", "_kimix_bin_dir",
    "kimix_external_thirdparty",
    "kimix_enable_tests", "kimix_slow_tests", "kimix_enable_api",
    "kimix_enable_llm", "kimix_enable_cli", "kimix_enable_runtime",
    "kimix_enable_unity_build", "kimix_enable_pch", "kimix_memory_strict",
    "kimix_mi_track_asan", "kimix_use_lto", "kimix_optimize", "kimix_rtti",
    "kimix_cxx_standard", "kimix_win_runtime"
}

local function config_table()
    local out = {}
    for _, key in ipairs(KIMIX_CONFIG_KEYS) do
        local value = config.get(key)
        if type(value) == "string" or type(value) == "number" or type(value) == "boolean" then
            out[key] = value
        end
    end
    return out
end

local targets = {}
for _, target in pairs(project.targets()) do
    targets[#targets + 1] = {
        name = target:name(),
        kind = target:kind(),
        enabled = target:is_enabled(),
        deps = flat_strings(target:get("deps")),
        defines_self = flat_strings(target:get("defines")),
        defines_from = flat_strings(target:get_from("defines", "*")),
        files = target_files(target),
        tests = target_tests(target),
        targetdir = (path.translate(target:targetdir(), "/")),
        -- The artifact a "build ok" must have produced (empty for headeronly/phony
        -- targets). The gate's --reuse / --skip-build artifact check needs it: it
        -- is how "nothing to compile because the tree is up to date" is told apart
        -- from "nothing to reuse".
        targetfile = (path.translate(target:targetfile() or "", "/"))
    }
end
table.sort(targets, function(a, b) return a.name < b.name end)

local payload = {
    project_dir = path.translate(os.projectdir(), "/"),
    config = config_table(),
    targets = targets
}

print("--8<--KIMIX-GRAPH-BEGIN")
print(json.encode(payload))
print("--8<--KIMIX-GRAPH-END")