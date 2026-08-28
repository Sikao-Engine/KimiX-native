# bash tool — C++ implementation report

Worktree: `C:/dev/kimix_wt/bash` (branch `agent/bash`), base commit `48bddc9`.
Plan: `C:/dev/kimi-agent/plans/bash.md` (§3.1, §3.3, §3.4 + AGENT_TASK.md scope).
Namespace: `kimix::builtin_tools::bash`.

## Files

- `src/builtin_tools/bash_tool.h` — public API, contract comments, ownership notes.
- `src/builtin_tools/bash_tool.cpp` — implementation (pure kernels, no subprocess spawning).
- `tests/unit/builtin_tools/test_bash_tool.cpp` — Boost.UT behavioural tests incl. golden vectors.
- `src/builtin_tools/reports/bash.md` — this report.

## Function-by-function mapping

### Exit-code semantics (plan §3.1; source of truth `src/kimix/tools/file/bash/output_enhance.py`)

| C++ symbol | Python reference | Notes |
|---|---|---|
| `has_top_level_pipe` | `_has_top_level_pipe` (57–99) | Hand-written char scanner: quote state (`'`, `"`, `` ` ``), backslash escape, paren depth; a single `|` at depth 0 that is not part of `||` returns true. Byte-exact mirror incl. the reference's quirks (backslash escapes inside double quotes; `||` handled by adjacent-pipe checks; no heredoc awareness — see deviations). |
| `base_command_name` | `_base_command_name` (41–54) | `strip()`, take text after the last `&&`/`||`/`|`/`;`, then first non-assignment word; strip directory; drop `.exe` (case-insensitive). Ported locally (kimix-llm cannot link the runtime_py copy). |
| `interpret_exit_code` | `interpret_exit_code` (119–157) | Returns `None` for exit 0/None/unknown; **SIGPIPE rule first** (exit 141 + `has_top_level_pipe` → the exact SIGPIPE message), then grep/egrep/fgrep/rg/ag/ack exit 1, diff/colordiff exit 1, find exit 1, test/`[` exit 1, curl 6/7/22/28 notes, git exit 1. Message strings are byte-identical (the git message contains U+2014 EM DASH `E2 80 94`, verified against the reference). |
| `is_expected_exit` | `is_expected_exit` (160–176) + `_is_expected_exit_py` (102–116) | 141 + top-level pipe, or exit 1 for the grep family, diff/colordiff, test/`[`, find. |

### Output truncation with error preservation (plan §3.3; source of truth `src/kimix/tools/common.py`)

| C++ symbol | Python reference | Notes |
|---|---|---|
| `error_keywords` / `error_keyword_count` | `_ERROR_KEYWORDS` (334–345) | Exact 36-entry table, reference order. `_ERROR_PATTERN` (347–350) is `\b(?:kw|...)\b` + `IGNORECASE`; the native matcher replicates ASCII `\b` via word-char tests on both sides of each keyword run (keywords containing spaces like `permission denied` get boundary tests on first/last chars only). |
| `find_error_line_index` | `_find_error_line_index` (353–358) | 1-based index of the first line containing any keyword; `nullopt` otherwise. Line splitting mirrors `str.splitlines()` on LF/CRLF/CR only (the plan's §8 risk note: `filter_output` normalizes line endings first). |
| `truncate_lines` | `_truncate_lines` (1100–1164) | Byte-exact: unchanged when `n <= max_lines` or `max_lines <= 0`; `head_n = max_lines // 2`, `tail_n = max_lines - head_n - 1`; fold marker `"\n\n[... {omitted} lines omitted{note} ...]\n\n"`; error preservation keeps `lines[max(head_n, e-ctx) : min(n-tail_n, e+ctx+1)]` with note `" ({k} error-context line(s) preserved)"`. |

### RTK command rewrite scanner (plan §3.4; source of truth `src/kimix/tools/common.py`)

| C++ symbol | Python reference | Notes |
|---|---|---|
| `split_shell_segments` | `_split_shell_segments` (1268–1347) | Splits on `;`, `&&`, `||`; a single `|`/`&` stays in the segment. Quote helpers `_find_ansi_c_end` (1167–1179), `_find_backtick_end` (1182–1194), `_find_dq_end` (1197–1224), `_find_matching_paren` (1227–1265) are ported as internal static helpers. |
| `is_known_rtk_command` | `_is_known_rtk_command` (456–460) + `_RTK_KNOWN_COMMANDS` (363–439) | Static table mirror, `find` intentionally excluded; strips `.exe`, ASCII case-insensitive. |
| `rewrite_shell_segment` | `_rewrite_shell_segment` (1429–1466) | Skips `RTK_DISABLED=1`, shell assignments (`^[A-Za-z_][A-Za-z0-9_]*=`), prefix modifiers sudo/time/nohup/nice; stems the token (`strip("\"'")` + `Path.stem` incl. `\`/`/` separators and the leading-dot rule) and inserts `rtk ` / `& rtk `. |
| `maybe_rewrite_shell_command_with_rtk` | `_maybe_rewrite_shell_command_with_rtk` (1469–1538) | Decision kernel. The Python-side gates `_rtk_available()`/`_rtk_binary_path()` are injected as `rtk_available`/`rtk_binary_path` parameters (kernel stays pure). Multi-segment commands are never rewritten (glued-output rationale in the reference comments 1518–1525). |

### Bounded-run capture/timeout/kill policy state machine (AGENT_TASK.md scope)

Not a byte-exact port of one function — a pure decision kernel abstracting the
bounded "run and capture" loop (streamed chunks + elapsed ms → decision). It
follows the ordering documented in `background/utils.py` `wait_for_output`
(307–369) and `bash_tool.py.__call__`:

1. drain every buffered chunk (bounded append),
2. test the wait pattern against the FULL accumulated output,
3. a process-exit event completes the run (the caller's `thread_is_alive`
   check takes the completion path even when the total timeout was reached on
   the same event),
4. total-timeout check (`elapsed >= timeout`; `timeout <= 0` fires
   immediately),
5. inactivity check only when `inactivity_timeout_ms > 0`.

| C++ symbol | Python reference | Notes |
|---|---|---|
| `bounded_append_capture` | `background/utils.py` `bounded_append` (42–76) | Character-based (code points) like Python `len(str)` slicing: head `int(cap*0.4)` + marker `"\n[... (output truncated, keeping first N and last M chars)]\n"` + tail `cap - head`. The marker is appended on top (not budgeted), exactly like the reference. |
| `capture_machine` | `wait_for_output` (307–369) policy | Pattern stop / complete stop / timeout kill / inactivity stop; replays the stop decision after finish; records a late `process_exited` exit code. |
| `process_exited_banner` | `common.py` ProcessStream (2118–2124) | `"\n[Process exited with code {rc}, error at line {n}]"` / `"\n[Process exited with code {rc}]"`. |

## Left in Python (with the plan's justification)

- **Subprocess spawning** (asyncio/create_subprocess_exec, process-tree
  killing, Windows job objects): stays in Python — `plans/bash.md` §1 keeps the
  subprocess in Python; only the CPU kernels move to C++.
- **`detect_self_kill` / `self_kill_hint`**: owned by the pwsh agent per
  `src/builtin_tools/README.md` cross-tool ownership map. Not implemented here.
- **RTK availability gates** (`_rtk_available`, `_rtk_binary_path`), the
  `_is_known_rtk_command`-adjacent environment probes, and any OS probes:
  injected into the pure kernel as parameters (`plans/bash.md` §3.4 keeps the
  gates Python-side; the known-command table was moved to C++ as a static
  table since it is a pure decision input).
- **Non-ASCII routing**: the kernels are ASCII-gated; the shim routes
  non-ASCII commands/output to the `_compat` Python mirrors (project-wide
  convention, `plans/bash.md` §8 risks).

## Deviations / notes

1. **No 126/127/128+N exit-code rules.** The task brief listed "signal/SIGPIPE/
   126/127/128+N rules" as a possible part of `interpret_exit_code`. The actual
   Python reference (`output_enhance.py` 119–157) contains **no** 126/127/128+N
   handling — only SIGPIPE-141 and the well-known-command table. Per the task
   instruction ("follow the reference and record the deviation"), the kernel
   implements exactly the reference and omits 126/127/128+N.
2. **`has_top_level_pipe` is not heredoc-aware.** The task brief says
   "quote/heredoc-aware"; the reference scanner has no heredoc state, so a `|`
   inside a heredoc body at depth 0 is reported as a top-level pipeline
   (verified: `'cat <<EOF\nx | y\nEOF'` → True). Followed the reference.
3. **`kimix::nullopt` does not exist in kimix-core** (only `std::nullopt`);
   the port uses `std::nullopt`. Mechanical.
4. **`_ERROR_KEYWORDS` has 36 entries** (not 40 as an early plan/header draft
   said). Verified against `common.py` 334–345.
5. **State-machine event order** is a documented policy choice (see above),
   not a line-by-line port of `wait_for_output`; the reference loop and the
   caller's `thread_is_alive` re-check in `bash_tool.py` justify completion
   winning over a same-event timeout.
6. **A prior agent session left untracked `bash_tool.h/.cpp` in this
   worktree** despite the task stating the tree was clean. I reviewed them
   line-by-line against the Python reference, fixed the `kimix::nullopt`
   compile error (3 sites), corrected the header comment (36 not 40 entries),
   added `#include <utility>`, then added the tests/report and verified by
   build + run. No behavioural changes were needed — the kernels already
   matched the reference (confirmed by golden vectors).

## Follow-up phase: moving the actual spawn into C++ (reproc)

`kimix-reproc` is vendored (`src/ext/reproc`, `#include <reproc/reproc.h>`,
`#include <reproc++/reproc.hpp>`) and already a dependency of kimix-llm. A
follow-up phase that replaces the Python subprocess with a C++ spawn would use:

- `reproc_process` / `reproc_options` (working dir, environment, redirect
  setup), `reproc_start`, `reproc_poll` (`REPROC_DEADLINE` / `REPROC_TIMEOUT`
  event sources), `reproc_read` (stdout/stderr drains feeding
  `capture_machine::on_event`), `reproc_wait`, and `reproc_stop` with
  `REPROC_STOP` / `REPROC_TERMINATE` / `REPROC_KILL` stop actions (the
  `timeout_kill` / `inactivity_stop` decisions map to `reproc_stop` +
  `REPROC_KILL`). The C++ convenience wrapper is `reproc::process`
  (`reproc++/reproc.hpp`). No process is spawned in the unit tests; the
  state machine is exercised purely with synthetic events.

## Verification

- `xmake f -m debug -y -c`
- `xmake build kimix-llm`
- `xmake build test_builtin_bash`
- `./bin/debug/test_builtin_bash.exe` → all tests pass.

Golden vectors were captured by running the actual Python reference
(`output_enhance.py`, `common.py`) and embedded in the Boost.UT test file:
21 `has_top_level_pipe` vectors, 40 exit-code vectors
(`interpret_exit_code` + `is_expected_exit`), 13 `find_error_line_index`
vectors, 14 `truncate_lines` vectors, 28 RTK rewrite vectors, plus
behavioural coverage for the state machine, banner, segments, and keyword
table.

Test/assert counts: see the test run output (Suite 'global': all tests passed).
