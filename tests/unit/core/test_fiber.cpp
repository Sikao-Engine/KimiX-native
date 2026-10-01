// Test for fiber.h (kimix::fiber), fiber_future.h and shared_function.h.
// This test covers:
// - scheduler binding + worker_thread_count()
// - schedule()/async() (event and Future<T> results), move-only captures
// - event / counter / fiber mutex + condition_variable round trips
// - parallel() over job ids (single-id and range bodies, chunked claims)
// - parallel() over iterator ranges (element and range bodies, inline path)
// - async_parallel() (returned counter, caller-owned counter, iterator form)
// - kimix_fiber_defer scope-exit execution
// - MULTI-THREADING proof: chunked jobs are simultaneously in flight on more
//   than one worker OS thread, and a long parallel() spread over several
//   distinct threads.
// Boost.UT runs the registered suites from the _test destructors at the end of
// main(), so the scheduler declared first here still outlives every suite.
// All assertions run on the calling (main) thread: ut's reporters are not
// thread-safe, so worker tasks only write plain data.

#include "ut/ut.hpp"

#include <core/kimix_core.h> // umbrella first: it fixes the winsock2.h order
#include <core/fiber.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <numeric>
#include <thread>
#include <utility>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

/// A stamp identifying the OS thread that ran the current fiber.
uint64_t thread_stamp() noexcept {
    return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

/// Number of distinct stamps (sort + unique; the count is tiny).
size_t distinct_count(kimix::vector<uint64_t> values) {
    std::sort(values.begin(), values.end());
    return static_cast<size_t>(std::unique(values.begin(), values.end()) - values.begin());
}

/// Hold the current thread busy for about `ms` milliseconds. Deliberately a
/// spin, never a sleep: a sleep would park the OS thread instead of proving
/// that two fibers run on two different threads at the same time.
void busy_wait_ms(double ms) noexcept {
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::duration<double, std::milli>(ms);
    volatile uint64_t sink = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        for (int i = 0; i < 1000; ++i) { ++sink; }
    }
}

