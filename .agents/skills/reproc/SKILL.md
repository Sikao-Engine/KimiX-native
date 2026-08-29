---
name: reproc
description: Cross-platform C library for running external processes. Use when spawning subprocesses, redirecting stdin/stdout/stderr, waiting, killing, draining output, or polling multiple processes in KimixBase.
---

# reproc

reproc wraps OS process APIs (POSIX/Windows) into a single C API. It lives at `src/ext/reproc/reproc/`. The C++ wrapper is in `src/ext/reproc/reproc++/`.

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

`argv` is a NULL-terminated array: `{"cmd", "arg1", NULL}`.

## Error Handling

All functions return a negative error code on failure:

| Constant | Meaning |
|----------|---------|
| `REPROC_EINVAL` | Invalid argument |
| `REPROC_ETIMEDOUT` | Timeout/deadline expired |
| `REPROC_EPIPE` | Stream closed (normal EOF on read) |
| `REPROC_ENOMEM` | Allocation failed |
| `REPROC_EWOULDBLOCK` | Operation would block (nonblocking mode) |

Convert to message with `reproc_strerror(r)`.

## Common Patterns

### 1. Run-and-wait (`reproc_run` / `reproc_run_ex`)

One-shot helper from `<reproc/run.h>`. It starts the process, optionally drains output, waits, and cleans up.

```c
#include <reproc/run.h>

// Inherit parent streams, no output capture.
int r = reproc_run(argv, (reproc_options){ .deadline = 5000 });
```

### 2. Capture output with `reproc_drain`

```c
#include <reproc/drain.h>

reproc_close(process, REPROC_STREAM_IN);

char *output = NULL;
reproc_sink sink = reproc_sink_string(&output);

int r = reproc_drain(process, sink, REPROC_SINK_NULL);
if (r < 0) { /* error */ }

printf("%s", output);
reproc_free(output);

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

### 4. Poll multiple processes

Set `reproc_options.nonblocking = true`, then:

```c
reproc_event_source src = { process, REPROC_EVENT_OUT, 0 };
int r = reproc_poll(&src, 1, 1000);
if (r > 0 && (src.events & REPROC_EVENT_OUT)) {
    // reproc_read(...) will not block
}
```

## Useful Options

All go into `reproc_options`:

- `working_directory` — child CWD.
- `env.behavior` — `REPROC_ENV_EXTEND` (default) or `REPROC_ENV_EMPTY`.
- `env.extra` — NULL-terminated `KEY=VALUE` array.
- `redirect.in/out/err` — `REPROC_REDIRECT_PIPE`/`PARENT`/`DISCARD`/`HANDLE`/`FILE`/`PATH`.
- `redirect.parent`/`discard`/`file`/`path` — shortcuts.
- `deadline` — max lifetime in ms (enforced by `reproc_poll`).
- `input.data`/`input.size` — pre-write data to stdin.
- `nonblocking` — enable nonblocking I/O for `reproc_poll`.
- `stop` — default stop actions used by `reproc_destroy`.

## Stopping Processes

```c
reproc_stop(process, (reproc_stop_actions) {
  { REPROC_STOP_WAIT, 10000 },       // wait 10s
  { REPROC_STOP_TERMINATE, 5000 },   // then SIGTERM/CTRL-BREAK, wait 5s
  { REPROC_STOP_KILL, 0 }            // then SIGKILL/TerminateProcess
});
```

## C++ Wrapper

```cpp
#include <reproc++/run.hpp>
reproc::options opts;
opts.deadline = reproc::milliseconds(5000);
std::error_code ec = reproc::run({ "git", "status" }, opts);
```

## Cleanup Rule

Always use the safe destroy idiom:

```c
process = reproc_destroy(process);  // returns NULL, safe to repeat
```

## Reference

- C headers: `src/ext/reproc/reproc/include/reproc/`
- Examples: `src/ext/reproc/reproc/examples/`
- C++ headers: `src/ext/reproc/reproc++/include/reproc++/`
