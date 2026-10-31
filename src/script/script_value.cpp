/*
 * script_value.cpp -- the boundary value layer (L5), in both directions:
 *
 *   outbound  a chunk's return values / a host function's arguments become
 *             `Value`s without letting anything unbounded or addressable cross;
 *   inbound   a host's `Value::Kind::table` (canonical JSON text) is
 *             materialised back into a real Lua table (phase N3).
 *
 * The two directions live in ONE FILE on purpose: one `yyjson_alc` (the
 * mimalloc-backed kSbYyAlc below), one set of caps, one node budget, so the text
 * the writer emits is exactly the text the reader accepts and the two cannot
 * drift apart.
 *
 * RULES ENFORCED HERE (outbound)
 * ------------------------------
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
 * RULES ENFORCED HERE (inbound)
 * -----------------------------
 *   - The text is parsed with YYJSON_READ_NOFLAG -- strict: no comments, no
 *     NaN/Infinity, no single quotes, no trailing comma, no trailing garbage --
 *     and through kSbYyAlc, so the document is mimalloc memory like everything
 *     else this module allocates.
 *   - A validating walk then checks every cap BEFORE a single Lua value is built:
 *     the text against max_result_bytes, the node count against the writer's
 *     budget (max_result_bytes / 8 + 16), the nesting depth against
 *     max_result_depth (and kSbJsonDepthLimit, the native-recursion bound), one
 *     string against max_string_bytes, and DUPLICATE OBJECT KEYS, which are
 *     refused exactly (canonical JSON never has one).  The push walk that follows
 *     therefore runs on a tree that cannot violate a cap: it can only fail on the
 *     interpreter's own limits, and it charges the Lua memory budget up front.
 *   - `null` -> Lua nil (documented in script_value.h and src/script/README.md).
 *
 * WHY NOTHING IS KEPT ON THE STACK
 * --------------------------------
 * The outbound walk can raise (LIMIT).  A raise is a longjmp, so every scratch
 * buffer lives in SandboxState (json_path / json_key_stack / json_buffer /
 * json_detail) and the failing path only records a message and returns NULL
 * upwards; the raise itself happens once, after the yyjson document has been
 * freed and no C++ object with a destructor is alive.  The inbound walk uses the
 * same state-owned key stack for the same reason (its only Lua calls come after
 * the document is validated, and the interpreter memory it needs was charged
 * against the budget beforehand -- see sb_json_to_lua).
 */
#include <script/script_value.h>
#include <core/memory.h>

#include <algorithm>
#include <cstdio>
#include <memory>

#include <yyjson.h>

#include <script/script_error.h>
#include <script/script_lua.h>
#include <script/script_state.h>

