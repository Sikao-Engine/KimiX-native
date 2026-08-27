/*
 * print_stream.h — Asynchronous print stream (kimix::runtime::print).
 *
 * PrintStream owns a lock-free concurrent queue (rbc::ConcurrentQueue from
 * src/core/rbc_concurrent_queue.h) plus a dedicated worker thread created
 * with std::thread. `print()` enqueues a formatted message and wakes the
 * worker via std::condition_variable; the worker pops one message per wake-up
 * and writes it to stdout with std::fwrite / std::fflush.
 *
 * Thread lifecycle:
 * - The worker loops forever: it waits on the condition variable (enabled OR
 *   queue non-empty), pops at most one message, writes it, and exits once the
 *   stream is disabled and the queue is empty.
 * - The destructor sets the enabled flag to false, notifies the condition
 *   variable and joins the worker thread, draining any remaining messages so
 *   no output is lost at shutdown.
 *
 * A process-wide instance is available through print_stream(); the class can
 * also be instantiated directly (each instance owns its own queue + thread).
 */

#pragma once

#include <core/kimix_core.h>

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <mutex>
#include <thread>

#include <core/rbc_concurrent_queue.h>

namespace kimix {
namespace runtime {
namespace print {

// A queued print job: already-formatted text plus a flush hint.
struct print_message {
    kimix::string text;
    bool flush = true;
};

class KIMIX_RUNTIME_API PrintStream {
public:
    PrintStream();
    ~PrintStream();

    PrintStream(const PrintStream&) = delete;
    PrintStream& operator=(const PrintStream&) = delete;

    // Write raw text as-is (no printf interpretation). When `flush` is true
    // the worker thread fflushes stdout after writing.
    void print(kimix::string_view text, bool flush = true);

    // printf-style convenience: formats the message then enqueues it
    // (flush = true). Named `printf` (not overloaded on `print`) so calls like
    // printf("num=%d\n", 42) are unambiguous against print(string_view, bool).
    void printf(const char* fmt, ...);

    // printf-style formatting with an explicit va_list.
    void vprint(const char* fmt, va_list args, bool flush = true);

    bool enabled() const noexcept {
        return enabled_.load(std::memory_order_acquire);
    }

private:
    void thread_loop();

    rbc::ConcurrentQueue<print_message> queue_;
    // mutex_/cv_ are declared BEFORE thread_: the worker thread is started in
    // the constructor and must find them fully constructed (members initialize
    // in declaration order, not init-list order).
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> enabled_{true};
    std::thread thread_;
};

// Process-wide instance. Thread-safe static initialization (C++11); the
// worker thread is joined when the instance is destroyed at process exit.
KIMIX_RUNTIME_API PrintStream& print_stream();

} // namespace print
} // namespace runtime
} // namespace kimix
