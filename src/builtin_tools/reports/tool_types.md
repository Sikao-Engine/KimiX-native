# tool_types / utf8_util / tool / tool_registry — shared infrastructure parity report

Worktree: `C:\dev\kimix-base` (branch `main`, uncommitted).
Files owned by this task:

| file | role |
|---|---|
| `src/builtin_tools/tool_types.h` / `.cpp` | shared line-stream kernels (`truncate_line`, `join_with_byte_limit`, `fold_lines`, `dedup_lines`) + the output-pipeline constants |
| `src/builtin_tools/utf8_util.h` / `.cpp` | UTF-8 primitives (`is_ascii`, `decode_code_point`, `utf8_code_point_count`, `utf8_byte_offset_of_code_point`, `utf8_floor_boundary`, `utf8_validate`, `utf8_strict_error`) |
| `src/builtin_tools/tool.h` / `.cpp` | `ToolParams`, `ValueElement`, alias resolution, JSON (de)serialization |
| `src/builtin_tools/tool_registry.h` / `.cpp` | static tool registration, `find` / `find_ci` / `create` |
| `tests/unit/builtin_tools/tool_types_goldens.inc` *(new, generated)* | 21 217 reference-derived vectors (19 950 UTF-8, 1 142 line-stream, 125 JSON) |
| `scripts/gen_tool_types_goldens.py` *(new)* | single source of the goldens |
| `tests/unit/builtin_tools/test_tool_types.cpp`, `test_tool.cpp` *(extended)* | golden replay + hand-written contract tests |
| `python/tests/test_parity_output_utils.py` *(new)* | live differential harness + golden-freshness check |

Reference implementation: `C:\dev\kimi-agent\kimi-cli\src\kimi_cli\tools\file\output_utils.py`,
`.../grep_local.py`, `.../glob.py`, and CPython itself (`bytes.decode("utf-8")`,
`json`). Never `python/kimix_native/*`.

## What was verified as byte-exact

