// tool_registry.cpp - Implementation of the static-constructor tool registry
// (see tool_registry.h).

#include "builtin_tools/tool_registry.h"

#include <utility>

#include "builtin_tools/tool.h" // alias_detail::for_each_alias_name / alias_name_equals

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
    return _tools;
}

size_t ToolRegistry::size() const {
    std::lock_guard<kimix::spin_mutex> guard(_mutex);
    return _tools.size();
}

} // namespace kimix::builtin_tools
