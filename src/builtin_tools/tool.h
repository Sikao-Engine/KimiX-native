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
// Exception contract:
//   * serialize() clears `out` and appends compact UTF-8 JSON text; it never
//     fails for a valid value tree (allocation failure throws std::bad_alloc).
//   * deserialize() throws std::runtime_error with a descriptive message on
//     malformed JSON or a non-object root (including the empty span). Kernels
//     that must not throw use the non-throwing try_deserialize() helper.
//
// Recursive type (design decision D1): std::variant requires complete types,
// so the object alternative is a kimix::shared_ptr<ToolParams> (forward
// declared; shared_ptr supports incomplete types). Arrays are
// kimix::vector<ValueElement>; a "JSON array of objects" is a vector whose
// elements hold the object alternative. Default copy shares object subtrees
// (deep-copy is the caller's concern).

#pragma once

#include <cstdint>
#include <utility>
#include <variant>

#include <core/kimix_core.h> // kimix::string, vector, span, shared_ptr, unordered_map, string_hash, variant

namespace kimix::builtin_tools {

// Dummy placeholder Session owned by the caller; tools receive it via
// constructor.
struct Session {}; // empty dummy class

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
    // std::get. On the wrong alternative std::get throws
    // std::bad_variant_access (exceptions are on) - callers must probe with
    // the is_*() family first (or use data() + std::holds_alternative).
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

// A JSON object body: an ordered map of key -> ValueElement.
class ToolParams {
public:
    using value_map = kimix::unordered_map<kimix::string, ValueElement,
                                           kimix::string_hash>; // D2

    value_map values; // the JSON object body

    // Map-like convenience helpers (thin wrappers over `values`).
    // Heterogeneous lookup by kimix::string_view is not available for
    // kimix::unordered_map (the hash functor is not transparent), so lookups
    // convert to a kimix::string key internally.
    bool contains(kimix::string_view key) const {
        return values.find(kimix::string(key)) != values.end();
    }
    ValueElement *get(kimix::string_view key) {
        auto it = values.find(kimix::string(key));
        return (it != values.end()) ? &it->second : nullptr;
    }
    const ValueElement *get(kimix::string_view key) const {
        auto it = values.find(kimix::string(key));
        return (it != values.end()) ? &it->second : nullptr;
    }
    ValueElement &operator[](kimix::string_view key) {
        return values[kimix::string(key)]; // inserts null when absent
    }

    // Serialize this object as compact UTF-8 JSON text into `out` (out is
    // cleared first, no trailing NUL). Uses yyjson_mut_* with the mimalloc
    // allocator (kYYJsonAlcMi).
    void serialize(kimix::vector<char> &out) const;

    // Parse `in` as a UTF-8 JSON object and replace `values`.
    // Throws std::runtime_error on invalid JSON or a non-object root.
    void deserialize(kimix::span<char const> in);

    // Non-throwing convenience for kernel callers: returns true on success,
    // false on failure with a descriptive message in `error` (cleared on
    // success). Implemented on top of deserialize().
    bool try_deserialize(kimix::span<char const> in, kimix::string &error);
};

// Base class for concrete built-in tools. The caller owns the Session and
// keeps it alive for the Tool's lifetime; concrete tools receive it via the
// constructor and may query it through session().
class Tool {
public:
    explicit Tool(Session *session) : _session(session) {}
    virtual ~Tool(); // out-of-line in tool.cpp (vtable anchor)

    // Pure virtual: concrete tools override it to run with parsed parameters.
    // `parameters` may be null (no parameters).
    virtual void operator()(ToolParams const *parameters) = 0;

    Session *session() const { return _session; }

protected:
    Session *_session = nullptr;
};

} // namespace kimix::builtin_tools
