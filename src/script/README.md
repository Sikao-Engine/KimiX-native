# `src/script/` — the Lua script sandbox (`kimix-script`)

`kimix::script::Sandbox` runs **one Lua chunk per call** in a throw-away,
budgeted `lua_State` whose environment holds only a vetted subset of the standard
library. A script can never read or write a file, spawn a process, load a module,
reach debug introspection, obtain an address, exit the process, read the
environment, or allocate/run without a bound — and when it tries, the run
**aborts loudly** with a message that names the API, the category and the script
line, and that message travels back to the caller as a typed `Result`.

```cpp
#include <script/script_sandbox.h>

kimix::script::Sandbox sb;                       // default Limits
auto r = sb.run("return 1 + 1", "script.lua");
if (r.ok()) { use(r.results[0].i); }             // 2
else        { report(r.status, r.message); }     // "script.lua:7: DENIED: io.open — …"
```

The same surface exists as a plain C ABI for foreign callers:
`<api/ffi_lua.h>` (`kimix_lua_*`, documented in `docs/ffi.md`).

---

## 1. The five layers

Defence in depth, not redundancy — each layer closes a hole the one above it
leaves open.

| Layer | Mechanism | What it stops |
|---|---|---|
| **L0** creation | `lua_newstate(budget_alloc, self, fixed_seed)`, `lua_atpanic`, `lua_setwarnf`, one extraspace header. Never `luaL_newstate`, never `luaL_openlibs`. | inherited libraries, a stderr panic, a stderr warning handler, address/time-based non-determinism |
| **L1** whitelist | `luaL_openselectedlibs(L, base\|string\|table\|math\|utf8, 0)` | `io`, `os`, `package`, `debug`, `require` are never opened — structurally absent |
| **L2** tripwires | tables/functions that exist, are callable, and raise `DENIED: <api> — <why>` | *silent* containment: `io` is a `table`, not `nil`, so the attempt is named instead of "attempt to index a nil value" |
| **L3** sticky + re-raise | a first-violation-wins flag in the extraspace header plus re-raising `pcall`/`xpcall`/`coroutine.resume`/`coroutine.close` | a script swallowing a denial with its own `pcall` (stock Lua catches a plain error 1000/1000 times) |
| **L4** budgets | one budgeted `lua_Alloc`, one `LUA_MASKCOUNT` hook, a cancel flag | unbounded memory, runaway CPU, a host that wants out |
| **L5** marshalling | bounded, cycle-safe conversion of return values; output caps | a hostile *return value* turning into a native OOM or hang |

## 2. Capability matrix

| Capability | Behaviour |
|---|---|
| file read/write/stat/delete/rename | impossible — `DENIED: io.open — file I/O is not available in the sandbox` |
| subprocess (`os.execute`, `io.popen`) | impossible — `DENIED`, `os.exit` can never kill the host |
| dynamic loading (`require`, `package.loadlib`, `dofile`, `loadfile`) | impossible — `DENIED … module loading is not available in the sandbox` |
| debug introspection (`debug.*`) | impossible — `DENIED … debug introspection is not available in the sandbox` |
| bytecode | `string.dump` is denied; `load` forces text mode and refuses a binary chunk with `LOADERR: attempt to load a binary chunk (mode is 't')` |
| pointers | no userdata is ever reachable; `tostring`/`print`/`string.format('%s')` render tables and functions as `table: #1` / `function: #2`; `%p` is denied |
| env vars / locale / clock | not exposed (`os.getenv`, `os.setlocale`, `os.time`, … are tripwires) |
| `collectgarbage` | only `"count"`, only when enabled at construction |
| stdout/stderr | captured into `Result::output`, never written to the process streams |
| global mutation across runs | impossible with `fresh_state_per_run` (the default): a fresh state and a fresh `_ENV` per run |
| heap/GC state outliving a run | impossible: the state is closed and every byte released (`alloc_used == 0`, reported on stderr if a host bug ever breaks it) |
| memory | one budget (`Limits::max_memory_bytes`), one single-allocation cap, sticky OOM (a retry loop cannot outlive the first denial) |
| CPU | wall-clock deadline + optional instruction budget through the count hook, and `Sandbox::cancel()` from another thread |

