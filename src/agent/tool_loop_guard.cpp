// agent/tool_loop_guard.cpp - ToolLoopGuard implementation (see tool_loop_guard.h).
//
// Detector logic is a 1:1 port of kimi_cli/soul/toolset.py's KimiToolset
// detector sections (init at 914-935, begin_step 1039-1096, end_step
// 1098-1104, reset_loop_detectors 1106-1134, _projected_streak_for_call
// 1144-1153, handle() detector block 1253-1343). Every <system-reminder>
// string is the reference's verbatim text.

#include "agent/tool_loop_guard.h"

#include <algorithm>
#include <cstring>

#include <llm/yyjson_alc.h>
#include "yyjson.h"

namespace kimix::agent {

// ── Reference reminder texts (verbatim from toolset.py) ─────────────────────

kimix::string_view loop_reminder_text_1() noexcept {
    return "\n\n<system-reminder>\n"
           "Stop repeating the same tool call with identical parameters. "
           "Try a different method or finish."
           "\n</system-reminder>";
}

kimix::string loop_reminder_text_2(kimix::string_view tool_name,
                                   int32_t repeat_count,
                                   kimix::string_view canonical_args) {
    kimix::string out = "\n\n<system-reminder>\n"
                        "Repeated identical call:\n- tool: ";
    out.append(tool_name.data(), tool_name.size());
    out += "\n- repeated_times: ";
    out += std::to_string(repeat_count);
    out += "\n- arguments: ";
    out.append(canonical_args.data(), canonical_args.size());
    out += "\nStop repeating. Choose a different action or finish."
           "\n</system-reminder>";
    return out;
}

kimix::string_view loop_reminder_text_3() noexcept {
    return "\n\n<system-reminder>\n"
           "Dead-end loop detected. Stop all tool calls. "
           "Return a text-only summary of the problem and what is needed next."
           "\n</system-reminder>";
}

kimix::string loop_turn_total_reminder(int32_t total_calls) {
    kimix::string out = "\n\n<system-reminder>\nTool calls repeat ";
    out += std::to_string(total_calls);
    out += " times. Stop or finish."
           "\n</system-reminder>";
    return out;
}

kimix::string_view loop_cycle_reminder_text() noexcept {
    return "\n\n<system-reminder>\n"
           "This tool call already ran earlier this turn. "
           "Use the existing result or change approach."
           "\n</system-reminder>";
}

kimix::string loop_cycle_reminder_text_2(kimix::string_view tool_name,
                                         int32_t cycle_count) {
    kimix::string out = "\n\n<system-reminder>\n'";
    out.append(tool_name.data(), tool_name.size());
    out += "' repeated ";
    out += std::to_string(cycle_count);
    out += " times in a cycle. Stop using these arguments or finish."
           "\n</system-reminder>";
    return out;
}

kimix::string_view loop_diff_args_reminder_text_1() noexcept {
    return "\n\n<system-reminder>\n"
           "Same tool called repeatedly with different args. Change approach or finish."
           "\n</system-reminder>";
}

kimix::string loop_diff_args_reminder_text_2(kimix::string_view tool_name,
                                             int32_t call_count) {
    kimix::string out = "\n\n<system-reminder>\n'";
    out.append(tool_name.data(), tool_name.size());
    out += "' called ";
    out += std::to_string(call_count);
    out += " times with different args. Stop or finish."
           "\n</system-reminder>";
    return out;
}

kimix::string loop_diff_args_reminder_text_3(kimix::string_view tool_name,
                                             int32_t call_count) {
    kimix::string out = "\n\n<system-reminder>\n'";
    out.append(tool_name.data(), tool_name.size());
    out += "' called ";
    out += std::to_string(call_count);
    out += " times. Stop now. Change approach or finish."
           "\n</system-reminder>";
    return out;
}

namespace {

// _make_diff_args_reminder (toolset.py:826-835): progressively stronger
// warnings based on the call count.
kimix::string make_diff_args_reminder(kimix::string_view tool_name,
                                      int32_t call_count) {
    if (call_count >= kDiffArgsHardStopStart) {
        return loop_diff_args_reminder_text_3(tool_name, call_count);
    }
    if (call_count <= kDiffArgsWarnThresholds[0]) {
        return kimix::string(loop_diff_args_reminder_text_1());
    }
    if (call_count <= kDiffArgsWarnThresholds[1]) {
        return loop_diff_args_reminder_text_2(tool_name, call_count);
    }
    return loop_diff_args_reminder_text_3(tool_name, call_count);
}

// Deep-copy `src` into `doc` with every object's keys sorted byte-wise
// (_sort_json_value, toolset.py:838-841). UTF-8 byte order equals Python's
// code-point order, so the canonical forms match orjson's sorted dumps.
yyjson_mut_val *mut_copy_sorted(yyjson_mut_doc *doc, yyjson_val *src) {
    switch (yyjson_get_type(src)) {
    case YYJSON_TYPE_OBJ: {
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        kimix::vector<std::pair<kimix::string, yyjson_val *>> pairs;
        size_t idx = 0;
        size_t max = 0;
        yyjson_val *key = nullptr;
        yyjson_val *val = nullptr;
        yyjson_obj_foreach(src, idx, max, key, val) {
            pairs.emplace_back(kimix::string(yyjson_get_str(key),
                                             yyjson_get_len(key)),
                               val);
        }
        std::sort(pairs.begin(), pairs.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });
        for (const auto &p : pairs) {
            yyjson_mut_obj_add(obj, yyjson_mut_strncpy(doc, p.first.data(),
                                                        p.first.size()),
                               mut_copy_sorted(doc, p.second));
        }
        return obj;
    }
    case YYJSON_TYPE_ARR: {
        yyjson_mut_val *arr = yyjson_mut_arr(doc);
        size_t idx = 0;
        size_t max = 0;
        yyjson_val *item = nullptr;
        yyjson_arr_foreach(src, idx, max, item) {
            yyjson_mut_arr_add_val(arr, mut_copy_sorted(doc, item));
        }
        return arr;
    }
    default:
        // Scalars (this fork's yyjson_val_mut_copy only supports object/array
        // roots, so every leaf is copied explicitly).
        if (yyjson_is_str(src)) {
            return yyjson_mut_strncpy(doc, yyjson_get_str(src),
                                      yyjson_get_len(src));
        }
        if (yyjson_is_uint(src)) {
            return yyjson_mut_uint(doc, yyjson_get_uint(src));
        }
        if (yyjson_is_sint(src)) {
            return yyjson_mut_sint(doc, yyjson_get_sint(src));
        }
        if (yyjson_is_real(src)) {
            return yyjson_mut_real(doc, yyjson_get_real(src));
        }
        if (yyjson_is_bool(src)) {
            return yyjson_mut_bool(doc, yyjson_get_bool(src));
        }
        return yyjson_mut_null(doc);
    }
}

} // namespace

