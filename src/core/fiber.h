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
 *   kimix::fiber::scheduler sched{4};              // 4 worker threads (RAII)
 *   kimix::fiber::parallel(1000u, [](uint32_t i) noexcept { work(i); });
 *   auto fut = kimix::fiber::async([] { return answer(); });
 *   auto v   = fut.wait();                         // -> the returned value
 *   kimix::fiber::parallel(data.begin(), data.end(), 64,
 *                          [](auto l, auto r) noexcept { process(l, r); });
 *   { kimix_fiber_defer(cleanup()); }              // runs at scope exit
 *
 * RULES
 * - Bind a `scheduler` to the calling thread before any schedule/async/parallel
 *   call: marl only checks that in debug builds, so kimix::fiber::detail::
 *   schedule_task() reports and aborts on every mode instead of dereferencing a
 *   null Scheduler*. The blocking `parallel()` forms degrade to running the work
 *   inline on the calling thread when they fit in a single task.
 * - Every submission wraps the closure in kimix::SharedFunction, so a task may
 *   capture move-only state: marl's Task stores a copy-constructible
 *   std::function and would otherwise reject it. Luisa does that wrapping by
 *   hand at each call site (luisa::SharedFunction inside every parallel() body);
 *   here it is automatic.
 * - Wait with the fiber primitives (event / counter / future / fiber mutex /
 *   fiber condition_variable), never std::mutex or std::this_thread::sleep_for:
 *   parking an OS thread also parks every fiber scheduled on it.
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

/// Worker-thread count of the scheduler bound to this thread; falls back to the
/// hardware concurrency when nothing is bound (so a size hint never reads a null
/// Scheduler*).
inline uint32_t worker_thread_count() noexcept {
    auto *s = marl::Scheduler::get();
    if (s == nullptr) [[unlikely]] { return std::thread::hardware_concurrency(); }
    return static_cast<uint32_t>(s->config().workerThread.count);
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

}// namespace detail

// ---------------------------------------------------------------------------
// Task submission
// ---------------------------------------------------------------------------

/// Run f on the scheduler as soon as a worker picks it up (fire and forget).
template<class F>
    requires(std::is_invocable_v<F>)
void schedule(F &&f) noexcept {
    detail::schedule_task(SharedFunction<void()>{std::forward<F>(f)});
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

// ---------------------------------------------------------------------------
// parallel() / async_parallel() over job ids
// ---------------------------------------------------------------------------

/// Submit [0, job_count) across the workers into a caller-owned counter: each of
/// the tasks pulls `internal_jobs` ids at a time from one shared cursor and
/// calls evt.done() when it drains. Wait with evt.wait().
template<class F>
    requires(std::is_invocable_v<F, uint32_t> || std::is_invocable_v<F, uint32_t, uint32_t>)
void async_parallel(counter &evt, uint32_t job_count, F &&f, uint32_t internal_jobs = 1) noexcept {
    auto const tasks = std::min(detail::claim_count(job_count, internal_jobs), worker_thread_count());
    if (tasks == 0) { return; }
    evt.add(tasks);
    auto body = detail::make_job_body<uint32_t>(std::forward<F>(f));
    auto task = detail::make_batch_task<uint32_t>(job_count, internal_jobs, evt, std::move(body));
    for (uint32_t i = 0; i < tasks; ++i) { detail::schedule_task(task); }
}

/// Run [0, job_count) across the workers. `f` takes a single job id or a
/// half-open (begin, end) range; `internal_jobs` is the number of ids claimed
/// per claim. Blocks until every job has run; a one-task split runs inline.
template<class F>
    requires(std::is_invocable_v<F, uint32_t> || std::is_invocable_v<F, uint32_t, uint32_t>)
void parallel(uint32_t job_count, F &&f, uint32_t internal_jobs = 1) noexcept {
    auto body = detail::make_job_body<uint32_t>(std::forward<F>(f));
    auto const tasks = std::min(detail::claim_count(job_count, internal_jobs), worker_thread_count());
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
[[nodiscard]] auto async_parallel(uint32_t job_count, F &&f, uint32_t internal_jobs = 1) noexcept {
    counter evt{0u};
    kimix::fiber::async_parallel(evt, job_count, std::forward<F>(f), internal_jobs);
    return evt;
}

// ---------------------------------------------------------------------------
// parallel() / async_parallel() over iterator ranges
// ---------------------------------------------------------------------------

/// async_parallel() over [begin, end) in batches of `batch` elements, into a
/// caller-owned counter. `f` gets the half-open iterator range of each batch.
/// The split path advances from `begin` per claim, so it needs random access.
template<class F, class Iter>
    requires(std::is_invocable_v<F, Iter, Iter>)
void async_parallel(counter &evt, Iter begin, Iter end, size_t batch, F &&f) {
    auto const total = detail::distance_iter(begin, end);
    if (total <= 0) { return; }
    auto const n = static_cast<size_t>(total);
    auto const tasks = std::min(detail::claim_count(n, batch), worker_thread_count());
    if (tasks == 0) { return; }
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
[[nodiscard]] auto async_parallel(Iter begin, Iter end, size_t batch, F &&f) {
    counter evt{0u};
    kimix::fiber::async_parallel(evt, begin, end, batch, std::forward<F>(f));
    return evt;
}

/// Visit [begin, end) in batches of `batch` elements. `f` takes one element
/// iterator or a half-open iterator range. At most `inplace_batch_threshold`
/// batches (or anything that fits a single task) run inline on the calling
/// fiber; otherwise the batches are split over the workers and this blocks.
template<class Iter, class F>
    requires(std::is_invocable_v<F, Iter> || std::is_invocable_v<F, Iter, Iter>)
void parallel(Iter begin, Iter end, size_t batch, F &&f, size_t inplace_batch_threshold = 1) {
    auto const total = detail::distance_iter(begin, end);
    if (total <= 0) { return; }
    auto const n = static_cast<size_t>(total);
    auto const batch_count = (n + batch - 1) / batch;
    auto const tasks = std::min(static_cast<uint32_t>(batch_count), worker_thread_count());
    if (batch_count <= inplace_batch_threshold || tasks <= 1) {
        // Sequential drain: advance() only, so input/forward iterators work.
        auto it = begin;
        for (size_t i = 0; i < batch_count; ++i) {
            auto const step = std::min<size_t>(batch, n - i * batch);
            auto const l = it;
            detail::advance_iter(it, step);
            if constexpr (std::is_invocable_v<F, Iter>) {
                for (auto e = l; e != it; ++e) { f(e); }
            } else {
                f(l, it);
            }
        }
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
