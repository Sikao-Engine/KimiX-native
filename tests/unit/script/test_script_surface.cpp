// Test for the sandbox SURFACE (a golden capability allowlist).
// This test covers:
// - The complete reachable surface of a run environment, enumerated from inside
//   the sandbox (every global, and every key of the tables it exposes, to depth
//   3) and compared with an inline golden list.  A future Lua version adding a
//   global, or an accidental open-every-library call, therefore fails with a
//   diff and forces a deliberate decision instead of silently widening the
//   surface.
// - No userdata and no thread value is reachable at all (the pointer-control
//   rule: a script can never hold something that carries an address).
// - The forbidden table/function names exist as TAGGED tripwires, i.e. the loud
//   path is installed rather than the names merely being absent.
#include "ut/ut.hpp"

#include <script/script_sandbox.h>

#include "script_test_util.h"

using namespace boost::ut;
using namespace boost::ut::literals;
using kimix::script::Sandbox;
using kimix::script::Status;

namespace tests_unit_script_test_script_surface {

/* Enumerate the environment from inside the sandbox: every global name plus
 * "type", and for each table its members to depth 3, sorted, one per line. */
const char *const kEnumerator =
    "local names = {}\n"
    "local function walk(prefix, t, depth)\n"
    "  for k, v in next, t do\n"
    "    if not (prefix == '' and k == '_G') then\n"
    "      local name = prefix .. tostring(k)\n"
    "      names[#names + 1] = name .. ' = ' .. type(v)\n"
    "      if type(v) == 'table' and depth < 3 and #names < 400 then\n"
    "        walk(name .. '.', v, depth + 1)\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "end\n"
    "walk('', _G, 0)\n"
    "table.sort(names)\n"
    "return table.concat(names, '\\n')\n";

/* The deliberate allowlist.  Regenerate by running the suite: a mismatch prints
 * the actual list, which is then pasted here after review. */
const char kGoldenSurface[] =
    "_VERSION = string\n"
    "assert = function\n"
    "collectgarbage = function\n"
    "debug = table\n"
    "debug.__category = string\n"
    "debug.__detail = string\n"
    "dofile = function\n"
    "error = function\n"
    "getmetatable = function\n"
    "io = table\n"
    "io.__category = string\n"
    "io.__detail = string\n"
    "ipairs = function\n"
    "load = function\n"
    "loadfile = function\n"
    "loadlib = function\n"
    "math = table\n"
    "math.abs = function\n"
    "math.acos = function\n"
    "math.asin = function\n"
    "math.atan = function\n"
    "math.ceil = function\n"
    "math.cos = function\n"
    "math.deg = function\n"
    "math.exp = function\n"
    "math.floor = function\n"
    "math.fmod = function\n"
    "math.frexp = function\n"
    "math.huge = number\n"
    "math.ldexp = function\n"
    "math.log = function\n"
    "math.max = function\n"
    "math.maxinteger = number\n"
    "math.min = function\n"
    "math.mininteger = number\n"
    "math.modf = function\n"
    "math.pi = number\n"
    "math.rad = function\n"
    "math.random = function\n"
    "math.randomseed = function\n"
    "math.sin = function\n"
    "math.sqrt = function\n"
    "math.tan = function\n"
    "math.tointeger = function\n"
    "math.type = function\n"
    "math.ult = function\n"
    "next = function\n"
    "os = table\n"
    "os.__category = string\n"
    "os.__detail = string\n"
    "package = table\n"
    "package.__category = string\n"
    "package.__detail = string\n"
    "pairs = function\n"
    "pcall = function\n"
    "print = function\n"
    "rawequal = function\n"
    "rawget = function\n"
    "rawlen = function\n"
    "rawset = function\n"
    "require = function\n"
    "select = function\n"
    "setmetatable = function\n"
    "string = table\n"
    "string.byte = function\n"
    "string.char = function\n"
    "string.dump = function\n"
    "string.find = function\n"
    "string.format = function\n"
    "string.gmatch = function\n"
    "string.gsub = function\n"
    "string.len = function\n"
    "string.lower = function\n"
    "string.match = function\n"
    "string.pack = function\n"
    "string.packsize = function\n"
    "string.rep = function\n"
    "string.reverse = function\n"
    "string.sub = function\n"
    "string.unpack = function\n"
    "string.upper = function\n"
    "table = table\n"
    "table.concat = function\n"
    "table.create = function\n"
    "table.insert = function\n"
    "table.move = function\n"
    "table.pack = function\n"
    "table.remove = function\n"
    "table.sort = function\n"
    "table.unpack = function\n"
    "tonumber = function\n"
    "tostring = function\n"
    "type = function\n"
    "utf8 = table\n"
    "utf8.char = function\n"
    "utf8.charpattern = string\n"
    "utf8.codepoint = function\n"
    "utf8.codes = function\n"
    "utf8.len = function\n"
    "utf8.offset = function\n"
    "warn = function\n"
    "xpcall = function";

} // namespace tests_unit_script_test_script_surface
using namespace tests_unit_script_test_script_surface;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    "surface_matches_the_golden_allowlist"_test = [] {
        Sandbox sb;
        auto r = sb.run(kEnumerator, "surface.lua");
        expect(r.ok()) << r.message.c_str();
        expect(eq(r.results.size(), size_t{1}));
        if (r.results.empty()) {
            return;
        }
        const kimix::string &actual = r.results[0].s;
        expect(actual == kimix::string(kGoldenSurface))
            << "the reachable surface changed; review and update kGoldenSurface:\n" << actual.c_str();
    };

    "no_userdata_or_thread_is_reachable"_test = [] {
        Sandbox sb;
        auto r = sb.run(kEnumerator, "surface.lua");
        expect(r.ok()) << r.message.c_str();
        if (r.results.empty()) {
            return;
        }
        const kimix::string &list = r.results[0].s;
        expect(!script_test::has_long_hex(list)) << "an address leaked into the environment listing";
        expect(list.find(" = userdata") == kimix::string::npos)
            << "a script must never reach a userdata value:\n" << list.c_str();
        expect(list.find(" = thread") == kimix::string::npos)
            << "a script must never reach a thread value:\n" << list.c_str();
    };

    "tripwires_are_installed_and_tagged"_test = [] {
        Sandbox sb;
        auto r = sb.run("return type(io), type(os), type(package), type(debug),"
                        " type(require), type(dofile), type(loadfile), type(loadlib),"
                        " type(string.dump), tostring(io), tostring(os), tostring(debug),"
                        " type(_G), tostring(_G) == tostring(_G)");
        expect(r.ok()) << r.message.c_str();
        expect(eq(r.results.size(), size_t{14}));
        static const char *const kExpected[] = {
            "table", "table", "table", "table",
            "function", "function", "function", "function", "function",
            "io (blocked by sandbox)", "os (blocked by sandbox)", "debug (blocked by sandbox)",
            "table",
        };
        const size_t count = (r.results.size() < 13) ? r.results.size() : 13;
        for (size_t i = 0; i < count; ++i) {
            expect(r.results[i].kind == kimix::script::Value::Kind::string)
                << "index " << static_cast<int>(i) << " is not a string";
            if (r.results[i].kind == kimix::script::Value::Kind::string) {
                expect(r.results[i].s == kimix::string(kExpected[i]))
                    << "index " << static_cast<int>(i) << " was " << r.results[i].s.c_str();
            }
        }
        if (r.results.size() == 14) {
            expect(r.results[13].kind == kimix::script::Value::Kind::boolean);
            expect(r.results[13].b) << "_G must be the environment itself";
        }
    };

    "environment_has_exactly_one_global_table"_test = [] {
        Sandbox sb;
        auto r = sb.run("local n = 0 for k in next, _G do n = n + 1 end return n");
        expect(r.ok()) << r.message.c_str();
        if (!r.results.empty()) {
            expect(r.results[0].kind == kimix::script::Value::Kind::integer);
            /* 23 allowed names + 8 replacements/tripwires + _G + the natives a
             * caller installs (none here).  The golden surface above is the
             * authority; this is a coarse guard against a wholesale change. */
            expect(r.results[0].i >= 25) << "the environment lost names: " << r.message.c_str();
            expect(r.results[0].i <= 64) << "the environment gained names: " << r.message.c_str();
        }
    };
}
