// tool_registry.h - Static-constructor tool registration for the built-in
// agent tools (pattern ported from RoboCute rbc/extensions/common/
// module_register.h: a global object whose constructor appends one entry to a
// process-wide registry before main() runs).
//
// Every concrete Tool subclass in src/builtin_tools registers itself under a
// lowercase registry key ("bash", "read", ...) matching the tool attribute
// names used in the kimi-cli agent JSON manifests, plus the meta information
// the agent needs to expose it to an LLM (description + JSON-schema
// parameters), a list of accepted alias names, and a factory that constructs
// one instance for a Session.
//
// Design rules (project conventions):
//   * namespace kimix::builtin_tools; kimix:: containers only; no RTTI.
//   * Unity build: the registrar objects are `static` per registration site so
//     the concatenated kimix-llm TU keeps internal linkage for them.

#pragma once

#include <type_traits>

#include <core/kimix_core.h>

#include "builtin_tools/tool.h"

namespace kimix::builtin_tools {

// Meta information of one registered tool class.
struct ToolMeta {
    kimix::string name;            // canonical registry key, e.g. "bash"
    kimix::string aliases;         // space-separated alternate names ("" = none)
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
    // Fuzzy lookup (hallucination tolerance); null when absent. Resolution
    // order, mirroring the param-alias design in tool.h:
    //   (a) the canonical name, exactly as sent;
    //   (b) the canonical name, ASCII case-insensitive;
    //   (c) the declared alternates, by exact name;
    //   (d) the declared alternates, folded (case-insensitive with '_'/'-'/
    //       ' ' ignored, so "joboutput" also matches "JobOutput").
    const ToolMeta *resolve(kimix::string_view name) const;
    // Case-insensitive lookup; goes through the full resolution above so it
    // also accepts declared alternates. Null when absent.
    const ToolMeta *find_ci(kimix::string_view name) const;

    // Construct one instance of the named tool (full fuzzy resolution).
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
                  kimix::string_view parameters_json)
        : ToolRegistrar(name, {}, description, parameters_json) {}

    ToolRegistrar(kimix::string_view name, kimix::string_view aliases,
                  kimix::string_view description,
                  kimix::string_view parameters_json) {
        ToolMeta meta;
        // `name` may be a qualified name ("glob::Glob") when the macro is
        // used with a namespaced class; the registry key is the plain class
        // name after the last "::".
        const size_t scope = name.rfind("::");
        meta.name = (scope == kimix::string_view::npos)
                        ? kimix::string(name)
                        : kimix::string(name.substr(scope + 2));
        meta.aliases = aliases;
        meta.description = description;
        meta.parameters_json = parameters_json;
        // The factory must know the registry key so stage 1 can register
        // the instance pointer under it (see allocate_tool_instance below).
        const kimix::string registry_key = meta.name;
        meta.factory = [registry_key](Session *session) {
            return create_tool_instance<T>(registry_key, session);
        };
        ToolRegistry::instance().register_tool(std::move(meta));
    }
};

// ---------------------------------------------------------------------------
// Two-stage tool initialization
// ---------------------------------------------------------------------------
// A concrete tool instance is created in two stages, exposed as two
// functions so a caller can run stage 1 for EVERY tool it intends to create
// before running stage 2 on any of them:
//
//   1. allocate_tool_instance: the tool's full memory block is allocated
//      with kimix::allocate_with_allocator and the (not yet constructed)
//      pointer is registered in the session map under the registry key;
//   2. construct_tool_instance: the object is constructed on that memory
//      with placement new.
//
// With that discipline the session map already holds the pointer of every
// tool in the set while any stage-2 constructor runs, so one tool's
// constructor can already see the other tools' instance pointers through
// Session::tool_pointers (e.g. Pwsh keeps the Bash pointer for its validity
// probe) and such a pointer is never null for a tool that is part of the
// set. A fetched pointer may still belong to an object that is under
// construction: constructors must only STORE such a pointer (never call a
// method on it - that is undefined behaviour until its constructor ran).
// Every real use belongs in valid() / operator(), which only run after the
// stage-2 call that produced them returned.
//
// Registration is keep-first-live: when the session already registered a
// LIVE instance under the key, the new instance does not steal the slot, and
// ~Tool removes the entry it owns - so Session::tool_pointer() never hands
// out a pointer to a destroyed tool and the map always reflects the
// instances some owner keeps alive.
//
// The memory pairs with kimix::deallocate_with_allocator; `Tool` inherits
// IOperatorNewBase, so the unique_ptr's `delete` releases it through the
// same mimalloc allocator (its placement-new overload accepts the raw
// memory unchanged).
template <class T>
T *allocate_tool_instance(kimix::string_view registry_name, Session *session) {
    static_assert(std::is_base_of<Tool, T>::value,
                  "allocate_tool_instance<T>: T must derive from Tool");
    // Stage 1: the tool's full memory block + registration - the pointer
    // exists before the constructor runs (never over a live sibling's
    // registration).
    T *memory = kimix::allocate_with_allocator<T>();
    if (session != nullptr &&
        session->tool_pointer(registry_name) == nullptr) {
        session->tool_pointers[kimix::string(registry_name)] = memory;
    }
    return memory;
}

