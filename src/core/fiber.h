/*
 * fiber.h — kimix::fiber: fiber-scheduled parallel work on top of marl.
 *
 * A marl scheduler owns N worker OS threads; submitted work runs on lightweight
 * FIBERS multiplexed over those threads. Blocking on an event / counter / future
 * inside a fiber yields the fiber and lets the same thread run other work, so
 * "wait for a dependency" costs no OS thread and every worker stays busy.
 * This is the kimix port of <luisa/core/fiber.h>
 * (C:/dev/compute/include/luisa/core/fiber.h), backed by the vendored
 * src/ext/marl submodule (the kimix-marl target).
 *
 *   #include <core/fiber.h>                        // NOT part of kimix_core.h
 *
 * kimix::fiber::scheduler sched{4}; // 4 worker threads (RAII)
 * kimix::fiber::parallel(1000u, [](uint32_t i) noexcept { work(i); });
 * kimix::fiber::parallel(jobs, body, 1u, 8u); // 8u = task_limit (capped fan-out)
 * auto fut = kimix::fiber::async([] { return answer(); });
 * auto v = fut.wait(); // -> the returned value
 * kimix::fiber::parallel(data.begin(), data.end(), 64,
 * [](auto l, auto r) noexcept { process(l, r); });
 * kimix::fiber::sleep_for(50ms); // yields the fiber, unlike std::sleep_for
 * kimix::fiber::blocking_call([] { return foreign_blocking_api(); });
 * kimix::fiber::schedule_background([] { background_work(); }); // never aborts
 * { kimix_fiber_defer(cleanup()); } // runs at scope exit
 *
 * RULES
 * - A submission (schedule/async/parallel split) needs a scheduler bound to the
 * calling thread: the root main() binds the process-wide pool
 * (shared_scheduler().bind()), tests bind their own `scheduler`. marl only
 * checks the binding in debug builds, so kimix::fiber::detail::schedule_task()
 * reports and aborts on every mode instead of dereferencing a null Scheduler*.
 * The blocking `parallel()` forms never abort: on a thread with no scheduler
 * bound (worker_thread_count() == 1) they run the work inline. The one
 * exception is schedule_background(), which binds the shared pool itself.
 * - Fibers are cooperative: a job that parks the OS worker (std::mutex, a
 * blocking syscall, a socket wait) starves the fibers queued behind it on that
 * thread. Make the wait fiber-aware (fiber sleep_for / event / counter / future)
 * or run it through blocking_call(). Concurrency of a blocking fan-out is still
 * capped exactly by parallel()'s task_limit; the fan-out shares the ambient
 * pool, so a body that parks only costs the workers the old std::thread pool
 * would have cost anyway.
 * - Every submission wraps the closure in kimix::SharedFunction, so a task may
 * capture move-only state: marl's Task stores a copy-constructible
 * std::function and would otherwise reject it. Luisa does that wrapping by
 * hand at each call site (luisa::SharedFunction inside every parallel() body);
 * here it is automatic.
 * - fiber primitives are ref-counted HANDLES over shared state — capture them by
 *   value in a task closure, never by reference to a local that may die.
 * - A `scheduler` must outlive the work submitted to it; destroy it only after
 *   every event/counter/future being waited on has been resolved.
 *
 * Related headers: <core/shared_function.h> (ref-counted callable),
 * <core/fiber_future.h> (Future<T>).
 */
#pragma once

// marl's scheduler/event headers transitively pull <Windows.h>; winsock2.h has
// to be there first (the same ordering rule the kimix_core.h umbrella applies),
// otherwise windows.h loads winsock.h and a later <ws2tcpip.h>/<httplib.h>
// collides in a unity batch.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <thread>
#include <type_traits>
#include <utility>

#include <marl/conditionvariable.h>
#include <marl/event.h>
#include <marl/finally.h>
#include <marl/mutex.h>
#include <marl/scheduler.h>
#include <marl/waitgroup.h>

#include "fiber_future.h"
#include "shared_function.h"