| shared API | Python / CPython source of truth | vectors | status |
|---|---|---|---|
| `truncate_line(text, max_len, out)` | `output_utils.truncate_line` (134-150) | 55 | byte-exact (unchanged) |
| `fold_lines(lines, max_lines, head, tail, out, &omitted)` | `output_utils.fold_lines` (54-91) | 730 | byte-exact (unchanged) |
| `dedup_lines(lines, min_repeats, out, &saved)` | `output_utils.dedup_lines` (94-131) | 100 | byte-exact (unchanged) |
| `utf8_strict_error` / `utf8_validate` | CPython `bytes.decode("utf-8")` (`UnicodeDecodeError.start` / `.reason`) | 19 235 | **FIXED** (see #1) |
| `utf8_code_point_count`, `utf8_byte_offset_of_code_point`, `decode_code_point`, `utf8_floor_boundary`, `is_ascii` | CPython `len(str)`, `str[:k].encode()` offsets, `ord()` for valid UTF-8 | 715 | byte-exact (unchanged) |
| `join_with_byte_limit(lines, max_bytes, out, &truncated, &omitted)` | `grep_local._join_with_byte_limit` (618-632), identical loop inlined in `glob.py` 631-637 | 216 | **FIXED** (see #2) |
| `grep::parse_rtk_rg_output` (consumer of `output_utils.parse_rtk_rg_output`) | `output_utils.parse_rtk_rg_output` (161-239) | 41 | byte-exact (unchanged) |
| constants `k_max_output_bytes` (100 KiB = `100 << 10`), `k_record_cap` (500), `k_max_head_limit` (500) | `glob.py`/`grep_local.py`/`hash_line.py` `MAX_BYTES`, `grep_recorder.RECORDER_CAP` | — | correct; `k_max_lines_fold` documented as *not* `output_utils.DEFAULT_MAX_LINES` (200) |
| `ToolParams::get` / `contains` / `get_exact` / `contains_exact` / `with_aliases` / `add_alias` | `src/builtin_tools/README.md` §"Fuzzy alias matching" (canonical → alias exact → alias folded) | 8 new blocks | byte-exact contract, unchanged |
| `ToolParams::deserialize` / `serialize` / `try_deserialize` | CPython `json.loads` / typed round-trip | 60 + 38 + 6 + 15 + 6 | **1 fix** (see #3), 3 documented divergences |
| `ToolRegistry::{register_tool, find, find_ci, create, all, size}` | header contract | 2 new blocks | byte-exact contract, unchanged |

**Consumer tools verified as byte-exact on top of these** (each rebuilt and run):

| tool | shared API it consumes | result |
|---|---|---|
| glob | `truncate_line`, `fold_lines` (head/tail computed in `glob_tool.cpp::shape_output` exactly like `glob.py` 643-645) | `test_builtin_glob`: all passed (425 asserts / 41 tests) |
| grep | `parse_rtk_rg_output`, its own `join_with_byte_limit` copy, `utf8_validate` | `test_builtin_grep`: all passed (1383 asserts / 52 tests) |
| write | `utf8_strict_error` (via `write::utf8_decode_error`), `utf8_validate` | `test_builtin_write`: all passed (4379 asserts / 58 tests) |
| fetch_url | `truncate_line` (markdown `max_length` cap) | `test_builtin_fetch_url`: all passed (303 asserts / 65 tests) |
| bash / read / todo / web_search / run / agent / … | `utf8_code_point_count`, `utf8_validate`, `dedup_lines`-style helpers | full suite green (see *Verification*) |

## Discrepancies found and fixed

### #1 `utf8_strict_error`: the maximal-subpart rule (19 235-vector corpus, 3020 mismatches)

CPython validates the continuation bytes that are **present** before it can report a
truncated sequence; the port checked `i + len > n` first and therefore reported
`"unexpected end of data"` for every truncated sequence whose available continuation
byte was already illegal.

minimal repro

```
input : b"\xe0\x00"          (also b"\xed\xa0", b"\xf0\x90\x41", b"\xf0\x90\x41"…)
CPython: UnicodeDecodeError('utf-8', b'\xe0\x00', 0, 1, 'invalid continuation byte')
C++ (before): ok=false, offset=0, reason="unexpected end of data"
C++ (after) : ok=false, offset=0, reason="invalid continuation byte"   ✔
```

Found by transcribing the port into Python and diffing 421 103 inputs against
`bytes.decode("utf-8")`: **exactly one** diff class, 20 187 instances, all
`unexpected end of data` vs `invalid continuation byte`. Fix = validate the present
continuation bytes first, then report truncation (`utf8_util.cpp`). Regression vectors
in the golden corpus + hand-written cases in `test_tool_types.cpp`
(`utf8_decode_error_golden`, `utf8_helpers`).

### #2 `join_with_byte_limit`: the crossing line must be kept (216-vector corpus, 80 mismatches)

`output_utils.py` has no byte-budget join; the same-named Python function is
`grep_local._join_with_byte_limit` (the loop is inlined verbatim in `glob.py`
631-637, so `glob_tool.cpp::shape_output` already matched it). The shared helper
stopped *before* the line that reaches the budget and its header comment promised a
"fold note" that no code appended.

```
input : ["aaaa","bbbb","cccc","dddd"], max_bytes=10
Python: ("aaaa\nbbbb\ncccc", True)      # the crossing line is kept
C++ (before): "aaaa\nbbbb", truncated=true, omitted=2
C++ (after) : "aaaa\nbbbb\ncccc", truncated=true, omitted=1   ✔
```

The signature is unchanged; `omitted` remains the native extension (input lines left
after the crossing line). No caller existed, and the header/.cpp comments now name the
real reference. It is also the documented "the line that reaches the cap is kept"
rule of `glob.py`, so the two ports finally agree.

### #3 `ToolParams::serialize`: an object key with an embedded NUL was truncated

`yyjson_mut_obj_add_val()` takes a NUL-terminated key, so a key such as
`"a\u0000b"` (which `deserialize` *does* produce from `{"a\u0000b":1}`) was written
back as `"a"`.

```
input : {"a\u0000b":1}
C++ (before): serialize -> {"a":1}          (key truncated, round-trip lossy)
C++ (after) : serialize -> {"a\u0000b":1}   ✔
```

Fix: build the key as a yyjson string with an explicit length
(`yyjson_mut_strncpy` + `yyjson_mut_obj_add`). Regression test:
`json_serialize_fidelity` (embedded-NUL key and empty key).

## Documented divergences (pinned by tests, not fixed)

`ToolParams` holds `null / bool / int64 / uint64 / double / UTF-8 string`; yyjson is
called with `YYJSON_READ_NOFLAG`. The generator classifies every JSON vector itself
(`json_reasons()`), so nothing is hand-labelled; a text that mixes two reasons is a
hard generator error.

| bucket | example | CPython | native | why |
|---|---|---|---|---|
| non-object root (6) | `[1,2,3]`, `42`, `"str"` | parses | rejected | `ToolParams` is a JSON *object* body (documented contract) |
| unrepresentable (15) | `{"a":NaN}`, `{"a":1e999}`, `{"a":"\ud800"}` | parses (`nan`/`inf`/lone surrogate) | rejected | no NaN/Inf literal, non-finite double is an error, a lone `\udXXX` escape is not UTF-8. Fixing it needs a new `ValueElement` alternative (raw/bignum) — an API change, out of scope. A caller that needs these stays on the Python mirror. |
| lossy integer (6) | `{"a":123456789012345678901234567890}`, `{"a":-9223372036854775809}` | exact `int` | `double` (yyjson's documented policy) | integers outside `[INT64_MIN, UINT64_MAX]` silently lose exactness. Inside that range everything is exact (uint64 max verified). |

Other known, non-parity-relevant notes:

* Invalid **UTF-8 input** has no CPython answer; the native policy is "one code point
  per byte that cannot be folded" and is locked in `utf8_invalid_input_policy`
  (`b"\xe0\x80\x80"` → 1, `b"\xed\xa0\x80"` → 3, `b"\xe1\x80"` → 2,
  `b"\xf4\x90\x80\x80"` → 4). The sibling `src/runtime/common/utf8.cpp` chooses the
  *opposite* policy (overlong rejected, CESU surrogate accepted — pinned in the
  Python harness). Both are only ever fed valid UTF-8; neither is reachable with the
  other's policy, and `utf8_strict_error` (the strict predicate) is CPython-exact.
* `serialize()` emits object keys in `kimix::unordered_map` order, not insertion
  order (deterministic for a given insertion sequence, and JSON objects are
  unordered). The JSON goldens sort keys on both sides so this cannot mask a value
  bug.
* yyjson writes doubles with `%.17g`, so `3.14` serializes as `3.1400000000000001`
  while `json.dumps(3.14)` writes `3.14`. The **value** round-trips bit-exactly
  (tested with `memcmp` over 9 doubles incl. `-0.0`, `5e-324`, `DBL_MAX`).
* `ToolRegistry::find()` / `create()` return a pointer *into* the registry vector
  after the mutex is released, so a registration concurrent with a lookup (or an
  insertion after a lookup) could dangle — the header calls the registry
  "thread-safe". Every registration happens at static-init and nothing re-registers
  at runtime, so no caller can trigger it today. Deliberately **not** changed:
  a fix needs a different container (e.g. `vector<unique_ptr<ToolMeta>>`), and no
  deterministic regression test can distinguish the two storages.
* `grep_tool.cpp::parse_rtk_rg_output` (owned by the grep agent) rejects a protocol
  header whose counts exceed `uint32_t` (`digits_to_u32` → `false`, so
  `{"99999999999999999999999 matches in 1 files:"}` passes through as a real line
  while Python parses the integer). Out of this task's edit scope; the rtk golden
  corpus stays inside the uint32 domain on purpose.

## Verification

```
# goldens regenerated from the live reference (deterministic, --check clean)
python scripts/gen_tool_types_goldens.py
  wrote tests/unit/builtin_tools/tool_types_goldens.inc (21401 lines, 2428859 bytes)

# the two targets this task extends
python scripts/build_locked.py -- xmake build test_builtin_tool_types
  Suite 'global': all tests passed (104 asserts in 15 tests)     # was 74 asserts / 7 tests
python scripts/build_locked.py -- xmake build test_builtin_tool
  Suite 'global': all tests passed (321 asserts in 27 tests)     # was 117 asserts / 19 tests

# EVERY other builtin_tools target, rebuilt and run in this worktree (final pass)
  test_builtin_param_aliases   76 asserts / 27 tests
  test_builtin_read          3386 / 56
  test_builtin_grep          1383 / 52
  test_builtin_bash         18933 / 60
  test_builtin_glob           425 / 41
  test_builtin_write         4379 / 58
  test_builtin_edit           419 / 76
  test_builtin_python         637 / 46
  test_builtin_pwsh           444 / 36
  test_builtin_compact        143 / 55
  test_builtin_fetch_url      303 / 65
  test_builtin_read_image     814 / 44
  test_builtin_todo          3711 / 33
  test_builtin_web_search     151 / 35
  test_builtin_plan          1687 / 53
  test_builtin_run           1676 / 68
  test_builtin_job_output     897 / 63
  test_builtin_agent         6062 / 76
  test_builtin_workflow      1246 / 77
  test_agent                  123 / 16
  (all "Suite 'global': all tests passed"; test_builtin_retrieve does not compile, see below)

# python
python scripts/build_locked.py -- xmake build runtime_py
  build ok, spent 25.125s
python -m pytest python/tests/test_parity_output_utils.py -q
  14 passed in 1.19s
python -m pytest python/tests/test_parity_output_utils.py python/tests/test_builtin_tools.py -q
  33 passed in 6.96s
```

`runtime_py` was rebuilt after the C++ fixes. Note that none of the changed code is
reachable from Python (see *Known gaps*): the harness's live surface
(`runtime_py.common.is_ascii` / `utf8_code_point_count`) lives in
`src/runtime/common/utf8.cpp`, which this task did not touch, so a stale `.pyd` could
not have changed the harness verdict either way.

Pre-fix evidence (the same tests, before the C++ changes; the line numbers are those
of the pre-fix file):

```
FAILED in: tests\unit\builtin_tools\test_tool_types.cpp:379 - [3020 == 0]
       CPython bytes.decode('utf-8') parity over  19235  vectors
FAILED in: tests\unit\builtin_tools\test_tool_types.cpp:493 - join_with_byte_limit … want "aaaa\nbbbb\ncccc"
FAILED in: tests\unit\builtin_tools\test_tool_types.cpp:500 - [80 == 0]
       grep_local._join_with_byte_limit parity ( 216  vectors)
```

`test_builtin_retrieve` does **not** build in this worktree, for an unrelated reason:
`tests/unit/builtin_tools/test_retrieve_tool.cpp:344` uses the Boost.UT digit-pack
literal `99999999999999999999999_i`, which MSVC rejects (`error C2975` at
`ut.hpp(2465)`). That file is another agent's uncommitted work; no file of this task is
involved.

## Corpus provenance

* `tests/test_token_filter.py:812-843` and `tests/test_tools_async.py:225-227,536-538`
  (kimi-agent's own rtk fold-protocol samples) are in the `parse_rtk_rg_output` corpus
  verbatim; `tests/test_token_filter.py:131-171` motivates the 1000-line fold vectors.
* The UTF-8 corpus is exhaustive over the single bytes, over all 256 second bytes for
  22 representative leads (one per codec behaviour class), over the 3-byte lead ×
  first-continuation matrix, over a 4-byte continuation matrix, plus 6000 deterministic
  fuzz inputs (skewed and uniform) and 2000 random valid strings.
* The JSON corpus is hand-picked around the model boundaries (both integer ranges, the
  double range, `\u0000`, surrogate pairs, duplicate keys, 64-level nesting, 100 keys,
  whitespace, trailing content) and then classified by the generator.

## Known gaps

* No build-file change was needed: `test_builtin_tool_types` and `test_builtin_tool`
  were already registered in `tests/xmake.lua`, and no new target or source file was
  added to a project. `scripts/gen_tool_types_goldens.py` is a tools-only script (like
  the other `gen_*_goldens.py`).
* There is no encoder-side kernel: CPython's `UnicodeEncodeError` wording
  ("surrogates not allowed") is not represented anywhere, because a Python `str`
  carrying a lone surrogate cannot cross the binding boundary in the first place
  (`PyUnicode_AsUTF8AndSize` raises), and every `str.encode("utf-8")` in the reference
  is fed text that already round-tripped as UTF-8. The harness asserts the wording never
  appears in the goldens.
* `builtin_tools::utf8_*` and every `tool_types` kernel are **not reachable from
  Python** (no pybind11 binding exists; `runtime_py.common.{is_ascii,
  utf8_code_point_count}` are the *parallel* `src/runtime/common/utf8.cpp`
  implementations). The live harness therefore compares `common.*` with CPython, and
  the `builtin_tools` side is covered by the golden replay in Boost.UT only.
* `ToolParams` / `ToolRegistry` have no Python binding either; their goldens are
  Boost.UT-only, with CPython `json` as the value reference.
* rtk protocol counts above `uint32_t` are untested (see the grep note above), as is
  `ToolRegistry` behaviour under concurrent registration (no caller does it).
* `src/runtime/common/utf8.cpp` is a third implementation of the same primitives with
  a different malformed-input policy; it is outside this task's file scope and was not
  changed.
