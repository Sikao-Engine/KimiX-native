// pwsh_tool.h - Built-in pwsh tool kernels: self-kill guard, PS7 -> PS5.1
// syntax transform, PowerShell fixer, hardline floor, and RTK rewrite.
//
// Plan: D:/KimiX-native/plans/pwsh.md.
// Python source of truth:
//   * safety.py (self-kill guard + hardline floor)
//   * _shell_compat.py (pwsh_transform, fix_pwsh_command)
//   * common.py (_maybe_rewrite_shell_command_with_rtk)
//
// Reused native kernels:
//   * runtime/parse/shell_scanner.h — PWSH_TRANSFORM and PWSH_FIX
//   * runtime/tools/shell_safety.h  — check_hardline_blocked
//   * builtin_tools/bash_tool.h     — maybe_rewrite_shell_command_with_rtk
//
// Contract:
//   * Pure CPU kernel: no file/system/network access. OS introspection and
//     subprocess lifecycle stay in Python.
//   * ASCII-only for transform / fix / hardline / self-kill: non-ASCII input
//     is reported through tool_status::unsupported so the shim routes the call
//     to the Python mirror.
//   * No std::string/std::vector in public APIs; no RTTI.
//
// Everything compiles into the kimix-llm static library; the tool-private
// namespace keeps the unity (jumbo) build collision-free.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "builtin_tools/tool_types.h"
#include "builtin_tools/tool.h"
#include "builtin_tools/bash_tool.h"

#include <runtime/parse/shell_scanner.h>
#include <runtime/tools/shell_safety.h>

namespace kimix::builtin_tools {
namespace pwsh {

using kimix::builtin_tools::tool_error;
using kimix::builtin_tools::tool_status;

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

// ---------------------------------------------------------------------------
// PowerShell 7.x -> 5.1 syntax transformation
// ---------------------------------------------------------------------------
struct transform_result {
    tool_status status = tool_status::ok;
    kimix::string command;
    kimix::vector<kimix::string> warnings;
};

// Mirrors _shell_compat.py::pwsh_transform.
// Non-ASCII input routes to tool_status::unsupported.
transform_result pwsh_transform(kimix::string_view code);

// ---------------------------------------------------------------------------
// PowerShell command validator / auto-repair
// ---------------------------------------------------------------------------
struct fix_result {
    bool valid = false;
    bool changed = false;
    kimix::string command;
    kimix::string warning;
};

// Mirrors _shell_compat.py::fix_pwsh_command.
// Non-ASCII input routes to tool_status::unsupported.
fix_result fix_pwsh_command(kimix::string_view command);

// ---------------------------------------------------------------------------
// Hardline safety floor
// ---------------------------------------------------------------------------
struct hardline_result {
    bool blocked = false;
    kimix::string description;
};

// Delegates to runtime::tools::check_hardline_blocked.
// Non-ASCII input routes to tool_status::unsupported.
hardline_result check_hardline_blocked(kimix::string_view command);

// ---------------------------------------------------------------------------
// RTK command rewrite (pwsh mode)
// ---------------------------------------------------------------------------
// Delegates to bash::maybe_rewrite_shell_command_with_rtk with pwsh=true.
kimix::builtin_tools::bash::rewrite_result
maybe_rewrite_with_rtk(kimix::string_view command,
                       bool token_kill,
                       bool rtk_available,
                       kimix::string_view rtk_binary_path,
                       bool exclude_read = false);

// ---------------------------------------------------------------------------
// Tool class and standard integration
// ---------------------------------------------------------------------------
class Pwsh : public kimix::builtin_tools::Tool {
public:
    explicit Pwsh(kimix::builtin_tools::Session *session);
    void operator()(kimix::builtin_tools::ToolParams const *parameters) override;

    // Access the serialized JSON produced by the last operator() invocation.
    kimix::vector<char> const &last_result() const { return _last_result; }

private:
    kimix::vector<char> _last_result;
};

} // namespace pwsh
} // namespace kimix::builtin_tools