// ---------------------------------------------------------------------------
// kimix_fiber_defer(stmt) — run `stmt` when the surrounding SCOPE closes
// (golang-style defer, but scope-bound, not function-bound). Built on
// marl::make_finally(), so it also fires when the scope is left early.
//
//   void say_hello_world() {
//       kimix_fiber_defer(std::printf("world\n"));
//       std::printf("hello ");
//   }
// ---------------------------------------------------------------------------
#define KIMIX_FIBER_CONCAT_(a, b) a##b
#define KIMIX_FIBER_CONCAT(a, b) KIMIX_FIBER_CONCAT_(a, b)
#define kimix_fiber_defer(...) \
    auto KIMIX_FIBER_CONCAT(kimix_fiber_defer_, __LINE__) = marl::make_finally([&]() noexcept { __VA_ARGS__; })

namespace kimix::fiber {

// ---------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------

/// Owns the marl worker-thread pool and binds it to the calling thread.
/// RAII: binds on construction, unbinds on destruction. Neither copyable nor
/// movable — one live scheduler per thread, destroyed after its work drains.
class scheduler {
public:
    using internal_t = marl::Scheduler;

    scheduler() noexcept : _internal{internal_t::Config::allCores()} { _internal.bind(); }
    explicit scheduler(uint32_t thread_count) noexcept
        : _internal{internal_t::Config().setWorkerThreadCount(static_cast<int>(thread_count))} {
        _internal.bind();
    }
    scheduler(scheduler const &) = delete;
    scheduler(scheduler &&) = delete;
    scheduler &operator=(scheduler const &) = delete;
    scheduler &operator=(scheduler &&) = delete;
    ~scheduler() noexcept { _internal.unbind(); }

private:
    internal_t _internal;
};

/// True when the calling thread can submit fiber work, i.e. a scheduler is bound
/// to it. Everything that submits (schedule/async/parallel with a real split)
/// needs this; `parallel()` degrades to running inline without it.
inline bool is_bound() noexcept { return marl::Scheduler::get() != nullptr; }

namespace detail {
/// The process-wide marl pool, built on first use and deliberately never
/// destroyed: kimix runs inside foreign hosts (a Python interpreter, the CLI)
/// whose shutdown order it does not control, and marl's ~Scheduler blocks until
/// every bound thread has unbound AND every in-flight task has drained — a pool
/// torn down mid-flight is a hang at exit, not a clean shutdown.
/// Sized by KIMIX_FIBER_WORKER_THREADS, else one worker per logical core with a
/// floor of 4 so the usual 5-8 wide fan-outs never queue behind the pool.
inline marl::Scheduler *shared_scheduler_pool() noexcept {
    static marl::Scheduler *pool = [] {
        auto config = marl::Scheduler::Config::allCores();
        if (auto const *raw = std::getenv("KIMIX_FIBER_WORKER_THREADS"); raw != nullptr) {
            long const n = std::strtol(raw, nullptr, 10);
            if (n > 0) { config.setWorkerThreadCount(static_cast<int>(n)); }
        } else if (auto const cores = std::thread::hardware_concurrency(); cores > 0 && cores < 4u) {
            config.setWorkerThreadCount(4);
        }
        return kimix::new_with_allocator<marl::Scheduler>(config);
    }();
    return pool;
}

}// namespace detail

/// The process-wide scheduler, created on the first call. Root main functions
/// (src/cli/main.cpp) bind it once for the whole process; the binding is what
/// every schedule()/parallel() on that thread submits to. It is never destroyed
/// (see detail::shared_scheduler_pool), so main never has to unbind.
[[nodiscard]] inline marl::Scheduler &shared_scheduler() noexcept { return *detail::shared_scheduler_pool(); }

// ---------------------------------------------------------------------------
// Synchronization primitives (aliases over marl's fiber-aware versions)
// ---------------------------------------------------------------------------

/// Ref-counted counter: add(n) / done() / wait() until it drains to zero.
using counter = marl::WaitGroup;
using lock = marl::lock;
using condition_variable = marl::ConditionVariable;
using mutex = marl::mutex;
template<typename T>
using future = Future<T>;

/// Manual/auto-reset signal: signal() releases the waiter(s), clear() resets.
struct event {
    using Mode = marl::Event::Mode;

    explicit event(Mode mode = Mode::Manual, bool init_signalled = false) noexcept
        : _evt{mode, init_signalled} {}

