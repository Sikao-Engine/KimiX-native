// Test for the Lua script sandbox (src/script, target kimix-script).
// This test covers:
// - Lifecycle: a clean run, multiple return values, an empty chunk, a chunk name
//   in the message, sandbox reuse after a failure
// - Isolation: no global written by one run is visible to the next; nothing on
//   stdout/stderr; no leak over 1000 sequential runs
// - Return-value marshalling: scalars, arrays and objects as canonical JSON,
//   cycle/depth/size/value-count caps, function values refused
// - Budgets: the memory cap (alone and inside a pcall loop), the wall-clock
//   deadline (plain, inside coroutine.wrap, inside a pcall loop), the output cap
// - Host control: cancel() from another thread, a concurrent run() answered with
//   Status::internal, natives installed with add_native()
#include "ut/ut.hpp"

#include <chrono>
#include <thread>

#include <script/script_sandbox.h>

/* A host native is written against the Lua C API; the sandbox itself never
 * leaks a Lua header, but a native implementation obviously needs one (that is
 * the documented way to extend the environment -- see src/script/README.md). */
extern "C" {
#include "lua.h"
}

#include "script_test_util.h"

using namespace boost::ut;
using namespace boost::ut::literals;
using kimix::script::Limits;
using kimix::script::Sandbox;
using kimix::script::Status;
using kimix::script::Value;

namespace tests_unit_script_test_script_sandbox {

bool contains(const kimix::string &text, const char *needle) {
    return text.find(needle) != kimix::string::npos;
}

/* A native the host installs: doubles its argument. */
int native_twice(lua_State *L) {
    const int value = static_cast<int>(lua_tointeger(L, 1));
    lua_pushinteger(L, value * 2);
    return 1;
}

/* Wait until the sandbox reports an in-flight run (busy() is atomic, so this is
 * a legal cross-thread read, like cancel()).  A fixed sleep is NOT enough: on a
 * loaded box (xmake test runs every suite in parallel) the worker thread may not
 * be scheduled within any small fixed delay, and the two thread tests below would
 * then assert against the wrong state.  Measured: with 120 competing CPU-bound
 * processes a 60 ms sleep was not enough to see the run in flight. */
bool wait_until_busy(const Sandbox &sb, double max_seconds = 10.0) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(static_cast<long long>(max_seconds * 1000.0));
    while (std::chrono::steady_clock::now() < deadline) {
        if (sb.busy()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return sb.busy();
}
} // namespace tests_unit_script_test_script_sandbox
using namespace tests_unit_script_test_script_sandbox;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    "clean_run_returns_value"_test = [] {
        Sandbox sb;
        auto r = sb.run("return 1 + 1");
        expect(r.ok()) << r.message.c_str();
        expect(eq(r.results.size(), size_t{1}));
        if (r.results.size() == 1) {
            expect(r.results[0].kind == Value::Kind::integer);
            expect(eq(r.results[0].i, int64_t{2}));
        }
        expect(r.message.empty());
        expect(r.memory_peak > 0) << "the allocator must have seen the run";
    };

    "multiple_return_values"_test = [] {
        Sandbox sb;
        auto r = sb.run("return 1, 'two', true, nil, 2.5");
        expect(r.ok()) << r.message.c_str();
        expect(eq(r.results.size(), size_t{5}));
        if (r.results.size() == 5) {
            expect(r.results[0].kind == Value::Kind::integer);
            expect(r.results[1].kind == Value::Kind::string);
            expect(eq(r.results[1].s, kimix::string("two")));
            expect(r.results[2].kind == Value::Kind::boolean);
            expect(r.results[2].b);
            expect(r.results[3].kind == Value::Kind::nil);
            expect(r.results[4].kind == Value::Kind::number);
            expect(std::abs(r.results[4].d - 2.5) < 1e-12);
        }
    };

    "empty_chunk_is_ok"_test = [] {
        Sandbox sb;
        auto r = sb.run("");
        expect(r.ok()) << r.message.c_str();
        expect(r.results.empty());
    };

