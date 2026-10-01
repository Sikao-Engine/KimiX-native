---
name: fiber
description: KimixBase fiber API guide — kimix::fiber over the vendored marl scheduler. Use when writing or editing C++ code that runs work on several threads (scheduler, schedule/async, event/counter/future, parallel()/async_parallel() over job ids or iterator ranges, kimix_fiber_defer), or when touching src/ext/marl, the kimix-marl target, or src/core/fiber*.h / src/core/shared_function.h.
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

```cpp
kimix::fiber::scheduler sched;      // one worker per logical core
kimix::fiber::scheduler sched{4};   // fixed pool
// RAII: binds to the calling thread on construction, unbinds on destruction.
```

Non-copyable, non-movable, one per thread, and it must outlive every wait: declare it before the scopes that submit work (in a test `main()` that means declaring it first — Boost.UT runs the suites from the `_test` destructors at the end of `main`, while the scheduler is still alive). Submitting work with no bound scheduler aborts with a message (`detail::schedule_task()` — marl's own check is compiled out under `NDEBUG`).

## Synchronization primitives

```cpp
kimix::fiber::event evt{kimix::fiber::event::Mode::Manual, false}; // default: Manual, unsignalled
evt.signal(); evt.clear(); evt.wait(); bool s = evt.test(); bool q = evt.is_signalled();

kimix::fiber::counter cnt{3};      // marl::WaitGroup — a ref-counted HANDLE
cnt.add(2); cnt.done(); cnt.wait();

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
int v = f.wait();                                                    // yields the fiber until signalled
```

The closure must be invocable with no arguments; move-only captures are fine — `schedule`/`async` wrap the closure in `kimix::SharedFunction` because marl's `Task` stores a copy-constructible `std::function` (luisa does the same wrapping by hand inside every `parallel()` body; here it is automatic).

## Parallel for

Over job ids — body takes one id **or** a half-open `(begin, end)` range:

```cpp
kimix::fiber::parallel(1000u, [](uint32_t i) noexcept { work(i); });
kimix::fiber::parallel(1000u, [](uint32_t b, uint32_t e) noexcept { work(b, e); });
kimix::fiber::parallel(1000u, f, /*internal_jobs=*/16);   // claim 16 ids per round
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

- **Never** block a fiber with `std::mutex`, `std::condition_variable::wait_for`, `std::this_thread::sleep_for`, or a blocking syscall that outlives the wait: the OS thread is parked and the fibers scheduled behind it on that thread starve. Use the fiber primitives above.
- A job body is invoked concurrently from several workers: write to disjoint elements, or protect shared state yourself (`std::atomic` for counters is fine — it never yields the fiber).
- `parallel()`/`parallel(begin, end, ...)` run **inline** when the work fits one task (or the batch count is at/below `inplace_batch_threshold`), so those calls need no bound scheduler at all — useful for code that may run on a foreign thread.
- `worker_thread_count()` reports the bound scheduler's pool (falls back to `std::thread::hardware_concurrency()` when nothing is bound); use it to size batches.
- marl asserts (`MARL_ASSERT`, e.g. "counter done() below zero", "no bound scheduler") are live in `-m debug`/`-m releasedbg` and compiled out in `-m release` — verify fiber code in debug as well as release.
- Third-party rule: `src/ext/marl` is vendored — do not edit it. If marl lacks something, add it to `src/core/fiber.h`.

## Test

`tests/unit/core/test_fiber.cpp` → target `test_fiber` (registered with `test_proj("test_fiber", "unit/core/test_fiber.cpp")` in `tests/xmake.lua`). It covers scheduler binding, `schedule`/`async`, event/counter/mutex+cv round trips, every `parallel`/`async_parallel` overload, `kimix_fiber_defer`, move-only captures, `SharedFunction` ref-counting, and the multi-threading proof:

- a long `parallel()` over 512 slow jobs asserts the jobs landed on ≥2 distinct OS threads (`std::hash<std::thread::id>` stamps, sort + unique);
- `parallel(worker_thread_count(), ...)` with one job per task asserts a peak of ≥2 jobs simultaneously in flight (busy-spin, never `sleep_for`, so a parked thread cannot fake the result).

```bash
xmake f --kimix_enable_tests=true        # tests are file-level skipped when off
xmake build test_fiber && xmake run test_fiber
xmake f -m debug && xmake build -r test_fiber && xmake run test_fiber   # marl asserts active
```

Assertions run on the main thread only: ut's reporters are not thread-safe, so worker tasks just write plain data.
