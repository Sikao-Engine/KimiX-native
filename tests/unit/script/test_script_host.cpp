// Test for the sandbox HOST SURFACE (src/script/script_host.cpp).
// This test covers:
// - A host function bound with bind_host_function() is called by a script and its
//   result comes back; arguments of every crossable kind (nil/boolean/integer/
//   number/string) arrive with the right Value::Kind and payload, and a table
//   argument arrives as CANONICAL JSON (exact bytes).
// - A function / a coroutine argument is a sticky LIMIT naming the type, which a
//   pcall in the script cannot swallow.
// - Results of every kind reach the script; more results than
//   Limits::max_host_ret_values and more arguments than Limits::max_host_args are
//   sticky LIMITs, as is exhausting Limits::max_host_calls.
// - TABLES CROSS BOTH WAYS (phase N3): a host function may RETURN a
//   Value::Kind::table and the script sees a real Lua table (object -> string
//   keys, array -> 1..n, integers stay integers, `null` -> nil); a table global
//   installed with set_global() is materialised by the run and survives two runs
//   until clear_globals(); a script table survives a round trip through a host
//   function unchanged.
// - The inbound reader is STRICT: trailing garbage, NaN/Infinity, comments,
//   duplicate keys, an empty text, a depth bomb and an over-cap single string are
//   all refused -- as a plain false at set_global() time (host time: never a
//   raise, never a poisoned run) and as a sticky LIMIT naming the reason when a
//   host function's return value is pushed (script time, which a pcall in the
//   script cannot swallow).
// - A host failure is a NON-sticky RUNTIME error: "<chunk>:<line>: RUNTIME: host
//   function '<name>' failed: <error>" when uncaught, catchable by pcall, and a
//   stale error never leaks into the next call.
// - set_global(): every kind, visible in two consecutive runs, gone after
//   clear_globals(), refused for an invalid table text.
// - run(RunSpec): a named call with 0 / 1 / many arguments, a call_name that is
//   not a function, and byte-identical status/results versus run(source, chunk)
//   when call_name is empty.
// - A host that calls run() on its own Sandbox gets Status::internal (no deadlock,
//   the outer run still ends sanely).
// - bind_host_function() validation (empty name, "2bad", "a b", null fn,
//   moved-from Sandbox) and the RESERVED-NAME rule: a name the environment itself
//   installs ("print", "io", "_G", ...) cannot be bound or set, and the reserved
//   table of script_env.cpp covers every name the built environment exposes.
#include "ut/ut.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

/* INTERNAL header: sb_env_name_reserved is what the two host entry points consult.
 * It carries no Lua header, so a test may call it directly. */
#include <script/script_env.h>
#include <script/script_sandbox.h>

#include "script_test_util.h"

using namespace boost::ut;
using namespace boost::ut::literals;
using kimix::script::HostCall;
using kimix::script::Limits;
using kimix::script::Result;
using kimix::script::Sandbox;
using kimix::script::Status;
using kimix::script::Value;
using RunSpec = kimix::script::Sandbox::RunSpec;

namespace tests_unit_script_test_script_host {

bool contains(const kimix::string &text, const char *needle) {
    return text.find(needle) != kimix::string::npos;
}

bool starts_with(const kimix::string &text, const char *prefix) {
    const size_t len = std::strlen(prefix);
    return text.size() >= len && std::memcmp(text.data(), prefix, len) == 0;
}

Value int_value(int64_t v) {
    Value value;
    value.kind = Value::Kind::integer;
    value.i = v;
    value.d = static_cast<double>(v);
    return value;
}

Value string_value(const char *text) {
    Value value;
    value.kind = Value::Kind::string;
    value.s.assign(text);
    return value;
}

/* A Kind::table result/value carrying the canonical JSON text in `text`. */
Value table_value(const char *text) {
    Value value;
    value.kind = Value::Kind::table;
    value.s.assign(text);
    return value;
}

/* Everything a host callback is allowed to remember between calls: copies only,
 * never a pointer into the interpreter. */
struct host_probe {
    int calls = 0;
    uint64_t last_index = 0;
    kimix::string last_name;
    size_t last_argc = 0;
    kimix::vector<Value::Kind> kinds;
    kimix::vector<kimix::string> payloads; /* Value::s per argument */
    kimix::string error;                   /* the text a failing host reports */
    kimix::string table_text;              /* the canonical JSON a table result carries */
    Status inner_status = Status::ok;      /* for the re-entrant run() case */
    kimix::string inner_message;
    Sandbox *self = nullptr;

