/*
 * script_host.cpp -- the host surface of the sandbox: bind_host_function,
 * set_global, clear_globals, and the bridge that turns a script call into a
 * HostCall and a HostCall's answer back into Lua values.
 *
 * WHY THE TRAMPOLINE EXISTS
 * -------------------------
 * A host function is foreign code with no access to the interpreter, so the
 * sandbox decides what it may see: marshalled copies of the arguments (a table
 * arrives as canonical JSON, a function/thread/userdata is REFUSED), a bounded
 * output array for the results, and a copied error string.  All of that happens
 * in sb_host_trampoline, BEFORE the host callback runs and AFTER it returns --
 * never while a foreign frame is live -- so every raise in this file happens
 * with no host state and no C++ object with a destructor alive (README
 * §"The longjmp rule").
 *
 * TABLES CROSS BOTH WAYS
 * ----------------------
 * Outbound, a script table becomes canonical JSON text (sb_json_build_table).
 * Inbound, a `Value::Kind::table` carrying such a text is materialised back into
 * a real Lua table by the strict reader in script_value.cpp (sb_json_to_lua):
 * same file, same mimalloc `yyjson_alc`, same caps, so the two directions cannot
 * drift apart.  Two paths differ in who is blamed for a malformed text:
 *
 *   - SCRIPT time (a host function's return value, an installed global, a
 *     RunSpec argument): sb_push_value raises a sticky Status::limit carrying the
 *     reader's reason -- a bad text there would silently change what the script
 *     sees, so it aborts the run;
 *   - HOST time (Sandbox::set_global): the text is validated once with the
 *     standalone strict parse sb_json_check (no lua_State, no interpreter
 *     memory) and a broken text is a plain false -- the C ABI's
 *     KIMIX_ERR_INVALID_ARG -- so a host mistake cannot poison a later run.
 *
 * WHERE THE BUDGETS GO
 * --------------------
 *   Limits::max_host_calls      -- one run may call into host code N times (the
 *                                  cap is sticky: a script cannot hammer the
 *                                  host in a pcall loop)
 *   Limits::max_host_args       -- how many arguments a host function accepts
 *   Limits::max_host_ret_values -- how many values it may return
 * All three are sticky Status::limit raises, exactly like the other caps.
 *
 * WHAT IS *NOT* STICKY
 * --------------------
 * A host failure (the callback returned false, or wrote HostCall::error) is an
 * ordinary RUNTIME error: the host frame has already returned, nothing is
 * recorded as a violation, and the script's own pcall/xpcall catches it like any
 * other Lua error (invariant I3 of the plan).  The location prefix is composed
 * here with sb_where(), because sb_msgh's own luaL_where can only see this C
 * function (no line info); sb_msgh then inserts the documented "RUNTIME: " tag.
 *
 * WHAT A HOST MAY NOT NAME
 * ------------------------
 * bind_host_function and set_global refuse every name the environment itself
 * installs (sb_env_name_reserved, script_env.cpp): binding over `print` would
 * make sb_env_verify report Status::behaviour for the rest of the Sandbox's life,
 * and `set_global("print", nil)` would delete the capability.  Both answer false
 * -- no raise, because this is host time -- and the C ABI maps that false to
 * KIMIX_ERR_INVALID_ARG.  add_native deliberately keeps the older, unrestricted
 * behaviour (see its comment in script_sandbox.h).
 *
 * The file-local helpers sit in a named namespace (the tree is a unity build;
 * an anonymous namespace would collide across files).
 */
#include <script/script_host.h>

#include <cstdio>
#include <cstring>

#include <script/script_env.h>
#include <script/script_error.h>
#include <script/script_lua.h>
#include <script/script_state.h>
#include <script/script_value.h>

