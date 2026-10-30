// Test for the vendored Lua 5.5.1 library, built as the SHARED `lua` target
// (lua.dll exporting the plain C API; KIMIX_LUA_USE_SHARED makes LUA_API
// dllimport here). luaL_newstate routes through the exported luaL_alloc, which
// this project compiles with KIMIX_LUA_USE_MIMALLOC, so every allocation below
// exercises the mimalloc-backed realloc/free paths across the dll boundary.
// This test covers:
// - Version sanity (LUA_VERSION_NUM / lua_version)
// - State lifecycle (luaL_newstate / luaL_openlibs / lua_close)
// - Running chunks (luaL_dostring, luaL_loadstring + lua_pcall) and reading
//   globals back with lua_getglobal / lua_tointegerx / lua_tolstring
// - Table construction and iteration from C (lua_createtable, lua_seti,
//   lua_geti, luaL_len)
// - Protected-mode error handling (state stays usable after a caught error)
// - C function callbacks (lua_pushcfunction called from Lua)
// - Allocator / GC stress (lua_gc LUA_GCCOLLECT over many strings/tables)

#include "ut/ut.hpp"

// The vendored lua.h / lauxlib.h / lualib.h carry no `extern "C"` block
// (luaconf.h only gives LUAMOD_API the `extern "C"` spelling for luaopen_*
// functions), so C++ consumers must wrap the includes themselves — otherwise
// the dllimport declarations are C++-mangled and never match the C names
// exported by lua.dll / lua.lib.
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <cstring>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace tests_unit_ext_test_lua {

// RAII guard: one Lua state per test, always closed (also on assertion
// failure — the ut `expect` on a failed soft assert does not throw, but a hard
// assert would; the guard keeps the close unconditional).
struct lua_state {
    lua_State *L = nullptr;

    lua_state() : L(luaL_newstate()) {
        if (L) luaL_openlibs(L);
    }
    ~lua_state() {
        if (L) lua_close(L);
    }
    lua_state(const lua_state &) = delete;
    lua_state &operator=(const lua_state &) = delete;

    // Run a chunk in protected mode; returns the status and leaves any error
    // message (or results) on the stack. Unlike the luaL_dostring macro (whose
    // `||` collapses the status to 0/1), this keeps the real error code.
    int dostring(const char *chunk) {
        int status = luaL_loadstring(L, chunk);
        if (status != LUA_OK) return status;
        return lua_pcall(L, 0, LUA_MULTRET, 0);
    }
};

// C function called from Lua: returns the sum of its two integer arguments.
int c_add(lua_State *L) {
    lua_Integer a = luaL_checkinteger(L, 1);
    lua_Integer b = luaL_checkinteger(L, 2);
    lua_pushinteger(L, a + b);
    return 1;
}

} // namespace tests_unit_ext_test_lua
using namespace tests_unit_ext_test_lua;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "lua_version_sanity"_test = [] {
        expect(eq(LUA_VERSION_NUM, 505)) << "vendored Lua must be 5.5.x";
        lua_state ls;
        expect(ls.L != nullptr) << "luaL_newstate should succeed";
        if (!ls.L) return;
        // lua_version returns the numeric version as a lua_Number (505.0),
        // never a pointer — non-null == non-zero floating value.
        expect(lua_version(ls.L) != 0.0) << "lua_version should report the version";
    };

    "state_lifecycle"_test = [] {
        // A state that is opened and closed cleanly, twice, in one process —
        // each luaL_newstate installs the exported mimalloc-backed luaL_alloc.
        {
            lua_state ls;
            expect(ls.L != nullptr) << "first state should open";
            if (ls.L) expect(ls.dostring("local x = 1") == LUA_OK);
        }
        {
            lua_State *L = luaL_newstate(); // no openlibs: minimal state
            expect(L != nullptr) << "second state should open";
            if (L) {
                expect(eq(lua_gettop(L), 0)) << "fresh state has an empty stack";
                lua_close(L);
            }
        }
    };

    "run_chunk_arithmetic"_test = [] {
        lua_state ls;
        if (!ls.L) return;
        const char *chunk = "x = (3 + 4) * 2  -- 14\n"
                            "s = string.format('%d-%s', x, 'ok')\n"
                            "f = x / 4\n";
        // The convenience macro: (luaL_loadstring || lua_pcall) -> 0 on success.
        expect(eq(luaL_dostring(ls.L, chunk), 0)) << "chunk should run";

        // Read the globals back across the C boundary.
        expect(eq(lua_getglobal(ls.L, "x"), LUA_TNUMBER));
        expect(lua_isinteger(ls.L, -1)) << "x should be an integer";
        int isnum = 0;
        lua_Integer xi = lua_tointegerx(ls.L, -1, &isnum);
        expect(isnum) << "x should convert to integer";
        expect(eq(xi, 14)) << "(3+4)*2 == 14";
        lua_pop(ls.L, 1);

        expect(eq(lua_getglobal(ls.L, "s"), LUA_TSTRING));
        expect(strcmp(lua_tostring(ls.L, -1), "14-ok") == 0) << "string.format result";
        lua_pop(ls.L, 1);

        expect(eq(lua_getglobal(ls.L, "f"), LUA_TNUMBER));
        expect(lua_tonumber(ls.L, -1) == 3.5) << "float division";
        lua_pop(ls.L, 1);
    };

    "load_and_pcall"_test = [] {
        lua_state ls;
        if (!ls.L) return;
        // Explicit luaL_loadstring + lua_pcall with a fixed result count,
        // instead of the luaL_dostring convenience macro.
        const char *chunk = "return 2 + 3, 'two results'";
        expect(eq(luaL_loadstring(ls.L, chunk), LUA_OK)) << "chunk should compile";
        expect(eq(lua_pcall(ls.L, 0, 2, 0), LUA_OK)) << "chunk should call";
        expect(eq(lua_gettop(ls.L), 2)) << "two results on the stack";
        expect(eq(lua_tointeger(ls.L, 1), 5));
        expect(eq(lua_type(ls.L, 2), LUA_TSTRING));
        expect(strcmp(lua_tostring(ls.L, 2), "two results") == 0);
        lua_pop(ls.L, 2);
    };

    "table_build_iterate"_test = [] {
        lua_state ls;
        if (!ls.L) return;
        // Build {10, 20, 30, 40} from C, set it as a global, iterate it from C.
        lua_createtable(ls.L, 4, 0);
        for (int i = 1; i <= 4; i++) {
            lua_pushinteger(ls.L, i * 10);
            lua_seti(ls.L, -2, i); // t[i] = value, pops the value
        }
        expect(eq(lua_rawlen(ls.L, -1), 4u)) << "raw length of the array part";
        lua_setglobal(ls.L, "arr");

        // luaL_len through Lua on the global table.
        expect(eq(lua_getglobal(ls.L, "arr"), LUA_TTABLE));
        expect(eq(luaL_len(ls.L, -1), 4)) << "#arr == 4";
        lua_pop(ls.L, 1);

        // Sum the array part with lua_geti.
        lua_Integer sum = 0;
        expect(eq(lua_getglobal(ls.L, "arr"), LUA_TTABLE));
        for (int i = 1; i <= 4; i++) {
            lua_geti(ls.L, -1, i); // push arr[i]
            sum += lua_tointeger(ls.L, -1);
            lua_pop(ls.L, 1);      // pop arr[i]
        }
        lua_pop(ls.L, 1);          // pop arr
        expect(eq(sum, 100)) << "10+20+30+40 == 100";

        // Traverse the same table with lua_next from C.
        expect(eq(lua_getglobal(ls.L, "arr"), LUA_TTABLE));
        int n = 0;
        lua_Integer nsum = 0;
        lua_pushnil(ls.L);
        while (lua_next(ls.L, -2) != 0) {
            n++;
            nsum += lua_tointeger(ls.L, -1);
            lua_pop(ls.L, 1); // keep the key for the next step
        }
        expect(eq(n, 4)) << "lua_next should visit all 4 entries";
        expect(eq(nsum, 100));
        lua_pop(ls.L, 1); // the table
    };

    "error_handling"_test = [] {
        lua_state ls;
        if (!ls.L) return;
        // A runtime error inside a protected call must come back as a message
        // on the stack, and the state must remain usable.
        int status = ls.dostring("error('boom')");
        expect(eq(status, LUA_ERRRUN)) << "error() should surface as LUA_ERRRUN";
        expect(eq(lua_type(ls.L, -1), LUA_TSTRING)) << "error object on the stack";
        expect(strstr(lua_tostring(ls.L, -1), "boom") != nullptr) << "message should contain 'boom'";
        lua_pop(ls.L, 1);

        // A syntax error is caught at load time by luaL_loadstring.
        expect(eq(luaL_loadstring(ls.L, "local = ="), LUA_ERRSYNTAX)) << "malformed chunk should not compile";
        lua_pop(ls.L, 1); // the syntax error message

        // The state must stay fully usable after both kinds of errors:
        status = ls.dostring("y = 40 + 2");
        expect(eq(status, LUA_OK)) << "state stays usable after errors";
        expect(eq(lua_getglobal(ls.L, "y"), LUA_TNUMBER));
        expect(eq(lua_tointeger(ls.L, -1), 42));
        lua_pop(ls.L, 1);
    };

    "c_function_callback"_test = [] {
        lua_state ls;
        if (!ls.L) return;
        // Register c_add and call it from a Lua chunk; the return value must
        // cross the C boundary back to the caller.
        lua_register(ls.L, "c_add", c_add); // pushcfunction + setglobal
        expect(eq(lua_getglobal(ls.L, "c_add"), LUA_TFUNCTION));
        expect(lua_iscfunction(ls.L, -1)) << "c_add should be a C function";
        lua_pop(ls.L, 1);

        int status = ls.dostring("result = c_add(19, 23)");
        expect(eq(status, LUA_OK)) << "calling the C function from Lua should work";
        expect(eq(lua_getglobal(ls.L, "result"), LUA_TNUMBER));
        expect(eq(lua_tointeger(ls.L, -1), 42)) << "19 + 23 crosses the boundary intact";
        lua_pop(ls.L, 1);

        // A C function that errors is still protected by lua_pcall.
        status = ls.dostring("return c_add('not a number')");
        expect(eq(status, LUA_ERRRUN)) << "luaL_checkinteger failure is a protected error";
        expect(strstr(lua_tostring(ls.L, -1), "number") != nullptr) << "checkerror message";
        lua_pop(ls.L, 1);
    };

    "gc_allocator_stress"_test = [] {
        // Many strings and tables churned through the exported mimalloc-backed
        // luaL_alloc (realloc + free paths), then a full collect and a clean
        // close. Two states run back-to-back to cover open/close repetition.
        for (int round = 0; round < 2; round++) {
            lua_state ls;
            if (!ls.L) return;
            int status = ls.dostring(
                "local acc = {}\n"
                "for i = 1, 2000 do\n"
                "  acc[i] = { id = i, name = string.rep('x', i % 64 + 1), t = tostring(i) }\n"
                "end\n"
                "local s = 0\n"
                "for i = 1, #acc do s = s + acc[i].id end\n"
                "total = s\n"
                "acc = nil\n");
            expect(eq(status, LUA_OK)) << "churn chunk should run";
            expect(eq(lua_getglobal(ls.L, "total"), LUA_TNUMBER));
            expect(eq(lua_tointeger(ls.L, -1), 2000 * 2001 / 2)) << "sum of 1..2000";
            lua_pop(ls.L, 1);

            // Force a full GC (collects the garbage above through kimix::deallocate),
            // then confirm the state still computes and the heap reports size.
            expect(eq(lua_gc(ls.L, LUA_GCCOLLECT), 0)) << "full collect should complete";
            expect(gt(lua_gc(ls.L, LUA_GCCOUNT), 0)) << "LUA_GCCOUNT should see live memory";
            expect(eq(lua_gc(ls.L, LUA_GCISRUNNING), 1)) << "collector still running";

            // Even after a full collect, a bad method on a string is still a
            // clean protected error, and the state keeps working.
            status = ls.dostring("check = ('abcdef'):no_such_method()");
            expect(eq(status, LUA_ERRRUN)) << "missing method must raise a protected error";
            lua_pop(ls.L, 1);
            status = ls.dostring("check = #('abcdef') + 1");
            expect(eq(status, LUA_OK)) << "state usable after full GC";
            expect(eq(lua_getglobal(ls.L, "check"), LUA_TNUMBER));
            expect(eq(lua_tointeger(ls.L, -1), 7));
            lua_pop(ls.L, 1);
            // ~lua_state closes the state here.
        }
    };
}