What is **not** in scope, stated so nobody oversells the result:

- Another module inside the same process can still `dlopen` Lua, call
  `luaL_openlibs` or link its own copy. This protects the host from *scripts*, not
  the process from its own modules. `scripts/check_lua_sandbox.py` is the guard
  rail against that happening by accident in this tree.
- No kernel-level isolation: a memory-safety bug in the Lua C core would be a
  process compromise.
- Uninterruptible C work (a pathological `string.find`/`gsub` pattern burns CPU
  with zero hook strikes) is *bounded* by the memory budget, not eliminated.
- Process separation (fork a helper, JSON over pipes) is the real answer for
  untrusted third-party scripts and is deliberately deferred.

The interpreter memory arena (not wired in yet). The vendored interpreter can be
built with a per-state size-class arena (`KIMIX_LUA_ARENA`, see
`docs/lua_memory_arena.md`): `luaL_newstate_arena()` serves the state from
64 KiB segments with exact-size classes for table array/node blocks, and
`lua_ArenaTrim()`/`lua_gc(LUA_GCTRIM)` give the capacity back. The sandbox does
not use it: `script_alloc.cpp`'s budgeted `lua_Alloc` is the outermost layer and
must stay that way (it is what enforces `Limits::max_memory_bytes` and reports
logical bytes in the `MEMORY:` message), and `script_state.cpp` builds its state
with `lua_newstate` + that allocator. Nesting the arena *underneath* the budget
— budget counts logical bytes, the arena serves them — plus a
`lua_ArenaTrim(1)` at the end of every run and the arena counters inside
`Result` is the plan's `[needs src/script]` follow-up, not a capability change:
`luaL_newstate` and the whole capability matrix above stay exactly as they are.
An arena build adds `table.new`/`table.shrink` to the interpreter's `table`
library; `script_env.cpp` drops both for every run, so the reachable surface
(the golden list in `test_script_surface`) is the same with the arena built in or
out.

## 3. Message format

One format, composed in exactly one place (`script_error.cpp`), so what a
script's own error handler sees and what the host receives cannot diverge:

```
<chunk>:<line>: <PREFIX>: <api> — <detail>
```

| Prefix | `Status` | Example |
|---|---|---|
| `DENIED: ` | `denied` | `script.lua:7: DENIED: io.open — file I/O is not available in the sandbox` |
| `LOADERR: ` | `denied` | `script.lua:5: LOADERR: attempt to load a binary chunk (mode is 't')` |
| `MEMORY: ` | `memory` | `script.lua:9: MEMORY: allocation of 67108864 bytes exceeds the sandbox budget (used 14361 of 16777216)` |
| `TIMEOUT: ` | `timeout` | `script.lua:2: TIMEOUT: script exceeded its 2.000 s deadline (4523000 instructions)` |
| `CANCELLED: ` | `cancelled` | `script.lua:2: CANCELLED: the host cancelled this run (512000 instructions executed)` |
| `LIMIT: ` | `limit` | `script.lua:N: LIMIT: output exceeds the 65536-byte cap` |
| `RUNTIME: ` | `runtime` | `script.lua:1: RUNTIME: attempt to index a nil value (global 'nosuch')` |
| `BEHAVIOUR: ` | `behaviour` | `script.lua:1: BEHAVIOUR: a sandbox primitive was replaced by the script` |
| `SYNTAX: ` | `syntax` | Lua's own message, kept verbatim |
| `INTERNAL: ` | `internal` | a host bug (a concurrent `run()`), or a Lua panic |

`<line>` comes from `luaL_where(L, 1)` at raise time. `LUA_IDSIZE` is 60, so a
long chunk name is truncated **inside the message**; the full name is always
carried in `Result::violation.chunk` (and `kimix_lua_last_violation`).

## 4. The per-run-state rule

A run creates its own `lua_State`, builds its own `_ENV`, runs, collects, and
closes. That costs ~13 % more than reusing a state (≈110 µs vs ≈100 µs in a debug
build, see `tests/unit/script/test_script_bench.cpp`) and buys three things
nothing else does:

