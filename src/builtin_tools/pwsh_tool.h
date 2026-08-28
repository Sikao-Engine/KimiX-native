// pwsh_tool.h - Built-in pwsh tool kernels: the self-kill guard.
//
// Plan: C:/dev/kimi-agent/plans/pwsh.md (§3.2 "New kernel - self-kill guard").
// Python source of truth: C:/dev/kimi-agent/src/kimix/tools/file/bash/safety.py
//   - command_detection_variants        (lines 48-70)
//   - _segment_tokens / _segment_text   (lines 73-81, 438-440)
//   - _looks_like_flag                  (lines 84-91)
//   - _split_image_name                 (lines 388-398)
//   - _numeric_pid_targets              (lines 443-460)
//   - _loop_pid_sources + headers       (lines 469-505)
//   - _variable_pid_hit                 (lines 508-538)
//   - _name_kill_hit                    (lines 541-561)
//   - _pattern_kill_hit                 (lines 564-584)
//   - _pkill_full_match                 (lines 587-595)
//   - detect_self_kill                  (lines 598-770)
//   - _SELF_KILL_GUIDANCE / self_kill_hint (lines 773-806)
//
// Contract (per plan §3.2 / §8 conformance gate):
//   * Pure CPU kernel: stateless, no globals, no file/system/network access,
//     no exceptions escape. Every input the Python side resolves via OS
//     introspection (_agent_pids / _agent_image_names / _agent_cmdline) is
//     passed in by the caller and stays Python per the plan.
//   * ASCII-only: every byte of every input must be < 0x80. Non-ASCII input
//     is reported through tool_status::unsupported so the shim routes the
//     whole call to the Python mirror (Python's str.isdigit / str.isalpha /
//     re \b are Unicode-aware, so an ASCII scanner is exact only for ASCII).
//   * Regex-free pkill subset: pkill patterns are full extended regexes in
//     Python. This kernel implements only the plain case-insensitive
//     substring subset; when any pkill pattern token contains a regex
//     metacharacter ([](){}+?|^$\.), detect_self_kill returns
//     tool_status::unsupported so the shim falls back to Python (never a
//     false block, never std::regex).
//   * Hit descriptions are byte-exact ports of the Python f-strings.
//
// Everything compiles into the kimix-llm static library; the tool-private
// namespace keeps the unity (jumbo) build collision-free.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "builtin_tools/tool_types.h"

namespace kimix::builtin_tools::pwsh {

// Result of the self-kill scan (variant entry point used by the shim so it
// can both build the hint message and record which rule fired).
struct self_kill_result {
    tool_status status = tool_status::ok; // ok: hit/description are valid;
                                          // unsupported: route to Python mirror
    bool hit = false;                     // true when the command would kill the agent
    kimix::optional<kimix::string> description; // byte-exact Python description when hit
    kimix::string rule_id;                // matched rule: "kill", "taskkill",
                                          // "stop-process", "get-process",
                                          // "pkill", "killall", "wmic" ("" when no hit)
};

// safety.py _SELF_KILL_GUIDANCE (773-780) - guidance text appended to the hint.
extern const char *const k_self_kill_guidance;

// safety.py command_detection_variants (48-70): deduped deobfuscation
// variants of `command` (whitespace-collapsed original, quote/backslash
// stripped + lowercased, lowercased collapsed; at most 3 entries).
// Empty / whitespace-only input yields an empty list, matching Python
// safety.py:61-62 (the `variants or [collapsed]` fallback only applies to a
// non-empty command that produces no variants, which never happens).
void command_detection_variants(kimix::string_view command,
                                kimix::vector<kimix::string> &out);

// safety.py detect_self_kill (598-770). Returns a short description when
// `command` would kill the agent process (one of `protected_pids` or a
// name/pattern target matching `image_names` / `cmdline`), else nullopt.
//
// `image_names` are expected lowercased by the shim (Python does
// {n.lower() ...}); the kernel lowercases them defensively too.
// `cmdline` is only consulted by `pkill -f` ("" disables that haystack).
//
// Status contract:
//   ok          - `return value` is valid (hit description or nullopt)
//   unsupported - non-ASCII input, or a pkill pattern token containing a
//                 regex metacharacter; the caller must use the Python mirror
kimix::optional<kimix::string> detect_self_kill(
    kimix::string_view command,
    const kimix::unordered_set<int64_t> &protected_pids,
    const kimix::unordered_set<kimix::string, kimix::string_hash> &image_names,
    kimix::string_view cmdline,
    tool_status &status);

// Same scan, but also reports the matched rule id (see self_kill_result).
self_kill_result detect_self_kill_ex(
    kimix::string_view command,
    const kimix::unordered_set<int64_t> &protected_pids,
    const kimix::unordered_set<kimix::string, kimix::string_hash> &image_names,
    kimix::string_view cmdline);

// safety.py self_kill_hint (783-806) minus OS introspection: run
// detect_self_kill over every deobfuscation variant of `command` using the
// caller-resolved agent identity (`protected_pids`, `image_names`,
// `cmdline`, `agent_pid` = os.getpid() on the Python side). Returns the
// composed hint message, or nullopt when the command is safe.
//
// Status contract: ok (result valid) or unsupported (some variant fell
// outside the native subset - non-ASCII or regex-metachar pkill pattern;
// the caller must route the whole command to the Python mirror).
kimix::optional<kimix::string> self_kill_hint(
    kimix::string_view command,
    const kimix::unordered_set<int64_t> &protected_pids,
    const kimix::unordered_set<kimix::string, kimix::string_hash> &image_names,
    kimix::string_view cmdline,
    int64_t agent_pid,
    tool_status &status);

} // namespace kimix::builtin_tools::pwsh
