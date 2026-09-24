# workflow (AgentSwarm + best-of-N) — C++ port parity report

Tool: `kimix.tools.swarm` (`AgentSwarm`, LLM-facing name `workflow`)
Namespace: `kimix::builtin_tools::workflow`
Files: `src/builtin_tools/workflow_tool.h`, `src/builtin_tools/workflow_tool.cpp`
Reference: `C:/dev/kimi-agent/src/kimix/tools/swarm/__init__.py` (542 lines) and
`C:/dev/kimi-agent/src/kimix/tools/swarm/best_of_n.py` (423 lines)
Tests: `tests/unit/builtin_tools/test_workflow_tool.cpp` — **77 tests, 1246 asserts, all passing**
Goldens: `scripts/gen_workflow_goldens.py` -> `tests/unit/builtin_tools/workflow_goldens.inc`
Target: `test_builtin_workflow` (already registered in `tests/xmake.lua`, no change needed)

```text
python scripts/gen_workflow_goldens.py         # rewrite the goldens
python scripts/gen_workflow_goldens.py --check # fail when the checked-in .inc is stale
python scripts/build_locked.py -- xmake build test_builtin_workflow
./bin/debug/test_builtin_workflow.exe          # Suite 'global': all tests passed (1246 asserts in 77 tests)
```

`workflow_tool.cpp` is not exposed through `runtime_py` (no `src/runtime/py/*.cpp` binding), so the port
is verified by a **golden-replay Boost.UT test** whose expectations are produced by running the *real*
Python objects — never a `kimix_native` mirror, never a hand-typed constant.

## The differential harness

`scripts/gen_workflow_goldens.py` imports the kimi-agent checkout (`<repo>/src` first, so kimi-cli's
`kimix` shim cannot shadow it) and drives it offline:

* `utils.prompt_async` / `utils.close_session_async` / `swarm._resolve_subagent_session` are stubbed with
  a scripted *rule* interpreter (`match` substring -> ordered outcomes: echo / text / silent / fail /
  pick-B), mirroring what the C++ test injects as `Workflow::runner`;
* the best-of-N workspace helpers (`create_worker_workspace`, `cleanup_worker_workspace`, `collect_diff`,
  `apply_diff_to_workspace`, `_snapshot_files`) are stubbed — the C++ port receives the same effects as an
  injected `workspace_hooks` struct, so no git worktree is ever created and the corpus is machine
  independent;
* `swarm.time` is replaced by a fixed-step clock (the `elapsed="…"` attribute is a wall-clock value; the
  golden replays the recorded value into the C++ runner) and `asyncio.sleep` is a no-op (the retry backoff
  and the token bucket have their own golden tables);
* per case the generator records the *whole* serialized result tree: the result envelope
  (`ok`/`status`/`message`/`brief`/`output`), the list of prompts dispatched to sub-agents, the selector
  review texts, the `apply_diff_to_workspace` calls, and every candidate field.

163 generated rows are replayed by the test: 71 end-to-end tool cases plus kernel tables for
`expand_template`, `validate_uniqueness`, `xml_escape`, `render_results`, the `<best_of_n_result>`
renderer, `is_rate_limit_error`, `retry_delay_seconds`, the `rate_limiter` token bucket,
`format_candidates_for_review`, the four message helpers, `select_best_candidate` (self-eval, majority,
tie-breaks, invalid-index fallback, all-failed), `run_parallel_sample` and `best_of_n` (degenerate
`n <= 1`, apply, verify, verification rejection, invalid-index fallback).

Against `git HEAD` (`git checkout HEAD -- src/builtin_tools/workflow_tool.{h,cpp}`, rebuild, run) the
golden test reports **`tests: 76 | 6 failed`, `asserts: 1238 | 1157 passed | 81 failed`**; with the fixed
port it reports `all tests passed (1246 asserts in 77 tests)`.

## Bugs found and fixed

Each one was caught by the generated goldens *before* the fix (the `[diff] <case> <field> at byte N`
diagnostic printed the first differing byte).

1. **`render_results` indented the subagent block with one space instead of four.** The reference's f-string
   literals start with 4 spaces (`    <subagent …`), 6 for `<output>`/`<error>`, 4 for `</subagent>`
   (verified by byte-dumping the reference source: the agent display collapses runs of spaces, which is how
   the port came to use one):
   * repro: `{"description": "audit <all> the things & stuff", "prompt_template": "Fix errors in {{item}}.",
     "items": ["a.py", "b\"c.py", "d'e.py"]}`
   * Python: 636 bytes — `…<subagents>\n    <subagent id="agent-0" …>\n      <output>…</output>\n    </subagent>…`
   * C++ (before): 603 bytes, every one of those lines indented by 1 space (`<output>` by 1, `</subagent>` by 1);
     the difference is exactly `3*(4-1) + 3*(6-1) + 3*(4-1) = 33` bytes for three sub-agents.
   * fixed in `render_results`; the two hand-written expectations in the old test that encoded the 1-space
     form were corrected as well (they asserted the bug, not the reference).
