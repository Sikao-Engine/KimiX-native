/*
 * fiber_future.h — kimix::fiber::Future<T>, a fiber-blocking value future.
 *
 * marl::Event only carries a signal; a task that produces a result needs the
 * value too. Future<T> is the fiber-aware equivalent of std::promise/std::shared_future
 * built on marl's fiber primitives: the producer fiber calls signal(args...),
 * every waiting fiber yields (the worker thread keeps running other fibers) and
 * resumes with the stored value.
 *
 *   auto f = kimix::fiber::Future<int>{};
 *   kimix::fiber::schedule([f] { f.signal(42); });
 *   int v = f.wait();      // -> 42
 *
 * Copying a Future copies the handle; all copies share one state, so any of them
 * may signal or wait, and a signalled value can be read by every waiter.
 *
 * Header-only; NOT part of kimix_core.h — include <core/fiber_future.h> (or
 * <core/fiber.h>, which includes it) directly. Requires the marl target
 * (src/ext/marl) on the include/link line.
 */
#pragma once

// marl's mutex/condition-variable headers transitively pull <Windows.h> on
// Windows; winsock2.h must come first (the kimix_core.h umbrella does the same),
// otherwise windows.h loads winsock.h and a later <ws2tcpip.h> collides.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

#include <marl/conditionvariable.h>
#include <marl/memory.h>
#include <marl/mutex.h>

#include "stl/optional.h"

namespace kimix::fiber {

/// Shared state: the value plus the fiber-aware mutex/cv guarding it.
template<typename T>
class FutureShared {
public:
    explicit FutureShared(marl::Allocator *allocator) noexcept : cv{allocator} {}

    template<typename... Args>
        requires(std::is_constructible_v<T, Args &&...>)
    void signal(Args &&...args) {
        {
            marl::lock l{mutex};
            _result.reset();
            _result.emplace(std::forward<Args>(args)...);
        }
        cv.notify_all();
    }

    void clear() noexcept {
        marl::lock l{mutex};
        _result.reset();
    }

    T &wait() {
        marl::lock l{mutex};
        cv.wait(l, [&] { return static_cast<bool>(_result); });
        return *_result;
    }

    [[nodiscard]] bool test() noexcept {
        marl::lock l{mutex};
        return _result.has_value();
    }

private:
    marl::mutex mutex;
    marl::ConditionVariable cv;
    kimix::optional<T> _result;
};

/// Fiber-blocking future: signal() the value once, wait() for it from any fiber.
template<typename T>
class Future {
public:
    using Shared = FutureShared<T>;
    using SharedPtr = marl::shared_ptr<Shared>;

    explicit Future(marl::Allocator *allocator = marl::Allocator::Default) noexcept
        : _shared{allocator->make_shared<Shared>(allocator)} {}

    Future(Future const &) = default;
    Future(Future &&) = default;
    Future &operator=(Future const &) = default;
    Future &operator=(Future &&) = default;

    /// Store the value (constructing T from args...) and wake every waiter.
    template<typename... Args>
        requires(std::is_constructible_v<T, Args &&...>)
    void signal(Args &&...args) const {
        _shared->signal(std::forward<Args>(args)...);
    }

    /// Drop the stored value (only meaningful before another wait()).
    void clear() const { _shared->clear(); }

    /// Block the calling fiber until a value is signalled, then return it.
    [[nodiscard]] T &wait() const { return _shared->wait(); }

    /// Non-blocking poll: true once a value has been signalled.
    [[nodiscard]] bool test() const { return _shared->test(); }

    [[nodiscard]] bool is_signalled() const { return _shared->test(); }

private:
    SharedPtr _shared;
};

}// namespace kimix::fiber
