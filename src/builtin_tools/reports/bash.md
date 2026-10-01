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
| _prepare_command | Python callback only | `Bash::config::prepare_command` | Windows/MSYS backslash normalization stays in the Python callback. The Git Bash compatibility fix (`fix_bash_command`) was removed: `Bash::run` passes the command to the shell as written. |
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

Tests

Added tests in tests/unit/builtin_tools/test_bash_tool.cpp:

- command_detection_variants
- detect_hardline_command_recursive_delete
- detect_hardline_command_other_patterns
- check_hardline_blocked_variants
- foreground_background_guidance
- annotate_failure
- parse_bash_params
- bash_tool_class_safety_floors
- bash_tool_class_self_kill_reuse
- bash_tool_class_operator_serialize
- bash_spawn_script_composition / bash_is_git_bash_install_marker (execute-mode MSYSTEM neutralization)
- bash_tool_class_command_pass_through (Bash::run hands the command over unchanged)

Existing tests cover the already-ported kernels (has_top_level_pipe, base_command_name, interpret_exit_code, find_error_line_index, truncate_lines, RTK, capture_machine, process_exited_banner).

Windows Git Bash compatibility fix (plans/bash.md 3.2) - REMOVED

The C++ port of kimi-agent's BashFix scanner (`fix_bash_command`, the
`bash_fix_*` kernels, the generated `GENERATED:BASH-FIX-DATA` fallback /
unsupported tables, `bash_compatibility_prelude()`, `bash_windows_temp_dir()`
and `Bash::config::compat_fix_enabled`) has been removed from
`src/builtin_tools/bash_tool.{h,cpp}`. `Bash::run` now hands the command to Git
Bash exactly as written (after the Python `prepare_command` callback and the
RTK rewrite): no fallback-definition prefix, no Windows path rewrites, no
`nul` -> `/dev/null` rewrite and no `tool_status::unsupported` rejection of
commands Git Bash does not ship.

What stays is the spawn layer:

| C++ symbol | Python reference | Notes |
|---|---|---|
| `bash_is_git_bash_install` | `bash_tool.py _is_git_bash_install` (276-307) | Windows-only `<root>/cmd/git.exe` marker probe; never true for a real MSYS2 install. |
| `bash_spawn_script` | `bash_tool.py _with_msystem_neutralized(_PIPEFAIL_PREFIX + cmd)` (307-329, 886) | `export MSYSTEM=; set -o pipefail; <command>` on a Git Bash install, `set -o pipefail; <command>` elsewhere - the MSYSTEM macro is the only command mutation left. |
| `bash_native_env` | `bash_tool.py _bash_subprocess_env` | `MSYS_NO_PATHCONV=1` + `MSYS2_ARG_CONV_EXCL=*` child-env deltas. |

The runtime BASH_FIX scanner (`src/runtime/parse/shell_scanner.cpp`, exposed to
Python through `runtime_py`) is a separate kernel and is untouched; its name
tables still regenerate from the reference with
`python scripts/gen_bash_fix_data.py --tables-runtime`.

Tests and verification

- `bash_spawn_script_composition` and `bash_is_git_bash_install_marker` cover
  the pipefail prefix and the `export MSYSTEM=; ` neutralization (the marker
  test builds a throw-away `bin/bash.exe` + `cmd/git.exe` tree in the temp
  directory, so it is Windows-only).
- `bash_tool_class_command_pass_through` pins the removal: `rev <<< abc`,
  `journalctl -u svc -f`, `cd D:\x && echo hi > nul` and non-ASCII input all
  come back out of `Bash::run` byte-for-byte.
- The deleted `bash_fix_*` suites went with the kernel, together with the
  `bash_fix_goldens.inc` / `bash_fix_prefix_goldens.inc` vectors and the
  `--tables` / `--goldens` modes of `scripts/gen_bash_fix_data.py`; the
  pure-Python reference (and its own test suite) still lives in kimi-agent.

Test target name: test_builtin_bash

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

Build / verification status

- python scripts/check_cpp_syntax.py src/builtin_tools/bash_tool.h - OK
- python scripts/check_cpp_syntax.py src/builtin_tools/bash_tool.cpp - OK (clangd)
- python scripts/check_cpp_syntax.py tests/unit/builtin_tools/test_bash_tool.cpp - OK (clangd)
- xmake build test_builtin_bash (debug) + bin/debug/test_builtin_bash.exe ->
  all tests passed (18441 asserts in 43 tests; the 43-vs-54 delta is the
  removed `bash_fix_*` suites)
- xmake f -m release -y && xmake build test_builtin_bash + run -> same result
- xmake build (all targets) - build ok
- xmake test - 149 of 150 passed; the one failure (`test_cli`'s
  `cli_app_dry_run_report_real_provider`) is pre-existing: it reproduces on the
  unmodified tree because the external kimi-agent worker manifest requests a
  `kimix.tools.file.run:Run` tool this build does not register

