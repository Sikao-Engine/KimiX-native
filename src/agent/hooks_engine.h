// agent/hooks_engine.h - The tool lifecycle hooks engine (F6 / audit G07).
//
// Port of kimi_cli/hooks/* onto the native soul's synchronous turn:
//   * engine.py HookEngine: hook definitions indexed by event, regex matchers
//     filtered against the matcher value ("tool_name"), aggregation
//     ("block" wins, first blocked reason reported), fail-open error
//     isolation, and the `trigger` result list;
//   * events.py payload builders: hook_event_name/session_id/cwd plus the
//     per-event fields (tool_name, tool_input, tool_call_id, tool_output,
//     error), byte-shaped like the reference dicts;
//   * runner.py HookResult: action "allow" | "block", reason, timed_out -
//     exit code 2 (or a JSON {"hookSpecificOutput": {"permissionDecision":
//     "deny"}}) means block. The C++ engine accepts SCRIPTED hooks (a
//     callback returning the verdict); shell-command hooks are the
//     config-plumbed layer that stays out of scope here (no config parser
//     changes), so run_hook's subprocess semantics are reduced to the verdict
//     mapping the callback performs.
//
// Deviations (documented, forced by the synchronous single-thread turn):
//   * the reference runs matched hooks concurrently (asyncio.gather) and
//     PostToolUse/PostToolUseFailure fire-and-forget; the C++ engine runs
//     matched hooks in registration order and the fire-and-forget events are
//     triggered synchronously after the result is captured (their outcome is
//     never able to fail the call, preserving fire-and-forget semantics).
//   * timeouts are not part of the scripted-hook contract (a hook callback
//     that cannot answer simply returns; config timeouts belong to the
//     config-plumbed layer).
//
// Rules (see .agents/skills/cpp): namespace kimix::agent, kimix:: containers,
// no exceptions, no RTTI, MSVC + GCC portable.

#pragma once

#include <cstdint>
#include <utility>

#include <core/kimix_core.h>

namespace kimix::agent {
namespace hooks {

// HookEventType (kimi_cli/hooks/config.py) - the events the reference
// defines; only the three tool-lifecycle ones are triggered by the soul.
inline constexpr kimix::string_view kEventPreToolUse = "PreToolUse";
inline constexpr kimix::string_view kEventPostToolUse = "PostToolUse";
inline constexpr kimix::string_view kEventPostToolUseFailure =
    "PostToolUseFailure";

// HookResult (runner.py:13-21).
struct HookResult {
    kimix::string action; // "allow" | "block" (runner.py's Literal)
    kimix::string reason;
    kimix::string stdout_text;
    kimix::string stderr_text;
    int exit_code = 0;
    bool timed_out = false;

    bool blocked() const noexcept { return action == "block"; }
};

// One scripted hook definition (HookDef minus the shell command).
using HookCallback = kimix::function<HookResult(const kimix::string &payload_json,
                                                kimix::string &error)>;
struct HookDef {
    kimix::string event;  // e.g. "PreToolUse"
    kimix::string matcher; // regex over the matcher value; "" matches all
    // The hook body. `payload_json` is the event payload (a JSON object with
    // hook_event_name/session_id/cwd/...); `error` is set when the hook could
    // not run (the engine fails open). Returning "block" blocks the call.
    HookCallback callback;
};

// The engine (engine.py HookEngine). Thread-safe enough for single-threaded
// turn execution: trigger() is const but not re-entrant.
class HookEngine {
public:
    HookEngine() = default;

    // add_hook / add_hooks (engine.py:133-141): register and re-index.
    bool add_hook(kimix::string_view event, kimix::string_view matcher,
                  HookCallback callback);
    void add_hooks(kimix::span<const HookDef> defs);

    // trigger (engine.py:205-244): run every hook whose matcher matches
    // `matcher_value` with the payload; returns per-hook results in
    // registration order. Fail-open: a hook whose callback reports an error
    // yields an "allow" result (never blocks), and the aggregate verdict of
    // the caller is "block" only when some hook answered "block".
    kimix::vector<HookResult> trigger(kimix::string_view event,
                                      kimix::string_view matcher_value,
                                      kimix::string_view payload_json) const;

    bool has_hooks() const noexcept;
    bool has_hooks_for(kimix::string_view event) const noexcept;
    // summary (engine.py:161-169): event -> hook count.
    kimix::vector<std::pair<kimix::string, size_t>> summary() const;

    size_t size() const noexcept { return _hooks.size(); }

private:
    kimix::vector<HookDef> _hooks;
};

// ── events.py payload builders ───────────────────────────────────────────────
// JSON objects with the exact field set of events.py; `tool_input` is embedded
// verbatim (it must be a JSON object; "{}" is used when absent).

kimix::string pre_tool_use_payload(kimix::string_view session_id,
                                   kimix::string_view cwd,
                                   kimix::string_view tool_name,
                                   kimix::string_view tool_input_json,
                                   kimix::string_view tool_call_id);

kimix::string post_tool_use_payload(kimix::string_view session_id,
                                    kimix::string_view cwd,
                                    kimix::string_view tool_name,
                                    kimix::string_view tool_input_json,
                                    kimix::string_view tool_output,
                                    kimix::string_view tool_call_id);

kimix::string post_tool_use_failure_payload(kimix::string_view session_id,
                                            kimix::string_view cwd,
                                            kimix::string_view tool_name,
                                            kimix::string_view tool_input_json,
                                            kimix::string_view error,
                                            kimix::string_view tool_call_id);

} // namespace hooks
} // namespace kimix::agent