2. **`mode: null` was accepted and silently meant `fanout`.** `AgentSwarmParams.mode` is a
   non-Optional `Literal`, so an explicit JSON null is a `literal_error`.
   * repro: `{"description":"d","mode":null,"items":["a","b"],"prompt_template":"do {{item}}"}`
   * Python: validation error `Input should be 'fanout' or 'parallel_sample'`
   * C++ (before): ran the fanout swarm — `status=ok`, `brief="Swarm completed"`, two sub-agents dispatched.
3. **A non-string literal value produced the wrong wording.** `{"mode": 7}` -> Python's `literal_error`
   text is `Input should be 'fanout' or 'parallel_sample'`; the port answered `mode must be a string`.
   Same for `selector`. Fixed with a `(<field>=<value>)` renderer (`wf_render_value`: `None`, `True`,
   numbers, raw strings, compact containers) that keeps the project-wide message convention.
4. **`items: null` was treated as "absent".** `items: list[str]` has no `None`, so a JSON null is a
   `list_type` error; the port fell through and answered `Provide at least 2 items or resume_agent_ids.`
   Fixed: a present `items` must be an array.
5. **`subagent_type: 5` / `null` was silently ignored** and the call ran with the default `"coder"`.
   Python rejects both (`string_type`). Fixed: a present non-string is `invalid_input`.
6. **`sample_n` was stricter than pydantic.** `"3"`, `2.0` and `true` are accepted by the reference
   (lax mode: `int()` on a string, an integral float, bool -> 0/1) and mean `n=3`, `n=2`, `n=1`; the port
   answered `sample_n must be an integer`. Fixed (`wf_py_int_from_string` implements `int()` including
   underscores and surrounding whitespace; `2.5` is still rejected).
7. **The rate-limit retry loop was missing.** `_run_subagent_task` retries a rate-limit failure up to
   `_MAX_RETRIES` (4 attempts) with `1, 2, 4` second backoff; `run_swarm` never retried.
   * repro: two tasks where the `alpha` task fails once with `429 too many requests on task alpha` and then
     succeeds
   * Python: 3 dispatched prompts (`task alpha`, `task alpha`, `task beta`), `<succeeded>2</succeeded>`
   * C++ (before): 2 prompts, `<succeeded>1</succeeded>`, `<failed>1</failed>` with the 429 text.
   * a second golden (`fanout_retry_exhausted`, permanently throttled) pins the 4-attempt ceiling.
8. **The best-of-N failure brief was decided by substring.** The reference distinguishes
   `VerificationRejectedError` from `AllCandidatesFailedError` by *type*; the port searched the message for
   `"verification"`, so an all-failed sample set whose error text happens to contain that word was
   reported as `selected sample failed verification` instead of `all samples failed`
   (golden `parallel_sample_all_failed_looks_like_verification`). Fixed with
   `best_of_n_failure { none, all_candidates_failed, verification_rejected }`.
9. **A missing sample error rendered as `""` instead of `"None"`.** Python's f-strings interpolate the
   `None` (reachable through the injectable-runner API): `all 3 sampled candidates failed: #0: boom;
   #1: bang; #2: None` and `=== Candidate 0 (failed: None, 0 steps) ===`. Fixed in
   `all_candidates_failed_message` / `format_candidates_for_review`.
10. **The swarm-session gate ran after the recursion guard.** `AgentSwarm.__init__` raises
    `SkipThisTool` for a session without `custom_data["is_swarm_session"]`, i.e. the tool is never offered
    at all — so the gate must precede every call-time guard. A session that is both a sub-agent and not a
    swarm session now answers `unsupported` / `invalid tool.` instead of the recursion error.
11. **`KIMI_CODE_AGENT_SWARM_MAX_CONCURRENCY` was advertised but never read.** `_run_swarm` resolves the
    fan-out concurrency from it (`max(1, int(raw))`, `_DEFAULT_BURST` when absent/unparsable) and derives
    the token-bucket burst from `min(_DEFAULT_BURST, max_concurrency)`; the port always used the burst.
    Fixed (`env_max_concurrency()`, used by the fan-out path only — the best-of-N path keeps
    `run_parallel_sample`'s own default of 5), with a kernel test for `3`, `" 7 "`, `1_0`, `0`, `-4`,
    `abc` and unset.

## Documented deviations (asserted or explained, not "fixed")

* **`resume_agent_ids` key order.** Python dispatches the resumed agents in JSON insertion order; the C++
  `ToolParams` body is a `kimix::unordered_map` (see `tool.h`), so the order is unrecoverable at the tool
  boundary. `parse_params` sorts the keys, which makes the resumed agents' indices (and therefore the
  rendered order) deterministic but not insertion-ordered.
