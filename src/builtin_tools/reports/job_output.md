job_output built-in tool — C++ implementation report

Reference: C:/dev/kimi-agent/src/kimix/tools/background/__init__.py (503 lines,
class `TaskOutput`) + background/utils.py (626 lines: BackgroundStream,
FinishedTask/TaskData, bounded_append) + kimix/tools/common.py
(`_append_elapsed`, `_elapsed_suffix`, `_elapsed_tag`, `_format_elapsed_seconds`,
`_coerce_seconds`, `ELAPSED_REPORT_MINIMUM_SECONDS`) + kosong/tooling
(`ToolOk` / `ToolError` / `BriefDisplayBlock`).

Files touched:

- src/builtin_tools/job_output_tool.cpp / job_output_tool.h — fixes below
- tests/unit/builtin_tools/test_job_output_tool.cpp — golden replay + fixes
- tests/unit/builtin_tools/job_output_goldens.inc — new (64 reference cases)
- scripts/gen_job_output_goldens.py — new golden generator (--dump / --check)
- python/tests/test_parity_job_output.py — new live differential test
  (`runtime_py.tools.bounded_append`, 126 tests)
- src/builtin_tools/reports/job_output.md — this report

Function map (kernels this round touched)

| C++ symbol | Python reference | Notes |
|---|---|---|
| bounded_append (runtime) | background/utils.py 58-92 (+ the tell-guard call site, 66-79) | Verified, no change: code-point (never byte) caps, the `([... keeping first N and last M chars])` marker, the `(content, truncated)` pair. Live differential through `runtime_py.tools.bounded_append` against the *verbatim Python body* AND against the same body driven through the reference's own tell-guard with our kernel behind it, plus kimi-agent's `_compat_bounded_append`. |
| format_duration (new) | common.py `_format_elapsed_seconds` 976-991 | Three forms: `{:.2f}s` < 60s, `<m>m<ss>s` < 3600s, `<h>h<mm>m`. `_coerce_seconds` rejects negatives/NaN/Inf. |
| format_elapsed_suffix / format_elapsed_tag / append_elapsed (new) | common.py `_elapsed_suffix` 1011-1020, `_elapsed_tag` 1023-1033, `_append_elapsed` 1036-1057 | 1.0 s `ELAPSED_REPORT_MINIMUM_SECONDS` gate; empty message becomes the suffix *without* its leading space (`(3.50s)`); idempotent against `_ELAPSED_SUFFIX_RE` (scanned backwards byte-exactly). |
| format_elapsed_cell | `_list_tasks` 183 | Unchanged on purpose: the list table keeps the plain `{:.1f}s` form (95.0 s renders as `95.0s` there, `1m35s` in the tag). |
| display_kind (new) | `job_id.split("_")[0] if job_id else "task"` (`_get_output` 475, `_get_history_output` 315) | The display-block default is "task", *not* the list table's "unknown". |
| record_finished_task / get_finished_task / clear_finished_tasks / finished_task_count (new) | utils.py FinishedTask 110-137, `_store_finished_locked` 543-556, `record_finished_task` 565-571, `get_finished_task` 574-580 | `MAX_FINISHED_TASKS = 25`, oldest evicted first, re-recording refreshes recency + fields, lookup strips the id. Process-wide (one process hosts one session's tasks; the tool instance is built per call by ToolRegistry::create, so per-instance state would not survive a second call). |
| build_history_output_text + jo_write_history_result (new) | `_get_history_output` 275-334 | Serves a job that already left the registry: saved processed output (`(no output)` fallback), `wait_matched:` line, elapsed tag, `\n[retrieved from finished-task history]`; error + `Task 'x' failed` brief for a failed record. |
| _kill_task branch | 191-246 | Records the finished result *before* `remove_task_id`, `_append_elapsed` message, and the two *different* output fallbacks. |
| _get_output branch | 336-487 | Records before dropping the id; `spent_seconds = None` while the job is alive. |

Discrepancies found and fixed (each one is a failing golden / reference repro)

1. **Every response carried a `[status: ...]` suffix the reference never
   produces.** The port appended `\n[status: running|completed|killed]` to
   `output` (and to the failure `output`) and justified it with the tool
   description ("Every response ends with `[status: ...]`").
   Repro: get a running job with `"partial\n"` buffered.
   Python output: `partial\n`. C++ before: `partial\n\n[status: running]`.
   Evidence: a repo-wide grep of the kimi-agent checkout finds the string only
   in the two description literals (`background/__init__.py` 60 and 110) — no
   code path, no display layer, no `kimi_agent_sdk` hook appends it.
   Fix: the suffix is gone from every model-visible string; the state stays
   available as the machine-readable `status_text` field (`running` /
   `completed` / `killed`), and `status_suffix()` is kept only as a tested
   helper. The golden replay fails on `output` for all 20+ `get` cases if the
   suffix is re-added, so the regression is pinned.

2. **Durations used the wrong format (and the wrong sub-second behaviour).**
   The port formatted the elapsed tag/suffix with `{:.1f}s` / `{:.2f}s`; the
   reference uses `_format_elapsed_seconds`, which switches to `1m35s` /
   `1h02m`, and gates the whole annotation on
   `ELAPSED_REPORT_MINIMUM_SECONDS = 1.0`.
   Repro A (95 s job): Python message `(1m35s)`, output
   `slow\n\n[Process completed in 1m35s]`; C++ before ` (95.0s)` /
   `[Process completed in 95.00s]`.
   Repro B (0.4 s job): Python output `quick\n`, message ``; C++ before
   `quick\n\n[Process completed in 0.40s]`, message ` (0.4s)`.
   Fix: `format_duration` + `format_elapsed_suffix` / `format_elapsed_tag` /
   `append_elapsed`, all gated by `k_elapsed_report_minimum_seconds`.

3. **The `message` field never matched `_append_elapsed`.** Python runs every
   returned message through `_append_elapsed(message, spent_seconds)`:
   a finished job with no other message gets `(3.50s)` (the suffix *with* its
   parens, only the leading space stripped); a *live* job gets `""` because
   `_get_output` sets `spent_seconds = None` (line 438) even when the stream
   already knows its elapsed time.
   Repro: get a 3.5 s finished job. Python message `(3.50s)`; C++ before `""`.
   Repro: get a live job whose stream reports 2.0 s. Python message `""`; C++
   before would have produced ` (2.0s)` from the task's elapsed time — the
   golden case `get_running_partial` catches it.
   Fix: `append_elapsed` on every path + the alive/finished split
   (`spent_seconds`), including the idempotence rule (`_ELAPSED_SUFFIX_RE`:
   `boom (1m05s)` must not be annotated twice).

4. **A failed kill returned `(no output)` where the reference returns `""`.**
   `_kill_task` has two different fallbacks: `processed if processed else "(no
   output)"` on success and `processed if processed else ""` on failure.
   Repro: kill a job with empty buffered output and a non-zero exit.
   Python `output`: `""`. C++ before: `(no output)`.
   Fix: `kill_failed_output_text` (empty stays empty) next to
   `kill_output_text` (success fallback).

5. **The finished-task history did not exist: a second read of a completed job
   answered "not found".** `_get_output` / `_kill_task` record a `FinishedTask`
   *before* `remove_task_id`, and any later call whose id is no longer in the
   registry is served by `_get_history_output` (which re-applies
   `wait_for_pattern` against the saved raw output and appends
   `[retrieved from finished-task history]`).
   Repro (kimi-agent's own regression test,
   `tests/test_bash.py::TestBashFinishedJobHistory`): `echo hello_world` in the
   background, `job_output` (wait) → completed, then `job_output` again.
   Python: `hello_world … [retrieved from finished-task history]`. C++ before:
   `Task 'bash_1' not found`.
   Fix: `record_finished_task` / `get_finished_task` (25-entry LRU, mirroring
   `_store_finished_locked`) + the history branch in both `get` and `kill` +
   `build_history_output_text`. The golden block
   `history_first_read` → `history_second_read*` (11 cases) walks the whole
   behaviour, including the substitution of a live stream for a stale record
   (`history_shadowed_by_registry`).

6. **`brief` was wrong on every `get` success.** `ToolOk.__init__` ignores the
   `brief` argument whenever a `display_block` is passed, so the reference's
   `result.brief` is `""` for `_get_output` / `_get_history_output` (the text
   "Task output retrieved" is dropped) while the port returned that text.
   Repro: any successful get. Python `brief`: `""`; C++ before: `Task output
   retrieved`.
   Fix: `jo_ok(..., "")` on both success paths (the list/kill/error briefs are
   real and unchanged).

Verified correct (looked suspicious, settled by evidence)

- `bounded_append`: 126 live differential tests (boundary `cap` / `cap+1` /
  `cap-1`, caps 0/1/2/3/negative, `int(cap*0.4)` head and `cap-head` tail,
  CJK/astral/combining strings, empty text, a 2 000 000-char cap, 1500
  fixed-seed fuzz rows and a 60-case chunk-sequence replay) — zero mismatches
  against the reference body, the guarded call path and the `_compat` mirror.
- The `(truncated)` flag is `len(content) + len(text) > cap` in *code points*:
  the port's byte-length fast path is only an early-out and falls through to a
  code-point count, which the Unicode fuzz pins.
- Correct without change: the list table (`| Task ID | Kind | Status | Elapsed
  |`, `-` for a falsy elapsed, `N background task(s)`, `No running tasks.`),
  `not_found_message` wording with the *unstripped* id and only-started task
  list, `task_kind` vs `display_kind` defaults, `output_path` export branches
  (raw output, `"`x` is still running, call `job_output` again, "` prefix,
  backslash→slash display path), the `wait_matched: true|false` line ordering
  (before the elapsed tag, after the export text), param parsing
  (`job_id|task_id`, `wait|block`, legacy `timeout_ms` →
  `max(1, ms // 1000)`, `kill: true` → `action='kill'`, the `[1, 7200]` bounds
  and the "task_id is required for action='kill'." error), and `success ==
  (exit_code == 0)` for every process-backed task.
- The `[1, 7200]`/alias/param surface and every message string in the corpus
  are pinned byte-for-byte by the 64-case golden replay
  (`scripts/gen_job_output_goldens.py` drives the real `TaskOutput` against
  scripted streams; `test_job_output_tool.cpp` replays them and additionally
  checks the id removals, the leftover registry and the `timeout*1000` handed
  to the blocking wait).

Known gaps (reported, not fixed — with the reason)

1. `Invalid wait_for_pattern: <error>`: the reference reports CPython's
   `regex` module text (`missing ), unterminated subpattern at position 0` for
   `(`), the port reports its own scanner's text (`missing closing ')'`).
   The two engines' diagnostics (with positions and construct names) cannot be
   mapped; the goldens therefore exclude the two invalid-pattern cases and say
   so in the generated header.
