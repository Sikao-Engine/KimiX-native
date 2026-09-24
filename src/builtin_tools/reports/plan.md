# plan (WritePlan / ReadPlan / EditPlan) — C++ kernel port (implementation report)

Tool: `kimix.tools.note` (`WritePlan`, `ReadPlan`, `EditPlan`)
Namespace: `kimix::builtin_tools::plan`
Files: `src/builtin_tools/plan_tool.h`, `src/builtin_tools/plan_tool.cpp`
Reference: `C:/dev/kimi-agent/src/kimix/tools/note/__init__.py` (517 lines)
Tests: `tests/unit/builtin_tools/test_plan_tool.cpp` — 53 tests, **1687 asserts, all passing**
Goldens: `scripts/gen_plan_goldens.py` -> `tests/unit/builtin_tools/plan_goldens.inc`
Target: `test_builtin_plan` (already registered in `tests/xmake.lua`, no change needed)

```
python scripts/gen_plan_goldens.py            # rewrite the goldens
python scripts/gen_plan_goldens.py --check    # fail if the checked-in .inc is stale
python scripts/gen_plan_goldens.py --schemas  # rewrite the LLM-facing tool meta in plan_tool.cpp
python scripts/build_locked.py -- xmake build test_builtin_plan
./bin/debug/test_builtin_plan.exe             # Suite 'global': all tests passed (1687 asserts in 53 tests)
```

**Heads-up for anyone re-running this by hand:** `xmake build <target>` does not always
notice a freshly rewritten test source (a stale object gave me a false "all tests passed"
twice). Delete the object before claiming a result:

```
rm -f build/.objs/test_builtin_plan/windows/x64/debug/tests/unit/builtin_tools/test_plan_tool.cpp.obj
rm -f build/.objs/kimix-llm/windows/x64/debug/build/.gens/kimix-llm/windows/unity_build/unity_2.cpp.obj   # the batch holding plan_tool.cpp
```

`plan_tool.cpp` is not exposed through `runtime_py` (no `src/runtime/py/*.cpp` binding), so the port is
verified by a golden-replay Boost.UT test whose expectations are produced by running the *real* Python
objects — never a `kimix_native` mirror, never a hand-typed constant.

## What was ported (function-by-function mapping)

| C++ kernel (`plan_tool.h`) | Python source of truth (`note/__init__.py`) | Lines |
|---|---|---|
| `k_max_lines` / `k_max_line_length` / `k_max_bytes` | `MAX_LINES` / `MAX_LINE_LENGTH` / `MAX_BYTES` | 15-17 |
| `parse_write_params` | `WritePlanParams` (content\|text alias, `mode` Literal) | 30-40 |
| `parse_read_params` | `ReadPlanParams` (`ge` bounds + `_validate_line_offset`) | 83-119 |
| `parse_edit_params` / `pl_parse_edit_object` | `EditPlanParams` (`edit`\|`edits`, single-or-list), `Edit` (`old`\|`old_string`), plus kosong's `_repair_dict_for_model` / `_maybe_parse_json_string` for the embedded-JSON form | 288-308 |
| `render_forward` | `ReadPlan._read_forward` | 168-216 |
| `render_tail` | `ReadPlan._read_tail` | 218-283 |
| `apply_char_window` | `ReadPlan.__call__` `output[char_offset:max_char]` | 158-161 |
| `pl_build_message` / `format_line_list` | the `" ".join(message_parts)` status line + the Python list repr of the truncated line numbers | 198-216 / 266-283 |
| `apply_plan_edit` | `EditPlan._apply_edit` (no-op guard, replace_all, first-hit, strip match, fuzzy match, suggestion) | 417-457 |
| `apply_plan_edits` | `EditPlan.__call__` edit loop (sum the counts, keep the LAST suggestion) | 479-491 |
| `read_plan_file` / `write_plan_file` | `Path.exists/is_file/read_text` and `mkdir(parents=True, exist_ok=True)` + `open(..., "w"/"a")` | 55-78 / 142-151 / 469-504 |
| `WritePlan` / `ReadPlan` / `EditPlan` (Tool) | the three `__call__` bodies + the `_enable_plan` gate | 54-78 / 133-166 / 459-514 |
| `KIMIX_REGISTER_TOOL` meta | `CallableTool2.name/description` + `base.parameters` (pydantic schema) | 45-46 / 124 / 313 |