namespace kimix::script {

namespace src_script_script_host {

/* Marshal the Lua value at `idx` into `out`.  `out` is state-owned scratch, so a
 * raise below skips no destructor.  Raises -- never returns -- for a type that
 * cannot cross the boundary. */
void sbx_marshal_arg(lua_State *L, SandboxState *s, int idx, Value &out) {
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        out.kind = Value::Kind::nil;
        break;
    case LUA_TBOOLEAN:
        out.kind = Value::Kind::boolean;
        out.b = lua_toboolean(L, idx) != 0;
        break;
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx)) {
            out.kind = Value::Kind::integer;
            out.i = static_cast<int64_t>(lua_tointeger(L, idx));
            out.d = static_cast<double>(out.i);
        } else {
            out.kind = Value::Kind::number;
            out.d = static_cast<double>(lua_tonumber(L, idx));
        }
        break;
    case LUA_TSTRING: {
        size_t len = 0;
        const char *text = lua_tolstring(L, idx, &len);
        out.kind = Value::Kind::string;
        out.s.assign((text != nullptr) ? text : "", (text != nullptr) ? len : 0);
        break;
    }
    case LUA_TTABLE:
        /* The same bounded, cycle-safe, canonical walk the return-value
         * marshaller uses -- one writer, not a second one for host arguments. */
        if (!sb_json_build_table(L, s, idx)) {
            sb_raise(L, Status::limit, nullptr, "limit", s->json_detail);
            return; /* not reached: sb_raise raises */
        }
        out.kind = Value::Kind::table;
        out.s.assign(s->json_buffer.data(), s->json_buffer.size());
        break;
    default:
        /* function / thread / userdata / lightuserdata: the outbound half of the
         * pointer-control rule, worded like sb_marshal_results. */
        sb_raisef(L, Status::limit, "limit", "argument of type '%s' cannot cross the sandbox boundary",
                  lua_typename(L, lua_type(L, idx)));
        return; /* not reached: sb_raisef raises */
    }
}

/* The JSON reader's caps, taken from a Limits.  set_global validates a table text
 * at host time, where there is no SandboxState to read the three fields from. */
SbJsonCaps sbx_json_caps(const Limits &limits) noexcept {
    SbJsonCaps caps;
    caps.max_bytes = limits.max_result_bytes;   /* Limits::max_result_bytes */
    caps.max_string = limits.max_string_bytes;  /* Limits::max_string_bytes */
    caps.max_depth = limits.max_result_depth;   /* Limits::max_result_depth */
    return caps;
}

} // namespace src_script_script_host
using namespace src_script_script_host;

