---
name: debug
description: Debug crashes and test failures via stack-traces, host-side logging, and buffer inspection.
---

# Debugging C/C++ Applications

## 1. Interpreting Stack-Traces

When a crash or error is emitted, capture the full console output first.

**What to look for:**
- **Top frames** — the actual fault (null dereference, assertion, backend error).
- **Project frames** — functions from your own codebase. These are the call-sites that triggered the error.
- **Library/backend frames** — symbols from third-party libraries tell you which external path failed. Here that means the vendored `src/ext` libraries (`mimalloc`, `yyjson`, `reproc`, `sqlite`, `pybind11`, `cpp-httplib`/`mbedtls`) or a system DLL (`ntdll`, `ucrtbase`).
- **Last log line** — often the preceding log message shows the dispatch or function name that triggered the bug.

**Action:**
1. Read the innermost frame (first after the crash header). This is the immediate cause.
2. Walk upward until you hit a recognizable API call from your codebase. That is the *call-site*.
3. If the trace ends inside a driver/shared library, suspect (a) invalid resource usage (out-of-bounds buffer/image access), or (b) backend-specific limitation.

**Project-specific facts (this repo):**

- `kimix::backtrace()` (`src/core/platform.h:63`) is **not implemented**: it returns an empty vector on Windows (`src/core/platform.cpp:135-138`) and on Unix (`src/core/platform.cpp:194-197`). Never plan a debug step around it — capture the trace from outside the process with `scripts/debugger.py` (§8).
- No crash handler is installed anywhere in `src/` (no `SetUnhandledExceptionFilter`, no vectored handler), and CRT/assert dialogs are redirected to stderr and the WER dialog suppressed by default (`src/xmake.lua:71-73` → `src/core/win_message_box_suppression.h:29-37`). A crash therefore leaves *only* its exit code: `0xC0000005` access violation, `0xC0000409` fast-fail / `std::terminate`.
- `DbgHelp` is linked into kimix-core **only in debug mode** (`src/xmake.lua:77-79`), so `-m debug` is the mode to reproduce in.

## 2. Plan Before Fixing

Once the stack-trace points to a file/line or API call, write a **debug plan** in this order:

1. **Hypothesis** — state what you believe caused the failure in one sentence.
2. **Verification** — describe the smallest code change or log addition that can confirm/disprove the hypothesis.
3. **Fix strategy** — if verified, what exactly will you change.
4. **Rollback marker** — note the original state so you can undo cleanly.

**If the fix fails:**
- Save the failed attempt (e.g., with memory/notes).
- Re-read the stack-trace and the saved steps. Do not repeat a failed hypothesis.
- Pick the next most likely cause and repeat from step 1.

## 3. When There Is No Stack-Trace

Silent failures (hang, wrong result, test timeout) provide no trace.

**Find the entry point:**
- C++ tests are standalone Boost.UT executables registered by `test_proj(name, source[, callable])` in `tests/xmake.lua`; the `source` file named there holds `main()`. There is no CMake build in this repo.
- Run the failing binary and read the harness output — `xmake run test_kimix_core`, or the exe directly at `bin/debug/test_kimix_core.exe`. The output directory is `bin/<mode>` (`kimix_bin_dir` defaults to `bin`, `xmake.lua:39-41`; the mode suffix is appended in `scripts/xmake_func.lua:150-157`, and the targetdir is set at `xmake.lua:201-205`).
- Boost.UT's own CLI: a **leading** positional argument is the name filter (`bin/debug/test_kimix_core.exe "add*"`), and any unknown option *after* the first option is a hard `exit(-1)` (`tests/ut/ut.hpp:801-813`). `--list-test-names-only` prints the suite (`tests/ut/ut.hpp:735`).
- If the run reports `0 asserts in N tests`, nothing executed: the Boost.UT harness only runs a test body when exceptions are enabled (`tests/xmake.lua:57-64`) — check the target's `set_values("kimix_enable_exception", true)`.
- Do not trust a leftover exe in `bin/debug`: the deleted ad-hoc e2e binaries (`soul_e2e.exe`, `new_tools_e2e.exe`) are still on disk although `xmake show -l targets` no longer lists them. The target list is the truth, not the directory.

**Add host-side logging:** there is no logging macro, no logger object and no log-level switch in this repo — the project is built exception-free and failures travel by return value, so diagnostics are plain `stderr` writes, exactly like the production code (`src/agent/soul.cpp:2181`, `src/core/binary_file_stream.cpp:21`):

