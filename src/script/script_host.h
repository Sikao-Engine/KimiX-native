/*
 * script_host.h -- INTERNAL: the host-function bridge of the sandbox.
 *
 * Everything here is internal to src/script: a consumer includes
 * script_sandbox.h and touches only the public Sandbox surface
 * (bind_host_function / set_global / clear_globals / run(RunSpec)).  No
 * lua_State and no Lua header leaks out of the module (README §"Native-side
 * rules").
 *
 * THE SHAPE OF A BINDING
 * ----------------------
 * A host function is NOT installed as the script-visible C function.  What the
 * environment receives is one closure of `sb_host_trampoline` with exactly two
 * upvalues (k_host_binding_upvalues):
 *
 *     [1] light userdata  the host's `user` pointer (never dereferenced here)
 *     [2] integer         the binding's index in SandboxState::host_bindings
 *
 * Upvalues are not script-reachable (no debug library, no string.dump), so a
 * script can neither forge nor inspect them; and the index -- not a pointer --
 * is what makes a stale closure from an earlier run of a REUSED state
 * (fresh_state_per_run = false) detectable: the trampoline looks the record up
 * and verifies `host` plus `user` before it calls anything.  A rebound name
 * therefore reports Status::internal instead of silently invoking the wrong
 * host callback.
 *
 * THE LONGJMP RULE
 * ----------------
 * sb_host_trampoline IS a lua_CFunction: it raises (Status::limit for every
 * budget it enforces, an ordinary RUNTIME for a host failure the script's own
 * pcall may catch) and it therefore keeps NOTHING with a destructor on its own
 * frame.  The marshalled arguments, the result slots and the copied error text
 * all live in SandboxState, and the one non-trivial local (HostCall, which
 * carries a kimix::string) is confined to a block that is exited BEFORE any
 * raise happens.
 *
 * ONE PRECONDITION ON THE HOST
 * ----------------------------
 * `HostCall::name` is a view over the binding's stored name, i.e. over the
 * Sandbox's own list, so a callback must not call bind_host_function/set_global
 * while its own call is in flight (that can replace the list under the view).
 * Rebinding BETWEEN calls, and rebinding from inside a callback and returning
 * without touching `name` again, are both fine: the trampoline re-resolves the
 * binding by index on the next call and only ever uses POD copies of the name
 * for its own messages.
 */
#pragma once
#ifndef KIMIX_SCRIPT_HOST_H
#define KIMIX_SCRIPT_HOST_H

#include <script/script_api.h>
#include <script/script_status.h>
#include <script/script_value.h>

struct lua_State;

namespace kimix::script {

struct SandboxState;

/* The lua_CFunction installed for every host binding.  Resolves its binding from
 * the closure upvalues, marshals arguments into state-owned scratch, calls the
 * host function, pushes the results.  POD-only across any raise. */
int sb_host_trampoline(lua_State *L);

/* Install the sandbox globals into the env table at `env`.  A table global is
 * materialised here (sb_push_value -> sb_json_to_lua).  Raises on failure. */
void sb_host_install_globals(lua_State *L, SandboxState *s, int env);

/* Push one Value onto the stack: nil/boolean/integer/number/string directly, and
 * a Kind::table through the strict JSON reader (sb_json_to_lua, script_value.cpp)
 * -- so a table crosses from the host exactly the way it crossed out.  `s` is the
 * reader's cap/budget source.  false when it refused: the caller raises, and the
 * reader's reason is in state->json_detail. */
[[nodiscard]] bool sb_push_value(lua_State *L, SandboxState *s, const Value &value);

/* The global-name rule shared by add_native / bind_host_function / set_global:
 * [A-Za-z_][A-Za-z0-9_]*, non-empty.  No allocation, cannot fail otherwise. */
[[nodiscard]] bool sb_valid_name(kimix::string_view name) noexcept;

} // namespace kimix::script

#endif /* KIMIX_SCRIPT_HOST_H */
