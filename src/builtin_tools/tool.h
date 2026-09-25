// tool.h - Generic tool-parameter + tool-base infrastructure for the
// built-in agent tools (kimi-agent `CallableTool2`-style).
//
// Plan: src/builtin_tools/README.md deliverables (generic Tool/ToolParams/
// ValueElement infrastructure).
//
// Design rules (project conventions):
//   * namespace kimix::builtin_tools; classes CamelCase, functions/members
//     snake_case, private members _snake_case; K&R braces, 4-space indent,
//     `int *p` pointer style; fixed-width integers.
//   * kimix:: containers / strings in every public API (kimix::string,
//     kimix::vector, kimix::span, kimix::shared_ptr, kimix::variant) - never
//     bare std:: containers.
//   * No RTTI (dynamic_cast / typeid forbidden): variant dispatch uses
//     std::holds_alternative / std::get_if / std::get only.
//   * Unity build (batch 8) for kimix-llm: no file-scope using namespace and
//     every TU-local helper in tool.cpp is static / anonymous-namespace with
//     the `tl_` prefix.
//
// Serialization uses the vendored yyjson library with a mimalloc-backed
// allocator (kimix::llm::kYYJsonAlcMi, see src/llm/yyjson_alc.h). The writer
// buffer is allocated through that allocator and must be released with
// mi_free() (never free()).
//
// Validity contract (Tool::valid()):
// * every concrete tool answers whether it can do its job HERE - the external
// program it drives is installed (python, Git Bash, PowerShell), the
// platform supports it, and the session enables the feature (plan tools,
// the workflow tool). It is a statement about the environment and the
// session configuration, never about one call's arguments (bad arguments are
// data in the result payload).
// * the agent soul calls it right after the constructor and drops a tool that
// answers false: the model is never shown a tool that cannot run here.
// * every implementation consults tool_valid("<registry key>", <probe>) so a
// test or an embedder can pin the answer (tool_availability below).
//
// Error contract (no exceptions: kimix_enable_exception=false):
//   * serialize() clears `out` and appends compact UTF-8 JSON text; it returns
//     true on success. On failure it returns false with `out` cleared and (when
//     an `error` out-parameter is given) a descriptive message.
//   * deserialize() returns false with a descriptive message on malformed JSON
//     or a non-object root (including the empty span), leaving `values`
//     untouched. Kernels that only need a message can use try_deserialize().
//
// Recursive type (design decision D1): std::variant requires complete types,
// so the object alternative is a kimix::shared_ptr<ToolParams> (forward
// declared; shared_ptr supports incomplete types). Arrays are
// kimix::vector<ValueElement>; a "JSON array of objects" is a vector whose
// elements hold the object alternative. Default copy shares object subtrees
// (deep-copy is the caller's concern).
//
// Fuzzy alias matching (agent hallucination tolerance):
//   Parsing is exact by default - every argument has ONE canonical name and,
//   with an empty alias table, ToolParams::get() is a plain exact-key lookup.
//   The arguments of a tool call, however, are produced by an LLM, which may
//   name an argument "wrong but reasonable" (the Bash tool documents `cmd` and
//   the model sends `command`). Each tool therefore declares, for every one of
//   its arguments, the alternate spellings it accepts (param_alias), and its
//   parse entry point installs that table with ToolParams::with_aliases():
//
//       static const kimix::builtin_tools::param_alias k_bash_aliases[] = {
//           {"cmd", "command cmdline command_line shell_command"},
//           ...
//       };
//       ...
//       const ToolParams resolved = ToolParams::with_aliases(params, k_bash_aliases);
//       if (params != nullptr) { params = &resolved; } // canonical + aliases
//
//   Resolution order (ToolParams::get / contains):
//     1. the canonical key, exactly as sent - a well-formed call never
//        involves an alias;
//     2. the declared alternates, by exact name;
//     3. the declared alternates, by folded name (ASCII case-insensitive with
//        '_', '-' and ' ' ignored, so "command_line" also matches
//        "commandLine" / "Command-Line").
//   A JSON null is treated as absent for steps 2/3, and a value that resolves
//   to null is never preferred over the canonical key. alias_map is a
//   side-table: it is never serialized and never part of the JSON body.

#pragma once

#include <cstdint>
#include <utility>
#include <variant>

