// Test for the sandbox ABORT CONTRACT (the point of the whole module).
// This test covers:
// - The 17 forbidden APIs of the plan, each in 7 wrappers: a direct call, pcall,
//   xpcall(f, h), pcall(xpcall, f, h), pcall(pcall, f), coroutine.resume, and a
//   coroutine that yields BEFORE it calls the API.  Every one of them must answer
// status == denied with "<chunk>:<line>: DENIED: <api> <EM DASH> <why>"
// (U+2014, the separator SB_DASH emits -- not the ASCII "--").
// - The denial is not swallowable: `while true do pcall(...) end` aborts.
// - Bytecode: load(string.dump(f)), a raw "\27Lua" chunk and load(s,'n','b') are
//   all refused.
// - Pointer hygiene: tostring/string.format('%s') never print an address, and
//   string.format('%p') is denied.
// - A denial inside a __gc finalizer is surfaced (and nothing reaches stderr).
// - A script cannot restore a catcher by replacing a sandbox primitive: that is
//   Status::behaviour with the results dropped.
#include "ut/ut.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>

#include <script/script_sandbox.h>

#include "script_test_util.h"

using namespace boost::ut;
using namespace boost::ut::literals;
using kimix::script::Limits;
using kimix::script::Sandbox;
using kimix::script::Status;
using kimix::script::Value;

