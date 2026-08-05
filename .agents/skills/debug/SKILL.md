---
name: debug
description: Debug crashes and test failures via stack-traces, host/device logging, and buffer inspection.
---

# Debugging C/C++ Applications

## 1. Interpreting Stack-Traces

When a crash or error is emitted, capture the full console output first.

**What to look for:**
- **Top frames** — the actual fault (null dereference, assertion, backend error).
- **Project frames** — functions from your own codebase. These are the call-sites that triggered the error.
- **Library/backend frames** — symbols from third-party libraries tell you which external path failed (e.g., GPU backend, driver, system library).
- **Last log line** — often the preceding log message shows the dispatch or function name that triggered the bug.

**Action:**
1. Read the innermost frame (first after the crash header). This is the immediate cause.
2. Walk upward until you hit a recognizable API call from your codebase. That is the *call-site*.
3. If the trace ends inside a driver/shared library, suspect (a) invalid resource usage (out-of-bounds buffer/image access), or (b) backend-specific limitation.

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
- Read the build file (CMakeLists.txt or xmake.lua) near the failing target to locate the executable source file and its `main()`.
- Identify the test harness and how the runtime is initialized.

**Add host-side logging:**
```cpp
// Use your project's logging macros, e.g.:
LOG_DEBUG("Entering {}:{}", __FILE__, __func__);
LOG_INFO("Buffer size = {}", buf.size());
LOG_TRACE("Dispatching operation X");
```

**Set log level early** (before main logic if possible):
```cpp
set_log_level(LOG_LEVEL_VERBOSE);  // or LOG_LEVEL_DEBUG
```

**Progressive narrowing:**
1. Log at the start of `main()` and at every major phase (init → resource creation → processing → dispatch).
2. If the failure happens during a GPU/compute dispatch, add device-side logging.
3. If the failure is a wrong numerical result, use buffer read-back to inspect values.

## 4. Device-Side / Kernel Logging

For GPU/shader debugging, use whatever logging mechanism your compute API provides:

- **CUDA**: `printf()` inside kernels, captured via `cudaDeviceSynchronize()`
- **DirectX**: `printf()` in HLSL with debug layer enabled
- **Vulkan**: `debugPrintfEXT` extension
- **Metal**: `printf()` in MSL with debug device
- **Custom compute APIs**: Check for `device_log` or similar async logging facilities

**Important:** Device logs are asynchronous. Always synchronize the stream/queue before assuming all logs have arrived. If a kernel hangs, the callback may never fire for logs buffered inside the failing dispatch.

## 5. Buffer-Based Debug Inspection

When you need to inspect many values or avoid per-thread log flooding, write results into a buffer and read back on the host.

**Pattern:**
1. Allocate a debug buffer on the device.
2. Write computed values to the buffer from within kernels/shaders.
3. Synchronize and read back to host memory.
4. Inspect values on the host.

**Reducer pattern for conditional values:**
- Allocate a counter buffer at index 0.
- In the kernel, atomically increment the counter and write the debug payload into `debug_buf[counter]`.
- This captures the first N interesting threads without over-allocating.

## 6. Environment Variables for Backend Diagnosis

Many compute frameworks support environment variables for debug output:

| Variable | Effect |
|---|---|
| `*_DUMP_SOURCE=1` | Dumps generated shader/bytecode sources. |
| `*_LOG_LEVEL=verbose` | Sets maximum verbosity at startup. |
| `*_ENABLE_VALIDATION=1` | Wraps the device in a validation layer to catch API misuse. |

Common dump locations:
- **DirectX**: HLSL output in working directory.
- **Vulkan**: SPIR-V assembly in working directory.
- **CUDA**: `.cu` and PTX in cache directories.
- **Metal**: `.metal` source in cache directories.

## 7. Decision Checklist

| Symptom | First Action | Next Action |
|---|---|---|
| Crash with stack-trace | Read innermost + first project frame | Hypothesize → plan → fix |
| Silent wrong result | Add host-side logging at entry points | Use buffer read-back to inspect values |
| Kernel dispatch hangs | Check `synchronize()` and stream callback | Add minimal device log at start of kernel |
| Backend compilation error | Set `*_DUMP_SOURCE=1` | Inspect generated assembly/source |
| Suspected API/resource misuse | Set `*_ENABLE_VALIDATION=1` | Re-run and read validation messages |
| Test timeout | Read build file for target entry | Narrow phase with host logging |
| Memory error (use-after-free, OOB) | Rebuild with `--policies=build.sanitizer.address` | Read sanitizer report for alloc/access/dealloc trace |
| Data race / deadlock | Rebuild with `--policies=build.sanitizer.thread` | Read sanitizer report for conflicting access sites |
| Undefined behavior | Rebuild with `--policies=build.sanitizer.undefined` | Fix flagged operations (shifts, overflows, misaligned ptrs) |

