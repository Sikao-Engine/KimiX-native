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

void write_message(const print_message& msg) {
    if (!msg.text.empty()) {
        std::fwrite(msg.text.data(), 1, msg.text.size(), stdout);
    }
    if (msg.flush) {
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
    print_message msg;
    msg.text.assign(text.data(), text.size());
    msg.flush = flush;
    queue_.enqueue(std::move(msg));
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
    kimix::string text;
    text.resize(static_cast<size_t>(needed));
    if (needed > 0) {
        std::vsnprintf(text.data(), static_cast<size_t>(needed) + 1, fmt, args);
    }
    print(kimix::string_view(text.data(), text.size()), flush);
}

void PrintStream::thread_loop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return !enabled_.load(std::memory_order_acquire) ||
                       queue_.size_approx() > 0;
            });
        }

        // Pop at most one message per wake-up and write it.
        print_message msg;
        if (queue_.try_dequeue(msg)) {
            write_message(msg);
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
