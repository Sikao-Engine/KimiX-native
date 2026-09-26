// test_tool_loop_guard.cpp - Unit tests for the per-turn repeated-tool-call
// loop detectors (src/agent/tool_loop_guard.h), a port of the detector state
// machine embedded in kimi_cli's KimiToolset (soul/toolset.py:685-862,
//1039-1343): identical-argument streak reminders at 3/8/12 + force-stop at 16,
// cycle detection at 2/3 + stop at 4, different-args warnings at 15/25/35 +
// stop at 40, turn-total nudge at 60 then every 20, and the reasoning-present
// reset. All expected reminder strings are the reference's verbatim text.
//
// Framework: Boost.UT (tests/ut/ut.hpp).
#include "ut/ut.hpp"

#include <agent/tool_loop_guard.h>

using namespace boost::ut;
using namespace boost::ut::literals;

namespace {

using kimix::agent::ToolLoopGuard;

// Drives the guard the way KimiSoul::turn does: one call per step.
struct sim {
    ToolLoopGuard guard;
    kimix::vector<std::pair<kimix::string, kimix::string>> last;
    void turn() {
        guard.begin_turn();
        last.clear();
    }
    // Runs one step with the given raw (name, arguments_json) calls and
    // returns each call's verdict.
    kimix::vector<ToolLoopGuard::verdict>
    step(std::initializer_list<std::pair<const char *, const char *>> calls) {
        guard.begin_step(last);
        kimix::vector<ToolLoopGuard::verdict> out;
        for (const auto &c : calls) {
            out.push_back(guard.record_call(c.first, c.second));
        }
        last = guard.end_step();
        return out;
    }
};

constexpr const char *k_r1 =
    "\n\n<system-reminder>\n"
    "Stop repeating the same tool call with identical parameters. "
    "Try a different method or finish."
    "\n</system-reminder>";
constexpr const char *k_r3 =
    "\n\n<system-reminder>\n"
    "Dead-end loop detected. Stop all tool calls. "
    "Return a text-only summary of the problem and what is needed next."
    "\n</system-reminder>";
constexpr const char *k_cycle1 =
    "\n\n<system-reminder>\n"
    "This tool call already ran earlier this turn. "
    "Use the existing result or change approach."
    "\n</system-reminder>";

} // namespace

