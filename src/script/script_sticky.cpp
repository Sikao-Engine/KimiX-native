/*
 * script_sticky.cpp -- the sticky violation model (L3 of the sandbox layers).
 *
 * THE RULE THIS FILE IMPLEMENTS
 * -----------------------------
 * A denied capability must ABORT THE RUN, and a script must not be able to
 * swallow the denial with its own pcall/xpcall/coroutine.resume (stock Lua
 * catches a plain error 1000 times out of 1000).  So:
 *
 *   1. the FIRST violation records itself in the per-run state (kind, api,
 *      category, detail, chunk, line) -- a later violation cannot overwrite it;
 *   2. the message is composed ONCE, here, and the raise pushes exactly those
 *      bytes, so a script's handler and the host see the same text;
 *   3. every catcher reachable from a script (pcall, xpcall, coroutine.resume,
 *      coroutine.close) re-raises the sticky message after the inner protected
 *      call returns -- see script_env.cpp
 *      (sb_finishpcall / sb_coresume / sb_coclose);
 *   4. Sandbox::run() treats a set flag as final even when the chunk itself
 *      returned LUA_OK, and drops whatever the chunk returned.
 *
 * THE LONGJMP RULE
 * ----------------
 * lua_error() longjmps.  Nothing in these functions keeps a C++ object with a
 * destructor alive across that call: the message is composed into
 * state->body (a member array), the kimix::strings live in the state, and the
 * function bodies end with `return sb_do_raise(L);`.  The project is built
 * without exceptions, so this is the only unwinding path that exists.
 */
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <script/script_error.h>
#include <script/script_lua.h>
#include <script/script_state.h>

namespace kimix::script {

namespace src_script_script_sticky {

/* Push "<where><body>" and raise it.  Never returns. */
int sb_do_raise(lua_State *L) {
    SandboxState *s = sb_from(L);
    luaL_where(L, 1);                          /* "chunk:line: " or "" */
    lua_pushstring(L, s->body);                /* the composed body */
    lua_concat(L, 2);                          /* "<where>BODY" */
    size_t len = 0;
    const char *msg = lua_tolstring(L, -1, &len);
    if (msg != nullptr) {
        s->sticky_msg.assign(msg, len);
        s->violation.line = sb_line_from_message(msg, len);
    }
    return lua_error(L);
}

} // namespace src_script_script_sticky
using namespace src_script_script_sticky;

int sb_raise(lua_State *L, Status kind, const char *api, const char *category, const char *detail) {
    SandboxState *s = sb_from(L);
    if (s->sticky_kind == Status::ok) {
        s->sticky_kind = kind;
        s->violation.kind = kind;
        s->violation.api.assign(api != nullptr ? api : "");
        s->violation.category.assign(category != nullptr ? category : "");
        s->violation.detail.assign(detail != nullptr ? detail : "");
        s->violation.chunk.assign(s->chunk_name);
        sb_compose_body(s->body, sizeof(s->body), kind, api, detail);
    }
    return sb_do_raise(L);
}

int sb_raise_verbatim(lua_State *L, Status kind, const char *api, const char *category, const char *detail,
                      const char *verbatim) {
    SandboxState *s = sb_from(L);
    if (s->sticky_kind == Status::ok) {
        s->sticky_kind = kind;
        s->violation.kind = kind;
        s->violation.api.assign(api != nullptr ? api : "");
        s->violation.category.assign(category != nullptr ? category : "");
        s->violation.detail.assign(detail != nullptr ? detail : "");
        s->violation.chunk.assign(s->chunk_name);
        std::snprintf(s->body, sizeof(s->body), "%s", (verbatim != nullptr) ? verbatim : "");
    }
    return sb_do_raise(L);
}

int sb_raisef(lua_State *L, Status kind, const char *category, const char *fmt, ...) {
    char text[256];
    text[0] = '\0';
    if (fmt != nullptr) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(text, sizeof(text), fmt, ap);
        va_end(ap);
    }
    return sb_raise(L, kind, nullptr, category, text);
}

int sb_sticky_reraise(lua_State *L) {
    SandboxState *s = sb_from(L);
    lua_pushlstring(L, s->sticky_msg.data(), s->sticky_msg.size());
    return lua_error(L);
}

} // namespace kimix::script