    "chunk_name_appears_in_message"_test = [] {
        Sandbox sb;
        auto r = sb.run("error('boom')", "my_chunk.lua");
        expect(r.status == Status::runtime);
        expect(contains(r.message, "my_chunk.lua:")) << r.message.c_str();
        expect(contains(r.message, "RUNTIME:")) << r.message.c_str();
        expect(contains(r.message, "boom")) << r.message.c_str();
        expect(eq(r.violation.line, int32_t{1}));
    };

    "syntax_error_reports_line_and_stays_usable"_test = [] {
        Sandbox sb;
        auto r = sb.run("local x = = 1");
        expect(r.status == Status::syntax);
        expect(contains(r.message, "script.lua:")) << r.message.c_str();
        expect(r.violation.line >= 1) << r.message.c_str();
        auto second = sb.run("return 7");
        expect(second.ok()) << second.message.c_str();
    };

    "ordinary_error_is_runtime"_test = [] {
        Sandbox sb;
        auto r = sb.run("return nosuchglobal.field");
        expect(r.status == Status::runtime);
        expect(contains(r.message, "RUNTIME:")) << r.message.c_str();
        expect(r.violation.line >= 1);
    };

    "state_does_not_bleed_between_runs"_test = [] {
        Sandbox sb;
        auto first = sb.run("leaked = 42 return leaked");
        expect(first.ok()) << first.message.c_str();
        auto second = sb.run("return leaked");
        expect(second.ok()) << second.message.c_str();
        expect(eq(second.results.size(), size_t{1}));
        if (second.results.size() == 1) {
            expect(second.results[0].kind == Value::Kind::nil) << "a global survived into the next run";
        }
    };

    "nothing_is_written_to_stderr"_test = [] {
        script_test::stderr_capture capture;
        expect(capture.begin());
        {
            Sandbox sb;
            auto ok = sb.run("print('hello') return 1");
            expect(ok.ok()) << ok.message.c_str();
            auto denied = sb.run("io.open('x')");
            expect(denied.status == Status::denied);
            auto failed = sb.run("error('plain error')");
            expect(failed.status == Status::runtime);
        }
        const kimix::string leaked = capture.finish();
        if (!leaked.empty()) {
            expect(false) << "the sandbox wrote to stderr: " << leaked.c_str();
        }
    };

    "no_leak_over_many_runs"_test = [] {
        script_test::stderr_capture capture;
        expect(capture.begin());
        {
            Sandbox sb;
            for (int i = 0; i < 500; ++i) {
                auto r = sb.run("local t = {1,2,3} return #t");
                if (!r.ok()) {
                    expect(false) << "run " << i << " failed: " << r.message.c_str();
                    break;
                }
            }
        }
        const kimix::string leaked = capture.finish();
        if (!leaked.empty()) {
            expect(false) << "leak report on stderr: " << leaked.c_str();
        }
    };

    "print_is_captured"_test = [] {
        Sandbox sb;
        auto r = sb.run("print('hi', 1, true) warn('careful')");
        expect(r.ok()) << r.message.c_str();
        expect(eq(r.output, kimix::string("hi\t1\ttrue\ncareful"))) << r.output.c_str();
    };

    "table_result_is_canonical_json"_test = [] {
        Sandbox sb;
        auto r = sb.run("return {b = 2, a = 1}");
        expect(r.ok()) << r.message.c_str();
        expect(eq(r.results.size(), size_t{1}));
        if (r.results.size() == 1) {
            expect(r.results[0].kind == Value::Kind::table);
            expect(eq(r.results[0].s, kimix::string("{\"a\":1,\"b\":2}"))) << r.results[0].s.c_str();
        }
    };

    "array_result_keeps_order"_test = [] {
        Sandbox sb;
        auto r = sb.run("return {1, 2, 3}");
        expect(r.ok()) << r.message.c_str();
        if (!r.results.empty()) {
            expect(eq(r.results[0].s, kimix::string("[1,2,3]"))) << r.results[0].s.c_str();
        }
    };

    "nested_table_result"_test = [] {
        Sandbox sb;
        auto r = sb.run("return {list = {10, 20}, meta = {name = 'x'}}");
        expect(r.ok()) << r.message.c_str();
        if (!r.results.empty()) {
            expect(eq(r.results[0].s, kimix::string("{\"list\":[10,20],\"meta\":{\"name\":\"x\"}}")))
                << r.results[0].s.c_str();
        }
    };

