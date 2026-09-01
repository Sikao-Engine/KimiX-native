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
#include "bench_util.h"
#include <runtime/print/print_stream.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using namespace boost::ut;
using namespace boost::ut::literals;
using kimix::runtime::print::PrintStream;
using kimix::runtime::print::print_stream;

namespace {

// Redirect stdout to a temporary file. PrintStream's worker thread writes to
// the process-wide CRT stdout, and all targets share the same dynamic CRT, so
// output lands in the file. Restores stdout to its original destination on
// destruction (fd-based restore, so it works when the process is piped and
// there is no console).
class stdout_capture {
public:
    stdout_capture() {
        path_ = (std::filesystem::temp_directory_path() /
                 "kimix_print_stream_test.txt")
                    .string();
        std::remove(path_.c_str());
        std::fflush(stdout);
#ifdef _WIN32
        saved_fd_ = _dup(1);
#endif
        redirected_ = std::freopen(path_.c_str(), "wb", stdout) != nullptr;
    }

    ~stdout_capture() {
        std::fflush(stdout);
#ifdef _WIN32
        if (saved_fd_ >= 0) {
            _dup2(saved_fd_, 1);
            _close(saved_fd_);
        }
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
    int saved_fd_ = -1;
    bool redirected_ = false;
};

// Redirect stdout to the OS null device during drain benchmarks (NUL on
// Windows), so the measured time covers the queue + worker instead of the
// console. Restores stdout to its original destination on destruction (fd
// based, so pipe capture by CI/harnesses keeps working). Benchmark timing
// lines are written to stderr by bench_util.h, so they are never swallowed.
class stdout_to_nul {
public:
    stdout_to_nul() {
        std::fflush(stdout);
#ifdef _WIN32
        saved_fd_ = _dup(1);
        redirected_ = std::freopen("NUL", "w", stdout) != nullptr;
#else
        redirected_ = std::freopen("/dev/null", "w", stdout) != nullptr;
#endif
    }

    ~stdout_to_nul() {
        std::fflush(stdout);
#ifdef _WIN32
        if (saved_fd_ >= 0) {
            _dup2(saved_fd_, 1);
            _close(saved_fd_);
        }
#else
        std::freopen("/dev/tty", "w", stdout);
#endif
    }

    bool ok() const { return redirected_; }

private:
    int saved_fd_ = -1;
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

    // ---- Benchmarks -----------------------------------------------------
    // PrintStream is an async producer/consumer, so per-op latency is not the
    // right metric. These cases measure the end-to-end drain: a local
    // PrintStream is created and fed N prints inside the timed call, and its
    // destructor (which disables, notifies and joins the worker) is the drain
    // barrier, so the measured time covers enqueue + worker drain + write.
    // Throughput is reported as prints/s via kimix_bench::run(ops_per_iter=N).
    // stdout is redirected to NUL ("stdout_to_nul") during the timed passes so
    // timing measures queue + worker, not the console; each case still verifies
    // exact output content/order with expect() first (stdout_capture -> file).

    "bench_print_drain_100k_sso"_test = [] {
        // Correctness pre-pass: 10 flush=false prints, exact content + order.
        {
            stdout_capture cap;
            expect(cap.ok());
            {
                PrintStream s;
                for (int i = 0; i < 10; ++i) {
                    s.print("hello\n", false);
                }
            } // destructor joins -> drains
            expect(cap.wait_for("hello\nhello\nhello\nhello\nhello\n"
                                "hello\nhello\nhello\nhello\nhello\n"));
        }
        stdout_to_nul nul;
        expect(nul.ok());
        constexpr size_t kOps = 100000; // SSO path (5-byte payloads)
        kimix_bench::run("print/drain_100k_sso", [&] {
            PrintStream s;
            for (size_t i = 0; i < kOps; ++i) {
                s.print("hello\n", false);
            }
            // destructor drains + joins
        }, kOps);
    };

    "bench_print_drain_4producers_25k"_test = [] {
        // Correctness pre-pass: 2 producers x 50 printf lines, every line
        // intact and seen exactly once (same analyzer as the integrity test).
        {
            stdout_capture cap;
            expect(cap.ok());
            {
                PrintStream s;
                constexpr int kT = 2, kL = 50;
                std::vector<std::thread> threads;
                threads.reserve(kT);
                for (int t = 0; t < kT; ++t) {
                    threads.emplace_back([&s, t] {
                        for (int j = 0; j < kL; ++j) {
                            s.printf("t%d-%d\n", t, j);
                        }
                    });
                }
                for (auto& th : threads) {
                    th.join();
                }
            } // destructor joins -> drains
            bool done = false;
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline) {
                std::fflush(stdout);
                const line_stats st = analyze(cap.read(), 2, 50);
                if (st.line_count == 100 && st.bad == 0 && st.all_once) {
                    done = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            expect(done);
        }
        stdout_to_nul nul;
        expect(nul.ok());
        constexpr size_t kOps = 100000; // 4 producers x 25k
        kimix_bench::run("print/drain_4p_x_25k", [&] {
            PrintStream s;
            constexpr int kThreads = 4;
            std::vector<std::thread> threads;
            threads.reserve(kThreads);
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&s] {
                    for (int j = 0; j < 25000; ++j) {
                        s.print("line\n", false);
                    }
                });
            }
            for (auto& th : threads) {
                th.join();
            }
            // destructor drains + joins
        }, kOps);
    };