Reused, never re-implemented: `read::split_lines` (aiofiles universal-newline read with
`errors="replace"`), `read::truncate_line_read` (`kimi_cli.tools.utils.truncate_line`, marker `"..."`),
`edit::normalize_newlines` / `find_similar` / `try_strip_match` / `best_fuzzy_match`, `kimix::repair`
(JSON repair for the embedded-`edit` string, same as `todo_tool.cpp`).

The edit chain deliberately does **not** reuse `edit::apply_edit`: `EditPlan` returns *no* suggestion when
a fuzzy match succeeds, while the file-edit tool answers `"fuzzy-matched at NN%: '...'"`.

## The differential harness

`scripts/gen_plan_goldens.py` imports `kimix.tools.note` straight from the kimi-agent checkout
(defensive against the `kimi-cli/src/kimix` shim, like `python/tests/_parity_ref.py`) and:

* drives every tool call through `CallableTool2.call(json)` — the same entry point the agent toolset
  uses, so pydantic validation, kosong's alias/repair pass and the tool body all run;
* operates on real files in a temp directory whose path is replaced by the `@PLAN@` token (the test
  substitutes its own temp directory back in), so the goldens are machine independent;
* records what matters for each call: `status`/`is_error`, `message`, `output`, `brief`, the resulting
  bytes on disk, and — for `ReadPlan` — the C++-only bookkeeping (`start_line`, `total_lines`,
  `max_lines_reached`, `max_bytes_reached`, `truncated_line_numbers`) *parsed out of the reference's own
  message* (the parse is validated by rebuilding the message and comparing it byte for byte);
* asserts its own model of the write path while generating: for every writing case the claimed logical
  text must reproduce CPython's on-disk bytes exactly (`base + translate(LF -> CRLF)`), so a wrong claim
  fails generation instead of writing a wrong golden.

Corpus: 82 end-to-end rows (47 `ReadPlan`, 24 `EditPlan`, 11 `WritePlan`) + 42 direct render rows
(25 forward / 17 tail, including the byte-budget corpora whose rendered output is recorded as a
byte/line count) + 18 char-window rows + 39 pydantic parameter rows + 3 LLM-facing tool-meta rows +
3 OS-error rows + 3 gate rows. It covers every case from kimi-agent's `tests/test_note.py` (all three
tool classes, both init gates, the missing-`plan_writing_path` errors, the forward/tail/char-window
reads, the MAX_LINES/MAX_BYTES limits, the `_apply_edit` matrix, the CRLF normalization, the
string-edit repair), plus LF/CRLF/lone-CR/vertical-tab/NUL/invalid-UTF-8/BOM/long-line corpora,
`MAX_LINES+1` files, 1200-line files, both byte budgets, unicode, and every message branch.

The goldens are pure ASCII (every byte outside `[!-~]` — spaces included — is a 3-digit octal escape,
so no pipeline can fold or strip a checked-in value) and every literal is chunked below MSVC's 16 KiB
limit. Large corpora are stored as `\002R<us>COUNT<us>UNIT` recipes that the test expands and
length-checks.

## Bugs found and fixed (each was caught by the new test before the fix)

Both sides were rebuilt from scratch before each measurement (see the staleness note above).
Against `git show HEAD:src/builtin_tools/plan_tool.cpp` the new test reports
**`tests: 53 | 7 failed`, `asserts: 1687 | 1612 passed | 75 failed`**; with the fixed port it reports
`all tests passed (1687 asserts in 53 tests)`.

