/*
 * script_sandbox.h -- the public C++ surface of the Lua script sandbox
 * (kimix-script).
 *
 * WHAT THIS IS
 * ------------
 * `kimix::script::Sandbox` runs one Lua chunk per call in a throw-away lua_State
 * whose environment contains ONLY a vetted subset of the standard library:
 * base (minus the file/process/bytecode entry points), string, table, math and
 * utf8.  A script that reaches for anything else (file I/O, subprocesses, module
 * loading, bytecode, debug introspection, pointers, unbounded memory, unbounded
 * CPU) gets a LOUD denial that ABORTS the run and travels back to the caller as
 * a typed Result -- it cannot be swallowed by the script's own pcall/xpcall/
 * coroutine.resume.  See src/script/README.md for the five defence layers and
 * the message format.
 *
 * The seven properties every caller can rely on:
 *   1. run() never throws (the project is built without exceptions) and never
 *      returns a bare failure: every non-ok Result carries a message.
 *   2. Nothing reaches stdout/stderr: print()/warn() are captured into
 *      Result::output, Lua warnings (__gc errors) are captured too.
 *   3. A run cannot exceed its memory budget, its deadline, or its
 *      output/source/result caps (Limits).
 *   4. State is per run (fresh_state_per_run, the default): no global written by
 *      one run is visible to the next, and every byte is released when the run
 *      ends (asserted in debug builds).
 *   5. No address ever crosses the boundary: results are nil/boolean/number/
 *      string/canonical-JSON, and tostring()/string.format('%s') render tables
 *      and functions as "table: #1" / "function: #2".
 *   6. Determinism: the state is created with a fixed seed and math.randomseed
 *      is pinned, so `pairs` order and math.random are reproducible.
 *   7. The host can extend a run without weakening it.  A host function
 *      (bind_host_function) is a POD-only C callback reached through a closure,
 *      so the script never obtains a pointer into the host and the host never
 *      obtains the lua_State; it is budgeted per run (Limits::max_host_calls) and
 *      a host failure is an ordinary RUNTIME error the script's own pcall can
 *      catch.  Globals (set_global) are COPIED into the Sandbox, and
 *      run(RunSpec) can call a named Lua function with arguments.
 *
 * HOST FUNCTIONS
 * --------------
 * A host function never sees the interpreter.  It receives one POD-safe HostCall
 * describing the invocation -- the bound name, the marshalled arguments, an
 * output array for the results, an error slot and the 1-based ordinal of the
 * call inside this run -- and answers true (produced results) or false (raise
 * HostCall::error as a catchable RUNTIME error).  What may cross:
 *
 *   script -> host args   nil, boolean, integer, number, string, table (as
 *                         canonical JSON); anything else is a sticky LIMIT
 *   host -> script rets   nil, boolean, integer, number, string, table (the
 *                         canonical JSON text is materialised by the strict
 *                         reader); a malformed text is a sticky LIMIT
 *   host -> script global nil, boolean, integer, number, string, table (same
 *                         reader); a text that is not strict JSON makes
 *                         set_global() return false (no raise: host time)
 *
 * A table therefore crosses BOTH ways, and the text is the same canonical form
 * in both directions (minified, object keys byte-sorted).  `null` becomes Lua
 * nil, so an object key whose value is null disappears; duplicate object keys are
 * refused; the same caps apply as outbound (Limits::max_result_bytes,
 * max_result_depth, max_string_bytes).
 *
 * RESERVED NAMES
 * --------------
 * The host may not bind or set a name the environment itself installs: `print`,
 * `pcall`, `load`, `tostring`, `warn`, `collectgarbage`, `string`, `table`,
 * `math`, `utf8`, `coroutine`, `io`, `os`, `package`, `debug`, `dofile`,
 * `loadfile`, `require`, `loadlib`, `_G`, and the safe base functions it copies
 * (`assert`, `error`, `type`, `next`, `pairs`, `ipairs`, `select`, `tonumber`,
 * `setmetatable`, `getmetatable`, `rawget`, `rawset`, `rawequal`, `rawlen`,
 * `_VERSION`).  Overwriting one either deletes a capability or poisons every
 * later run (`sb_env_verify` => Status::behaviour), so bind_host_function and
 * set_global simply return false for it.  The authoritative list is one table in
 * script_env.cpp (sb_env_name_reserved).
 *
 *   kimix::script::Sandbox sb;
 *   Limits limits;                       // defaults: 10'000 host calls per run
 *   limits.max_host_calls = 100;
 *   sb.bind_host_function("now", &host_now, &clock);   // script calls now()
 *   sb.set_global("version", version_value);           // copied, not referenced
 *   RunSpec spec;
 *   spec.source = "function add(a, b) return a + b end";
 *   spec.call_name = "add";                            // call it after the chunk
 *   Value args[2]; args[0].kind = Value::Kind::integer; args[0].i = 2; ...
 *   spec.args = args; spec.argc = 2;
 *   auto r = sb.run(spec);                             // r.results[0].i == 5
 *
 * THREADING
 * ---------
 * A Sandbox is NOT thread-safe: a lua_State is single-threaded and this class
 * keeps one per run.  A concurrent run() on the same Sandbox is answered with
 * Status::internal ("concurrent run on one Sandbox").  `cancel()` is the ONLY
 * call that may be made from another thread while a run is in flight; `busy()`
 * is the read-only probe.  bind_host_function()/set_global()/clear_globals()
 * must not run concurrently with a run() (the binding list a live run points at
 * is replaced).
 *
 * USAGE
 * -----
 *   kimix::script::Sandbox sb;                     // default Limits
 *   auto r = sb.run("return 1 + 1", "script.lua");
 *   if (r.ok()) { use(r.results[0].i); }           // 2
 *   else        { report(r.status, r.message); }   // "script.lua:1: DENIED: ..."
 *
 * See also: docs/ffi.md (the same surface as a plain C ABI, kimix_lua_*),
 * src/script/README.md (design + residual risks).
 */
