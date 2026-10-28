/*
 * script_env.cpp -- the run environment: allow list, replacements, tripwires,
 * the redacting converter, and the integrity post-check.
 *
 * See script_env.h for the model.  Two implementation rules matter:
 *
 *   1. EVERY lua_CFunction in this file is POD-only and raises at the very end
 *      ("return sb_raise(...)" / "return lua_error(L)").  lua_error longjmps, so
 *      a kimix::string or any other non-trivial local would be skipped rather
 *      than destroyed (README §"The longjmp rule").  Scratch buffers live in
 *      SandboxState instead.
 *   2. The replacements RE-RAISE.  A sticky violation is final: sb_pcall /
 *      sb_xpcall / coroutine.resume / coroutine.close check the sticky flag
 *      after their inner protected call and raise the recorded message, so a
 *      script's own pcall cannot turn a denial into a `false, "..."` return
 *      value.  That single rule is what makes "abort the run" true.
 */
#include <script/script_env.h>

#include <cstdio>
#include <cstring>

#include <script/script_error.h>
#include <script/script_host.h>
#include <script/script_lua.h>
#include <script/script_state.h>

namespace kimix::script {

namespace src_script_script_env {

/* ------------------------------------------------------------------------- */
/* Shared vocabulary                                                         */
/* ------------------------------------------------------------------------- */

const char *const kCatFileIo = "file-io";
const char *const kCatProcess = "process";
const char *const kCatEnv = "env";
const char *const kCatTime = "time";
const char *const kCatModule = "module";
const char *const kCatIntrospection = "introspection";
const char *const kCatBytecode = "bytecode";
const char *const kCatBytekey = "pointer";
const char *const kCatGc = "gc";

const char *const kDetailFileIo = "file I/O is not available in the sandbox";
const char *const kDetailProcess = "process and OS access is not available in the sandbox";
const char *const kDetailModule = "module loading is not available in the sandbox";
const char *const kDetailDebug = "debug introspection is not available in the sandbox";
const char *const kDetailLoadFile = "loading files is not available in the sandbox";
const char *const kDetailBytecode = "bytecode dumping is not available in the sandbox";

/* The registry key that holds the per-run redaction-id table.  The ADDRESS of
 * this object is the light-userdata key (stable, module-private). */
const char k_id_table_key = 0;

unsigned char *sb_key_addr() {
    return reinterpret_cast<unsigned char *>(const_cast<char *>(&k_id_table_key));
}

/* ------------------------------------------------------------------------- */
/* Output capture                                                            */
/* ------------------------------------------------------------------------- */

/* Append to the captured output; a cap overrun is a sticky LIMIT (the script
 * cannot flood the host's memory through print()). */
void sbx_output_append(lua_State *L, SandboxState *s, const char *text, size_t len) {
    if (text == nullptr) {
        return;
    }
    const size_t room = (s->output.size() < s->max_output) ? (s->max_output - s->output.size()) : 0;
    if (len > room) {
        s->output.append(text, room);
        sb_raisef(L, Status::limit, "limit", "output exceeds the %zu-byte cap", s->max_output);
        return; /* not reached */
    }
    s->output.append(text, len);
}

/* ------------------------------------------------------------------------- */
/* Redacting converter (tostring / print / error objects)                     */
/* ------------------------------------------------------------------------- */

/* The stable per-object id behind "table: #1" -- a linear counter per run, kept
 * in a registry table so the same object keeps the same number. */
int sbx_object_id(lua_State *L, SandboxState *s, int idx) {
    lua_pushlightuserdata(L, sb_key_addr());
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushlightuserdata(L, sb_key_addr());
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX); /* registry[key] = table (pops key+value) */
    }
    const int table = lua_gettop(L);
    lua_pushvalue(L, idx);
    lua_rawget(L, table);
    int id = 0;
    if (lua_isinteger(L, -1)) {
        id = static_cast<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
        id = static_cast<int>(++s->id_counter);
        lua_pushvalue(L, idx);
        lua_pushinteger(L, id);
        lua_rawset(L, table);
    }
    lua_pop(L, 1); /* the id table */
    return id;
}

/* The one place a value becomes text in the sandbox.  Never leaks an address: a
 * non-scalar with __tostring is asked for its own text, everything else gets the
 * redacted "<kind>: #<n>" form. */