    void signal() const noexcept { _evt.signal(); }
    void clear() const noexcept { _evt.clear(); }
    void wait() const noexcept { _evt.wait(); }
    [[nodiscard]] bool test() const noexcept { return _evt.test(); }
    [[nodiscard]] bool is_signalled() const noexcept { return _evt.isSignalled(); }

private:
    marl::Event _evt;
};

/// How many worker threads the calling thread can spread work over: the pool
/// size of the scheduler bound to it, or 1 when nothing is bound — an unbound
/// thread only has itself, which is exactly what makes the blocking parallel()
/// forms fall back to running inline instead of submitting work marl cannot run.
inline uint32_t worker_thread_count() noexcept {
    auto *s = marl::Scheduler::get();
    if (s == nullptr) [[unlikely]] { return 1u; }
    return static_cast<uint32_t>(std::max<int>(s->config().workerThread.count, 0));
}

/// Fiber-friendly wait: park the CALLING FIBER for `d` without parking the OS
/// worker underneath it, so the other fibers on that thread keep running. On a
/// thread with no scheduler bound it behaves exactly like
/// `std::this_thread::sleep_for` (marl's ConditionVariable delegates the timed
/// wait to std::condition_variable), which is what makes it a drop-in for the
/// existing poll / retry-backoff loops.
template<class Rep, class Period>
void sleep_for(std::chrono::duration<Rep, Period> const &d) noexcept {
    if (d.count() <= 0) { return; }
    marl::mutex m;
    marl::ConditionVariable cv;
    marl::lock l{m};
    // A predicate that never holds: the wait ends only when the deadline passes.
    cv.wait_for(l, d, [] { return false; });
}

namespace detail {

/// The single funnel for every submission: "no scheduler bound on this thread"
/// becomes a reported abort instead of a null dereference (marl's own check is
/// compiled out when NDEBUG is set).
inline void schedule_task(SharedFunction<void()> task) noexcept {
    if (marl::Scheduler::get() == nullptr) [[unlikely]] {
        std::fputs("kimix::fiber: submitting work requires a bound kimix::fiber::scheduler on the calling thread; aborting\n", stderr);
        std::fflush(stderr);
        std::abort();
    }
    marl::schedule(std::move(task));
}

/// An atomic that is copy-DELETED but move-allowed (the name mirrors luisa's):
/// a task closure owns exactly one shared cursor — moved in at build time and
/// then shared by every copy of that closure's SharedFunction.
template<typename T>
struct NonMovableAtomic {
    std::atomic<T> value;
    NonMovableAtomic() noexcept = default;
    explicit NonMovableAtomic(T t) noexcept : value{t} {}
    NonMovableAtomic(NonMovableAtomic const &) = delete;
    NonMovableAtomic(NonMovableAtomic &&rhs) noexcept : value{rhs.value.load()} {}
};

/// Types with an std::iterator_traits (pointers included) advance/distance via
/// the STL; index-like types (counters, offsets) use += and -.
template<typename T>
concept iterator_like = requires {
    typename std::iterator_traits<T>::difference_type;
};

template<typename Iter>
void advance_iter(Iter &it, size_t n) {
    if constexpr (iterator_like<Iter>) {
        std::advance(it, static_cast<typename std::iterator_traits<Iter>::difference_type>(n));
    } else {
        it += n;
    }
}

template<typename Iter>
[[nodiscard]] ptrdiff_t distance_iter(Iter first, Iter last) {
    if constexpr (iterator_like<Iter>) {
        return static_cast<ptrdiff_t>(std::distance(first, last));
    } else {
        return last > first ? static_cast<ptrdiff_t>(last - first) : static_cast<ptrdiff_t>(first - last);
    }
}

/// Normalize a "single job id" body into the half-open range form (a range body
/// is passed through untouched).
template<typename Index, typename Fn>
[[nodiscard]] inline auto make_job_body(Fn &&fn) {
    using Func = std::remove_reference_t<Fn>;
    return [f = std::forward<Fn>(fn)](Index begin, Index end) mutable noexcept {
        if constexpr (std::is_invocable_v<Func &, Index>) {
            for (auto i = begin; i < end; ++i) { f(i); }
        } else {
            f(begin, end);
        }
    };
}

/// The work-sharing core of every parallel()/async_parallel(): N copies of ONE
/// task race on the atomic cursor owned by that task, each claim taking `chunk`
/// job ids at a time; every task calls evt.done() once it has drained.
template<typename Index, typename Body>
[[nodiscard]] inline SharedFunction<void()> make_batch_task(Index job_count,
                                                             Index chunk,
                                                             counter const &evt,
                                                             Body &&body) {
    return SharedFunction<void()>{
        [cursor = NonMovableAtomic<Index>(Index{0}), job_count, chunk, evt, body = std::forward<Body>(body)]() mutable noexcept {
            Index i = Index{0};
            while ((i = cursor.value.fetch_add(chunk)) < job_count) {
                body(i, std::min<Index>(i + chunk, job_count));
            }
            evt.done();
        }};
}

/// How many tasks it takes to cover `work_units` ids when every claim takes
/// `chunk` of them (at least one task).
[[nodiscard]] inline uint32_t claim_count(size_t work_units, size_t chunk) noexcept {
    return static_cast<uint32_t>(chunk > 0 ? std::max<size_t>(work_units / chunk, 1u) : 1u);
}

/// The task count of a split: the claims needed, capped by the workers this
/// thread can reach and by the caller's own `task_limit` (0 = no cap).
/// Anything <= 1 means the caller runs the work inline, which is also how an
/// unbound thread (worker_thread_count() == 1) gets there.
[[nodiscard]] inline uint32_t task_count(uint32_t claimed, uint32_t task_limit) noexcept {
    auto const workers = worker_thread_count();
    return std::min(claimed, task_limit > 0u ? std::min(task_limit, workers) : workers);
}

/// The sequential form of the iterator split: walk `n` elements from `begin` in
/// batches of `batch` and call `f` per batch (element iterator or range body).
/// advance() only, so input/forward iterators work here — the split path needs
/// random access.
template<class Iter, class F>
void for_each_batch(Iter begin, size_t n, size_t batch, F &&f) {
    auto const batches = (n + batch - 1) / batch;
    auto it = begin;
    for (size_t i = 0; i < batches; ++i) {
        auto const step = std::min<size_t>(batch, n - i * batch);
        auto const l = it;
        advance_iter(it, step);
        if constexpr (std::is_invocable_v<F, Iter>) {
            for (auto e = l; e != it; ++e) { f(e); }
        } else {
            f(l, it);
        }
    }
}

}// namespace detail

// ---------------------------------------------------------------------------
// Task submission
// ---------------------------------------------------------------------------

/// Run f on the scheduler as soon as a worker picks it up (fire and forget).
/// Aborts with a message when the calling thread has no scheduler bound — the
/// blocking parallel() forms degrade to inline instead, but a fire-and-forget
/// task has no caller to fall back to. Code that must also work on a foreign,
/// unbound thread (a host callback, a tool invoked outside the CLI) uses
/// schedule_background() instead.
template<class F>
    requires(std::is_invocable_v<F>)
void schedule(F &&f) noexcept {
    detail::schedule_task(SharedFunction<void()>{std::forward<F>(f)});
}

/// Fire-and-forget task that ALSO works from a thread with no scheduler bound:
/// the process-wide shared pool is bound for the submission and released right
/// after, while the task itself keeps running on the pool's workers (unbinding
/// the submitter does not touch in-flight work, and the shared pool is never
/// destroyed). When the calling thread already has a scheduler bound the task
/// goes to that ambient pool, exactly like schedule(). This is the submission
/// path for long-lived background work started from arbitrary call sites —
/// background sub-agent runs, interactive task drains — after the project
/// dropped its per-call-site scoped pools in favor of one pool bound at the
/// root main.
template<class F>
    requires(std::is_invocable_v<F>)
void schedule_background(F &&f) noexcept {
    auto wrapper = SharedFunction<void()>{std::forward<F>(f)};
    if (marl::Scheduler::get() != nullptr) [[likely]] {
        marl::schedule(std::move(wrapper));
        return;
    }
    auto &pool = shared_scheduler();
    pool.bind();
    marl::schedule(std::move(wrapper));
    pool.unbind();
}

/// Submit f and return the handle to wait on: an `event` for a void f, a
/// `Future<Ret>` carrying f's return value otherwise.
template<class F>
    requires(std::is_invocable_v<F>)
[[nodiscard]] auto async(F &&f) noexcept {
    using RetType = decltype(f());
    if constexpr (std::is_same_v<RetType, void>) {
        event evt;
        detail::schedule_task(SharedFunction<void()>{[evt, func = std::forward<F>(f)]() mutable noexcept {
            func();
            evt.signal();
        }});
        return evt;
    } else {
        future<RetType> fut;
        detail::schedule_task(SharedFunction<void()>{[fut, func = std::forward<F>(f)]() mutable noexcept {
            fut.signal(func());
        }});
        return fut;
    }
}

/// Run a blocking, non-yielding call on a fresh OS thread and yield the calling
/// fiber until it returns — marl's documented pattern for work that would
/// otherwise park a worker (a subprocess wait, a socket read, a foreign blocking
/// API). The new thread is bound to the caller's scheduler, so `f` may itself
/// submit fiber work. On a thread with no scheduler bound this is a plain call:
/// there is no other fiber to hand the worker to.
/// It is the same thread-per-call cost as calling the blocking function on a
/// `std::thread` and joining it, minus the hand-written join bookkeeping.
template<class F>
[[nodiscard]] inline auto blocking_call(F &&f) noexcept -> decltype(f()) {
    using Ret = decltype(f());
    static_assert(!std::is_reference_v<Ret>,
                  "blocking_call() needs a callable that returns a value (the result is moved out of the worker thread)");
    auto *sched = marl::Scheduler::get();
    if (sched == nullptr) [[unlikely]] { return f(); }
    counter done{1u};
    if constexpr (std::is_void_v<Ret>) {
        std::thread worker{[sched, &done, &f] {
            sched->bind();
            f();
            sched->unbind();
            done.done(); // last action, so the join below returns at once
        }};
        done.wait();
        worker.join();
    } else {
        kimix::optional<std::remove_const_t<Ret>> result;
        std::thread worker{[sched, &result, &done, &f] {
            sched->bind();
            result.emplace(f());
            sched->unbind();
            done.done(); // last action, so the join below returns at once
        }};
        done.wait();
        worker.join();
        return std::move(result).value();
    }
}

// ---------------------------------------------------------------------------
// parallel() / async_parallel() over job ids
// ---------------------------------------------------------------------------

/// Submit [0, job_count) across the workers into a caller-owned counter: each of
/// the tasks pulls `internal_jobs` ids at a time from one shared cursor and
/// calls evt.done() when it drains. Wait with evt.wait(). `task_limit` caps the
/// number of tasks (0 = no cap) — that is the knob a caller uses to keep a
/// concurrency budget, e.g. "never more than N tools running at once".
template<class F>
    requires(std::is_invocable_v<F, uint32_t> || std::is_invocable_v<F, uint32_t, uint32_t>)
void async_parallel(counter &evt, uint32_t job_count, F &&f, uint32_t internal_jobs = 1, uint32_t task_limit = 0) noexcept {
    auto body = detail::make_job_body<uint32_t>(std::forward<F>(f));
    auto const tasks = detail::task_count(detail::claim_count(job_count, internal_jobs), task_limit);
    if (tasks <= 1) {
        // Single task, or no scheduler bound: run the jobs here and resolve the
        // counter exactly once, so a waiter sees the same accounting either way.
        evt.add(1u);
        body(0u, job_count);
        evt.done();
        return;
    }
    evt.add(tasks);
    auto task = detail::make_batch_task<uint32_t>(job_count, internal_jobs, evt, std::move(body));
    for (uint32_t i = 0; i < tasks; ++i) { detail::schedule_task(task); }
}

/// Run [0, job_count) across the workers. `f` takes a single job id or a
/// half-open (begin, end) range; `internal_jobs` is the number of ids claimed
/// per claim; `task_limit` caps the task count (0 = no cap). Blocks until every
/// job has run; a one-task split (and any call on a thread with no scheduler
/// bound) runs inline on the calling thread.
template<class F>
    requires(std::is_invocable_v<F, uint32_t> || std::is_invocable_v<F, uint32_t, uint32_t>)
void parallel(uint32_t job_count, F &&f, uint32_t internal_jobs = 1, uint32_t task_limit = 0) noexcept {
    auto body = detail::make_job_body<uint32_t>(std::forward<F>(f));
    auto const tasks = detail::task_count(detail::claim_count(job_count, internal_jobs), task_limit);
    if (tasks > 1) {
        counter evt{tasks};
        auto task = detail::make_batch_task<uint32_t>(job_count, internal_jobs, evt, std::move(body));
        for (uint32_t i = 0; i < tasks; ++i) { detail::schedule_task(task); }
        evt.wait();
    } else {
        body(0u, job_count);
    }
}

/// The same split, non-blocking: returns the `counter` to wait on later, so the
/// caller keeps working while the jobs run.
template<class F>
    requires(std::is_invocable_v<F, uint32_t>)
[[nodiscard]] auto async_parallel(uint32_t job_count, F &&f, uint32_t internal_jobs = 1, uint32_t task_limit = 0) noexcept {
    counter evt{0u};
    kimix::fiber::async_parallel(evt, job_count, std::forward<F>(f), internal_jobs, task_limit);
    return evt;
}

// ---------------------------------------------------------------------------
// parallel() / async_parallel() over iterator ranges
// ---------------------------------------------------------------------------

/// async_parallel() over [begin, end) in batches of `batch` elements, into a
/// caller-owned counter. `f` gets the half-open iterator range of each batch.
/// The split path advances from `begin` per claim, so it needs random access;
/// the inline fallback only advances, so any forward iterator works there.
template<class F, class Iter>
    requires(std::is_invocable_v<F, Iter, Iter>)
void async_parallel(counter &evt, Iter begin, Iter end, size_t batch, F &&f, uint32_t task_limit = 0) {
    auto const total = detail::distance_iter(begin, end);
    if (total <= 0) { return; }
    auto const n = static_cast<size_t>(total);
    auto const tasks = detail::task_count(detail::claim_count(n, batch), task_limit);
    if (tasks <= 1) {
        evt.add(1u);
        detail::for_each_batch(begin, n, batch, f);
        evt.done();
        return;
    }
    evt.add(tasks);
    auto body = [begin, f = std::forward<F>(f)](size_t lo, size_t hi) mutable noexcept {
        auto l = begin;
        detail::advance_iter(l, lo);
        auto r = l;
        detail::advance_iter(r, hi - lo);
        f(l, r);
    };
    auto task = detail::make_batch_task<size_t>(n, batch, evt, std::move(body));
    for (uint32_t i = 0; i < tasks; ++i) { detail::schedule_task(task); }
}

/// Non-blocking iterator split; returns the `counter` to wait on.
template<class F, class Iter>
    requires(std::is_invocable_v<F, Iter, Iter>)
[[nodiscard]] auto async_parallel(Iter begin, Iter end, size_t batch, F &&f, uint32_t task_limit = 0) {
    counter evt{0u};
    kimix::fiber::async_parallel(evt, begin, end, batch, std::forward<F>(f), task_limit);
    return evt;
}

/// Visit [begin, end) in batches of `batch` elements. `f` takes one element
/// iterator or a half-open iterator range. At most `inplace_batch_threshold`
/// batches (or anything that fits a single task, including every call on a
/// thread with no scheduler bound) run inline on the calling thread; otherwise
/// the batches are split over the workers (capped by `task_limit`, 0 = no cap)
/// and this blocks.
template<class Iter, class F>
    requires(std::is_invocable_v<F, Iter> || std::is_invocable_v<F, Iter, Iter>)
void parallel(Iter begin, Iter end, size_t batch, F &&f, size_t inplace_batch_threshold = 1, uint32_t task_limit = 0) {
    auto const total = detail::distance_iter(begin, end);
    if (total <= 0) { return; }
    auto const n = static_cast<size_t>(total);
    auto const batch_count = (n + batch - 1) / batch;
    auto const tasks = detail::task_count(static_cast<uint32_t>(batch_count), task_limit);
    if (batch_count <= inplace_batch_threshold || tasks <= 1) {
        detail::for_each_batch(begin, n, batch, f);
    } else {
        counter evt{tasks};
        auto body = [begin, f = std::forward<F>(f)](size_t lo, size_t hi) mutable noexcept {
            auto l = begin;
            detail::advance_iter(l, lo);
            auto r = l;
            detail::advance_iter(r, hi - lo);
            if constexpr (std::is_invocable_v<F, Iter>) {
                for (auto it = l; it != r; ++it) { f(it); }
            } else {
                f(l, r);
            }
        };
        auto task = detail::make_batch_task<size_t>(n, batch, evt, std::move(body));
        for (uint32_t i = 0; i < tasks; ++i) { detail::schedule_task(task); }
        evt.wait();
    }
}

}// namespace kimix::fiber
