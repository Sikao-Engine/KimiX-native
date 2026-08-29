pwsh — implementation report

Plan: `D:/KimiX-native/plans/pwsh.md`.
Python source of truth:
  * `D:/kimi-agent/src/kimix/tools/file/bash/safety.py` (self-kill guard, hardline floor)
  * `D:/kimi-agent/bin/kimix_native/_shell_compat.py` (`pwsh_transform`, `fix_pwsh_command`)
  * `D:/kimi-agent/src/kimix/tools/common.py` (`_maybe_rewrite_shell_command_with_rtk`)

What was ported

One header/source pair, namespace `kimix::builtin_tools::pwsh`:

* `src/builtin_tools/pwsh_tool.h` — public API + `Pwsh` Tool subclass.
* `src/builtin_tools/pwsh_tool.cpp` — implementation.
* `src/runtime/parse/shell_scanner.h` / `.cpp` — extended `scan_shell` with an optional `warnings` output for `PWSH_TRANSFORM`.
* `tests/unit/builtin_tools/test_pwsh_tool.cpp` — Boost.UT golden vectors.
* `src/builtin_tools/reports/pwsh.md` — this report.

Public API:

* `command_detection_variants(command, out)` — deobfuscation variants.
* `detect_self_kill(...)` / `detect_self_kill_ex(...)` / `self_kill_hint(...)` — existing self-kill guard.
* `pwsh_transform(code)` — PS7 -> PS5.1 syntax transform with per-line warnings.
* `fix_pwsh_command(command)` — PowerShell fixer / repair validator.
* `check_hardline_blocked(command)` — hardline safety floor wrapper.
* `maybe_rewrite_with_rtk(...)` — RTK rewrite in pwsh mode.
* `class Pwsh` — `Tool` subclass that dispatches by a `mode` parameter.

Implementation notes

* `pwsh_transform` gates non-ASCII input to `tool_status::unsupported`, then calls `scan_shell(PWSH_TRANSFORM)` and returns the transformed command plus the "Line N: ..." warnings produced by the scanner.
* `fix_pwsh_command` gates non-ASCII input to `valid=false`, then calls `scan_shell(PWSH_FIX)` and maps the warning code to the exact warning strings used by `_shell_compat.py`.
* `check_hardline_blocked` gates non-ASCII input to `blocked=false` and delegates to the existing `kimix::runtime::tools::check_hardline_blocked`.
* `maybe_rewrite_with_rtk` is a thin wrapper that calls `bash::maybe_rewrite_shell_command_with_rtk(..., /*pwsh=*/true)`.
* `Pwsh::operator()` reads a `mode` field (`transform`, `fix`, `hardline`, `rtk_rewrite`, `self_kill_hint`) and serializes the kernel result into `_last_result`.

What stays in Python

Per the plan:

* PowerShell executable discovery, subprocess spawning, streaming, timeout, kill.
* Base64 command encoding (`_maybe_encode_command`).
* `wait_for_pattern` regex compilation.
* Console init / try-catch wrapper and one-shot argv builder.
* Output post-processing (dedup, export, summarize, secret redaction).
* Session / config state and interactive REPL management.
* Workdir filesystem validation.

Deviations

1. **PWSH_FIX warning strings.** The plan's table listed older "apparently unbalanced ... verified valid" messages. The current `_shell_compat.py` warning strings are the unclosed/here-string/block-comment/trailing-comment/stop-parsing/comment-only/trailing-continuation messages; the C++ wrapper maps warning codes to those exact current strings.
2. **Deterministic image-name tie-break.** Already documented in the self-kill guard report; retained unchanged.
3. **pkill regex-metacharacter gate.** Retained from the existing self-kill guard.
4. **`Pwsh::operator()` mode dispatch.** The plan only said "dispatch to the pure kernels"; the concrete `operator()` uses a `mode` string so the same Tool subclass can expose every kernel through the standard binding path.

Verification

* Syntax checks via `python scripts/check_cpp_syntax.py` passed for:
  * `src/runtime/parse/shell_scanner.cpp`
  * `src/builtin_tools/pwsh_tool.h`
  * `src/builtin_tools/pwsh_tool.cpp`
  * `tests/unit/builtin_tools/test_pwsh_tool.cpp`

* Golden vectors for `pwsh_transform`, `fix_pwsh_command`, `check_hardline_blocked` and `maybe_rewrite_with_rtk` were harvested from the Python reference on ASCII inputs.

Files changed

* `src/builtin_tools/pwsh_tool.h`
* `src/builtin_tools/pwsh_tool.cpp`
* `src/runtime/parse/shell_scanner.h`
* `src/runtime/parse/shell_scanner.cpp`
* `tests/unit/builtin_tools/test_pwsh_tool.cpp`
* `src/builtin_tools/reports/pwsh.md`

No `issue/pwsh.md` — no missing vendored library blocks the work.
