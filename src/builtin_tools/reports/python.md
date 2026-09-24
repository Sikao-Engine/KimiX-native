python tool — C++ implementation report

Tool: python (kimi-agent built-in tool)
Files: src/builtin_tools/python_tool.h + python_tool.cpp (pure kernels)
       src/builtin_tools/python_tool_class.cpp (Tool class: native execution)
Tests: tests/unit/builtin_tools/test_python_tool.cpp (target test_builtin_python)
       tests/unit/builtin_tools/python_goldens.inc (GENERATED)
       scripts/gen_python_goldens.py (generator)
       python/tests/test_parity_python.py (differential, via runtime_py)
Namespace: kimix::builtin_tools::python

1. Source of truth

The Python reference is the kimi-agent checkout (C:/dev/kimi-agent); the port
plan is D:/KimiX-native/plans/python.md.  Where the plan and the reference
disagree, the reference wins (see §4.1).

  src/kimix/tools/py/__init__.py
    _resolve_python / _resolve_python_uncached (105-152) → resolve_python_exe
        + Python::detect_python_exe (real filesystem probe)
    _build_env (154-203)                                 → prepare_python_env
    _module_not_found_hint (205-215)                     → module_not_found_hint
    Params._validate_source (78-84)                      → Tool-class messages
    _resolve_script_source (283-307)                     → file-mode detection
    __call__ / _start_interactive / _execute_code / _continue_session (309-651)
  src/kimix/tools/common.py
    _create_script_file (728-741)                        → ScriptFileWriter::plan_path
    _extract_export_path (937-945)                       → extract_export_path
    _build_session_output_block (1081-1122)              → build_session_output_block
    _display_temp_path (744-758)                         → pyc_display_path (class)
  src/kimix/tools/security.py
    scrub_child_env (47-77)                              → scrub_child_env
  src/kimix/tools/background/utils.py
    wait_for_output pattern step (418)                   → classify/match_wait_pattern

2. Function-by-function mapping

