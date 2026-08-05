/*
 * fiber.h — Fiber-based parallelism on marl (kimix::fiber namespace).
 *
 * RAII scheduler:
 *   kimix::fiber::scheduler  — binds itself to the current thread on
 *   construction and unbinds on destruction. Must be created on the main
 *   thread before any fiber API is used.
 *
 * Queries:
 *   kimix::fiber::worker_thread_count() — number of worker threads.
 *
 * Task submission:
 *   schedule(f)           — run f asynchronously on the scheduler.
 *   async(f)              — run f, returns event (void) or future<T> (value).
 *   async_parallel(cnt,f) — run f(i) for i=0..cnt-1, returns counter.
 *   parallel(cnt,f)       — run f(i) [or f(begin,end)] blocking.
 *   parallel(begin,end,batch,f) — range-based parallel-for.
 *
 * Synchronization:
 *   counter (alias for marl::WaitGroup) — add()/done()/wait().
 *   event   — signal()/wait()/test()/clear()/is_signalled().
 *   future<T> — signal(value)/wait()/test()/clear()/is_signalled().
 *   lock, mutex, condition_variable (marl wrappers).
 *
 * Scope guard:
 *   KIMIX_FIBER_DEFER(statements) — run on scope exit.
 *
 * Example:
 *   kimix::fiber::scheduler sched;
 *   kimix::fiber::parallel(1000, [](uint32_t i) { /* work *\/ });
 */
#pragma once

// Fiber-based parallelism built on top of marl.
//
// Provides an RAII scheduler wrapper, fiber-aware synchronization primitives
// (event/future/counter), and task/parallel-for helpers in the kimix::fiber
// namespace. All blocking waits yield the current fiber instead of the OS
// thread, so worker threads stay busy running other tasks.

#include <algorithm>
#include <atomic>
#include <cstdint>
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

#include "stl/memory.h"
#include "stl/optional.h"

// ---------------------------------------------------------------------------
// KIMIX_FIBER_DEFER — run statements when the surrounding scope exits
// ---------------------------------------------------------------------------

#define KIMIX_FIBER_CONCAT_IMPL_(a, b) a##b
#define KIMIX_FIBER_CONCAT_(a, b) KIMIX_FIBER_CONCAT_IMPL_(a, b)

/// Defer execution of a statement until the surrounding scope is closed,
/// typically used for cleanup logic once a function returns.
///
/// Note: unlike Go's defer, the statement runs when the surrounding *scope*
/// is closed, not necessarily the function.
#define KIMIX_FIBER_DEFER(...) \
    auto KIMIX_FIBER_CONCAT_(_kimix_fiber_defer_, __LINE__) = marl::make_finally([&]() noexcept { __VA_ARGS__; })

namespace kimix::fiber {

namespace detail {

// std::atomic wrapper that is move- (but not copy-) constructible, so it can
// be captured by value in a lambda while still referring to a single counter.
template <typename T>
struct non_movable_atomic {
    std::atomic<T> value;
    non_movable_atomic() noexcept = default;
    explicit non_movable_atomic(T t) noexcept : value{t} {}
    non_movable_atomic(const non_movable_atomic &) = delete;
    non_movable_atomic(non_movable_atomic &&rhs) noexcept : value{rhs.value.load()} {}
};

// True for iterator-like types (including pointers), false for index-like types.
template <typename T>
concept iterator_type = requires { typename std::iterator_traits<T>::iterator_category; };

template <typename Iter>
inline void advance(Iter &ite, size_t n) {
    if constexpr (iterator_type<std::remove_cvref_t<Iter>>) {
        std::advance(ite, n);
    } else {
        ite += n;
    }
}

template <typename Iter>
[[nodiscard]] inline auto distance(Iter begin, Iter end) {
    if constexpr (iterator_type<std::remove_cvref_t<Iter>>) {
        return std::distance(begin, end);
    } else {
        return end > begin ? end - begin : begin - end;
    }
}

// Wrap a (possibly move-only) callable in a copyable lambda sharing ownership
// of the callable, so one logical task can be scheduled on multiple workers.
template <typename F>
[[nodiscard]] inline auto make_shared_callable(F &&f) {
    using func_type = std::remove_cvref_t<F>;
    auto shared = std::allocate_shared<func_type>(allocator<func_type>{}, std::forward<F>(f));
    return [shared = std::move(shared)]() mutable noexcept { (*shared)(); };
}

} // namespace detail

// ---------------------------------------------------------------------------
// Scheduler — RAII wrapper around marl::Scheduler
// ---------------------------------------------------------------------------

/// Fiber scheduler. Binds itself to the current thread on construction and
/// unbinds on destruction. Construct once on the main thread before using
/// any of the schedule/parallel helpers.
class scheduler {

public:
    using internal_type = marl::Scheduler;