#pragma once
#ifndef KIMIX_SCRIPT_SANDBOX_H
#define KIMIX_SCRIPT_SANDBOX_H

#include <cstdint>

#include <core/stl/memory.h> /* kimix::size_literals (16_M, ...), kimix::unique_ptr */
#include <core/stl/string.h>
#include <core/stl/vector.h>

#include <script/script_api.h>
#include <script/script_status.h>
#include <script/script_value.h>

/* Forward-declared on purpose: a consumer of this header never sees a vendored
 * Lua header, so nothing outside src/script can call the Lua C API through it. */
struct lua_State;

namespace kimix::script {

/* The resource contract of one run.  Everything a script can consume is
 * bounded here; the defaults are the ones the plan's tests assert. */
struct Limits {
    /* Whole-state budget, all threads, counted by the budgeted lua_Alloc
     * (script_alloc.cpp).  `used` includes the bootstrap state (~14 KB). */
    size_t max_memory_bytes = 16_M;
    /* The chunk source itself (`run`'s argument, and what `load` accumulates). */
    size_t max_source_bytes = 1_M;
    /* A SINGLE allocation cap: a script asking for one huge string is refused
     * early, with a clearer message than a slowly exhausted budget. */
    size_t max_string_bytes = 8_M;
    /* Captured print()/warn() text. */
    size_t max_output_bytes = 64_k;
    /* Marshalled return values (canonical JSON for tables). */
    size_t max_result_bytes = 256_k;
    uint32_t max_result_values = 64;
    /* Nesting cap for a returned table. */
    uint32_t max_result_depth = 32;
    /* Host-function surface (see "HOST FUNCTIONS"): how many times one run may
     * call into host code (script -> host amplification cap), how many arguments
     * a host function accepts, and how many values it may return. */
    uint32_t max_host_calls = 10'000;
    uint32_t max_host_args = 16;
    uint32_t max_host_ret_values = 16;
    /* Wall-clock deadline (0 = none). */
    double max_seconds = 2.0;
    /* Instruction budget (0 = unbounded; the wall clock still applies). */
    uint64_t max_instructions = 0;
    /* Count-hook granularity: the deadline is checked every N VM instructions. */
    uint32_t hook_instruction_granularity = 10'000;
    /* lua_newstate(alloc, ud, seed) seed and math.randomseed: fixed, so hash
     * order and math.random are reproducible run to run. */
    unsigned seed = 0x00C0FFEEu;
    uint32_t random_seed = 0x5EED2025u;
    /* true (default) = a new lua_State per run, i.e. perfect run-to-run
     * isolation.  false reuses one state: faster (~13 %), but globals and
     * library tables survive between runs -- documented as NOT isolated. */
    bool fresh_state_per_run = true;
};

/* One host-function invocation, as the sandbox sees it.  POD-safe: no
 * lua_State, no pointer that outlives the call, no way to longjmp from here. */
struct HostCall {
    kimix::string_view name; /* the bound name (stable, owned by the Sandbox) */
    const Value *args;       /* marshalled arguments; null when argc == 0 */
    size_t argc;
    Value *rets;             /* caller-provided array of ret_cap slots */
    size_t ret_cap;
    size_t produced;         /* out: how many of rets[0..produced) are valid */
    /* Library-provided POD scratch for a host that wants to report a failure in
     * its OWN words: write a NUL-terminated message into [err, err + err_cap) and
     * give it back through `error` (or just return false, in which case the
     * sandbox raises its own text).  Reset before every call, at least 512 bytes
     * on every platform, valid only for the duration of the call -- copy nothing
     * out of it.  State-owned, so it needs no destructor and a raise inside the
     * trampoline cannot leak it. */
    char *err = nullptr;
    size_t err_cap = 0;
    kimix::string error;     /* out: on failure, raised as a normal Lua error */
    uint64_t index;          /* in: 1-based ordinal inside this run */
};

/* false => the sandbox raises `error` (catchable by the script's pcall);
 * true  => `produced` results.  Never longjmps, never sees the interpreter. */
using HostFn = bool (*)(void *user, HostCall &call);

class KIMIX_SCRIPT_API Sandbox {
public:
    explicit Sandbox(const Limits &limits = Limits{});
    ~Sandbox();