#include <core/kimix_core.h> // kimix::string, vector, span, shared_ptr, unordered_map, string_hash, variant
#include <core/memory.h>
namespace kimix::builtin_tools {

namespace todo { struct todo_state; } // fwd (todo_tool.h); shared_ptr tolerates it
namespace agents { class agent_registry; } // fwd (agent_tool.h); shared_ptr tolerates it

// Session owned by the caller; tools receive it via constructor.
// Extended from the original empty placeholder so tools created through the
// ToolRegistry can anchor relative paths and opt into real OS effects:
// * work_dir  - session working directory ("" == process cwd).
// * native_io - when true, tools perform real file-system access / process
//               spawning instead of returning prepared data for the Python
//               mirror. Unit tests pass nullptr or native_io == false and
//               keep the pure-kernel behaviour.
// * state_dir - directory of the persisted session state (state.json; the
//               todo tools save/load their list there). "" == in-memory
//               only (state lives in `todo_state` for the process lifetime).
// * todo_state - shared todo list state owned by the session; lazily
//                created by the todo tools (todo::session_todos) and shared
//                by every tool instance bound to this session.
// * session_id - this session's own id. Used by the agent tools
//                (send_message caller prefix / self-message rejection) and by
//                list_agents/interrupt_agent bookkeeping. Mirrors
//                kimi_cli `Session.id`.
// * plan_path - the "plan_writing_path" custom_data entry of the Python
//               session: the single file the WritePlan/ReadPlan/EditPlan tools
//               operate on. "" == no plan file configured (the plan tools
//               answer with their "no plan_writing_path set" error).
// * plan_enabled - mirror of the note-tool module flag `_enable_plan`. When
//               false the plan tools report tool_status::unsupported, which is
//               the C++ counterpart of Python's SkipThisTool (the tool is not
//               offered to the model at all).
// * is_sub_agent - mirror of custom_config["is_sub_agent"]; guards against
//               recursive subagent / workflow spawns.
// * parent_session_id - mirror of custom_config["parent_session_id"]; a
//               sub-agent's send_message always targets this id.
// * swarm_enabled - mirror of custom_data["is_swarm_session"]; the workflow
//               (AgentSwarm) tool is only offered inside a swarm session.
// * agents - shared sub-agent store owned by the session (port of
//               AgentSessionStore + the module-level registries in
//               kimix/tools/agent/__init__.py). Lazily created by the agent
//               tools and shared by every tool instance of this session.
struct Session {
    kimix::string work_dir;
    bool native_io = false;
    kimix::string state_dir;
    kimix::shared_ptr<todo::todo_state> todo_state;
    kimix::string session_id;
    kimix::string plan_path;
    bool plan_enabled = false;
    bool is_sub_agent = false;
    kimix::string parent_session_id;
    bool swarm_enabled = false;
    kimix::shared_ptr<agents::agent_registry> agents;
};

class ToolParams;

// A single JSON value: every JSON type + nested object (via ToolParams) and
// array of any ValueElement (including objects).
class ValueElement {
public:
    using Array = kimix::vector<ValueElement>;
    using ObjectPtr = kimix::shared_ptr<ToolParams>; // D1: pointer breaks the recursive-type cycle
    using variant_t = kimix::variant<
        std::nullptr_t, // JSON null (default state)
        bool,           // JSON true / false
        int64_t,        // JSON signed integer
        uint64_t,       // JSON unsigned integer
        double,         // JSON real number
        kimix::string,  // JSON string
        Array,          // JSON array (of any ValueElement incl. objects)
        ObjectPtr>;     // JSON object -> nested ToolParams

    ValueElement() = default; // -> null

    // Tagged factories (avoid std::variant int -> int64/double ambiguity).
    static ValueElement make_null() { return ValueElement{}; }
    static ValueElement make_bool(bool b) {
        ValueElement e;
        e._data = b;
        return e;
    }
    static ValueElement make_int(int64_t i) {
        ValueElement e;
        e._data = i;
        return e;
    }
    static ValueElement make_uint(uint64_t u) {
        ValueElement e;
        e._data = u;
        return e;
    }
    static ValueElement make_real(double d) {
        ValueElement e;
        e._data = d;
        return e;
    }
    static ValueElement make_string(kimix::string s) {
        ValueElement e;
        e._data = std::move(s);
        return e;
    }
    static ValueElement make_array(Array a) {
        ValueElement e;
        e._data = std::move(a);
        return e;
    }
    static ValueElement make_object(ObjectPtr o) { // nested ToolParams
        ValueElement e;
        e._data = std::move(o);
        return e;
    }

