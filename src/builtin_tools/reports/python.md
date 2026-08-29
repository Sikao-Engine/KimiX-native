python tool — C++ implementation report

Tool: python (kimi-agent built-in tool)
Files: src/builtin_tools/python_tool.h + src/builtin_tools/python_tool.cpp
Tests: tests/unit/builtin_tools/test_python_tool.cpp
Namespace: kimix::builtin_tools::python

1. Source of truth

This implementation follows the worktree plan:

* `D:\KimiX-native\plans\python.md` — authoritative port plan for the python
  tool C++ kernels.

The Python reference code named in that plan lives under `D:\kimi-agent`
(outside this worktree) and was used as the behaviour reference:

* `src/kimix/tools/py/__init__.py`
  * `_resolve_python` / `_resolve_python_uncached` (103–150) → `resolve_python_exe`
  * `_build_env` (152–201) → `prepare_python_env`
  * `_module_not_found_hint` (203–213) → `module_not_found_hint`
* `src/kimix/tools/common.py`
  * `_create_script_file` (722–741) → `ScriptFileWriter::plan_path` /
    `plan_script_path`
  * `_extract_export_path` (932–945) → `extract_export_path`
  * `_build_session_output_block` (948–991) → `build_session_output_block`
* `src/kimix/tools/security.py`
  * `scrub_child_env` (47–77) → `scrub_child_env`
* `src/kimix/tools/background/utils.py`
  * `BackgroundStream.wait_for_output` pattern step (338) →
    `classify_wait_pattern` / `match_wait_pattern`

2. Function-by-function mapping

