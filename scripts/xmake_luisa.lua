-- Minimal LuisaCompute bridge for kimix-native.
-- The external tree is the sibling checkout at thirdparty/LuisaCompute.

local lc_root = path.normalize(path.join(os.scriptdir(), "../../LuisaCompute"))
local lc_ext = path.join(lc_root, "src/ext")

local function lc_file(relative)
    return path.join(lc_root, relative)
end

local lc_options = {
    {"lc_enable_mimalloc", true},
    {"lc_enable_custom_malloc", false},
    {"lc_external_marl", false},
    {"lc_use_system_stl", false},
    {"lc_disable_win_message_box", true},
    {"lc_check_unity_build", false},
    {"lc_spdlog_use_xrepo", false},
    {"lc_enable_dsl", true},
    {"lc_yyjson_use_xrepo", false},
    {"lc_lmdb_use_xrepo", false},
    {"lc_enable_xir", true},
    {"lc_safe_mode", false},
    {"lc_tile_tirx_bridge", false},
    {"lc_dx_backend", is_plat("windows")},
    {"lc_vk_backend", true},
    {"lc_cuda_backend", false},
    {"lc_enable_tests", false},
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

for _, entry in ipairs(lc_options) do
    option(entry[1])
        set_default(entry[2])
        set_showmenu(false)
    option_end()
end

local function lc_apply_option_defaults()
    for _, entry in ipairs(lc_options) do
        if get_config(entry[1]) == nil then
            set_config(entry[1], entry[2])
        end
    end
end

function lc_set_pcxxheader(...)
    if get_config("kimix_enable_pch") then
        set_pcxxheader(...)
    end
end

function kimix_luisa_embedding_enabled()
    return has_config("kimix_external_thirdparty") and has_config("kimix_luisa_embedding")
end

local function lc_include_embedding_closure()
    rule("lc_install_sdk")
        before_build(function(target)
            import("find_sdk", {rootdir = lc_file("scripts")})
            find_sdk.on_install_sdk(target, "lc_install_sdk")
        end)
    rule_end()

    includes(lc_file("src/ext/liblmdb/xmake.lua"))
    includes(lc_file("src/ext/volk/xmake.lua"))

    if has_config("lc_vk_backend") then
        includes(lc_file("src/ext/glslang/xmake.lua"))
        target("lc-glslang")
            add_defines("ENABLE_HLSL", {public = true})
            add_files(lc_file("src/ext/glslang/glslang/HLSL/*.cpp"))
        target_end()

        target("spirv-headers")
            set_kind("headeronly")
            add_includedirs(lc_file("src/ext/spirv-headers/include"),
                lc_file("src/ext/spirv-headers/include/spirv/unified1"), {public = true})
        target_end()
        includes(lc_file("src/ext/SPIRV-Tools/xmake.lua"))
        target("spirv-tools")
            remove_files(lc_file("src/ext/SPIRV-Tools/source/mimalloc.cpp"))
        target_end()
    end

    includes(lc_file("src/vstl/xmake.lua"))
    includes(lc_file("src/runtime/xmake.lua"))
    includes(lc_file("src/dsl/xmake.lua"))
    includes(lc_file("src/tile/xmake.lua"))
    includes(lc_file("src/backends/common/xmake.lua"))

    target("lc_backend_sdk")
        set_kind("phony")
        local libnames = {}
        if os.host() == "windows" and (has_config("lc_dx_backend") or has_config("lc_vk_backend")) then
            table.insert(libnames, "dx_sdk")
        end
        if os.host() == "linux" and os.arch() == "x86_64" and has_config("lc_vk_backend") then
            table.insert(libnames, "vk_sdk")
        end
        if #libnames > 0 then
            add_rules("lc_install_sdk", {libnames = libnames})
        end
    target_end()

    if has_config("lc_dx_backend") then
        includes(lc_file("src/backends/dx/xmake.lua"))
    end
    if has_config("lc_vk_backend") then
        includes(lc_file("src/backends/vk/xmake.lua"))
    end
    includes(lc_file("src/backends/validation/xmake.lua"))

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

function kimix_luisa_include()
    if not has_config("kimix_external_thirdparty") then
        return
    end
    lc_apply_option_defaults()
    print("note: external third-party leg: kimix-thirdparty -> LuisaCompute (" .. lc_root .. ")")

    includes(lc_file("src/ext/EASTL/packages/mimalloc/xmake.lua"))
    lc_eastl_enable_mimalloc = true
    lc_eastl_enable_custom_malloc = false
    includes(lc_file("src/ext/EASTL/xmake.lua"))
    target("eastl")
        add_deps("mimalloc")
    target_end()
    includes(lc_file("src/ext/spdlog/xmake.lua"))

    target("btree")
        set_kind("headeronly")
        add_includedirs(lc_file("src/ext/BTree/include"), {public = true})
    target_end()

    target("lc-yyjson")
        _config_project({project_kind = "static"})
        on_load(function(target)
            target:add("files", lc_file("src/ext/yyjson/src/yyjson.c"))
            target:add("includedirs", lc_file("src/ext/yyjson/src"), {public = true})
            target:add("cxflags", "/utf-8", {tools = "cl"})
        end)
    target_end()

    target("lc-check-winsdk")
        set_kind("phony")
    target_end()

    includes(lc_file("src/core/xmake.lua"))
    target("lc-core")
        add_deps("lc-yyjson")
    target_end()

    if kimix_luisa_embedding_enabled() then
        lc_include_embedding_closure()
    end
end

function kimix_luisa_include_embedding()
    if not kimix_luisa_embedding_enabled() then
        return
    end
    includes(lc_file("examples/embedding/xmake.lua"))

    local example_dir = lc_file("examples/embedding")
    for _, name in ipairs({
        "lc-example-embedding",
        "example_embed_pack_model",
        "example_embed_retrieval_lib",
        "example_embed_retrieval",
        "example_embed_dump_golden",
        "example_embed_text",
    }) do
        target(name)
            add_includedirs(lc_file("examples"), example_dir,
                lc_file("src/ext/yyjson/src"), {public = true})
        target_end()
    end
    target("example_embed_retrieval_lib")
        add_includedirs(path.join(example_dir, "api"), {public = true})
    target_end()
end