1. **Alias priority was inverted.** pydantic's `populate_by_name` gives the *declared alias* priority when
   both spellings are present, the opposite of the generic `ToolParams::get` order documented in
   `tool.h`:
   * repro: `WritePlanParams.model_validate({"content": "canonical", "text": "alias"}).content`
     -> Python `'alias'`, C++ wrote `'canonical'` (now pinned by the `write_aliases_both` golden);
   * same for `Edit.old/old_string`, `Edit.new/new_string` and `EditPlanParams.edit/edits`
     (`{"edit": ..., "edits": ...}` -> the reference uses `edits`).
   * fix: `pl_string_param` resolves `params->get_exact(alias)` first; `parse_edit_params` looks up
     `edits` before `edit`.
2. **An explicit JSON null silently fell back to the default.** `{"mode": null}` made the port *write the
   plan* with `overwrite`; pydantic rejects it (`Literal` has no null), so Python answers a validation
   error. Same for `line_offset`/`n_lines`/`max_char`/`char_offset`/`replace_all`.
   Golden rows: `write_mode_null`, `read_line_offset_null`.
3. **A fractional float was truncated.** `{"line_offset": 1.5}` -> Python `ValidationError`
   (`int_from_float`), C++ used `1`. Fix: `pl_int_param` rejects a real with a fractional part.
4. **A non-boolean `replace_all` was silently `false`.** `{"replace_all": "yes"}` -> Python replaces
   *all* occurrences; the port replaced only the first. The port has no coercion pass, so it now answers
   `invalid_input` (documented deviation, see below).
5. **An empty edit list was rejected.** `{"edits": []}` -> Python accepts it and returns the
   "No replacements were made." error (`no_change`); the C++ answered `invalid_input`
   ("edit list must not be empty"). Fix: accept the empty list.
6. **A JSON-string `edit` was rejected.** `{"edit": '{"old": "hello", "new": "hi"}'}` -> kosong's repair
   pass parses the embedded object and the edit applies (kimi-agent's own
   `test_note.py::test_string_edit_json_is_repaired`); the C++ answered
   `invalid_input` and left the file unchanged. Fix: `pl_embedded_json` (same rule as
   `todo_tool.cpp::td_parse_embedded_json`).
7. **`WritePlan`'s message/brief were invented.** The reference returns
   `ToolOk(output=f"Plan {action} {path}")` with the default `message=""` and no `brief`; the port put the
   text in *both* `message` and `output` and added `brief="Write plan"`. `EditPlan` likewise invented
   `brief="Edit plan"` on success. Both now match the reference (the model sees the same bytes).
