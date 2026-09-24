# Run built-in tool — C++ implementation report

Reference: `C:/dev/kimi-agent/src/kimix/tools/file/run.py` (799 lines) + the
kernels it reuses (`kimix/tools/common.py`, `kimix/tools/file/bash/output_enhance.py`,
`kimix/tools/file/bash/safety.py`, `kosong/tooling/__init__.py`).

Files touched:

- `src/builtin_tools/run_tool.cpp` / `run_tool.h` — fixes below
- `tests/unit/builtin_tools/test_run_tool.cpp` — new golden/behaviour tests
- `tests/unit/builtin_tools/run_goldens.inc` — extended (regenerated)
- `scripts/gen_run_data.py` — new golden generator (`--write` / `--check`)
- `python/tests/test_parity_run.py` — new differential test (17 tests)
- `src/builtin_tools/reports/run.md` — this report

## Function map (kernels this round touched)

| C++ symbol | Python reference | Notes |
|---|---|---|
| `dedup_output` (new) | `common._dedup_output(output, threshold=3, max_block_lines=1)` | `Counter` semantics: collapse a line whose **total** occurrence count is `> 3`, marker carries that total; lines re-joined with `\n` after a `splitlines()` split (normalizes CRLF/CR, drops the trailing terminator). |
| `shape_output` (new) | `common._token_filter_output` (portable stages) | `apply_dedup = token_kill && !rtk_rewritten`; then `dedup_output`; then `_truncate_lines(max_lines, preserve_errors=True, error_context_lines=2)`; `changed` mirrors `output != original_output`. |
| `success_message` / `failure_message` (new) | `run.py` 557-559 / 586-587 | `success` / `[rtk] success`; exit-code meaning or `expected non-zero exit`; `failed` / `[rtk] failed` + ` Hint: <hint>`. |
| `rn_int_param` | `RunParams.timeout` / `max_lines` through `CallableTool2.call` → `_repair_dict_for_model` (lax coercion + `_clamp_numeric_value`) | numeric input is **clamped** into `[ge, le]`; coerced input (bool / string / integral float) is validated strictly. |
| `rn_bool_param` | same repair pass (`shell`, `run_in_background`) | pydantic lax bool: ints/floats by truthiness + `true/false/yes/no/on/off/1/0`; uncoercible → error. |
| `rn_maybe_apply_rtk` (new) + `run_config::run_rtk_check` | `run.py` 369-380 | `[rtk, executable, args...]` argv, display `rtk ...`, `[rtk]`-prefixed messages, **local dedup skipped** for rtk rewrites. |
| `Run::operator()` shaping/annotate order | `run.py` 486-527 | `annotate_failure` now receives the *post-process* output (like the reference), not the raw capture. |

## Discrepancies found and fixed (each had a failing repro before the fix)

