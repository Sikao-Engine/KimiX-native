# Bash built-in tool — C++ implementation report

Worktree: `D:/KimiX-native`
Plan: `plans/bash.md`
Base commit: (worktree has local changes on top of `HEAD`)

## Files touched / created

- `src/builtin_tools/bash_tool.h` — public API additions (hardline, guidance, annotate, params, Bash class)
- `src/builtin_tools/bash_tool.cpp` — implementations for the new kernels and the Bash tool class
- `tests/unit/builtin_tools/test_bash_tool.cpp` — new golden-vector / behavioural tests
- `src/builtin_tools/reports/bash.md` — this report

## Function-by-function map to Python source

| C++ symbol | Python reference | Notes |
|---|---|---|
| `command_detection_variants` | `safety.py command_detection_variants` (48-70) | Three variants: whitespace-collapsed original, quote/backslash-stripped + lowercased, lowercased collapsed. Empty / whitespace input yields an empty list. |
| `detect_hardline_command` | `safety.py detect_hardline_command` (153-203) | Single-variant detector. Returns `hardline_result{blocked, description}`. |
| `check_hardline_blocked` | `safety.py check_hardline_blocked` (206-219) | Iterates over `command_detection_variants` until one matches. |
| `foreground_background_guidance` | `safety.py foreground_background_guidance` (254-269) | Quoted spans stripped. Long-running pattern scan. |
| `annotate_failure` | `output_enhance.py annotate_failure` (179-217) | Scans first 4000 ASCII bytes. `command`/`exit_code` are accepted for signature compatibility only. |
| `parse_bash_params` | `bash_tool.py BashParams` (565-587) | Deserializes the generic `ToolParams` into `bash_params`. Supports `cmd` and alias `command`. |
| `Bash` class | `bash_tool.py Bash.__call__` (706-992) | Does **not** spawn processes. Runs safety floors, calls injected callbacks, and serializes the result. |
| `process_exited_banner` | `common.py ProcessStream completion banner` (2118-2126) | Already present; retained unchanged. |
| `has_top_level_pipe` | `output_enhance.py _has_top_level_pipe` (57-99) | Already present; retained. |
| `base_command_name` | `output_enhance.py _base_command_name` (41-54) | Already present; retained. |
| `interpret_exit_code` / `is_expected_exit` | `output_enhance.py` (119-176) | Already present; retained. |
| `find_error_line_index` / `truncate_lines` | `common.py` (334-1164) | Already present; retained. |
| `split_shell_segments` / RTK rewrite | `common.py` (1268-1538) | Already present; retained. |
| `capture_machine` / `bounded_append_capture` | `background/utils.py` (42-76, 253-369) | Already present; retained. |

## Port vs stay in Python

| Python symbol / subsystem | Port? | Where in C++ | Justification |
|---|---|---|---|
| `BashParams` parsing | Port | `bash_params`, `parse_bash_params` | Pure JSON/parameter validation. |
| `_hardline_blocked` / `check_hardline_blocked` | Port | `check_hardline_blocked`, `detect_hardline_command` | Pure string scanning; no OS access. |
| `self_kill_hint` / `detect_self_kill` | Reuse | `kimix::builtin_tools::pwsh::self_kill_hint` | Cross-tool ownership: pwsh owns this symbol. |
| `foreground_background_guidance` | Port | `foreground_background_guidance` | Regex-like pattern scan; no I/O. |
| `annotate_failure` | Port | `annotate_failure` | Output substring scan; no I/O. |
| `interpret_exit_code` / `is_expected_exit` | Already ported | `interpret_exit_code`, `is_expected_exit` | Pure string kernel. |
| `_has_top_level_pipe` / `_base_command_name` | Already ported | `has_top_level_pipe`, `base_command_name` | Pure string kernel. |
| `_find_error_line_index` / `_truncate_lines` | Already ported | `find_error_line_index`, `truncate_lines` | Pure output processing. |
| `_split_shell_segments` / RTK rewrite | Already ported | `split_shell_segments`, etc. | Pure shell scanner. |
| `capture_machine` bounded run policy | Already ported | `capture_machine`, `capture_event`, `capture_config` | Pure state machine. |
| `_build_session_output_block` | Reuse | `python::build_session_output_block` | Already ported in `python_tool.h`. |
| `find_bash` / bash discovery | Stay Python | — | Uses `subprocess.run`, `shutil.which`, Windows/MSYS probing. |
| `_prepare_command` | Stay Python (callback) | `Bash::config::prepare_command` | Windows/MSYS-specific normalization; injected as callback. |
| `_encode_startup_script` | Stay Python | — | Uses gzip + pybase64. |
| `ProcessTask` / `BackgroundStream` | Stay Python | — | Subprocess spawn, async I/O, threading, process-tree registry. |
| `kill_child_tree` | Stay Python | — | OS process-tree termination. |
| `_token_filter_output` full pipeline | Stay Python | C++ kernels used when called | Depends on rich ANSI parser and `micro_compress`, not vendored. |
| `_summarize_long_output_async` | Stay Python | — | LLM network call. |
| `_maybe_export_*` / temp file lifecycle | Stay Python | — | Filesystem temp-folder management. |
| `redact_sensitive_output` | Stay Python (callback) | `Bash::config::redact_secrets` | Secret scanner not vendored. |