    // Type probes.
    bool is_null() const { return std::holds_alternative<std::nullptr_t>(_data); }
    bool is_bool() const { return std::holds_alternative<bool>(_data); }
    bool is_int() const { return std::holds_alternative<int64_t>(_data); }
    bool is_uint() const { return std::holds_alternative<uint64_t>(_data); }
    bool is_real() const { return std::holds_alternative<double>(_data); }
    bool is_string() const { return std::holds_alternative<kimix::string>(_data); }
    bool is_array() const { return std::holds_alternative<Array>(_data); }
    bool is_object() const { return std::holds_alternative<ObjectPtr>(_data); }

    // Typed getters. No RTTI: dispatch happens at compile time through
    // std::get. Exceptions are disabled, so a wrong alternative cannot be
    // reported with std::bad_variant_access: callers must probe with the is_*()
    // family first (or use data() + std::holds_alternative) - std::get on a
    // mismatching alternative aborts.
    bool as_bool() const { return std::get<bool>(_data); }
    int64_t as_int() const { return std::get<int64_t>(_data); }
    uint64_t as_uint() const { return std::get<uint64_t>(_data); }
    double as_real() const { return std::get<double>(_data); }
    const kimix::string &as_string() const { return std::get<kimix::string>(_data); }
    const Array &as_array() const { return std::get<Array>(_data); }
    ToolParams *as_object() {
        auto *ptr = std::get_if<ObjectPtr>(&_data);
        return (ptr != nullptr) ? ptr->get() : nullptr;
    }
    const ToolParams *as_object() const {
        const auto *ptr = std::get_if<ObjectPtr>(&_data);
        return (ptr != nullptr) ? ptr->get() : nullptr;
    }

    // Generic escape hatch (variant access; still no RTTI).
    const variant_t &data() const { return _data; }
    variant_t &data() { return _data; }

private:
    variant_t _data;
};

// One fuzzy-alias declaration: `canonical` is the documented name of a tool
// argument, `alternates` lists the spellings that are also accepted for it,
// separated by spaces, ',', '|', ';' or tabs (e.g.
// {"cmd", "command cmdline command_line"}). Both are non-owning views, so a
// declaration table is a plain `static const param_alias[]` literal.
struct param_alias {
    kimix::string_view canonical;
    kimix::string_view alternates;
};

// Implementation details of the alias matching (header-inline, no allocation).
namespace alias_detail {

// ASCII lowercase, locale-independent (the project is ASCII-only by design).
inline char alias_fold(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Characters ignored when comparing an argument name with a declared alias:
// '_' and '-' (so "command_line" == "commandLine" == "Command-Line") and ' '
// (a key can never contain the list separator, but folding it is harmless).
inline bool alias_ignorable(char c) noexcept {
    return c == '_' || c == '-' || c == ' ';
}

// True when two argument names are equal modulo case and '_'/'-'/' '.
inline bool alias_name_equals(kimix::string_view a,
                              kimix::string_view b) noexcept {
    size_t i = 0;
    size_t j = 0;
    for (;;) {
        while (i < a.size() && alias_ignorable(a[i])) {
            ++i;
        }
        while (j < b.size() && alias_ignorable(b[j])) {
            ++j;
        }
        if (i >= a.size() || j >= b.size()) {
            break;
        }
        if (alias_fold(a[i]) != alias_fold(b[j])) {
            return false;
        }
        ++i;
        ++j;
    }
    return i >= a.size() && j >= b.size(); // trailing ignorables are skipped
}

// Calls `fn(name)` for every alternate name in `list` (empty names skipped).
template <class Fn>
inline void for_each_alias_name(kimix::string_view list, Fn &&fn) {
    size_t start = 0;
    for (size_t i = 0; i <= list.size(); ++i) {
        const bool end = (i == list.size());
        const char c = end ? '\0' : list[i];
        const bool sep = end || c == ' ' || c == '\t' || c == ',' || c == '|' ||
                         c == ';' || c == '\n' || c == '\r';
        if (!sep) {
            continue;
        }
        if (i > start) {
            fn(list.substr(start, i - start));
        }
        start = i + 1;
    }
}

} // namespace alias_detail

// A JSON object body: an ordered map of key -> ValueElement.
class ToolParams {
public:
    using value_map = kimix::unordered_map<kimix::string, ValueElement,
                                           kimix::string_hash>; // D2