int main() {
    "canonical_arguments_sort_keys_recursively"_test = [] {
        using kimix::agent::loop_reminder_text_2;
        // Access canonicalization through the streak reminder: the R2 text
        // carries the canonical arguments verbatim.
        sim s;
        s.turn();
        // Steps 1..7 build the streak to 8 without reminders (3 would fire
        // otherwise); use fresh steps of the identical call.
        for (int i = 0; i < 7; ++i) {
            auto v = s.step({{"read", "{\"b\":1,\"a\":{\"y\":2,\"x\":[3,1]}}"}});
            if (i == 2) {
                expect(v[0].reminder == k_r1); // streak 3
            }
        }
        auto v = s.step({{"read", "{\"a\":{\"x\":[3,1],\"y\":2},\"b\":1}"}});
        expect(v[0].reminder.find("- arguments: {\"a\":{\"x\":[3,1],\"y\":2},\"b\":1}") !=
               kimix::string::npos);
        expect(v[0].reminder.find("- repeated_times: 8") != kimix::string::npos);
    };

    "canonical_arguments_invalid_json_falls_back_to_raw"_test = [] {
        sim s;
        s.turn();
        for (int i = 0; i < 7; ++i) {
            s.step({{"read", "{not json"}});
        }
        auto v = s.step({{"read", "{not json"}});
        const size_t pos = v[0].reminder.find("- arguments: {not json");
        expect(pos != kimix::string::npos);
    };

    "identical_streak_graded_reminders_and_force_stop"_test = [] {
        sim s;
        s.turn();
        auto v = s.step({{"read", "{\"x\":1}"}});
        expect(v[0].reminder.empty());
        v = s.step({{"read", "{\"x\":1}"}});
        expect(v[0].reminder.empty()); // streak 2: below the first threshold
        v = s.step({{"read", "{\"x\":1}"}});
        expect(v[0].reminder == k_r1); // streak 3
        expect(!v[0].force_stop);
        for (int i = 4; i <= 7; ++i) {
            v = s.step({{"read", "{\"x\":1}"}});
            expect(v[0].reminder == k_r1);
        }
        v = s.step({{"read", "{\"x\":1}"}}); // streak 8: R2 with details
        expect(v[0].reminder.find("Repeated identical call:") != kimix::string::npos);
        expect(v[0].reminder.find("- tool: read") != kimix::string::npos);
        expect(v[0].reminder.find("- repeated_times: 8") != kimix::string::npos);
        expect(!v[0].force_stop);
        for (int i = 9; i <= 11; ++i) {
            v = s.step({{"read", "{\"x\":1}"}});
            expect(v[0].reminder.find("- repeated_times: 8") != kimix::string::npos ||
                   v[0].reminder.find("Repeated identical call:") != kimix::string::npos);
        }
        v = s.step({{"read", "{\"x\":1}"}}); // streak 12: R3
        expect(v[0].reminder == k_r3);
        expect(!v[0].force_stop);
        for (int i = 13; i <= 15; ++i) {
            v = s.step({{"read", "{\"x\":1}"}});
            expect(v[0].reminder == k_r3);
        }
        v = s.step({{"read", "{\"x\":1}"}}); // streak 16: force stop
        expect(v[0].reminder == k_r3);
        expect(v[0].force_stop);
        expect(s.guard.force_stop_turn());
        expect(s.guard.force_stop_reason() == "adjacent-repeat");
        expect(s.guard.force_stop_tool() == "read");
    };

    "cycle_detection_reminds_and_stops"_test = [] {
        sim s;
        s.turn();
        s.step({{"A", "{\"x\":1}"}, {"B", "{\"x\":1}"}, {"C", "{\"x\":1}"}});
        // Second cycle: every call is a cross-step duplicate with cycle_count 2.
        auto v = s.step({{"A", "{\"x\":1}"}, {"B", "{\"x\":1}"}, {"C", "{\"x\":1}"}});
        expect(v[0].reminder == k_cycle1);
        expect(v[1].reminder == k_cycle1);
        expect(v[2].reminder == k_cycle1);
        expect(!v[0].force_stop);
        // Third cycle: cycle_count 3 -> the stronger per-tool text.
        v = s.step({{"A", "{\"x\":1}"}, {"B", "{\"x\":1}"}, {"C", "{\"x\":1}"}});
        expect(v[0].reminder.find("'A' repeated 3 times in a cycle. Stop using these "
                                  "arguments or finish.") != kimix::string::npos);
        // Fourth cycle: force stop at cycle_count 4.
        v = s.step({{"A", "{\"x\":1}"}, {"B", "{\"x\":1}"}, {"C", "{\"x\":1}"}});
        expect(v[0].force_stop);
        expect(s.guard.force_stop_reason() == "cycle-repeat");
    };

    "different_args_graded_warnings_and_hard_stop"_test = [] {
        sim s;
        s.turn();
        kimix::vector<ToolLoopGuard::verdict> v;
        for (int i = 1; i <= 14; ++i) {
            const kimix::string args = kimix::format("{{\"x\":{}}}", i);
            v = s.step({{"read", args.c_str()}});
            expect(v[0].reminder.empty());
            expect(!v[0].force_stop);
        }
        v = s.step({{"read", "{\"x\":15}"}});
        expect(v[0].reminder ==
               "\n\n<system-reminder>\n"
               "Same tool called repeatedly with different args. Change approach or finish."
               "\n</system-reminder>");
        expect(!v[0].force_stop);
        for (int i = 16; i <= 24; ++i) {
            v = s.step({{"read", kimix::format("{{\"x\":{}}}", i).c_str()}});
            expect(v[0].reminder.empty());
        }
        v = s.step({{"read", "{\"x\":25}"}});
        expect(v[0].reminder ==
               "\n\n<system-reminder>\n"
               "'read' called 25 times with different args. Stop or finish."
               "\n</system-reminder>");
        for (int i = 26; i <= 34; ++i) {
            v = s.step({{"read", kimix::format("{{\"x\":{}}}", i).c_str()}});
            expect(v[0].reminder.empty());
        }
        v = s.step({{"read", "{\"x\":35}"}});
        expect(v[0].reminder ==
               "\n\n<system-reminder>\n"
               "'read' called 35 times. Stop now. Change approach or finish."
               "\n</system-reminder>");
        for (int i = 36; i <= 39; ++i) {
            v = s.step({{"read", kimix::format("{{\"x\":{}}}", i).c_str()}});
            expect(v[0].reminder.empty());
        }
        v = s.step({{"read", "{\"x\":40}"}});
        expect(v[0].force_stop);
        expect(s.guard.force_stop_reason() == "different-args-overuse");
    };

    "turn_total_reminder_at_60_then_every_20"_test = [] {
        sim s;
        s.turn();
        // Unique tool names keep every other detector silent.
        for (int i = 1; i <= 59; ++i) {
      const kimix::string name = kimix::format("tool{}", i);
      auto v = s.step({{name.c_str(), "{}"}});
            expect(v[0].reminder.empty());
        }
        auto v = s.step({{"tool60", "{}"}});
        expect(v[0].reminder ==
               "\n\n<system-reminder>\n"
               "Tool calls repeat 60 times. Stop or finish."
               "\n</system-reminder>");
        for (int i = 61; i <= 79; ++i) {
            const kimix::string name = kimix::format("tool{}", i);
            v = s.step({{name.c_str(), "{}"}});
            expect(v[0].reminder.empty());
        }
        v = s.step({{"tool80", "{}"}});
        expect(v[0].reminder ==
               "\n\n<system-reminder>\n"
               "Tool calls repeat 80 times. Stop or finish."
               "\n</system-reminder>");
    };

    "reset_loop_detectors_clears_counters_and_trip"_test = [] {
        sim s;
        s.turn();
        for (int i = 0; i < 15; ++i) {
            s.step({{"read", "{\"x\":1}"}});
        }
        s.guard.reset_loop_detectors();
        expect(!s.guard.force_stop_turn());
        // The same call again starts from a clean slate: no reminder until 3.
        auto v = s.step({{"read", "{\"x\":1}"}});
        expect(v[0].reminder.empty());
        expect(s.guard.turn_total_calls() == 1);
    };

    "fresh_begin_step_clears_cycle_counts"_test = [] {
        sim s;
        s.turn();
        s.step({{"A", "{\"x\":1}"}, {"B", "{\"x\":1}"}});
        s.step({{"A", "{\"x\":1}"}, {"B", "{\"x\":1}"}}); // cycle_count 2
        // A retried step with no previous calls restarts cycle counting.
        s.last.clear();
        auto v = s.step({{"A", "{\"x\":1}"}});
        expect(v[0].reminder.empty());
        expect(!v[0].force_stop);
    };

    return 0;
}
