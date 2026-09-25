// pwsh_tool.h - Built-in pwsh tool kernels (self-kill guard, PS7 -> PS5.1
// syntax transform, PowerShell fixer, hardline floor, RTK rewrite) plus the
// Pwsh tool class, which runs PowerShell for real through the reproc runner.
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
//   * Reused native kernels: shell_scanner (transform/fix), shell_safety
//     (hardline) and the bash RTK rewriter - see builtin_tools/process_runner.h
//     for the subprocess layer. The kernels themselves are pure CPU: no file,
//     system or network access (the host lookup in detect_pwsh_path() only
//     stats PATH candidates). Process lifecycle lives in the runner, shared
//     with the bash / python / run tools.
//   * ASCII-only for transform / fix / hardline / self-kill: non-ASCII input
//     is reported through tool_status::unsupported so the shim routes the call
//     to the Python mirror. An execution request whose command cannot be
//     analysed natively is therefore refused rather than run unchecked.
//   * No std::string/std::vector in public APIs; no RTTI.
//
// Everything compiles into the kimix-llm static library; the tool-private
// namespace keeps the unity (jumbo) build collision-free.

#pragma once

#include <cstdint>

#include <core/kimix_core.h>

#include "builtin_tools/bash_tool.h"
#include "builtin_tools/tool.h"
#include "builtin_tools/tool_types.h"

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
  bool hit = false; // true when the command would kill the agent
  kimix::optional<kimix::string>
      description;       // byte-exact Python description when hit
  kimix::string rule_id; // matched rule: "kill", "taskkill",
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
    kimix::string_view cmdline, tool_status &status);

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
    kimix::string_view cmdline, int64_t agent_pid, tool_status &status);

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
maybe_rewrite_with_rtk(kimix::string_view command, bool token_kill,
                       bool rtk_available, kimix::string_view rtk_binary_path,
                       bool exclude_read = false);

// ---------------------------------------------------------------------------
// One-shot PowerShell execution machinery (shell_common.py)
// ---------------------------------------------------------------------------
// _PWSH_CONSOLE_INIT (pwsh_tool.py 70-74): prepended to every command line so
// the child reports UTF-8 and never propagates Ctrl+C back to the parent.
extern const char *const k_pwsh_console_init;
inline constexpr size_t k_pwsh_encode_threshold = 8000;
// The reference passes the short forms of -Command / -EncodedCommand.
inline constexpr const char *k_pwsh_param_command = "-C";
inline constexpr const char *k_pwsh_param_encoded = "-Enc";

// shell_common.wrap_pwsh_command (100-111): console init + try/catch so a
// terminating error exits non-zero + `;exit $LASTEXITCODE` so the real native
// exit code of the last program survives (PowerShell otherwise flattens every
// failure to 1).
kimix::string wrap_pwsh_command(kimix::string_view command);

// pwsh_tool.py _maybe_encode_command (820-832): above k_pwsh_encode_threshold
// characters the payload becomes Base64(UTF-16LE) passed as `-Enc`, otherwise
// it is passed verbatim as `-C` (both short forms, like the reference).
struct pwsh_payload {
  kimix::string param; // "-C" or "-Enc"
  kimix::string value; // raw command or base64 text
  bool encoded = false;
};
pwsh_payload pwsh_maybe_encode(kimix::string_view raw);

// shell_common.pwsh_argv (122-151): repair the command (fix_pwsh_command),
// downgrade PS7 syntax when no PowerShell 7 host exists (pwsh_transform), wrap
// it and build the one-shot argv.
struct pwsh_argv_result {
  tool_status status = tool_status::ok;
  kimix::vector<kimix::string> argv; // empty unless status == ok
  kimix::string executable;
  kimix::string prepared; // repaired/downgraded command line
  kimix::string warning;  // accumulated "[WARNING] ..." text
  kimix::string message;  // failure wording when status != ok
};
// `pwsh_path` is the resolved host ("" == resolve with detect_pwsh_path()).
pwsh_argv_result build_pwsh_argv(kimix::string_view command,
                                 kimix::string_view pwsh_path = {});
// Interactive REPL argv (pwsh_tool.py 529-535): no -NonI, -NoExit + the console
// init, optionally followed by the first command line.
kimix::vector<kimix::string>
build_pwsh_interactive_argv(kimix::string_view command,
                            kimix::string_view pwsh_path = {});
// Base64 (standard padded alphabet, no line breaks) of raw bytes and UTF-8 ->
// UTF-16LE - the two halves of the `-EncodedCommand` payload, public so tests
// can pin them against the reference's base64.b64encode(s.encode("utf-16-le")).
kimix::string pwsh_base64_string(kimix::string_view bytes);
kimix::string pwsh_utf16le_string(kimix::string_view text);
// True when the resolved host is Windows PowerShell 5.1 (the name spells
// "powershell"), i.e. PowerShell 7 syntax must be downgraded first.
bool pwsh_is_windows_powershell(kimix::string_view executable);

// ---------------------------------------------------------------------------
// Tool class and standard integration
//
// The kernels above are pure CPU, but the Pwsh tool class itself DOES manage a
// subprocess: with Session::native_io it runs PowerShell through the same
// reproc-backed runner the bash / python / run tools use
// (builtin_tools/process_runner.h), in the reference's three execution modes
// (execute / send / interactive). Without native_io it stays kernel-only and
// the Python mirror owns the process.
// ---------------------------------------------------------------------------

// Resolve a PowerShell host for the validity probe: PowerShell 7 (`pwsh`) on
// PATH, then Windows PowerShell (`powershell` on PATH, else the in-box
// %SystemRoot%\System32\WindowsPowerShell\v1.0 location, which a trimmed PATH
// can miss). Empty when nothing was found. Existence-only: no process is
// spawned.
kimix::string detect_pwsh_path();

class Pwsh : public kimix::builtin_tools::Tool {
public:
  explicit Pwsh(kimix::builtin_tools::Session *session);
  // Kernel modes (transform / fix / hardline / rtk_rewrite / self_kill_hint)
  // analyse a command line; the execution modes (execute / send /
  // interactive) run a real PowerShell process when the session is native.
  // A native session that names no mode asks for `execute` (the reference
  // default); a non-native one keeps the historical `transform` default so
  // the Python shim is unaffected.
  void operator()(kimix::builtin_tools::ToolParams const *parameters) override;

  // Validity needs a PowerShell host: false when nothing can run a
  // PowerShell command line here (no pwsh and no Windows PowerShell). The
  // soul pairs this tool with the bash tool: when bash is invalid (no Git
  // Bash on Windows) pwsh is the shell the prompt advertises, so keeping
  // this probe honest is what makes the fallback work.
  bool valid() const override;

  // Access the serialized JSON produced by the last operator() invocation.
  kimix::vector<char> const &last_result() const { return _last_result; }
  void result_json(kimix::vector<char> &out) const override {
    out = _last_result;
  }

private:
  kimix::vector<char> _last_result;
};

} // namespace pwsh
} // namespace kimix::builtin_tools
