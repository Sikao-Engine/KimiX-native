// Test for clock.h (kimix::Clock).
// This test covers:
// - toc() returns positive value
// - Multiple tocs show increasing time
// - Clock resolution is reasonable (< 1 sec for a simple operation)

#include "ut/ut.hpp"
#include <core/kimix_core.h>

#include <thread>
#include <chrono>

using namespace boost::ut;
using namespace boost::ut::literals;

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "clock_toc_positive"_test = [] {
        kimix::Clock clock;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto elapsed = clock.toc();
        expect(elapsed > 0.0) << "elapsed time should be positive";
    };

    "clock_multiple_tocs_increasing"_test = [] {
        kimix::Clock clock;
        auto t1 = clock.toc();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        auto t2 = clock.toc();
        expect(t2 >= t1) << "subsequent tocs should be >= previous";
    };

    "clock_reset"_test = [] {
        kimix::Clock clock;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto t1 = clock.toc();
        clock.reset();
        auto t2 = clock.toc();
        expect(t2 < t1) << "after reset, elapsed should be less than before reset";
    };

    "clock_toc_seconds"_test = [] {
        kimix::Clock clock;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        auto secs = clock.toc_seconds();
        expect(secs > 0.0) << "toc_seconds should return positive value";
        expect(secs < 1.0) << "5ms sleep should be well under 1 second";
    };

    "clock_toc_reset"_test = [] {
        kimix::Clock clock;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        auto elapsed = clock.toc_reset();
        expect(elapsed > 0.0) << "toc_reset should return positive value";

        // After reset, clock should start fresh
        auto new_elapsed = clock.toc();
        expect(new_elapsed < elapsed) << "after toc_reset, elapsed should be small";
    };

    "clock_now_ms"_test = [] {
        auto t1 = kimix::Clock::now_ms();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        auto t2 = kimix::Clock::now_ms();
        expect(t2 > t1) << "now_ms should increase over time";
    };

    "clock_resolution_reasonable"_test = [] {
        kimix::Clock clock;
        // Do a simple operation — elapsed should be well under 1 second
        volatile int x = 0;
        for (int i = 0; i < 1000; ++i) { x += i; }
        (void)x;
        auto elapsed_ms = clock.toc();
        expect(elapsed_ms < 1000.0) << "elapsed should be well under 1 second for simple loop";
        expect(elapsed_ms >= 0.0) << "elapsed should be non-negative";
    };

}