```cpp
std::fprintf(stderr, "[dbg] %s:%d size=%zu\n", __func__, __LINE__, buf.size());
// kimix containers are STL-shaped: .size(), .data(), operator[] all work.
// For many values, build one line with kimix::StringScratch and print it once:
// (src/core/string_scratch.h:44, c_str() at :139, size() at :148)
kimix::StringScratch ss;
ss << "n=" << n << " first=" << v[0];
std::fprintf(stderr, "[dbg] %s\n", ss.c_str());
```

Write to **stderr**, not `stdout`: output that is still sitting in a queue or a buffer when the process dies is lost (§4).

**Progressive narrowing:**
1. Log at the start of `main()` and at every major phase (init → allocation → processing → I/O).
2. If the failure is a hang rather than a crash, skip to §4.
3. If the failure is a wrong value, dump the buffer itself instead of one log line per element — §5.

## 4. Hangs, Deadlocks and Asynchronous Output

This is a CPU-only library: there is no device, kernel or shader, so there is no "device-side logging" step and no `device_log` facility to look for. A hang is always a host thread that never returns:

- `kimix::spin_mutex::lock()` spins until it wins the lock — no timeout, no yield (`src/core/spin_mutex.h:20-30`). A missed `unlock()` or a recursive take hangs silently while burning one core; break in with a debugger (or TSan, §9) rather than waiting for it.
- `conditional_mutex_t<false, Mutex>` (`src/core/thread_safety.h:14-24`, and the `false` specialization at `:36`) compiles `lock()`/`unlock()` **away**, and `Pool<T, bool ThreadSafe = true>` is thread-safe unless you opt out (`src/core/pool.h:41`). A `ThreadSafe = false` container can never deadlock — so a race there means the flag is wrong, not that lock order is broken.
- `kimix::runtime::PrintStream` is asynchronous: `print()` enqueues into a lock-free `rbc::ConcurrentQueue` and a dedicated worker thread writes/fflushes `stdout` (`src/runtime/print/print_stream.h:4-6`, `:179`). "The log line disappeared" usually means it was never drained — flush, or fall back to `std::fprintf(stderr, ...)`, before the point where the process dies.
- Destroying a joinable `std::thread` calls `std::terminate` → `abort()` → exit code `0xC0000409` (`src/builtin_tools/agent_tool.cpp:795`). That reads as a crash of the *whole* process ("the CLI died with -1073740791"), not of the worker.

## 5. Buffer-Based Debug Inspection

When you need to inspect many values or avoid per-thread log flooding, accumulate into a buffer and dump it once on the host.

**Pattern:**
1. Reserve a scratch buffer (`kimix::StringScratch`, or a `kimix::vector<T>` sized up front).
2. Have each producer/phase write its payload into it instead of logging per item.
3. Dump once (`std::fprintf(stderr, ...)`, or `FirstFit::dump_free_list()` below) at the point where the failure is visible.

**Reducer pattern for conditional values:**
- Reserve slot 0 of the buffer as a counter.
- Each worker thread atomically increments the counter and writes its payload into `debug_buf[counter]`.
- This captures the first N interesting events without over-allocating or flooding the log.

**Inspectors the allocator code already ships:**

| Facility | What it tells you | Evidence |
|---|---|---|
| `kimix::FirstFit::dump_free_list()` | every free block (`offset`/`size`) plus total free bytes, printed to `stderr` | `src/core/first_fit.h:168-178` |
| `FirstFit::buffer()` / `total_size()` / `begin()`–`end()` | the raw managed region and iteration over free-list nodes | `src/core/first_fit.h:180-183` |
| `kimix::Pool<T, ThreadSafe>::allocated_count()` | live object count — the cheapest double-free / leak invariant | `src/core/pool.h:100` |
| `kimix::StringScratch` (`operator<<`, `c_str()`, `size()`) | bulk formatting into one buffer for a one-shot dump | `src/core/string_scratch.h:44,139,148` |

**The heap is mimalloc, not the CRT.** `IOperatorNewBase` routes every `operator new` to `mi_malloc` (`src/core/memory.h:4-5`) and mimalloc is built with `MI_WIN_NOREDIRECT`, so it does not hook the CRT allocator (`src/ext/xmake.lua:21`):