| C++ symbol | Python reference | Notes |
|---|---|---|
| `ScriptFileWriter::plan_path` / `plan_script_path` | `common.py` `_create_script_file` 722–741 | Pure path arithmetic: `<base_dir>/<index><ext>`, monotonic index, thread-safe via `kimix::spin_mutex`. No filesystem calls in the kernel; the caller owns the temp-folder lifecycle. The reference's `_temp_idx` counter is represented by the writer's `next_index()`. |
| `resolve_python_exe` | `py/__init__.py` `_resolve_python_uncached` 120–150 | Decision kernel: (1) override (`KIMIX_PYTHON_EXECUTABLE`) if non-empty and exists; (2) walk up from each search base probing `<base>/.venv/Scripts/python.exe` then `<base>/.venv/bin/python`; (3) `VIRTUAL_ENV` probing `<venv>/Scripts/python.exe` then `<venv>/bin/python`; (4) fallback (`sys.executable` — injected, not derivable from C++). File existence is injected through a `kimix::function<bool(kimix::string_view)>` probe. Returns `std::nullopt` only when every candidate is missing and there is no fallback. |
| `scrub_child_env` | `security.py` `scrub_child_env` 47–77 | Byte-exact: keep when the uppercased name starts with a safe prefix; otherwise drop when it contains a secret substring; otherwise keep. Order preserved, input never mutated. Non-ASCII names are compared byte-wise (agrees with `str.upper()` for ASCII names; the Python native gate is `name.isascii()`). |
| `prepare_python_env` (delta) | `py/__init__.py` `_build_env` 152–201 | Computes the delta (added/changed vars) instead of the full env dict (per plan §3.4.4; the caller applies it over the already-scrubbed base snapshot). Venv detection via injected `pyvenv.cfg` probe. `std::nullopt` == the reference's `None` zero-copy fast path (not a venv AND share bin already first in `PATH`). Delta order matches the reference assignment order: `VIRTUAL_ENV` first, then `PATH`. `already_first` uses the same startswith-or-equal rule on the raw parent `PATH`. |
| `module_not_found_hint` | `py/__init__.py` `_module_not_found_hint` 203–213 | Byte-level scanner for the ASCII marker `ModuleNotFoundError: No module named '…'` / `"…"`. Returns the byte-exact hint string (leading space included) or `""`. Handles single/double quotes, mismatched quotes, newlines inside the module name, first-match-wins, and the non-empty module-name rule. |
| `build_session_output_block` | `common.py` `_build_session_output_block` 948–991 | Ordered block: `task_id`, `status`, `exit_code`, `exit_code_meaning`, `failure_hint`, `output: |`, `output_truncated`, `output_path`, `wait_matched`, `elapsed_seconds`, `original_path`. Falsy optional strings render `null`; `output_truncated`/`wait_matched` render `true`/`false`; `elapsed_seconds` uses `{:.2f}` (`kimix::format`, correctly-rounded). `textwrap.indent` semantics: every line containing at least one non-whitespace char gets the 2-space prefix (blank/whitespace-only lines are left untouched); output is `rstrip("\n")`'d first. |
| `extract_export_path` | `common.py` `_extract_export_path` 932–945 | Four markers in **exact plan order**: `"exported to file "`, `"added to file "`, `"exported to file: "`, `"added to file: "`. Returns everything after the first marker with **all** trailing `]` and `` ` `` chars stripped (`str.rstrip("]\`")`), or `std::nullopt` when no marker is present. An empty tail yields an empty string. |
| `classify_wait_pattern` / `match_wait_pattern` | `background/utils.py` `wait_for_output` 307–369 (pattern step 338) | Native subset: `literal` (no metacharacters → exact substring search, byte-exact with `pattern.search`), `glob` (fnmatch-style `* ? [seq] [!seq]` with ranges, wrapped in leading/trailing `*` for search semantics), `unsupported` (regex-only metacharacters `. ^ $ + { } \ | ( )` or non-ASCII → `tool_status::unsupported`, caller routes to Python regex engine). Empty pattern → `tool_status::invalid_input`. |

3. What deliberately stays in Python

Per `plans/python.md` §4:

* Process spawn/stream/wait/input (`ProcessTask` + `BackgroundStream` — async
  I/O, callback threading, process-tree registry/atexit cleanup,
  `kill_child_tree`).
* `_syntax_check_error` — requires CPython `compile()`.
* Long-output summarization (`_summarize_long_output_async`) — LLM network call.
* `asyncio.Semaphore(8)` concurrency, `_resolve_python` result caching,
  `_python_config` reading.
* rtk marker parsing (`parse_rtk_rg_output`), `_display_temp_path` display
  normalization, `_original_saved_message` formatting, `_maybe_export_*` /
  `_save_original_output_async` export pipeline, `_temp_set` keyed-append
  variant of the script writer.
* Temp-folder lifecycle/cleanup (the kernel only plans paths).
* Full-regex `wait_for_pattern` strings that are non-ASCII or contain regex-only
  metacharacters.

4. Deviations and design notes

1. **wait_for_pattern glob semantics (documented in `python_tool.h`):**
   the reference compiles the pattern with Python's regex engine, so e.g.
   `ready*` is a regex (matches `"ready"` without the trailing `*`), while the
   native glob matches `"ready"` followed by anything. Patterns that are
   invalid regexes in the reference (e.g. `*done*` → "nothing to repeat")
   are treated as valid globs natively instead of surfacing the reference's
   "Invalid wait_for_pattern" error. Patterns with regex-only metacharacters
   return `tool_status::unsupported` so the Python mirror handles them with
   exact reference behaviour.

2. **`build_session_output_block` splitlines subset.** The kernel splits on
   the ASCII separators (`\n`, `\r\n`, `\r`, `\x0b`, `\x0c`); Python's
   `str.splitlines()` additionally splits on `\x1c`–`\x1e`, `\x85`,
   U+2028/U+2029. Those bytes are left inside the line. Outputs routed to the
   native kernel are ASCII-gated at the binding layer, so this matches in
   practice.

3. **Path arithmetic keeps the base's own separators.** The kernels append
   components with the host separator but do not re-normalize separators
   already present in the injected base/python_exe strings (pathlib would
   rewrite `C:/x` → `C:\x` on Windows). The binding layer passes host-style
   paths, so real callers see the same strings the Python side produces.

4. **`prepare_python_env` returns a delta, not the full env dict.** The
   reference returns a complete snapshot (with scrubbing applied when
   `scrub_env` is on); the C++ kernel returns only the variables it adds or
   changes (`VIRTUAL_ENV`, `PATH`), which the caller applies over the
   scrubbed base. This is the `env_delta` contract in the plan.

5. **`extract_export_path` markers follow the plan exactly.** Earlier code
   used markers `"exported to file \`"` and `"added to file \`"` (with embedded
   backticks). The plan §3.4.7 specifies the markers without backticks:
   `"exported to file "`, `"added to file "`, `"exported to file: "`,
   `"added to file: "`. The implementation now matches that order; backticks
   are stripped only from the **trailing** end of the captured tail. This
   means an input like `"exported to file \`C:/t/0.txt\`"` yields
   `" \`C:/t/0.txt"` (leading space and backtick remain, trailing backtick is
   stripped). The binding layer should prefer the colon variants or plain
   space-separated paths for clean extraction.

6. **Environment scrubbing for non-ASCII names.** The kernel is defined for
   ASCII names; non-ASCII bytes are compared byte-wise. The binding layer must
   route non-ASCII env names to the Python mirror, matching `security.py`
   line 63.

5. Verification

Build and run the unit test target:

```bash
python bootstrap.py --toolchain msvc -m debug
xmake build test_builtin_python
xmake run test_builtin_python
```

Expected result: `Suite 'global': all tests passed.`

Test registration in `tests/xmake.lua` (local only, do not commit):

```lua
builtin_tools_test("test_builtin_python", "unit/builtin_tools/test_python_tool.cpp")
```