    scheduler() noexcept
        : _scheduler{internal_type::Config::allCores()} {
        _scheduler.bind();
    }
    explicit scheduler(uint32_t thread_count) noexcept
        : _scheduler{internal_type::Config().setWorkerThreadCount(static_cast<int>(thread_count))} {
        _scheduler.bind();
    }
    scheduler(const scheduler &) = delete;
    scheduler(scheduler &&) = delete;
    scheduler &operator=(const scheduler &) = delete;
    scheduler &operator=(scheduler &&) = delete;
    ~scheduler() noexcept {
        _scheduler.unbind();
    }

private:
    internal_type _scheduler;
};

// ---------------------------------------------------------------------------
// Synchronization primitives
// ---------------------------------------------------------------------------

using counter = marl::WaitGroup;
using lock = marl::lock;
using condition_variable = marl::ConditionVariable;
using mutex = marl::mutex;

/// Fiber-aware event: wait() blocks the current fiber, not the OS thread.
class event {

public:
    using Mode = marl::Event::Mode;

    explicit event(Mode mode = Mode::Manual, bool init_signalled = false) noexcept
        : _event{mode, init_signalled} {}
    /// Signal the event.
    void signal() const noexcept { _event.signal(); }
    /// Clear the signalled state (manual mode).
    void clear() const noexcept { _event.clear(); }
    /// Wait until the event is signalled.
    void wait() const noexcept { _event.wait(); }
    /// Test without blocking. In auto mode a signalled state is cleared upon return.
    [[nodiscard]] bool test() const noexcept { return _event.test(); }
    /// Like test(), but never clears the signalled state.
    [[nodiscard]] bool is_signalled() const noexcept { return _event.isSignalled(); }

private:
    marl::Event _event;
};

/// Fiber-aware future: wait() blocks the current fiber until signal() is
/// called with a value from another fiber or thread.
template <typename T>
class future {

private:
    struct shared_state {
        mutex _mutex;
        condition_variable _condition;
        optional<T> _value;
    };
    shared_ptr<shared_state> _state;

public:
    future()
        : _state{std::allocate_shared<shared_state>(allocator<shared_state>{})} {}
    future(const future &) = default;
    future(future &&) noexcept = default;
    future &operator=(const future &) = default;
    future &operator=(future &&) noexcept = default;
    ~future() = default;