- In a `-m debug` build the vendored mimalloc compiles with `MI_DEBUG=2` (the default whenever `NDEBUG` is undefined, `src/ext/mimalloc/include/mimalloc/types.h:76-81`). xmake adds `-DNDEBUG` only for the release-family modes (`<xmake>/rules/mode/xmake.lua:69-71,101-103,132-134,169-171`) and kimix's own rule never adds it, so debug builds really do get the checks: double free, corrupted free list and invalid-pointer free then assert **inside mimalloc**, which is often a sharper report than ASan.
- Because mimalloc sub-allocates from its own pages, ASan's redzones sit at the region boundary, not behind each `mi_malloc` block: an overflow of one `kimix::vector` can slip past ASan and surface later as a corrupted free list — dump the free list.
- The whole process deliberately shares **one** mimalloc heap across module boundaries (`src/ext/xmake.lua:15-19`), so freeing a pointer with the wrong module's deallocator is a first-class crash candidate. Across the C FFI, memory must be released by the matching `kimix_mem_free` / `kimix_vec_destroy` / `*_doc_free` ("one heap, and the matching free", `docs/ffi.md`).

## 6. Environment Variables for Backend Diagnosis

There is no shader/bytecode dump and no API validation layer to switch on — this library has no GPU backend. The switches that do exist decide *which implementation runs*, which is the fastest way to localise a failure to native C++ or to the Python fallback:

| Variable | Effect | Evidence |
|---|---|---|
| `KIMIX_NATIVE=0` | never import `runtime_py`; run the pure-Python shim for every kernel | `python/kimix_native/__init__.py:24` |
| `KIMIX_NATIVE=1` | require the compiled module (`ImportError` if it cannot load) instead of falling back silently | `python/kimix_native/__init__.py:9` |
| `KIMIX_NATIVE_<KERNEL>=0` | per-kernel override (e.g. `KIMIX_NATIVE_TEXT=0`) to bisect which kernel diverges | `python/kimix_native/__init__.py:39` |
| `KIMIX_PYTHON_EXECUTABLE=<path>` | pin the interpreter the python tool spawns (legacy name `PYTHON_EXE`) | `src/builtin_tools/python_tool_class.cpp:234-237` |

> **Windows env-var trap:** `runtime_py.pyd` is built against the debug CRT while `python.exe` uses the release CRT, and each CRT instance keeps its own `getenv` table. That is why the binding layer reads the environment through `GetEnvironmentVariableA` (`src/runtime/py/module.cpp:32-37`; same reasoning in `src/builtin_tools/read_image_tool.cpp:70-78`). If a variable set from Python is invisible to C++, suspect `std::getenv` before you suspect your spelling.

## 7. Decision Checklist

| Symptom | First Action | Next Action |
|---|---|---|
| Crash with stack-trace | Read innermost + first project frame | Hypothesize → plan → fix |
| Crash, no trace (bare exit code, e.g. `0xC0000005`) | Re-run the exe under `scripts/debugger.py` (§8) | Read the PDB-based trace; dialogs are suppressed, so this is the only trace you get |
| Silent wrong result | Add `stderr` logging at entry points (§3) | Dump the buffer / free list instead of per-item logs (§5) |
| Thread hangs, process at 100% CPU | Suspect `spin_mutex` (no timeout, `src/core/spin_mutex.h:20-30`) or an undrained queue | Break in via §8; check `unlock()` on every path (§4) |
| `std::terminate` / `0xC0000409` | Look for a joinable `std::thread` being replaced or destroyed | Move the handle out and join first (`src/builtin_tools/agent_tool.cpp:795` records exactly this bug) |
| "0 asserts in N tests" | The test target lost exceptions, so no body ran | Keep `set_values("kimix_enable_exception", true)` (`tests/xmake.lua:57-64`) |
| Exe dies before `main()`, no output | Missing import: `runtime_py.pyd` links `python3xx.dll` | Use `xmake run <test_target>`, which prepends the Python dir to `PATH` (`tests/xmake.lua:10-11,67-73`) |
| C++ and Python disagree | Re-run the Python side with `KIMIX_NATIVE=0` (§6) | The shim is now the reference; diff the two outputs |
| Crash inside `runtime_py.pyd` / `kimix_api.dll` | Run `python.exe` itself under `scripts/debugger.py` with `bin\debug` as the PDB path (§8) | Check the pybind/FFI rules below (§8) — exceptions and heap ownership both cross that boundary |
| Local compile/syntax error after an edit | `python scripts/check_cpp_syntax.py <file>` (clangd diagnostics, `scripts/check_cpp_syntax.py:336`) | `xmake build <test_target>` for the real compiler errors |
| Memory error (use-after-free, OOB) | Rebuild with `--policies=build.sanitizer.address` | Read sanitizer report for alloc/access/dealloc trace; also check the mimalloc `MI_DEBUG=2` assert (§5) |
| Data race / deadlock | Rebuild with `--policies=build.sanitizer.thread` | Read sanitizer report for conflicting access sites (clang/gcc; see §9) |
| Undefined behavior | Rebuild with `--policies=build.sanitizer.undefined` | Fix flagged operations (shifts, overflows, misaligned ptrs) |