## ASCII gates and deviations

1. **ASCII gate.** Every new kernel is exact only for ASCII input. Non-ASCII input is treated as safe / no-hint so the Python shim (which gates on `str.isascii()`) routes it to the Python mirror. This matches the project-wide convention.
2. **Line splitting.** `truncate_lines` and `find_error_line_index` split on LF / CRLF / CR only. Python `str.splitlines()` recognizes additional Unicode terminators, but the Bash pipeline normalizes line endings before these kernels run.
3. **Word boundaries.** The error-keyword matcher uses ASCII `\w = [A-Za-z0-9_]` boundaries.
4. **No heredoc awareness in `has_top_level_pipe`.** Follows the Python reference exactly.
5. **No 126/127/128+N exit-code rules.** Follows the Python reference exactly.
6. **RTK multi-segment rule.** Multi-segment commands (top-level `;` / `&&` / `||`) are never rewritten, matching the reference.
7. **Self-kill regex subset.** The pwsh-owned kernel supports only the plain pkill subset; regex metacharacters return `tool_status::unsupported`. Bash forwards to the same kernel.
8. **Bounded-run ordering.** `capture_machine` ordering: append → pattern → process-exit → total-timeout → inactivity.
9. **Hardline deobfuscation.** `command_detection_variants` produces at most three variants as in the reference.
10. **Bash class configuration.** The plan's `Bash::config` did not include agent-identity fields needed by `pwsh::self_kill_hint`; this implementation adds `protected_pids`, `image_names`, `cmdline`, and `agent_pid` to the config struct. Documented as a deliberate deviation.
11. **Forbidden-command policy.** The reference `_forbidden_error` is config-driven and stays in Python; the C++ class provides `forbidden_keywords` as a simple substring guard for the native path. Complex config-driven rules remain on the Python side.

## Tests

Added tests in `tests/unit/builtin_tools/test_bash_tool.cpp`:

- `command_detection_variants`
- `detect_hardline_command_recursive_delete`
- `detect_hardline_command_other_patterns`
- `check_hardline_blocked_variants`
- `foreground_background_guidance`
- `annotate_failure`
- `parse_bash_params`
- `bash_tool_class_safety_floors`
- `bash_tool_class_self_kill_reuse`
- `bash_tool_class_operator_serialize`

Existing tests cover the already-ported kernels (`has_top_level_pipe`, `base_command_name`, `interpret_exit_code`, `find_error_line_index`, `truncate_lines`, RTK, `capture_machine`, `process_exited_banner`).

Test target name: `test_builtin_bash`

Build / run command (supervisor runs these):

```bash
xmake build test_builtin_bash
xmake run test_builtin_bash
```

Sanitizer run:

```bash
xmake f -c --policies=build.sanitizer.address,build.sanitizer.undefined
xmake build -r test_builtin_bash
xmake run test_builtin_bash
```

## Build / verification status

- `python scripts/check_cpp_syntax.py src/builtin_tools/bash_tool.h` — OK
- `python scripts/check_cpp_syntax.py src/builtin_tools/bash_tool.cpp` — OK
- `python scripts/check_cpp_syntax.py tests/unit/builtin_tools/test_bash_tool.cpp` — OK
- `xmake build` / `xmake run` were **not** run per task instructions; the supervisor builds/runs all targets.