    /// Construct the value in-place and wake all waiters.
    template <typename... Args>
        requires std::constructible_from<T, Args...>
    void signal(Args &&...args) const {
        lock lock{_state->_mutex};
        _state->_value.reset();
        _state->_value.emplace(std::forward<Args>(args)...);
        _state->_condition.notify_all();
    }
    /// Clear the signalled state.
    void clear() const {
        lock lock{_state->_mutex};
        _state->_value.reset();
    }
    /// Block the current fiber until the value is set, then return it.
    [[nodiscard]] T &wait() const {
        lock lock{_state->_mutex};
        _state->_condition.wait(lock, [&] { return _state->_value.has_value(); });
        return *_state->_value;
    }
    /// Test without blocking.
    [[nodiscard]] bool test() const {
        lock lock{_state->_mutex};
        return _state->_value.has_value();
    }
    /// Alias of test().
    [[nodiscard]] bool is_signalled() const { return test(); }
};

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

/// Number of worker threads of the scheduler bound to the current thread.
[[nodiscard]] inline uint32_t worker_thread_count() {
    return static_cast<uint32_t>(marl::Scheduler::get()->config().workerThread.count);
}

// ---------------------------------------------------------------------------
// Task submission
// ---------------------------------------------------------------------------

/// Schedule a function to run asynchronously on the bound scheduler.
template <typename F>
    requires std::is_invocable_v<F>
inline void schedule(F &&f) noexcept {
    marl::schedule(std::forward<F>(f));
}

/// Run a function asynchronously; returns an event (void result) or a
/// future<T> (non-void result) signalled with the result on completion.
template <typename F>
    requires std::is_invocable_v<F>
[[nodiscard]] inline auto async(F &&lambda) noexcept {
    using return_type = std::invoke_result_t<F>;
    if constexpr (std::is_void_v<return_type>) {
        event evt;
        marl::schedule([evt, lambda = std::forward<F>(lambda)]() mutable noexcept {
            lambda();
            evt.signal();
        });
        return evt;
    } else {
        future<return_type> evt;
        marl::schedule([evt, lambda = std::forward<F>(lambda)]() mutable noexcept {
            evt.signal(lambda());
        });
        return evt;
    }
}

/// Asynchronously run `job_count` jobs, invoking lambda(job_index) for each.
/// Jobs are grouped into batches of `internal_jobs` per fetch. Returns a
/// counter reaching zero when all jobs are done.
template <typename F>
    requires std::is_invocable_v<F, uint32_t>
[[nodiscard]] inline auto async_parallel(uint32_t job_count, F &&lambda, uint32_t internal_jobs = 1) noexcept {
    auto thread_count = std::clamp<uint32_t>(job_count / internal_jobs, 1u, worker_thread_count());
    counter evt{thread_count};
    auto func = detail::make_shared_callable(
        [job_counter = detail::non_movable_atomic<uint32_t>{0u}, job_count, internal_jobs, evt,
         lambda = std::forward<F>(lambda)]() mutable noexcept {
            auto i = 0u;
            while ((i = job_counter.value.fetch_add(internal_jobs)) < job_count) {
                auto end = std::min(i + internal_jobs, job_count);
                for (auto v = i; v < end; ++v) {
                    lambda(v);
                }
            }
            evt.done();
        });
    for (auto i = 0u; i < thread_count; ++i) {
        marl::schedule(func);
    }
    return evt;
}

/// Same as above, but accumulates onto an existing counter instead of
/// returning a new one.
template <typename F>
    requires std::is_invocable_v<F, uint32_t>
inline void async_parallel(counter &evt, uint32_t job_count, F &&lambda, uint32_t internal_jobs = 1) noexcept {
    auto thread_count = std::clamp<uint32_t>(job_count / internal_jobs, 1u, worker_thread_count());
    evt.add(thread_count);
    auto func = detail::make_shared_callable(
        [job_counter = detail::non_movable_atomic<uint32_t>{0u}, job_count, internal_jobs, evt,
         lambda = std::forward<F>(lambda)]() mutable noexcept {
            auto i = 0u;
            while ((i = job_counter.value.fetch_add(internal_jobs)) < job_count) {
                auto end = std::min(i + internal_jobs, job_count);
                for (auto v = i; v < end; ++v) {
                    lambda(v);
                }
            }
            evt.done();
        });
    for (auto i = 0u; i < thread_count; ++i) {
        marl::schedule(func);
    }
}

/// Synchronously run `job_count` jobs in parallel; blocks until all finish.
/// The lambda takes either a single job index — lambda(job_index) — or a
/// job range — lambda(begin_index, end_index).
template <typename F>
    requires(std::is_invocable_v<F, uint32_t> || std::is_invocable_v<F, uint32_t, uint32_t>)
inline void parallel(uint32_t job_count, F &&lambda, uint32_t internal_jobs = 1) noexcept {
    auto thread_count = std::clamp<uint32_t>(job_count / internal_jobs, 1u, worker_thread_count());
    if (thread_count > 1u) {
        counter evt{thread_count};
        auto func = detail::make_shared_callable(
            [job_counter = detail::non_movable_atomic<uint32_t>{0u}, job_count, internal_jobs, evt,
             lambda = std::forward<F>(lambda)]() mutable noexcept {
                auto i = 0u;
                while ((i = job_counter.value.fetch_add(internal_jobs)) < job_count) {
                    auto end = std::min(i + internal_jobs, job_count);
                    if constexpr (std::is_invocable_v<F, uint32_t>) {
                        for (auto v = i; v < end; ++v) {
                            lambda(v);
                        }
                    } else {
                        lambda(i, end);
                    }
                }
                evt.done();
            });
        for (auto i = 0u; i < thread_count; ++i) {
            marl::schedule(func);
        }
        evt.wait();
    } else {
        if constexpr (std::is_invocable_v<F, uint32_t>) {
            for (auto i = 0u; i < job_count; ++i) {
                lambda(i);
            }
        } else {
            lambda(0u, job_count);
        }
    }
}

/// Synchronously invoke f for each batch of `batch` consecutive elements in
/// [begin, end); blocks until all batches finish. f takes either a single
/// element iterator — f(iter) — or a batch range — f(batch_begin, batch_end).
/// Ranges smaller than `inplace_batch_threshold` batches run inline instead.
template <typename Iter, typename F>
    requires(std::is_invocable_v<F, Iter> || std::is_invocable_v<F, Iter, Iter>)
inline void parallel(Iter begin, Iter end, size_t batch, F f, size_t inplace_batch_threshold = 1u) {
    auto n = detail::distance(begin, end);
    auto batch_count = static_cast<size_t>((n + static_cast<decltype(n)>(batch) - 1) / static_cast<decltype(n)>(batch));
    if (batch_count < inplace_batch_threshold) {
        for (auto i = 0u; i < batch_count; ++i) {
            auto to_advance = std::min(static_cast<size_t>(n), batch);
            auto l = begin;
            auto r = begin;
            detail::advance(r, to_advance);
            n -= to_advance;
            if constexpr (std::is_invocable_v<F, Iter>) {
                for (auto b = l; b != r; ++b) {
                    f(b);
                }
            } else {
                f(l, r);
            }
            begin = r;
        }
    } else {
        auto thread_count = std::clamp<size_t>(batch_count, 1u, std::thread::hardware_concurrency());
        counter wg{static_cast<unsigned int>(thread_count)};
        auto shared_func = detail::make_shared_callable(
            [batch_counter = detail::non_movable_atomic<size_t>{0u}, wg, batch_count, batch, n, begin,
             f = std::forward<F>(f)]() mutable {
                auto i = 0u;
                while ((i = batch_counter.value.fetch_add(1u)) < batch_count) {
                    auto begin_idx = i * batch;
                    auto end_idx = std::min<size_t>((i + 1u) * batch, static_cast<size_t>(n));
                    auto batch_begin = begin;
                    auto batch_end = begin;
                    detail::advance(batch_begin, begin_idx);
                    detail::advance(batch_end, end_idx);
                    if constexpr (std::is_invocable_v<F, Iter>) {
                        for (auto b = batch_begin; b != batch_end; ++b) {
                            f(b);
                        }
                    } else {
                        f(batch_begin, batch_end);
                    }
                }
                wg.done();
            });
        for (auto i = 0u; i < thread_count; ++i) {
            marl::schedule(shared_func);
        }
        wg.wait();
    }
}

/// Asynchronous range-based parallel for over batches; f takes a batch range
/// f(batch_begin, batch_end). Returns a counter reaching zero when done.
template <typename F, typename Iter>
    requires std::is_invocable_v<F, Iter, Iter>
[[nodiscard]] inline auto async_parallel(Iter begin, Iter end, size_t batch, F f, size_t inplace_batch_threshold = 1u) {
    auto n = detail::distance(begin, end);
    auto batch_count = static_cast<size_t>((n + static_cast<decltype(n)>(batch) - 1) / static_cast<decltype(n)>(batch));
    auto thread_count = std::clamp<size_t>(batch_count, 1u, std::thread::hardware_concurrency());
    counter wg{static_cast<unsigned int>(thread_count)};
    auto shared_func = detail::make_shared_callable(
        [batch_counter = detail::non_movable_atomic<size_t>{0u}, wg, batch_count, batch, n, begin,
         f = std::forward<F>(f)]() mutable {
            auto i = 0u;
            while ((i = batch_counter.value.fetch_add(1u)) < batch_count) {
                auto begin_idx = i * batch;
                auto end_idx = std::min<size_t>((i + 1u) * batch, static_cast<size_t>(n));
                auto batch_begin = begin;
                auto batch_end = begin;
                detail::advance(batch_begin, begin_idx);
                detail::advance(batch_end, end_idx);
                f(batch_begin, batch_end);
            }
            wg.done();
        });
    for (auto i = 0u; i < thread_count; ++i) {
        marl::schedule(shared_func);
    }
    return wg;
}

/// Same as above, but accumulates onto an existing counter.
template <typename F, typename Iter>
    requires std::is_invocable_v<F, Iter, Iter>
inline void async_parallel(counter &wg, Iter begin, Iter end, size_t batch, F f, size_t inplace_batch_threshold = 1u) {
    auto n = detail::distance(begin, end);
    auto batch_count = static_cast<size_t>((n + static_cast<decltype(n)>(batch) - 1) / static_cast<decltype(n)>(batch));
    auto thread_count = std::clamp<size_t>(batch_count, 1u, std::thread::hardware_concurrency());
    wg.add(static_cast<unsigned int>(thread_count));
    auto shared_func = detail::make_shared_callable(
        [batch_counter = detail::non_movable_atomic<size_t>{0u}, wg, batch_count, batch, n, begin,
         f = std::forward<F>(f)]() mutable {
            auto i = 0u;
            while ((i = batch_counter.value.fetch_add(1u)) < batch_count) {
                auto begin_idx = i * batch;
                auto end_idx = std::min<size_t>((i + 1u) * batch, static_cast<size_t>(n));
                auto batch_begin = begin;
                auto batch_end = begin;
                detail::advance(batch_begin, begin_idx);
                detail::advance(batch_end, end_idx);
                f(batch_begin, batch_end);
            }
            wg.done();
        });
    for (auto i = 0u; i < thread_count; ++i) {
        marl::schedule(shared_func);
    }
}

} // namespace kimix::fiber