kimix::string
ToolLoopGuard::canonical_tool_arguments(kimix::string_view arguments_json) {
    if (arguments_json.empty()) {
        return kimix::string();
    }
    kimix::string buf(arguments_json);
    yyjson_doc *doc = yyjson_read_opts(buf.data(), buf.size(), 0,
                                       &kimix::llm::kYYJsonAlcMi, nullptr);
    if (doc == nullptr) {
        // _canonical_tool_arguments_text's JSONDecodeError fallback: the
        // arguments string itself is the canonical form.
        return kimix::string(arguments_json);
    }
    yyjson_mut_doc *mut = yyjson_mut_doc_new(&kimix::llm::kYYJsonAlcMi);
    yyjson_mut_val *sorted = mut_copy_sorted(mut, yyjson_doc_get_root(doc));
    yyjson_mut_doc_set_root(mut, sorted);
    size_t len = 0;
    char *text = yyjson_mut_write_opts(mut, 0, &kimix::llm::kYYJsonAlcMi,
                                       &len, nullptr);
    kimix::string out;
    if (text != nullptr) {
        out.assign(text, len);
        mi_free(text);
    }
    yyjson_mut_doc_free(mut);
    yyjson_doc_free(doc);
    return out;
}

ToolLoopGuard::call_key
ToolLoopGuard::normalize_call_key(kimix::string_view tool_name,
                                  kimix::string_view arguments_json) {
    call_key key;
    key.tool.assign(tool_name.data(), tool_name.size());
    key.args = canonical_tool_arguments(arguments_json);
    return key;
}

void ToolLoopGuard::begin_turn() noexcept {
    // toolset.py:1070-1076 (turn-id change) plus a full fresh start: the
    // previous turn's calls are no longer relevant to anything.
    _previous_step_calls.clear();
    _current_step_calls.clear();
    _step_closed = false;
    _dedup_triggered = false;
    _seen_call_keys.clear();
    _consecutive_key.reset();
    _consecutive_count = 0;
    _call_key_counts.clear();
    _tool_call_counts.clear();
    _tool_warned_at.clear();
    _turn_total_calls = 0;
    _turn_tool_warning_issued = false;
    _force_stop_turn = false;
    _force_stop_reason.clear();
    _force_stop_tool.clear();
    _force_stop_args.clear();
}