namespace kimix::script {

namespace src_script_script_value {

/* The JSON document and its output buffer use the same mimalloc heap as this
 * module's kimix containers (per-image standalone, process-wide external). */
void *sb_yy_malloc(void *ctx, size_t size) {
    (void)ctx;
    return kimix::allocate(size);
}
void *sb_yy_realloc(void *ctx, void *ptr, size_t old_size, size_t size) {
    (void)ctx;
    (void)old_size;
    return kimix::reallocate(ptr, size);
}
void sb_yy_free(void *ctx, void *ptr) {
    (void)ctx;
    kimix::deallocate(ptr);
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

/* ============================= the reader ================================
 * Host -> script.  Same file, same allocator, same caps as the writer above. */

/* RAII owner of a parsed document.  yyjson_doc is a C object, so it gets an
 * EXPLICIT deleter (the house rule) instead of `delete`: yyjson_doc_free returns
 * the pools to the alc the document was created with, i.e. to mimalloc. */
using SbJsonDocPtr = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>;

/* The scratch of ONE validating walk.  It owns no container: the per-level key
 * lists are supplied by the caller, so the reader path can point them at
 * SandboxState (a longjmp-safe home, exactly like the writer's json_key_stack)
 * while the host-time validator points them at its own frame.  Everything else
 * here is a scalar or a pointer, so an instance may live across a raising call.
 */
struct SbJsonReader {
    kimix::vector<kimix::vector<kimix::string>> *keys = nullptr; /* one list per level */
    SbJsonCaps caps;
    size_t nodes = 0;       /* this walk's node count (the writer's json_nodes twin) */
    size_t build_bytes = 0; /* estimated interpreter bytes the push walk will need */
    char *detail = nullptr;
    size_t detail_cap = 0;
    bool failed = false;

    /* Record the FIRST failure (same rule as sb_json_fail) and answer false. */
    bool fail(const char *text) noexcept {
        if (!failed) {
            failed = true;
            if (detail != nullptr && detail_cap > 0) {
                std::snprintf(detail, detail_cap, "%s", text);
            }
        }
        return false;
    }
};

bool sbx_reader_check_node(yyjson_val *val, SbJsonReader &rd, uint32_t depth);

/* A JSON string node against the single-string cap + the build-cost estimate. */
bool sbx_reader_check_string(yyjson_val *val, SbJsonReader &rd) {
    const size_t len = yyjson_get_len(val);
    if (len > rd.caps.max_string) {
        char text[192];
        std::snprintf(text, sizeof(text), "a JSON string of %zu bytes exceeds the %zu-byte string cap", len,
                      rd.caps.max_string);
        return rd.fail(text);
    }
    rd.build_bytes += len + 64; /* the interned Lua string: header + bytes + NUL */
    return true;
}

/* An object: every key (string cap, copy into this LEVEL's list) and every value,
 * then the duplicate check. */
bool sbx_reader_check_object(yyjson_val *val, SbJsonReader &rd, uint32_t depth) {
    if (depth > rd.caps.max_depth) {
        char text[160];
        std::snprintf(text, sizeof(text), "the JSON table exceeds the %u-level nesting cap",
                      static_cast<unsigned>(rd.caps.max_depth));
        return rd.fail(text);
    }
    /* The keys of THIS nesting level: a child object collects its own one level
     * deeper, so the parent's list survives while its values are walked. */
    kimix::vector<kimix::string> &keys = (*rd.keys)[depth];
    keys.clear();
    yyjson_obj_iter iter = yyjson_obj_iter_with(val);
    yyjson_val *key = nullptr;
    while ((key = yyjson_obj_iter_next(&iter)) != nullptr) {
        const size_t len = yyjson_get_len(key);
        if (len > rd.caps.max_string) {
            char text[192];
            std::snprintf(text, sizeof(text), "a JSON object key of %zu bytes exceeds the %zu-byte string cap", len,
                          rd.caps.max_string);
            return rd.fail(text);
        }
        /* A local container is safe HERE: this validating phase makes no Lua call,
         * so the only unwind path of the module (lua_error's longjmp) cannot pass
         * through it.  The per-level lists above live in the state for the same
         * reason the writer's do. */
        kimix::string copy;
        copy.assign(yyjson_get_str(key), len);
        keys.push_back(static_cast<kimix::string &&>(copy));
        rd.build_bytes += len + 96; /* the key string + its hash-part slot */
        yyjson_val *child = yyjson_obj_iter_get_val(key);
        if (child == nullptr) {
            return rd.fail("the JSON object is missing a value");
        }
        if (!sbx_reader_check_node(child, rd, depth + 1)) {
            return false;
        }
    }
    rd.build_bytes += 128; /* the table object itself + lookup slack */
    /* Duplicate detection: sort this level's keys and compare neighbours.  Exact
     * (a duplicate can never hide behind yyjson's own "first one wins"), O(k log k)
     * per object instead of a quadratic probe per key, and it reuses the list that
     * had to be collected anyway. */
    std::sort(keys.begin(), keys.end());
    for (size_t i = 1; i < keys.size(); ++i) {
        if (keys[i] == keys[i - 1]) {
            char text[192];
            std::snprintf(text, sizeof(text), "duplicate object key '%.64s' in the JSON table", keys[i].c_str());
            return rd.fail(text);
        }
    }
    return true;
}

/* An array: 1..n, every element checked one level deeper. */
bool sbx_reader_check_array(yyjson_val *val, SbJsonReader &rd, uint32_t depth) {
    if (depth > rd.caps.max_depth) {
        char text[160];
        std::snprintf(text, sizeof(text), "the JSON table exceeds the %u-level nesting cap",
                      static_cast<unsigned>(rd.caps.max_depth));
        return rd.fail(text);
    }
    const size_t count = yyjson_arr_size(val);
    rd.build_bytes += 128 + (count * 32); /* table + one slot per element */
    yyjson_arr_iter iter = yyjson_arr_iter_with(val);
    yyjson_val *child = nullptr;
    while ((child = yyjson_arr_iter_next(&iter)) != nullptr) {
        if (!sbx_reader_check_node(child, rd, depth + 1)) {
            return false;
        }
    }
    return true;
}

/* Charge one node against the writer's own budget, then dispatch. */
bool sbx_reader_check_node(yyjson_val *val, SbJsonReader &rd, uint32_t depth) {
    if (rd.failed) {
        return false;
    }
    const size_t node_cap = (rd.caps.max_bytes / 8) + 16; /* exactly sb_json_charge */
    if (++rd.nodes > node_cap) {
        char text[160];
        std::snprintf(text, sizeof(text), "the JSON table exceeds the sandbox result cap (%zu nodes)", node_cap);
        return rd.fail(text);
    }
    switch (yyjson_get_type(val)) {
    case YYJSON_TYPE_OBJ:
        return sbx_reader_check_object(val, rd, depth);
    case YYJSON_TYPE_ARR:
        return sbx_reader_check_array(val, rd, depth);
    case YYJSON_TYPE_STR:
        return sbx_reader_check_string(val, rd);
    case YYJSON_TYPE_NUM:
        rd.build_bytes += 16;
        return true;
    case YYJSON_TYPE_BOOL:
    case YYJSON_TYPE_NULL:
    case YYJSON_TYPE_NONE:
    default:
        rd.build_bytes += 8;
        return true;
    }
}

/* Strict parse + the validating walk.  On success `out_doc` owns the document
 * (the caller's SbJsonDocPtr releases it); on failure it stays empty, so every
 * error path in here frees the document through the one RAII owner.  No Lua call
 * happens here. */
bool sbx_reader_open(const char *json, size_t len, SbJsonReader &rd, SbJsonDocPtr &out_doc) {
    if (len == 0) {
        return rd.fail("an empty JSON text cannot become a table");
    }
    if (len > rd.caps.max_bytes) {
        char text[160];
        std::snprintf(text, sizeof(text), "the JSON text exceeds the %zu-byte result cap", rd.caps.max_bytes);
        return rd.fail(text);
    }
    /* Strict: NOFLAG.  No comments, no NaN/Infinity, no trailing comma and no
     * trailing garbage; a parse failure is a host mistake, so its position and
     * description travel back verbatim. */
    yyjson_read_err err{};
    SbJsonDocPtr doc(yyjson_read_opts(const_cast<char *>(json), len, YYJSON_READ_NOFLAG, &kSbYyAlc, &err),
                     &yyjson_doc_free);
    if (!doc) {
        char text[192];
        std::snprintf(text, sizeof(text), "the JSON text is not valid strict JSON: %s (byte %zu)",
                      (err.msg != nullptr) ? err.msg : "parse error", err.pos);
        return rd.fail(text);
    }
    yyjson_val *root = yyjson_doc_get_root(doc.get());
    if (root == nullptr) {
        return rd.fail("the JSON text holds no value");
    }
    if (!yyjson_is_obj(root) && !yyjson_is_arr(root)) {
        return rd.fail("a table must be a JSON object or an array, not a scalar");
    }
    if (!sbx_reader_check_node(root, rd, 0)) {
        return false;
    }
    out_doc = static_cast<SbJsonDocPtr &&>(doc); /* ownership moves out; `doc` frees nothing */
    return true;
}

/* The push walk.  It runs ONLY on a tree sbx_reader_check_node accepted, so no cap
 * can fail here and the recursion is bounded by caps.max_depth; false can only
 * mean the interpreter's own stack is exhausted. */
bool sbx_reader_push(lua_State *L, yyjson_val *val) {
    switch (yyjson_get_type(val)) {
    case YYJSON_TYPE_NULL:
        /* null -> nil.  In a container that is a hole; the two container cases
         * below skip the slot instead of writing nil, which is the same value with
         * one stack slot less (see script_value.h for the documented consequence). */
        lua_pushnil(L);
        return true;
    case YYJSON_TYPE_BOOL:
        lua_pushboolean(L, yyjson_get_bool(val) ? 1 : 0);
        return true;
    case YYJSON_TYPE_NUM:
        /* An integer stays an integer when it is integral AND fits: an unsigned
         * value above INT64_MAX cannot be a lua_Integer, so it falls back to float. */
        if (yyjson_is_uint(val)) {
            const uint64_t unsigned_value = yyjson_get_uint(val);
            if (unsigned_value <= static_cast<uint64_t>(INT64_MAX)) {
                lua_pushinteger(L, static_cast<lua_Integer>(unsigned_value));
            } else {
                lua_pushnumber(L, static_cast<lua_Number>(unsigned_value));
            }
        } else if (yyjson_is_sint(val)) {
            lua_pushinteger(L, static_cast<lua_Integer>(yyjson_get_sint(val)));
        } else {
            lua_pushnumber(L, static_cast<lua_Number>(yyjson_get_real(val)));
        }
        return true;
    case YYJSON_TYPE_STR:
        lua_pushlstring(L, yyjson_get_str(val), yyjson_get_len(val));
        return true;
    case YYJSON_TYPE_ARR: {
        const size_t count = yyjson_arr_size(val);
        if (!lua_checkstack(L, 4)) {
            return false;
        }
        /* Presize the array part so the table is built in one allocation (and so a
         * large array cannot degrade into repeated rehashes). */
        lua_createtable(L, (count <= 0x7FFFFFFFu) ? static_cast<int>(count) : 0, 0);
        yyjson_arr_iter iter = yyjson_arr_iter_with(val);
        yyjson_val *child = nullptr;
        lua_Integer index = 0;
        while ((child = yyjson_arr_iter_next(&iter)) != nullptr) {
            ++index;
            if (yyjson_is_null(child)) {
                continue; /* a hole, not an entry */
            }
            if (!sbx_reader_push(L, child)) {
                return false;
            }
            lua_rawseti(L, -2, index);
        }
        return true;
    }
    case YYJSON_TYPE_OBJ: {
        const size_t count = yyjson_obj_size(val);
        if (!lua_checkstack(L, 5)) {
            return false;
        }
        lua_createtable(L, 0, (count <= 0x7FFFFFFFu) ? static_cast<int>(count) : 0);
        yyjson_obj_iter iter = yyjson_obj_iter_with(val);
        yyjson_val *key = nullptr;
        while ((key = yyjson_obj_iter_next(&iter)) != nullptr) {
            yyjson_val *child = yyjson_obj_iter_get_val(key);
            if (child == nullptr || yyjson_is_null(child)) {
                continue; /* a null value cannot live in a Lua table: the key goes */
            }
            lua_pushlstring(L, yyjson_get_str(key), yyjson_get_len(key));
            if (!sbx_reader_push(L, child)) {
                return false;
            }
            lua_rawset(L, -3);
        }
        return true;
    }
    case YYJSON_TYPE_NONE:
    default:
        /* Unreachable: the validating walk accepted only the types above. */
        lua_pushnil(L);
        return true;
    }
}

} // namespace src_script_script_value
using namespace src_script_script_value;

/* Materialise the JSON text in [json, json+len) as ONE Lua value on the stack.
 * Declared in script_value.h, defined outside the helper namespace for the same
 * reason as the writer below: script_host.cpp calls it through that declaration. */
bool sb_json_to_lua(lua_State *L, SandboxState *s, const char *json, size_t len) {
    s->json_failed = false;
    s->json_detail[0] = '\0';

    SbJsonReader rd;
    rd.caps.max_bytes = s->max_result_bytes;
    rd.caps.max_string = s->max_string;
    /* The host's depth cap, further bounded by the native-recursion limit -- the
     * same rule the writer applies to json_depth_cap. */
    rd.caps.max_depth = (s->max_result_depth > kSbJsonDepthLimit) ? kSbJsonDepthLimit : s->max_result_depth;
    rd.keys = &s->json_key_stack; /* STATE-owned: this frame may be longjmped past */
    rd.detail = s->json_detail;
    rd.detail_cap = sizeof(s->json_detail);
    s->json_key_stack.resize(static_cast<size_t>(rd.caps.max_depth) + 2);
    for (kimix::vector<kimix::string> &keys : s->json_key_stack) {
        keys.clear();
    }

    /* The house rule for a C-owned object: an explicit deleter, so EVERY return
     * below -- each failure path included -- releases the document. */
    SbJsonDocPtr doc(nullptr, &yyjson_doc_free);
    if (!sbx_reader_open(json, len, rd, doc)) {
        s->json_failed = true; /* the reason is already in s->json_detail */
        return false;
    }

    /* The push walk allocates interpreter memory, and the budgeted lua_Alloc
     * answers a denial with NULL -- Lua then raises a MEMORY longjmp, which would
     * skip the deleter above and strand the document.  So the estimate the
     * validating walk accumulated is charged HERE, while the failure can still be
     * returned (a plain false, the caller's LIMIT) with the document released. */
    const size_t build_bytes = rd.build_bytes + 4096; /* + stack/lookup slack */
    if (s->max_memory == 0 || build_bytes > s->max_memory || s->alloc_used + build_bytes > s->max_memory) {
        char text[192];
        std::snprintf(text, sizeof(text),
                      "the JSON table needs about %zu bytes of interpreter memory (used %zu of %zu)",
                      build_bytes, s->alloc_used, s->max_memory);
        s->json_failed = true;
        std::snprintf(s->json_detail, sizeof(s->json_detail), "%s", text);
        return false;
    }

    if (!sbx_reader_push(L, yyjson_doc_get_root(doc.get()))) {
        s->json_failed = true;
        std::snprintf(s->json_detail, sizeof(s->json_detail), "%s", "the interpreter stack is exhausted");
        return false;
    }
    return true;
}

/* Host-time validation of a table text (Sandbox::set_global): the same parse and
 * the same validating walk, no interpreter and therefore no build step. */
bool sb_json_check(const char *json, size_t len, const SbJsonCaps &caps) noexcept {
    if (json == nullptr) {
        return false;
    }
    /* This function is NOT on a longjmp path (it never calls into the
     * interpreter and never allocates interpreter memory), which is exactly why
     * the two containers below may be ordinary locals. */
    kimix::vector<kimix::vector<kimix::string>> keys;
    char detail[192];
    detail[0] = '\0';
    SbJsonReader rd;
    rd.caps.max_bytes = caps.max_bytes;
    rd.caps.max_string = caps.max_string;
    rd.caps.max_depth = (caps.max_depth > kSbJsonDepthLimit) ? kSbJsonDepthLimit : caps.max_depth;
    rd.keys = &keys;
    rd.detail = detail;
    rd.detail_cap = sizeof(detail);
    keys.resize(static_cast<size_t>(rd.caps.max_depth) + 2);

    SbJsonDocPtr doc(nullptr, &yyjson_doc_free);
    if (!sbx_reader_open(json, len, rd, doc)) {
        return false;
    }
    /* One owner, one free: this pass builds no Lua value, so `doc` releases the
     * document here and nothing is handed on. */
    return true;
}

/* Build the canonical JSON of the table at `idx` into state->json_buffer.
 * Declared in script_value.h and defined here OUTSIDE the helper namespace: the
 * host-argument marshaller (script_host.cpp) calls it through that declaration,
 * so it is one function, not two look-alikes in different namespaces. */
bool sb_json_build_table(lua_State *L, SandboxState *s, int idx) {
    s->json_failed = false;
    s->json_detail[0] = '\0';
    s->json_nodes = 0;
    s->json_path.clear();
    s->json_buffer.clear();
    /* A stack bound, not corruption: past kSbJsonDepthLimit levels the message is
     * a LIMIT (the reader applies the same bound in the other direction). */
    s->json_depth_cap = (s->max_result_depth > kSbJsonDepthLimit) ? kSbJsonDepthLimit : s->max_result_depth;
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