    // Fuzzy-alias table: canonical argument name -> the alternate names that
    // are also accepted for it (space/comma separated, see param_alias).
    // Empty by default, which makes every lookup a plain exact-key lookup.
    using alias_map_t = kimix::unordered_map<kimix::string, kimix::string,
                                             kimix::string_hash>;

    value_map values; // the JSON object body
    alias_map_t alias_map; // canonical name -> accepted alternate names

    // Map-like convenience helpers (thin wrappers over `values`).
    // Heterogeneous lookup by kimix::string_view is not available for
    // kimix::unordered_map (the hash functor is not transparent), so lookups
    // convert to a kimix::string key internally.
    //
    // get() / contains() consult `alias_map` (see the header comment): the
    // canonical key wins, a declared alias is used only when the canonical key
    // is absent or JSON null. get_exact() / contains_exact() never look at the
    // alias table - use them where a stray argument name must NOT be accepted.
    bool contains(kimix::string_view key) const { return get(key) != nullptr; }
    bool contains_exact(kimix::string_view key) const {
        return values.find(kimix::string(key)) != values.end();
    }
    ValueElement *get(kimix::string_view key) {
        return const_cast<ValueElement *>(static_cast<const ToolParams *>(this)
                                              ->get(key));
    }
    const ValueElement *get(kimix::string_view key) const {
        const ValueElement *hit = get_exact(key);
        if (hit != nullptr && !hit->is_null()) {
            return hit; // 1. the canonical key, exactly as sent
        }
        // The canonical key is absent (or JSON null): fall back to the names
        // declared for it, if any.
        const ValueElement *aliased = get_alias(key);
        return (aliased != nullptr) ? aliased : hit;
    }
    ValueElement *get_exact(kimix::string_view key) {
        return const_cast<ValueElement *>(
            static_cast<const ToolParams *>(this)->get_exact(key));
    }
    const ValueElement *get_exact(kimix::string_view key) const {
        auto it = values.find(kimix::string(key));
        return (it != values.end()) ? &it->second : nullptr;
    }
    ValueElement &operator[](kimix::string_view key) {
        return values[kimix::string(key)]; // inserts null when absent
    }

    // Record alias declarations, merging into `alias_map`. Existing entries
    // keep their order; a name already recorded (folded comparison) is skipped,
    // so re-installing the same table is idempotent. `alternates` is the
    // separator-separated list described by param_alias.
    void add_alias(kimix::string_view canonical, kimix::string_view alternates);
    void add_aliases(kimix::span<const param_alias> decls) {
        for (const param_alias &decl : decls) {
            add_alias(decl.canonical, decl.alternates);
        }
    }

    // A copy of `*params` (an empty object when `params` is null) with `decls`
    // installed. Parse entry points hold a `const ToolParams *`, so they cannot
    // install the table in place; this factory gives them an alias-resolving
    // view without changing any of their lookups:
    //
    //     const ToolParams resolved = ToolParams::with_aliases(params, k_aliases);
    //     if (params != nullptr) { params = &resolved; }
    //
    // The copy shares nested objects (see D1) and only copies the argument map.
    template <size_t N>
    static ToolParams with_aliases(const ToolParams *params,
                                   const param_alias (&decls)[N]) {
        ToolParams out;
        if (params != nullptr) {
            out.values = params->values;
            out.alias_map = params->alias_map;
        }
        out.add_aliases(kimix::span<const param_alias>(decls, N));
        return out;
    }

    // Serialize this object as compact UTF-8 JSON text into `out` (out is
    // cleared first, no trailing NUL). Uses yyjson_mut_* with the mimalloc
    // allocator (kYYJsonAlcMi). `alias_map` is a side-table and is NOT part of
    // the serialized payload.
    //
    // Never throws (kimix is built without exceptions): returns true on
    // success; on failure (yyjson could not allocate the document or the
    // output buffer) it returns false, leaves `out` cleared and, when `error`
    // is non-null, stores a descriptive message there.
    bool serialize(kimix::vector<char> &out, kimix::string *error = nullptr) const;

    // Parse `in` as a UTF-8 JSON object and replace `values`. `alias_map` (a
    // caller-installed declaration table) is left untouched: it describes the
    // reader's expectations, not the payload.
    //
    // Never throws: returns true on success; on failure (malformed JSON, a
    // non-object root or the empty span) it returns false, leaves `values`
    // untouched and, when `error` is non-null, stores a descriptive message
    // there.
    bool deserialize(kimix::span<char const> in, kimix::string *error = nullptr);

