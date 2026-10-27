/*
 * script_value.cpp -- the outbound boundary (L5): turning a chunk's return
 * values into `Value`s without letting anything unbounded or addressable cross.
 *
 * RULES ENFORCED HERE
 * -------------------
 *   - nil / boolean / integer / number / string are copied by value.
 *   - A table becomes CANONICAL JSON: minified, object keys sorted by byte
 *     order, arrays kept as arrays, numbers printed by yyjson (integers exact).
 *     The walk is depth-capped, byte-capped and CYCLE-SAFE (the visited path is
 *     kept in the state, not on the C stack, so a self-referencing table cannot
 *     hang the native side).
 *   - function / thread / userdata are REFUSED with Status::limit.  That is the
 *     outbound half of the pointer-control rule: a Value never carries a
 *     pointer, so the native side can never call back into the interpreter.
 *
 * WHY NOTHING IS KEPT ON THE STACK
 * --------------------------------
 * The walk can raise (LIMIT).  A raise is a longjmp, so every scratch buffer
 * lives in SandboxState (json_path / json_key_stack / json_buffer / json_detail) and
 * the failing path only records a message and returns NULL upwards; the raise
 * itself happens once, after the yyjson document has been freed and no C++
 * object with a destructor is alive.
 */
#include <script/script_value.h>

#include <algorithm>
#include <cstdio>

#include <mimalloc.h>
#include <yyjson.h>

#include <script/script_error.h>
#include <script/script_lua.h>
#include <script/script_state.h>