    "bench_print_flush_bursts"_test = [] {
        // Correctness pre-pass: flush=true (default) prints appear promptly.
        {
            stdout_capture cap;
            expect(cap.ok());
            {
                PrintStream s;
                for (int i = 0; i < 5; ++i) {
                    s.print("f\n"); // flush = true
                }
            }
            expect(cap.wait_for("f\nf\nf\nf\nf\n"));
        }
        stdout_to_nul nul;
        expect(nul.ok());
        constexpr size_t kOps = 100000;
        kimix_bench::run("print/drain_100k_flush", [&] {
            PrintStream s;
            for (size_t i = 0; i < kOps; ++i) {
                s.print("hello\n"); // flush = true every print
            }
        }, kOps);
        kimix_bench::run("print/drain_100k_noflush", [&] {
            PrintStream s;
            for (size_t i = 0; i < kOps; ++i) {
                s.print("hello\n", false);
            }
        }, kOps);
    };

    "bench_print_drain_large_64k"_test = [] {
        const kimix::string big(65536, 'x');
        // Correctness pre-pass: two 64 KiB payloads, exact content.
        {
            stdout_capture cap;
            expect(cap.ok());
            {
                PrintStream s;
                s.print(kimix::string_view(big), false);
                s.print(kimix::string_view(big), false);
            }
            kimix::string want;
            want.reserve(big.size() * 2);
            want.append(big);
            want.append(big);
            expect(cap.wait_for(kimix::string_view(want)));
        }
        stdout_to_nul nul;
        expect(nul.ok());
        constexpr size_t kOps = 10000;
        kimix_bench::run("print/drain_10k_x_64k", [&] {
            PrintStream s;
            for (size_t i = 0; i < kOps; ++i) {
                s.print(kimix::string_view(big), false);
            }
        }, kOps, static_cast<double>(big.size()));
    };

    "bench_print_interleaved_small_large"_test = [] {
        const kimix::string big(8192, 'z');
        // Correctness pre-pass: 512 small + 1 large, exact content + order.
        {
            stdout_capture cap;
            expect(cap.ok());
            {
                PrintStream s;
                for (int i = 0; i < 513; ++i) {
                    if (i % 256 == 0) {
                        s.print(kimix::string_view(big), false);
                    } else {
                        s.print("s\n", false);
                    }
                }
            }
            kimix::string want;
            want.reserve(big.size() + 512 * 2);
            for (int i = 0; i < 513; ++i) {
                if (i % 256 == 0) {
                    want.append(big);
                } else {
                    want.append("s\n");
                }
            }
            expect(cap.wait_for(kimix::string_view(want)));
        }
        stdout_to_nul nul;
        expect(nul.ok());
        constexpr size_t kOps = 51200; // 200 large (8 KiB) + 51000 small
        kimix_bench::run("print/interleave_51k_small_200x8k", [&] {
            PrintStream s;
            for (size_t i = 0; i < kOps; ++i) {
                if (i % 256 == 0) {
                    s.print(kimix::string_view(big), false);
                } else {
                    s.print("s\n", false);
                }
            }
        }, kOps);
    };

    "bench_print_printf_large_8k"_test = [] {
        const kimix::string filler(8192, 'y');
        // Correctness pre-pass: printf-formatted 8 KiB lines, exact content.
        {
            stdout_capture cap;
            expect(cap.ok());
            {
                PrintStream s;
                for (int i = 0; i < 3; ++i) {
                    s.printf("%08d:%s\n", 7, filler.c_str());
                }
            }
            kimix::string want;
            want.reserve(3 * (filler.size() + 10));
            for (int i = 0; i < 3; ++i) {
                want.append("00000007:");
                want.append(filler);
                want.append("\n");
            }
            expect(cap.wait_for(kimix::string_view(want)));
        }
        stdout_to_nul nul;
        expect(nul.ok());
        constexpr size_t kOps = 2000; // vprint heap path (8 KiB formatted)
        kimix_bench::run("print/printf_2k_x_8k", [&] {
            PrintStream s;
            for (size_t i = 0; i < kOps; ++i) {
                s.printf("%08zu:%s\n", i, filler.c_str());
            }
        }, kOps, static_cast<double>(filler.size()));
    };
}