8. **`WritePlan`'s failure message carried an invented wrapper.** Python's `except` block reports
   `str(exc)` verbatim with `brief="Failed to write plan"`; the port answered
   `"Failed to write plan. Error: cannot open plan file"`. `write_plan_file` now returns the raw detail,
   `WritePlan` passes it through, `EditPlan` wraps it as `"Failed to edit plan. Error: ..."` (which *is*
   the reference's message for the edit path).
9. **`EditPlan` on a non-file reported the wrong branch.** The reference has no `is_file()` pre-check for
   `EditPlan` (only `ReadPlan` has one): it reads the path, catches the raised `OSError` and answers
   `"Failed to edit plan. Error: <oserr>"` with `brief="Failed to edit plan"`. The port answered
   `` "`<path>` is not a file." `` with `brief="Invalid path"` (the `ReadPlan` shape). Now the
   `ReadPlan`/`EditPlan` split is explicit.
10. **The LLM-facing tool definition diverged.** `KimiSoul::tool_definitions()` hands
    `ToolMeta::description` + `parameters_json` to the model verbatim, and the committed literal used the
    *field* names and dropped defaults, while kosong serializes the pydantic schema **by alias**:
    `WritePlan` requires `text` (not `content`), `EditPlan` requires `edits` with `old_string`/`new_string`
    members (not `edit`/`old`/`new`), and `ReadPlan` publishes the defaults (`line_offset: 1`,
    `n_lines: 1000`, `max_char: 65536`, `char_offset: 0`) plus `minimum` bounds. The model was therefore
    told to send argument names the reference never documents. The block between the
    `GENERATED:PLAN-TOOL-META` markers is now produced by `--schemas` from the reference and asserted
    structurally by `tool_meta_matches_the_reference` (all three descriptions and schemas are now
    byte-identical to the pydantic output).

Also repaired, in the *test*:

* the committed `plan_goldens.inc` claimed to be generated by `.kimix_cache/plan_goldens.py`, which no
  longer exists (it was a scratch file, not a `scripts/` generator), so the file could not be
  re-derived; it is now regenerated from scratch with `--check` support;
* one hand-transcribed expectation asserted `"...Did you mean:\n candidate"` (one space) for a suggestion
  that the port and the reference build as `"\n  " + "  candidate"`. The assertion was therefore **false**
  — yet the committed suite reported it as passing (`45 tests / 177 asserts / 0 failures`), because
  Boost.UT's expression handling is not dependable for `kimix::string` (custom allocator) temporaries:
  with a single `printf` inserted before it the very same statement fails, and it fails reliably once the
  operands are `std::string`. All 51 string expectations in the test file therefore compare
  `sv_of(x) == std::string("…")`; that form prints both operands on failure and is what caught the
  `write_aliases_both` / `message mismatch` divergences above. The corrected expectation pins both the
  two-space-indent and the four-space-overall form.

## Documented deviations (asserted by the tests, not "fixed")

1. **CPython text-mode newline translation.** The reference writes plan files through a text-mode file
   object, so on Windows every `\n` of the written text becomes `\r\n` (and a literal `\r` in the content
   survives as `\r\r\n`). The port writes the UTF-8 bytes verbatim — the project-wide convention also
   documented for `write` and `web_search`'s `store_full_text`. The goldens record *both* forms
   (`write_logical` and `python_bytes`) and the test asserts the translation is the **only** difference.
   This matters for a plan file consumed outside the agent (the tools themselves read with universal
   newlines, so their output is unaffected).
2. **kosong's repair-layer coercions are not reproduced.** The reference runs
   `_repair_dict_for_model` before the tool body, which str/int/bool-coerces scalars and *clamps* pydantic
   `Field` constraints, so Python accepts some arguments the pydantic model rejects outright:
   * `{"content": 123}` -> accepted as `"123"` (string coercion);
   * `{"line_offset": "3"}` -> accepted as `3`;
   * `{"n_lines": 0}` / `{"max_char": -1}` / `{"char_offset": -1}` -> clamped to the `ge` bound;
   * `{"replace_all": "yes"}` -> accepted as `true`.
   The `src/builtin_tools/*` ports implement the *pydantic* contract only (e.g.
   `read::validate_int_option` returns the byte-exact pydantic `ValueError` text) and have no repair
   layer, so these rows are strict `invalid_input` in C++. Each one is an explicit `deviation` row in
   `kPlanParamGoldens` that the test requires to be rejected, so the gap is visible and cannot silently
   change. (The two structural repairs kimi-agent's *own tests* pin for these tools — embedded-JSON
   `edit` and the empty edit list — were implemented, see bugs 5/6.)
3. **OS-error text.** CPython's `OSError` wording (`"[Errno 13] Permission denied: '<path>'"`) is platform
   specific and cannot be reproduced; `kPlanOsErrorGoldens` pins the platform-independent parts
   (is_error, brief, output) plus the wrapper prefix the reference's `except` block produces (`""` for
   `WritePlan` because it reports `str(exc)` verbatim, `"Failed to edit plan. Error: "` for `EditPlan`).
4. **`_enable_plan` gate.** Python raises `SkipThisTool` from the constructor, so a disabled plan tool is
   never offered to the model. The C++ registry is process-wide and static (a tool cannot un-register
   itself per session), so the mirror is `Session::plan_enabled == false` -> every invocation answers
   `tool_status::unsupported` (message `"plan tools are disabled for this session"`, brief
   `"invalid tool."`) without touching the file system. All three tools gate identically, and the gate is
   checked before parameter parsing; `kPlanGateGoldens` records the Python `SkipThisTool` fact.
