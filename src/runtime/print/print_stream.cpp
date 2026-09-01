/*
 * print_stream.cpp — implementation of the asynchronous print stream.
 *
 * Compiled into runtime_py by the recursive glob in src/xmake.lua
 * (add_files("runtime/**.cpp")).
 */

#include <runtime/print/print_stream.h>

#include <cstdio>

namespace kimix {
namespace runtime {
namespace print {

namespace {

void write_payload(const char* data, size_t n, bool flush) {
    if (n > 0) {
        std::fwrite(data, 1, n, stdout);
    }
    if (flush) {
        std::fflush(stdout);
    }
}

} // namespace

PrintStream::PrintStream() : thread_([this] { thread_loop(); }) {}

PrintStream::~PrintStream() {
    enabled_.store(false, std::memory_order_release);
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void PrintStream::print(kimix::string_view text, bool flush) {
    enqueue_message(detail::message_buffer(text), flush);
}

void PrintStream::enqueue_message(detail::message_buffer&& msg, bool flush) {
    if (flush) {
        flush_pending_.store(true, std::memory_order_relaxed);
    }
    queue_.enqueue(std::move(msg));
    // Note: notifying on every enqueue is intentional. The worker drains the
    // whole queue per wake-up, and an empty-queue-conditional wake was A/B
    // measured (notify every 64th enqueue, mutex-guarded) with no throughput
    // gain on Windows/x64 — MSVC's condition_variable no-waiter notify is
    // cheap, while the mutex acquire made flush-heavy paths measurably worse.
    // The unconditional notify also keeps tail latency of a lone flush=false
    // print equivalent to a synchronous stream.
    cv_.notify_one();
}

void PrintStream::printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprint(fmt, args, true);
    va_end(args);
}

void PrintStream::vprint(const char* fmt, va_list args, bool flush) {
    if (fmt == nullptr) {
        return;
    }
    va_list copy;
    va_copy(copy, args);
    const int needed = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        return; // formatting failed; drop the message
    }
    kimix::vector<char> text;
    text.resize(static_cast<size_t>(needed) + 1); // +1 for the NUL terminator
    std::vsnprintf(text.data(), static_cast<size_t>(needed) + 1, fmt, args);
    text.resize(static_cast<size_t>(needed));     // drop the terminator from size
    // Hand the formatted buffer to the queue by move: no second copy of the
    // formatted payload on the producer path.
    enqueue_message(detail::message_buffer(std::move(text)), flush);
}

void PrintStream::thread_loop() {
    // Persistent buffers: keep the per-item dequeue slot and the drained
    // small-message batch alive across cycles so their capacity is reused
    // instead of being reallocated on every wake-up.
    detail::message_buffer msg;
    kimix::vector<char> batch;
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return !enabled_.load(std::memory_order_acquire) ||
                       queue_.size_approx() > 0 ||
                       flush_pending_.load(std::memory_order_acquire);
            });
        }

        // Drain the whole queue per wake-up. SSO-size payloads are gathered
        // into one contiguous batch, so a burst of log lines is written as a
        // single fwrite; larger (heap-backed) payloads are streamed straight
        // from their own already-allocated buffer instead of being copied
        // into the batch. The pending gathered run is emitted before each
        // streamed payload to preserve exact FIFO output order.
        while (queue_.try_dequeue(msg)) {
            if (msg.size() <= detail::message_buffer::kInline) {
                batch.insert(batch.end(), msg.begin(), msg.end());
            } else {
                if (!batch.empty()) {
                    write_payload(batch.data(), batch.size(), false);
                    batch.clear();
                }
                write_payload(msg.data(), msg.size(), false);
            }
        }

        // Any flush hint seen while draining applies to everything written
        // this cycle: fflush once, after the streamed payloads as well.
        const bool flush =
            flush_pending_.exchange(false, std::memory_order_acq_rel);
        if (!batch.empty()) {
            write_payload(batch.data(), batch.size(), flush);
            batch.clear(); // keep the capacity for the next cycle
        } else if (flush) {
            std::fflush(stdout);
        }

        // Shutdown: drain whatever is still queued, then exit.
        if (!enabled_.load(std::memory_order_acquire) &&
            queue_.size_approx() == 0) {
            break;
        }
    }
}

KIMIX_RUNTIME_API PrintStream& print_stream() {
    static PrintStream instance;
    return instance;
}

} // namespace print
} // namespace runtime
} // namespace kimix
