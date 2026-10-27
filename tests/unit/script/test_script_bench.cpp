// Benchmark for the Lua script sandbox (src/script, target kimix-script).
//
// Gated: only runs when the environment variable KIMIX_SCRIPT_BENCH=1 is set, so
// `xmake test` stays fast (without it the binary registers nothing and exits).
// Run with:
//   xmake build test_script_bench
//   set KIMIX_SCRIPT_BENCH=1 && xmake run test_script_bench
//
// What it measures (all numbers printed, no timing assertions -- a slow machine
// must not fail the suite):
// - fresh vs reused state: the ~13 % premium the per-run lua_State decision
//   costs (plan §3, measured 84 us vs 73 us for a 500-element chunk).
// - count-hook granularity 1e3 / 1e4 / 1e5 over the same loop, i.e. what the
//   deadline watchdog costs per instruction.
// - marshalling cost for a 10 000-element array result (canonical JSON).
// - the denial path: how expensive a tripwire + sticky re-raise is, which is the
//   path a hostile script hammers.
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include <script/script_sandbox.h>

#include "script_test_util.h"

using kimix::script::Limits;
using kimix::script::Sandbox;
using kimix::script::Status;

namespace tests_unit_script_test_script_bench {

template <typename Body>
double seconds_for(Body &&body) {
    const auto started = std::chrono::steady_clock::now();
    body();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

/* A chunk with a 500-element table built and walked -- the plan's cost baseline. */
const char *const kBaselineChunk =
    "local t = {}\n"
    "for i = 1, 500 do t[i] = i * 2 end\n"
    "local sum = 0\n"
    "for i = 1, #t do sum = sum + t[i] end\n"
    "return sum\n";

void report(const char *label, double total_seconds, int iterations) {
    const double per_run_us = (total_seconds / static_cast<double>(iterations)) * 1e6;
    std::printf("%-42s %9.2f us/run  (%d runs, %.3f s total)\n", label, per_run_us, iterations,
                total_seconds);
}

} // namespace tests_unit_script_test_script_bench
using namespace tests_unit_script_test_script_bench;

int main() {
    const char *gate = std::getenv("KIMIX_SCRIPT_BENCH");
    if (gate == nullptr || gate[0] == '0') {
        std::printf("test_script_bench: set KIMIX_SCRIPT_BENCH=1 to run the benchmark\n");
        return 0;
    }

    constexpr int kRuns = 200;

    /* 1. fresh state per run (the default) vs a reused state. */
    {
        Limits limits;
        Sandbox sb(limits);
        double total = 0.0;
        for (int i = 0; i < kRuns; ++i) {
            total += seconds_for([&] {
                auto r = sb.run(kBaselineChunk);
                if (!r.ok()) {
                    std::printf("unexpected failure: %s\n", r.message.c_str());
                }
            });
        }
        report("fresh state per run (default)", total, kRuns);
    }
    {
        Limits limits;
        limits.fresh_state_per_run = false;
        Sandbox sb(limits);
        double total = 0.0;
        for (int i = 0; i < kRuns; ++i) {
            total += seconds_for([&] {
                auto r = sb.run(kBaselineChunk);
                if (!r.ok()) {
                    std::printf("unexpected failure: %s\n", r.message.c_str());
                }
            });
        }
        report("reused state (fresh_state_per_run=false)", total, kRuns);
    }

    /* 2. hook granularity: the same 300 000-iteration loop at 1e3 / 1e4 / 1e5. */
    {
        static const uint32_t kGranularities[] = {1000, 10000, 100000};
        for (uint32_t granularity : kGranularities) {
            Limits limits;
            limits.max_seconds = 30.0;
            limits.hook_instruction_granularity = granularity;
            Sandbox sb(limits);
            const double total = seconds_for([&] {
                auto r = sb.run("local s = 0 for i = 1, 300000 do s = s + i end return s");
                if (!r.ok()) {
                    std::printf("unexpected failure: %s\n", r.message.c_str());
                }
            });
            char label[96];
            std::snprintf(label, sizeof(label), "300k-iteration loop, hook every %u instructions", granularity);
            report(label, total, 1);
        }
    }

    /* 3. marshalling a 10 000-element array into canonical JSON. */
    {
        Limits limits;
        limits.max_result_bytes = 4 * 1024 * 1024;
        Sandbox sb(limits);
        const double total = seconds_for([&] {
            auto r = sb.run("local t = {} for i = 1, 10000 do t[i] = i end return t");
            if (!r.ok()) {
                std::printf("unexpected failure: %s\n", r.message.c_str());
            } else if (!r.results.empty()) {
                std::printf("  10k-element JSON result: %zu bytes\n", r.results[0].s.size());
            }
        });
        report("marshal a 10 000-element array", total, 1);
    }

    /* 4. the denial path: one tripwire + sticky re-raise per iteration. */
    {
        constexpr int kDenials = 2000;
        Sandbox sb;
        const double total = seconds_for([&] {
            for (int i = 0; i < kDenials; ++i) {
                auto r = sb.run("return io.open('x')");
                if (r.status != Status::denied) {
                    std::printf("expected denied, got %s\n", kimix::script::status_name(r.status));
                }
            }
        });
        report("denial path (io.open + abort)", total, kDenials);
    }

    /* 5. a denial that a script keeps retrying inside pcall: the loop must end. */
    {
        Limits limits;
        limits.max_seconds = 5.0;
        Sandbox sb(limits);
        const double total = seconds_for([&] {
            auto r = sb.run("while true do pcall(function() return io.open('x') end) end");
            if (r.status != Status::denied) {
                std::printf("expected denied, got %s\n", kimix::script::status_name(r.status));
            }
        });
        std::printf("%-42s %9.3f s\n", "unswallowable denial loop (bounded)", total);
    }
    return 0;
}