5. **`custom_data['plan_called']` is not mirrored.** `WritePlan` sets it after a successful write, but it
   is never *read* anywhere in kimi-agent (`grep -rn plan_called` -> only the assignment), and the C++
   `Session` has no `custom_data` map, so there is nothing to mirror.
6. **`native_io` / injected-content test affordances.** A `Session` without `native_io` answers
   `tool_status::unsupported` (`"native plan <action> requires a native_io session"`), and
   `plan_path_override` lets the unit tests bypass the gate; neither exists in the reference, they are
   port-local plumbing (documented in `plan_tool.h`).

## Verified as correct although it looked suspicious

* `max_char` is a slice **END**, not a length (`output[char_offset:max_char]`): `"abcdef"[2:4] == "cd"`.
  The port already had it right; the window goldens (including the inverted, zero-length and past-the-end
  windows, over unicode text) settle it.
* `ReadPlan` omits `"Total lines in file"` whenever the scan stopped early (line or byte budget) but
  always reports it in tail mode; `max_lines_reached` is only true when the *cap* was the reason. Matched
  by the message goldens, and the C++-side bookkeeping fields are checked against the message-derived
  expectations.
* `_read_tail` keeps the **newest** lines under the byte budget and re-anchors `start_line` at the first
  surviving line (the port's O(n) ring matches the reference's `list.append` + `pop(0)`); the
  `60 x 1901`-byte corpus pins `8..60` and `"53 lines read ... from line 8"`.
* `_try_strip_match` runs before the fuzzy path and can splice the *middle* of a line:
  `"hello world\n"` with `old="hello worl"` -> `"xd\n"` (verified end to end).
* A successful **fuzzy** match reports no suggestion and replaces the whole matched line
  (`"hellp world\n"` + `old="hello world"` -> `"HI\n"`); a failed one reports the `_find_similar`
  suggestion after `"\n\nDid you mean:\n  "`.
* `_apply_edit` returns the *normalized* (LF) content on a hit, so a single successful edit rewrites the
  whole file with LF endings (`"a\r\nb\r\n"` -> `"z\n"`), whereas a no-op leaves the bytes untouched.
* `{"content": 123}` is **accepted** by the reference (kosong's repair coerces int -> str) — verified
  directly with pydantic plus the repair pass before being recorded as a deviation rather than a C++ bug.

## Known gaps

* `pl_read_file`'s `fopen` failure and `write_plan_file`'s short-write path are unreachable in a
  deterministic test (they need a file that exists and is a regular file yet cannot be opened, or an
  I/O error mid-write); the corpus covers the reachable failures (missing file, directory, parent path is
  a file).
* A *malformed* embedded-JSON `edit` string (`'{"old": "a", "new": "b",}'`) goes through `kimix::repair`;
  the goldens cover the valid object/array forms and the non-JSON/non-structural rejects, not the repair
  ladder itself.
* The plan-session retry reminder (`kimix.utils.prompt.build_plan_retry_reminder`,
  `PLAN_RETRY_CHECK_PREFIX = "[system check]"`, exercised by kimi-agent's
  `tests/test_plan_retry_reminder.py`) is a prompt-layer helper with no C++ counterpart in this checkout
  (`grep -rn "system check" src/` -> nothing); porting it is outside this tool's boundary.
* `tests/unit/tools/` in the kimi-agent checkout contains no plan/note test (`test_note.py` is the only
  reference suite for these tools); it was used in full as corpus material.
* `src/agent/demo/new_tools_e2e.cpp` exercises WritePlan/ReadPlan/EditPlan against a real LLM; it was not
  re-run here (no LLM in this environment). Its assertions ("successfully edited" in the transcript,
  the edited step on disk) still hold — `EditPlan`'s success message is unchanged.
