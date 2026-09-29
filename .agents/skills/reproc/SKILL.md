---
name: reproc
description: Cross-platform C library for running external processes. Use when spawning subprocesses, redirecting stdin/stdout/stderr, waiting, killing, draining output, or polling multiple processes in KimixBase.
---

# reproc

reproc wraps OS process APIs (POSIX/Windows) into a single C API. Vendored (submodule `LuisaGroup/reproc`) at `src/ext/reproc/reproc/`, version **14.2.7** (`src/ext/reproc/CMakeLists.txt:5`) plus the fork commit `f81a8a2` *Fix Windows exit status and Winsock reference counting*. The C++ wrapper is in `src/ext/reproc/reproc++/`.

**Tool code never calls reproc directly.** `src/builtin_tools/process_runner.*` (`kimix::builtin_tools::proc`) is the only layer allowed to spawn processes (AGENTS.md); bash, pwsh, python, job_output, workflow and CLI `/cmd` all route through `proc::run_process` / `proc::start_task`. `process_runner.cpp` is also the only file in `src/` that includes `<reproc/reproc.h>` (`src/builtin_tools/process_runner.cpp:33`). Use this page to read or change that file, not to write new spawn code.

## Build wiring

`src/ext/xmake.lua:99` builds the static target `kimix-reproc`: C sources are selected by platform suffix (`.posix` vs `.windows`, `src/ext/xmake.lua:119-134`), Windows additionally public-links `ws2_32` (the socket pipes, `:135-136`), `reproc++/src/reproc.cpp` is compiled as well (`:141`), and the include dirs `reproc/include` + `reproc++/include` are public (`:114-116`). Static, so `REPROC_EXPORT` expands to nothing (`reproc/export.h`). Dependents get it through `kimix-llm`'s `add_deps` (`src/xmake.lua:172`) — never add a direct third-party dep. reproc++ is built but unused here: `process_runner.cpp` uses the C API only.

## Lifecycle

Every successful `reproc_start` must be paired with `reproc_wait`/`reproc_stop` and `reproc_destroy`:

```c
#include <reproc/reproc.h>

reproc_t *process = reproc_new();
if (!process) { /* ENOMEM */ }

int r = reproc_start(process, argv, (reproc_options){ 0 });
if (r < 0) { /* error */ }

// ... interact ...

r = reproc_wait(process, REPROC_INFINITE);   // or reproc_stop
process = reproc_destroy(process);           // safe idiom
```

`argv` is a NULL-terminated array of UTF-8, NUL-terminated strings: `{"cmd", "arg1", NULL}` (`reproc/reproc.h:340-350`). Note what `(reproc_options){0}` really means: `in`/`out` piped but `err` inherited, and all `stop` actions `REPROC_STOP_NOOP` — so `reproc_destroy` waits for the deadline (or forever) and then terminates (`reproc.h:513-516`). `reproc_pid(process)` gives the child pid (`reproc.h:367`; `REPROC_EINVAL` on error, `reproc.h:357`) — cached by the runner at registration (`process_runner.cpp:827`, `:959`).

One `reproc_t` must never be driven from two threads at the same time (`src/ext/reproc/README.md:238-243`): `process_runner` gives each interactive task a drain thread that owns the handle for its whole life, including the terminate on a stop request, and the requesting thread joins it before `reproc_stop`/`reproc_destroy` (`src/builtin_tools/process_runner.h:86-96`).

## Error Handling

All functions return a negative error code on failure — except that `reproc_read`/`reproc_write` return a byte count, `reproc_poll` a source count, and `reproc_wait`/`reproc_stop`/`reproc_run` return the child's **exit status**, which is not an error code (see *Exit status vs error*).

| Constant | Meaning |
|----------|---------|
| `REPROC_EINVAL` | Invalid argument |
| `REPROC_ETIMEDOUT` | Timeout/deadline expired |
| `REPROC_EPIPE` | Stream closed (normal EOF on read) |
| `REPROC_ENOMEM` | Allocation failed |
| `REPROC_EWOULDBLOCK` | Operation would block (nonblocking mode) |

Convert to message with `reproc_strerror(r)`.