namespace kimix::script {

namespace src_script_script_value {

/* One heap: the JSON document and its output buffer are mimalloc memory, the
 * same heap the kimix containers use (kimix-core.dll). */
void *sb_yy_malloc(void *ctx, size_t size) {
    (void)ctx;
    return mi_malloc(size);
}
void *sb_yy_realloc(void *ctx, void *ptr, size_t old_size, size_t size) {
    (void)ctx;
    (void)old_size;
    return mi_realloc(ptr, size);
}
void sb_yy_free(void *ctx, void *ptr) {
    (void)ctx;
    mi_free(ptr);
}

const yyjson_alc kSbYyAlc = {sb_yy_malloc, sb_yy_realloc, sb_yy_free, nullptr};

/* A non-standard table would need flags the JSON writer does not have, so:
 * invalid UTF-8 in a Lua string is copied byte by byte, and inf/nan become
 * null (a Value must always be valid JSON). */
constexpr yyjson_write_flag kSbWriteFlags =
    YYJSON_WRITE_ALLOW_INVALID_UNICODE | YYJSON_WRITE_INF_AND_NAN_AS_NULL;

/* Record a failure (first one wins) and answer false; never raises. */
bool sb_json_fail(SandboxState *s, const char *text) {
    if (!s->json_failed) {
        s->json_failed = true;
        std::snprintf(s->json_detail, sizeof(s->json_detail), "%s", text);
    }
    return false;
}

/* Count one node against the byte cap: a JSON node is at least a few bytes on
 * the wire, so bounding the node count bounds the walk's memory too. */
bool sb_json_charge(SandboxState *s) {
    const size_t node_cap = (s->max_result_bytes / 8) + 16;
    if (++s->json_nodes > node_cap) {
        return sb_json_fail(s, "result table exceeds the sandbox result cap");
    }
    return true;
}

bool sb_json_path_contains(const SandboxState *s, const void *table) {
    for (const void *entry : s->json_path) {
        if (entry == table) {
            return true;
        }
    }
    return false;
}

yyjson_mut_val *sb_json_value(lua_State *L, SandboxState *s, yyjson_mut_doc *doc, int idx, int depth);

/* A Lua table as a JSON array (1..n all present) or object (sorted keys). */
yyjson_mut_val *sb_json_table(lua_State *L, SandboxState *s, yyjson_mut_doc *doc, int idx, int depth) {
    if (static_cast<uint32_t>(depth) > s->json_depth_cap) {
        char text[128];
        std::snprintf(text, sizeof(text), "result table exceeds the %u-level nesting cap",
                      static_cast<unsigned>(s->json_depth_cap));
        sb_json_fail(s, text);
        return nullptr;
    }
    const void *identity = lua_topointer(L, idx);
    if (identity == nullptr || sb_json_path_contains(s, identity)) {
        sb_json_fail(s, "cyclic result table cannot cross the sandbox boundary");
        return nullptr;
    }
    if (!lua_checkstack(L, 6)) {
        sb_json_fail(s, "result table exceeds the interpreter stack");
        return nullptr;
    }
    s->json_path.push_back(identity);

    const lua_Unsigned array_len = lua_rawlen(L, idx);
    bool is_array = array_len > 0;
    for (lua_Unsigned i = 1; is_array && i <= array_len; ++i) {
        if (lua_rawgeti(L, idx, static_cast<lua_Integer>(i)) == LUA_TNIL) {
            is_array = false;
        }
        lua_pop(L, 1);
    }

    yyjson_mut_val *result = nullptr;
    if (is_array) {
        result = yyjson_mut_arr(doc);
        for (lua_Unsigned i = 1; result != nullptr && i <= array_len; ++i) {
            lua_rawgeti(L, idx, static_cast<lua_Integer>(i));
            yyjson_mut_val *child = sb_json_value(L, s, doc, lua_gettop(L), depth + 1);
            lua_pop(L, 1);
            if (child == nullptr || !sb_json_charge(s) || !yyjson_mut_arr_append(result, child)) {
                if (child != nullptr && !s->json_failed) {
                    sb_json_fail(s, "result table exceeds the sandbox result cap");
                }
                result = nullptr;
            }
        }
    } else {
        /* Collect the keys first so the object can be emitted in canonical
         * (byte-sorted) order -- yyjson keeps insertion order.  The list belongs
         * to THIS nesting level (json_key_stack[depth]): the children walked
         * below collect their own keys one level deeper. */
        kimix::vector<kimix::string> &keys = s->json_key_stack[depth];
        keys.clear();
        bool keys_ok = true;
        lua_pushnil(L);
        while (lua_next(L, idx) != 0) {
            /* key at -2, value at -1 */
            const int key_type = lua_type(L, -2);
            if (key_type == LUA_TSTRING) {
                size_t len = 0;
                const char *text = lua_tolstring(L, -2, &len);
                kimix::string key;
                key.assign(text, len);
                keys.push_back(static_cast<kimix::string &&>(key));
            } else if (key_type == LUA_TNUMBER) {
                char text[48];
                int written = 0;
                if (lua_isinteger(L, -2)) {
                    written = std::snprintf(text, sizeof(text), "%lld",
                                            static_cast<long long>(lua_tointeger(L, -2)));
                } else {
                    written = std::snprintf(text, sizeof(text), "%.17g", static_cast<double>(lua_tonumber(L, -2)));
                }
                kimix::string key;
                key.assign(text, (written > 0) ? static_cast<size_t>(written) : 0);
                keys.push_back(static_cast<kimix::string &&>(key));
            } else if (key_type == LUA_TBOOLEAN) {
                keys.push_back(lua_toboolean(L, -2) != 0 ? "true" : "false");
            } else {
                keys_ok = false;
                char text[128];
                std::snprintf(text, sizeof(text), "result table key of type '%s' cannot cross the sandbox boundary",
                              lua_typename(L, key_type));
                sb_json_fail(s, text);
                lua_pop(L, 2); /* value + key */
                break;
            }
            lua_pop(L, 1); /* value; keep the key for lua_next */
        }
        if (keys_ok) {
            std::sort(keys.begin(), keys.end());
            result = yyjson_mut_obj(doc);
            for (size_t i = 0; result != nullptr && i < keys.size(); ++i) {
                const kimix::string &key = keys[i];
                lua_pushlstring(L, key.data(), key.size());
                const int value_type = lua_rawget(L, idx);
                yyjson_mut_val *child = sb_json_value(L, s, doc, lua_gettop(L), depth + 1);
                lua_pop(L, 1);
                /* strncpy, not strn: yyjson_mut_strn does NOT copy the bytes and
                 * this buffer is a scratch list that does not outlive the walk. */
                if (child == nullptr || !sb_json_charge(s) ||
                    !yyjson_mut_obj_add(result, yyjson_mut_strncpy(doc, key.data(), key.size()), child)) {
                    if (child != nullptr && !s->json_failed) {
                        sb_json_fail(s, "result table exceeds the sandbox result cap");
                    }
                    result = nullptr;
                }
                (void)value_type;
            }
        }
    }

    s->json_path.pop_back();
    return s->json_failed ? nullptr : result;
}

yyjson_mut_val *sb_json_value(lua_State *L, SandboxState *s, yyjson_mut_doc *doc, int idx, int depth) {
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        return yyjson_mut_null(doc);
    case LUA_TBOOLEAN:
        return yyjson_mut_bool(doc, lua_toboolean(L, idx) != 0);
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx)) {
            return yyjson_mut_sint(doc, static_cast<int64_t>(lua_tointeger(L, idx)));
        }
        return yyjson_mut_real(doc, static_cast<double>(lua_tonumber(L, idx)));
    case LUA_TSTRING: {
        size_t len = 0;
        const char *text = lua_tolstring(L, idx, &len);
        return yyjson_mut_strn(doc, (text != nullptr) ? text : "", len);
    }
    case LUA_TTABLE:
        return sb_json_table(L, s, doc, idx, depth);
    default: {
        char text[128];
        std::snprintf(text, sizeof(text), "result value of type '%s' cannot cross the sandbox boundary",
                      lua_typename(L, lua_type(L, idx)));
        sb_json_fail(s, text);
        return nullptr;
    }
    }
}

