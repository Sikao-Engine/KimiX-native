# python tool — C++ implementation report

Tool: `python` (kimi-agent built-in tool)
Files: `src/builtin_tools/python_tool.h` + `src/builtin_tools/python_tool.cpp`
Tests: `tests/unit/builtin_tools/test_python_tool.cpp`
Namespace: `kimix::builtin_tools::python`

## 1. Source of truth

The plan file `C:/dev/kimi-agent/plans/python.md` named in the task brief was
**missing** in this environment (`C:/dev/kimi-agent/plans/` is empty).  This
port therefore followed, in order:

1. `AGENT_TASK.md` (worktree root) — the exact scope list for this tool.
2. `C:/dev/kimi-agent/src/kimix/tools/py/__init__.py` — `_resolve_python` /
   `_resolve_python_uncached` (103–150), `_build_env` (152–201),
   `_module_not_found_hint` (203–213).
3. `C:/dev/kimi-agent/src/kimix/tools/common.py` — `_create_script_file`
   (722–741), `_extract_export_path` (932–945),
   `_build_session_output_block` (948–991).
4. `C:/dev/kimi-agent/src/kimix/tools/security.py` — `scrub_child_env`
   (47–77).
5. `C:/dev/kimi-agent/src/kimix/tools/background/utils.py` —
   `BackgroundStream.wait_for_output` (307–369), the `pattern.search(output)`
   step (338).

Behaviour was locked by running the reference function bodies (copied verbatim)
against golden vectors on Python 3.14 (with the `regex` module), and the C++
kernels were checked byte-for-byte against those goldens.

## 2. Function-by-function mapping

