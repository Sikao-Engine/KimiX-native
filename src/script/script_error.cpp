/*
 * script_error.cpp -- the single message formatter of the sandbox.
 *
 * Pure formatting: no Lua call, no allocation, no longjmp.  The raise helpers
 * that use it live in script_sticky.cpp; the C functions that call those live
 * in script_env.cpp / script_hook.cpp / script_alloc.cpp.
 */
#include <script/script_error.h>

#include <cstdio>
#include <cstring>

#include <script/script_lua.h>

namespace kimix::script {

namespace src_script_script_error {

/* NUL-terminated prefix per Status.  Kept as literals (not the string_views of
 * script_status.h) so snprintf can use them directly. */
const char *sb_prefix_cstr(Status kind) noexcept {
    switch (kind) {
    case Status::ok: return "";
    case Status::syntax: return "SYNTAX: ";
    case Status::runtime: return "RUNTIME: ";
    case Status::denied: return "DENIED: ";
    case Status::memory: return "MEMORY: ";
    case Status::timeout: return "TIMEOUT: ";
    case Status::behaviour: return "BEHAVIOUR: ";
    case Status::cancelled: return "CANCELLED: ";
    case Status::limit: return "LIMIT: ";
    case Status::internal: return "INTERNAL: ";
    }
    return "";
}

} // namespace src_script_script_error
using namespace src_script_script_error;

size_t sb_compose_body(char *out, size_t cap, Status kind, const char *api, const char *detail) noexcept {
    if (out == nullptr || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    const char *prefix = sb_prefix_cstr(kind);
    const char *det = (detail != nullptr) ? detail : "";
    int written;
    if (api != nullptr && api[0] != '\0') {
        written = std::snprintf(out, cap, "%s%s%s%s", prefix, api, SB_DASH, det);
    } else {
        written = std::snprintf(out, cap, "%s%s", prefix, det);
    }
    if (written < 0) {
        out[0] = '\0';
        return 0;
    }
    const size_t used = static_cast<size_t>(written);
    return (used < cap) ? used : (cap - 1);
}

size_t sb_where(lua_State *L, char *out, size_t cap) noexcept {
    if (out == nullptr || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    luaL_where(L, 1); /* pushes "chunk:line: " or "" */
    size_t len = 0;
    const char *text = lua_tolstring(L, -1, &len);
    if (text == nullptr) {
        lua_pop(L, 1);
        return 0;
    }
    if (len >= cap) {
        len = cap - 1;
    }
    std::memcpy(out, text, len);
    out[len] = '\0';
    lua_pop(L, 1);
    return len;
}

size_t sb_location_prefix_len(const char *msg, size_t len) noexcept {
    if (msg == nullptr) {
        return 0;
    }
    size_t best = 0;
    for (size_t i = 0; i + 1 < len; ++i) {
        if (msg[i] != ':' || msg[i + 1] != ' ') {
            continue;
        }
        size_t j = i;
        size_t digits = 0;
        while (j > 0 && msg[j - 1] >= '0' && msg[j - 1] <= '9') {
            --j;
            ++digits;
        }
        if (digits == 0 || j == 0 || msg[j - 1] != ':') {
            continue;
        }
        best = i + 2; /* accept the LAST such group: "C:\a:b.lua:12: " */
    }
    return best;
}

int32_t sb_line_from_message(const char *msg, size_t len) noexcept {
    const size_t prefix = sb_location_prefix_len(msg, len);
    if (prefix < 3) {
        return 0;
    }
    const size_t end = prefix - 2; /* index just past the digits */
    size_t start = end;
    while (start > 0 && msg[start - 1] >= '0' && msg[start - 1] <= '9') {
        --start;
    }
    int32_t value = 0;
    for (size_t i = start; i < end; ++i) {
        value = value * 10 + static_cast<int32_t>(msg[i] - '0');
        if (value > 100000000) {
            return value;
        }
    }
    return value;
}

} // namespace kimix::script