void sbx_push_redacted(lua_State *L, SandboxState *s, int idx) {
    const int type = lua_type(L, idx);
    switch (type) {
    case LUA_TNIL:
        lua_pushliteral(L, "nil");
        return;
    case LUA_TBOOLEAN:
        lua_pushstring(L, lua_toboolean(L, idx) != 0 ? "true" : "false");
        return;
    case LUA_TNUMBER:
    case LUA_TSTRING:
        /* Scalars: stock formatting, no address involved. */
        lua_pushvalue(L, idx);
        luaL_tolstring(L, -1, nullptr);
        lua_remove(L, -2);
        return;
    default:
        break;
    }

    /* A value with its own __tostring answers for itself (that is also what
     * makes string.format('%s', <host object>) informative). */
    const int metafield = luaL_getmetafield(L, idx, "__tostring");
    if (metafield == LUA_TFUNCTION) {
        lua_pushvalue(L, idx);
        lua_call(L, 1, 1);
        if (!lua_isstring(L, -1)) {
            lua_pop(L, 1);
            lua_pushfstring(L, "%s (__tostring did not return a string)", lua_typename(L, type));
        }
        return;
    }
    if (metafield != LUA_TNIL) {
        lua_pop(L, 1);
    }

    const int id = sbx_object_id(L, s, idx);
    lua_pushfstring(L, "%s: #%d", lua_typename(L, type), id);
}

/* ------------------------------------------------------------------------- */
/* Replacements                                                              */
/* ------------------------------------------------------------------------- */

/* `pcall` / `xpcall` continuation: the sticky check comes FIRST, so a denial
 * that the protected call "handled" is raised again instead of returned. */
int sbx_finishpcall(lua_State *L, int status, lua_KContext extra) {
    SandboxState *s = sb_from(L);
    if (s->sticky_kind != Status::ok) {
        return sb_sticky_reraise(L);
    }
    if (status != LUA_OK && status != LUA_YIELD) { /* error? */
        lua_pushboolean(L, 0);    /* first result (false) */
        lua_pushvalue(L, -2);     /* error message */
        return 2;                 /* return false, msg */
    }
    return lua_gettop(L) - static_cast<int>(extra);
}

int sbx_pcall(lua_State *L) {
    (void)lua_tointeger(L, lua_upvalueindex(1)); /* tag: identity for the post-check */
    int status;
    luaL_checkany(L, 1);
    lua_pushboolean(L, 1); /* first result if no errors */
    lua_insert(L, 1);      /* put it in place */
    status = lua_pcallk(L, lua_gettop(L) - 2, LUA_MULTRET, 0, 0, &sbx_finishpcall);
    return sbx_finishpcall(L, status, 0);
}

int sbx_xpcall(lua_State *L) {
    (void)lua_tointeger(L, lua_upvalueindex(1));
    int status;
    const int n = lua_gettop(L);
    luaL_checktype(L, 2, LUA_TFUNCTION); /* check error function */
    lua_pushboolean(L, 1);               /* first result */
    lua_pushvalue(L, 1);                 /* function */
    lua_rotate(L, 3, 2);                 /* move them below the function's arguments */
    status = lua_pcallk(L, n - 2, LUA_MULTRET, 2, 2, &sbx_finishpcall);
    return sbx_finishpcall(L, status, 2);
}

int sbx_print(lua_State *L) {
    SandboxState *s = sb_from(L);
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) {
            sbx_output_append(L, s, "\t", 1);
        }
        sbx_push_redacted(L, s, i);
        size_t len = 0;
        const char *text = lua_tolstring(L, -1, &len);
        sbx_output_append(L, s, text, len);
        lua_pop(L, 1);
    }
    sbx_output_append(L, s, "\n", 1);
    return 0;
}

int sbx_warn(lua_State *L) {
    SandboxState *s = sb_from(L);
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        luaL_checkstring(L, i);
        size_t len = 0;
        const char *text = lua_tolstring(L, i, &len);
        sbx_output_append(L, s, text, len);
    }
    return 0;
}

int sbx_tostring(lua_State *L) {
    SandboxState *s = sb_from(L);
    luaL_checkany(L, 1);
    sbx_push_redacted(L, s, 1);
    return 1;
}

/* `string.format`: `%p` names a pointer, so it is denied; a non-scalar argument
 * becomes its redacted text instead of luaL_tolstring's "%s: %p".  Both close
 * the residual address leak from the host side, without patching the vendored
 * sources. */
int sbx_format(lua_State *L) {
    SandboxState *s = sb_from(L);
    (void)lua_tointeger(L, lua_upvalueindex(1));
    size_t fmt_len = 0;
    const char *fmt = luaL_checklstring(L, 1, &fmt_len);
    for (size_t i = 0; i + 1 < fmt_len; ++i) {
        if (fmt[i] != '%') {
            continue;
        }
        const char c = fmt[i + 1];
        if (c == '%') {
            ++i;
            continue;
        }
        if (c == 'p') {
            return sb_raise(L, Status::denied, "string.format", kCatBytekey,
                            "format '%p' is not available in the sandbox");
        }
    }
    const int base = lua_gettop(L);
    for (int i = 2; i <= base; ++i) {
        const int type = lua_type(L, i);
        if (type == LUA_TTABLE || type == LUA_TFUNCTION || type == LUA_TTHREAD || type == LUA_TUSERDATA ||
            type == LUA_TLIGHTUSERDATA) {
            sbx_push_redacted(L, s, i);
            lua_replace(L, i);
        }
    }
    lua_pushvalue(L, lua_upvalueindex(2)); /* the stock string.format */
    lua_insert(L, 1);
    lua_call(L, base, LUA_MULTRET);
    return lua_gettop(L);
}