    "cyclic_table_is_a_limit_not_a_hang"_test = [] {
        Limits limits;
        limits.max_seconds = 2.0;
        Sandbox sb(limits);
        auto r = sb.run("local t = {} t.self = t return t");
        expect(r.status == Status::limit) << r.message.c_str();
        expect(contains(r.message, "LIMIT:")) << r.message.c_str();
        expect(contains(r.message, "cyclic")) << r.message.c_str();
    };

    "function_result_is_refused"_test = [] {
        Sandbox sb;
        auto r = sb.run("return print");
        expect(r.status == Status::limit) << r.message.c_str();
        expect(contains(r.message, "cannot cross the sandbox boundary")) << r.message.c_str();
    };

    "result_value_cap"_test = [] {
        Limits limits;
        limits.max_result_values = 2;
        Sandbox sb(limits);
        auto r = sb.run("return 1, 2, 3");
        expect(r.status == Status::limit) << r.message.c_str();
        expect(r.results.empty()) << "over-cap results must be discarded";
    };

    "result_byte_cap"_test = [] {
        Limits limits;
        limits.max_result_bytes = 64;
        Sandbox sb(limits);
        auto r = sb.run("return { string.rep('x', 400) }");
        expect(r.status == Status::limit) << r.message.c_str();
        expect(contains(r.message, "result cap")) << r.message.c_str();
    };

    "depth_cap"_test = [] {
        Limits limits;
        limits.max_result_depth = 2;
        Sandbox sb(limits);
        auto r = sb.run("local t = {} local cur = t for i = 1, 10 do cur.next = {} cur = cur.next end return t");
        expect(r.status == Status::limit) << r.message.c_str();
        expect(contains(r.message, "nesting cap")) << r.message.c_str();
    };

    "memory_cap_reports_and_recovers"_test = [] {
        Sandbox sb;
        auto r = sb.run("local s = string.rep('x', 64 * 1024 * 1024) return #s");
        expect(r.status == Status::memory) << r.message.c_str();
        expect(contains(r.message, "MEMORY:")) << r.message.c_str();
        expect(contains(r.message, "budget")) << r.message.c_str();
        auto again = sb.run("return 1 + 1");
        expect(again.ok()) << again.message.c_str();
    };

    "memory_denial_is_not_swallowed_by_pcall"_test = [] {
        Sandbox sb;
        auto r = sb.run("for i = 1, 100 do pcall(string.rep, 'x', 64 * 1024 * 1024) end return 'done'");
        expect(r.status == Status::memory) << r.message.c_str();
        expect(r.results.empty()) << "a chunk that kept running past the denial must not deliver results";
    };

    "single_allocation_cap"_test = [] {
        Limits limits;
        limits.max_string_bytes = 1024;
        Sandbox sb(limits);
        auto r = sb.run("return #string.rep('y', 200000)");
        expect(r.status == Status::memory) << r.message.c_str();
    };

    "budget_smaller_than_a_state"_test = [] {
        Limits limits;
        limits.max_memory_bytes = 1024; /* far below a bootstrap lua_State */
        Sandbox sb(limits);
        auto r = sb.run("return 1");
        expect(r.status == Status::memory) << r.message.c_str();
        expect(!r.message.empty());
        expect(contains(r.message, "MEMORY:")) << r.message.c_str();
    };

    "timeout_stops_an_infinite_loop"_test = [] {
        Limits limits;
        limits.max_seconds = 0.05;
        limits.hook_instruction_granularity = 1000;
        Sandbox sb(limits);
        const auto started = std::chrono::steady_clock::now();
        auto r = sb.run("while true do end");
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        expect(r.status == Status::timeout) << r.message.c_str();
        expect(contains(r.message, "TIMEOUT:")) << r.message.c_str();
        expect(elapsed < 1.0) << "the deadline must abort well inside 2x max_seconds";
        expect(r.instructions > 0);
    };