| C++ symbol | Python reference | Notes |
|---|---|---|
| ScriptFileWriter::plan_path / plan_script_path | common.py _create_script_file 728-741 | Pure path arithmetic: <base_dir>/<index><ext>, monotonic index, thread-safe via kimix::spin_mutex. The reference resolves the path with Path.resolve(); the kernel performs the plain join (resolve() only normalises the existing directory's case on Windows) — the generator asserts both agree. |
| resolve_python_exe | _resolve_python_uncached 122-152 | Byte-exact golden replay over an injected fake filesystem: override > .venv walk-up (Scripts before bin, nearest parent first) > VIRTUAL_ENV > fallback. The reference always falls back to sys.executable, which the kernel receives as its `fallback` argument ("" == std::nullopt). |
| Python::detect_python_exe(work_dir) | same | The real-filesystem wrapper: KIMIX_PYTHON_EXECUTABLE (PYTHON_EXE kept as a port-compatible alias) → resolve_python_exe over {work_dir, cwd} → first interpreter on PATH as the sys.executable stand-in. |
| scrub_child_env | security.py scrub_child_env 47-77 | Byte-exact ASCII port: safe prefix wins, else drop when the uppercased name contains a secret substring, insertion order preserved. Non-ASCII names never reach the native path (the reference itself gates on name.isascii()). |
| prepare_python_env (delta) | py/__init__.py _build_env 154-203 | Returns the env *delta* (PATH / VIRTUAL_ENV) instead of a full snapshot; std::nullopt == the reference's None zero-copy fast path. Golden-verified field by field. |
| module_not_found_hint | _module_not_found_hint 205-215 | Byte-level scanner for the ASCII marker; matches the reference's `regex` search including empty/mismatched-quote/embedded-newline cases and the first-match-wins rule. |
| build_session_output_block | common.py _build_session_output_block 1081-1122 | Ordered block + textwrap.indent semantics; {:.2f} elapsed; falsy optional strings render null. Shared with the bash/run tools. |
| extract_export_path | common.py _extract_export_path 937-945 | Four markers in reference order, trailing rstrip("]`"); None vs "" distinguished. |
| classify/match_wait_pattern | background/utils.py 418 (pattern.search) | Native subset = literal only (no regex metacharacter, ASCII). Everything else -> tool_status::unsupported, i.e. the caller must route it to the Python regex engine. |
| Python (Tool class) | __call__ / _execute_code / _start_interactive / _continue_session | Real subprocess execution through proc::run_process / proc::start_task; mirrors the reference's parameter validation, file-mode detection, interpreter resolution, mode dispatch and failure message (minus the ported-elsewhere bits listed in §5). |

3. Verification (green)

Golden replay: scripts/gen_python_goldens.py drives the *real* Python
implementation (never a kimix_native mirror) over 8 kernel families plus
fuzzed corpora and writes byte-exact C literals into
tests/unit/builtin_tools/python_goldens.inc; test_python_tool.cpp replays every
row through the C++ kernels.

    python scripts/gen_python_goldens.py            # regenerate
    python scripts/gen_python_goldens.py --check    # fail when stale
    python scripts/build_locked.py -- xmake build test_builtin_python
    ./bin/debug/test_builtin_python.exe
    → Suite 'global': all tests passed (N asserts in M tests)

Differential (live) test against the reference, through the pybind11 binding
`runtime_py.builtin_tools.python` (src/runtime/py/py_builtin_python.cpp,
registered in src/runtime/py/module.cpp) — corpora plus fixed-seed fuzz:

    python scripts/build_locked.py -- xmake build runtime_py
    python -m pytest python/tests/test_parity_python.py -q

3.1 process_runner semantics this tool relies on (verified by the class tests)

* exit code propagation: proc::run_result::exit_code -> block.exit_code; the
  ModuleNotFoundError test asserts "exit_code: 1" in the rendered block.
* stdout/stderr merge: child stderr is merged into the captured stream (on
  Windows stderr is redirected to the stdout file), so the python traceback
  reaches the captured output and the pip hint — asserted by
  python_tool_class_module_not_found_hint_in_message.
* timeout kill: timeout_ms terminates the child (reproc stop TERMINATE -> KILL)
  and the class reports block.status "timeout" with the reference's
  "use `job_output`" message — asserted by
  python_tool_class_timeout_kills_child.
* output draining: the bounded capture (output_cap_chars) is what the class
  reports; on POSIX the two pipes are polled concurrently, so byte-exact
  stdout/stderr *interleaving* is not guaranteed there (Windows, the platform
  verified here, has a single merged stream).
* environment: proc::run_options only *extends* the parent env
  (REPROC_ENV_EXTEND) — see §6.2.
* cwd: opts.working_directory = session.work_dir.  The reference passes
  cwd=None (the child inherits the agent's cwd); the two agree whenever the
  session work dir is the process cwd.  Unlike the bash tool there is no
  workdir pre-validation because the reference has no cwd parameter at all; an
  unusable work dir surfaces as the runner's spawn_error
  (status invalid_input).

4. Fixes made during parity verification

4.1 extract_export_path — the first two markers lost their backtick.
The reference markers are "exported to file `", "added to file `",
"exported to file: ", "added to file: ".  The port followed the plan's
(incorrect) marker list without the backticks, so
"exported to file `C:/t/0.txt`" returned "`C:/t/0.txt" (leading backtick kept)
and "exported to file C:/t/0.txt" matched when the reference returns None.
Fixed in python_tool.cpp; pinned by python_goldens_extract_export_path /
extract_export_path_markers (15 assertions fail on the pre-fix code — verified
by temporarily reverting the markers).

4.2 wait_for_pattern — the fnmatch "glob" subset was not regex-exact.
The reference compiles wait_for_pattern with the `regex` engine and calls
pattern.search(buffer).  The port answered the fnmatch metacharacters
(* ? [seq]) with glob semantics, which disagrees in both directions:
regex "ready*" matches "read done" (* quantifies the preceding 'y') while
fnmatch("read done", "*ready**") is False; conversely a pattern that is an
invalid regex ("*done*") matched as a glob instead of surfacing the reference's
error.  classify_wait_pattern now returns `literal` only for patterns with no
regex metacharacter, `unsupported` for everything else (mirroring
bash capture_machine::pattern_matches, which the process runner uses for the
same reason).  ']' and '}' are deliberately treated as literals: they are inert
without a leading '[' / '{', both of which are metacharacters (verified against
the reference engine over 60k random patterns from the allowed alphabet).
Pinned by python_goldens_wait_pattern and test_glob_metacharacter_is_not_native.

4.3 The Tool class ignored the ported kernels and several reference rules:
* `code` ending in ".py" that names an existing file now runs that file
  (_resolve_script_source priority 1) instead of writing the path text into a
  temp script (which python then failed to compile).
* interpreter resolution now uses resolve_python_exe with the reference
  precedence (KIMIX_PYTHON_EXECUTABLE ↦ .venv walk-up ↦ VIRTUAL_ENV ↦ PATH
  fallback) instead of a bare PATH scan honouring PYTHON_EXE.
* parameter contract aligned with Params: `file` is an alias of `code`
  (AliasChoices), `mode` ∈ {execute, send, interactive} with the deprecated
  `run`/`background` values and the hidden `interactive` bool
  (normalize_mode_validator), `max_lines` declared; validation runs before the
  interpreter lookup and emits the reference messages
  ("`code` must be provided (unless mode='interactive' or task_id is set)." /
  "code cannot be empty when continuing a session via task_id").
* task_id continuation now *sends* the code to the task's stdin and then
  collects output (mirrors _continue_session) instead of only polling.
* temp scripts live in <work_dir>/.kimix_cache/tmp_<pid>/<index>.py (the
  reference _temp_folder naming) and messages carry the short display form
  (_display_temp_path: ".kimix_cache/tmp_<pid>/0.py").
* failure messages carry the interpreter and the ported
  module_not_found_hint; the result block reports status "failed"/"timeout"
  with the exit code (bash's convention).

5. What deliberately stays in Python

* `_syntax_check_error` — needs CPython compile().
* Long-output summarization (_summarize_long_output_async) — LLM network call.
* The rtk/dedup/truncate token-filter pipeline (_token_filter_output,
  _maybe_export_*), i.e. `max_lines` folding and output export: the native
  path returns the bounded raw capture (output_cap_chars = 200000) with
  output_truncated set.  See §6.1.
* `_append_elapsed` / `_format_elapsed_seconds` (no C++ port exists in any
  tool; the elapsed suffix is missing from every native tool message).
* Environment scrubbing of the child (scrub_env / env_passthrough /
  redact_secrets) and `_build_env`'s PATH/VIRTUAL_ENV rewrite at spawn time:
  proc::run_options can only *extend* the parent environment
  (REPROC_ENV_EXTEND).  See §6.2.
* asyncio.Semaphore(8), _resolve_python result caching, _python_config reads,
  temp-folder cleanup, `_temp_set` keyed-append exports.

6. Known gaps / deviations

6.1 Output shaping.  The reference runs the child output through
    _token_filter_output (dedup + max_lines fold via _truncate_lines) and
    summarizes >64 KiB outputs.  The native class returns the raw bounded
    capture.  Porting it means calling bash::truncate_lines and implementing
    the rtk/dedup stages; not done here (the Python mirror owns it per §5).

6.2 Child environment.  Because the runner can only extend the parent env, a
    secret-looking variable is still inherited by the native child (the
    reference scrubs it by default).  Fixing this needs a "replace env" mode in
    proc::run_options (shared with bash/run/job-output) — reported, not
    changed.

6.3 Message text.  The class's messages match the reference wording except for
    the "(1.23s)" elapsed suffix (§5) and the exported-output variants.

6.4 Empty wait_for_pattern.  The reference compiles "" (it matches every
    buffer); the kernel reports tool_status::invalid_input instead of guessing
    between "no pattern" and "empty regex".  Pinned as a documented deviation
    in the goldens row `empty_pattern`.

6.5 ASCII gate.  scrub_child_env / wait-pattern classification are defined for
    ASCII input.  Non-ASCII env names are routed to Python by the reference
    itself; non-ASCII wait patterns are classified `unsupported`.

6.6 Relative base paths.  resolve_python_exe walks the ancestors of an
    absolute base exactly like Path.parents; for a *relative* base the
    reference also probes ".", which the kernel does not model (the binding
    layer passes the session dir / cwd, both absolute).

7. Test registration

    builtin_tools_test("test_builtin_python", "unit/builtin_tools/test_python_tool.cpp")

(already present in tests/xmake.lua — no edit was needed.)
