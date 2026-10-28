/*
 * script_env.h -- INTERNAL: the run environment (L2 of the sandbox layers).
 *
 * `sb_env_build` creates the fresh _ENV table of one run and fills it with
 * exactly three kinds of name:
 *
 *   ALLOW   the safe base functions and the string/table/math/utf8 tables,
 *   REPLACE a vetted C wrapper in place of a stock function (load, pcall,
 *           xpcall, print, warn, tostring, collectgarbage, string.format, and
 *           coroutine.resume/close when coroutines are on),
 *   DENY    a LOUD tripwire: a table or function that exists, is callable, and
 *           raises "DENIED: <api> -- <why>" naming the exact member (io.open,
 *           os.exit, package.loadlib, ...).
 *
 * A tripwire beats an absent global on purpose: "attempt to index a nil value"
 * is silent and unhelpful, while the tripwire tells the script (and the host)
 * what it tried to do and why it is not allowed.
 *
 * `sb_env_verify` is the integrity post-check: it re-reads every primitive from
 * the environment and proves it is still the sandbox's own C closure (same
 * lua_CFunction AND same upvalue-1 tag), so a chunk that does
 * `pcall = function() return true end` is reported as Status::behaviour instead
 * of being silently obeyed.  `sb_push_text` is the redacting converter behind
 * tostring/print/error-to-string: it never prints an address.
 *
 * `sb_env_name_reserved` is the host-time half of the same rule: it answers from
 * ONE table in script_env.cpp that lists every name `sb_env_build` installs, so
 * a host cannot bind_host_function / set_global over a name the environment owns
 * (see its declaration below).
 */
#pragma once
#ifndef KIMIX_SCRIPT_ENV_H
#define KIMIX_SCRIPT_ENV_H

#include <cstdint>

#include <core/stl/string.h>
#include <core/stl/vector.h>

struct lua_State;

namespace kimix::script {

struct SandboxNative;
struct SandboxState;

/* Build the environment and leave it on the top of the stack (the caller owns
 * that stack slot).  `natives` are the host functions from
 * Sandbox::add_native; the host-function bindings (bind_host_function) are read
 * from the state (SandboxState::host_bindings) because the trampoline resolves
 * them by index.  May raise (LIMIT / MEMORY) -- never returns normally in that
 * case. */
void sb_env_build(lua_State *L, SandboxState *s, const kimix::vector<SandboxNative> &natives, bool gc_count);

/* true when every sandbox primitive in the environment is still ours.  Records
 * the BEHAVIOUR violation itself on failure (no raise). */
bool sb_env_verify(lua_State *L, SandboxState *s);

/* true when `name` is one the environment itself installs (an allow-listed
 * primitive, a replacement, or a tripwire).  A host may NOT bind or set such a
 * name: overwriting it either deletes a capability or poisons every later run.
 *
 * The answer comes from ONE table in script_env.cpp, next to the install code
 * (kEnvReservedNames); that table must be updated whenever a name is added to
 * the environment.  test_script_host enumerates the built environment and fails
 * when a reachable name is missing from it, so the drift is a test failure, not
 * a silent hole.  Host-time only: no allocation, no raise, no Lua call. */
[[nodiscard]] bool sb_env_name_reserved(kimix::string_view name) noexcept;

/* Push the redacted text of the value at `idx` ("nil", "true", "1.5",
 * "table: #1", "function: #2", or the value's own __tostring result).  Returns
 * false only when the interpreter cannot allocate. */
bool sb_push_text(lua_State *L, SandboxState *s, int idx);

} // namespace kimix::script

#endif /* KIMIX_SCRIPT_ENV_H */
