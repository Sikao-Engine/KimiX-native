// tool_registry.cpp - Implementation of the static-constructor tool registry
// (see tool_registry.h).

#include "builtin_tools/tool_registry.h"

#include <cstdio>
#include <utility>

#include "builtin_tools/tool.h" // alias_detail::for_each_alias_name / alias_name_equals
#include "builtin_tools/tool_schema_validate.h"

namespace kimix::builtin_tools {

namespace {

// ASCII case folding for the case-insensitive lookup.
char reg_lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool reg_iequals(kimix::string_view a, kimix::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (reg_lower_ascii(a[i]) != reg_lower_ascii(b[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

ToolRegistry &ToolRegistry::instance() {
    static ToolRegistry registry; // Meyers singleton: safe before main()
    return registry;
}

void ToolRegistry::register_tool(ToolMeta meta) {
    std::lock_guard<kimix::spin_mutex> guard(_mutex);
    for (ToolMeta &existing : _tools) {
        if (existing.name == meta.name) {
            existing = std::move(meta); // last registration wins
            return;
        }
    }
    _tools.push_back(std::move(meta));
}

bool ToolRegistry::register_tool_validated(ToolMeta meta, kimix::string &error) {
    // F12 (kosong/tooling/__init__.py:33-79): an invalid parameters schema
    // must fail at load, not ship to the provider. The reference raises from
    // the pydantic model validator (the toolset build fails); the native
    // mirror without exceptions is refusal + a diagnostic.
    kimix::string schema_error;
    if (!schema_validate::validate_parameters_schema(meta.parameters_json,
                                                     schema_error)) {
        kimix::string diagnostic = "invalid parameters schema for tool '";
        diagnostic += meta.name;
        diagnostic += "': ";
        diagnostic += schema_error;
        {
            std::lock_guard<kimix::spin_mutex> guard(_mutex);
            _diagnostics.push_back(diagnostic);
        }
        error = diagnostic;
        // Static constructors cannot log through the CLI; a one-line warning
        // on stderr is the best-effort equivalent of the reference's raise.
        std::fprintf(stderr, "tool registry: %s\n", diagnostic.c_str());
        return false;
    }
    register_tool(std::move(meta));
    error.clear();
    return true;
}

bool ToolRegistry::register_external_tool(
    kimix::string_view name, kimix::string_view description,
    kimix::string_view parameters_json, ExternalToolCall call,
    kimix::string &error) {
    // F14 (toolset.py register_external_tool 1766-1785 + WireExternalTool
    // 2213-2252).
    error.clear();
    {
        std::lock_guard<kimix::spin_mutex> guard(_mutex);
        for (const ToolMeta &existing : _tools) {
            if (existing.name == name && !existing.external) {
                error = "tool name conflicts with existing tool";
                return false;
            }
        }
    }
    // WireExternalTool.__init__ -> CallableTool's model validator validates
    // the parameters schema; a failure is reported as (False, str(e)).
    kimix::string schema_error;
    if (!schema_validate::validate_parameters_schema(parameters_json,
                                                     schema_error)) {
        error = schema_error;
        return false;
    }
    ToolMeta meta;
    meta.name = kimix::string(name);
    // "description or \"No description provided.\"" (WireExternalTool).
    meta.description = description.empty() ? kimix::string("No description provided.")
                                           : kimix::string(description);
    meta.parameters_json = kimix::string(parameters_json);
    meta.external = true;
    meta.external_call = std::move(call);
    register_tool(std::move(meta)); // replaces an existing external entry
    return true;
}

const kimix::vector<kimix::string> &ToolRegistry::registration_diagnostics() const {
    std::lock_guard<kimix::spin_mutex> guard(_mutex);
    return _diagnostics;
}

bool ToolRegistry::unregister_tool(kimix::string_view name) {
    std::lock_guard<kimix::spin_mutex> guard(_mutex);
    for (auto it = _tools.begin(); it != _tools.end(); ++it) {
        if (it->name == name) {
            _tools.erase(it);
            return true;
        }
    }
    return false;
}

const ToolMeta *ToolRegistry::find(kimix::string_view name) const {
    std::lock_guard<kimix::spin_mutex> guard(_mutex);
    for (const ToolMeta &meta : _tools) {
        if (meta.name == name) {
            return &meta;
        }
    }
    return nullptr;
}

const ToolMeta *ToolRegistry::resolve(kimix::string_view name) const {
    std::lock_guard<kimix::spin_mutex> guard(_mutex);
    // (a) exact canonical name.
    for (const ToolMeta &meta : _tools) {
        if (meta.name == name) {
            return &meta;
        }
    }
    // (b) canonical name, ASCII case-insensitive.
    for (const ToolMeta &meta : _tools) {
        if (reg_iequals(meta.name, name)) {
            return &meta;
        }
    }
    // (c) declared alternates, exact.
    for (const ToolMeta &meta : _tools) {
        const ToolMeta *hit = nullptr;
        alias_detail::for_each_alias_name(meta.aliases, [&](kimix::string_view alias) {
            if (hit == nullptr && alias == name) {
                hit = &meta;
            }
        });
        if (hit != nullptr) {
            return hit;
        }
    }
    // (d) declared alternates, folded ('_'/'-'/' ' ignored, case-insensitive).
    for (const ToolMeta &meta : _tools) {
        const ToolMeta *hit = nullptr;
        alias_detail::for_each_alias_name(meta.aliases, [&](kimix::string_view alias) {
            if (hit == nullptr && alias_detail::alias_name_equals(alias, name)) {
                hit = &meta;
            }
        });
        if (hit != nullptr) {
            return hit;
        }
    }
    return nullptr;
}

const ToolMeta *ToolRegistry::find_ci(kimix::string_view name) const {
    return resolve(name);
}

kimix::unique_ptr<Tool> ToolRegistry::create(kimix::string_view name,
                                             Session *session) const {
    const ToolMeta *meta = resolve(name);
    if (meta == nullptr || !meta->factory) {
        return nullptr;
    }
    return meta->factory(session);
}

kimix::vector<ToolMeta> ToolRegistry::all() const {
    std::lock_guard<kimix::spin_mutex> guard(_mutex);
    return kimix::vector<ToolMeta>(_tools.begin(), _tools.end());
}

size_t ToolRegistry::size() const {
    std::lock_guard<kimix::spin_mutex> guard(_mutex);
    return _tools.size();
}

} // namespace kimix::builtin_tools