The `REPROC_E*` constants are `extern const int` objects, not macros (`reproc/reproc.h:22-31`), so they cannot appear in `case` labels — compare with `==`. Same file also exports `REPROC_INFINITE`, `REPROC_DEADLINE` (timeout values for `reproc_wait`) and `REPROC_SIGKILL` / `REPROC_SIGTERM` (exit statuses). This version has **no** `reproc_exit_code()`, `reproc_error_*()` or `REPROC_MORE`: the exit status is the return value of `reproc_wait`/`reproc_stop`.

## Common Patterns

### 1. Run-and-wait (`reproc_run` / `reproc_run_ex`)

One-shot helper from `<reproc/run.h>`. It starts the process, optionally drains output, waits, and cleans up.

```c
#include <reproc/run.h>

// Inherit parent streams, no output capture.
int r = reproc_run(argv, (reproc_options){ .deadline = 5000 });
```

`reproc_run` silently forces `redirect.parent = true` unless `discard`/`file`/`path` is set (`reproc/src/run.c:7-15`), `reproc_run_ex` rejects `options.fork` with `REPROC_EINVAL` (`run.c:22-28`), and the return value is the child's exit status from `reproc_stop` — negative is not necessarily an error (`run.c:45`, see below).

### 2. Capture output with `reproc_drain`

Upstream helper from `<reproc/drain.h>`; `process_runner` does **not** use it (it runs its own poll loop into `bash::capture_machine` so the output cap and wait pattern stay in one place). `reproc_drain` calls each sink once up front with an empty buffer and `stream == REPROC_STREAM_IN`, and calls a sink once with `size == 0` when its stream closes (`reproc/include/reproc/drain.h:29-36`).

```c
#include <reproc/drain.h>

reproc_close(process, REPROC_STREAM_IN);

char *output = NULL;
reproc_sink sink = reproc_sink_string(&output);

int r = reproc_drain(process, sink, REPROC_SINK_NULL);
if (r < 0) { /* error */ }

printf("%s", output);
reproc_free(output); // reproc_free returns NULL: output = reproc_free(output)

r = reproc_wait(process, REPROC_INFINITE);
```

Custom sink:

```c
int sink_fn(REPROC_STREAM stream, const uint8_t *buffer,
            size_t size, void *context)
{
    // size == 0 means stream closed
    fwrite(buffer, 1, size, context ? context : stdout);
    return 0;   // return non-zero to abort drain
}
reproc_sink sink = { sink_fn, stdout };
```

### 3. Manual read/write

```c
uint8_t buf[4096];
int r = reproc_read(process, REPROC_STREAM_OUT, buf, sizeof(buf));
if (r == REPROC_EPIPE) { /* EOF */ }
else if (r < 0) { /* error */ }
size_t n = (size_t) r;

// Write to stdin
int w = reproc_write(process, data, size);
reproc_close(process, REPROC_STREAM_IN);  // signal EOF to child
```

`reproc_read` returns the byte count (`REPROC_EPIPE` = closed/EOF, `REPROC_EWOULDBLOCK` when `nonblocking` is set and nothing is ready); `reproc_write` writes only *up to* `size` bytes, so a full payload needs a retry loop — `process_runner` feeds stdin inside the poll loop, retrying on `REPROC_EWOULDBLOCK` (`src/builtin_tools/process_runner.cpp:484-507`). Read with a big buffer: `k_read_buf_size` is 64 KiB (`process_runner.cpp:62`). On POSIX, ignore `SIGPIPE` in the parent or a closed stdin kills it (`README.md` Gotchas, `reproc.h:404-406`).

### 4. Poll multiple processes

Set `reproc_options.nonblocking = true`, then:

```c
reproc_event_source src = { process, REPROC_EVENT_OUT, 0 };
int r = reproc_poll(&src, 1, 1000);
if (r > 0 && (src.events & REPROC_EVENT_OUT)) {
    // reproc_read(...) will not block
}
```

`reproc_poll` returns the number of sources that got events, `0` when the timeout expires, or `REPROC_EPIPE` when no source has a pollable pipe left (`reproc/reproc.h:377-385`). `REPROC_EVENT_DEADLINE` is always in `interests` (`reproc.h:308-310`). `process_runner` polls `REPROC_EVENT_OUT | REPROC_EVENT_ERR | REPROC_EVENT_EXIT` with a 100 ms tick and turns the exit event into `reproc_wait(p, 0)` (`src/builtin_tools/process_runner.cpp:516-525`).