// Stage 2: placement new on the stage-1 memory (no allocation, no throw
// path in project code). `memory` must come from allocate_tool_instance<T>.
template <class T>
T *construct_tool_instance(T *memory, Session *session) {
    static_assert(std::is_base_of<Tool, T>::value,
                  "construct_tool_instance<T>: T must derive from Tool");
    return ::new (static_cast<void *>(memory)) T(session);
}

// Both stages in one call - the path the registered ToolMeta factory uses
// (and any single-tool caller). Callers creating a whole tool set should
// prefer calling allocate_tool_instance for every tool first, then
// construct_tool_instance for every tool (see the discipline note above).
template <class T>
kimix::unique_ptr<Tool>
create_tool_instance(kimix::string_view registry_name, Session *session) {
    static_assert(std::is_base_of<Tool, T>::value,
                  "create_tool_instance<T>: T must derive from Tool");
    T *memory = allocate_tool_instance<T>(registry_name, session);
    return kimix::unique_ptr<Tool>(construct_tool_instance<T>(memory, session));
}

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
// class name, with a space-separated list of accepted alias names (fuzzy
// tool-name matching; empty string = none). Needed because the registry key
// is the lowercase agent-facing name ("bash", "send_message", ...) while the
// C++ class keeps its CamelCase name - and because the natural class name may
// collide with a platform macro (Windows' <winuser.h> does
// `#define SendMessage SendMessageA`, so the send_message tool class is named
// SendMessageTool).
#define KIMIX_REGISTER_TOOL_NAMED(Class, Name, Desc, SchemaJson) \
    KIMIX_REGISTER_TOOL_NAMED_IMPL(Class, Name, __LINE__, Desc, SchemaJson)
#define KIMIX_REGISTER_TOOL_NAMED_IMPL(Class, Name, Line, Desc, SchemaJson) \
    static const ::kimix::builtin_tools::ToolRegistrar<Class> \
        KIMIX_REGISTER_TOOL_CAT(l_class_registrar_, Line)(Name, Desc, SchemaJson)
// Register `Class` under an EXPLICIT registry name WITH aliases (the form used
// by every built-in tool: lowercase canonical key + alternates).
#define KIMIX_REGISTER_TOOL_NAMED_ALIASED(Class, Name, Desc, SchemaJson,        \
                                          Aliases)                              \
    KIMIX_REGISTER_TOOL_NAMED_ALIASED_IMPL(Class, Name, __LINE__, Desc,         \
                                           SchemaJson, Aliases)
#define KIMIX_REGISTER_TOOL_NAMED_ALIASED_IMPL(Class, Name, Line, Desc,         \
                                               SchemaJson, Aliases)             \
    static const ::kimix::builtin_tools::ToolRegistrar<Class>                   \
        KIMIX_REGISTER_TOOL_CAT(l_class_registrar_, Line)(                       \
            Name, Aliases, Desc, SchemaJson)

#define KIMIX_REGISTER_TOOL(Class, Desc, SchemaJson) \
    KIMIX_REGISTER_TOOL_IMPL(Class, __LINE__, Desc, SchemaJson)
