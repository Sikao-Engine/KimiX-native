// agent/tool_loop_guard.h - Per-turn repeated-tool-call loop detectors.
//
// Port of the detector state machine embedded in kimi_cli's KimiToolset
// (soul/toolset.py:685-862 init/reset/begin_step/end_step/handle detector
// sections): four detectors watch every tool call of a turn and emit graded
// <system-reminder> texts appended to the tool result the model sees:
//
//   1. identical-argument consecutive streak (cross-step): soft reminders at
//      3 / 8 / 12 calls, force-stop at 16 ("adjacent-repeat");
//   2. cycle-aware per-call-key counting: the exact (tool, canonical args)
//      pair reappearing anywhere in the turn (reminders at 2 / 3, force-stop
//      at 4, "cycle-repeat") - catches A(x) -> B(x) -> C(x) -> A(x) cycles the
//      consecutive streak cannot see;
//   3. different-arguments per-tool counting: graded warnings at 15 / 25 / 35
//      calls of the same tool, force-stop at 40 ("different-args-overuse");
//   4. turn-total soft nudge at 60 calls, then every 20.
//
// A force-stop raises force_stop_turn(); the soul maps it to the reference's
// stop_reason "tool_call_repeat" and runs the loop-recovery gate
// (kimisoul.py:1378-1439, soul.cpp).  Every reminder string below is the
// reference's verbatim text.
//
// A3 (kimisoul.py:227-244,1931-1935): a step whose assistant response carries
// non-empty reasoning resets ALL detectors via reset_loop_detectors() before
// the outcome checks - thinking between tool calls means progress.
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers.

#pragma once

#include <cstdint>
#include <utility>

#include <core/kimix_core.h>

namespace kimix::agent {

// Reference thresholds (toolset.py:713-726,771-781), named as in Python.
inline constexpr int32_t kRepeatReminder1Start = 3;     // _REPEAT_REMINDER_1_START
inline constexpr int32_t kRepeatReminder2Start = 8;     // _REPEAT_REMINDER_2_START
inline constexpr int32_t kRepeatReminder3Start = 12;    // _REPEAT_REMINDER_3_START
inline constexpr int32_t kRepeatForceStopStreak = 16;   // _REPEAT_FORCE_STOP_STREAK
inline constexpr int32_t kDiffArgsHardStopStart = 40;   // _DIFF_ARGS_HARD_STOP_START
inline constexpr int32_t kDiffArgsWarnThresholds[] = {15, 25, 35};
inline constexpr int32_t kTurnTotalReminderStart = 60;   // _TURN_TOTAL_REMINDER_START
inline constexpr int32_t kTurnTotalReminderInterval = 20; // _TURN_TOTAL_REMINDER_INTERVAL
inline constexpr int32_t kCycleReminderStart = 2;        // _CYCLE_REMINDER_START
inline constexpr int32_t kCycleReminder2Start = 3;       // _CYCLE_REMINDER_2_START
inline constexpr int32_t kCycleForceStop = 4;            // _CYCLE_FORCE_STOP

class ToolLoopGuard {
public:
    // One detector decision for a single tool call: the reminder text to
    // append to the tool result ("" when none) and whether the turn must be
    // force-stopped.
    struct verdict {
        kimix::string reminder;
        bool force_stop = false;
    };

    ToolLoopGuard() = default;

    // Per-turn reset (KimiToolset's turn-id change in begin_step,
    // toolset.py:1070-1076): clears the per-tool different-args counters, the
    // cycle counts, the turn total and any trip state. The soul calls this at
    // the start of every turn().
    void begin_turn() noexcept;

    // Called before each step with the previous step's tool calls as
    // (tool_name, raw_arguments_json) pairs (toolset.py:1039-1096). An empty
    // list means a fresh call history (turn start / retried step): the
    // seen-set, consecutive streak and cycle counts restart from it. Also
    // clears any previous force-stop trip - the reference re-arms the toolset
    // at every begin_step.
    void begin_step(kimix::span<
                    const std::pair<kimix::string, kimix::string>> previous_calls);

    // Feed one tool call (about to be / just) executed. Canonicalizes the
    // arguments (toolset.py:838-862), advances every detector and returns the
    // merged reminder text (repeat/cycle first, then different-args, then
    // turn-total - toolset.py:1337-1343) plus the force-stop flag.
    verdict record_call(kimix::string_view tool_name,
                        kimix::string_view arguments_json);