| C++ symbol | Python reference | Notes |
|---|---|---|
| `ScriptFileWriter::plan_path` / `plan_script_path` | `common.py _create_script_file` 722–741 | Pure path arithmetic: `<base_dir>/<index><ext>`, monotonic index, thread-safe via `kimix::spin_mutex`. No filesystem calls in the kernel (no `mkdir`, no `write`); the caller owns the temp-folder lifecycle. The reference's `_temp_idx` counter is represented by the writer's `next_index()`. |
| `resolve_python_exe` | `py/__init__.py _resolve_python_uncached` 120–150 | Decision kernel: (1) `override` (`KIMIX_PYTHON_EXECUTABLE`) if non-empty and exists; (2) walk up from each search base probing `<base>/.venv/Scripts/python.exe` then `<base>/.venv/bin/python`; (3) `VIRTUAL_ENV` probing `<venv>/Scripts/python.exe` then `<venv>/bin/python`; (4) `fallback` (`sys.executable` — injected, not derivable from C++). File existence is injected through a `kimix::function<bool(kimix::string_view)>` probe (`Path.is_file()`). Returns `std::nullopt` only when every candidate is missing and there is no fallback (the Python side always falls back to `sys.executable`). |
| `scrub_child_env` | `security.py scrub_child_env` 47–77 | Byte-exact: keep when the UPPERCASE name starts with a safe prefix; otherwise drop when it contains a secret substring; otherwise keep. Order preserved, input never mutated. Non-ASCII names are compared byte-wise (agrees with `str.upper()` for ASCII names; the Python native gate is `name.isascii()`). |
| `prepare_python_env` (delta) | `py/__init__.py _build_env` 152–201 | Computes the *delta* (added/changed vars) instead of the full env dict (per AGENT_TASK "env_delta" scope; the caller applies it over the already-scrubbed base snapshot). Venv detection via injected `pyvenv.cfg` probe. `std::nullopt` == the reference's `None` zero-copy fast path (not a venv AND share bin already first in PATH). Delta order matches the reference assignment order: `VIRTUAL_ENV` first, then `PATH`. `already_first` uses the same startswith-or-equal rule on the raw parent PATH. |
| `module_not_found_hint` | `py/__init__.py _module_not_found_hint` 203–213 | Byte-level scanner for the ASCII marker `ModuleNotFoundError: No module named '…'` / `"…"` (regex `['"]([^'"]+)['"]`). Returns the byte-exact hint string (leading space included) or `""`. Handles single/double quotes, mismatched quotes, newlines inside the module name, first-match-wins, and the non-empty module-name rule. |
| `build_session_output_block` | `common.py _build_session_output_block` 948–991 | Ordered block: `task_id, status, exit_code, exit_code_meaning, failure_hint, output: |, output_truncated, output_path, wait_matched, elapsed_seconds, original_path`. Falsy optional strings render `null`; `output_truncated`/`wait_matched` render `true|false`; `elapsed_seconds` uses `{:.2f}` (std::format, correctly-rounded — verified equal to Python on the golden vectors including 2.675 → "2.67"). `textwrap.indent` semantics: every line containing at least one non-whitespace char gets the 2-space prefix (blank/whitespace-only lines are left untouched); output is `rstrip("\n")`'d first. |
| `extract_export_path` | `common.py _extract_export_path` 932–945 | Four markers in reference order: `` exported to file ` ``, `` added to file ` ``, `exported to file: `, `added to file: `. Returns everything after the first marker with ALL trailing `]` and `` ` `` chars stripped (`str.rstrip("]`")`), or `std::nullopt` when no marker is present (empty tail yields an empty string, matching the reference). |
| `classify_wait_pattern` / `match_wait_pattern` | `background/utils.py wait_for_output` 307–369 (pattern step 338) | Native subset: `literal` (no metacharacters → exact substring search, byte-exact with `pattern.search`), `glob` (fnmatch-style `* ? [seq] [!seq]` with ranges, wrapped in leading/trailing `*` for search semantics), `unsupported` (regex-only metacharacters `. ^ $ + { } \ | ( )` or non-ASCII → `tool_status::unsupported`, so the caller routes to the Python regex engine). Empty pattern → `tool_status::invalid_input`. |

## 3. What deliberately stays in Python

Not ported, per `AGENT_TASK.md` scope and the header's non-goals list (the
plan's §3.9 "non-goals" wording could not be quoted because `plans/python.md`
is missing; the same boundaries are stated in `python_tool.h`):

- Process spawn/stream/wait/input (`ProcessTask` + `BackgroundStream` — async
  I/O, callback threading, process-tree registry/atexit cleanup,
  `kill_child_tree`) — orchestration, not a CPU kernel.
- `_syntax_check_error` — requires CPython `compile()`.
- Long-output summarization (`_summarize_long_output_async`) — LLM network
  call.
- `asyncio.Semaphore(8)` concurrency, `_resolve_python` result caching,
  `_python_config` reading.
- rtk marker parsing (`parse_rtk_rg_output`), `_display_temp_path` display
  normalization, `_original_saved_message` formatting, `_maybe_export_*` /
  `_save_original_output_async` export pipeline, `_temp_set` keyed-append
  variant of the script writer (only the export pipeline uses it).
- Temp-folder lifecycle/cleanup (the kernel only plans paths).

## 4. Deviations and design notes

1. **Plan file missing.** `C:/dev/kimi-agent/plans/python.md` does not exist;
   the port used `AGENT_TASK.md`'s scope + the Python source + captured goldens
   (see §1).  All §3/§7/§8 content from the plan was therefore reconstructed
   from the task brief; no plan-only requirement is known to be unaddressed.
2. **`wait_for_pattern` glob semantics** (documented in `python_tool.h`):
   the reference compiles the pattern with Python's `regex` engine, so e.g.
   `ready*` is a regex (matches "ready" without the trailing `*`), while the
   native glob matches "ready" followed by anything.  Patterns that are
   invalid regexes in the reference (e.g. `*done*` → "nothing to repeat")
   are treated as valid globs natively instead of surfacing the reference's
   `Invalid wait_for_pattern` error.  Patterns with regex-only metacharacters
   return `tool_status::unsupported` so the Python mirror handles them with
   exact reference behaviour.
3. **`build_session_output_block` splitlines subset.** The kernel splits on
   the ASCII separators (`\n`, `\r`, `\r\n`, `\x0b`, `\x0c`); Python's
   `str.splitlines()` additionally splits on `\x1c`–`\x1e`, `\x85`,
   `U+2028/2029`.  Those bytes are left inside the line.  Outputs routed to
   the native kernel are ASCII-gated at the binding layer, so this matches in
   practice.
4. **Path arithmetic keeps the base's own separators.** The kernels append
   components with the host separator but do not re-normalize separators
   already present in the injected base/`python_exe` strings (pathlib would
   normalize `C:/x` → `C:\x` on Windows).  The binding layer passes host-style
   paths, so real callers see the same strings the Python side produces.
5. **`prepare_python_env` returns a delta, not the full env dict.** The
   reference returns a complete snapshot (with scrubbing applied when
   `scrub_env` is on); the C++ kernel returns only the variables it adds or
   changes (`VIRTUAL_ENV`, `PATH`), which the caller applies over the
   scrubbed base.  This is the `env_delta` contract in `AGENT_TASK.md`.
6. **Test fixes against the Python reference** (the draft's tests had never
   been run):
   - `session_block_indent_predicate` expected `" beta"` (1 space) but CPython
     `textwrap.indent` indents every line that is not whitespace-only, so
     `" beta"` becomes `"   beta"` (3 spaces).  Golden corrected.
   - All path literals were made platform-aware (`host()` helper): the
     kernels join with the host separator, so forward-slash fixtures only
     matched on POSIX.  Tests now pass on Windows.
   - Added MSVC-friendly `bases()` helper for `std::span` arguments (MSVC
     rejects braced-init-list → span) and replaced boost::ut `eq()` on scoped
     enums with direct `==` comparisons (enums are not streamable).
   - Added glob character-range support (`[0-9]`) to `glob_match_here`
     (fnmatch requires ranges; the draft's matcher only did literal sets).
7. **Bug fixed in the draft**: `resolve_python_exe` probed
   `<venv>/.venv/Scripts/python.exe` for the `VIRTUAL_ENV` step; the reference
   probes `<venv>/Scripts/python.exe`.  Also `kimix::nullopt` does not exist
   in kimix-core (only `std::nullopt`).

## 5. Verification

```
xmake f -m debug -y -c
xmake build kimix-llm
xmake build test_builtin_python
./bin/debug/test_builtin_python.exe
```

Result: `Suite 'global': all tests passed (159 asserts in 30 tests)`.

Test registration used locally (restored before commit, not committed):

```
builtin_tools_test("test_builtin_python", "unit/builtin_tools/test_python_tool.cpp")
```

## 6. Test counts

- 30 Boost.UT tests, 159 assertions, all passing on Windows x64 (MSVC 2022,
  debug).
- Coverage: plan path naming/thread-safety (3), interpreter resolution
  precedence + walk-up + VIRTUAL_ENV + fallback (8), env scrub (1) + env
  delta fast-path/prepend/dedup/empty/venv/no-pyvenv (6), module-not-found
  hint (3), session block full/empty/indent/elapsed (4), export-path (1),
  wait-pattern literal/glob/unsupported/empty (4).
