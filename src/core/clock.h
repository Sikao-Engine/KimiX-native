/*
 * clock.h — kimix::Clock simple timer using std::chrono::steady_clock.
 *
 * kimix::Clock:
 *   Construction starts timing immediately.
 *   Methods:
 *     toc()          — returns elapsed time in milliseconds (double)
 *     toc_seconds()  — returns elapsed time in seconds (double)
 *     reset()        — resets the start time to now
 *     toc_reset()    — returns elapsed ms and resets at the same time
 *     now_ms()       — static; returns current steady_clock time epoch in ms
 *
 * Example:
 *   kimix::Clock clock;
 *   // ... do work ...
 *   double ms = clock.toc();
 *   clock.reset();
 */
#pragma once

#include <chrono>

namespace kimix {

// ---------------------------------------------------------------------------
// Clock — simple timer using std::chrono::steady_clock
// ---------------------------------------------------------------------------

class Clock {
public:
    using clock_type = std::chrono::steady_clock;
    using time_point = clock_type::time_point;

    Clock() noexcept : _start(clock_type::now()) {}

    // Reset the clock
    void reset() noexcept {
        _start = clock_type::now();
    }

    // Returns elapsed time in milliseconds
    double toc() const noexcept {
        auto now = clock_type::now();
        return std::chrono::duration<double, std::milli>(now - _start).count();
    }

    // Returns elapsed time in seconds
    double toc_seconds() const noexcept {
        auto now = clock_type::now();
        return std::chrono::duration<double>(now - _start).count();
    }

    // Returns elapsed time and resets the clock
    double toc_reset() noexcept {
        auto now = clock_type::now();
        auto elapsed = std::chrono::duration<double, std::milli>(now - _start).count();
        _start = now;
        return elapsed;
    }

    // Returns current time point as milliseconds since epoch
    static double now_ms() noexcept {
        auto now = clock_type::now();
        return std::chrono::duration<double, std::milli>(now.time_since_epoch()).count();
    }

private:
    time_point _start;
};

} // namespace kimix