2. `wait_for_pattern` matching is **literal** on the native wait path
   (`proc::wait_task` uses `snapshot.find(pattern)`), while the reference
   compiles the pattern with `regex` and calls `pattern.search`. `wait=True`
   with `\d+`/`a|b` therefore reports `wait_matched: false` (Python: true).
   The port also validates the pattern with `regex_lite`, which rejects
   Python-valid patterns outside its subset (look-around, backrefs, named
   groups). Repo convention for the sibling tools (python/bash:
   `classify_wait_pattern` / `match_wait_pattern`) is to return
   `tool_status::unsupported` so the caller routes to Python; doing that here
   would have to apply only when `wait=True` (the reference merely *validates*
   an unused pattern when `wait=False`), and job_output has no Python fallback
   in this build — so this is left as a design decision rather than changed.
3. `success()` is derived from `exit_code == 0`; the reference reads the
   worker's own success flag (`BackgroundStream._success`). Identical for every
   process-backed task (bash/run/python return it from the exit code), only a
   hypothetical success-with-no-exit-code stream would differ.
4. The rtk / summarize side channels (`_maybe_export_output_async`,
   `_maybe_export_rtk_original_async`, the `[rtk output exported to: …]` /
   `[original output exported to: …]` suffixes) are not implemented natively;
   the generator stubs them out and the port models `original_path` as an
   injected field (unchanged from the original port).