1. **Dedup used the wrong algorithm + wrong marker count.**
   `run_tool.cpp` called `builtin_tools::dedup_lines` (a port of
   `output_utils.dedup_lines`: *consecutive* runs, marker `run_len - 1`) instead of
   `common._dedup_output` (Counter over the whole output, collapse at count `> 3`,
   marker = total count).
   Repro: `"ERROR\n" * 10` → C++ `"ERROR  (9 repeats)"`, Python
   `"ERROR  (10 repeats)"` (kimi-agent's own `tests/test_run.py::
   test_success_message_includes_original_path_after_dedup` feeds exactly that
   string). Also `"ERROR\n" * 3` was collapsed by the C++ and is *not* by the
   reference.
2. **Dedup re-join dropped the trailing newline / kept CR bytes, and only ran
   when a collapse happened.** The reference always rebuilds the output as
   `"\n".join(str.splitlines())`, so `"a\nb\n"` → `"a\nb"` and `"a\r\nb\r\n"` →
   `"a\nb"`; the C++ split on `'\n'` only and left the text untouched otherwise.
3. **Head/tail fold hid the first error.** `truncate_lines(..., preserve_errors=false, 2)`
   was called with the wrong flag; the reference default is `preserve_errors=True`
   (+ the `" (N error-context line(s) preserved)"` fold note). Repro: 30 filler
   lines + `error: boom` + filler, `max_lines=6` → Python keeps `error: boom` and
   the note, C++ folded it away.
4. **`annotate_failure` saw the raw capture.** `run.py:527` computes the hint from
   the post-process output; the C++ passed `rr.output`.
5. **RTK rewrite was not implemented** (`run_config::run_rtk_check` existed but was
   never called): rtk-known commands were not re-run through `rtk`, the display
   command/messages lacked the `rtk`/`[rtk]` form, and local dedup was not
   skipped. Now mirrors `run.py` 369-380 and
   `tests/test_run.py::test_run_prepends_rtk_for_known_command`. Gated on the
   callback, so hosts that do not install it are unaffected.
6. **Out-of-range / coercible parameters were rejected instead of clamped.**
   `timeout: 0` / `901` / `1000`, `max_lines: 2`, `timeout: "45"`, `timeout: true`,
   `run_in_background: "yes"` all succeed in kimi-agent (clamp/coercion in the
   argument-repair pass) but produced `invalid_input` in C++. Now clamped/coerced;
   values that only *became* numeric through coercion still fail exactly like the
   reference (`"0"`, `7.9`, `false`, `"abc"`).

## Verified correct (looked suspicious, settled by evidence)

- `shlex.split` (both posix modes) / `shlex.quote` / `shlex.join`: 296 golden rows
  (legacy corpus + edge cases + deterministic LCG fuzz) regenerated from the host
  CPython and re-verified live by the pytest.
- `_cd_prefix` quotes (`cd '/tmp/a b' && `, `cd 'C:\it''s'; `) and the shell-mode
  `cd` prefix.
- Exit-code classification and the "expected exit" cases: `grep` (1) → *No matches
  found*, `diff` (1) → *Files differ*, SIGPIPE 141 with a top-level pipe →
  expected, 141 without a pipe → not expected, `None`/0 → `None`/`False`.
- `_find_error_line_index` and the fold marker text against the reference over the
  corpus + fuzz.
- `annotate_failure`: Run uses `kimix::runtime::tools::annotate_failure`
  (`shell_safety.cpp`), which is **not** ASCII-gated and therefore matches the
  pure-Python reference even for non-ASCII output (the bash-owned wrapper
  `bash::annotate_failure`, exposed as `shell.annotate_failure`, *is* gated —
  documented deviation, pinned in `test_parity_run.py`).
- The full `_token_filter_output` pipeline (ANSI strip via `rich` +
  `micro_compress` + dedup + fold + temp-file export) was executed against the
  ported stages for all 68 shape vectors; the 9 vectors where they differ are
  exactly the `micro_compress` ones (CRLF/whitespace-only/control-byte inputs) and
  are pinned explicitly by `test_micro_compress_divergence_is_documented`.

## Known gaps (not ported, documented)

- **Temp-file lifecycle.** `_export_to_temp_file_async` / `_save_original_output_async`
  / `_original_saved_message` / `_maybe_export_output_async` (OUTPUT_LIMIT 16384)
  are Python-side: the C++ does not save the original stream, so the reference's
  ` [original saved to ...]` message suffix and the `output_path` it reports are
  missing; a failure with non-empty output is not re-homed into
  `saved to file \`<temp>\`` (`run.py` 530-542). `> 65536` chars only sets
  `output_truncated` instead of summarizing.
- **`micro_compress` and the `rich` ANSI parser** (the two lossy stages ahead of the
  dedup). ANSI escapes in child output therefore survive in the C++ block.
- **Spawn/capture layer** (`process_runner.cpp`, shared with bash): the reference
  prefixes every stderr chunk with `[stderr] `, appends
  `\n[Process exited with code N]` (or `, error at line L`), runs `filter_output`
  (ANSI strip + CRLF normalize) per chunk, and caps the capture at
  `BACKGROUND_MAX_OUTPUT_CHARS`. The C++ merges stderr into the stdout file
  (dup2) and does none of the three. Report-only: it is a shared layer and the
  bash tool has the same gap.
- **Timeout hand-off.** `run.py` registers the foreground run as a task, so a
  timeout returns `Running in background. task_id: \`<real id>\`. ...` with partial
  output and leaves the child alive; the C++ reports the guidance message with a
  `(detached)` placeholder id, and the `run_in_background + wait_for_pattern`
  path does not return the `status: running` block.
- **`python -c` payloads > 30000 chars** are not moved into a script file
  (`run.py` 391-399, `k_inline_python_limit` is still unused).
- **Non-optional fields sent as explicit JSON `null`** (`timeout`, `shell`,
  `run_in_background`, `mode`, `command`) are treated as *absent* by the C++ while
  pydantic rejects them.
- `_is_known_rtk_command` / `is_known_rtk_command` stem handling uses
  WindowsPath semantics (the reference host).

## Verification

```
python scripts/gen_run_data.py --check            -> run_goldens.inc is up to date (61509 bytes)
python scripts/build_locked.py -- xmake build test_builtin_run   -> build ok
./bin/debug/test_builtin_run.exe                  -> Suite 'global': all tests passed (1676 asserts in 68 tests)
python scripts/build_locked.py -- xmake build test_builtin_param_aliases -> build ok
./bin/debug/test_builtin_param_aliases.exe        -> all tests passed (76 asserts in 27 tests)
python -m pytest python/tests/test_parity_run.py -q -> 17 passed
```

Golden provenance: the legacy `kShlexGoldens` / `kQuoteGoldens` / `kJoinGoldens`
rows are reproduced byte-for-byte (same order, same values) by
`scripts/gen_run_data.py`, so the committed table diff is additive only.