/* Emit the document into state->json_buffer, capped at max_result_bytes. */
bool sb_json_write(SandboxState *s, yyjson_mut_doc *doc) {
    size_t cap = s->json_buffer.capacity();
    if (cap < 64) {
        cap = 64;
    }
    const size_t cap_limit = s->max_result_bytes;
    for (;;) {
        if (cap > cap_limit + 1) {
            char text[128];
            std::snprintf(text, sizeof(text), "result table exceeds the %zu-byte result cap", cap_limit);
            return sb_json_fail(s, text);
        }
        s->json_buffer.resize(cap);
        const size_t written = yyjson_mut_write_buf(s->json_buffer.data(), cap, doc, kSbWriteFlags, nullptr);
        if (written != 0) {
            s->json_buffer.resize(written);
            return true;
        }
        s->json_buffer.resize(0);
        cap = cap + cap / 2 + 16;
    }
}

/* Build the canonical JSON of the table at `idx` into state->json_buffer. */
bool sb_json_build_table(lua_State *L, SandboxState *s, int idx) {
    s->json_failed = false;
    s->json_detail[0] = '\0';
    s->json_nodes = 0;
    s->json_path.clear();
    s->json_buffer.clear();
    /* A stack bound, not corruption: past 64 levels the message is a LIMIT. */
    s->json_depth_cap = (s->max_result_depth > 64) ? 64 : s->max_result_depth;
    s->json_key_stack.resize(static_cast<size_t>(s->json_depth_cap) + 2);
    for (kimix::vector<kimix::string> &keys : s->json_key_stack) {
        keys.clear();
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(&kSbYyAlc);
    if (doc == nullptr) {
        return sb_json_fail(s, "the sandbox could not allocate the result document");
    }
    yyjson_mut_val *root = sb_json_value(L, s, doc, idx, 0);
    if (root == nullptr || s->json_failed) {
        yyjson_mut_doc_free(doc);
        return false;
    }
    yyjson_mut_doc_set_root(doc, root);
    const bool ok = sb_json_write(s, doc);
    yyjson_mut_doc_free(doc);
    return ok;
}

} // namespace src_script_script_value
using namespace src_script_script_value;

void Value::clear() noexcept {
    kind = Kind::nil;
    b = false;
    i = 0;
    d = 0.0;
    s.clear();
}

const char *Value::kind_name(Kind k) noexcept {
    switch (k) {
    case Kind::nil: return "nil";
    case Kind::boolean: return "boolean";
    case Kind::integer: return "integer";
    case Kind::number: return "number";
    case Kind::string: return "string";
    case Kind::table: return "table";
    }
    return "nil";
}

bool sb_marshal_results(lua_State *L, SandboxState *s, int first, int count) {
    if (count < 0) {
        count = 0;
    }
    if (static_cast<uint32_t>(count) > s->max_result_values) {
        sb_raisef(L, Status::limit, "limit", "the chunk returned %d value(s); the sandbox allows %u", count,
                  static_cast<unsigned>(s->max_result_values));
        return false; /* not reached: sb_raisef raises */
    }
    for (int i = 0; i < count; ++i) {
        const int idx = first + i;
        const int type = lua_type(L, idx);
        if (type == LUA_TFUNCTION || type == LUA_TTHREAD || type == LUA_TUSERDATA || type == LUA_TLIGHTUSERDATA) {
            /* No C++ object is alive here, so the raise below is safe. */
            sb_raisef(L, Status::limit, "limit", "result value of type '%s' cannot cross the sandbox boundary",
                      lua_typename(L, type));
            return false; /* not reached: sb_raisef raises */
        }
        if (type == LUA_TTABLE) {
            if (!sb_json_build_table(L, s, idx)) {
                sb_raise(L, Status::limit, nullptr, "limit", s->json_detail);
                return false; /* not reached: sb_raise raises */
            }
            Value value;
            value.kind = Value::Kind::table;
            value.s.assign(s->json_buffer.data(), s->json_buffer.size());
            s->results.push_back(static_cast<Value &&>(value));
            continue;
        }
        Value value;
        switch (type) {
        case LUA_TNIL:
            value.kind = Value::Kind::nil;
            break;
        case LUA_TBOOLEAN:
            value.kind = Value::Kind::boolean;
            value.b = lua_toboolean(L, idx) != 0;
            break;
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) {
                value.kind = Value::Kind::integer;
                value.i = static_cast<int64_t>(lua_tointeger(L, idx));
                value.d = static_cast<double>(value.i);
            } else {
                value.kind = Value::Kind::number;
                value.d = static_cast<double>(lua_tonumber(L, idx));
            }
            break;
        case LUA_TSTRING: {
            size_t len = 0;
            const char *text = lua_tolstring(L, idx, &len);
            value.kind = Value::Kind::string;
            value.s.assign((text != nullptr) ? text : "", (text != nullptr) ? len : 0);
            break;
        }
        default:
            /* Unreachable: the type switch above covered everything else. */
            value.kind = Value::Kind::nil;
            break;
        }
        s->results.push_back(static_cast<Value &&>(value));
    }
    return true;
}

} // namespace kimix::script