bool sb_valid_name(kimix::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    const char first = name[0];
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_')) {
        return false;
    }
    for (size_t i = 1; i < name.size(); ++i) {
        const char c = name[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

int sb_host_trampoline(lua_State *L) {
    SandboxState *s = sb_from(L);

    /* 1. Reset the per-call scratch.  These are STATE members, never stack
     * locals: everything below can raise, and a raise must never skip a
     * kimix::string destructor.  Clearing host_error here is what keeps a stale
     * error from a previous call out of this one -- and host_err, its POD twin,
     * for exactly the same reason. */
    s->host_args.clear();
    s->host_rets.clear();
    s->host_error.clear();
    s->host_err[0] = '\0';

    /* 2. Amplification budget: a script cannot drive host code for free, and the
     * budget is sticky, so a pcall loop cannot spend it twice. */
    if (++s->host_calls > static_cast<uint64_t>(s->max_host_calls)) {
        return sb_raisef(L, Status::limit, "limit", "host call budget exhausted (%u calls)",
                         static_cast<unsigned>(s->max_host_calls));
    }

    /* 3. Marshal the arguments.  Everything that can raise happens BEFORE any
     * host code runs, so no foreign frame is live when it does. */
    const int argc = lua_gettop(L);
    if (static_cast<uint32_t>(argc) > s->max_host_args) {
        return sb_raisef(L, Status::limit, "limit", "host function received %d argument(s); the sandbox allows %u",
                         argc, static_cast<unsigned>(s->max_host_args));
    }
    for (int i = 0; i < argc; ++i) {
        s->host_args.emplace_back();
        sbx_marshal_arg(L, s, i + 1, s->host_args.back());
    }

    /* 4. Resolve the binding from the closure's upvalues: [1] the host's user
     * pointer, [2] the index into SandboxState::host_bindings.  A closure that
     * outlived its binding (a reused state after a rebind) is refused here
     * rather than dispatched to the wrong host callback. */
    if (!lua_isinteger(L, lua_upvalueindex(2))) {
        return sb_raisef(L, Status::internal, "internal", "host binding closure is corrupt (no index upvalue)");
    }
    const lua_Integer raw_index = lua_tointeger(L, lua_upvalueindex(2));
    if (s->host_bindings == nullptr || raw_index < 0 ||
        static_cast<size_t>(raw_index) >= s->host_bindings->size()) {
        return sb_raisef(L, Status::internal, "internal", "host binding is no longer installed (was the Sandbox changed?)");
    }
    const SandboxNative &record = (*s->host_bindings)[static_cast<size_t>(raw_index)];
    if (!record.host || record.host_fn == nullptr || record.user != lua_touserdata(L, lua_upvalueindex(1))) {
        return sb_raisef(L, Status::internal, "internal", "host binding '%s' changed since this script was compiled",
                         record.name.c_str());
    }
    /* A bounded POD copy of the name for the messages below, so the text does not
     * depend on the binding vector still being intact after the host ran. */
    char name_buf[64];
    std::snprintf(name_buf, sizeof(name_buf), "%s", record.name.c_str());

    /* 5. Size the result slots and call the host.  `call` is the only local with
     * a destructor, and it dies at the end of this block -- before any raise
     * below. */
    if (s->max_host_ret_values > 0) {
        /* State-owned slots, reused across calls: the host writes Values in, the
         * trampoline pushes them out, and the next call overwrites them. */
        s->host_rets.resize(static_cast<size_t>(s->max_host_ret_values));
    }
    bool answered = false;
    size_t produced = 0;
    {
        HostCall call;
        call.name = kimix::string_view(record.name.data(), record.name.size());
        call.args = s->host_args.empty() ? nullptr : s->host_args.data();
        call.argc = s->host_args.size();
        call.rets = s->host_rets.empty() ? nullptr : s->host_rets.data();
        call.ret_cap = s->host_rets.size();
        call.produced = 0;
        /* The POD error scratch, so a host can report its own failure text
         * without allocating and without a buffer it has to own. */
        call.err = s->host_err;
        call.err_cap = sizeof(s->host_err);
        call.index = s->host_calls;

        answered = record.host_fn(record.user, call);
        produced = call.produced;
        if (!answered) {
            /* Copy the failure text into state-owned memory now: the message is
             * composed after `call` is gone. */
            s->host_error.assign(call.error.data(), call.error.size());
        }
    }

    /* 6. A host failure is a NORMAL runtime error -- deliberately not sticky, so
     * the script's own pcall can catch it (invariant I3).  sb_msgh turns
     * "<where>host function '...' failed: ..." into the documented
     * "<where>RUNTIME: host function '...' failed: ...". */
    if (!answered) {
        char where[k_body_capacity];
        char text[k_body_capacity];
        where[0] = '\0';
        text[0] = '\0';
        sb_where(L, where, sizeof(where));
        std::snprintf(text, sizeof(text), "host function '%s' failed: %s", name_buf, s->host_error.c_str());
        lua_pushstring(L, where);
        lua_pushstring(L, text);
        lua_concat(L, 2);
        return lua_error(L);
    }

    /* 7. A host may not overrun the slot array it was given, and the stack must
     * have room for what it accepted. */
    if (produced > s->host_rets.size()) {
        return sb_raisef(L, Status::limit, "limit", "host function produced %zu result(s); the sandbox allows %zu",
                         produced, s->host_rets.size());
    }
    if (produced > 0 && !lua_checkstack(L, static_cast<int>(produced) + 1)) {
        return sb_raisef(L, Status::internal, "internal", "the interpreter stack is exhausted");
    }
    for (size_t i = 0; i < produced; ++i) {
        if (!sb_push_value(L, s, s->host_rets[i])) {
            return 0; /* not reached: sb_push_value raised */
        }
    }
    return static_cast<int>(produced);
}

void sb_host_install_globals(lua_State *L, SandboxState *s, int env) {
    if (s->globals == nullptr || s->globals->empty()) {
        return;
    }
    if (!lua_checkstack(L, 4)) {
        sb_raisef(L, Status::internal, "internal", "the interpreter stack is exhausted");
        return; /* not reached */
    }
    for (const SandboxGlobal &global : *s->globals) {
        /* A nil value installs nothing (it removes the name), which is how a host
         * clears a global it set in an earlier run.  A table is materialised here
         * (sb_push_value -> sb_json_to_lua); set_global already proved its text
         * parses, so a failure at this point is the interpreter's memory budget,
         * and the reader's reason says which. */
        if (!sb_push_value(L, s, global.value)) {
            sb_raise(L, Status::limit, nullptr, "limit", s->json_detail);
            return; /* not reached */
        }
        lua_setfield(L, env, global.name.c_str());
    }
}

bool sb_push_value(lua_State *L, SandboxState *s, const Value &value) {
    switch (value.kind) {
    case Value::Kind::nil:
        lua_pushnil(L);
        return true;
    case Value::Kind::boolean:
        lua_pushboolean(L, value.b ? 1 : 0);
        return true;
    case Value::Kind::integer:
        lua_pushinteger(L, static_cast<lua_Integer>(value.i));
        return true;
    case Value::Kind::number:
        lua_pushnumber(L, static_cast<lua_Number>(value.d));
        return true;
    case Value::Kind::string:
        lua_pushlstring(L, value.s.data(), value.s.size());
        return true;
    case Value::Kind::table:
        /* Host -> script: the same strict reader (and the same caps, the same
         * mimalloc alc) that the outbound marshaller's text came from, so a table
         * crosses both ways.  This runs while a host function's return value or an
         * installed global is being pushed, i.e. at SCRIPT time, so a malformed
         * text is a sticky LIMIT -- never a silently missing value.  The reader
         * itself does not raise; it leaves the reason in state->json_detail. */
        if (!sb_json_to_lua(L, s, value.s.data(), value.s.size())) {
            sb_raise(L, Status::limit, nullptr, "limit", s->json_detail);
            return false; /* not reached: sb_raise raises */
        }
        return true;
    }
    lua_pushnil(L);
    return true;
}

/* ---- Sandbox: the host surface ----------------------------------------- */

bool Sandbox::bind_host_function(kimix::string_view name, HostFn fn, void *user) {
    /* A reserved name is a host mistake, not a script one: report it as false (the
     * C ABI maps that to KIMIX_ERR_INVALID_ARG) instead of raising, and do it
     * before the rebind branch so an existing binding cannot be turned into a
     * primitive replacement either. */
    if (_impl == nullptr || fn == nullptr || !sb_valid_name(name) || sb_env_name_reserved(name)) {
        return false;
    }
    for (SandboxNative &binding : _impl->host_bindings) {
        if (binding.name.size() == name.size() && std::memcmp(binding.name.data(), name.data(), name.size()) == 0) {
            /* Re-binding replaces: the closure installed for this name is the same
             * trampoline, so only the record behind the index has to change. */
            binding.host = true;
            binding.host_fn = fn;
            binding.user = user;
            binding.fn = nullptr;
            binding.upvalues = k_host_binding_upvalues;
            return true;
        }
    }
    SandboxNative entry;
    entry.name.assign(name.data(), name.size());
    entry.fn = nullptr; /* the script-visible function is sb_host_trampoline */
    entry.upvalues = k_host_binding_upvalues;
    entry.user = user;
    entry.host = true;
    entry.host_fn = fn;
    _impl->host_bindings.push_back(static_cast<SandboxNative &&>(entry));
    return true;
}

bool Sandbox::set_global(kimix::string_view name, const Value &value) {
    /* Overwriting a name the environment owns would delete a capability
     * (`set_global("print", nil)`) or poison every later run (sb_env_verify =>
     * Status::behaviour), so it is refused here -- host time, hence a plain
     * false, never a raise. */
    if (_impl == nullptr || !sb_valid_name(name) || sb_env_name_reserved(name)) {
        return false;
    }
    switch (value.kind) {
    case Value::Kind::nil:
    case Value::Kind::boolean:
    case Value::Kind::integer:
    case Value::Kind::number:
    case Value::Kind::string:
        break;
    case Value::Kind::table:
        /* A table is carried as its canonical JSON text, and the run's install step
         * (sb_host_install_globals -> sb_push_value) materialises it.  The text is
         * validated ONCE here: a standalone strict parse with the same caps and the
         * same duplicate-key rule as the materialisation, but with no lua_State and
         * no interpreter memory, so a host mistake is a plain false NOW (the C ABI
         * maps it to KIMIX_ERR_INVALID_ARG) instead of a sticky LIMIT that poisons
         * the run that happens to install it.  Host time, no raise. */
        if (!sb_json_check(value.s.data(), value.s.size(), sbx_json_caps(_impl->limits))) {
            return false;
        }
        break;
    }
    for (SandboxGlobal &global : _impl->globals) {
        if (global.name.size() == name.size() && std::memcmp(global.name.data(), name.data(), name.size()) == 0) {
            global.value = value; /* overwrite */
            return true;
        }
    }
    SandboxGlobal entry;
    entry.name.assign(name.data(), name.size());
    entry.value = value;
    _impl->globals.push_back(static_cast<SandboxGlobal &&>(entry));
    return true;
}

void Sandbox::clear_globals() {
    if (_impl != nullptr) {
        _impl->globals.clear();
    }
}

} // namespace kimix::script