    /* Move-only (the state header holds pointers into this object). */
    Sandbox(Sandbox &&other) noexcept;
    Sandbox &operator=(Sandbox &&other) noexcept;
    Sandbox(const Sandbox &) = delete;
    Sandbox &operator=(const Sandbox &) = delete;

    /* Compile and run `source` with the sandbox environment.  The source is
     * copied before use.  `chunk_name` shows up in messages; keep it short
     * (LUA_IDSIZE is 60) -- the full name is carried in Result::violation.chunk.
     * Never throws; a concurrent call returns Status::internal. */
    Result run(kimix::string_view source, kimix::string_view chunk_name = "script.lua");

    /* The signature of a host-provided function (`lua_CFunction`).  The function
     * receives the sandbox lua_State and may only use the C API; it must be
     * POD-only and raise at the very end of its body (README §"Adding a native"). */
    using NativeFn = int (*)(lua_State *L);

    /* Install `name` into the run environment as a C function.  `upvalues` is
     * the number of nil upvalues to create for it (0 = a plain function).
     * Returns false when `name` is empty/not a valid identifier or `fn` is null.
     *
     * This is the ADVANCED escape hatch: it predates the host surface and sees
     * the interpreter, so it deliberately does NOT apply the reserved-name rule
     * above -- a host that installs an `io`-shaped native is asking for exactly
     * that, and the integrity post-check will report it as Status::behaviour.
     * Prefer bind_host_function. */
    bool add_native(kimix::string_view name, NativeFn fn, int upvalues = 0);

    /* ---- the host surface (see "HOST FUNCTIONS") ------------------------- */

    /* Install `name` into every subsequent run's _ENV as a host function.
     * `name` must match [A-Za-z_][A-Za-z0-9_]*; re-binding a name replaces it.
     * false on an invalid name, a name the environment itself installs (a
     * reserved name, see above -- "print", "io", "_G", ...), a null fn, or a
     * moved-from Sandbox.  No raise: this is host time, and the C ABI maps the
     * false to KIMIX_ERR_INVALID_ARG. */
    [[nodiscard]] bool bind_host_function(kimix::string_view name, HostFn fn, void *user = nullptr);

    /* Install (or overwrite) one global in every subsequent run's _ENV.  The value
     * is COPIED into the Sandbox.  A Kind::nil value clears the global (it is
     * still installed, as nil, so it also hides a same-named primitive).
     * A Kind::table is carried as its canonical JSON text and materialised into a
     * real Lua table by the run's install step; its text is validated here, once,
     * with the strict reader's caps, so a text that could not be materialised is
     * refused up front.
     * false on an invalid name, a name the environment itself installs (a
     * reserved name: `set_global("print", nil)` would delete a capability), a
     * moved-from Sandbox, or a table whose text is not strict JSON (no raise --
     * this is host time; the C ABI maps it to KIMIX_ERR_INVALID_ARG). */
    [[nodiscard]] bool set_global(kimix::string_view name, const Value &value);
    void clear_globals();

    /* One run, everything the host may feed in.  `source` is the setup chunk;
     * `call_name` names a Lua global to call after it (empty = just run the
     * chunk); `args` are copied into the Sandbox before use. */
    struct RunSpec {
        kimix::string_view source;
        kimix::string_view chunk_name = "script.lua";
        kimix::string_view call_name; /* the Lua global to call; may be empty */
        const Value *args = nullptr;
        size_t argc = 0;
    };
    Result run(const RunSpec &spec);

    /* coroutine.create/resume/status/wrap/yield/close: default OFF.  When on,
     * resume/close are replaced with re-raising wrappers, so a denial inside a
     * coroutine still aborts the run. */
    bool allow_coroutines(bool enable) noexcept;

    /* collectgarbage("count") only: default OFF (every other option is denied,
     * so a script cannot stop or pause the collector). */
    bool allow_collectgarbage_count(bool enable) noexcept;

    /* Ask the in-flight run to abort (thread-safe; the count hook raises
     * Status::cancelled).  Cleared automatically at the start of the next run. */
    void cancel() noexcept;

    /* true while a run() is in flight on this Sandbox. */
    bool busy() const noexcept;

    const Limits &limits() const noexcept;

private:
    struct Impl;
    /* Out-of-line destructor + move members, so the incomplete Impl above is
     * enough for a consumer of this header (and Impl still carries the sandbox's
     * mimalloc-backed operator new, core/memory.h). */
    kimix::unique_ptr<Impl> _impl;
};

} // namespace kimix::script

#endif /* KIMIX_SCRIPT_SANDBOX_H */