## 8. Windows Crash Debugging with `scripts/debugger.py`

A lightweight Python debugger using Windows Debug API + DbgHelp.dll to launch an x64 executable, catch second-chance exceptions, and print a symbolic stack trace from PDB symbols.

**Usage:**
```bash
python scripts/debugger.py <path_to_exe> [pdb_search_path] [-- <args>...]
```

- **No `argparse`** — the positions are fixed (`scripts/debugger.py:717,728-730`): token 1 = exe, token 2 = `pdb_search_path`, everything after the **first** `--` is forwarded to the target (`:720-726`). `pdb_search_path` defaults to the EXE's own directory (`:558-559`); `--help` is not a flag (it would be taken as the exe path) and a missing exe raises `FileNotFoundError` (`:554-555`).
- xmake writes `bin/<mode>/<target>.pdb` next to the exe (e.g. `bin/debug/test_kimix_core.pdb`), so the default search path is usually enough.
- System frames are resolved through the Microsoft symbol server prefix `SRV*C:\Symbols*https://msdl.microsoft.com/download/symbols`, prepended to your directory (`:563`) — first run downloads symbols and is slow.
- It **launches** the target (`CreateProcess` + `DEBUG_PROCESS`, `:574-578`); it cannot attach to an already-running process.
- A trace is printed for **every** exception, first-chance included (`:620-633`), so the initial breakpoint and ordinary SEH/C++ exceptions show up as noise. The fatal one is followed by `Second-chance exception; the process is terminating.` (`:641-643`), after which the debugger exits.
- It polls `WaitForDebugEvent` with a 5 s timeout and loops forever (`:550,605-608`): if the target hangs, the debugger never returns — break out with Ctrl-C and use §4.
- Works on **Windows x64** with **Python 3.x** (64-bit recommended: the x64 `CONTEXT`/`StackWalk64` layout is hard-coded, `:56-63`).

**Examples** (a test binary; `bin\debug\test_kimix_core.pdb` sits next to the exe):
```bash
python scripts/debugger.py bin/debug/test_kimix_core.exe
python scripts/debugger.py bin/debug/test_kimix_core.exe -- "add*" # Boost.UT name filter
```

**Debugging a crash inside the pybind / FFI layers:**

`runtime_py.pyd` and `kimix_api.dll` are ordinary x64 images with their PDBs next to them (`bin/debug/runtime_py.pdb`, `bin/debug/kimix_api.pdb`), and the debugger loads symbols for every `LOAD_DLL_DEBUG_EVENT` (`:679-689`), so a fault *inside* either one resolves — if you debug the host that loads them. `<python_dir>` below is `python -c "import sys; print(sys.executable)"`:

```bash
python scripts/debugger.py <python_dir>\python.exe bin\debug -- -m pytest python/tests -k <expr> -x
```

- Args after `--` are joined into the command line with plain spaces and **no re-quoting** (`:570-572`), so prefer separate simple tokens (as above) or a `.py` repro file over `-c "...; ..."`.
- Exceptions may exist only in the binding layer `src/runtime/py/*.cpp` (`kimix_exceptions_targets`, `xmake.lua:64-75`); a `throw` that reaches an exception-free kernel TU (`/EHs-c-` at `scripts/xmake_func.lua:305`, `_HAS_EXCEPTIONS=0` at `:312`) is a `std::terminate` → `0xC0000409`, not a Python traceback.
- Every binding call releases the GIL around the kernel (GIL policy: `src/runtime/py/module.cpp:8-9`, guard at `:188`; `src/runtime/common/gil.h:39-44`) and must not call back into Python while released (`src/runtime/common/gil.h:25`): a pyd call that hangs with the other Python threads frozen is that rule broken, not a slow kernel.
- The FFI hands out pointers from its own single mimalloc heap; the matching `kimix_*_free`/`*_destroy` is the only legal deallocator (§5, `docs/ffi.md`), and a foreign-language double free faults **inside** `kimix_api.dll`.

## 9. Sanitizer Usage with XMake

XMake provides built-in support for compiler sanitizers (AddressSanitizer, ThreadSanitizer, etc.) to detect memory errors, data races, undefined behavior, and leaks at runtime.

### Via Legacy Mode Rules (do not use)

`mode.asan`, `mode.tsan`, `mode.msan`, `mode.lsan` and `mode.ubsan` still exist, but each one now only sets the matching policy *and* prints a deprecation warning (`<xmake>/rules/mode/xmake.lua`: e.g. `:231` `target:set("policy", "build.sanitizer.address", true)` and `:234` `wprint("deprecated: please use set_policy(...)")`; same pattern at `:260, :289, :318, :347`).