1. no global written by one run is visible to the next (a fresh `_ENV` alone is
   not enough — `string.rep = …` would still hijack the shared library table),
2. every byte is released when the run ends (`alloc_used == 0`, asserted),
3. `_G` is the run environment, so there is no path to the real globals.

`Limits::fresh_state_per_run = false` keeps one state alive across runs: faster,
and documented as **not isolated**. `_ENV` is still rebuilt per run, but library
tables survive.

## 5. The longjmp rule (read before adding a native)

The project is built without C++ exceptions, and `lua_error()` unwinds by
`longjmp`. **No C++ object with a destructor may be alive when a `lua_CFunction`
can raise.** Concretely:

- every `lua_CFunction` in this module is POD-only and raises at the very end
  (`return sb_raise(...)` / `return lua_error(L)`);
- scratch buffers live in `SandboxState` (`body`, `load_buffer`, `json_buffer`,
  `json_path`, `json_key_stack`, and the host scratch `host_args` / `host_rets` /
  `host_error` / `host_err` of §7), never on the stack of a raising frame;
- a raise goes through `sb_raise()` → `sb_do_raise()`, which composes the message
  **before** it longjmps;
- every call into the interpreter happens under the ONE `lua_pcallk` in
  `script_run.cpp` (`sb_bootstrap`), so an error there always lands on the same
  frames and no destructor is skipped.

### Audit of the calling sites (re-run it after a change)

```bash
grep -n "lua_call\|lua_pcall\|lua_resume\|lua_closethread" src/script/*.cpp
grep -n "lua_State \*" src/script/*.h          # only the INTERNAL headers
```

What it must show, and why each site is inside the protection:

| Site | Why it is protected |
|---|---|
| `script_run.cpp` — `lua_pcallk` (the boundary) and the bootstrap's `lua_call` | the chunk runs *inside* the boundary: its errors land on the boundary's frames |
| `script_env.cpp` — `sbx_load` (reader function), `sbx_format` (stock `string.format`), `sbx_collectgarbage` (stock `collectgarbage`), `sbx_tostring` (`__tostring`) | each is a `lua_CFunction`; it is only reachable from script code, i.e. only from inside the boundary, and its `lua_call` raises into that same `lua_pcallk` |
| `script_env.cpp` — `lua_pcallk` in `sbx_pcall`/`sbx_xpcall` (with the `sbx_finishpcall` continuation), `lua_resume`/`lua_closethread` in `sbx_coresume`/`sbx_coclose` | these ARE the re-raising wrappers: they make their own protected call and then re-raise the sticky message |
| `script_env.cpp` — `lua_call` for `math.randomseed` during env construction | inside the bootstrap, i.e. inside the boundary |
| `script_state.cpp` — `lua_pcall` around the final `collectgarbage` at teardown | the one call that runs after the boundary has returned; it protects the finalizers a raising `__gc` would otherwise make fatal |
| `script_host.cpp` — `sb_host_trampoline` (a `lua_CFunction`) and `sb_host_install_globals` | the trampoline is only reachable from script code, i.e. from inside the boundary, and each of its raises happens after the foreign frame returned and after its `HostCall` local died (§7); the install step runs inside the bootstrap |

`lua_State *` appears only in the INTERNAL headers (`script_state.h`,
`script_env.h`, `script_error.h`, `script_value.h`). The public
`script_sandbox.h` forward-declares `struct lua_State` and exposes it only as a
`NativeFn` parameter, and the C ABI (`api/ffi_lua.h`) never mentions it at all, so
nothing outside this directory can reach the interpreter through these headers.

## 6. Adding a native (a host-provided function)

```cpp
#include "lua.h"   // a native speaks the Lua C API; the sandbox itself never leaks it

static int host_hash(lua_State *L) {              // POD-only!
    size_t len = 0;
    const char *text = lua_tolstring(L, 1, &len);
    lua_pushinteger(L, static_cast<lua_Integer>(kimix::hash64(text, len)));
    return 1;                                     // raises only at the very end
}

kimix::script::Sandbox sb;
sb.add_native("host_hash", &host_hash);            // installed into every run's _ENV
auto r = sb.run("return host_hash('abc')");
```