## 8. Windows Crash Debugging with `scripts/debugger.py`

A lightweight Python debugger using Windows Debug API + DbgHelp.dll to launch an x64 executable, catch second-chance exceptions, and print a symbolic stack trace from PDB symbols.

**Usage:**
```bash
python scripts/debugger.py <path_to_exe> [pdb_search_path] [-- <args>...]
```

- Arguments after `--` are forwarded to the target executable.
- The PDB must be next to the EXE or in `pdb_search_path`.
- Works on **Windows x64** with **Python 3.x** (64-bit recommended).

**Example:**
```bash
python scripts/debugger.py build/bin/test.exe -- --gtest_filter=MyTest
```

## 9. Sanitizer Usage with XMake

XMake provides built-in support for compiler sanitizers (AddressSanitizer, ThreadSanitizer, etc.) to detect memory errors, data races, undefined behavior, and leaks at runtime.

### Via Built-in Mode Rules

Add these rules to your `xmake.lua` at the project or target level:

```lua
add_rules("mode.debug")   -- debug symbols, no optimization
add_rules("mode.asan")    -- AddressSanitizer (use-after-free, buffer overflows)
add_rules("mode.tsan")    -- ThreadSanitizer (data races)
add_rules("mode.lsan")    -- LeakSanitizer (memory leaks)
add_rules("mode.ubsan")   -- UndefinedBehaviorSanitizer (shift overflow, misaligned ptrs)
```

Then configure and run with the corresponding mode:

```bash
xmake f -m asan -c -y
xmake build
xmake run <target>
```

> **Note:** The legacy `mode.asan`/`mode.tsan`/`mode.lsan`/`mode.ubsan` rules produce deprecation warnings. Prefer the newer **policy-based** approach below.

### Via Policies (Recommended)

Policies propagate the sanitizer configuration to dependent packages and avoid deprecation warnings:

```lua
set_policy("build.sanitizer.address", true)
set_policy("build.sanitizer.thread", true)
set_policy("build.sanitizer.memory", true)
set_policy("build.sanitizer.leak", true)
set_policy("build.sanitizer.undefined", true)
```

Or from the command line (combine multiple with commas):

```bash
xmake f --policies=build.sanitizer.address,build.sanitizer.undefined -c -y
xmake build
xmake run <target>
```

### Available Sanitizers

| Policy | Rule (legacy) | Detects |
|---|---|---|
| `build.sanitizer.address` | `mode.asan` | Use-after-free, heap/stack buffer overflows, memory leaks |
| `build.sanitizer.thread` | `mode.tsan` | Data races, deadlocks (POSIX threads) |
| `build.sanitizer.memory` | — | Uninitialized memory reads |
| `build.sanitizer.leak` | `mode.lsan` | Memory leaks (standalone) |
| `build.sanitizer.undefined` | `mode.ubsan` | Integer overflow, shift overflow, misaligned pointers |

### Quick Start for Debugging

Combine sanitizers with debug mode for the best diagnostic output:

```bash
xmake f -m debug --policies=build.sanitizer.address,build.sanitizer.undefined -c -y
xmake build
xmake run <target>
```

When a sanitizer detects an error, it prints a detailed report with a stack trace showing the exact allocation, access, and deallocation sites.

## Summary

- **Stack-traces** → innermost frame = cause; upward walk = call-site.
- **Always plan** before editing; save failed attempts.
- **No trace** → read the build file, add host logging, then device/kernel logging.
- **Bulk value inspection** → prefer buffer write + host read-back over per-thread logging.
- **Backend/codegen issues** → set `*_DUMP_SOURCE=1` to inspect generated shaders and `*_ENABLE_VALIDATION=1` to catch API/resource misuse.
- **Memory/runtime errors** → rebuild with sanitizers (`--policies=build.sanitizer.address,build.sanitizer.undefined`) and read the detailed report.