    // Non-throwing convenience for kernel callers: returns true on success,
    // false on failure with a descriptive message in `error` (cleared on
    // success). Thin wrapper over deserialize().
    bool try_deserialize(kimix::span<char const> in, kimix::string &error);

private:
    // The value of the first declared alias of `key` that is present and not
    // JSON null; null when `key` has no declared alias / none is present.
    // Pass 1 compares names exactly, pass 2 folds case and '_'/'-'/' '; the
    // folded pass picks the lexicographically smallest matching key so the
    // result does not depend on hash-map iteration order.
    const ValueElement *get_alias(kimix::string_view key) const;
};

// ---------------------------------------------------------------------------
// Availability overrides (the test/embedder hook behind Tool::valid())
// ---------------------------------------------------------------------------
// Tool::valid() asks a question about the *environment* ("is Git Bash
// installed?", "is the plan feature on for this session?"), which a unit test
// can neither force nor reproduce on an arbitrary machine. This side table
// therefore lets a test or an embedder pin the answer for one registry key;
// every concrete `valid()` implementation consults it first through
// tool_valid() below. Keys are the ToolRegistry names ("bash", "python", ...).
//
// Process-wide state (spin_mutex guarded, Meyers singleton), like the
// ToolRegistry itself.
namespace tool_availability {

// Pin the answer for `key` (replacing any previous pin).
void set_override(kimix::string_view key, bool available);
// Drop one / every pin, restoring the real probes.
void clear_override(kimix::string_view key);
void clear_all();

// The pinned answer for `key`, or nullopt when the real probe applies.
kimix::optional<bool> override_of(kimix::string_view key);

} // namespace tool_availability

// The standard preamble of every concrete Tool::valid(): the pinned answer for
// `key` when one is installed, else `probed` (the tool's real environment
// check, evaluated by the caller). Keeping the lookup in one place means an
// override reaches every tool without each of them repeating the plumbing.
bool tool_valid(kimix::string_view key, bool probed);

// Shared probe for the file-system tools (read / write / edit / glob / grep /
// read_image): they resolve relative paths against Session::work_dir, so the
// only thing that can be missing is that directory. Usable when the session is
// null or names no work dir (the process cwd applies), or when the directory
// it names exists. Never throws (error_code flavour).
bool session_work_dir_usable(const Session *session);

// Base class for concrete built-in tools. The caller owns the Session and
// keeps it alive for the Tool's lifetime; concrete tools receive it via the
// constructor and may query it through session().
class Tool : public IOperatorNewBase {
public:
    explicit Tool(Session *session) : _session(session) {}
    virtual ~Tool(); // out-of-line in tool.cpp (vtable anchor)

    // Pure virtual: concrete tools override it to run with parsed parameters.
    // `parameters` may be null (no parameters).
    virtual void operator()(ToolParams const *parameters) = 0;

    // Pure virtual: can this tool actually do its job in this environment and
    // session? False == a hard precondition is missing: the external program
    // it drives is not installed (no python interpreter, no Git Bash on
    // Windows), the platform does not support it, or the session switched the
    // feature off (Session::plan_enabled for the plan tools,
    // Session::swarm_enabled for the workflow tool) - the C++ counterpart of
    // the reference's SkipThisTool.
    //
    // The answer is about the ENVIRONMENT and the SESSION configuration the
    // tool was constructed with - never about the arguments of one call (bad
    // arguments are data in the result payload, see the error contract above).
    //
    // The agent (src/agent/soul.cpp) calls it right after the constructor: a
    // tool that answers false is never cached, never listed in the tool
    // definitions sent to the LLM backend, and never executed. Validity is
    // re-checked on every definition rebuild, so a dependency that appears
    // later (a runner injected into the agent registry, an interpreter
    // installed mid-session) re-enables the tool.
    //
    // Implementations must not spawn processes and must be cheap enough for
    // one call per tool per definition rebuild (a PATH walk or a stat is
    // fine), must not mutate the tool, and must route through tool_valid()
    // above so the override table stays effective.
    virtual bool valid() const = 0;

    // Serialized JSON result of the last operator() invocation (cleared first).
    // The base implementation returns an empty buffer; concrete tools that keep
    // a serialized result override it. Tools whose result is a ToolParams
    // object serialize it on demand.
    virtual void result_json(kimix::vector<char> &out) const { out.clear(); }

    Session *session() const { return _session; }

protected:
    Session *_session = nullptr;
};

} // namespace kimix::builtin_tools
