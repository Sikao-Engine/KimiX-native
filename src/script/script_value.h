/*
 * script_value.h -- the one value type that may cross the sandbox boundary.
 *
 * A `Value` is a PURE DATA snapshot of a Lua value: no pointer, no lua_State,
 * no reference into the interpreter heap.  That is the outbound half of the
 * pointer-control rule in src/script/README.md:
 *
 *   nil / booleans / integers / numbers -> copied by value,
 *   strings                            -> copied bytes,
 *   tables                             -> canonical JSON text (minified,
 *                                         object keys sorted), produced by
 *                                         the marshaller in script_value.cpp,
 *   function / thread / userdata       -> REFUSED (Status::limit); a script
 *                                         can therefore never hand the native
 *                                         side something callable or
 *                                         addressable.
 *
 * TABLES CROSS BOTH WAYS.  The same canonical-JSON text is what the host hands
 * BACK: `Value::Kind::table` is materialised by the strict reader in
 * script_value.cpp (object -> table with string keys, array -> 1..n) under the
 * same caps as the writer.  Two consequences a caller must know:
 *   - a JSON `null` becomes Lua nil, so an OBJECT KEY whose value is null
 *     disappears from the table (a Lua table cannot hold nil); inside an ARRAY a
 *     null leaves a hole, so `#t` stops at the first null;
 *   - duplicate object keys are REFUSED (canonical JSON never has them, so a
 *     duplicate is a malformed input), as is anything that is not strict JSON.
 *
 * The type is deliberately flat (a Kind tag plus the three payload fields) so
 * a C caller can mirror it, and so a Result can hold a vector of them.
 */
#pragma once
#ifndef KIMIX_SCRIPT_VALUE_H
#define KIMIX_SCRIPT_VALUE_H

#include <cstddef>
#include <cstdint>

#include <core/stl/string.h>

struct lua_State;

namespace kimix::script {

struct SandboxState;

struct Value {
    enum class Kind : uint8_t {
        nil = 0,
        boolean,
        integer,
        number,
        string, /* Kind::string: the raw bytes (s) */
        table,  /* Kind::table:  canonical JSON text (s) */
    };

    Kind kind = Kind::nil;
    bool b = false;
    int64_t i = 0;
    double d = 0.0;
    kimix::string s;

    /* "nil" / "boolean" / "integer" / "number" / "string" / "table" -- the
     * same words the sandbox's LIMIT messages use. */
    static const char *kind_name(Kind k) noexcept;

    void clear() noexcept;
};

/* INTERNAL (script_value.cpp): marshal `count` Lua values starting at the
 * absolute stack index `first` into state->results.  Raises a Status::limit
 * error (and never returns) when a value cannot cross the boundary: a function,
 * a thread, a userdata, a table over the depth/byte cap, or a cycle. */
bool sb_marshal_results(lua_State *L, SandboxState *s, int first, int count);

/* INTERNAL (script_value.cpp): build the CANONICAL JSON of the table at `idx`
 * into state->json_buffer (minified, keys byte-sorted, cycle-safe, bounded by
 * max_result_bytes / max_result_depth).  The one table writer in the module:
 * the outbound marshaller and the host-argument marshaller (script_host.cpp)
 * both go through it.  Never raises; a failure leaves the reason in
 * state->json_detail for the caller to raise. */
bool sb_json_build_table(lua_State *L, SandboxState *s, int idx);

/* The caps the JSON READER enforces, as a plain POD of the three Limits fields it
 * needs.  Separate from Limits because the host-time validator
 * (sb_json_check, called by Sandbox::set_global, which has no SandboxState and no
 * lua_State yet) has to name the same three numbers without dragging the
 * Sandbox's state along. */
struct SbJsonCaps {
    size_t max_bytes = 0;   /* Limits::max_result_bytes: the text itself and the node budget */
    size_t max_string = 0;  /* Limits::max_string_bytes: one string */
    uint32_t max_depth = 0; /* Limits::max_result_depth (further capped by kSbJsonDepthLimit) */
};

/* Native-recursion bound of the reader walk (and of the writer's own
 * json_depth_cap): one native frame per nesting level, so a host that asked for
 * max_result_depth = 1'000'000 still cannot overflow the C stack. */
constexpr uint32_t kSbJsonDepthLimit = 64;

/* INTERNAL (script_value.cpp): materialise the strict JSON text in
 * [json, json+len) as a Lua value on the stack (object -> table with string
 * keys, array -> 1..n; the root must be an object or an array).
 *
 * Strict reader: no comments, no NaN/Infinity, no trailing garbage, no duplicate
 * object keys, `null` -> nil (so an object key with a null value disappears and
 * an array null is a hole).  Integers stay integers, other numbers become floats,
 * strings are pushed byte-exact.  Bounded by state->max_result_bytes (the text and
 * the total node count), state->max_result_depth and state->max_string_bytes, and
 * by the interpreter's own memory budget.
 *
 * Never raises -- it answers false and leaves the reason in state->json_detail;
 * the CALLER decides whether that is a sticky LIMIT (script time) or a plain
 * false (host time).  On failure nothing is left on the stack and nothing is
 * left allocated. */
[[nodiscard]] bool sb_json_to_lua(lua_State *L, SandboxState *s, const char *json, size_t len);

/* INTERNAL (script_value.cpp): validate a table text at HOST time, with no
 * lua_State and no interpreter memory (Sandbox::set_global uses it, so a broken
 * text is refused with a plain false -- the C ABI's KIMIX_ERR_INVALID_ARG --
 * instead of poisoning a later run with a sticky LIMIT).
 *
 * It runs exactly the parse + validating walk sb_json_to_lua runs (same caps,
 * same duplicate-key rule), and skips only the step that builds Lua values, so
 * "accepted here" always means "materialisable at run time".  Does not allocate
 * from the interpreter and cannot raise. */
[[nodiscard]] bool sb_json_check(const char *json, size_t len, const SbJsonCaps &caps) noexcept;

} // namespace kimix::script

#endif /* KIMIX_SCRIPT_VALUE_H */