5. The blocking wait delegates to `proc::wait_task`, so the reference's
   `wait_for_output` / `wait_with_inactivity_timeout` inactivity early return
   (`min(900, timeout)` / `DEFAULT_INACTIVITY_TIMEOUT = 120 s`) is not
   reproduced — the same shared-layer gap the bash/run tools report.
6. `remove_task_id`'s *safety net* (`_record_from_stream`: a best-effort record
   stored when something else drops a task) is not ported. It is currently
   unreachable: `job_output`'s `src.remove` is the only caller of
   `proc::remove_task` in the tree, and both call sites record first.
7. `jo_has_elapsed_suffix` scans ASCII whitespace while the reference's
   `regex` `\s` is Unicode-aware; tool messages are ASCII in practice.
8. `bounded_append`'s `cap` is `int64` in the binding; the Python reference
   accepts a *float* cap and would render `keeping first 400 and last 600.5
   chars`. No caller passes a float (`BACKGROUND_MAX_OUTPUT_CHARS` is an int).
9. The capture cap job_output reads through is the shared `proc` layer's
   `run_options::output_cap_chars`, whose default is **200 000** chars
   (`process_runner.h` 37, `bash_tool.h` 310 — both annotated
   "BACKGROUND_MAX_OUTPUT_CHARS") while the reference's
   `BACKGROUND_MAX_OUTPUT_CHARS` is **2 000 000** (utils.py 37). A job is
   therefore truncated 10x earlier than in Python, with the same marker line.
   The constant lives in the bash/run/python tools' headers (not this tool's
   files), so it is reported rather than changed here.

Verification (green)

    python scripts/gen_job_output_goldens.py            # regenerate
    python scripts/gen_job_output_goldens.py --check    # 64 cases, up to date
    python scripts/build_locked.py -- xmake build test_builtin_job_output
    ./bin/debug/test_builtin_job_output.exe
    → Suite 'global': all tests passed (897 asserts in 63 tests)

    python -m pytest python/tests/test_parity_job_output.py -q
    → 126 passed

Counter-check: re-adding the `[status: …]` suffix (a 2-line probe) makes the
golden replay fail on `output` for every `get` case and the two `[status:`
absence assertions fail — i.e. the tests bite on the pre-fix behaviour.
