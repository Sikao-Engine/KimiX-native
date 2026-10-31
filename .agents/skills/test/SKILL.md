---
name: test
description: Boost.UT test layout, adding tests, and running them with xmake.
---

# KimixBase Test Guide

Tests are standalone executables using [Boost.UT](https://github.com/boost-ext/ut) v2.3.1 (`BOOST_UT_VERSION 2'3'1`), vendored at `tests/ut/ut.hpp`. Only xmake is supported (there is no CMake build). Every suite is host/CPU-only — this tree has no device/GPU code, so there is no "device test" category.

## Layout

All test source files live in `tests/` under the directories below.

| Directory | Content | Typical extra dep (from the `callable`) |
|---|---|---|
| `unit/core/` | `kimix-core` types, STL wrappers, memory/pool, clock, format, json repair, and `test_core_leg_contract` (the container/ABI/logging/magic_enum contract of BOTH third-party legs) | — |
| unit/ext/ | vendored third-party behaviour (yyjson, xxhash, pybind11, mbedTLS, sqlite) | kimix-mbedtls, kimix-llm, kimix-sqlite3 |
| `unit/api/` | the `kimix_api` C FFI surface, driven through public C headers only | `kimix_api` (only when `kimix_enable_api` is on) |
| `unit/openai/`, `unit/openai_responses/`, `unit/anthropic/` | SSE stream parsers (`llm/openai/sse_parser.h`, `llm/{anthropic,openai_responses}/stream_parser.h` — header-only) | — |
| `unit/llm/`, `unit/kimi/` | provider dispatch, capabilities, wire options, request bodies | `kimix-llm` |
| `unit/builtin_tools/`, `unit/agent/` | built-in tools + the soul/turn loop (retry, pruning, injections, registry) | `kimix-llm` (CLI wiring also pulls `kimix-cli`) |
| `unit/cli/` | CLI args/config/session store/slash commands/renderer | `kimix-llm`, `kimix-cli` (+ `shell32` on Windows) |
| `unit/native/`, `unit/tools/` | runtime kernels behind `runtime_py` (utf8, index, search, codec, parse, diff, glob, security, compress) | `runtime_py`; the history-index and MCP-client suites use `kimix-llm` / `kimix-sqlite3` instead |
| `ut/` | vendored Boost.UT single header (`ut.hpp`) | — |

Registered sources are all named `test_*.cpp` — 140 registered targets, one per source file. Non-registered helpers live beside them: `unit/native/bench_util.h` + `unit/native/soul_test_util.h` (all helpers `inline`, so the unity-batched test TUs do not collide), generated golden tables (`unit/builtin_tools/*_goldens.inc`, `unit/cli/cli_config_goldens.inc`, `unit/native/shell_scanner_names_goldens.inc`) and fixture data (`unit/core/data/json_repair/`). `tests/verify_workspace_parity.py` is a standalone Python parity check, not a Boost.UT target.

Include path setup in `tests/xmake.lua` exposes `tests/` so test sources just write `#include "ut/ut.hpp"`. Do **not** use `../../` relative paths.

## Adding a Test

xmake (`tests/xmake.lua`):

```lua
-- Signature: test_proj(name, source[, callable])   (tests/xmake.lua)
--   callable: optional config callback for extra deps/includes/defines
-- kind: always "binary"; standalone tests link kimix-core + KIMIX_CORE_STATIC,
-- forces exceptions on, and is registered for `xmake test` via add_tests("default")
test_proj("test_kimix_core", "unit/core/test_kimix_core.cpp")

-- With extra config (any library target: kimix-llm, kimix-cli, runtime_py,
-- kimix-sqlite3, kimix-mbedtls, kimix_api):
test_proj("test_invalid_server_json", "unit/llm/test_invalid_server_json.cpp", function()
    add_deps("kimix-llm")
end)

-- Shorthand for the built-in-tool / agent suites: test_proj + add_deps("kimix-llm")
builtin_tools_test("test_builtin_edit", "unit/builtin_tools/test_edit_tool.cpp")
```

Registration is manual — a source file with no `test_proj` line never builds (the unregistered `unit/ext/test_eastl.cpp` and `unit/ext/test_mimalloc_*.cpp` are dead files). The whole file returns early when `kimix_enable_tests` is off, and `test_kimix_api` is additionally wrapped in `if has_config("kimix_enable_api") then` because `src/api` is skipped at file level. Tests that link `kimix-llm` / `kimix-cli` / `runtime_py` need no per-option `if`: the `kimix_feature_gate` rule (`scripts/xmake_func.lua`) disables them when their dep's option is off.

## C++ Test Templates & Style

> **Important rule:** Always keep test logic (assertions, setup, exercise, verify) in `main` function scope — never in file-scope `static auto` lambdas. File-scope static registrations can have unpredictable static initialization order and make it harder to control test filtering via CLI arguments.

### Template: Unit Test (main-scope pattern)

For simple CPU-only tests of the `kimix-core` library.

```cpp
// Test for <header>.h
// This test covers: <list of features>

#include "ut/ut.hpp"
#include <core/kimix_core.h>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "<scenario_name>"_test = [] {
        expect(true) << "description";
    };

    "<scenario_name2>"_test = [] {
        expect(condition) << "message on failure";
    };
}
```

### Simpler template (main scope, no separate test functions):

```cpp
#include "ut/ut.hpp"
#include <core/kimix_core.h>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "<feature>"_test = [] {
        // test body
        expect(condition);
    };
}
```

### Includes — canonical order

1. Test framework: `"ut/ut.hpp"`
2. Shared test helper (when the suite has one): `"bench_util.h"`
3. Project headers, module-qualified from `src/`: `<core/kimix_core.h>`, `<agent/soul.h>`, `<llm/llm.h>`, `<runtime/common/utf8.h>`, `<api/kimix_api.h>`
4. Standard library: `<cstdio>`, `<cmath>`, `<vector>`, etc.

### Using declarations

```cpp
using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix;    // when using kimix:: functions
```

The C-FFI suite (`unit/api/test_kimix_api.cpp`) is the exception: it names only what it uses — `using boost::ut::expect; using boost::ut::operator""_test;` — and its `main()` takes no args, so it is not name-filterable.

### Naming conventions

| Element | Convention | Example |
|---|---|---|
| Test source file | `test_<feature>.cpp` | `test_kimix_core.cpp` |
| Main scope test lambda | `"<snake_case_description>"_test` | `"add_basic"_test`, `"multiply_negative"_test` |
| Test executable | `test_<feature>`, usually area-prefixed (`test_native_*`, `test_builtin_*`, `test_cli_*`); need not match the file stem | `test_native_utf8` ← `unit/native/test_utf8.cpp`, `test_builtin_bash` ← `unit/builtin_tools/test_bash_tool.cpp` |
| Shared helper header | `<area>_util.h` next to the suites that use it | `unit/native/bench_util.h` |
| Generated golden table | `<topic>_goldens.inc`, `#include`d from the suite | `unit/builtin_tools/bash_rtk_goldens.inc` (regen: `scripts/gen_bash_fix_data.py --rtk`) |

### Assertions

```cpp
expect(condition);
expect(condition) << "descriptive message on failure";
expect(ptr != nullptr);
expect(eq(a, b)) << "values should be equal";        // Boost.UT eq()
expect(neq(a, b));
expect(gt(a, b));
expect(lt(a, b));

// Float comparison — always use epsilon, never direct ==
expect(std::abs(result - expected) < 1e-4f);

// For complex validation — accumulate errors, expect once
bool all_correct = true;
for (size_t i = 0; i < n; i++) {
    if (results[i] != expected[i]) {
        printf("Mismatch at [%zu]: got %d expected %d\n", i, results[i], expected[i]);
        all_correct = false;
    }
}
expect(all_correct) << "all elements must match expected values";
```

### File header comment

Every test file starts with a descriptive comment block:

```cpp
// Test for <module/feature>.
// This test covers:
// - <feature 1>
// - <feature 2>
```

### Test organization within a file

- Each test function covers one logical area
- Test function bodies are self-contained: create their own objects, run, validate
- Prefer many small `"name"_test` lambdas over one giant test
- Fixture builders may live at file scope, but inside an anonymous `namespace { ... }` (105 of 144 suites do this) — only the `"_test"` registrations belong in `main`
- Environment-dependent suites probe for the tool and skip themselves instead of failing (e.g. `test_builtin_process_runner`, `test_python_code_session` need a real bash/python)

## Build registration

**xmake** (`tests/xmake.lua`):

```lua
test_proj("test_kimix_core", "unit/core/test_kimix_core.cpp")
```

## Running

Before running any test binary, complete a full build:

```bash
xmake f -m debug -c -y       # configure
xmake build                  # build all targets (tests included when kimix_enable_tests=true)
xmake build test_kimix_core # build just the test target
xmake run test_kimix_core    # run one test binary
xmake test                  # run every registered test binary (test_proj adds add_tests("default"))
xmake test test_kimix_core/* # ...or one of them by name
./bin/debug/test_kimix_core.exe # or run the binary directly
./bin/debug/test_kimix_core.exe add_basic # run ONE test, by exact name
./bin/debug/test_kimix_core.exe --list-test-names-only # discover the names
```

> **Filtering caveat (vendored ut 2.3.1):** `cfg::parse` translates `*` into `.*`, but `detail::utility::regex_match` is a hand-rolled matcher that only understands `.` as a one-character wildcard — so a `*` in your pattern has to match a literal `*` in the test name. `"add*"` therefore runs **nothing** and still exits 0 (`all tests passed (0 asserts in 4 tests); 4 tests skipped`), a false green. Use the exact test name, `?` for one character, or `!name` to run everything except `name`. Filtering only works in suites that call `boost::ut::detail::cfg::parse_arg_with_fallback(argc, argv)` in `main` (79 of 144); an `int main()` binary ignores argv. `python bootstrap.py --test` is unrelated: it runs `xmake run kimix-test`, the hand-written `src/test/main.cpp` smoke binary.

### Slow suites (built, but not run by default)

The default `xmake test` sweep must stay a fast gate. Nine suites are therefore excluded from it and run only on request:

```bash
xmake f --kimix_slow_tests=y     # reconfigure once
xmake test                       # now includes the slow suites
```

The list lives in `tests/xmake.lua` as the `slow_suites` table, with the RELEASE-build seconds that justify each row (measured one process per binary; the median suite is 0.2 s and these nine are ~190 s of the ~350 s total). They are the real-process / large-corpus e2e runs: `test_builtin_process_runner` (74 s — spawn/drain/timeout lifecycle), `test_subagent_bg_startup_stress` (20 s), `test_builtin_bug_tool_report` (19 s), `test_native_glob` (16 s), `test_builtin_workflow` (14 s), `test_builtin_grep_engine_matrix` (13 s), `test_builtin_grep_engine` (12 s), `test_mcp_client` (11 s), `test_native_diff` (10 s).

The targets still BUILD either way, and `xmake run <target>` always runs one — the switch only decides whether `xmake test` executes it. Configuring prints the skipped list, so the omission is never silent. Add a row to that table (with a re-measured number) rather than to any per-file list; do not delete a suite to make a sweep faster.

### Running Tests with Sanitizers

To detect memory errors, undefined behavior, or data races in tests, rebuild with sanitizer policies enabled:

```bash
# AddressSanitizer (use-after-free, buffer overflows)
xmake f -m debug --policies=build.sanitizer.address -c -y
xmake build
xmake run test_kimix_core

# Combined: AddressSanitizer + UndefinedBehaviorSanitizer
xmake f -m debug --policies=build.sanitizer.address,build.sanitizer.undefined -c -y
xmake build
xmake run test_kimix_core
```

> **Tip:** Sanitizers produce detailed stack traces on the first error. Use `-m debug` for debug symbols so traces show file/line info.

Available sanitizer policies:

| Policy | Detects |
|---|---|
| `build.sanitizer.address` | Use-after-free, heap/stack buffer overflows, memory leaks |
| `build.sanitizer.thread` | Data races, deadlocks |
| `build.sanitizer.memory` | Uninitialized memory reads |
| `build.sanitizer.leak` | Memory leaks (standalone) |
| `build.sanitizer.undefined` | Integer overflow, shift overflow, misaligned pointers |

Combine multiple: `--policies=build.sanitizer.address,build.sanitizer.undefined`

Availability is toolchain-specific — see the xmake skill's *Sanitizer Modes*: TSan and LSan are Linux-only and UBSan on Windows depends on a clang runtime, so on this repo's default MSVC host `build.sanitizer.address` is the policy to reach for; verify the others before relying on them.

### Allocation Tracking (KIMIX_MEMORY_STRICT)

Debug builds can track every `kimix::allocate` / `kimix::deallocate` in a mutex-guarded hash map and report the blocks still live at process exit. It is OFF by default, and off means COMPILED OUT — without the option the tracker is not in the binary at all.

```bash
xmake f -m debug -c -y --kimix_memory_strict=true   # enable (reconfigure)
xmake build
xmake run test_memory                               # the tracker's own suite
xmake run <the suite you suspect>                   # one process at a time (see Cost below)
```

- **Debug only.** `release` / `releasedbg` define `NDEBUG`, so `src/core/memory.cpp` emits `#pragma message("kimix: KIMIX_MEMORY_STRICT has no effect when NDEBUG is defined ...")` and compiles the thin forwards only. (`/w` swallows `#pragma message` in this project, so the reliable proof is symbol absence: `bin/release/kimix-core.lib` has no `MemoryTracker`, `bin/debug`'s does.)
- **Cost.** One `std::mutex` + one hash-map insert/erase per allocation, plus ~50 B of bookkeeping per live block. Measured on the whole suite: 197 binaries, ~1600 s of CPU total (~10 min wall at `-j 6`), i.e. roughly 2-3x the default sweep — practical as a one-off audit of the entire tree, but still not something to put in the per-commit gate. Individually, an allocation-heavy suite (an index built from 10k terms) becomes minutes-slow and hundreds of MB heavy: leave `test_native_fuzzy` out of a sweep.
- **`memory_mark_permanent(ptr)` — the "this block IS the process" escape hatch.** The marl scheduler pool (`detail::shared_scheduler_pool()`, `src/core/fiber.cpp`) is created on first use and deliberately never destroyed; it calls `kimix::memory_mark_permanent(pool)` right after `new_with_allocator<marl::Scheduler>`, so the exit report stays silent about it while the block stays tracked (a later `deallocate` is not reported as untracked, and the live counters keep counting it). Use it ONLY for a singleton whose lifetime is the process — a real leak hidden behind it becomes invisible.
- **What it means.** The report lists blocks that are still LIVE at the moment the library unloads, which is not the same as leaked: a process-lifetime fixture, a lazily-built singleton or a still-scoped big index shows up too. A small, stable count whose sizes never change between runs is almost always a fixture; a count that grows with your new code is the leak you are hunting. `deallocate of an untracked ...` (below) is the line that indicates a genuinely broken alloc/free pair.
  A report looks like this, on **stderr**, at static destruction — AFTER the test bodies and after
  Boost.UT printed its summary:
  `[kimix][error] memory: leak check: N allocation(s), B byte(s) never freed`, then up to 64
  `[kimix][error] memory:   leaked block 0x<ptr> size=<n>` lines and `... N more`.
  It never fails a test by itself (exit code is unaffected), so read stderr, not the exit status.
- **Freeing a block the tracker never saw** (an alloc/free pair where one side bypassed the gateway) logs
  `[kimix][error] memory: deallocate of an untracked or already-freed pointer 0x<ptr>` and still frees. That line means a migration was missed — fix the call site rather than ignore it.
- **Assert from a test, without waiting for exit:** `kimix::memory_tracking_enabled()`,
  `live_allocation_count()`, `live_allocation_bytes()`, `memory_leak_report(FILE*)` (`src/core/memory.h`).
  Assert DELTAS around your own allocate/free pair, never an absolute count — other threads, fibers and suites share the map. `unit/core/test_memory.cpp` is the worked example, and it runs in every configuration because each tracker assertion is guarded by `memory_tracking_enabled()`.
- **Intentional leaks in fixtures** show up here. Own the block (an RAII wrapper) or document the one expected report line; do not leave the sweep noisy.
- **Three shapes this monitor has already caught** (fix them in this order before blaming the tracker):
  1. *A `shared_ptr` cycle.* `reg->runner = [reg](...)` makes the registry own a lambda that owns the registry, so neither is ever released (`test_agent_tool.cpp`); capture a raw pointer or a `weak_ptr` instead. A registry/session that owns a callback which captures its own `shared_ptr` is the general shape.
  2. *A `thread_local` kimix container.* A `thread_local kimix::vector` is constructed by the CRT's TLS initializer on every thread that touches it and destroyed by that thread's TLS destructor — which never runs for a thread still alive at process exit (any marl worker). Every such thread leaks the container's debug `_Container_proxy` (16 B) plus its buffer, which reads as "N x 16 byte(s) never freed" where N is the worker count. `src/llm/bash_parser/bp_models.cpp`'s teardown worklist is now an intrusive pointer list for exactly this reason. Per-thread scratch wants trivial thread-local storage, not a container.
  3. *A container buffer dropped instead of freed.* `HashMap`'s move constructor used to hand its node array to nobody (`map._node_array = nullptr` with no `deallocate`); it leaked one buffer per move. If a report shows a handful of container-sized blocks that appear only after a move/assignment, read the move constructor, not the caller.
- **Known still-live blocks (audited by a full sweep, not fixed).** A full strict sweep of all 197 registered
  suites currently ends with three suites reporting, all of it test-side and all of it
  `_Container_proxy`/`ChatResult` member containers built in a test scripted backend:
  `test_connection_recovery` (4 `ChatResult`s = 24 x 16 B + one 32 B block), `test_turn_resilience`
  (7 `ChatResult`s = 42 x 16 B + 4 small blocks) and `test_native_print_stream` (2 proxies plus a
  1 MiB and a 7066 B runtime_py buffer). For the two `ChatResult` suites the tracker proves the
  destructors never ran (no `deallocate` was ever issued for those proxies, `late frees ignored: 0`),
  the creating frames are the ordinary main thread, and the container addresses sit on a frame that
  has already been popped -- the next step is a debugger watchpoint on one leaked proxy, not more
  reading. Everything else in the tree is clean, so a *new* report in any other suite is a real leak.
- **Running the whole sweep by hand.** `xmake test` hides a passing suite's stderr, so a loop over the binaries is the way to see every report (needs Python + the MSVC bin dir on `PATH` for the debug binaries, see the DLL note in the debug skill):
  `for e in bin/debug/test_*.exe; do o=$("$e" 2>&1 | grep '\[kimix\]\[error\]'); [ -n "$o" ] && { echo "== $e"; echo "$o"; }; done`
- The FFI heap is the same heap: `kimix_mem_*` (`src/api/ffi_mem.cpp`) forwards to the gateway, so blocks handed to a binding are tracked too.

## Dependencies

Every test links `kimix-core` (plus `KIMIX_CORE_STATIC` in the standalone leg) and gets `kimix-llm` / `kimix-cli` / `runtime_py` / `kimix-sqlite3` / `kimix-mbedtls` / `kimix_api` only through the `callable`. The include path `tests/` is already exposed so `#include "ut/ut.hpp"` works; `src/` comes in publicly from `kimix-core`, which is why headers are module-qualified (`<core/kimix_core.h>`).

Two `test_proj` defaults are load-bearing:

- **Exceptions stay on** (`set_values("kimix_enable_exception", true)`) even though the library targets build with `kimix_enable_exception=false`. The Boost.UT runner calls a test body only inside `#if defined(__cpp_exceptions)` (`tests/ut/ut.hpp`), so an exception-free test binary registers its suites and silently runs nothing ("0 asserts in N tests") instead of failing.
- **`before_run` prepends the Python install dir to `PATH`**, because `runtime_py.pyd` links `python3xx.dll` and the `unit/native` / `unit/tools` suites load it as a shared library. `xmake run <test>` handles this for you; launching those binaries straight from `bin/<mode>` needs the same directory on `PATH`.

## What Not to Do

- Do not put new test sources directly under `tests/`. Pick the right subfolder.
- Do not add a `test_*.cpp` without registering it in `tests/xmake.lua` — an unregistered file is never compiled or run, and the omission is silent.
- Do not add ad-hoc e2e/demo executables (`src/*/demo/`, `scripts/cli_e2e.py` were removed); real-process coverage belongs in registered suites that skip when the external tool is missing.
- Do not rely on `*` globs in the Boost.UT command line (see the filtering caveat above); pass the exact test name.
- Do not create ad-hoc top-level folders (e.g. `for_agent/`, `next/`, `tmp/`). The layout above is the entire test taxonomy.
- Do not reintroduce other test frameworks. The framework is Boost.UT only. (`kimix-test`, built from `src/test/main.cpp`, is the one legacy exception: a hand-written `printf`-style smoke binary that `python bootstrap.py --test` runs — not a pattern to copy.)
- Do not delete or `// skip` failing tests to make a build pass — fix the code under test instead.