void ToolLoopGuard::begin_step(kimix::span<
                               const std::pair<kimix::string, kimix::string>>
                                   previous_calls) {
    // toolset.py:1058-1096.
    _previous_step_calls.clear();
    _previous_step_calls.reserve(previous_calls.size());
    for (const auto &p : previous_calls) {
        _previous_step_calls.push_back(normalize_call_key(p.first, p.second));
    }
    _current_step_calls.clear();
    _step_closed = false;
    _dedup_triggered = false;
    // The force-stop trip is per-step in the reference: begin_step clears it
    // (the soul consults force_stop_turn between end_step and the next
    // begin_step, i.e. inside outcome resolution).
    _force_stop_turn = false;
    _force_stop_reason.clear();
    _force_stop_tool.clear();
    _force_stop_args.clear();
    _turn_tool_warning_issued = false; // per-step (toolset.py:1076)

    if (_previous_step_calls.empty()) {
        _seen_call_keys.clear();
        _consecutive_key.reset();
        _consecutive_count = 0;
        // Fresh call history (turn start, a retried step, or a
        // back-to-the-future revert): cycle counting restarts with it.
        _call_key_counts.clear();
    } else {
        for (const call_key &key : _previous_step_calls) {
            _seen_call_keys.insert(key);
        }
        if (!_consecutive_key.has_value() && _consecutive_count == 0) {
            advance_consecutive_streak(previous_calls);
        }
    }
}

kimix::vector<std::pair<kimix::string, kimix::string>>
ToolLoopGuard::end_step() {
    // toolset.py:1098-1104.
    if (!_step_closed) {
        kimix::vector<std::pair<kimix::string, kimix::string>> current;
        current.reserve(_current_step_calls.size());
        for (const call_key &key : _current_step_calls) {
            current.emplace_back(key.tool, key.args);
        }
        advance_consecutive_streak(current);
        for (const call_key &key : _current_step_calls) {
            _seen_call_keys.insert(key);
        }
        _step_closed = true;
    }
    kimix::vector<std::pair<kimix::string, kimix::string>> out;
    out.reserve(_current_step_calls.size());
    for (const call_key &key : _current_step_calls) {
        out.emplace_back(key.tool, key.args);
    }
    return out;
}

void ToolLoopGuard::reset_loop_detectors() noexcept {
    // toolset.py:1106-1134: clears counters AND trip state, but NOT the
    // per-step call windows (_previous/_current_step_calls - begin_step/
    // end_step own those) and NOT _dedup_triggered (per-step flag).
    _consecutive_key.reset();
    _consecutive_count = 0;
    _seen_call_keys.clear();
    _call_key_counts.clear();
    _tool_call_counts.clear();
    _tool_warned_at.clear();
    _turn_total_calls = 0;
    _force_stop_turn = false;
    _force_stop_reason.clear();
    _force_stop_tool.clear();
    _force_stop_args.clear();
}

void ToolLoopGuard::advance_consecutive_streak(kimix::span<
    const std::pair<kimix::string, kimix::string>> calls) {
    for (const auto &p : calls) {
        const call_key key = normalize_call_key(p.first, p.second);
        if (_consecutive_key.has_value() && key == *_consecutive_key) {
            ++_consecutive_count;
        } else {
            _consecutive_key = key;
            _consecutive_count = 1;
        }
    }
}

int32_t ToolLoopGuard::projected_streak_for_call(size_t call_index) const noexcept {
    // toolset.py:1144-1153: replay this step's calls up to `call_index` on
    // top of the carried-over consecutive state.
    kimix::optional<call_key> key = _consecutive_key;
    int32_t count = _consecutive_count;
    for (size_t i = 0; i <= call_index && i < _current_step_calls.size(); ++i) {
        if (key.has_value() && _current_step_calls[i] == *key) {
            ++count;
        } else {
            key = _current_step_calls[i];
            count = 1;
        }
    }
    return count;
}

void ToolLoopGuard::mark_force_stop(kimix::string_view reason,
                                    const call_key &key) {
    _force_stop_turn = true;
    _force_stop_reason.assign(reason.data(), reason.size());
    _force_stop_tool = key.tool;
    _force_stop_args = key.args;
}

