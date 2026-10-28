/*
 * script_run.cpp -- THE single protected boundary of the sandbox (L3/L4 glue).
 *
 * Every entry into the interpreter happens under exactly one lua_pcallk, in
 * this file.  Three reasons, and each of them is load-bearing:
 *
 *   (a) every error -- a tripwire's luaL_error, lua_setupvalue, a metamethod, the
 *       JSON marshaller, the allocator's NULL, the count hook -- surfaces on the
 *       same frames, so there is exactly one place that decides what a failure
 *       means;
 *   (b) no C++ frame with a destructor sits between the longjmp source and this
 *       boundary: the C functions it calls are POD-only (README §"The longjmp
 *       rule"), so nothing needs unwinding;
 *   (c) the message handler (sb_msgh, index 1) turns whatever came out into the
 *       one documented message format, adds a bounded traceback, and grants the
 *       report slack a memory-denied state needs to describe itself.
 *
 * The chunk runs inside sb_bootstrap, which is also where the environment is
 * built, the chunk is compiled with mode "t", and the return values are
 * marshalled -- so "compile", "run", "marshal" share the one protection instead
 * of nesting three of them.
 */
#include <cstdio>
#include <cstring>

#include <script/script_env.h>
#include <script/script_error.h>
#include <script/script_host.h>
#include <script/script_lua.h>
#include <script/script_state.h>
#include <script/script_value.h>