Worse, `xmake f -m asan` alone does nothing in this repo: the project only registers `add_rules("mode.release", "mode.debug", "mode.releasedbg")` (`xmake.lua:2`), so no other mode rule is even loaded — check it yourself with `xmake show -t test_kimix_core`: the only `mode.*` rules on it are those three. Use the policy-based form below.

### Via Policies (Recommended)

The five policies are read at project *and* target level and turned into real compiler flags (`<xmake>/rules/c++/config/sanitizer.lua:36-47`, names declared in `<xmake>/core/project/policy.lua:64-73`). In this repo the natural home for a project-level `set_policy` is the root `xmake.lua`, which already carries two of them (`xmake.lua:3-4`):

```lua
set_policy("build.sanitizer.address", true) -- also: .thread .memory .leak .undefined
```

Or from the command line, which needs no source edit (`--policies` is an `xmake f` option):

```bash
xmake f -m debug --policies=build.sanitizer.address,build.sanitizer.undefined -c -y
xmake build
xmake run test_kimix_core
```

`bootstrap.py` has no sanitizer/`--policies` pass-through (its flags cover toolchain, mode, jobs and `--test`), so a sanitizer run goes through `xmake f` directly.

### Available Sanitizers

| Policy | Rule (legacy) | Detects |
|---|---|---|
| `build.sanitizer.address` | `mode.asan` | Use-after-free, heap/stack buffer overflows, memory leaks |
| `build.sanitizer.thread` | `mode.tsan` | Data races, deadlocks (POSIX threads) |
| `build.sanitizer.memory` | `mode.msan` | Uninitialized memory reads |
| `build.sanitizer.leak` | `mode.lsan` | Memory leaks (standalone) |
| `build.sanitizer.undefined` | `mode.ubsan` | Integer overflow, shift overflow, misaligned pointers |

### Toolchain Caveats (this repo)

- A policy becomes `-fsanitize=<name>` on the compile line, and on the link line for clang/gcc (`<xmake>/modules/private/utils/toolchain.lua:607-616`). Enabling any sanitizer also forces debug symbols on that target (`<xmake>/rules/c++/config/sanitizer.lua:49-53`) and, for `cl` binaries on Windows, expects a VS **17.7+** environment whose ASan runtime directory xmake injects into the target's `runenvs` (`:55-69`).
- Whether a given `<name>` is accepted depends on the compiler, and here a rejected flag is a hard error rather than a silent drop — the project sets `check.auto_ignore_flags` to false (`xmake.lua:4`). For the clang/GCC-only sanitizers (thread, memory) reconfigure with clang: `python bootstrap.py --toolchain clang-cl` (`bootstrap.py:737`).
- clang ASan on Windows only supports the MD/MT CRT (`<xmake>/modules/private/utils/toolchain.lua:619-620`) while kimix debug builds default to **MDd** (`scripts/xmake_func.lua:333`) — add `--kimix_win_runtime=MD` (the `kimix_win_runtime` option, `xmake.lua:46-49`).
- Keep the run step as `xmake run <test_target>`: a test exe that imports `runtime_py.pyd` needs the Python directory on `PATH` (`tests/xmake.lua:10-11,67-73`), which `xmake run` prepends for you.
- A sanitizer report already carries the allocation/access/deallocation stack, so §8 is not needed on top of it. Remember mimalloc sub-allocates: ASan's view of a kimix buffer is bounded by mimalloc's own pages (§5).

## Summary

- **Stack-traces** → innermost frame = cause; upward walk = call-site. `kimix::backtrace()` is a stub — capture traces with `scripts/debugger.py` on a `bin/debug` exe and its PDB.
- **Always plan** before editing; save failed attempts.
- **No trace** → find the failing `test_proj` entry in `tests/xmake.lua`, add `std::fprintf(stderr, ...)` logging; a hang is a host-thread problem (§4), not a missing device log.
- **Bulk value inspection** → accumulate into a buffer and dump once, ideally with the inspectors that already ship (`FirstFit::dump_free_list()`, `Pool::allocated_count()`).
- **Heap errors** → the heap is mimalloc, not the CRT: `-m debug` compiles `MI_DEBUG=2` checks, and the one-heap/one-module ownership rule decides whose `free` is legal.
- **Native vs Python fault** → `KIMIX_NATIVE=0` runs the pure-Python shim (§6); the debug-CRT/pyd split is why some env vars are invisible to C++.
- **Memory/UB/races** → rebuild with `--policies=build.sanitizer.address,build.sanitizer.undefined` (the legacy `mode.asan` rules are deprecated *and* not wired into this repo) and read the report.