## Differential parity round vs the kimi-agent reference

Harness: `python/tests/test_parity_bash.py` (1004 tests) compares
`runtime_py.builtin_tools.shell.*` against the *original* implementations in the
kimi-agent checkout, never against `python/kimix_native/*` mirrors:

| kernel group | reference | how it is reached |
|---|---|---|
| RTK scanner (`split_shell_segments`, `rewrite_shell_segment`, `is_known_rtk_command`, `maybe_rewrite_shell_command_with_rtk`) | `src/kimix/tools/common.py` | loaded **by path** (a session that imported the 22-line `kimi-cli/src/kimix` shim would otherwise substitute a different `common`) |
| `interpret_exit_code`, `is_expected_exit`, `annotate_failure`, `check_hardline_blocked`, `foreground_background_guidance`, `command_detection_variants` | `<kimi-agent>/bin/kimix_native/tools.py` `_compat_*` bodies | loaded by path as a package alias; the public kimi-agent APIs delegate to exactly these bodies (asserted on their AST) |
| `find_error_line_index`, `truncate_lines` | `src/kimix/tools/common.py` | same by-path module |

Corpora: kimi-agent's own vectors (`tests/test_token_filter.py`
`rtk_available` fixture, `tests/test_bash.py::TestBashRtkRewrite`,
`tests/test_output_enhance.py`, `tests/native/test_shell_security_equivalence.py`
`*_CORPUS`, read with `ast`), an adversarial corpus (quoting, separators,
subshells, env prefixes, comments, escaped separators, UNC / trailing-separator
path tokens) and fixed-seed fuzz corpora.  C++-side goldens:
`tests/unit/builtin_tools/bash_rtk_goldens.inc` (925 commands -> 7484 byte-exact
rows: 4 rewrite profiles, 3 segment profiles, split, known-command), generated by
`python scripts/gen_bash_fix_data.py --rtk`.

### Discrepancies found and fixed (all in `src/builtin_tools/bash_tool.cpp`)

1. `bash_token_stem` -- `Path(token).stem` was approximated by "text after the
   last separator" instead of CPython's `PurePath._parse_path` tail.
   `rewrite_shell_segment("ls\\")`: C++ `("ls\\", false)`, reference
   `("rtk ls\\", true)`; `"dir/git/"`: C++ refused, reference rewrites;
   `"\\\\git"` (incomplete UNC drive -> stem ""): C++ rewrote, reference does not.
   Fixed with the ntpath splitroot + "drop empty/`.` components" tail and the
   "stem must keep a non-dot character" rule (0 diffs on 40 000 path tokens).
2. Hardline command-word scan -- `\b(rm|rmdir|del)(?:\.exe)?\b` was replaced by a
   whitespace-token scan, which finds the command word only after whitespace:
   `check_hardline_blocked("./rm -rf /")` returned `(false, None)` while the
   reference blocks it (same for `/bin/rm -rf /`, `/usr/bin/rm -rf /`,
   `'/bin/rm' -rf /`, `c:\windows\system32\rm.exe -rf /`, `sh -c 'rm -rf /'`,
   `./format C:`, `/bin/kill 1`).  A hardline-floor bypass.  Fixed with a faithful
   `re.finditer` model (leftmost match, greedy optional `.exe`, resume at
   `match.end()`, group(1) picks the rule) for rm/rmdir/del, kill and format.
   The description text also now matches (`echo 'rm -rf /'` ->
   ``(`/'`)``, not ``(`/`)``).
3. `dd` raw-device rule -- the prefix test accepted `hd*`, `nv*`, `rd*` and
   ignored the `\b` before `of=`: a `dd` writing to `of=/dev/hda` (also
   `of=/dev/nv1`, `of=/dev/rd0`, and `xof=/dev/...`) was blocked although the
   reference runs it.  Fixed to `\bof=/dev/(?:sd|nvme|disk|rdisk)[a-z0-9]*`.
4. `bash_segment_tokens` -- a lone `&` was treated as a segment separator;
   the reference pattern is `;|\|\||&&|\||\n`.  `rm -f & x /`: C++ `(false, None)`
   vs reference `(true, ... (`/`))`.
5. `bash_looks_like_flag` -- `anything-after-slash` starting with a letter was
   treated as a Windows switch; the reference requires every character after `/`
   to be alphabetic (`rmdir /dev/sda /`: C++ blocked, reference does not).
6. Drive-root regex -- `^[a-z]:[\\/]?(?:[\\/]?\*)?$`, and `t.rstrip("/\\")` is
   used for the `~`/`$home` test only: `rm -rf c:**`, `c:*/`, `c://` are not
   blocked by the reference (the port blocked them).