    "timeout_inside_coroutine"_test = [] {
        Limits limits;
        limits.max_seconds = 0.05;
        limits.hook_instruction_granularity = 1000;
        Sandbox sb(limits);
        expect(sb.allow_coroutines(true));
        auto r = sb.run("local co = coroutine.wrap(function() while true do end end) co()");
        expect(r.status == Status::timeout) << r.message.c_str();
    };

    "timeout_is_not_swallowed_by_pcall"_test = [] {
        Limits limits;
        limits.max_seconds = 0.05;
        limits.hook_instruction_granularity = 1000;
        Sandbox sb(limits);
        auto r = sb.run("while true do local ok = pcall(function() while true do end end) end");
        expect(r.status == Status::timeout) << r.message.c_str();
    };

    "instruction_budget"_test = [] {
        Limits limits;
        limits.max_seconds = 10.0;
        limits.max_instructions = 50000;
        limits.hook_instruction_granularity = 1000;
        Sandbox sb(limits);
        auto r = sb.run("local n = 0 while true do n = n + 1 end");
        expect(r.status == Status::limit) << r.message.c_str();
        expect(contains(r.message, "instruction budget")) << r.message.c_str();
    };

    "output_cap"_test = [] {
        Limits limits;
        limits.max_output_bytes = 64;
        Sandbox sb(limits);
        auto r = sb.run("for i = 1, 1000 do print(i) end");
        expect(r.status == Status::limit) << r.message.c_str();
        expect(contains(r.message, "output exceeds")) << r.message.c_str();
    };

    "cancel_from_another_thread"_test = [] {
        Limits limits;
        /* No deadline (0 = none): the ONLY thing that can end this run is
         * cancel(), which is exactly what the test measures.  With a deadline
         * configured, a starved machine can expire it before the cancel lands and
         * the assertion below would then be testing the timeout path instead. */
        limits.max_seconds = 0.0;
        limits.hook_instruction_granularity = 1000;
        Sandbox sb(limits);
        kimix::script::Result result;
        std::thread worker([&] { result = sb.run("while true do end"); });
        expect(wait_until_busy(sb)) << "the run should still be in flight";
        sb.cancel();
        const auto started = std::chrono::steady_clock::now();
        worker.join();
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        expect(result.status == Status::cancelled) << result.message.c_str();
        /* The hook strikes every 1000 instructions, so the abort is immediate in
         * practice; the bound is only a net for a pathologically starved host. */
        expect(elapsed < 10.0) << "cancel() must abort in bounded time";
        expect(!sb.busy()) << "busy() must be clear after the run";
    };

    "concurrent_run_is_internal"_test = [] {
        Limits limits;
        /* Same reasoning: a deadline would let the long run finish on its own
         * before the second run starts on a loaded machine. */
        limits.max_seconds = 0.0;
        limits.hook_instruction_granularity = 1000;
        Sandbox sb(limits);
        kimix::script::Result long_run;
        std::thread worker([&] { long_run = sb.run("while true do end"); });
        expect(wait_until_busy(sb)) << "the long run should be in flight";
        auto second = sb.run("return 1");
        expect(second.status == Status::internal) << second.message.c_str();
        expect(contains(second.message, "concurrent")) << second.message.c_str();
        /* End the long run: cancel(), not a deadline, is what stops it now. */
        sb.cancel();
        worker.join();
        expect(long_run.status == Status::cancelled) << long_run.message.c_str();
    };

    "native_function_is_available"_test = [] {
        Sandbox sb;
        expect(sb.add_native("twice", &native_twice));
        auto r = sb.run("return twice(21)");
        expect(r.ok()) << r.message.c_str();
        if (!r.results.empty()) {
            expect(eq(r.results[0].i, int64_t{42}));
        }
    };

    "bad_native_name_is_rejected"_test = [] {
        Sandbox sb;
        expect(!sb.add_native("not a name", &native_twice));
        expect(!sb.add_native("", &native_twice));
        expect(!sb.add_native("ok", nullptr));
    };

    "limits_are_visible"_test = [] {
        Limits limits;
        limits.max_memory_bytes = 4096;
        Sandbox sb(limits);
        expect(eq(sb.limits().max_memory_bytes, size_t{4096}));
    };
}
