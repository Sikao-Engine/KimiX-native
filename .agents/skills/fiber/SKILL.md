---
name: fiber
description: KimixBase fiber API guide — kimix::fiber over the vendored marl scheduler. Use when writing or editing C++ code that runs work on several threads or waits (scheduler/shared_scheduler, schedule/schedule_background/async, event/counter/future, sleep_for, blocking_call, parallel()/async_parallel() over job ids or iterator ranges with a task_limit cap, kimix_fiber_defer), when replacing a std::thread fan-out with fibers, or when touching src/ext/marl, the kimix-marl target, or src/core/fiber*.h / src/core/shared_function.h.
---

# Fiber (`kimix::fiber`)

Fibers are user-space threads: a marl scheduler owns N worker OS threads and multiplexes submitted work (fibers) over them. Blocking on a fiber primitive yields the fiber so the same OS thread runs other work — that is what makes `wait()` cheap and keeps every worker busy.

KimixBase vendors [marl](https://github.com/LuisaGroup/marl) as the submodule `src/ext/marl` (pin `4ed34cc`, same fork as LuisaCompute) and exposes it through the header-only façade `src/core/fiber.h` — the kimix port of `<luisa/core/fiber.h>`.

## Build wiring

- `src/ext/xmake.lua` builds the static target **`kimix-marl`**: the C++ side is the submodule's own manual-unity TU `src/build.marl.cpp` (`#includes` debug/memory/scheduler/thread/trace.cpp plus `osfiber_windows.cpp` on `_WIN32`). On non-Windows the context switch is hand-written asm, so the `osfiber_<arch>.c` + `osfiber_asm_<arch>.S` pair for `target:arch()` is added — globbing every arch breaks cross-arch builds.
- `MARL_USE_SYSTEM_STL=1` is defined **publicly**: this marl fork defaults to EASTL (`marl/memory.h`, `future.h`, `finally.h`, `src/memory.cpp` all guard `<EASTL/...>` with it) and KimixBase does not vendor EASTL, so marl uses `std::unique_ptr/shared_ptr/function/optional`. Public because the define changes marl's own header declarations.
- `MARL_DLL` is deliberately **not** defined: a static link needs neither `MARL_DLL` nor `MARL_BUILDING_DLL`, so `marl/export.h` expands `MARL_EXPORT`/`MARL_NO_EXPORT` to nothing.
- The dep is declared once, on `kimix-core` (`src/xmake.lua`: `add_deps("mimalloc", "kimix-xxhash", "kimix-yyjson", "kimix-pybind11", "kimix-marl")`). Never `add_deps("kimix-marl")` from another target — third-party deps only ever hang off `kimix-core` (AGENTS.md rule). Objects are archived on demand, so a consumer that never includes `<core/fiber.h>` links none of them.

## Headers

| File | Contents |
|---|---|
| `src/core/fiber.h` | `scheduler`, aliases (`counter`/`lock`/`mutex`/`condition_variable`/`future`), `event`, `worker_thread_count()`, `schedule()`, `async()`, `parallel()`, `async_parallel()`, `kimix_fiber_defer` |
| `src/core/fiber_future.h` | `kimix::fiber::Future<T>` — fiber-blocking value future (`signal/clear/wait/test/is_signalled`) |
| `src/core/shared_function.h` | `kimix::SharedFunction<Sig>` — ref-counted callable that makes a move-only closure copyable |

None of them are part of the `kimix_core.h` umbrella (they drag in `<Windows.h>` + the Win32 Fiber API): include `<core/fiber.h>` directly. `fiber.h` includes `winsock2.h` before the marl headers itself, but keep the project convention of including `<core/kimix_core.h>` first in a TU.

## Scheduler — bind before submitting

## Scheduler — bind before submitting
```cpp
kimix::fiber::scheduler sched; // one worker per logical core
kimix::fiber::scheduler sched{4}; // fixed pool
// RAII: binds to the calling thread on construction, unbinds on destruction.
// A root main instead binds the never-destroyed shared pool once:
kimix::fiber::shared_scheduler().bind();
bool b = kimix::fiber::is_bound(); // does this thread have a scheduler?
```
scheduler is non-copyable, non-movable, one per thread, and it must outlive every wait: declare it before the scopes that submit work (in a test main() that means declaring it first — Boost.UT runs the suites from the _test destructors at the end of main, while the scheduler is still alive). Its destructor unbinds *before* destroying the pool, because marl's `~Scheduler` waits for every bound thread to unbind and for all in-flight tasks to drain.
shared_scheduler() is the lazily created process-wide pool (`KIMIX_FIBER_WORKER_THREADS`, else one worker per logical core with a floor of 4). It is intentionally never destroyed: kimix runs inside hosts whose shutdown order it does not control, and tearing a pool down mid-flight hangs at exit. A **root main** (`src/cli/main.cpp`) binds it once for the whole process; nothing in the libraries binds or owns a pool. The old per-call-site `scoped_scheduler` mechanism was removed in favor of this single binding plus `schedule_background()` (below) for foreign threads.
Submitting work with no bound scheduler aborts with a message (detail::schedule_task() — marl's own check is compiled out under `NDEBUG`); the blocking `parallel()` forms never abort, they run inline (see Rules); `schedule_background()` never aborts either — it binds the shared pool for the submission and releases it right after, while the task keeps running on the pool's workers. That makes it the path for long-lived background work started from arbitrary call sites (the background sub-agent run, the interactive-task drain); with a scheduler already bound it behaves exactly like `schedule()`.

## Synchronization primitives

```cpp
kimix::fiber::event evt{kimix::fiber::event::Mode::Manual, false}; // default: Manual, unsignalled
evt.signal(); evt.clear(); evt.wait(); bool s = evt.test(); bool q = evt.is_signalled();

kimix::fiber::counter cnt{3}; // marl::WaitGroup — a ref-counted HANDLE
cnt.add(2); cnt.done(); cnt.wait();
kimix::fiber::sleep_for(std::chrono::milliseconds{50}); // yields the fiber

kimix::fiber::mutex mtx;           // fiber-aware (std::mutex + fiber bookkeeping)
kimix::fiber::lock l{mtx};
kimix::fiber::condition_variable cv;
cv.wait(l, [&] { return ready; }); cv.notify_all();
```

`event`, `counter`, `Future<T>` are handles over shared state: **copy them into a task closure by value** (`[evt]`, `[cnt]`), never capture a local by reference that may die. Copies share the one counter/event, so `done()`/`signal()` from any copy resolves every waiter.

## Tasks

```cpp
kimix::fiber::schedule([x]() noexcept { work(x); });                 // fire and forget
kimix::fiber::event  e  = kimix::fiber::async([]() noexcept { work(); });
kimix::fiber::Future<int> f = kimix::fiber::async([]() noexcept { return 42; });
int v = f.wait(); // yields the fiber until signalled
// A foreign blocking call must not park a worker: run it on its own bound thread.
auto r = kimix::fiber::blocking_call([] { return httplib_blocking_request(); });
```
The closure must be invocable with no arguments; move-only captures are fine — `schedule`/`async` wrap the closure in `kimix::SharedFunction` because marl's `Task` stores a copy-constructible `std::function` (luisa does the same wrapping by hand inside every `parallel()` body; here it is automatic).

`sleep_for` is the fiber-aware pause: it parks the *fiber* on marl's `ConditionVariable` timed wait so the worker runs other fibers, and on a thread with no scheduler bound it is exactly `std::this_thread::sleep_for` — which is why poll/backoff loops in `process_runner`, `python_code_session`, `step_retry` and the LLM providers use it instead of the std call.
`blocking_call(f)` runs `f` on a fresh OS thread bound to the caller's scheduler and yields the calling fiber until it returns (marl's documented pattern for a wait that would otherwise park a worker: a socket read, a subprocess, any foreign blocking API). It spawns+joins one thread per call, so it costs what the `std::thread` it replaces cost; the thread is bound, so `f` may itself submit fiber work. Unbound caller ⇒ plain `f()` (nothing to yield to).

## Parallel for

Over job ids — body takes one id **or** a half-open `(begin, end)` range:

```cpp
kimix::fiber::parallel(1000u, [](uint32_t i) noexcept { work(i); });
kimix::fiber::parallel(1000u, [](uint32_t b, uint32_t e) noexcept { work(b, e); });
kimix::fiber::parallel(1000u, f, /*internal_jobs=*/16); // claim 16 ids per round
kimix::fiber::parallel(jobs, f, 1u, /*task_limit=*/8u); // at most 8 tasks at once
```

Over an iterator range — `batch` elements per claim, optional inline threshold:

```cpp
kimix::fiber::parallel(v.begin(), v.end(), 64, [](auto l, auto r) noexcept { work(l, r); });
kimix::fiber::parallel(v.begin(), v.end(), 32, [](auto it) noexcept { use(*it); });
kimix::fiber::parallel(v.begin(), v.end(), 16, body, /*inplace_batch_threshold=*/100);
```

Non-blocking variants return the `counter` to wait on later, or write into a caller-owned counter:

```cpp
auto cnt = kimix::fiber::async_parallel(1000u, f, /*internal_jobs=*/8);   // job ids
cnt.wait();
kimix::fiber::counter ext{0u};                                            // caller-owned
kimix::fiber::async_parallel(ext, 1000u, f);                              // ... ext.wait() later
auto c2 = kimix::fiber::async_parallel(v.begin(), v.end(), 64, range_body);
c2.wait();
```

`uint32_t` job-id form: `async_parallel(job_count, f, internal_jobs)` requires a body taking one id; the `(begin, end)` range body is only accepted by `parallel()` and by the counter-taking `async_parallel()`. Iterator form: the split path walks the range with `begin` + advance per claim, so it needs random access (the inline path only ever calls `advance`, so input/forward iterators work there).

How the split works: `min(ceil(work/chunk), worker_thread_count())` identical tasks race on one atomic cursor owned by the task closure, so an idle worker keeps claiming work instead of waiting on a static partition. Everything is submitted once and blocks only in `counter::wait()`.

## Defer

```cpp
{
    kimix_fiber_defer(release());   // runs when this SCOPE closes (marl::make_finally)
    use();
}
```

## Rules and gotchas

- **Never** block a fiber with `std::mutex`, `std::condition_variable::wait_for`, `std::this_thread::sleep_for`, or a blocking syscall that outlives the wait: the OS thread is parked and the fibers scheduled behind it on that thread starve. Use the fiber primitives above (`sleep_for`, `event`/`counter`/`Future`, fiber `mutex`/`condition_variable`) or `blocking_call()`.
- A job body is invoked concurrently from several workers: write to disjoint elements, or protect shared state yourself (`std::atomic` for counters is fine — it never yields the fiber).
- `parallel()`/`parallel(begin, end, ...)` run **inline** when the split collapses to one task, when the batch count is at/below `inplace_batch_threshold`, **or when the calling thread has no scheduler bound** (`worker_thread_count()` is 1 then). So the blocking forms are safe on a foreign thread: correct, just serial. `schedule()`/`async()` cannot degrade that way and abort with a message instead.
- `worker_thread_count()` is the pool the calling thread can spread work over: the bound scheduler's worker count, else 1. Use it to size batches — never `std::thread::hardware_concurrency()`, which over-promises parallelism to a thread that has no workers.
- marl asserts (`MARL_ASSERT`, e.g. "counter done() below zero", "no bound scheduler") are live in `-m debug`/`-m releasedbg` and compiled out in `-m release` — verify fiber code in debug as well as release.
- Third-party rule: `src/ext/marl` is vendored — do not edit it. If marl lacks something, add it to `src/core/fiber.h`.

## Fan-out recipe (replacing a std::thread pool)
The repo's hand-rolled "atomic cursor + vector<std::thread> + join" fan-outs are `parallel()` over the ambient pool with `task_limit` set to exactly their own width:

```cpp
// width = the configured concurrency cap, jobs = the work items
// (no pool object: the root main's shared pool carries the fan-out)
kimix::fiber::parallel(jobs, [&](uint32_t id) noexcept { run(id); },
                       /*internal_jobs=*/1u, /*task_limit=*/width);
// serial fast path stays explicit, so the default config pays nothing:
if (width <= 1) { for (uint32_t id = 0; id < jobs; ++id) run(id); }
```
Why `task_limit` instead of a private pool: the bodies block (subprocess pipes, HTTP), so an in-flight job parks an ambient worker — the exact behavior of the `std::thread` pool this replaced, with the same effective thread count. `task_limit` is what preserves the configured concurrency (`dispatch_concurrency`, workflow `max_concurrency`, the grep chunk count) without a second pool competing with the shared one. The former per-scope private pools (`scoped_scheduler{width}`) were removed: one pool bound at the root main covers every fan-out.
Converted sites (all keep their deterministic merge/join order): `src/builtin_tools/grep_engine.cpp` (file chunks), `src/agent/soul.cpp` `dispatch_tool_calls_parallel` Phase B (tool calls of one step), `src/builtin_tools/workflow_tool.cpp` `run_parallel_sample` (best-of-n samples) and `run_swarm` (swarm tasks). Poll/backoff waits that may run on a fiber go through `kimix::fiber::sleep_for`: `process_runner.cpp`, `python_code_session.cpp`, `workflow_tool.cpp`, `agent/step_retry.cpp`, and the four LLM providers' rate-limit backoff.
Background fibers (fire-and-forget on the shared pool via `schedule_background()`, with a `kimix::fiber::event` as the join point — signalled once as the task's very last action, waited on in place of a thread join): the background sub-agent runner (`agent_tool.cpp` — `agent_run::done`; the run parks an ambient worker during its minutes-long LLM HTTP waits, exactly the way the dedicated `std::thread` it replaced was parked) and the interactive-task drain (`process_runner.cpp` — owns the `reproc_t` for the child's whole life, waking on a 100 ms `reproc_poll` tick).
Deliberately still `std::thread` (each carries a comment saying why): the REPL stdin reader (`cli_repl.cpp` — `fgets` blocks until a human types, so a fiber version would park a pool worker for human-scale times with zero yield benefit) and the print-stream consumer (`runtime/print/print_stream.*` — waits on a std condition variable for the process lifetime).

## Test

`tests/unit/core/test_fiber.cpp` → target `test_fiber` (registered with `test_proj("test_fiber", "unit/core/test_fiber.cpp")` in `tests/xmake.lua`). It covers scheduler binding, `schedule`/`async`, `schedule_background` (shared-pool submission from an unbound thread + ambient-pool routing when bound), event/counter/mutex+cv round trips, every `parallel`/`async_parallel` overload, `kimix_fiber_defer`, move-only captures, `SharedFunction` ref-counting, the `task_limit` cap, the inline fallback on an unbound thread, `sleep_for` yielding and `blocking_call`, and the multi-threading proof:

- a long `parallel()` over 512 slow jobs asserts the jobs landed on ≥2 distinct OS threads (`std::hash<std::thread::id>` stamps, sort + unique);
- `parallel(worker_thread_count(), ...)` with one job per task asserts a peak of ≥2 jobs simultaneously in flight (busy-spin, never `sleep_for`, so a parked thread cannot fake the result).

```bash
xmake f --kimix_enable_tests=true        # tests are file-level skipped when off
xmake build test_fiber && xmake run test_fiber
xmake f -m debug && xmake build -r test_fiber && xmake run test_fiber   # marl asserts active
```

Assertions run on the main thread only: ut's reporters are not thread-safe, so worker tasks just write plain data.