/// True when the pool can actually run two fibers at once.
bool pool_is_parallel() noexcept { return kimix::fiber::worker_thread_count() > 1u; }

}// namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(argc, const_cast<const char **>(argv));

    kimix::fiber::scheduler sched{4u};

    "scheduler_binds_worker_threads"_test = [] {
        // scheduler{4} -> exactly four marl worker threads.
        expect(eq(kimix::fiber::worker_thread_count(), 4u)) << "worker_thread_count() must report the configured pool";
    };

    "schedule_runs_every_task"_test = [] {
        constexpr uint32_t k_tasks = 200u;
        std::atomic<uint32_t> ran{0u};
        kimix::fiber::counter done{k_tasks};
        for (uint32_t i = 0; i < k_tasks; ++i) {
            kimix::fiber::schedule([&ran, done]() noexcept {
                ran.fetch_add(1u, std::memory_order_relaxed);
                done.done();
            });
        }
        done.wait();
        expect(eq(ran.load(), k_tasks)) << "every scheduled task must run exactly once";
    };

    "async_void_returns_event"_test = [] {
        std::atomic<uint32_t> value{0u};
        auto evt = kimix::fiber::async([&value]() noexcept { value.store(7u); });
        evt.wait();
        expect(evt.is_signalled());
        expect(eq(value.load(), 7u)) << "async() over a void body delivers an event handle";
        evt.clear();
        expect(!evt.is_signalled()) << "clear() resets a manual-reset event";
    };

    "async_value_returns_future"_test = [] {
        auto int_future = kimix::fiber::async([]() noexcept { return 42; });
        expect(eq(int_future.wait(), 42)) << "async() must carry the return value";
        // A non-trivial T: the value lives in the shared state.
        auto str_future = kimix::fiber::async([]() noexcept { return kimix::string{"fiber"}; });
        expect(eq(str_future.wait(), kimix::string{"fiber"}));
        expect(str_future.test()) << "a signalled Future reports through test()";
    };

    "future_signal_wait_clear"_test = [] {
        kimix::fiber::Future<uint32_t> fut{};
        expect(!fut.test()) << "unsignalled future";
        std::atomic<uint32_t> answer{4u};
        kimix::fiber::schedule([fut, &answer]() noexcept { fut.signal(answer.load()); });
        auto const v = fut.wait();
        expect(eq(v, 4u));
        fut.clear();
        expect(!fut.test()) << "clear() drops the stored value";
        fut.signal(9u);
        auto second = fut;
        expect(eq(second.wait(), 9u)) << "copies of a Future share the signalled value";
    };

    "event_signalled_from_another_fiber"_test = [] {
        kimix::fiber::event evt{kimix::fiber::event::Mode::Manual, false};
        expect(!evt.is_signalled());
        kimix::fiber::schedule([evt]() noexcept { evt.signal(); });
        evt.wait();
        expect(evt.is_signalled());
    };

    "fiber_mutex_and_condition_variable"_test = [] {
        kimix::fiber::mutex m;
        kimix::fiber::condition_variable cv;
        bool ready = false;
        long observed = 0;
        kimix::fiber::schedule([&]() noexcept {
            kimix::fiber::lock l{m};
            ready = true;
            cv.notify_all();
        });
        {
            kimix::fiber::lock l{m};
            cv.wait(l, [&] { return ready; }); // yields this fiber, runs the producer
            observed = ready ? 1 : 0;
        }
        expect(eq(observed, 1)) << "the waiting fiber must resume after notify_all()";
    };

    "counter_add_done_wait"_test = [] {
        kimix::fiber::counter evt{1u};
        evt.add(3u);
        std::atomic<uint32_t> ran{0u};
        for (uint32_t i = 0; i < 4u; ++i) {
            kimix::fiber::schedule([&ran, evt]() noexcept {
                ran.fetch_add(1u, std::memory_order_relaxed);
                evt.done();
            });
        }
        evt.wait();
        expect(eq(ran.load(), 4u)) << "a counter drains when every handle copy calls done()";
    };

    "parallel_job_ids_spread_over_threads"_test = [] {
        // Jobs are deliberately slow (~0.2 ms each) so that the tasks running
        // them overlap: a single task cannot drain the cursor before the others
        // start, which is what makes the thread spread observable.
        constexpr uint32_t k_jobs = 512u;
        kimix::vector<uint8_t> hit(k_jobs, 0u);
        kimix::vector<uint64_t> stamps(k_jobs, 0u);
        kimix::fiber::parallel(k_jobs, [&](uint32_t i) noexcept {
            hit[i] = 1u;
            stamps[i] = thread_stamp();
            busy_wait_ms(0.2);
        });
        size_t ones = 0;
        for (auto b : hit) ones += b;
        expect(eq(ones, static_cast<size_t>(k_jobs))) << "every job id must run exactly once";
        if (pool_is_parallel()) {
            auto const threads = distinct_count(stamps);
            std::printf("[fiber] parallel(%u jobs) ran on %zu distinct OS threads\n", k_jobs, threads);
            expect(threads >= 2u) << "parallel() must spread work over worker threads, saw " << threads;
        }
    };

    "parallel_range_body_sums_jobs"_test = [] {
        constexpr uint32_t k_jobs = 1000u;
        std::atomic<uint64_t> sum{0u};
        kimix::fiber::parallel(k_jobs, [&sum](uint32_t begin, uint32_t end) noexcept {
            uint64_t local = 0u;
            for (auto i = begin; i < end; ++i) local += i;
            sum.fetch_add(local, std::memory_order_relaxed);
        });
        uint64_t expected = 0u;
        for (uint32_t i = 0; i < k_jobs; ++i) expected += i;
        expect(eq(sum.load(), expected)) << "the range body must cover [0, job_count) exactly once";
    };

    "parallel_chunked_claims"_test = [] {
        constexpr uint32_t k_jobs = 1000u;
        constexpr uint32_t k_chunk = 16u;
        kimix::vector<uint8_t> hit(k_jobs, 0u);
        kimix::fiber::parallel(k_jobs, [&](uint32_t i) noexcept { hit[i] = 1u; }, k_chunk);
        size_t ones = 0;
        for (auto b : hit) ones += b;
        expect(eq(ones, static_cast<size_t>(k_jobs))) << "internal_jobs chunking must neither skip nor repeat ids";
    };

    "parallel_jobs_run_simultaneously"_test = [] {
        // One chunk per worker: each task claims a single job, so a peak of more
        // than one entered job can only happen on different OS threads.
        if (!pool_is_parallel()) return;
        std::atomic<uint32_t> active{0u};
        std::atomic<uint32_t> peak{0u};
        auto const tasks = kimix::fiber::worker_thread_count();
        kimix::fiber::parallel(tasks, [&](uint32_t) noexcept {
            auto const now = active.fetch_add(1u, std::memory_order_acq_rel) + 1u;
            auto best = peak.load(std::memory_order_relaxed);
            while (now > best && !peak.compare_exchange_weak(best, now, std::memory_order_acq_rel)) {}
            busy_wait_ms(2.0);
            active.fetch_sub(1u, std::memory_order_acq_rel);
        });
        std::printf("[fiber] parallel(%u tasks) peak concurrency = %u jobs in flight\n", tasks, peak.load());
        expect(peak.load() >= 2u) << "expected jobs in flight on several workers, peak was " << peak.load();
    };

    "parallel_iterator_range_body"_test = [] {
        kimix::vector<int> v(4096, 1);
        kimix::fiber::parallel(v.begin(), v.end(), 64, [](auto l, auto r) noexcept {
            for (auto it = l; it != r; ++it) { *it += 2; }
        });
        auto const sum = std::accumulate(v.begin(), v.end(), 0);
        expect(eq(sum, 4096 * 3)) << "the iterator range form must visit each element once";
    };

    "parallel_iterator_element_body"_test = [] {
        kimix::vector<int> v(2000, 3);
        kimix::fiber::parallel(v.begin(), v.end(), 32, [](auto it) noexcept { *it *= 2; });
        auto const sum = std::accumulate(v.begin(), v.end(), 0);
        expect(eq(sum, 2000 * 6)) << "the iterator element form must touch each element once";
    };

    "parallel_iterator_inplace_threshold"_test = [] {
        kimix::vector<int> v(64, 1);
        // A threshold above the batch count keeps the whole walk inline.
        kimix::fiber::parallel(v.begin(), v.end(), 16, [](auto l, auto r) noexcept {
            for (auto it = l; it != r; ++it) { *it = 7; }
        }, 100u);
        auto const sum = std::accumulate(v.begin(), v.end(), 0);
        expect(eq(sum, 64 * 7)) << "the inline path must still process every element";
    };

    "parallel_empty_range_is_a_noop"_test = [] {
        kimix::vector<int> v;
        std::atomic<uint32_t> calls{0u};
        kimix::fiber::parallel(0u, [&calls](uint32_t) noexcept { calls.fetch_add(1u); });
        kimix::fiber::parallel(v.begin(), v.end(), 8, [&calls](auto l, auto r) noexcept {
            calls.fetch_add(static_cast<uint32_t>(r - l));
        });
        expect(eq(calls.load(), 0u)) << "an empty job set must not invoke the body";
    };

    "async_parallel_returns_counter"_test = [] {
        constexpr uint32_t k_jobs = 2048u;
        kimix::vector<uint8_t> hit(k_jobs, 0u);
        auto evt = kimix::fiber::async_parallel(k_jobs, [&](uint32_t i) noexcept { hit[i] = 1u; });
        evt.wait();
        size_t ones = 0;
        for (auto b : hit) ones += b;
        expect(eq(ones, static_cast<size_t>(k_jobs))) << "async_parallel() must drain the returned counter";
    };

    "async_parallel_external_counter"_test = [] {
        constexpr uint32_t k_jobs = 512u;
        kimix::fiber::counter evt{0u};
        std::atomic<uint32_t> ran{0u};
        kimix::fiber::async_parallel(evt, k_jobs, [&ran](uint32_t) noexcept { ran.fetch_add(1u, std::memory_order_relaxed); }, 8u);
        evt.wait();
        expect(eq(ran.load(), k_jobs)) << "a caller-owned counter must drain too";
    };

    "async_parallel_iterator_range"_test = [] {
        kimix::vector<int> v(3000, 1);
        auto evt = kimix::fiber::async_parallel(v.begin(), v.end(), 64, [](auto l, auto r) noexcept {
            for (auto it = l; it != r; ++it) { *it += 1; }
        });
        evt.wait();
        auto const sum = std::accumulate(v.begin(), v.end(), 0);
        expect(eq(sum, 3000 * 2)) << "the async iterator split must visit each element once";
    };

    "fiber_defer_runs_at_scope_exit"_test = [] {
        int log = 0;
        {
            kimix_fiber_defer(log += 2);
            log += 1;
        }
        expect(eq(log, 3)) << "kimix_fiber_defer() must run when the scope closes";
    };

    "schedule_accepts_move_only_capture"_test = [] {
        // The closure is move-only (unique_ptr capture) while marl's Task stores
        // a copy-constructible std::function: this only compiles because
        // kimix::fiber::schedule() wraps the closure in a kimix::SharedFunction.
        // The closure releases the object back into this frame — reading it after
        // the task's own copy died would be a use-after-free (the MSVC debug heap
        // fills freed memory with 0xDDDDDDDD and does catch that).
        auto box = std::make_unique<std::atomic<uint32_t>>(0u);
        kimix::unique_ptr<std::atomic<uint32_t>> survivor{};
        kimix::fiber::counter evt{1u};
        kimix::fiber::schedule([ptr = std::move(box), &survivor, evt]() mutable noexcept {
            ptr->store(123u, std::memory_order_relaxed);
            survivor = std::move(ptr);
            evt.done();
        });
        evt.wait();
        expect(survivor != nullptr) << "the move-only capture must reach the task";
        expect(eq(survivor->load(), 123u)) << "move-only captures must survive submission";
    };

    "parallel_from_inside_a_fiber"_test = [] {
        // A task that blocks on a nested parallel(): the outer fiber must yield
        // its thread instead of dead-locking the pool.
        std::atomic<uint32_t> sum{0u};
        kimix::fiber::event finished;
        kimix::fiber::schedule([&sum, finished]() noexcept {
            kimix::fiber::parallel(128u, [&sum](uint32_t i) noexcept { sum.fetch_add(i, std::memory_order_relaxed); });
            finished.signal();
        });
        finished.wait();
        uint32_t expected = 0u;
        for (uint32_t i = 0; i < 128u; ++i) expected += i;
        expect(eq(sum.load(), expected)) << "a nested parallel() inside a fiber must complete";
    };

    "shared_function_ref_counts"_test = [] {
        auto cell = std::make_shared<std::uint32_t>(0u);
        auto *raw = cell.get();
        kimix::SharedFunction<void()> f{[c = cell]() noexcept { ++(*c); }};
        expect(bool(f));
        auto g = f; // same target, ref == 2
        auto h = g; // ref == 3
        f();
        g();
        h();
        expect(eq(*raw, 3u)) << "copies of a SharedFunction must invoke the same target";
        kimix::SharedFunction<void()> empty{};
        expect(!bool(empty)) << "a default-constructed wrapper is empty";
    };

    "shared_function_returns_values"_test = [] {
        kimix::SharedFunction<int(int)> twice{[](int v) noexcept { return v * 2; }};
        expect(eq(twice(21), 42)) << "a SharedFunction must forward arguments and results";
    };

    "shared_function_move_only_state"_test = [] {
        // The whole point of the wrapper: a move-only closure becomes copyable.
        auto owner = std::make_unique<std::uint32_t>(41u);
        auto *raw = owner.get();
        kimix::SharedFunction<void()> f{[p = std::move(owner)]() mutable noexcept { ++(*p); }};
        auto copies = kimix::vector<kimix::SharedFunction<void()>>{};
        copies.push_back(f);
        copies.push_back(f);
        for (auto &c : copies) c();
        expect(eq(*raw, 43u)) << "each copy must call the one shared closure";
    };
}
