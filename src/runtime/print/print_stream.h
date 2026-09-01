/*
 * print_stream.h — Asynchronous print stream (kimix::runtime::print).
 *
 * PrintStream owns a lock-free concurrent queue (rbc::ConcurrentQueue from
 * src/core/rbc_concurrent_queue.h) of raw output buffers plus a dedicated
 * worker thread created with std::thread. `print()` enqueues a formatted
 * buffer and wakes the worker via std::condition_variable; the worker drains
 * all pending buffers per wake-up, gathers payloads up to a streaming
 * threshold into a single batch (streaming larger payloads straight from
 * their already-allocated buffer) and writes it to stdout with std::fwrite /
 * std::fflush.
 *
 * Queued payloads are detail::message_buffer — a char buffer with the same
 * surface as kimix::vector<char> (data/size/begin/end) but with a 64-byte
 * inline storage fast path, so short payloads (the common case for log lines)
 * never hit the heap on the producer hot path. Longer payloads fall back to a
 * mimalloc-backed kimix::vector<char>, and vprint hands its formatted buffer
 * over by move so large payloads are not copied twice on the producer path.
 *
 * Thread lifecycle:
 * - The worker loops forever: it waits on the condition variable (enabled OR
 *   queue non-empty OR a flush was requested), drains every queued buffer,
 *   writes the drained payload, and exits once the stream is disabled and
 *   the queue is empty.
 * - The destructor sets the enabled flag to false, notifies the condition
 *   variable and joins the worker thread, draining any remaining buffers so
 *   nothing is lost at shutdown.
 *
 * A process-wide instance is available through print_stream(); the class can
 * also be instantiated directly (each instance owns its own queue + thread).
 */

#pragma once

#include <core/kimix_core.h>

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <thread>

#include <core/rbc_concurrent_queue.h>

namespace kimix {
namespace runtime {
namespace print {

namespace detail {

// Raw char payload queued between producers and the worker. Short messages
// are stored in the inline array (no heap allocation); larger payloads use a
// mimalloc-backed kimix::vector<char>. Move-only, with noexcept moves so the
// lock-free queue takes the fast path.
class message_buffer {
public:
    static constexpr size_t kInline = 64;

    message_buffer() = default;
    message_buffer(kimix::string_view text) {
        append(text.data(), text.data() + text.size());
    }

    // Adopt an already-formatted buffer (used by vprint) so large payloads
    // are moved, not copied, on the producer hot path.
    explicit message_buffer(kimix::vector<char>&& text) {
        const size_t count = text.size();
        if (count > 0) {
            if (count <= kInline) {
                std::memcpy(small_, text.data(), count);
                inline_ = true;
            } else {
                heap_ = std::move(text);
                inline_ = false;
            }
        }
        size_ = count;
    }

    message_buffer(const message_buffer&) = delete;
    message_buffer& operator=(const message_buffer&) = delete;

    message_buffer(message_buffer&& other) noexcept { move_from(other); }

    message_buffer& operator=(message_buffer&& other) noexcept {
        if (this != &other) {
            move_from(other);
        }
        return *this;
    }

    const char* data() const noexcept {
        return inline_ ? small_ : heap_.data();
    }

    size_t size() const noexcept { return size_; }

    bool empty() const noexcept { return size_ == 0; }

    const char* begin() const noexcept { return data(); }

    const char* end() const noexcept { return data() + size_; }

private:
    void append(const char* first, const char* last) {
        const size_t count = static_cast<size_t>(last - first);
        if (count > 0) {
            if (count <= kInline) {
                std::memcpy(small_, first, count);
                inline_ = true;
            } else {
                heap_.assign(first, last);
                inline_ = false;
            }
        }
        size_ = count;
    }

    void move_from(message_buffer& other) noexcept {
        if (!inline_) {
            // Release any heap buffer we currently own.
            heap_ = kimix::vector<char>();
        }
        inline_ = other.inline_;
        size_ = other.size_;
        if (inline_) {
            std::memcpy(small_, other.small_, size_);
        } else {
            heap_ = std::move(other.heap_);
        }
        other.inline_ = true;
        other.size_ = 0;
    }

    bool inline_ = true;
    size_t size_ = 0;
    char small_[kInline];
    kimix::vector<char> heap_;
};

} // namespace detail

class KIMIX_RUNTIME_API PrintStream {
public:
    PrintStream();
    ~PrintStream();

    PrintStream(const PrintStream&) = delete;
    PrintStream& operator=(const PrintStream&) = delete;

    // Write raw text as-is (no printf interpretation). When `flush` is true
    // the worker thread fflushes stdout after writing.
    void print(kimix::string_view text, bool flush = true);

    // printf-style convenience: formats the message then queues it for the
    // worker (flush = true). Named `printf` (not overloaded on `print`) so
    // calls like printf("num=%d\n", 42) are unambiguous against
    // print(string_view, bool).
    void printf(const char* fmt, ...);

    // printf-style formatting with an explicit va_list.
    void vprint(const char* fmt, va_list args, bool flush = true);

    bool enabled() const noexcept {
        return enabled_.load(std::memory_order_acquire);
    }

private:
    void thread_loop();

    // Shared producer fast path: record the flush hint, enqueue the payload
    // and wake the worker. Kept private so producers cannot bypass the hint /
    // notify logic.
    void enqueue_message(detail::message_buffer&& msg, bool flush);

    // Lock-free queue of raw output buffers, fed by producers and drained by
    // the worker thread.
    rbc::ConcurrentQueue<detail::message_buffer> queue_;
    // Best-effort flush hint: set by producers, read and cleared by the worker
    // after it drains a batch. The worker flushes when a flush was requested.
    std::atomic<bool> flush_pending_{false};
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