ToolLoopGuard::verdict
ToolLoopGuard::build_repeat_reminder(int32_t streak,
                                     kimix::string_view tool_name,
                                     kimix::string_view canonical_args) const {
    // _build_repeat_reminder (toolset.py:753-763).
    verdict v;
    if (streak >= kRepeatForceStopStreak) {
        v.force_stop = true;
        v.reminder.assign(loop_reminder_text_3());
        return v;
    }
    if (streak >= kRepeatReminder3Start) {
        v.reminder.assign(loop_reminder_text_3());
        return v;
    }
    if (streak >= kRepeatReminder2Start) {
        v.reminder = loop_reminder_text_2(tool_name, streak, canonical_args);
        return v;
    }
    if (streak >= kRepeatReminder1Start) {
        v.reminder.assign(loop_reminder_text_1());
        return v;
    }
    return v;
}

ToolLoopGuard::verdict
ToolLoopGuard::record_call(kimix::string_view tool_name,
                           kimix::string_view arguments_json) {
    const call_key key = normalize_call_key(tool_name, arguments_json);
    kimix::string canonical_args = key.args;
    const size_t call_index = _current_step_calls.size();
    _current_step_calls.push_back(key);
    ++_turn_total_calls;

    // Per-tool different-args call counting (relaxed limitation).
    const int32_t call_count = ++_tool_call_counts[key.tool];

    // NOTE(port): the reference's same-step async duplicate short-circuit
    // (toolset.py:1264-1274) is intentionally not ported - the native loop
    // executes tool calls serially after the full response (gap row 25); a
    // same-step duplicate still counts as work here, exactly like any other
    // call.

    // Cycle-aware per-call-key counting (toolset.py:1276-1280): counted for
    // every call that actually executes.
    const int32_t cycle_count = ++_call_key_counts[key];

    const bool is_cross_step_dup = _seen_call_keys.count(key) != 0;
    verdict repeat;
    const int32_t repeat_count =
        static_cast<int32_t>(projected_streak_for_call(call_index));
    kimix::string reminder_text;
    if (is_cross_step_dup) {
        repeat = build_repeat_reminder(repeat_count, tool_name, canonical_args);
        _dedup_triggered = true;
        reminder_text = std::move(repeat.reminder);
        if (repeat.force_stop) {
            mark_force_stop("adjacent-repeat", key);
        }
    }

    // Cycle-aware repeat punishment (toolset.py:1293-1310).
    if (cycle_count >= kCycleReminderStart && repeat_count <= 1) {
        kimix::string cycle_reminder =
            cycle_count >= kCycleReminder2Start
                ? loop_cycle_reminder_text_2(tool_name, cycle_count)
                : kimix::string(loop_cycle_reminder_text());
        reminder_text += cycle_reminder;
        if (cycle_count >= kCycleForceStop) {
            mark_force_stop("cycle-repeat", key);
        }
    }

    // Different-args per-tool overuse check (toolset.py:1312-1319).
    kimix::string diff_args_reminder_text;
    if (!is_cross_step_dup &&
        (call_count == kDiffArgsWarnThresholds[0] ||
         call_count == kDiffArgsWarnThresholds[1] ||
         call_count == kDiffArgsWarnThresholds[2])) {
        kimix::set<int32_t> &warned_at = _tool_warned_at[key.tool];
        if (warned_at.count(call_count) == 0 && !_turn_tool_warning_issued) {
            warned_at.insert(call_count);
            _turn_tool_warning_issued = true;
            diff_args_reminder_text = make_diff_args_reminder(tool_name, call_count);
        }
    }

    // Hard per-tool call ceiling (different args) (toolset.py:1321-1324).
    if (!is_cross_step_dup && call_count >= kDiffArgsHardStopStart) {
        mark_force_stop("different-args-overuse", key);
    }

    // Soft nudge for very long turns (toolset.py:1326-1335).
    kimix::string turn_total_reminder_text;
    if (_turn_total_calls >= kTurnTotalReminderStart &&
        (_turn_total_calls == kTurnTotalReminderStart ||
         (_turn_total_calls - kTurnTotalReminderStart) % kTurnTotalReminderInterval ==
             0)) {
        turn_total_reminder_text = loop_turn_total_reminder(_turn_total_calls);
    }

    // Merge all reminder texts in the reference's order (toolset.py:1337-1343).
    verdict out;
    out.force_stop = _force_stop_turn;
    out.reminder = std::move(reminder_text);
    out.reminder += diff_args_reminder_text;
    out.reminder += turn_total_reminder_text;
    return out;
}

} // namespace kimix::agent
