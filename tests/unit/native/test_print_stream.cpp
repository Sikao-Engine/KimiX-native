// Test for src/runtime/print/print_stream.h (asynchronous print stream).
// This test covers:
// - basic queued printing through the process-wide singleton
// - printf-style formatting via PrintStream::printf(fmt, ...)
// - raw string_view printing (no printf interpretation, flush hint)
// - multi-producer integrity: N threads x M lines, every line intact
// - destructor shutdown: remaining messages are drained and the thread joins
// - large payloads (multi-pass vsnprintf + fwrite round-trip)
//
// PrintStream has no blocking flush() barrier, so the tests poll the captured
// stdout file until the expected output appears (bounded by a timeout).

#include "ut/ut.hpp"
#include <runtime/print/print_stream.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;
using kimix::runtime::print::PrintStream;
using kimix::runtime::print::print_stream;

namespace {

// Redirect stdout to a temporary file. PrintStream's worker thread writes to
// the process-wide CRT stdout, and all targets share the same dynamic CRT, so
// output lands in the file. Restores stdout on destruction.
class stdout_capture {
public:
    stdout_capture() {
        path_ = (std::filesystem::temp_directory_path() /
                 "kimix_print_stream_test.txt")
                    .string();
        std::remove(path_.c_str());
        std::fflush(stdout);
        redirected_ = std::freopen(path_.c_str(), "wb", stdout) != nullptr;
    }

    ~stdout_capture() {
        std::fflush(stdout);
#ifdef _WIN32
        std::freopen("CONOUT$", "w", stdout);
#else
        std::freopen("/dev/tty", "w", stdout);
#endif
        std::remove(path_.c_str());
    }

    bool ok() const { return redirected_; }

    // Read back everything currently in the file.
    kimix::string read() {
        std::FILE* f = std::fopen(path_.c_str(), "rb");
        if (f == nullptr) {
            return kimix::string();
        }
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        kimix::string out;
        if (n > 0) {
            out.resize(static_cast<size_t>(n));
            const size_t got = std::fread(out.data(), 1, static_cast<size_t>(n), f);
            out.resize(got);
        }
        std::fclose(f);
        return out;
    }

    // Poll until the file content equals `expected` (the worker is async; it
    // flushes per message when flush=true, and our fflush pushes any buffered
    // fwrite data). Returns true on success within the timeout.
    bool wait_for(kimix::string_view expected, int timeout_ms = 5000) {
        const kimix::string want(expected);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            std::fflush(stdout);
            if (read() == want) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

private:
    std::string path_;
    bool redirected_ = false;
};

struct line_stats {
    size_t line_count = 0;
    int bad = 0;      // malformed / unterminated trailing lines
    bool all_once = true; // every expected (t, j) line seen exactly once
};

line_stats analyze(const kimix::string& out, int kThreads, int kLines) {
    line_stats st;
    std::vector<std::vector<int>> seen(
        kThreads, std::vector<int>(kLines, 0));
    size_t pos = 0;
    while (pos < out.size()) {
        const size_t nl = out.find('\n', pos);
        if (nl == kimix::string::npos) {
            ++st.bad; // partial trailing line (worker still writing)
            break;
        }
        const kimix::string line = out.substr(pos, nl - pos);
        int t = -1;
        int j = -1;
        if (std::sscanf(line.c_str(), "t%d-%d", &t, &j) == 2 &&
            t >= 0 && t < kThreads && j >= 0 && j < kLines) {
            ++seen[t][j];
        } else {
            ++st.bad;
        }
        ++st.line_count;
        pos = nl + 1;
    }
    for (int t = 0; t < kThreads && st.all_once; ++t) {
        for (int j = 0; j < kLines; ++j) {
            if (seen[t][j] != 1) {
                st.all_once = false;
                break;
            }
        }
    }
    return st;
}

} // namespace

int main(int argc, char* argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char**>(argv));

    "print_basic_and_format"_test = [] {
        stdout_capture cap;
        expect(cap.ok());

        // Raw text (string_view overload; no printf interpretation).
        print_stream().print("hello\n");
        // printf-style formatting.
        print_stream().printf("num=%d\n", 42);
        // Raw text with an explicit flush hint; "%s" must NOT be interpreted.
        print_stream().print(kimix::string_view("raw %s text\n"), false);

        expect(cap.wait_for("hello\nnum=42\nraw %s text\n"));
    };

    "flush_hint_writes_unflushed_tail"_test = [] {
        stdout_capture cap;
        expect(cap.ok());

        print_stream().print("a");
        print_stream().print(kimix::string_view("b"), false);

        // Both messages are written by the worker; the unflushed "b" is
        // pushed to the file by the poll's fflush.
        expect(cap.wait_for("ab"));
    };

    "multi_producer_integrity"_test = [] {
        stdout_capture cap;
        expect(cap.ok());

        constexpr int kThreads = 8;
        constexpr int kLines = 200;
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([t] {
                for (int j = 0; j < kLines; ++j) {
                    print_stream().printf("t%d-%d\n", t, j);
                }
            });
        }
        for (auto& th : threads) {
            th.join();
        }

        bool done = false;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            std::fflush(stdout);
            const line_stats st = analyze(cap.read(), kThreads, kLines);
            if (st.line_count == size_t(kThreads * kLines) && st.bad == 0 &&
                st.all_once) {
                done = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(done);
    };

    "destructor_drains_and_joins"_test = [] {
        stdout_capture cap;
        expect(cap.ok());

        {
            // A standalone instance: no explicit flush — the destructor must
            // disable the loop, notify the condition variable, join the thread
            // and write the remaining queued messages.
            PrintStream local;
            local.print("alpha\n");
            local.printf("beta=%d\n", 7);
        }

        expect(cap.wait_for("alpha\nbeta=7\n"));
    };

    "large_payload"_test = [] {
        stdout_capture cap;
        expect(cap.ok());

        const size_t kSize = 1u << 20; // 1 MiB
        const kimix::string big(kSize, 'x');
        print_stream().print(kimix::string_view(big));

        expect(cap.wait_for(kimix::string_view(big), 10000));
    };
}