namespace kimix::script {

namespace src_script_script_run {

/* The message handler: one message format for every failure. */
int sb_msgh(lua_State *L) {
    SandboxState *s = sb_from(L);
    const bool was_reporting = s->reporting;
    s->reporting = true; /* the budget gets k_report_slack_bytes while we talk */

    if (s->sticky_kind != Status::ok && !s->sticky_msg.empty()) {
        /* A tripwire / the loader / a memory denial already composed the exact
         * bytes the host must see: re-push them verbatim. */
        lua_pushlstring(L, s->sticky_msg.data(), s->sticky_msg.size());
    } else {
        /* Ordinary error.  Keep Lua's own (already located) text, insert the
         * prefix right after the location so a gateway can classify it, and
         * redact a non-string error object. */
        size_t len = 0;
        const char *full = (lua_type(L, 1) == LUA_TSTRING) ? lua_tolstring(L, 1, &len) : nullptr;
        const size_t loc_len = (full != nullptr) ? sb_location_prefix_len(full, len) : 0;
        if (loc_len > 0) {
            lua_pushlstring(L, full, loc_len);
        } else {
            luaL_where(L, 1); /* "" or "chunk:line: " */
        }
        lua_pushliteral(L, "RUNTIME: ");
        if (full != nullptr) {
            lua_pushlstring(L, full + loc_len, len - loc_len);
        } else {
            (void)sb_push_text(L, s, 1);
        }
        lua_concat(L, 3);
        if (s->sticky_kind != Status::ok) { /* a sticky body without a message */
            size_t mlen = 0;
            const char *msg = lua_tolstring(L, -1, &mlen);
            if (msg != nullptr) {
                s->sticky_msg.assign(msg, mlen);
                s->violation.line = sb_line_from_message(msg, mlen);
            }
        }
    }

    /* Record where it happened, whatever the kind: the line is the first
     * "<chunk>:<line>: " group of the final message. */
    {
        size_t mlen = 0;
        const char *final_msg = lua_tolstring(L, -1, &mlen);
        if (final_msg != nullptr && s->violation.line == 0) {
            s->violation.line = sb_line_from_message(final_msg, mlen);
            if (s->violation.chunk.empty()) {
                s->violation.chunk.assign(s->chunk_name);
            }
        }
    }

    /* A bounded traceback: separate from the message, never fatal. */
    size_t len = 0;
    const char *msg = lua_tolstring(L, -1, &len);
    if (msg != nullptr) {
        luaL_traceback(L, L, msg, 1);
        size_t tlen = 0;
        const char *trace = lua_tolstring(L, -1, &tlen);
        if (trace != nullptr) {
            s->traceback.assign(trace, (tlen < k_traceback_bytes) ? tlen : k_traceback_bytes);
        }
        lua_pop(L, 1);
    }

    s->reporting = was_reporting;
    return 1;
}

/* Build the environment, install the host surface, compile the chunk, run it,
 * call the function the host named (if any), marshal the results.
 *
 * THE ONE-BOUNDARY RULE STILL HOLDS: compile + setup + the chunk + the named
 * call + the marshalling all run INSIDE the single lua_pcallk of run() -- this
 * function is what that pcallk calls.  So a raise anywhere below (a tripwire, a
 * host function's LIMIT, a `call_name` that is not a function, the JSON
 * marshaller) lands on the same frames, with no C++ object with a destructor
 * skipped on the way (README §"The longjmp rule"). */
int sb_bootstrap(lua_State *L) {
    SandboxState *s = sb_from(L);
    sb_env_build(L, s, *s->natives, s->gc_count);
    const int env = lua_gettop(L);

    /* The host's globals go in once the environment exists and before any script
     * code can observe it.  The values are the Sandbox's own copies (set_global),
     * so nothing the host owns is referenced across this run. */
    sb_host_install_globals(L, s, env);

    /* '@' tells Lua the name is a file name to use verbatim: without it a
     * message would read [string "script.lua"]:1: instead of script.lua:1:.
     * LUA_IDSIZE (60) truncates a long name inside the message; the full name is
     * carried in Result::violation.chunk. */
    char load_name[k_body_capacity];
    size_t name_len = s->chunk_name.size();
    if (name_len > sizeof(load_name) - 2) {
        name_len = sizeof(load_name) - 2;
    }
    load_name[0] = '@';
    std::memcpy(load_name + 1, s->chunk_name.data(), name_len);
    load_name[name_len + 1] = '\0';
    if (name_len > 0 && (s->chunk_name[0] == '@' || s->chunk_name[0] == '=')) {
        load_name[0] = s->chunk_name[0];
        std::memcpy(load_name + 1, s->chunk_name.data() + 1, name_len - 1);
        load_name[name_len] = '\0';
    }
    const int status = luaL_loadbufferx(L, s->source, s->source_len, load_name, "t");
    if (status != LUA_OK) {
        /* Keep Lua's own message (precise and line-numbered); record it as the
         * run's sticky message so the handler re-pushes these exact bytes. */
        size_t len = 0;
        const char *msg = lua_tolstring(L, -1, &len);
        if (s->sticky_kind == Status::ok && msg != nullptr) {
            s->sticky_kind = (status == LUA_ERRSYNTAX) ? Status::syntax
                            : (status == LUA_ERRMEM)  ? Status::memory
                                                      : Status::internal;
            s->violation.kind = s->sticky_kind;
            s->violation.chunk.assign(s->chunk_name);
            s->violation.line = sb_line_from_message(msg, len);
            s->sticky_msg.assign(msg, len);
        }
        return lua_error(L);
    }

    /* The chunk's _ENV is the run environment (a chunk with no upvalue keeps
     * none, in which case lua_setupvalue pops nothing). */
    lua_pushvalue(L, env);
    if (lua_setupvalue(L, -2, 1) == nullptr) {
        lua_pop(L, 1);
    }

    lua_call(L, 0, LUA_MULTRET); /* protected by the enclosing lua_pcallk */

    /* The setup chunk has run.  When the host named a function to call, the
     * chunk's own results are the PRELUDE (setup code), not the answer, so they
     * are dropped and the named function is called with the host's copied
     * arguments -- still inside the same protection.
     *
     * The lookup is lua_getfield on the RUN ENVIRONMENT, never lua_getglobal: the
     * chunk's _ENV is this table, while the state's global table stays
     * unreachable from script code. */
    if (!s->call_name.empty()) {
        lua_settop(L, env);
        lua_getfield(L, env, s->call_name.c_str());
        const int callee_type = lua_type(L, -1);
        if (callee_type != LUA_TFUNCTION) {
            /* Nothing has run yet that could give this a script line (the prelude
             * already returned), so the message carries no location and sb_msgh
             * supplies the "RUNTIME: " tag.  Non-sticky: the host can inspect it
             * and the caller's own pcall-free path still gets a typed Result. */
            char text[256];
            std::snprintf(text, sizeof(text), "'%s' is a %s, not a function", s->call_name.c_str(),
                          lua_typename(L, callee_type));
            lua_pushstring(L, text);
            return lua_error(L);
        }
        const int argc = static_cast<int>(s->call_args.size());
        if (argc > 0 && !lua_checkstack(L, argc + 1)) {
            return sb_raisef(L, Status::internal, "internal", "the interpreter stack is exhausted");
        }
        for (size_t i = 0; i < s->call_args.size(); ++i) {
            if (!sb_push_value(L, s, s->call_args[i])) {
                return 0; /* not reached: sb_push_value raised */
            }
        }
        lua_call(L, argc, LUA_MULTRET); /* same protection as the chunk above */
    }

    /* Collect while still inside the protection, so every __gc the chunk left
     * behind runs inside the error boundary: a finalizer that calls a forbidden
     * API becomes a sticky violation (surfaced in this run's Result) instead of a
     * warning on stderr or a panic.  The chunk's results are on the stack, which
     * is a GC root, so they survive. */
    lua_gc(L, LUA_GCCOLLECT);

    const int produced = lua_gettop(L) - env;
    sb_marshal_results(L, s, env + 1, produced);
    return 0;
}

/* Compose the MEMORY message for a state that never came up at all (the budget
 * was smaller than a bootstrap state), where no lua_State exists to be asked. */
void sb_compose_bootstrap_memory(const SandboxState *s, kimix::string &out) {
    char text[256];
    std::snprintf(text, sizeof(text),
                  "MEMORY: allocation of %zu bytes exceeds the sandbox budget (used %zu of %zu)",
                  s->pending_bytes, s->pending_used, s->pending_max);
    out.assign(text, std::strlen(text));
}

} // namespace src_script_script_run
using namespace src_script_script_run;

Result Sandbox::run(kimix::string_view source, kimix::string_view chunk_name) {
    /* One implementation.  An empty chunk name means the default, exactly as
     * before the RunSpec overload existed. */
    RunSpec spec;
    spec.source = source;
    if (!chunk_name.empty()) {
        spec.chunk_name = chunk_name;
    }
    return run(spec);
}

Result Sandbox::run(const RunSpec &spec) {
    Result result;
    Impl *impl = _impl.get();
    if (impl == nullptr) {
        result.status = Status::internal;
        result.message.assign("INTERNAL: this Sandbox was moved from");
        return result;
    }
    if (impl->busy.exchange(true, std::memory_order_acq_rel)) {
        result.status = Status::internal;
        result.message.assign("INTERNAL: concurrent run on one Sandbox (a lua_State is single-threaded)");
        return result;
    }

    /* No exceptions in this project, so a plain scope guard is enough. */
    struct BusyGuard {
        std::atomic<bool> *flag;
        ~BusyGuard() { flag->store(false, std::memory_order_release); }
    } busy_guard{&impl->busy};

    SandboxState &s = impl->state;
    const Limits &limits = impl->limits;

    if (spec.source.size() > limits.max_source_bytes) {
        char text[256];
        std::snprintf(text, sizeof(text), "LIMIT: the chunk source is %zu bytes; the sandbox allows %zu",
                      spec.source.size(), limits.max_source_bytes);
        result.status = Status::limit;
        result.message.assign(text, std::strlen(text));
        return result;
    }

    /* The source, the call name and the arguments must outlive the run: they live
     * in the Impl/state, never on this stack frame. */
    impl->source_copy.assign(spec.source.data(), spec.source.size());
    s.reset_run(limits);
    s.coroutines = impl->coroutines;
    s.gc_count = impl->gc_count;
    s.natives = &impl->natives;
    s.host_bindings = &impl->host_bindings;
    s.globals = &impl->globals;
    if (spec.chunk_name.empty()) {
        s.chunk_name.assign("script.lua");
    } else {
        s.chunk_name.assign(spec.chunk_name.data(), spec.chunk_name.size());
    }
    if (!spec.call_name.empty()) {
        s.call_name.assign(spec.call_name.data(), spec.call_name.size());
    }
    if (spec.args != nullptr && spec.argc > 0) {
        /* A state-owned copy: the caller's array may not be referenced past this
         * call (it may even be a temporary). */
        s.call_args.assign(spec.args, spec.args + spec.argc);
    }
    s.source = impl->source_copy.data();
    s.source_len = impl->source_copy.size();

    const bool ephemeral = limits.fresh_state_per_run || impl->L == nullptr;
    lua_State *L = ephemeral ? nullptr : impl->L;
    if (ephemeral) {
        L = sb_state_open(&s, limits);
        if (L == nullptr) {
            result.status = Status::memory;
            sb_compose_bootstrap_memory(&s, result.message);
            result.violation = s.violation;
            return result;
        }
    }

    /* The count hook is installed before ANY script code: every coroutine
     * created later inherits it (lstate.c), so the deadline and the cancel flag
     * cover the whole run. */
    const int granularity = (limits.hook_instruction_granularity > 0)
                                ? static_cast<int>(limits.hook_instruction_granularity)
                                : 1;
    s.hook_granularity = static_cast<uint32_t>(granularity);
    lua_sethook(L, &sb_hook, LUA_MASKCOUNT, granularity);
    s.clock.reset();

    lua_pushcfunction(L, &sb_msgh); /* index 1: the handler */
    lua_pushcfunction(L, &sb_bootstrap);
    const int status = lua_pcallk(L, 0, 0, 1, 0, nullptr);

    const size_t peak = s.alloc_peak;
    const uint64_t instructions = s.instructions;
    const double seconds = s.clock.toc_seconds();

    if (s.sticky_kind != Status::ok) {
        /* The sticky flag wins even when the chunk reported success: a denial
         * the script swallowed (or one raised inside a coroutine it never
         * resumed) ends the run and voids its results. */
        result.status = s.sticky_kind;
        result.message = s.sticky_msg;
        if (result.message.empty()) {
            result.message.assign(s.body);
        }
        if (status == LUA_OK) {
            result.message.append(" (the script continued after the denial; results were discarded)");
        }
        s.results.clear();
        s.output.clear();
    } else if (status == LUA_OK) {
        if (!sb_env_verify(L, &s)) {
            result.status = Status::behaviour;
            result.message = s.sticky_msg;
            s.results.clear();
            s.output.clear();
        } else {
            result.status = Status::ok;
            result.results = static_cast<kimix::vector<Value> &&>(s.results);
            result.output = s.output;
        }
    } else {
        result.status = (status == LUA_ERRERR) ? Status::internal : Status::runtime;
        size_t len = 0;
        const char *msg = lua_tolstring(L, -1, &len);
        if (msg != nullptr) {
            result.message.assign(msg, len);
        } else {
            result.message.assign("RUNTIME: the interpreter reported an error without a message");
        }
    }

    result.violation = s.violation;
    result.traceback = s.traceback;
    result.memory_peak = peak;
    result.instructions = instructions;
    result.host_calls = s.host_calls;
    result.seconds = seconds;

    if (ephemeral) {
        sb_state_close(L, &s);
        impl->L = nullptr;
    } else {
        impl->L = L;
    }
    return result;
}

} // namespace kimix::script
