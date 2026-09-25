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

Differential verification (kimi-base ↔ kimi-agent)

Two suites now compare every pwsh kernel against the kimi-agent reference
(`C:/dev/kimi-agent`, override with `KIMI_AGENT_ROOT`):

* `python/tests/test_parity_pwsh.py` — live differential over
  `runtime_py.builtin_tools.shell` for `fix_pwsh_command`, `pwsh_transform`,
  `pwsh_command_detection_variants`, `pwsh_check_hardline_blocked` and
  `pwsh_maybe_rewrite_with_rtk`.  Corpora: every input kimi-agent's own
  `tests/test_process_pwsh.py` / `tests/test_pwsh_fix.py` use, a curated
  adversarial list, a grammar cross-product (prefix × PS7 construct ×
  string/comment/`--%` decoration) and seeded fuzz.
* `tests/unit/builtin_tools/test_pwsh_tool.cpp` — generated golden vectors
  (`tests/unit/builtin_tools/pwsh_goldens.inc`, 4106 lines, produced by
  `scripts/gen_pwsh_goldens.py` from the same reference) for the kernels that
  have **no** Python binding: `detect_self_kill` (567 adversarial kill-target
  vectors), `self_kill_hint` (293), `command_detection_variants` (564),
  `pwsh_transform` (1741) and `fix_pwsh_command` (873).

Three real port bugs were found and fixed (all verified failing before the fix):

1. `fix_pwsh_command` reported the null-device warning for *any* repair.  The
   code derived one `nul_changed` boolean from the two nul passes *and* the
   quote scanner, so `echo "x` (nothing to do with `nul`) came back with
   `…\nRewrote Windows-style null-device redirection target(s)…`.  The two
   passes are now tracked separately (`nul_changed_first` / `nul_changed_after`).
2. `fix_pwsh_command` emitted the warnings in the wrong order.  The reference
   composes `[nul_warning, scanner_warning, nul_warning_after]`; the port put
   the scanner note first, so `echo > nul "x` produced
   `unclosed-dq\nnul` instead of `nul\nunclosed-dq`.
3. `pwsh_transform` dropped the command word before a rewritten expression.
   `_strip_command_prefix` returns the *adjusted* start and the reference
   rebuilds with `line[:start]`, but all three call sites
   (`_transform_nc_line`, `_transform_ternary_line`,
   `_transform_null_conditional_line`) kept using the unadjusted index, so
   `Write-Output $a?.Name` became `$(if ($null -ne $a) { $a.Name })` — a
   different command.  kimi-agent's own suite asserts the prefix survives
   (`test_null_conditional_after_command_prefix`,
   `test_cmd_prefix_with_variable_prop`,
   `test_command_followed_by_ternary_without_parens`).

Documented deviations (unchanged, now pinned by tests):

* Image-name tie-break.  `safety._name_kill_hit` iterates `image_names` (an
  unordered `set`) and returns the first entry matching the token's basename or
  stem.  With both `python` and `python.exe` in the set, the returned name — and
  therefore the description text — depends on `PYTHONHASHSEED`; the port sorts
  the names ascending.  `gen_pwsh_goldens.py` pins the same order so the vectors
  are reproducible (the instability itself is asserted in
  `test_image_name_tie_break_is_nondeterministic_in_the_reference`).
* ASCII gate — non-ASCII input returns the sentinels `valid=false` / empty
  command (route to the Python mirror), pinned by
  `test_ascii_gate_is_documented`.
* Reference-inherited false negative: bash's own-PID spellings (`kill $$`,
  `kill $PPID`, `kill $!`) are not resolved to the agent PID by
  `detect_self_kill`, so the port is silent for them too (vectors pin it).
* `Pwsh::operator()` is a kernel facade for a NON-native session: with
  `Session::native_io == false` it dispatches on `mode` (default `transform`)
  and never spawns, which is what the Python mirror expects. Pinned by
  `pwsh_tool_class_modes` in the C++ test.
* A native session DOES execute: `execute` / `send` / `interactive` run real
  PowerShell through `builtin_tools/process_runner.h` (see
  `src/builtin_tools/reports/process_runner` notes in `README.md`). A native
  call that names no mode means `execute` — the registry advertises
  `{command, timeout}` and the reference's `PowershellParams.mode` defaults to
  `execute`; answering such a call with the transform kernel would silently
  spawn nothing.

## Native execution (Pwsh tool class)

Source of truth: `pwsh_tool.py` (`Powershell.__call__`, `_execute_background`,
`_continue_session`, `_PWSH_CONSOLE_INIT`, `_maybe_encode_command`) and
`shell_common.py` (`PWSH_ONESHOT_FLAGS`, `wrap_pwsh_command`, `pwsh_argv`).

* Ported pipeline: hardline floor + self-kill guard on the RAW command →
  optional rtk rewrite → `fix_pwsh_command` repair (unrepairable ⇒ refused, an
  unbalanced quote ⇒ repaired with a `[WARNING]` like the reference) →
  `pwsh_transform` downgrade when the host is Windows PowerShell 5.1 →
  `_PWSH_CONSOLE_INIT + try{…}catch{…;exit 1} + ;exit $LASTEXITCODE` wrapper →
  `-C`, or `-Enc` with Base64(UTF-16LE) above 8000 CHARACTERS (`len(str)`, so
  5000 `é` are below the limit although they are 10000 bytes) → floors re-checked
  on the prepared text → argv
  `{exe, -NoP, -NonI, -Exec, Bypass, -NoL, -C|-Enc, payload}`.
* Interactive REPL argv follows the reference exactly (no `-NonI`, `-NoExit
  -Command <init>`), so `send` continuations and `job_output` work on it.
* Workdir: the reference prefixes `Set-Location -LiteralPath '…'` because its
  `ProcessTask` gets `cwd=None`; the native path passes the directory to reproc
  instead (`run_options::working_directory`, resolved against the session
  workspace), which is the same effect without the quoting hazard. Documented
  deviation.
* Non-ASCII command + only a 5.1 host available ⇒ `unsupported` (the downgrade
  kernel is ASCII-gated), rather than running an untransformed PS7 command.
* `timeout` outside 1..900 ⇒ `invalid_input` (`prompt_common.timeout_field`).
* Self-kill identity: the caller may pass `agent_pid` / `protected_pids` /
  `image_names` / `cmdline`; otherwise `agent_pid` defaults to the current
  process, which is the agent in the native CLI.

Tests: `tests/unit/builtin_tools/test_process_runner.cpp` runs real PowerShell
(foreground token, `exit 4` ⇒ failed, hardline `rmdir /s /q C:\` ⇒ blocked and
never spawned, interactive REPL + continuation round trip, non-native ⇒
`unsupported`) plus the pure argv/encoder parity vectors; it skips when no
PowerShell host is installed.

Result lines: `23 passed` (python) and
`Suite 'global': all tests passed (444 asserts in 36 tests)` (C++).