int sbx_collectgarbage(lua_State *L) {
    SandboxState *s = sb_from(L);
    (void)lua_tointeger(L, lua_upvalueindex(1));
    const char *option = luaL_optstring(L, 1, "collect");
    if (s->gc_count && std::strcmp(option, "count") == 0) {
        const int base = lua_gettop(L);
        lua_pushvalue(L, lua_upvalueindex(2)); /* the stock collectgarbage */
        lua_pushvalue(L, 1);
        lua_call(L, 1, LUA_MULTRET);
        return lua_gettop(L) - base;
    }
    return sb_raise(L, Status::denied, "collectgarbage", kCatGc,
                    "the collector can only be inspected with collectgarbage('count')");
}

/* `load`: text only.  A binary chunk is refused with the LOADERR text of the
 * plan (Appendix A) instead of reaching Lua's undumper, and a reader function is
 * accumulated against the source cap -- never handed to lua_load, whose reader
 * contract would let a script stream unlimited bytes. */
int sbx_load(lua_State *L) {
    SandboxState *s = sb_from(L);
    (void)lua_tointeger(L, lua_upvalueindex(1));
    const char *chunkname = luaL_optstring(L, 2, "=(load)");
    /* The mode is not a script choice: text only.  Asking for binary is refused
     * outright instead of being silently downgraded. */
    const char *mode = luaL_optstring(L, 3, "t");
    if (mode != nullptr && std::strchr(mode, 'b') != nullptr) {
        return sb_raise_verbatim(L, Status::denied, "load", kCatBytecode,
                                 "binary chunks are not available in the sandbox",
                                 "LOADERR: binary chunks are not available in the sandbox (mode is 't')");
    }
    const int type = lua_type(L, 1);
    int status;
    if (type == LUA_TSTRING) {
        size_t len = 0;
        const char *text = lua_tolstring(L, 1, &len);
        if (len > 0 && text != nullptr && text[0] == LUA_SIGNATURE[0]) {
            return sb_raise_verbatim(L, Status::denied, "load", kCatBytecode,
                                     "attempt to load a binary chunk (mode is 't')",
                                     "LOADERR: attempt to load a binary chunk (mode is 't')");
        }
        if (len > s->max_source) {
            return sb_raisef(L, Status::limit, "limit", "load source exceeds the %zu-byte cap", s->max_source);
        }
        status = luaL_loadbufferx(L, (text != nullptr) ? text : "", len, chunkname, "t");
    } else if (type == LUA_TFUNCTION) {
        s->load_buffer.clear();
        for (int round = 0;; ++round) {
            if (round > 100000) {
                return sb_raisef(L, Status::limit, "limit",
                                 "the load reader function kept returning data past the %zu-byte cap",
                                 s->max_source);
            }
            lua_pushvalue(L, 1);
            lua_call(L, 0, 1);
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                break;
            }
            if (!lua_isstring(L, -1)) {
                lua_pop(L, 1);
                return luaL_error(L, "reader function must return a string");
            }
            size_t len = 0;
            const char *piece = lua_tolstring(L, -1, &len);
            if (s->load_buffer.empty() && len > 0 && piece != nullptr && piece[0] == LUA_SIGNATURE[0]) {
                lua_pop(L, 1);
                return sb_raise_verbatim(L, Status::denied, "load", kCatBytecode,
                                         "attempt to load a binary chunk (mode is 't')",
                                         "LOADERR: attempt to load a binary chunk (mode is 't')");
            }
            if (s->load_buffer.size() + len > s->max_source) {
                lua_pop(L, 1);
                return sb_raisef(L, Status::limit, "limit", "load source exceeds the %zu-byte cap", s->max_source);
            }
            s->load_buffer.append(piece, len);
            lua_pop(L, 1);
        }
        status = luaL_loadbufferx(L, s->load_buffer.data(), s->load_buffer.size(), chunkname, "t");
    } else {
        return luaL_typeerror(L, 1, "string or function");
    }

    if (status != LUA_OK) { /* error: message is on top of the stack */
        luaL_pushfail(L);
        lua_insert(L, -2);
        return 2;
    }
    /* Inject the environment: the caller's 4th argument when it is a table, the
     * run's _ENV otherwise.  A loaded chunk therefore cannot escape to the real
     * globals, and `load(s, n, "t", other_env)` still works. */
    if (!lua_isnoneornil(L, 4)) {
        lua_pushvalue(L, 4);
    } else if (s->env_ref >= 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, s->env_ref);
    } else {
        lua_pushnil(L);
    }
    if (lua_setupvalue(L, -2, 1) == nullptr) {
        lua_pop(L, 1);
    }
    return 1;
}