    // Called after the step's tool results are in: advances the consecutive
    // streak with this step's calls and returns them (the soul hands them to
    // the next begin_step; toolset.py:1098-1104).
    kimix::vector<std::pair<kimix::string, kimix::string>> end_step();

    // A3 reset (toolset.py:1106-1134): clears consecutive/cycle/diff-args/
    // turn-total counters AND any trip state, but NOT the per-step call
    // windows (begin_step/end_step own those). The soul calls this when the
    // step's assistant response carried non-empty reasoning, BEFORE
    // consulting force_stop_turn().
    void reset_loop_detectors() noexcept;

    bool force_stop_turn() const noexcept { return _force_stop_turn; }
    // One of "adjacent-repeat" / "cycle-repeat" / "different-args-overuse";
    // "" when no trip is set.
    kimix::string_view force_stop_reason() const noexcept { return _force_stop_reason; }
    // The tool name of the tripping call key; "" when no trip is set.
    kimix::string_view force_stop_tool() const noexcept { return _force_stop_tool; }
    bool dedup_triggered() const noexcept { return _dedup_triggered; }
    int32_t turn_total_calls() const noexcept { return _turn_total_calls; }

private:
    struct call_key {
        kimix::string tool;
        kimix::string args;
        bool operator==(const call_key &other) const noexcept {
            return tool == other.tool && args == other.args;
        }
        bool operator<(const call_key &other) const noexcept {
            return tool < other.tool || (tool == other.tool && args < other.args);
        }
    };

    void mark_force_stop(kimix::string_view reason, const call_key &key);
    void advance_consecutive_streak(
        kimix::span<const std::pair<kimix::string, kimix::string>> calls);
    int32_t projected_streak_for_call(size_t call_index) const noexcept;
    verdict build_repeat_reminder(int32_t streak, kimix::string_view tool_name,
                                  kimix::string_view canonical_args) const;

    // Canonicalized JSON arguments (toolset.py:838-862): recursively
    // key-sorted, minified; the raw string when the arguments are not valid
    // JSON (_canonical_tool_arguments_text's fallback).
    static kimix::string canonical_tool_arguments(kimix::string_view arguments_json);
    static call_key normalize_call_key(kimix::string_view tool_name,
                                       kimix::string_view arguments_json);

    // Per-step windows (toolset.py:914-916).
    kimix::vector<call_key> _previous_step_calls;
    kimix::vector<call_key> _current_step_calls;
    bool _step_closed = false;
    bool _dedup_triggered = false;

    // Trip state (toolset.py:922-924,1179-1182).
    bool _force_stop_turn = false;
    kimix::string _force_stop_reason;
    kimix::string _force_stop_tool;
    kimix::string _force_stop_args;

    // Turn-scoped counters (toolset.py:925-935).
    kimix::set<call_key> _seen_call_keys;
    kimix::optional<call_key> _consecutive_key;
    int32_t _consecutive_count = 0;
    int32_t _turn_total_calls = 0;
    kimix::map<call_key, int32_t> _call_key_counts; // cycle-aware
    kimix::map<kimix::string, int32_t> _tool_call_counts;   // per-tool, diff-args
    kimix::map<kimix::string, kimix::set<int32_t>> _tool_warned_at;
    bool _turn_tool_warning_issued = false;
};

// ── Reference reminder texts (toolset.py:685-734,783-823), verbatim ─────────

kimix::string_view loop_reminder_text_1() noexcept;   // _REMINDER_TEXT_1
kimix::string loop_reminder_text_2(kimix::string_view tool_name, // _make_reminder_text_2
                                   int32_t repeat_count,
                                   kimix::string_view canonical_args);
kimix::string_view loop_reminder_text_3() noexcept;   // _REMINDER_TEXT_3
kimix::string loop_turn_total_reminder(int32_t total_calls); // _make_turn_total_reminder
kimix::string_view loop_cycle_reminder_text() noexcept;      // _CYCLE_REMINDER_TEXT
kimix::string loop_cycle_reminder_text_2(kimix::string_view tool_name, // _make_cycle_reminder_text_2
                                         int32_t cycle_count);
kimix::string_view loop_diff_args_reminder_text_1() noexcept; // _DIFF_ARGS_REMINDER_TEXT_1
kimix::string loop_diff_args_reminder_text_2(kimix::string_view tool_name, // _make_diff_args_reminder_text_2
                                             int32_t call_count);
kimix::string loop_diff_args_reminder_text_3(kimix::string_view tool_name, // _make_diff_args_reminder_text_3
                                             int32_t call_count);

} // namespace kimix::agent