namespace tests_unit_script_test_script_hostile {

bool contains(const kimix::string &text, const char *needle) {
    return text.find(needle) != kimix::string::npos;
}

bool starts_with(const kimix::string &text, const char *prefix) {
    const size_t len = std::strlen(prefix);
    return text.size() >= len && std::memcmp(text.data(), prefix, len) == 0;
}

/* The seven wrappers a denial has to survive.  `%s` is replaced by the Lua
 * expression that reaches the forbidden API. */
const char *const kContexts[] = {
    "return %s",
    "return pcall(function() return %s end)",
    "return xpcall(function() return %s end, function(e) return 'handled' end)",
    "return pcall(xpcall, function() return %s end, function(e) return 'handled' end)",
    "return pcall(pcall, function() return %s end)",
    "local co = coroutine.create(function() return %s end) return coroutine.resume(co)",
    "local co = coroutine.create(function() coroutine.yield() return %s end)"
    " coroutine.resume(co) return coroutine.resume(co)",
};

kimix::string build_chunk(const char *templ, const char *expr) {
    kimix::string source;
    const size_t len = std::strlen(templ);
    for (size_t i = 0; i < len; ++i) {
        if (templ[i] == '%' && i + 1 < len && templ[i + 1] == 's') {
            source.append(expr);
            ++i;
        } else {
            source.push_back(templ[i]);
        }
    }
    return source;
}

Limits hostile_limits() {
    Limits limits;
    limits.max_seconds = 1.0;
    limits.hook_instruction_granularity = 1000;
    return limits;
}

} // namespace tests_unit_script_test_script_hostile
using namespace tests_unit_script_test_script_hostile;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    "every_forbidden_api_is_denied_in_every_wrapper"_test = [] {
        size_t api_count = 0;
        const script_test::forbidden_api *apis = script_test::forbidden_apis(&api_count);
        for (size_t a = 0; a < api_count; ++a) {
            for (size_t c = 0; c < sizeof(kContexts) / sizeof(kContexts[0]); ++c) {
                Sandbox sb(hostile_limits());
                expect(sb.allow_coroutines(true));
                const kimix::string source = build_chunk(kContexts[c], apis[a].expr);
                auto r = sb.run(source);
                const char *where = apis[a].api;
                expect(r.status == Status::denied)
                    << where << " in context " << static_cast<int>(c) << ": " << r.message.c_str();
                expect(starts_with(r.message, "script.lua:")) << r.message.c_str();
                /* `load` refuses a binary chunk with the LOADERR text of the plan
                 * (Appendix A) rather than the DENIED shape; every other row must
                 * carry the DENIED prefix. */
                if (std::strcmp(apis[a].api, "load") == 0) {
                    expect(contains(r.message, "LOADERR: ")) << r.message.c_str();
                } else {
                    expect(contains(r.message, "DENIED: ")) << r.message.c_str();
                }
                expect(contains(r.message, apis[a].api)) << r.message.c_str();
                expect(r.violation.kind == Status::denied) << r.message.c_str();
                expect(r.violation.api == kimix::string(apis[a].api)) << r.violation.api.c_str();
                expect(r.violation.category == kimix::string(apis[a].category))
                    << r.violation.category.c_str();
                expect(r.violation.line >= 1) << r.message.c_str();
                expect(r.results.empty()) << "a denied run must not deliver results";
            }
        }
    };

    "denial_message_has_the_documented_shape"_test = [] {
        Sandbox sb(hostile_limits());
        auto r = sb.run("io.open('x.txt')");
        expect(r.status == Status::denied) << r.message.c_str();
        /* script.lua:1: DENIED: io.open -- file I/O is not available in the sandbox */
        const char *expected_api = "DENIED: io.open ";
        const char *expected_detail = "file I/O is not available in the sandbox";
        expect(starts_with(r.message, "script.lua:1: ")) << r.message.c_str();
        expect(contains(r.message, expected_api)) << r.message.c_str();
        expect(contains(r.message, expected_detail)) << r.message.c_str();
        /* the em dash separator (U+2014) between the api and the detail */
        expect(contains(r.message, " \xE2\x80\x94 ")) << r.message.c_str();
    };

    "pick_the_documented_messages"_test = [] {
        Sandbox sb(hostile_limits());
        auto exit_run = sb.run("os.exit(0)");
        expect(contains(exit_run.message, "terminating the host process is not available in the sandbox"))
            << exit_run.message.c_str();
        auto debug_run = sb.run("debug.getregistry()");
        expect(contains(debug_run.message, "debug introspection is not available in the sandbox"))
            << debug_run.message.c_str();
        expect(debug_run.violation.category == kimix::string("introspection"));
        auto env_run = sb.run("os.getenv('PATH')");
        expect(contains(env_run.message, "reading the environment is not available in the sandbox"))
            << env_run.message.c_str();
        expect(env_run.violation.category == kimix::string("env"));
        auto module_run = sb.run("require('nope')");
        expect(contains(module_run.message, "module loading is not available in the sandbox"))
            << module_run.message.c_str();
    };

    "pcall_loop_denial_terminates_in_bounded_time"_test = [] {
        Limits limits;
        limits.max_seconds = 0.50;
        limits.hook_instruction_granularity = 1000;
        Sandbox sb(limits);
        const auto started = std::chrono::steady_clock::now();
        auto r = sb.run("while true do local ok = pcall(function() return io.open('x') end) end");
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        expect(r.status == Status::denied) << r.message.c_str();
        expect(elapsed < 3.0) << "the denial has to abort the run, not be caught forever";
    };

    "bytecode_dump_is_denied"_test = [] {
        Sandbox sb(hostile_limits());
        auto r = sb.run("local f = function() return 42 end return load(string.dump(f))");
        expect(r.status == Status::denied) << r.message.c_str();
        expect(r.violation.api == kimix::string("string.dump"));
    };

    "raw_bytecode_chunk_is_refused"_test = [] {
        Sandbox sb(hostile_limits());
        auto r = sb.run("return load('\\27Lua' .. string.rep('\\0', 8))");
        expect(r.status == Status::denied) << r.message.c_str();
        expect(contains(r.message, "LOADERR:")) << r.message.c_str();
        expect(contains(r.message, "binary chunk")) << r.message.c_str();
        expect(r.violation.category == kimix::string("bytecode"));
    };

    "binary_load_mode_is_refused"_test = [] {
        Sandbox sb(hostile_limits());
        auto r = sb.run("return load('return 1', 'c', 'b')");
        expect(r.status == Status::denied) << r.message.c_str();
        expect(r.violation.api == kimix::string("load"));
        auto text_mode = sb.run("local f = load('return 1', 'c', 't') return f()");
        expect(text_mode.ok()) << text_mode.message.c_str();
    };

    "pointer_hygiene"_test = [] {
        Sandbox sb(hostile_limits());
        auto r = sb.run("return tostring({}), tostring(print), tostring(io),"
                        " string.format('%s', {}), string.format('%s', print)");
        expect(r.ok()) << r.message.c_str();
        for (const Value &value : r.results) {
            expect(!script_test::has_long_hex(value.s)) << "an address leaked: " << value.s.c_str();
        }
        if (!r.results.empty()) {
            expect(starts_with(r.results[0].s, "table: #")) << r.results[0].s.c_str();
        }
        if (r.results.size() > 1) {
            expect(starts_with(r.results[1].s, "function: #")) << r.results[1].s.c_str();
        }
        if (r.results.size() > 2) {
            expect(r.results[2].s == kimix::string("io (blocked by sandbox)")) << r.results[2].s.c_str();
        }
    };

    "percent_p_is_denied"_test = [] {
        Sandbox sb(hostile_limits());
        auto r = sb.run("return string.format('%p', print)");
        expect(r.status == Status::denied) << r.message.c_str();
        expect(r.violation.api == kimix::string("string.format"));
    };

    "gc_finalizer_denial_is_surfaced_without_stderr"_test = [] {
        script_test::stderr_capture capture;
        expect(capture.begin());
        Status status = Status::ok;
        kimix::string message;
        {
            Sandbox sb(hostile_limits());
            auto r = sb.run("setmetatable({}, {__gc = function() io.open('x') end}) return 1");
            status = r.status;
            message = r.message;
        }
        const kimix::string leaked = capture.finish();
        expect(status == Status::denied) << message.c_str();
        expect(contains(message, "DENIED: io.open")) << message.c_str();
        if (!leaked.empty()) {
            expect(false) << "a finalizer error reached stderr: " << leaked.c_str();
        }
    };

    "replacing_pcall_is_behaviour"_test = [] {
        Sandbox sb(hostile_limits());
        auto r = sb.run("pcall = function() return true end return 1");
        expect(r.status == Status::behaviour) << r.message.c_str();
        expect(contains(r.message, "BEHAVIOUR:")) << r.message.c_str();
        expect(r.results.empty()) << "results of a tampered run must be dropped";
    };

    "replacing_string_format_is_behaviour"_test = [] {
        Sandbox sb(hostile_limits());
        auto r = sb.run("string.format = function() return 'x' end return 1");
        expect(r.status == Status::behaviour) << r.message.c_str();
    };

    "replacing_a_tripwire_is_behaviour"_test = [] {
        Sandbox sb(hostile_limits());
        auto r = sb.run("io = { open = function() return nil end } return io.open('x')");
        expect(r.status == Status::behaviour) << r.message.c_str();
    };

    "environment_metatable_is_behaviour"_test = [] {
        Sandbox sb(hostile_limits());
        auto r = sb.run("setmetatable(_G, {__index = function() return print end}) return 1");
        expect(r.status == Status::behaviour) << r.message.c_str();
    };

    "os_exit_cannot_kill_the_host"_test = [] {
        Sandbox sb(hostile_limits());
        auto r = sb.run("os.exit(0)");
        expect(r.status == Status::denied) << r.message.c_str();
        /* Reaching this line at all proves the process survived. */
        auto after = sb.run("return 'alive'");
        expect(after.ok()) << after.message.c_str();
    };
}
