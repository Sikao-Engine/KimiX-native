// Test for core/fiber.h (kimix::fiber).
// This test covers:
// - scheduler construction and worker_thread_count()
// - schedule() with counter (WaitGroup) synchronization
// - event signal/wait/test/clear
// - future<T> signal/wait/test/clear
// - async() returning event (void) and future<T> (value)
// - async_parallel() job-index and range-based overloads
// - parallel() blocking job-index, job-range, and iterator-range overloads
// - KIMIX_FIBER_DEFER scope-exit execution

#include "ut/ut.hpp"
#include <core/fiber.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    // A scheduler must be bound to the main thread before any fiber API is used.
    kimix::fiber::scheduler scheduler;

    "scheduler_and_worker_thread_count"_test = [] {
        expect(kimix::fiber::worker_thread_count() >= 1u) << "scheduler must have at least one worker thread";
    };

    "schedule_with_counter"_test = [] {
        std::atomic<uint32_t> executed{0u};
        kimix::fiber::counter wg{8};
        for (auto i = 0; i < 8; ++i) {
            kimix::fiber::schedule([&executed, wg]() mutable noexcept {
                executed.fetch_add(1u);
                wg.done();
            });
        }
        wg.wait();
        expect(executed.load() == 8u) << "all scheduled tasks must execute";
    };

    "event_signal_wait_test_clear"_test = [] {
        kimix::fiber::event evt;
        expect(!evt.test()) << "event must start unsignalled";
        kimix::fiber::schedule([evt]() mutable noexcept { evt.signal(); });
        evt.wait();
        expect(evt.is_signalled()) << "event must be signalled after wait()";
        evt.clear();
        expect(!evt.test()) << "event must be unsignalled after clear()";
    };

    "future_signal_wait_test_clear"_test = [] {
        kimix::fiber::future<int> fut;
        expect(!fut.test()) << "future must start unsignalled";
        kimix::fiber::schedule([fut]() mutable noexcept { fut.signal(42); });
        expect(eq(fut.wait(), 42)) << "future must deliver the signalled value";
        expect(fut.is_signalled()) << "future must be signalled after wait()";
        fut.clear();
        expect(!fut.test()) << "future must be unsignalled after clear()";
    };

    "async_void_returns_event"_test = [] {
        std::atomic<bool> done{false};
        auto evt = kimix::fiber::async([&done]() noexcept { done.store(true); });
        evt.wait();
        expect(done.load()) << "async task must have executed";
    };

    "async_value_returns_future"_test = [] {
        auto fut = kimix::fiber::async([]() noexcept { return 42; });
        expect(eq(fut.wait(), 42)) << "async future must deliver the computed value";
    };

    "async_parallel_job_index"_test = [] {
        constexpr auto job_count = 1000u;
        std::vector<std::atomic<uint32_t>> flags(job_count);
        for (auto &f : flags) { f.store(0u); }
        auto wg = kimix::fiber::async_parallel(job_count, [&flags](uint32_t i) noexcept {
            flags[i].fetch_add(1u);
        });
        wg.wait();
        bool all_once = true;
        for (const auto &f : flags) {
            if (f.load() != 1u) { all_once = false; }
        }
        expect(all_once) << "every job index must execute exactly once";
    };

    "async_parallel_job_index_with_counter"_test = [] {
        constexpr auto job_count = 500u;
        std::atomic<uint32_t> sum{0u};
        kimix::fiber::counter wg;
        kimix::fiber::async_parallel(wg, job_count, [&sum](uint32_t i) noexcept {
            sum.fetch_add(i);
        }, 4u);
        wg.wait();
        expect(eq(sum.load(), job_count * (job_count - 1u) / 2u)) << "sum of all job indices must be correct";
    };

    "parallel_job_index_blocking"_test = [] {
        constexpr auto job_count = 1000u;
        std::vector<std::atomic<uint32_t>> flags(job_count);
        for (auto &f : flags) { f.store(0u); }
        kimix::fiber::parallel(job_count, [&flags](uint32_t i) noexcept {
            flags[i].fetch_add(1u);
        });
        bool all_once = true;
        for (const auto &f : flags) {
            if (f.load() != 1u) { all_once = false; }
        }
        expect(all_once) << "every job index must execute exactly once";
    };

    "parallel_job_range_blocking"_test = [] {
        constexpr auto job_count = 1000u;
        std::vector<std::atomic<uint32_t>> flags(job_count);
        for (auto &f : flags) { f.store(0u); }
        kimix::fiber::parallel(job_count, [&flags](uint32_t begin, uint32_t end) noexcept {
            for (auto i = begin; i < end; ++i) {
                flags[i].fetch_add(1u);
            }
        }, 8u);
        bool all_once = true;
        for (const auto &f : flags) {
            if (f.load() != 1u) { all_once = false; }
        }
        expect(all_once) << "every job in every range must execute exactly once";
    };

    "parallel_single_thread_fallback"_test = [] {
        // job_count / internal_jobs < 1 forces the inline single-thread path
        std::atomic<uint32_t> executed{0u};
        kimix::fiber::parallel(4u, [&executed](uint32_t) noexcept {
            executed.fetch_add(1u);
        }, 64u);
        expect(executed.load() == 4u) << "inline fallback must run all jobs";
    };

    "parallel_range_element_wise"_test = [] {
        constexpr auto n = 1000u;
        std::vector<uint32_t> data(n, 0u);
        kimix::fiber::parallel(data.begin(), data.end(), 16u, [](auto it) noexcept {
            *it += 1u;
        });
        bool all_one = true;
        for (auto v : data) {
            if (v != 1u) { all_one = false; }
        }
        expect(all_one) << "every element must be visited exactly once";
    };

    "parallel_range_batch_wise"_test = [] {
        constexpr auto n = 1000u;
        std::vector<std::atomic<uint32_t>> data(n);
        for (auto &v : data) { v.store(0u); }
        kimix::fiber::parallel(data.begin(), data.end(), 16u, [](auto l, auto r) noexcept {
            for (auto it = l; it != r; ++it) {
                it->fetch_add(1u);
            }
        });
        bool all_one = true;
        for (const auto &v : data) {
            if (v.load() != 1u) { all_one = false; }
        }
        expect(all_one) << "every element must be visited exactly once";
    };

    "parallel_range_inplace_threshold"_test = [] {
        // batch_count (2) < inplace_batch_threshold (3) forces the inline path
        std::vector<uint32_t> data(8u, 0u);
        kimix::fiber::parallel(data.begin(), data.end(), 4u, [](auto it) noexcept {
            *it += 1u;
        }, 3u);
        bool all_one = true;
        for (auto v : data) {
            if (v != 1u) { all_one = false; }
        }
        expect(all_one) << "inline path must visit every element exactly once";
    };

    "async_parallel_range"_test = [] {
        constexpr auto n = 1000u;
        std::vector<std::atomic<uint32_t>> data(n);
        for (auto &v : data) { v.store(0u); }
        auto wg = kimix::fiber::async_parallel(data.begin(), data.end(), 32u, [](auto l, auto r) noexcept {
            for (auto it = l; it != r; ++it) {
                it->fetch_add(1u);
            }
        });
        wg.wait();
        bool all_one = true;
        for (const auto &v : data) {
            if (v.load() != 1u) { all_one = false; }
        }
        expect(all_one) << "every element must be visited exactly once";
    };

    "fiber_defer_runs_on_scope_exit"_test = [] {
        auto value = 0;
        {
            KIMIX_FIBER_DEFER(value = 42);
            expect(value == 0) << "defer must not run before scope exit";
        }
        expect(value == 42) << "defer must run on scope exit";
    };
}