7. Fork bomb -- needs the literal `:|:&` (the port accepted `:|:` + `:&`
   anywhere: the `:(){ :|: x :&` shape was blocked, the reference does not block
   it).
8. `foreground_background_guidance` -- the 12 `_LONG_RUNNING_PATTERNS` were
   rewritten as token comparisons with `run` optional.  False positives:
   `yarn dev`, `npm start`, `pnpm start`, `bun serve` produced the
   long-running hint (the reference requires `<runner> run <verb>`); false
   negatives: `vite.dev`, `./node_modules/.bin/vite`, `(vite)`, `vite;` (the
   reference patterns carry real `\b` boundaries).  Fixed by evaluating the
   patterns as bounded literal matches over the whitespace-collapsed text.
   `tests/unit/builtin_tools/test_bash_tool.cpp` used to assert the buggy
   behaviour (`yarn start` / `pnpm watch` hint); corrected to the reference's
   expectations, with the regression vectors added.

Also fixed in `scripts/gen_bash_fix_data.py`: the generator wrote every file with
`Path.write_text`, which on Windows converted `bash_tool.cpp` and both `.inc`
files to CRLF (a whole-file diff against the LF blobs in git).  It now writes
with `newline=""`.  Re-running `--all` reproduces the committed data tables and
the 2946/100 fix goldens **byte-for-byte** (verified with `git diff --numstat`).

### Verification (this round)

- `python -m pytest python/tests/test_parity_bash.py -q` -> `1004 passed`
  (fuzz: 6000 split, 12000 x 5 rewrite profiles, 2500 x 3 segments, 20000
  hardline, 20000 guidance, 4000 exit-code, 3000 annotate, 400 truncation).
- `python scripts/build_locked.py -- xmake build test_builtin_bash` +
  `bin/debug/test_builtin_bash.exe` -> `all tests passed
  (18933 asserts in 60 tests)`, 2946/2946 + 100/100 fix goldens and 7484/7484 RTK
  goldens byte-exact.
- `python scripts/check_cpp_syntax.py` on `bash_tool.cpp` and
  `test_bash_tool.cpp` -> OK.

### Known deviations (unchanged, documented)

- ASCII gate: `find_error_line_index` uses ASCII `\b` (Unicode `\b` in the
  reference), the safety kernels return "not blocked"/"no hint" for non-ASCII,
  and `_read_shell_word`/`lstrip()` do not treat `\xa0`/`\x1c`-`\x1f` as space.
  kimi-agent's own callers gate with `str.isascii()` (`safety.py`) before using
  the native kernels.  `test_ascii_gate_is_documented` pins each difference.
- Reference `Path(...).stem` is `WindowsPath` on Windows and `PosixPath`
  elsewhere; the kernel implements the Windows flavour (the reference host).
- Spawn layer (report only, `process_runner.cpp` + `Bash::run`):
  the C++ argv is `[bash, --noprofile, --norc, -c, "set -o pipefail 2>/dev/null; <cmd>"]`
  while the reference builds ["-c", "export MSYSTEM=; " + "set -o pipefail; " + cmd]
  for a Git Bash install and neutralizes MSYSTEM through that command prefix
  (_MSYSTEM_NEUTRALIZE_PREFIX), not through the child environment (its
  docstring states the parent-environment spelling does not stick for Git Bash
  and MSYS2 re-injects it when absent).  The C++ only sets MSYSTEM= in the
  child env, so the neutralization likely does not take effect; the extra
  2>/dev/null also hides a set -o pipefail failure the reference surfaces.
  FIXED in the follow-up round: Bash::run now composes the script through
  bash_spawn_script(bash_path, command) = _with_msystem_neutralized(
  _PIPEFAIL_PREFIX + cmd) - "export MSYSTEM=; " only when
  bash_is_git_bash_install(bash_path) probes the <root>/cmd/git.exe marker
  (both the <root>/bin and <root>/usr/bin layouts, drive-anchored so the
  check is CWD-independent), then "set -o pipefail; " with no stderr
  suppression; the useless child-env MSYSTEM= delta was dropped from
  bash_native_env. Pinned by bash_spawn_script_composition and
  bash_is_git_bash_install_marker in tests/unit/builtin_tools/test_bash_tool.cpp.
- Harness hazards found while building this round (repros in the task report):
  a staged `<kimi-agent>/bin/runtime_py.pyd` shadows the freshly built extension
  once `kimi_cli` is imported, and `python/tests/test_history_index.py` puts
  `<kimi-agent>/kimi-cli/src` first on `sys.path`, which makes `import kimix`
  resolve to the 22-line shim package for every later test module.
