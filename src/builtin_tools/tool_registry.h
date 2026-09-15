// tool_registry.h - Static-constructor tool registration for the built-in
// agent tools (pattern ported from RoboCute rbc/extensions/common/
// module_register.h: a global object whose constructor appends one entry to a
// process-wide registry before main() runs).
//
// Every concrete Tool subclass in src/builtin_tools registers itself with its
// C++ class name ("Bash", "Read", ...) plus the meta information the agent
// needs to expose it to an LLM (description + JSON-schema parameters) and a
// factory that constructs one instance for a Session.
//
// Design rules (project conventions):
//   * namespace kimix::builtin_tools; kimix:: containers only; no RTTI.
//   * Unity build: the registrar objects are `static` per registration site so
//     the concatenated kimix-llm TU keeps internal linkage for them.

#pragma once

#include <core/kimix_core.h>

#include "builtin_tools/tool.h"

namespace kimix::builtin_tools {

// Meta information of one registered tool class.
struct ToolMeta {
    kimix::string name;            // derived class name, e.g. "Bash"
    kimix::string description;     // human/LLM-facing description
    kimix::string parameters_json; // JSON schema object (may be "{}")
    // Factory: constructs one Tool instance bound to `session` (may be null).
    kimix::function<kimix::unique_ptr<Tool>(Session *)> factory;
};

// Process-wide registry of tool classes. Thread-safe (spin_mutex guarded),
// Meyers-singleton so static registrars running before main() are safe.
class ToolRegistry {
public:
    static ToolRegistry &instance();

    // Append one entry. A duplicate `name` replaces the previous entry.
    void register_tool(ToolMeta meta);

    // Exact-name lookup; null when absent.
    const ToolMeta *find(kimix::string_view name) const;
    // Case-insensitive lookup ("bash" -> "Bash"); null when absent.
    const ToolMeta *find_ci(kimix::string_view name) const;

    // Construct one instance of the named tool (exact or case-insensitive).
    // Returns null when the name is unknown.
    kimix::unique_ptr<Tool> create(kimix::string_view name, Session *session) const;

    // Snapshot of every registered meta entry, in registration order.
    kimix::vector<ToolMeta> all() const;

    size_t size() const;

private:
    ToolRegistry() = default;
    ToolRegistry(const ToolRegistry &) = delete;
    ToolRegistry &operator=(const ToolRegistry &) = delete;

    mutable kimix::spin_mutex _mutex;
    kimix::vector<ToolMeta> _tools; // insertion order
};

// Static registrar: constructing one instance registers class T under
// `name` with the given description and JSON schema. Used through the
// KIMIX_REGISTER_TOOL macro at namespace scope in each tool's .cpp.
template <class T>
class ToolRegistrar {
public:
    ToolRegistrar(kimix::string_view name, kimix::string_view description,
                  kimix::string_view parameters_json) {
        ToolMeta meta;
        // `name` may be a qualified name ("glob::Glob") when the macro is
        // used with a namespaced class; the registry key is the plain class
        // name after the last "::".
        const size_t scope = name.rfind("::");
        meta.name = (scope == kimix::string_view::npos)
                        ? kimix::string(name)
                        : kimix::string(name.substr(scope + 2));
        meta.description = description;
        meta.parameters_json = parameters_json;
        meta.factory = [](Session *session) {
            return kimix::unique_ptr<Tool>(new T(session));
        };
        ToolRegistry::instance().register_tool(std::move(meta));
    }
};

} // namespace kimix::builtin_tools

// Register `Class` (a kimix::builtin_tools::Tool subclass constructible from a
// single Session* argument) under its class name with a description and a
// JSON-schema string for its parameters. Place at namespace scope in the
// tool's .cpp file.
#define KIMIX_REGISTER_TOOL_CAT(a, b) a##b
#define KIMIX_REGISTER_TOOL_IMPL(Class, Line, Desc, SchemaJson)                  \
    static const ::kimix::builtin_tools::ToolRegistrar<Class>                    \
        KIMIX_REGISTER_TOOL_CAT(l_class_registrar_, Line)(#Class, Desc, SchemaJson)
// Register `Class` under an EXPLICIT registry name instead of the stringized
// class name. Needed when the natural class name collides with a platform
// macro - Windows' <winuser.h> does `#define SendMessage SendMessageA`, so the
// send_message tool class is named SendMessageTool but registered as
// "SendMessage" (the CamelCase form of the agent-facing `send_message`).
#define KIMIX_REGISTER_TOOL_NAMED(Class, Name, Desc, SchemaJson) \
    KIMIX_REGISTER_TOOL_NAMED_IMPL(Class, Name, __LINE__, Desc, SchemaJson)
#define KIMIX_REGISTER_TOOL_NAMED_IMPL(Class, Name, Line, Desc, SchemaJson) \
    static const ::kimix::builtin_tools::ToolRegistrar<Class> \
        KIMIX_REGISTER_TOOL_CAT(l_class_registrar_, Line)(Name, Desc, SchemaJson)

#define KIMIX_REGISTER_TOOL(Class, Desc, SchemaJson) \
    KIMIX_REGISTER_TOOL_IMPL(Class, __LINE__, Desc, SchemaJson)