/* coroutine.resume with the sticky re-raise. */
int sbx_coresume(lua_State *L) {
    SandboxState *s = sb_from(L);
    (void)lua_tointeger(L, lua_upvalueindex(1));
    lua_State *co = lua_tothread(L, 1);
    if (co == nullptr) {
        return luaL_typeerror(L, 1, "coroutine");
    }
    const int narg = lua_gettop(L) - 1;
    if (narg > 0) {
        lua_xmove(L, co, narg);
    }
    int nres = 0;
    const int status = lua_resume(co, L, narg, &nres);
    if (s->sticky_kind != Status::ok) {
        return sb_sticky_reraise(L);
    }
    if (status == LUA_OK || status == LUA_YIELD) {
        if (nres > 0) {
            lua_xmove(co, L, nres);
        }
        lua_pushboolean(L, 1);
        lua_insert(L, -(nres + 1));
        return nres + 1;
    }
    if (lua_gettop(co) > 0) {
        lua_xmove(co, L, 1); /* the error object */
    } else {
        lua_pushliteral(L, "coroutine failed");
    }
    lua_pushboolean(L, 0);
    lua_insert(L, -2);
    return 2;
}

/* coroutine.close with the sticky re-raise. */
int sbx_coclose(lua_State *L) {
    SandboxState *s = sb_from(L);
    (void)lua_tointeger(L, lua_upvalueindex(1));
    lua_State *co = lua_tothread(L, 1);
    if (co == nullptr) {
        return luaL_typeerror(L, 1, "coroutine");
    }
    const int status = lua_closethread(co, L);
    if (s->sticky_kind != Status::ok) {
        return sb_sticky_reraise(L);
    }
    if (status == LUA_OK) {
        lua_pushboolean(L, 1);
        return 1;
    }
    if (lua_gettop(co) > 0) {
        lua_xmove(co, L, 1);
    } else {
        lua_pushliteral(L, "coroutine failed");
    }
    lua_pushboolean(L, 0);
    lua_insert(L, -2);
    return 2;
}

/* ------------------------------------------------------------------------- */
/* Tripwires                                                                 */
/* ------------------------------------------------------------------------- */

/* A tripwire FUNCTION (dofile / loadfile / require / loadlib / string.dump):
 * upvalues 1..4 = tag, api, category, detail. */
int sbx_tripwire_call(lua_State *L) {
    (void)lua_tointeger(L, lua_upvalueindex(1));
    const char *api = lua_tostring(L, lua_upvalueindex(2));
    const char *category = lua_tostring(L, lua_upvalueindex(3));
    const char *detail = lua_tostring(L, lua_upvalueindex(4));
    return sb_raise(L, Status::denied, api, category, detail);
}

/* The metatable __index/__newindex/__pairs of a tripwire TABLE (io / os /
 * package / debug): upvalues 1..2 = tag, table name.  The key at index 2 names
 * the exact member, so the message reads "DENIED: io.open -- ...". */
int sbx_tripwire_index(lua_State *L) {
    (void)lua_tointeger(L, lua_upvalueindex(1));
    const char *name = lua_tostring(L, lua_upvalueindex(2));
    const char *category = kCatProcess;
    const char *detail = kDetailProcess;
    char key_text[64];
    key_text[0] = '\0';
    const int key_type = lua_type(L, 2);
    if (key_type == LUA_TSTRING) {
        size_t len = 0;
        const char *key = lua_tolstring(L, 2, &len);
        if (len >= sizeof(key_text)) {
            len = sizeof(key_text) - 1;
        }
        std::memcpy(key_text, key, len);
        key_text[len] = '\0';
    } else if (key_type == LUA_TNUMBER) {
        std::snprintf(key_text, sizeof(key_text), "%lld", static_cast<long long>(lua_tointeger(L, 2)));
    } else {
        std::snprintf(key_text, sizeof(key_text), "?");
    }

    if (name != nullptr && std::strcmp(name, "io") == 0) {
        category = kCatFileIo;
        detail = kDetailFileIo;
    } else if (name != nullptr && std::strcmp(name, "package") == 0) {
        category = kCatModule;
        detail = kDetailModule;
    } else if (name != nullptr && std::strcmp(name, "debug") == 0) {
        category = kCatIntrospection;
        detail = kDetailDebug;
    } else if (name != nullptr && std::strcmp(name, "os") == 0) {
        category = kCatProcess;
        detail = kDetailProcess;
        if (std::strcmp(key_text, "exit") == 0) {
            detail = "terminating the host process is not available in the sandbox";
        } else if (std::strcmp(key_text, "getenv") == 0) {
            category = kCatEnv;
            detail = "reading the environment is not available in the sandbox";
        } else if (std::strcmp(key_text, "setlocale") == 0) {
            category = kCatEnv;
            detail = "changing the locale is not available in the sandbox";
        } else if (std::strcmp(key_text, "time") == 0 || std::strcmp(key_text, "date") == 0 ||
                   std::strcmp(key_text, "clock") == 0 || std::strcmp(key_text, "difftime") == 0) {
            category = kCatTime;
            detail = "reading the clock is not available in the sandbox";
        }
    }
    lua_pushfstring(L, "%s.%s", (name != nullptr) ? name : "?", key_text);
    /* The api string is on the stack, so the pointer stays valid for the copy
     * sb_raise makes before it longjmps. */
    return sb_raise(L, Status::denied, lua_tostring(L, -1), category, detail);
}

