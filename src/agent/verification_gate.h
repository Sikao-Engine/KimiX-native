// agent/verification_gate.h - Soul-layer verification gate.
//
// Port of kimi_cli/soul/verification_gate.py (152 lines). When a step ends
// with stop_reason "no_tool_calls" (and the turn was not force-stopped), the
// gate checks whether the turn is *actually* finished:
//
//   1. unfinished todos remain (loaded through the todo tool's session state,
//      so the gate always agrees with what todo_list would report -
//      kimisoul.py:664-686 _load_todo_states_for_reminder);
//   2. the turn modified files (an edit-class tool call, agent/tool_taxonomy.h)
//      but ran no verification-class call at all.
//
// If any condition hits, the gate returns a nudge text (wrapped in
// <system-reminder> by the caller, soul/message.py system_reminder parity) and
// the turn continues with one more step. A hard max_nudges cap per turn
// prevents deadlocks. All strings are the reference's verbatim text.
//
// Turn scope = the history suffix after the most recent REAL user message;
// injected system-reminder user messages are skipped (verification_gate.py:
//48-63, mirroring kimisoul.py:341-353).
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "builtin_tools/todo_tool.h"
#include "llm/llm.h"

namespace kimix::agent {

inline constexpr int32_t kVerificationGateMaxUnfinishedListed =
    10; // _MAX_UNFINISHED_LISTED

class VerificationGate {
public:
    explicit VerificationGate(int32_t max_nudges = 2) : _max_nudges(max_nudges < 0 ? 0 : max_nudges) {}

    // Per-turn reset of the nudge budget (verification_gate.py:42-45 turn-id
    // sync). The soul calls this at the start of every turn().
    void begin_turn() noexcept { _nudges = 0; }

    // The max-nudges bound comes from [loop_control]
    // (verification_gate_max_nudges, config.py:364-371), so the soul re-applies
    // it at every turn start rather than the gate owning the config.
    void set_max_nudges(int32_t max_nudges) noexcept {
        _max_nudges = max_nudges < 0 ? 0 : max_nudges;
    }

    int32_t nudge_count() const noexcept { return _nudges; }
    int32_t max_nudges() const noexcept { return _max_nudges; }

    // Returns the nudge text when the turn must continue, or nullopt when the
    // turn may end (verification_gate.py:111-152). `todos` is the session's
    // active todo tree (roots; children are flattened DFS, exactly like the
    // reference iterates the loaded todo list). Never fails on malformed
    // inputs: a broken todo list or unparseable tool arguments degrade to
    // "condition not triggered".
    kimix::optional<kimix::string>
    check(const kimix::vector<kimix::llm::Message> &history,
          kimix::span<const builtin_tools::todo::todo_item> todos);

    // Classification helpers (verification_gate.py:65-105), exposed for tests.
    static bool todolist_marks_done(kimix::string_view arguments_json);

private:
    int32_t _max_nudges;
    int32_t _nudges = 0;
};

// ── Reference nudge fragments (verification_gate.py), verbatim ──────────────

// "Unfinished todo_list tasks remain:\n- [status] title\n…" (up to
// kVerificationGateMaxUnfinishedListed items + "- … and N more").
kimix::string verification_gate_unfinished_todos_reason(
    kimix::span<const builtin_tools::todo::todo_item> todos);

// "You modified code this turn but ran no verification ..." block.
kimix::string_view verification_gate_unverified_edits_reason() noexcept;

// Wraps the joined reasons in the reference's nudge envelope:
// "The turn cannot finish yet — verification gate findings:\n\n…\n\nAddress
// the findings above, then finish. (nudge i/N this turn)"
kimix::string verification_gate_nudge_text(kimix::span<const kimix::string> reasons,
                                           int32_t nudge, int32_t max_nudges);

} // namespace kimix::agent