## Useful Options

All go into `reproc_options`:

- `working_directory` — child CWD (`NULL` = inherit).
- `env.behavior` — `REPROC_ENV_EXTEND` (default) or `REPROC_ENV_EMPTY`.
- `env.extra` — NULL-terminated, UTF-8 `KEY=VALUE` array (`reproc.h:182-195`). `process_runner` uses EXTEND plus `env.extra` (`process_runner.cpp:214-216`); secret scrubbing is done by the *caller* before it fills `run_options::extra_env` (`src/builtin_tools/python_tool.cpp:194`).
- `redirect.in/out/err` — `REPROC_REDIRECT_DEFAULT`/`PIPE`/`PARENT`/`DISCARD`/`STDOUT` (stderr only: merge into the child's stdout)/`HANDLE`/`FILE`/`PATH`. Unset defaults are `in`/`out` → `PIPE` but **`err` → `PARENT`** (`reproc.h:204-212`).
- `redirect.parent`/`discard`/`file`/`path` — shortcuts (mutually exclusive with the per-stream fields).
- `deadline` — `int` max lifetime in ms; only `reproc_poll` enforces it, and a blocking `reproc_read`/`reproc_write` can still deadlock unless `nonblocking` is on (`reproc.h:248-263`).
- `input.data`/`input.size` — pre-write stdin: must fit the 64 KiB pipe buffer, closes stdin afterwards and cannot be combined with `redirect.in` (`reproc.h:265-278`). `process_runner` deliberately does not use it (see Windows section).
- `fork` — POSIX only, errors on Windows; `argv` must then be `NULL` (`reproc.h:279-290`).
- `nonblocking` — put the pipes in nonblocking mode so `reproc_read`/`reproc_write` return `REPROC_EWOULDBLOCK` instead of blocking (`reproc.h:291-296`); `process_runner` always sets it (`process_runner.cpp:242`).
- `stop` — stop actions used by `reproc_destroy` (`reproc.h:242-247`).

## Stopping Processes

```c
reproc_stop(process, (reproc_stop_actions) {
  { REPROC_STOP_WAIT, 10000 },       // wait 10s
  { REPROC_STOP_TERMINATE, 5000 },   // then SIGTERM/CTRL-BREAK, wait 5s
  { REPROC_STOP_KILL, 0 } // then SIGKILL/TerminateProcess
});
```

Up to three actions, each followed by a `reproc_wait` for its timeout (`reproc.h:485-531`). With all three set to `REPROC_STOP_NOOP`, `reproc_stop` waits for the deadline and then terminates (`reproc.h:513-516`). A negative return is the child's exit status as often as it is an error — see *Exit status vs error* below.

What this repo actually uses: `pr_base_options` sets `options.stop` to TERMINATE/2000 ms → KILL/0 (`src/builtin_tools/process_runner.cpp:243-247`) and `pr_stop_actions()` (an explicit stop) is TERMINATE/1000 ms → KILL/500 ms (`process_runner.cpp:263-270`); the end of a foreground run prefixes a `REPROC_STOP_WAIT` step (`process_runner.cpp:583-588`). reproc only signals the direct child — there are no job objects, `CREATE_SUSPENDED` or process-tree kills anywhere in `src/` (the "process tree" wording in `process_runner.h:147` overstates what `stop_task` does).

## Exit status vs error (fork-specific API)

This build adds `reproc_wait_status(process, timeout, &status)` (`reproc/reproc.h:466`) and `reproc_stop_status(process, stop, &status)` (`reproc.h:540`), from fork commit `f81a8a2`. On Windows a crashing or fail-fast child exits with an NTSTATUS (e.g. `0xC0000005`), which is negative as `int` and therefore indistinguishable from an error code returned by `reproc_wait`/`reproc_stop` (`reproc.h:436-438`, `451-461`). The `_status` variants return `0` and write the status to the out-parameter. `process_runner` still uses the plain forms (`process_runner.cpp:515`, `:590`), so a negative `reproc_wait` result is treated as "not exited yet" and a crashed child only ends up on the timeout/kill path — use the `_status` entry points for new code there.

## C++ Wrapper

Built (from `reproc++/src/reproc.cpp`) but not used by any `src/` file. Timeout type is `reproc::milliseconds` (`reproc++/include/reproc++/reproc.hpp:39`) and `run` returns **both** the exit status and the error code:

```cpp
#include <reproc++/run.hpp>
#include <array>
#include <string>

reproc::options opts;
opts.deadline = reproc::milliseconds(5000);          // duration, not int
std::array<std::string, 2> args{"git", "status"};   // any container of strings
auto [status, ec] = reproc::run(args, opts);         // pair<int, error_code>
```

`reproc::arguments` is built from a container of strings (`std::vector<std::string>` / `std::array<std::string, N>`) or a NULL-terminated `const char *const *` — a bare brace-list of string literals does not compile (`reproc++/include/reproc++/arguments.hpp:10-27`).

The two-argument overload forces `redirect.parent = true` unless `discard`/`file`/`path` is set (`reproc++/include/reproc++/run.hpp:28-39`). Redirect kinds are the lowercase enumerators `reproc::redirect::pipe` / `parent` / `discard` / `stdout_` / `handle_` / `file_` / `path_` (`reproc.hpp:68-87`).

## How `process_runner` actually uses it (Windows reality)

reproc's Windows "pipes" are loopback TCP sockets, not anonymous pipes (`reproc/src/pipe.windows.c:24-40`, `:134`). MSYS2/Git Bash refuses socket-backed fds — `cat` on piped input answers `cat: -: Invalid argument`, any `>&2` fails with "cannot duplicate fd" — so on Windows `process_runner` keeps child streams out of reproc pipes entirely (`src/builtin_tools/process_runner.cpp:1-30`):

- **stdout** → `REPROC_REDIRECT_PATH` to a temp file (`pr_temp_path("reproc_out")`), tailed back into the capture buffer each tick (`process_runner.cpp:400-406`, `:459-463`); `run_options::stderr_path` overrides the location.
- **stderr** → merged into that same file with `REPROC_REDIRECT_STDOUT` (`process_runner.cpp:226-231`); when stdout is not a file it gets its own `REPROC_REDIRECT_PATH` file.
- **stdin** → a foreground run writes `stdin_input` to a temp file and uses `REPROC_REDIRECT_PATH` (an empty file = immediate EOF, `process_runner.cpp:413-419`); an interactive task gets a real Win32 anonymous pipe whose write end the registry owns (`process_runner.h:93-96`).
- **No pipes ⇒ nothing to poll**: in file mode `reproc_wait(p, 100)` is the 100 ms tick instead of `reproc_poll` (`process_runner.cpp:513-515`, task loop `:755-765`). POSIX keeps real OS pipes and the `reproc_poll` + `reproc_read` loop (`process_runner.cpp:464-480`, `:516-525`).
- **`options.input` is unused**: it must fit the 64 KiB pipe buffer and closes stdin early for a slow child; the loop writes instead.
- **Timeouts come from the loop**, not `options.deadline`: `run_options::timeout_ms` (kill) and `inactivity_timeout_ms` (adopt the live child into the task registry rather than kill it, `process_runner.cpp:575-580`).
- **Output is bounded and sanitized**: chunks go through `bash::capture_machine` (`output_cap_chars`) and `proc::sanitize_utf8`, which maps non-UTF-8 child bytes to U+FFFD so serialization cannot fail (`process_runner.h:188-200`).

## Cleanup Rule

Always use the safe destroy idiom:

```c
process = reproc_destroy(process);  // returns NULL, safe to repeat
```

## Reference

- C headers: `src/ext/reproc/reproc/include/reproc/` (`reproc.h`, `drain.h`, `run.h`, `export.h`)
- C++ headers: `src/ext/reproc/reproc++/include/reproc++/` (`reproc.hpp`, `run.hpp`, `drain.hpp`, `env.hpp`, `arguments.hpp`, `input.hpp`)
- Upstream notes: `src/ext/reproc/README.md` (*Multithreading*, *Gotchas*) and `CHANGELOG.md` (14.2.x); `src/ext/reproc/reproc/examples/` + `src/ext/reproc/reproc++/examples/` are upstream demos, **not** this repo's usage
- Project usage: `src/builtin_tools/process_runner.{h,cpp}`. Tests: `tests/unit/builtin_tools/test_process_runner.cpp` (xmake target `test_builtin_process_runner`, `tests/xmake.lua:471`) plus the live-spawn cases in `tests/unit/agent/test_agent.cpp`