    void record(const HostCall &call) {
        ++calls;
        last_index = call.index;
        last_name.assign(call.name.data(), call.name.size());
        last_argc = call.argc;
        kinds.clear();
        payloads.clear();
        for (size_t i = 0; i < call.argc; ++i) {
            kinds.push_back(call.args[i].kind);
            payloads.push_back(call.args[i].s);
        }
    }
};

/* "<kind>=<payload>" per argument, joined by '|', so one string compare checks every
 * marshalled field at once. */
void append_summary(kimix::string &out, const Value &value) {
    out.append(Value::kind_name(value.kind));
    char text[64];
    switch (value.kind) {
    case Value::Kind::nil:
        out.append("=-");
        return;
    case Value::Kind::boolean:
        out.append(value.b ? "=true" : "=false");
        return;
    case Value::Kind::integer:
        std::snprintf(text, sizeof(text), "=%lld", static_cast<long long>(value.i));
        out.append(text);
        return;
    case Value::Kind::number:
        std::snprintf(text, sizeof(text), "=%.17g", value.d);
        out.append(text);
        return;
    case Value::Kind::string:
    case Value::Kind::table:
        out.push_back('=');
        out.append(value.s.data(), value.s.size());
        return;
    }
}

/* add(a, b) -> a + b */
bool host_add(void *user, HostCall &call) {
    host_probe *probe = static_cast<host_probe *>(user);
    probe->record(call);
    if (call.argc != 2 || call.args[0].kind != Value::Kind::integer ||
        call.args[1].kind != Value::Kind::integer) {
        call.error.assign("expected two integers");
        return false;
    }
    if (call.ret_cap < 1) {
        call.error.assign("no result slot");
        return false;
    }
    call.rets[0] = int_value(call.args[0].i + call.args[1].i);
    call.produced = 1;
    return true;
}

/* echo(...) -> one string summarising every argument. */
bool host_echo(void *user, HostCall &call) {
    host_probe *probe = static_cast<host_probe *>(user);
    probe->record(call);
    if (call.ret_cap < 1) {
        call.error.assign("no result slot");
        return false;
    }
    kimix::string summary;
    for (size_t i = 0; i < call.argc; ++i) {
        if (i > 0) {
            summary.push_back('|');
        }
        append_summary(summary, call.args[i]);
    }
    call.rets[0] = string_value(summary.c_str());
    call.produced = 1;
    return true;
}

/* give() -> one result of every crossable kind. */
bool host_give(void *user, HostCall &call) {
    host_probe *probe = static_cast<host_probe *>(user);
    probe->record(call);
    if (call.ret_cap < 5) {
        call.error.assign("not enough slots");
        return false;
    }
    call.rets[0] = int_value(7);
    call.rets[1].kind = Value::Kind::number;
    call.rets[1].d = 2.5;
    call.rets[2].kind = Value::Kind::boolean;
    call.rets[2].b = true;
    call.rets[3] = string_value("x");
    call.rets[4] = Value{}; /* nil */
    call.produced = 5;
    return true;
}

/* flood() -> reports MORE results than the slots it was given (a host bug the
 * trampoline has to catch; the third value is deliberately not written, because
 * writing past ret_cap would be this callback's own overflow). */
bool host_flood(void *user, HostCall &call) {
    host_probe *probe = static_cast<host_probe *>(user);
    probe->record(call);
    if (call.ret_cap >= 2) {
        call.rets[0] = int_value(1);
        call.rets[1] = int_value(2);
    }
    call.produced = 3;
    return true;
}

/* boom() -> always fails with probe->error ("nope" in the tests). */
bool host_boom(void *user, HostCall &call) {
    host_probe *probe = static_cast<host_probe *>(user);
    probe->record(call);
    call.error.assign(probe->error.data(), probe->error.size());
    return false;
}

/* tick() -> 1 (used to count calls / to be installed under a plain name). */
bool host_tick(void *user, HostCall &call) {
    host_probe *probe = static_cast<host_probe *>(user);
    if (probe != nullptr) {
        probe->record(call);
    }
    if (call.ret_cap < 1) {
        call.error.assign("no result slot");
        return false;
    }
    call.rets[0] = int_value(1);
    call.produced = 1;
    return true;
}

/* choppy() -> fails on its first call, succeeds afterwards (stale-error probe). */
bool host_choppy(void *user, HostCall &call) {
    host_probe *probe = static_cast<host_probe *>(user);
    probe->record(call);
    if (call.index == 1) {
        call.error.assign("boom");
        return false;
    }
    if (call.ret_cap < 1) {
        call.error.assign("no result slot");
        return false;
    }
    call.rets[0] = string_value("fine");
    call.produced = 1;
    return true;
}

/* give_table() -> one Value::Kind::table carrying probe->table_text, i.e. the
 * inbound direction of the boundary (canonical JSON -> a real Lua table). */
bool host_give_table(void *user, HostCall &call) {
    host_probe *probe = static_cast<host_probe *>(user);
    probe->record(call);
    if (call.ret_cap < 1) {
        call.error.assign("no result slot");
        return false;
    }
    call.rets[0] = table_value(probe->table_text.c_str());
    call.produced = 1;
    return true;
}

/* roundtrip(t) -> the canonical JSON the sandbox made of `t`, handed straight back
 * as a table result, so the script can prove the two directions agree. */
bool host_roundtrip(void *user, HostCall &call) {
    host_probe *probe = static_cast<host_probe *>(user);
    probe->record(call);
    if (call.argc != 1 || call.args[0].kind != Value::Kind::table) {
        call.error.assign("expected one table argument");
        return false;
    }
    if (call.ret_cap < 1) {
        call.error.assign("no result slot");
        return false;
    }
    call.rets[0] = table_value(call.args[0].s.c_str());
    call.produced = 1;
    return true;
}

/* inner() -> runs a script on the Sandbox that is calling it right now. */
bool host_inner_run(void *user, HostCall &call) {
    host_probe *probe = static_cast<host_probe *>(user);
    probe->record(call);
    const Result inner = probe->self->run("return 1");
    probe->inner_status = inner.status;
    probe->inner_message.assign(inner.message.data(), inner.message.size());
    if (call.ret_cap < 1) {
        call.error.assign("no result slot");
        return false;
    }
    call.rets[0] = string_value("inner=internal");
    call.produced = 1;
    return true;
}

bool same_results(const Result &a, const Result &b) {
    if (a.results.size() != b.results.size()) {
        return false;
    }
    for (size_t i = 0; i < a.results.size(); ++i) {
        const Value &x = a.results[i];
        const Value &y = b.results[i];
        if (x.kind != y.kind || x.b != y.b || x.i != y.i || x.d != y.d || x.s != y.s) {
            return false;
        }
    }
    return true;
}

/* Every name reachable in _ENV, sorted, one per line. */
const char *const kEnvNameList = "local names = {}\n"
                                 "for k in next, _G do\n"
                                 "  if type(k) == 'string' then names[#names + 1] = k end\n"
                                 "end\n"
                                 "table.sort(names)\n"
                                 "return table.concat(names, '\\n')\n";

/* Assert that the reserved-name table of script_env.cpp covers the environment the
 * sandbox actually built (empty string in *missing when it does not). */
bool every_name_reserved(Sandbox &sb, kimix::string *missing) {
    const Result r = sb.run(kEnvNameList, "surface.lua");
    if (!r.ok() || r.results.empty() || r.results[0].kind != Value::Kind::string) {
        *missing = kimix::string("<the enumerator did not return a string>");
        return false;
    }
    const kimix::string &list = r.results[0].s;
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find('\n', start);
        if (end == kimix::string::npos) {
            end = list.size();
        }
        const kimix::string name = list.substr(start, end - start);
        if (!name.empty() && !kimix::script::sb_env_name_reserved(name)) {
            *missing = name;
            return false;
        }
        start = end + 1;
    }
    return true;
}

} // namespace tests_unit_script_test_script_host
using namespace tests_unit_script_test_script_host;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    "host_function_result_reaches_the_script"_test = [] {
        host_probe probe;
        Sandbox sb;
        expect(sb.bind_host_function("add", &host_add, &probe));
        auto r = sb.run("return add(2, 3), type(add), tostring(add)");
        expect(r.ok()) << r.message.c_str();
        expect(eq(r.results.size(), size_t{3}));
        if (r.results.size() == 3) {
            expect(r.results[0].kind == Value::Kind::integer);
            expect(eq(r.results[0].i, int64_t{5})) << "the host result must reach the script";
            expect(eq(r.results[1].s, kimix::string("function")));
            /* The binding is a C closure with private upvalues, never a pointer the
             * script could see or dump: tostring() renders it redacted. */
            expect(starts_with(r.results[2].s, "function: #")) << r.results[2].s.c_str();
            expect(!script_test::has_long_hex(r.results[2].s)) << r.results[2].s.c_str();
        }
        expect(eq(probe.calls, 1));
        expect(eq(probe.last_index, uint64_t{1})) << "the first call of a run is ordinal 1";
        expect(eq(probe.last_name, kimix::string("add")));
        expect(eq(probe.last_argc, size_t{2}));
    };

    "argument_of_every_crossable_kind"_test = [] {
        host_probe probe;
        Sandbox sb;
        expect(sb.bind_host_function("echo", &host_echo, &probe));
        auto r = sb.run("return echo(nil, true, 42, 1.5, 'two')");
        expect(r.ok()) << r.message.c_str();
        expect(eq(r.results.size(), size_t{1}));
        if (r.results.size() == 1) {
            expect(eq(r.results[0].s, kimix::string("nil=-|boolean=true|integer=42|number=1.5|string=two")))
                << r.results[0].s.c_str();
        }
        /* ...and the harness sees the same kinds the summary was built from. */
        expect(eq(probe.last_argc, size_t{5}));
        expect(eq(probe.kinds.size(), size_t{5}));
        if (probe.kinds.size() == 5) {
            expect(probe.kinds[0] == Value::Kind::nil);
            expect(probe.kinds[1] == Value::Kind::boolean);
            expect(probe.kinds[2] == Value::Kind::integer);
            expect(probe.kinds[3] == Value::Kind::number);
            expect(probe.kinds[4] == Value::Kind::string);
            expect(eq(probe.payloads[2], kimix::string("")));
            expect(eq(probe.payloads[4], kimix::string("two")));
        }
        /* An integer stays exact and a float stays a float (no silent widening). */
        auto exact = sb.run("return echo(9007199254740993, 1.25)");
        expect(exact.ok()) << exact.message.c_str();
        if (!exact.results.empty()) {
            expect(eq(exact.results[0].s, kimix::string("integer=9007199254740993|number=1.25")))
                << exact.results[0].s.c_str();
        }
    };

    "table_argument_arrives_as_canonical_json"_test = [] {
        host_probe probe;
        Sandbox sb;
        expect(sb.bind_host_function("echo", &host_echo, &probe));
        auto r = sb.run("return echo({b = {true, false}, a = 1})");
        expect(r.ok()) << r.message.c_str();
        expect(eq(r.results.size(), size_t{1}));
        if (r.results.size() == 1) {
            expect(eq(r.results[0].s, kimix::string("table={\"a\":1,\"b\":[true,false]}")))
                << r.results[0].s.c_str();
        }
        /* the host received exactly those bytes, as Kind::table */
        expect(eq(probe.kinds.size(), size_t{1}));
        if (probe.kinds.size() == 1) {
            expect(probe.kinds[0] == Value::Kind::table);
            expect(eq(probe.payloads[0], kimix::string("{\"a\":1,\"b\":[true,false]}"))) << probe.payloads[0].c_str();
        }
        /* canonical = minified + byte-sorted keys; inf becomes a JSON null */
        auto sorted = sb.run("return echo({n = math.huge, z = 'x', a = 2})");
        expect(sorted.ok()) << sorted.message.c_str();
        if (!sorted.results.empty()) {
            expect(eq(sorted.results[0].s, kimix::string("table={\"a\":2,\"n\":null,\"z\":\"x\"}")))
                << sorted.results[0].s.c_str();
        }
    };

    /* ---- tables crossing IN (phase N3: the strict JSON reader) --------------- */

    "table_result_reaches_the_script_as_a_table"_test = [] {
        host_probe probe;
        probe.table_text.assign("{\"a\":1,\"b\":[true,null],\"c\":\"x\"}");
        Sandbox sb;
        expect(sb.bind_host_function("give_table", &host_give_table, &probe));
        auto r = sb.run("local t = give_table()\n"
                        "return type(t), t.a, t.b[1], t.b[2] == nil, t.c, #t.b, t.a + 1");
        expect(r.ok()) << r.message.c_str();
        expect(eq(r.results.size(), size_t{7}));
        if (r.results.size() == 7) {
            expect(eq(r.results[0].s, kimix::string("table"))) << "the result must be a real Lua table";
            expect(r.results[1].kind == Value::Kind::integer) << "a JSON integer stays an integer";
            expect(eq(r.results[1].i, int64_t{1}));
            expect(r.results[2].kind == Value::Kind::boolean);
            expect(r.results[2].b) << "t.b[1] must be true";
            expect(r.results[3].kind == Value::Kind::boolean);
            expect(r.results[3].b) << "a JSON null must arrive as nil";
            expect(eq(r.results[4].s, kimix::string("x")));
            /* `#t.b` is 1, not 2: a JSON null becomes a HOLE, and a Lua sequence
             * border stops in front of a hole ([true, null] has no border at 2 --
             * a table cannot hold nil).  The element is not lost: index 3 of
             * [true, null, "z"] is still 3. */
            expect(eq(r.results[5].i, int64_t{1})) << "a null hole shortens the # border";
            expect(eq(r.results[6].i, int64_t{2})) << "the pushed value is a plain, mutable Lua table";
        }
        /* An array keeps its indices around an interior null. */
        probe.table_text.assign("{\"xs\":[1,null,3]}");
        auto holes = sb.run("local t = give_table() return t.xs[1], t.xs[2] == nil, t.xs[3]");
        expect(holes.ok()) << holes.message.c_str();
        expect(eq(holes.results.size(), size_t{3}));
        if (holes.results.size() == 3) {
            expect(eq(holes.results[0].i, int64_t{1}));
            expect(holes.results[1].kind == Value::Kind::boolean);
            expect(holes.results[1].b);
            expect(eq(holes.results[2].i, int64_t{3})) << "an interior null must not shift the indices";
        }
    };

    "table_global_survives_two_runs_and_clear_globals"_test = [] {
        Sandbox sb;
        expect(sb.set_global("cfg", table_value("{\"a\":1,\"nested\":{\"b\":[1,2,3]},\"s\":\"x\"}")))
            << "a table global is accepted (canonical JSON text)";

        auto first = sb.run("return type(cfg), cfg.a, cfg.nested.b[3], cfg.s, #cfg.nested.b");
        expect(first.ok()) << first.message.c_str();
        expect(eq(first.results.size(), size_t{5}));
        if (first.results.size() == 5) {
            expect(eq(first.results[0].s, kimix::string("table")));
            expect(eq(first.results[1].i, int64_t{1}));
            expect(eq(first.results[2].i, int64_t{3}));
            expect(eq(first.results[3].s, kimix::string("x")));
            expect(eq(first.results[4].i, int64_t{3}));
        }

        /* A global is a copy that outlives the run that used it: the SECOND run of
         * the same Sandbox still sees it (state is otherwise per run). */
        auto second = sb.run("return cfg.a + cfg.nested.b[3]");
        expect(second.ok()) << second.message.c_str();
        if (!second.results.empty()) {
            expect(eq(second.results[0].i, int64_t{4}));
        }

        /* A top-level ARRAY text, and the documented `null` rule: an object key
         * whose value is null disappears (a Lua table cannot hold nil). */
        expect(sb.set_global("list", table_value("[1,2,3]")));
        expect(sb.set_global("holes", table_value("{\"a\":null,\"b\":2}")));
        auto shapes = sb.run("return type(list), #list, list[3], holes.a == nil, holes.b");
        expect(shapes.ok()) << shapes.message.c_str();
        expect(eq(shapes.results.size(), size_t{5}));
        if (shapes.results.size() == 5) {
            expect(eq(shapes.results[0].s, kimix::string("table")));
            expect(eq(shapes.results[1].i, int64_t{3})) << "an array root becomes 1..n";
            expect(eq(shapes.results[2].i, int64_t{3}));
            expect(shapes.results[3].kind == Value::Kind::boolean);
            expect(shapes.results[3].b) << "an object key with a null value must be absent";
            expect(eq(shapes.results[4].i, int64_t{2}));
        }

        sb.clear_globals();
        auto cleared = sb.run("return type(cfg), type(list), type(holes)");
        expect(cleared.ok()) << cleared.message.c_str();
        for (const Value &value : cleared.results) {
            expect(eq(value.s, kimix::string("nil"))) << "clear_globals() removes a table global too";
        }
    };

    "hostile_table_text_is_refused_at_set_global"_test = [] {
        Sandbox sb;
        static const char *const kBad[] = {
            "{\"a\":1} x",       /* trailing garbage */
            "{\"a\":NaN}",       /* no NaN literal in strict JSON */
            "{\"a\":Infinity}",  /* ...nor Infinity */
            "{\"a\":-Infinity}", /* ...in either sign */
            "{\"a\":1 /*x*/}",   /* no comments */
            "{\"a\":1,}",        /* no trailing comma */
            "{\"a\":1,\"a\":2}", /* duplicate key: canonical JSON never has one */
            "",                  /* empty */
            "not json",
            nullptr,
        };
        for (int i = 0; kBad[i] != nullptr; ++i) {
            expect(!sb.set_global("cfg", table_value(kBad[i]))) << "accepted a malformed text: " << kBad[i];
        }

        /* A depth bomb: 200 nested arrays, far past Limits::max_result_depth (32).
         * It must be refused, not overflow the native stack. */
        Value bomb;
        bomb.kind = Value::Kind::table;
        for (int i = 0; i < 200; ++i) {
            bomb.s.push_back('[');
        }
        bomb.s.append("1");
        for (int i = 0; i < 200; ++i) {
            bomb.s.push_back(']');
        }
        expect(!sb.set_global("deep", bomb)) << "a 200-level depth bomb must be refused";

        /* Nothing was installed, and no run was poisoned: the Sandbox is intact. */
        auto r = sb.run("return type(cfg), type(deep), 'alive'");
        expect(r.ok()) << r.message.c_str() << " (a refused text must not poison the run)";
        expect(eq(r.results.size(), size_t{3}));
        if (r.results.size() == 3) {
            expect(eq(r.results[0].s, kimix::string("nil")));
            expect(eq(r.results[1].s, kimix::string("nil")));
            expect(eq(r.results[2].s, kimix::string("alive")));
        }

        /* Control: the same name accepts a well-formed text, so "refused" is about
         * the text and not about the name. */
        expect(sb.set_global("cfg", table_value("{\"a\":7}")));
        auto ok = sb.run("return type(cfg), cfg.a");
        expect(ok.ok()) << ok.message.c_str();
        expect(eq(ok.results.size(), size_t{2}));
        if (ok.results.size() == 2) {
            expect(eq(ok.results[0].s, kimix::string("table")));
            expect(eq(ok.results[1].i, int64_t{7}));
        }
    };

    "hostile_table_result_is_a_sticky_limit"_test = [] {
        host_probe probe;
        probe.table_text.assign("{\"a\":1,\"a\":2}");
        Sandbox sb;
        expect(sb.bind_host_function("bad", &host_give_table, &probe));

        auto direct = sb.run("return bad()");
        expect(direct.status == Status::limit) << direct.message.c_str();
        expect(contains(direct.message, "duplicate object key 'a'")) << direct.message.c_str();
        expect(direct.violation.kind == Status::limit) << "a host-supplied table text is script time, so it is sticky";
        expect(direct.results.empty()) << "a denied run must not deliver results";

        /* The script's own pcall must not turn the denial into a value. */
        auto caught = sb.run("return pcall(function() return bad() end)");
        expect(caught.status == Status::limit) << caught.message.c_str();
        expect(caught.results.empty()) << "a pcall must not swallow a sticky denial";

        /* A parse failure names the position, and the message keeps the LIMIT tag. */
        probe.table_text.assign("{\"a\":1} x");
        auto garbage = sb.run("return bad()");
        expect(garbage.status == Status::limit) << garbage.message.c_str();
        expect(contains(garbage.message, "LIMIT: the JSON text is not valid strict JSON")) << garbage.message.c_str();
        expect(contains(garbage.message, "byte 8")) << garbage.message.c_str();

        /* Only the RUN was poisoned: the Sandbox still works once the host hands it
         * a text it can honour. */
        probe.table_text.assign("{\"a\":5}");
        auto after = sb.run("local t = bad() return type(t), t.a");
        expect(after.ok()) << after.message.c_str();
        expect(eq(after.results.size(), size_t{2}));
        if (after.results.size() == 2) {
            expect(eq(after.results[0].s, kimix::string("table")));
            expect(eq(after.results[1].i, int64_t{5}));
        }
    };

    "table_round_trip_through_a_host_function"_test = [] {
        host_probe probe;
        Sandbox sb;
        expect(sb.bind_host_function("roundtrip", &host_roundtrip, &probe));
        auto r = sb.run("local t = {a = 1, b = {true, false}, s = 'x'}\n"
                        "local back = roundtrip(t)\n"
                        "return type(back), back.a, back.b[2], back.s, #back.b");
        expect(r.ok()) << r.message.c_str();
        expect(eq(r.results.size(), size_t{5}));
        if (r.results.size() == 5) {
            expect(eq(r.results[0].s, kimix::string("table")));
            expect(eq(r.results[1].i, int64_t{1})) << "t.a must survive the round trip";
            expect(r.results[2].kind == Value::Kind::boolean);
            expect(!r.results[2].b);
            expect(eq(r.results[3].s, kimix::string("x")));
            expect(eq(r.results[4].i, int64_t{2}));
        }
        /* The host saw exactly the canonical JSON the writer produced. */
        expect(eq(probe.kinds.size(), size_t{1}));
        if (probe.kinds.size() == 1) {
            expect(probe.kinds[0] == Value::Kind::table);
            expect(eq(probe.payloads[0], kimix::string("{\"a\":1,\"b\":[true,false],\"s\":\"x\"}")))
                << probe.payloads[0].c_str();
        }
        expect(eq(probe.calls, 1));
    };

    "over_cap_json_string_is_refused"_test = [] {
        Limits limits;
        limits.max_string_bytes = 4096;
        Sandbox sb(limits);

        Value big;
        big.kind = Value::Kind::table;
        big.s.assign("{\"s\":\"");
        for (int i = 0; i < 8000; ++i) {
            big.s.push_back('x');
        }
        big.s.append("\"}");
        expect(!sb.set_global("big", big)) << "a single string over Limits::max_string_bytes is refused";

        /* At script time the same text is a sticky LIMIT naming the string cap. */
        host_probe probe;
        probe.table_text = big.s;
        expect(sb.bind_host_function("big_table", &host_give_table, &probe));
        auto r = sb.run("return big_table()");
        expect(r.status == Status::limit) << r.message.c_str();
        expect(contains(r.message, "string cap")) << r.message.c_str();
        expect(r.results.empty());

        /* The Sandbox is still usable. */
        auto ok = sb.run("return 'fine'");
        expect(ok.ok()) << ok.message.c_str();
    };

    "reserved_names_still_refuse_tables"_test = [] {
        host_probe probe;
        probe.table_text.assign("{\"a\":1}");
        Sandbox sb;
        static const char *const kReserved[] = {"print", "_G", "io", "table", "pcall", nullptr};
        for (int i = 0; kReserved[i] != nullptr; ++i) {
            const char *name = kReserved[i];
            expect(!sb.set_global(name, table_value("{\"a\":1}"))) << "set a reserved name with a table: " << name;
            expect(!sb.bind_host_function(name, &host_give_table, &probe)) << "bound a reserved name: " << name;
        }
        expect(eq(probe.calls, 0));

        /* Nothing was installed and no capability was deleted. */
        auto intact = sb.run("print('kept') return type(print), type(table), type(_G), type(io)");
        expect(intact.ok()) << intact.message.c_str();
        expect(eq(intact.output, kimix::string("kept\n")));
        expect(eq(intact.results.size(), size_t{4}));
        if (intact.results.size() == 4) {
            expect(eq(intact.results[0].s, kimix::string("function")));
            expect(eq(intact.results[1].s, kimix::string("table")));
            expect(eq(intact.results[2].s, kimix::string("table")));
            expect(eq(intact.results[3].s, kimix::string("table")));
        }

        /* A name that merely looks similar still takes a table. */
        expect(sb.set_global("printf", table_value("{\"a\":1}")));
        auto mine = sb.run("return type(printf), printf.a");
        expect(mine.ok()) << mine.message.c_str();
        expect(eq(mine.results.size(), size_t{2}));
        if (mine.results.size() == 2) {
            expect(eq(mine.results[0].s, kimix::string("table")));
            expect(eq(mine.results[1].i, int64_t{1}));
        }
    };

    "non_crossable_argument_is_a_sticky_limit"_test = [] {
        host_probe probe;
        Sandbox sb;
        expect(sb.bind_host_function("echo", &host_echo, &probe));
        auto direct = sb.run("return echo(print)");
        expect(direct.status == Status::limit) << direct.message.c_str();
        expect(contains(direct.message, "argument of type 'function' cannot cross the sandbox boundary"))
            << direct.message.c_str();
        expect(direct.violation.kind == Status::limit) << direct.message.c_str();
        expect(direct.results.empty()) << "a denied run must not deliver results";

        /* The script's own pcall must not turn a budget denial into a value. */
        auto caught = sb.run("return pcall(function() return echo(print) end)");
        expect(caught.status == Status::limit) << caught.message.c_str();
        expect(caught.results.empty()) << "a pcall must not swallow a sticky denial";

        /* A coroutine object is a thread: same rule, message names the type. */
        expect(sb.allow_coroutines(true));
        auto thread = sb.run("local co = coroutine.create(function() end)\n"
                             "return pcall(function() return echo(co) end)");
        expect(thread.status == Status::limit) << thread.message.c_str();
        expect(contains(thread.message, "argument of type 'thread'")) << thread.message.c_str();

        expect(eq(probe.calls, 0)) << "the host must never run for a refused argument";
    };

    "results_of_every_kind_reach_the_script"_test = [] {
        host_probe probe;
        Sandbox sb;
        expect(sb.bind_host_function("give", &host_give, &probe));
        auto r = sb.run("return give()");
        expect(r.ok()) << r.message.c_str();
        expect(eq(r.results.size(), size_t{5}));
        if (r.results.size() == 5) {
            expect(r.results[0].kind == Value::Kind::integer);
            expect(eq(r.results[0].i, int64_t{7}));
            expect(r.results[1].kind == Value::Kind::number);
            expect(std::abs(r.results[1].d - 2.5) < 1e-12);
            expect(r.results[2].kind == Value::Kind::boolean);
            expect(r.results[2].b);
            expect(r.results[3].kind == Value::Kind::string);
            expect(eq(r.results[3].s, kimix::string("x")));
            expect(r.results[4].kind == Value::Kind::nil);
        }
        /* The values are real Lua values, not text: arithmetic on them works. */
        auto used = sb.run("local a, b, c, d, e = give()\n"
                           "return a + 1, b * 2, not c, d .. '!', e == nil");
        expect(used.ok()) << used.message.c_str();
        expect(eq(used.results.size(), size_t{5}));
        if (used.results.size() == 5) {
            expect(eq(used.results[0].i, int64_t{8}));
            expect(used.results[1].kind == Value::Kind::number);
            expect(std::abs(used.results[1].d - 5.0) < 1e-12);
            expect(used.results[2].kind == Value::Kind::boolean);
            expect(!used.results[2].b);
            expect(eq(used.results[3].s, kimix::string("x!")));
            expect(used.results[4].kind == Value::Kind::boolean);
            expect(used.results[4].b);
        }
    };

    "ret_value_cap_is_a_limit"_test = [] {
        Limits limits;
        limits.max_host_ret_values = 2;
        Sandbox sb(limits);
        host_probe probe;
        expect(sb.bind_host_function("flood", &host_flood, &probe));
        auto r = sb.run("return flood()");
        expect(r.status == Status::limit) << r.message.c_str();
        expect(contains(r.message, "host function produced 3 result(s); the sandbox allows 2")) << r.message.c_str();
        expect(r.results.empty());
        expect(eq(probe.calls, 1));
    };

    "argument_count_cap_is_a_limit"_test = [] {
        Limits limits;
        limits.max_host_args = 2;
        Sandbox sb(limits);
        host_probe probe;
        expect(sb.bind_host_function("echo", &host_echo, &probe));
        auto r = sb.run("return echo(1, 2, 3)");
        expect(r.status == Status::limit) << r.message.c_str();
        expect(contains(r.message, "host function received 3 argument(s); the sandbox allows 2"))
            << r.message.c_str();
        expect(eq(probe.calls, 0)) << "the host must not run with an over-cap argument list";
        /* exactly at the cap is fine */
        auto ok = sb.run("return echo(1, 2)");
        expect(ok.ok()) << ok.message.c_str();
        if (!ok.results.empty()) {
            expect(eq(ok.results[0].s, kimix::string("integer=1|integer=2"))) << ok.results[0].s.c_str();
        }
    };

    "host_failure_is_a_non_sticky_runtime_error"_test = [] {
        host_probe probe;
        probe.error.assign("nope");
        Sandbox sb;
        expect(sb.bind_host_function("boom", &host_boom, &probe));

        /* Uncaught: the documented shape, with the chunk:line prefix. */
        auto uncaught = sb.run("return boom()");
        expect(uncaught.status == Status::runtime) << uncaught.message.c_str();
        expect(starts_with(uncaught.message, "script.lua:1: ")) << uncaught.message.c_str();
        expect(contains(uncaught.message, "RUNTIME: host function 'boom' failed: nope")) << uncaught.message.c_str();
        expect(uncaught.violation.kind == Status::ok)
            << "a host failure must not be recorded as a sticky violation";

        /* Caught: the script's own pcall sees the text and the run continues. */
        auto caught = sb.run("local ok, msg = pcall(boom)\n"
                            "return ok, msg, 'continued'");
        expect(caught.ok()) << caught.message.c_str();
        expect(eq(caught.results.size(), size_t{3}));
        if (caught.results.size() == 3) {
            expect(caught.results[0].kind == Value::Kind::boolean);
            expect(!caught.results[0].b);
            expect(contains(caught.results[1].s, "host function 'boom' failed: nope")) << caught.results[1].s.c_str();
            expect(eq(caught.results[2].s, kimix::string("continued")));
        }
        expect(eq(probe.calls, 2));
    };

    "host_call_budget_is_sticky"_test = [] {
        Limits limits;
        limits.max_host_calls = 3;
        Sandbox sb(limits);
        host_probe probe;
        expect(sb.bind_host_function("tick", &host_tick, &probe));
        /* The pcall loop is the point: spending the budget is final. */
        auto r = sb.run("for i = 1, 5 do pcall(tick) end return 'done'");
        expect(r.status == Status::limit) << r.message.c_str();
        expect(contains(r.message, "host call budget exhausted (3 calls)")) << r.message.c_str();
        expect(r.violation.kind == Status::limit) << r.message.c_str();
        expect(r.results.empty()) << "a run that kept going past the denial delivers nothing";
        expect(eq(probe.calls, 3)) << "the host must be entered exactly max_host_calls times";

        /* A fresh run gets the budget back. */
        auto again = sb.run("return tick()");
        expect(again.ok()) << again.message.c_str();
        expect(eq(probe.calls, 4));
        expect(eq(probe.last_index, uint64_t{1})) << "the ordinal restarts with every run";
    };

    "globals_of_every_kind_survive_two_runs_and_clear"_test = [] {
        Sandbox sb;
        Value integer = int_value(42);
        Value text = string_value("hi");
        Value flag;
        flag.kind = Value::Kind::boolean;
        flag.b = true;
        Value real;
        real.kind = Value::Kind::number;
        real.d = 2.5;
        Value empty; /* Kind::nil */
        expect(sb.set_global("my_int", integer));
        expect(sb.set_global("my_str", text));
        expect(sb.set_global("my_flag", flag));
        expect(sb.set_global("my_num", real));
        expect(sb.set_global("my_nil", empty));

        auto first = sb.run("return type(my_int), my_int, my_str, my_flag, my_num, type(my_nil)");
        expect(first.ok()) << first.message.c_str();
        expect(eq(first.results.size(), size_t{6}));
        if (first.results.size() == 6) {
            expect(eq(first.results[0].s, kimix::string("number")));
            expect(eq(first.results[1].i, int64_t{42}));
            expect(eq(first.results[2].s, kimix::string("hi")));
            expect(first.results[3].kind == Value::Kind::boolean);
            expect(first.results[3].b);
            expect(first.results[4].kind == Value::Kind::number);
            expect(std::abs(first.results[4].d - 2.5) < 1e-12);
            expect(eq(first.results[5].s, kimix::string("nil")));
        }

        /* A global is a copy that outlives the run that used it: the SECOND run of
         * the same Sandbox still sees it (state is otherwise per run). */
        auto second = sb.run("return my_int + 1");
        expect(second.ok()) << second.message.c_str();
        if (!second.results.empty()) {
            expect(eq(second.results[0].i, int64_t{43}));
        }

        /* A table crosses as its canonical JSON text, validated once at host time
         * (a standalone strict parse: no lua_State yet) and materialised by the
         * run's install step. */
        expect(sb.set_global("my_table", table_value("{\"a\":1,\"b\":[true,false]}")))
            << "a table global with a valid text is accepted";
        /* A text that is not strict JSON is a plain false: host time, no raise, and
         * nothing poisoned for the next run. */
        expect(!sb.set_global("my_broken", table_value("{\"a\":1} x")))
            << "a malformed table text is refused, not raised";
        auto with_table = sb.run("return type(my_table), my_table.a, my_table.b[2], type(my_broken)");
        expect(with_table.ok()) << with_table.message.c_str();
        expect(eq(with_table.results.size(), size_t{4}));
        if (with_table.results.size() == 4) {
            expect(eq(with_table.results[0].s, kimix::string("table")));
            expect(eq(with_table.results[1].i, int64_t{1}));
            expect(with_table.results[2].kind == Value::Kind::boolean);
            expect(!with_table.results[2].b) << "my_table.b[2] is JSON false";
            expect(eq(with_table.results[3].s, kimix::string("nil"))) << "the refused global was never installed";
        }

        sb.clear_globals();
        auto cleared = sb.run("return type(my_int), type(my_str), type(my_flag)");
        expect(cleared.ok()) << cleared.message.c_str();
        for (const Value &value : cleared.results) {
            expect(eq(value.s, kimix::string("nil"))) << "clear_globals() must remove every global";
        }
    };

    "runspec_calls_a_named_function"_test = [] {
        Sandbox sb;

        /* 0 arguments, and the setup chunk's own results are dropped. */
        RunSpec zero;
        zero.source = "function zero() return 0 end\nreturn 'dropped'";
        zero.call_name = "zero";
        auto r0 = sb.run(zero);
        expect(r0.ok()) << r0.message.c_str();
        expect(eq(r0.results.size(), size_t{1}));
        if (r0.results.size() == 1) {
            expect(r0.results[0].kind == Value::Kind::integer);
            expect(eq(r0.results[0].i, int64_t{0})) << "the named call's results, not the prelude's";
        }

        /* 1 argument. */
        RunSpec double_it;
        double_it.source = "function twice(a) return a * 2 end";
        double_it.call_name = "twice";
        Value one[1] = {int_value(21)};
        double_it.args = one;
        double_it.argc = 1;
        auto r1 = sb.run(double_it);
        expect(r1.ok()) << r1.message.c_str();
        if (!r1.results.empty()) {
            expect(eq(r1.results[0].i, int64_t{42}));
        }

        /* Several arguments of mixed kinds. */
        RunSpec join;
        join.source = "function join(a, b, c, d) return a .. '|' .. b .. '|' .. c .. '|' .. tostring(d) end";
        join.call_name = "join";
        Value four[4] = {string_value("a"), string_value("b"), string_value("c"), int_value(3)};
        join.args = four;
        join.argc = 4;
        auto r4 = sb.run(join);
        expect(r4.ok()) << r4.message.c_str();
        if (!r4.results.empty()) {
            expect(eq(r4.results[0].s, kimix::string("a|b|c|3"))) << r4.results[0].s.c_str();
        }

        /* The named function is looked up in the RUN environment, not in the real
         * globals table: a function the chunk defines is what gets called. */
        RunSpec shadow;
        shadow.source = "function tick() return 'chunk' end";
        shadow.call_name = "tick";
        auto rs = sb.run(shadow);
        expect(rs.ok()) << rs.message.c_str();
        if (!rs.results.empty()) {
            expect(eq(rs.results[0].s, kimix::string("chunk")));
        }
    };

    "runspec_call_name_must_be_a_function"_test = [] {
        Sandbox sb;
        RunSpec number_call;
        number_call.source = "answer = 42";
        number_call.call_name = "answer";
        auto r = sb.run(number_call);
        expect(r.status == Status::runtime) << r.message.c_str();
        expect(contains(r.message, "'answer' is a number, not a function")) << r.message.c_str();
        expect(contains(r.message, "RUNTIME:")) << r.message.c_str();
        expect(r.results.empty());

        RunSpec nil_call;
        nil_call.source = "";
        nil_call.call_name = "nope";
        auto n = sb.run(nil_call);
        expect(n.status == Status::runtime) << n.message.c_str();
        expect(contains(n.message, "'nope' is a nil, not a function")) << n.message.c_str();

        /* The Sandbox stays usable after both. */
        auto after = sb.run("return 1");
        expect(after.ok()) << after.message.c_str();
    };

    "empty_call_name_equals_a_plain_run"_test = [] {
        Sandbox sb;
        const char *const source = "local t = {1, 2, 3} pcall(function() end) return #t, 'two', {a = 1}";
        auto plain = sb.run(source, "plain.lua");
        expect(plain.ok()) << plain.message.c_str();

        RunSpec spec;
        spec.source = source;
        spec.chunk_name = "plain.lua";
        auto via_spec = sb.run(spec);

        expect(via_spec.status == plain.status);
        expect(eq(via_spec.message, plain.message)) << via_spec.message.c_str();
        expect(same_results(plain, via_spec)) << "an empty call_name must not change the answer";
        expect(eq(plain.results.size(), size_t{3}));
        if (plain.results.size() == 3) {
            expect(eq(plain.results[0].i, int64_t{3}));
            expect(eq(plain.results[1].s, kimix::string("two")));
            expect(eq(plain.results[2].s, kimix::string("{\"a\":1}")));
        }
    };

    "host_calling_run_on_its_own_sandbox_is_internal"_test = [] {
        host_probe probe;
        Sandbox sb;
        probe.self = &sb;
        expect(sb.bind_host_function("inner", &host_inner_run, &probe));
        auto r = sb.run("return inner()");
        expect(r.ok()) << "the outer run must end sanely: " << r.message.c_str();
        expect(probe.inner_status == Status::internal) << probe.inner_message.c_str();
        expect(contains(probe.inner_message, "concurrent run on one Sandbox")) << probe.inner_message.c_str();
        if (!r.results.empty()) {
            expect(eq(r.results[0].s, kimix::string("inner=internal")));
        }
        expect(!sb.busy()) << "the outer run must have released the busy flag";
        auto after = sb.run("return 2");
        expect(after.ok()) << after.message.c_str();
    };

    "stale_host_error_does_not_leak"_test = [] {
        host_probe probe;
        Sandbox sb;
        expect(sb.bind_host_function("choppy", &host_choppy, &probe));
        auto r = sb.run("local ok, err = pcall(choppy)\n"
                        "local value = choppy()\n"
                        "return ok, err, value");
        expect(r.ok()) << r.message.c_str() << " (a stale error must not fail the run)";
        expect(eq(r.results.size(), size_t{3}));
        if (r.results.size() == 3) {
            expect(r.results[0].kind == Value::Kind::boolean);
            expect(!r.results[0].b) << "the first call failed";
            expect(contains(r.results[1].s, "host function 'choppy' failed: boom")) << r.results[1].s.c_str();
            expect(eq(r.results[2].s, kimix::string("fine")))
                << "'boom' from call 1 must not be visible to call 2";
        }
        expect(eq(probe.calls, 2));
        expect(eq(probe.last_index, uint64_t{2}));
    };

    "no_binding_means_no_behaviour_change"_test = [] {
        const char *const source = "return 1 + 1, 'two', {a = 1}";
        /* A hook strike every instruction, so Result::instructions is an observable
         * of these tiny scripts (the default granularity of 10'000 needs a long
         * loop to show anything). */
        Limits limits;
        limits.hook_instruction_granularity = 1;
        Sandbox plain(limits);
        Sandbox with_host(limits);
        host_probe probe;
        expect(with_host.bind_host_function("unused", &host_tick, &probe));

        auto baseline = plain.run(source);
        auto observed = with_host.run(source);
        expect(baseline.ok()) << baseline.message.c_str();
        expect(observed.ok()) << observed.message.c_str();
        expect(observed.status == baseline.status);
        expect(eq(observed.message, baseline.message));
        expect(same_results(baseline, observed)) << "an installed-but-unused binding changed the answer";
        expect(eq(observed.results.size(), size_t{3}));
        if (observed.results.size() == 3) {
            expect(eq(observed.results[0].i, int64_t{2}));
            expect(eq(observed.results[1].s, kimix::string("two")));
            expect(eq(observed.results[2].s, kimix::string("{\"a\":1}")));
        }
        /* The run cost is still measured, i.e. nothing was short-circuited. */
        expect(observed.instructions > 0);
        expect(observed.memory_peak > 0);
        expect(eq(probe.calls, 0)) << "an unused binding must never be invoked";
        /* The default host budgets are the documented ones. */
        expect(eq(with_host.limits().max_host_calls, uint32_t{10000}));
        expect(eq(with_host.limits().max_host_args, uint32_t{16}));
        expect(eq(with_host.limits().max_host_ret_values, uint32_t{16}));
    };

    "bind_host_function_validation"_test = [] {
        host_probe probe;
        Sandbox sb;
        expect(!sb.bind_host_function("", &host_tick, &probe)) << "an empty name is not a name";
        expect(!sb.bind_host_function("2bad", &host_tick, &probe)) << "a name may not start with a digit";
        expect(!sb.bind_host_function("a b", &host_tick, &probe)) << "a name may not contain a space";
        expect(!sb.bind_host_function("add", nullptr, &probe)) << "a null fn installs nothing";

        Sandbox moved_to(static_cast<Sandbox &&>(sb));
        expect(!sb.bind_host_function("add", &host_tick, &probe)) << "a moved-from Sandbox accepts nothing";
        expect(!sb.set_global("x", int_value(1))) << "a moved-from Sandbox accepts nothing";
        auto dead = sb.run("return 1");
        expect(dead.status == Status::internal) << dead.message.c_str();
        expect(contains(dead.message, "moved from")) << dead.message.c_str();

        /* The move TARGET is fully functional. */
        expect(moved_to.bind_host_function("tick", &host_tick, &probe));
        auto ok = moved_to.run("return tick()");
        expect(ok.ok()) << ok.message.c_str();
        if (!ok.results.empty()) {
            expect(eq(ok.results[0].i, int64_t{1}));
        }
    };

    "reserved_names_cannot_be_bound_or_set"_test = [] {
        host_probe probe;
        Sandbox sb;
        /* Every kind of name the environment installs: an allow-listed primitive, a
         * replacement, a tripwire, a library table and _G. */
        static const char *const kReserved[] = {
            "print",  "pcall",   "xpcall",  "load",    "tostring", "warn",      "collectgarbage",
            "string", "table",   "math",    "utf8",    "coroutine", "io",       "os",
            "package", "debug",  "dofile",  "loadfile", "require",  "loadlib",  "_G",
            "assert",  "error",  "type",    "next",    "pairs",    "ipairs",    "select",
            "tonumber", "setmetatable", "getmetatable", "rawget", "rawset",   "rawequal",
            "rawlen", "_VERSION", nullptr,
        };
        Value one = int_value(1);
        Value empty;
        for (int i = 0; kReserved[i] != nullptr; ++i) {
            const char *name = kReserved[i];
            expect(kimix::script::sb_env_name_reserved(name)) << name << " must be a reserved name";
            expect(!sb.bind_host_function(name, &host_tick, &probe)) << "bound a reserved name: " << name;
            expect(!sb.set_global(name, one)) << "set a reserved name: " << name;
            expect(!sb.set_global(name, empty)) << "cleared a reserved name: " << name;
        }
        expect(eq(probe.calls, 0));

        /* Nothing was installed: the run is ok (NOT behaviour), and print still
         * captures, so no capability was deleted by the rejected attempts. */
        auto intact = sb.run("print('kept') return type(print), type(pcall), type(_G), type(io)");
        expect(intact.ok()) << intact.message.c_str() << " (a rejected name must not poison the run)";
        expect(eq(intact.output, kimix::string("kept\n"))) << intact.output.c_str();
        expect(eq(intact.results.size(), size_t{4}));
        if (intact.results.size() == 4) {
            expect(eq(intact.results[0].s, kimix::string("function")));
            expect(eq(intact.results[1].s, kimix::string("function")));
            expect(eq(intact.results[2].s, kimix::string("table")));
            expect(eq(intact.results[3].s, kimix::string("table")));
        }

        /* Names that merely LOOK similar are ordinary names: they still work. */
        expect(sb.bind_host_function("printf", &host_tick, &probe));
        expect(sb.set_global("my_global", int_value(9)));
        auto mine = sb.run("return printf(), my_global, type(printf)");
        expect(mine.ok()) << mine.message.c_str();
        expect(eq(mine.results.size(), size_t{3}));
        if (mine.results.size() == 3) {
            expect(eq(mine.results[0].i, int64_t{1}));
            expect(eq(mine.results[1].i, int64_t{9}));
            expect(eq(mine.results[2].s, kimix::string("function")));
        }
        expect(eq(probe.calls, 1));
    };

    "every_environment_name_is_reserved"_test = [] {
        /* The drift guard for kEnvReservedNames (script_env.cpp): every name the
         * built environment exposes must be in that table, otherwise a host could
         * still overwrite it.  Adding a global to the environment without adding it
         * to the table turns this red instead of leaving a silent hole. */
        Sandbox sb;
        kimix::string missing;
        expect(every_name_reserved(sb, &missing)) << "reachable but not reserved: " << missing.c_str();
        expect(sb.allow_coroutines(true));
        expect(every_name_reserved(sb, &missing))
            << "reachable but not reserved (coroutines on): " << missing.c_str();
    };
}