* **`_validate_uniqueness` duplicate rendering.** Python interpolates a `set` literal whose iteration order
  is unspecified (hash randomisation); the port renders the same `repr`s sorted. The goldens therefore
  record the canonical (sorted) form.
* **The fuzzy alias tables.** `k_workflow_aliases` mirrors the repo-wide convention introduced in `tool.h`
  ("wrong-but-reasonable argument names are accepted", cf. `k_run_aliases` / `k_grep_aliases`), which is
  broader than the reference's repair pass: measured against `_COMMON_FIELD_ALIASES` + `_fuzzy_match_keys`,
  `desc`/`summary` -> `description`, `samples` -> `sample_n` and `selection`/`agent_type` are accepted by
  both, while `name`, `task_description`, `template`, `prefix`, `inputs`, `tasks`, `agent_ids` and
  `workflow_mode` are accepted *only* by the port (the reference ignores them, so the call then fails or
  uses the default). Keeping the generic mechanism was preferred over trimming one tool's table.
* **pydantic message wording.** Pydantic's own multi-line `ValidationError` text is not reproduced; a
  parameter error becomes `"<pydantic text> (<field>=<value>)"` or `missing required field: <name>`,
  exactly like the other ported tools (`job_output`, `run`, `agent`). `python_error` is recorded in the
  goldens next to `expect_message` so the mapping stays auditable.
* **A Python `ToolOk` carries no brief.** The C++ envelope always has one, so `Swarm completed` /
  `best-of-N completed` are authored (the repo's convention), and the golden records them.
* **`agent_swarm_in_flight` (one `workflow` call per response) is not ported.** The C++ `Session` has no
  `custom_data` map and the native tool call is synchronous per session; a process-global registry keyed by
  `Session*` was judged worse than the gap. Python's own test (`test_concurrent_call_rejected`) is
  therefore not mirrored.
* **`_RateLimiter`'s defensive clamps** (`burst < 1 -> 1`, `interval <= 0 -> 1.0`) exist in the C++ only;
  Python's arithmetic is unguarded. Unreachable through the tool (`burst = min(5, concurrency >= 1)`).
* **Native workspace hooks** (`native_workspace_hooks`: git worktree / recursive copy, `_COPY_IGNORE`,
  `git diff HEAD` + untracked-file diffs, copy-back apply) mirror `best_of_n.py` 66-232 by inspection but
  are only exercised through injected hooks in the tests; they need a real git checkout to run end to end.

## Verified as correct although it looked suspicious

* `_render_results`' `elapsed` handling: `f"{x:.1f}s" if result.elapsed else "-"` means a **0.0** elapsed
  renders as `-` (Python truthiness), and the port's `has_value() && != 0.0` + `{:.1f}` matches on every
  golden (`0.4s`, `1.2s`, `12.0s`, `-`).
* `xml_escape` == `html.escape(quote=True)`, including `&#x27;` for the apostrophe and byte-passthrough of
  non-ASCII text (`café ✓`, `模块/a.py`).
* `<best_of_n_result>`'s *double* escaping: the failure status is escaped once into `status="failed: …"`
  and then escaped again as an attribute value — the port reproduces it byte-exactly.
* The majority tie-break `max(votes, key=lambda idx: (votes[idx], -idx))` == "highest votes, then lowest
  index", and majority is only used with >= 3 viable candidates (2 viable falls back to self-eval).
* `run_parallel_sample`'s `"[workspace:<kind>]"` diff marker (kind stashing), the `n < 1` guard, and the
  "only one viable candidate" short-circuit (the selector is not called).
* The default self-eval selector's review prompt, the candidate review text and the majority pair text are
  byte-identical to the reference (the golden compares every review text the selector received).
* The empty-prompt form of `parallel_sample`: `prompt_template.replace("{{item}}", "").strip()` and
  `f"{prefix}{suffix}".strip()` (the golden reconciles `Refactor  carefully.` with its double space).

## Known gaps

* The generator drives pure pydantic validation (the same layer `plan_tool` chose), so kosong's
  argument-repair pass (`_repair_dict_for_model`: key aliases, fuzzy matching, `_coerce_value`,
  list<->scalar wrapping) is not part of the goldens — only the port's equivalent `ToolParams::with_aliases`
  resolution and the coercion for `sample_n` are covered.
* The concurrency *ordering* of `run_swarm` / `run_parallel_sample` under >1 worker is not asserted (the
  dispatch lists are compared as multisets); the resolved `max_concurrency` is only verified through
  `env_max_concurrency`.
* `native_workspace_hooks` and the git-worktree path (see above) are unverified against a live repository.
* `KIMI_CODE_AGENT_SWARM_MAX_CONCURRENCY` wiring into `operator()` is covered by the kernel test and code
  review, not by an observable golden (the variable only changes scheduling).
* The retry path really sleeps (`1 + 2 + 4` s for `fanout_retry_exhausted`), which makes the target take
  ~15 s; that is the reference behaviour, not a test artifact.