/* __tostring of a tripwire table: "io (blocked by sandbox)".  This is what makes
 * string.format('%s', io) informative instead of an address. */
int sbx_tripwire_tostring(lua_State *L) {
    (void)lua_tointeger(L, lua_upvalueindex(1));
    const char *name = lua_tostring(L, lua_upvalueindex(2));
    lua_pushfstring(L, "%s (blocked by sandbox)", (name != nullptr) ? name : "?");
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Environment construction helpers                                          */
/* ------------------------------------------------------------------------- */

void sbx_set_closure(lua_State *L, int env, const char *name, lua_CFunction fn, int32_t tag) {
    lua_pushinteger(L, tag);
    lua_pushcclosure(L, fn, k_primitive_upvalues);
    lua_setfield(L, env, name);
}

void sbx_set_tripwire_fn(lua_State *L, int env, const char *name, const char *api, const char *category,
                         const char *detail) {
    lua_pushinteger(L, kTagTripwireFunction);
    lua_pushstring(L, api);
    lua_pushstring(L, category);
    lua_pushstring(L, detail);
    lua_pushcclosure(L, &sbx_tripwire_call, 4);
    lua_setfield(L, env, name);
}

void sbx_set_tripwire_table(lua_State *L, int env, const char *name, const char *category, const char *detail) {
    lua_newtable(L); /* the tripwire table */
    const int table = lua_gettop(L);

    /* Two readable members, so a host extension can introspect a tripwire table
     * without triggering the raise.  They are set BEFORE the metatable exists:
     * once __newindex is in place every write into this table raises. */
    lua_pushstring(L, detail);
    lua_setfield(L, table, "__detail");
    lua_pushstring(L, category);
    lua_setfield(L, table, "__category");

    lua_newtable(L); /* its metatable */
    const int mt = lua_gettop(L);
    static const char *const kRaisers[] = {"__index", "__newindex", "__pairs", nullptr};
    for (int i = 0; kRaisers[i] != nullptr; ++i) {
        lua_pushinteger(L, kTagTripwireIndex);
        lua_pushstring(L, name);
        lua_pushcclosure(L, &sbx_tripwire_index, 2);
        lua_setfield(L, mt, kRaisers[i]);
    }
    lua_pushinteger(L, kTagTripwireTostring);
    lua_pushstring(L, name);
    lua_pushcclosure(L, &sbx_tripwire_tostring, 2);
    lua_setfield(L, mt, "__tostring");
    lua_pushstring(L, name);
    lua_setfield(L, mt, "__name");
    lua_setmetatable(L, table);
    lua_setfield(L, env, name);
}

/* Wrap the stock base function `name` (taken from the REAL globals table, which
 * is never part of the environment) and install it into the env under the same
 * name.  Upvalue 2 keeps the stock implementation for the paths that delegate. */
void sbx_wrap_global(lua_State *L, int env, int globals, const char *name, lua_CFunction fn, int32_t tag) {
    lua_pushinteger(L, tag);
    lua_getfield(L, globals, name); /* upvalue 2: the stock implementation */
    lua_pushcclosure(L, fn, 2);
    lua_setfield(L, env, name);
}

/* Same, for a member of a library table the environment already carries
 * (string.format): the stock implementation comes from that very table. */
void sbx_wrap_lib(lua_State *L, int env, const char *lib, const char *field, lua_CFunction fn, int32_t tag) {
    lua_getfield(L, env, lib);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    const int owner = lua_gettop(L);
    lua_pushinteger(L, tag);
    lua_getfield(L, owner, field);
    lua_pushcclosure(L, fn, 2);
    lua_setfield(L, owner, field);
    lua_pop(L, 1);
}

/* A tripwire that replaces a library member (string.dump). */
void sbx_tripwire_lib_fn(lua_State *L, int env, const char *lib, const char *field, const char *api,
                         const char *category, const char *detail) {
    lua_getfield(L, env, lib);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    const int owner = lua_gettop(L);
    lua_pushinteger(L, kTagStringDump);
    lua_pushstring(L, api);
    lua_pushstring(L, category);
    lua_pushstring(L, detail);
    lua_pushcclosure(L, &sbx_tripwire_call, 4);
    lua_setfield(L, owner, field);
    lua_pop(L, 1);
}

/* ------------------------------------------------------------------------- */
/* Reserved names                                                            */
/* ------------------------------------------------------------------------- */

/* EVERY name sb_env_build installs into _ENV, in one table -- it is the only
 * source of truth behind sb_env_name_reserved.  A host must not bind or set one
 * of these (Sandbox::bind_host_function / set_global return false):
 *
 *   - binding over a REPLACE/tripwire name makes sb_env_verify report
 *     Status::behaviour ("a sandbox primitive was replaced"), so it poisons
 *     every later run of that Sandbox;
 *   - set_global("print", nil) would delete the capability outright.
 *
 * Both are exactly what invariant R3 ("no capability deleted") forbids, which is
 * why the check lives at host time (a plain false, no raise) instead of being
 * detected afterwards by the integrity post-check.
 *
 * UPDATE THIS TABLE whenever a name is added to (or removed from) the
 * environment built below.  test_script_host enumerates the built environment
 * from inside the sandbox and asserts that every reachable name is reserved
 * here, so forgetting an entry is a red suite rather than an open hole. */
const char *const kEnvReservedNames[] = {
    /* ALLOW: the base functions copied from the real globals table. */
    "assert", "error", "ipairs", "pairs", "next", "select", "tonumber", "type",
    "rawget", "rawset", "rawequal", "rawlen", "setmetatable", "getmetatable", "_VERSION",
    /* ALLOW: the safe library tables, plus coroutine when it is enabled. */
    "string", "table", "math", "utf8", "coroutine",
    /* REPLACE: the vetted wrappers (and the stock functions they wrap). */
    "load", "pcall", "xpcall", "print", "warn", "tostring", "collectgarbage",
    /* DENY: the tripwire tables and the tripwire functions. */
    "io", "os", "package", "debug", "require", "loadlib", "dofile", "loadfile",
    /* The environment's own self-reference. */
    "_G",
    nullptr,
};

/* Byte-exact membership test over a NULL-terminated name table. */
bool sbx_name_listed(const char *const *names, kimix::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (int i = 0; names[i] != nullptr; ++i) {
        const kimix::string_view entry(names[i]);
        if (entry.size() == name.size() && std::memcmp(entry.data(), name.data(), name.size()) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace src_script_script_env
using namespace src_script_script_env;

/* ------------------------------------------------------------------------- */
/* Public entry points                                                       */
/* ------------------------------------------------------------------------- */

bool sb_push_text(lua_State *L, SandboxState *s, int idx) {
    if (idx < 0) {
        idx = lua_gettop(L) + idx + 1;
    }
    if (idx <= 0) {
        lua_pushliteral(L, "nil");
        return true;
    }
    sbx_push_redacted(L, s, idx);
    return true;
}

bool sb_env_name_reserved(kimix::string_view name) noexcept { return sbx_name_listed(kEnvReservedNames, name); }

void sb_env_build(lua_State *L, SandboxState *s, const kimix::vector<SandboxNative> &natives, bool gc_count) {
    s->gc_count = gc_count;
    if (!lua_checkstack(L, 16)) {
        sb_raisef(L, Status::internal, "internal", "the interpreter stack is exhausted");
        return; /* not reached */
    }

    /* A fresh redaction-id table per run, so "table: #1" is reproducible. */
    lua_pushlightuserdata(L, sb_key_addr());
    lua_newtable(L);
    lua_rawset(L, LUA_REGISTRYINDEX);

    lua_newtable(L); /* the environment */
    const int env = lua_gettop(L);

    /* 1. ALLOW: the safe base functions and the four safe library tables.
     * Steps 1-3 install exactly the names listed in kEnvReservedNames above --
     * add a name AND its table entry together. */
    static const char *const kCopy[] = {"assert",  "error",    "ipairs",   "pairs",     "next",
                                        "select",  "tonumber", "type",     "rawget",    "rawset",
                                        "rawequal", "rawlen",  "setmetatable", "getmetatable",
                                        "_VERSION", nullptr};
    lua_pushglobaltable(L);
    const int globals = lua_gettop(L);
    for (int i = 0; kCopy[i] != nullptr; ++i) {
        lua_getfield(L, globals, kCopy[i]);
        lua_setfield(L, env, kCopy[i]);
    }
    static const char *const kLibs[] = {"string", "table", "math", "utf8", nullptr};
    for (int i = 0; kLibs[i] != nullptr; ++i) {
        lua_getfield(L, globals, kLibs[i]);
        lua_setfield(L, env, kLibs[i]);
    }
    if (s->coroutines) { /* only reachable when the mask opened LUA_COLIBK */
        lua_getfield(L, globals, "coroutine");
        lua_setfield(L, env, "coroutine");
    }
    /* 2. REPLACE: the vetted wrappers. */
    sbx_set_closure(L, env, "load", &sbx_load, kTagLoad);
    sbx_set_closure(L, env, "pcall", &sbx_pcall, kTagPcall);
    sbx_set_closure(L, env, "xpcall", &sbx_xpcall, kTagXpcall);
    sbx_set_closure(L, env, "print", &sbx_print, kTagPrint);
    sbx_set_closure(L, env, "warn", &sbx_warn, kTagWarn);
    sbx_set_closure(L, env, "tostring", &sbx_tostring, kTagTostring);
    /* The stock implementations of these two are NOT copied into the env, so
     * they are taken from the real globals table instead. */
    sbx_wrap_global(L, env, globals, "collectgarbage", &sbx_collectgarbage, kTagCollectgarbage);
    sbx_wrap_lib(L, env, "string", "format", &sbx_format, kTagFormat);
    /* string.dump is a tripwire: bytecode may never leave the state. */
    sbx_tripwire_lib_fn(L, env, "string", "dump", "string.dump", kCatBytecode, kDetailBytecode);
    /* The vendored interpreter can add build-dependent helpers to its
     * libraries: KIMIX_LUA_ARENA adds table.new / table.shrink (see
     * docs/lua_memory_arena.md). A sandbox surface must not vary with a
     * compile-time switch, and a script gains nothing from two memory-shaping
     * hints while the budgeted allocator is the layer that decides footprint,
     * so they are dropped here. The capability matrix, the golden surface and
     * every hostile test then read the same whether the arena is built in or
     * not. */
    lua_getfield(L, env, "table");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, -2, "new");
        lua_pushnil(L);
        lua_setfield(L, -2, "shrink");
    }
    lua_pop(L, 1);
    lua_pop(L, 1); /* globals */

    if (s->coroutines) {
        lua_getfield(L, env, "coroutine");
        if (lua_istable(L, -1)) {
            const int co = lua_gettop(L);
            lua_pushinteger(L, kTagCoroutineResume);
            lua_pushcclosure(L, &sbx_coresume, k_primitive_upvalues);
            lua_setfield(L, co, "resume");
            lua_pushinteger(L, kTagCoroutineClose);
            lua_pushcclosure(L, &sbx_coclose, k_primitive_upvalues);
            lua_setfield(L, co, "close");
        }
        lua_pop(L, 1);
    }

    /* 3. DENY: the loud tripwires. */
    sbx_set_tripwire_table(L, env, "io", kCatFileIo, kDetailFileIo);
    sbx_set_tripwire_table(L, env, "os", kCatProcess, kDetailProcess);
    sbx_set_tripwire_table(L, env, "package", kCatModule, kDetailModule);
    sbx_set_tripwire_table(L, env, "debug", kCatIntrospection, kDetailDebug);
    sbx_set_tripwire_fn(L, env, "require", "require", kCatModule, kDetailModule);
    sbx_set_tripwire_fn(L, env, "loadlib", "loadlib", kCatModule, kDetailModule);
    sbx_set_tripwire_fn(L, env, "dofile", "dofile", kCatFileIo, kDetailLoadFile);
    sbx_set_tripwire_fn(L, env, "loadfile", "loadfile", kCatFileIo, kDetailLoadFile);

    /* 4. Host-provided natives. */
    for (const SandboxNative &native : natives) {
        const int upvalues = (native.upvalues > 0) ? native.upvalues : 0;
        for (int i = 0; i < upvalues; ++i) {
            lua_pushnil(L); /* the host fills them from its own C function */
        }
        lua_pushcclosure(L, native.fn, upvalues);
        lua_setfield(L, env, native.name.c_str());
    }

    /* 4b. Host-function bindings (Sandbox::bind_host_function).  What the script
     * sees is ALWAYS sb_host_trampoline, with exactly two upvalues:
     *   [1] light userdata -- the host's `user` pointer (never dereferenced here)
     *   [2] integer         -- the binding's index in s->host_bindings
     * The index has to address the very vector the trampoline resolves it in
     * (where it also re-checks `host` and `user`), so this walks host_bindings
     * rather than the add_native list above.  Upvalues are unreachable from
     * script code: there is no debug library and no string.dump. */
    if (s->host_bindings != nullptr) {
        for (size_t i = 0; i < s->host_bindings->size(); ++i) {
            const SandboxNative &binding = (*s->host_bindings)[i];
            if (!binding.host) {
                continue;
            }
            lua_pushlightuserdata(L, binding.user);
            lua_pushinteger(L, static_cast<lua_Integer>(i));
            lua_pushcclosure(L, &sb_host_trampoline, k_host_binding_upvalues);
            lua_setfield(L, env, binding.name.c_str());
        }
    }

    /* 5. _G is the environment itself -- the real globals table stays
     * unreachable from script code. */
    lua_pushvalue(L, env);
    lua_setfield(L, env, "_G");

    /* 6. Deterministic math.random. */
    lua_getfield(L, env, "math");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "randomseed");
        if (lua_isfunction(L, -1)) {
            lua_pushinteger(L, static_cast<lua_Integer>(s->random_seed));
            lua_call(L, 1, 0);
        } else {
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    /* Pin the environment in the registry: `load` and the integrity check read
     * it back, and the bootstrap sets the chunk's _ENV from it. */
    if (s->env_ref >= 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, s->env_ref);
        s->env_ref = -2;
    }
    lua_pushvalue(L, env);
    s->env_ref = luaL_ref(L, LUA_REGISTRYINDEX);
}

namespace src_script_script_env {

bool sbx_check_closure(lua_State *L, int idx, lua_CFunction fn, int32_t tag) {
    if (lua_type(L, idx) != LUA_TFUNCTION) {
        return false;
    }
    if (lua_tocfunction(L, idx) != fn) {
        return false;
    }
    lua_pushvalue(L, idx);
    const char *name = lua_getupvalue(L, -1, 1); /* pushes the upvalue value */
    bool ok = false;
    if (name != nullptr) {
        ok = lua_isinteger(L, -1) && lua_tointeger(L, -1) == static_cast<lua_Integer>(tag);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return ok;
}

bool sbx_check_prim(lua_State *L, int env, const char *name, lua_CFunction fn, int32_t tag) {
    lua_getfield(L, env, name);
    const bool ok = sbx_check_closure(L, -1, fn, tag);
    lua_pop(L, 1);
    return ok;
}

bool sbx_check_tripwire_table(lua_State *L, int env, const char *name) {
    if (lua_getfield(L, env, name) != LUA_TTABLE) {
        lua_pop(L, 1);
        return false;
    }
    const int table = lua_gettop(L);
    bool ok = false;
    if (lua_getmetatable(L, table)) {
        lua_getfield(L, -1, "__index");
        ok = sbx_check_closure(L, -1, &sbx_tripwire_index, kTagTripwireIndex);
        lua_pop(L, 2);
    }
    lua_pop(L, 1);
    return ok;
}

bool sbx_check_tripwire_fn(lua_State *L, int env, const char *name) {
    lua_getfield(L, env, name);
    const bool ok = sbx_check_closure(L, -1, &sbx_tripwire_call, kTagTripwireFunction);
    lua_pop(L, 1);
    return ok;
}

} // namespace src_script_script_env
using namespace src_script_script_env;

bool sb_env_verify(lua_State *L, SandboxState *s) {
    if (s->env_ref < 0) {
        return false;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, s->env_ref);
    const int env = lua_gettop(L);
    bool ok = lua_istable(L, env);

    /* A script that gives the environment a metatable could intercept every
     * later lookup -- that alone is a behaviour failure. */
    if (ok && lua_getmetatable(L, env)) {
        lua_pop(L, 1);
        ok = false;
    }
    if (ok) {
        ok = sbx_check_prim(L, env, "load", &sbx_load, kTagLoad) &&
             sbx_check_prim(L, env, "pcall", &sbx_pcall, kTagPcall) &&
             sbx_check_prim(L, env, "xpcall", &sbx_xpcall, kTagXpcall) &&
             sbx_check_prim(L, env, "print", &sbx_print, kTagPrint) &&
             sbx_check_prim(L, env, "warn", &sbx_warn, kTagWarn) &&
             sbx_check_prim(L, env, "tostring", &sbx_tostring, kTagTostring);
    }
    if (ok) {
        lua_getfield(L, env, "string");
        if (lua_istable(L, -1)) {
            const int str_lib = lua_gettop(L);
            lua_getfield(L, str_lib, "format");
            ok = sbx_check_closure(L, -1, &sbx_format, kTagFormat);
            lua_pop(L, 1);
            lua_getfield(L, str_lib, "dump");
            ok = ok && sbx_check_closure(L, -1, &sbx_tripwire_call, kTagStringDump);
            lua_pop(L, 1);
        } else {
            ok = false;
        }
        lua_pop(L, 1);
    }
    if (ok) {
        ok = sbx_check_tripwire_table(L, env, "io") && sbx_check_tripwire_table(L, env, "os") &&
             sbx_check_tripwire_table(L, env, "package") && sbx_check_tripwire_table(L, env, "debug") &&
             sbx_check_tripwire_fn(L, env, "require") && sbx_check_tripwire_fn(L, env, "dofile") &&
             sbx_check_tripwire_fn(L, env, "loadfile") && sbx_check_tripwire_fn(L, env, "loadlib");
    }
    if (ok) { /* _G must still be the environment itself */
        lua_getfield(L, env, "_G");
        ok = lua_istable(L, -1) && lua_rawequal(L, -1, env);
        lua_pop(L, 1);
    }
    lua_pop(L, 1); /* env */

    if (!ok) {
        s->sticky_kind = Status::behaviour;
        s->violation.kind = Status::behaviour;
        s->violation.category.assign("integrity");
        s->violation.detail.assign("a sandbox primitive was replaced or the environment was given a metatable");
        s->violation.chunk.assign(s->chunk_name);
        sb_compose_body(s->body, sizeof(s->body), Status::behaviour, nullptr,
                        "a sandbox primitive was replaced by the script");
        s->sticky_msg.assign(s->body, std::strlen(s->body));
    }
    return ok;
}

} // namespace kimix::script