Rules for a native: it is installed per run, it sees the sandbox `lua_State`, it
must not keep a pointer to it, it must be POD-only across any raising call, and it
should deny nothing — the sandbox's own tripwires already cover the dangerous
capabilities. A native is how a host injects a *deterministic* capability (a
clock, randomness, a lookup) without opening a forbidden one.

`add_native` is the advanced escape hatch (it sees the interpreter). The ordinary
way to extend a run is the host surface of script_host.* —
`bind_host_function(name, HostFn, user)`, `set_global(name, Value)`,
`clear_globals()` and `run(RunSpec)`; §7 is its contract. Both install entry
points refuse a name the environment itself installs (`print`, `pcall`, `io`,
`_G`, …) by returning false: overwriting one would either delete a capability or
make the integrity post-check report `Status::behaviour` for the rest of the
Sandbox's life. The authoritative list is the single `kEnvReservedNames` table in
script_env.cpp (`sb_env_name_reserved`), and test_script_host fails if it drifts.

A **foreign** caller never sees `NativeFn`: it hands out a real `lua_State *`, so it
stays internal to src/script and no C ABI entry point accepts one. The C ABI's
equivalent is `kimix_lua_host_bind` (`<api/ffi_lua.h>`, docs/ffi.md §"Lua script
sandbox") — a plain C function that receives marshalled `kimix_lua_value` arrays
and never the interpreter.

### Tables cross in both directions

A table is the one value that needs a text form, and both directions live in
script_value.cpp around ONE mimalloc-backed `yyjson_alc` (`kSbYyAlc`:
`mi_malloc`/`mi_realloc`/`mi_free`) — the module declares no yyjson dependency of its
own, kimix-core already carries the header and the symbols.

- Outbound, a script table becomes CANONICAL JSON: minified, object keys sorted
  by byte order, arrays kept as arrays, integers exact.
- Inbound, a `Value::Kind::table` carrying such a text is materialised back into
  a real Lua table (`sb_json_to_lua`): an object becomes a table with string keys,
  an array becomes 1..n, an integer stays an integer (`lua_pushinteger`) unless
  it does not fit, other numbers become floats, strings are pushed byte-exact.
  The strict reader enforces the writer's own caps during the walk: the text
  itself and any single string against `Limits::max_result_bytes` /
  `Limits::max_string_bytes`, the nesting depth against `Limits::max_result_depth`
  (further bounded by a fixed 64-level native-recursion limit, so a depth bomb is
  refused instead of overflowing the C stack), and the node count against the
  writer's budget (max_result_bytes / 8 + 16). The interpreter memory the pushed
  table will need is charged against the run's budget before a single Lua value
  is built, so an over-budget table is a LIMIT and never a stranded document.
- The reader is STRICT: no comments, no NaN/Infinity, no trailing comma, no
  trailing garbage, and DUPLICATE object keys are refused — canonical JSON never
  has one, so a duplicate is a malformed input, and it is detected exactly (the
  level's keys are collected and sorted, O(k log k) per object) rather than by
  trusting the parser's choice of winner.

What `null` costs, stated once: a Lua table cannot hold nil, so a JSON null
becomes nil — inside an object the key disappears entirely, inside an array it
leaves a hole (so `#` stops in front of the first one, while the indices around
it are preserved). A host that must carry "explicitly absent" as a distinct
state should model it as a value of its own.

The two inbound paths differ in who is blamed. At SCRIPT time (a host function's
return value, an installed global, a `RunSpec` argument) a text that cannot be
materialised is a sticky `LIMIT` naming the reason — it would silently change what
the script sees, so it aborts the run and a script `pcall` cannot swallow it. At
HOST time (`set_global`) the text is validated with a standalone strict parse
(`sb_json_check`: same caps, same duplicate rule, no `lua_State` and no interpreter
memory) and a broken text is a plain `false` — the C ABI's
`KIMIX_ERR_INVALID_ARG` — so a host mistake cannot poison a later run.

## 7. Host functions

The host surface is how a caller extends a run without weakening it:
`bind_host_function(name, HostFn, user)`, `set_global(name, Value)`,
`clear_globals()` and `run(RunSpec)`. What the script sees is a closure, so it
never obtains a pointer into the host; what the host function sees is one POD
struct, so it never obtains the interpreter — no `lua_State`, no stack index, no
`lua_*` call. That is the reason this surface exists next to `add_native` (§6)
rather than on top of it.

### The `HostFn` contract

```cpp
bool fn(void *user, HostCall &call); // true => call.produced results, false => raise call.error
```

| `HostCall` field | Direction | Meaning |
|---|---|---|
| `name` | in | the bound name, a view over the Sandbox's own copy (valid for the call) |
| `args`, `argc` | in | the marshalled arguments: nil / boolean / integer / number / string / table (canonical JSON); `args` is null when `argc` is 0 |
| `rets`, `ret_cap` | in | the result slots (`ret_cap` == `Limits::max_host_ret_values`, 0 slots when that is 0) |
| `produced` | out | how many of `rets[0..produced)` are valid; ignored when the callback answers false |
| `err`, `err_cap` | in | POD scratch, 512 bytes, reset before every call — the no-allocation way to report a failure in the host's own words |
| `error` | out | the `kimix::string` twin of `err`, raised when the callback answers false |
| `index` | in | the 1-based ordinal of this call inside the run — the number `max_host_calls` counts |

Answer `true` with `produced` set, or `false` with `error` (or `err`) filled in.
Producing more than `ret_cap` results is a sticky `LIMIT`, not a memory bug.
`kimix_lua_host_ctx` in the C ABI is this struct minus the value payloads: the
adapter has already built the `kimix_lua_value` argument and result views.

### The longjmp rule applies here too

`sb_host_trampoline` is a `lua_CFunction` (§5), so it may hold nothing a raise
would skip. Every per-call buffer lives in `SandboxState`: `host_args`,
`host_rets`, `host_error` (a `kimix::string`) and `host_err` (its 512-byte POD
twin), all four cleared before every call so a stale message cannot be read back
by the next one. Both raises happen with no foreign frame alive — argument
marshalling and the result push run inside the trampoline, never while host code
is on the stack — and the trampoline's one non-trivial local, `HostCall` (it
carries a `kimix::string`), dies in a block that is exited before any raise on
either path.

### What a host function must NOT do

| Must not | Why |
|---|---|
| keep `args`, `rets`, `err` or a `Value *` past the call | they are state-owned scratch reused by the next call; the pointer is dead the moment the callback returns |
| call back into the same `Sandbox` | the in-flight guard answers `Status::internal` (`INTERNAL: concurrent run on one Sandbox (a lua_State is single-threaded)`); the outer run finishes normally, nothing re-enters |
| block | the deadline is a wall-clock check inside the count hook, so it is observed on the NEXT interpreter tick — a blocking host function stops the run for as long as it blocks |
| rebind while its own `call.name` is live | `name` is a view over the Sandbox's binding list, and `bind_host_function`/`set_global` replace that list. Rebinding between calls is fine; so is rebinding inside a call that then stops touching `name`, because the trampoline re-resolves its binding by index on the next call |
| touch the interpreter | there is none: `HostCall` carries no `lua_State`, and no Lua header leaves the module |

### The three caps, and why the count hook cannot replace them

| `Limits` field | Default | Overrun |
|---|---|---|
| `max_host_calls` | 10'000 | sticky `LIMIT: host call budget exhausted (N calls)` |
| `max_host_args` | 16 | sticky `LIMIT: host function received N argument(s); the sandbox allows M` |
| `max_host_ret_values` | 16 | sticky `LIMIT: host function produced N result(s); the sandbox allows M` |

The count hook (§L4) bounds *interpreter* work: a host call is one VM instruction
however much work the host does inside it, so the hook cannot bound how much
foreign work a script causes. It cannot fire while a host frame is live either (no
instruction executes there), and its granularity — 10'000 instructions by default —
would let a script issue thousands of calls before the first tick. The call count,
the argument count and the result count are three different numbers, so a single
instruction counter cannot stand in for them. All three raises are sticky, so the
usual `pcall` loop cannot spend a budget twice.

### Reserved names

A host may not bind or set a name the environment itself installs: `print`,
`pcall`, `load`, `tostring`, `warn`, `collectgarbage`, `string`, `table`, `math`,
`utf8`, `coroutine`, the tripwire tables and functions (`io`, `os`, `package`,
`debug`, `require`, `dofile`, `loadfile`, `loadlib`), `_G`, and the safe base
functions the environment copies. Overwriting one would either delete a capability
(`set_global("print", nil)`) or make the integrity post-check answer
`Status::behaviour` for the rest of the Sandbox's life, so
`bind_host_function`/`set_global` return false — host time, no raise, and the C ABI
maps that false to `KIMIX_ERR_INVALID_ARG`.

The single source of truth is the `kEnvReservedNames` table in script_env.cpp,
consulted through `sb_env_name_reserved`; test_script_host's
`every_environment_name_is_reserved` walks the built environment and fails if a
name it exposes is missing from the table. `add_native` keeps the older,
unrestricted rule (§6) on purpose.

### Determinism

The sandbox pins its interpreter seed and `math.randomseed`, so `pairs` order and
`math.random` are reproducible run to run. A host function is the one sanctioned
non-deterministic surface — a clock, randomness, a lookup, a capability with no
file or process behind it — and keeping it deterministic when the caller wants
reproducible runs is the caller's job: a host that answers from `index` alone
stays reproducible, a host that reads the wall clock does not.

### Message shapes

A host failure is a NORMAL Lua error, so the script's own `pcall` catches it and
the run can still end `ok`:

```
script.lua:3: RUNTIME: host function 'boom' failed: nope
```

Every cap overrun is instead a STICKY `LIMIT`: it aborts the run, no `pcall` can
swallow it, and `Result::results` comes back empty. `run(RunSpec)`'s `call_name`
that is not a function is `RUNTIME` too, but carries no location — nothing
script-visible is executing when the lookup fails:

```
RUNTIME: 'answer' is a number, not a function
```

## 8. Files

| File | Role |
|---|---|
| `script_sandbox.h` | the public C++ surface: `Sandbox`, `Limits`, `Result` |
| `script_status.h` | `Status`, the message prefixes, `Violation`, `Result` |
| `script_value.h` | `Value` — the only thing that crosses the boundary (no pointers) — plus the canonical-JSON writer and the strict reader (`sb_json_to_lua` / `sb_json_check`) |
| `script_state.h/.cpp` | `SandboxState`, state creation/teardown, panic, warnings, the `Sandbox` object |
| `script_alloc.h/.cpp` | the budgeted `lua_Alloc` (memory cap + sticky OOM) |
| `script_env.h/.cpp` | `_ENV`: allow list, replacements, tripwires, redaction, integrity post-check |
| `script_host.h/.cpp` | the host surface: `bind_host_function`, `set_global`, `clear_globals`, the host trampoline, `Value` → stack and (via script_value.cpp) table text → Lua table; the binding's two upvalues and the stale-binding check |
| `script_error.h/.cpp` | the one message format and the raise helpers |
| `script_sticky.cpp` | first-violation-wins + re-raising the sticky message |
| `script_hook.cpp` | the count hook: deadline, instruction budget, cancel |
| `script_run.cpp` | THE single `lua_pcallk` boundary, the message handler, the run flow |
| `script_api.h` | `KIMIX_SCRIPT_API` (three states, `_STATIC` → nothing) |
| `script_lua.h` | the only include of the vendored Lua C API (wrapped in `extern "C"`) |

## 9. Tests

| Target | What it proves |
|---|---|
| `test_script_sandbox` | lifecycle, isolation, no stderr, no leak, marshalling + caps, memory/timeout/output budgets, cancel, concurrent-run rejection, natives, and that installing no host binding changes nothing (same status, same results, no message) while the default host budgets stay 10'000 / 16 / 16 |
| `test_script_hostile` | the abort contract: 17 forbidden APIs × 7 wrappers, bytecode refusal, pointer hygiene, a denial inside a `__gc`, primitive replacement ⇒ `behaviour`; and that the host surface smuggles no capability either — a table argument carrying `__index`/`__len`/`__pairs`/`__tostring` is marshalled without invoking any of them (raw traversal only), `string.dump` of a bound host function is denied before the host is reached, and a denial raised after a host call still wins |
| `test_script_surface` | the golden capability allowlist (any new reachable global fails the suite until reviewed) and the tagged tripwires |
| `test_script_host` | the host surface: a bound function called from a script, every crossable argument/result kind (tables as canonical JSON both ways), the three host budgets (sticky), a host failure as a non-sticky catchable RUNTIME, `set_global`/`clear_globals`, `run(RunSpec)` with a named call, a re-entrant `run()` answered with `internal`, the strict inbound reader (garbage/depth-bomb/duplicate/over-cap string), `bind_host_function`'s validation, and the reserved-name rule |
| `test_script_bench` | env-gated benchmark (`KIMIX_SCRIPT_BENCH=1`): fresh vs reused state, hook granularity, marshalling, the denial path |
| `test_lua` (untouched) | the raw vendored library still behaves (it uses `luaL_newstate`, which the sandbox avoids) |

```bash
xmake f -m debug -c -y && xmake build
xmake run test_script_sandbox
xmake run test_script_hostile
xmake run test_script_surface
xmake run test_script_host
python scripts/check_lua_sandbox.py          # the source lint
python bootstrap.py --test                   # smoke binary, not the suites
```

## 10. Residual risks and open decisions

| Risk | Status | Mitigation |
|---|---|---|
| Uninterruptible C work: a pathological `string.find`/`gsub` pattern burns CPU with 0 hook strikes | **open** | the memory budget bounds the subject size; a match-step budget in `lstrlib.c` (behind a `KIMIX_LUA_SANDBOX` define) or process isolation would close it |
| A host bug that calls the Lua API unprotected ⇒ panic ⇒ `abort()` | accepted | the panic names itself a host bug; `src/script` never leaks a `lua_State` |
| `lua_close` still runs finalizers that may call script code | accepted | a protected `collectgarbage('collect')` runs first, inside the run's protection, so a denying finalizer becomes a sticky violation |
| `LUA_IDSIZE = 60` truncates long chunk names inside a message | accepted | keep names ≤ 40 chars; the full name is in `violation.chunk` |
| A concurrent `run()` on one `Sandbox` | rejected | `Status::internal` (`busy` is an atomic exchange) |
| The vendored Lua sources are unmodified | deliberate | the sandbox is written from the host side; the optional `KIMIX_LUA_SANDBOX` source hardening of the plan (S-I8) is **not** applied, because `string.format('%p')` and `%s` of a non-scalar are already closed by wrapping `string.format` in `script_env.cpp` |
| Another module can still open `io` | by design | the lint plus "never return a `lua_State*`" |

Open decisions, with the default that is implemented:

1. **Coroutines** — off by default (`allow_coroutines(false)`); enabling installs
   re-raising `resume`/`close` wrappers.
2. **`collectgarbage`** — only `"count"`, only when enabled. Default off.
3. **Clock/env/randomness** — denied. A host that needs one injects a native.
4. **`load`** — text only (mode `"t"`), reader functions accumulated against the
   source cap, `_ENV` injected.
5. **Table results** — canonical JSON (minified, keys byte-sorted), depth ≤ 32,
   ≤ 256 KiB by default. Tables cross IN in the same format, through the strict
   reader: no comments, no NaN/Infinity, no duplicate keys, and a `null` becomes
   nil — so an object key whose value is null disappears and an array null is a
   hole. A table global is validated once at `set_global` time (plain false on a
   bad text) and materialised by the run's install step.
6. **Module name** — `src/script/`, `kimix::script`, `kimix-script`.
