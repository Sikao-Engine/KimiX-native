# src/cli S6 — `test_cli` coverage + the reference-derived golden gate

Step S6 of `src/cli/PLAN.md` (§6 work package S6: "`tests/unit/cli/test_cli.cpp`,
`tests/xmake.lua` → `xmake build test_cli` + `test_cli.exe` all green").
This report is the per-module traceability record of the S6 test wave: which
behaviour each test pins, where the expectations come from, and the exact
verification log.

Reference (read-only): `C:/dev/kimi-agent` — `src/kimix/cli_impl/{args,constants}.py`,
`src/kimix/utils/io.py`, `kimi_cli/config.py` (`_MODEL_DEFAULTS`,
`_tokenize_model_name`, `_keywords_match`, `_resolve_model_defaults`),
`src/kimix/agent_*.json`; derived specs `.kimix_cache/cli_specs/01_args_and_flow.md`,
`02_config.md`.

Nothing outside the listed deliverables was changed **except one 6-line production
fix** in `src/cli/cli_config.cpp` (see §5.1 — it is the only way to make the
documented `extend:"default"` rule observable at all). No file in `src/ext/**`,
`src/core/**`, `src/xmake.lua`, `src/cli/xmake.lua`, `src/cli/PLAN.md`,
`tests/xmake.lua` or the reference checkout was touched; no git write command was
run.

## 1. Files

| file | lines | content |
|---|---|---|
| `tests/unit/cli/test_cli.cpp` | 4267 (+1820) | +29 tests and 11 helpers; every S3/S4/S5 test kept unchanged |
| `tests/unit/cli/cli_config_goldens.inc` | 292 (new) | generated expectations (model defaults corpus + manifest tool paths) — never hand-edit |
| `scripts/gen_cli_config_goldens.py` | 587 (new) | derives the `.inc` from the reference (`--check`-able, `--reference`/`--out`/`--python`) |
| `src/cli/cli_config.cpp` | 1010 (+10) | the `extend:"default"` fix (§5.1): 1 line of code replaced by 3 + an 8-line comment |
| `src/cli/reports/cli_tests.md` | this file | coverage matrix + verification log |

Run:

```bash
python scripts/build_locked.py --timeout 900 -- xmake build test_cli
./bin/debug/test_cli.exe                       # 3998 asserts in 79 tests
python scripts/gen_cli_config_goldens.py       # regenerate the .inc
python scripts/gen_cli_config_goldens.py --check
```

The test binary takes no arguments (its `main()` does not call UT's
`parse_arg_with_fallback`, unchanged from S3): the per-section assert numbers in
§4.3 were measured by temporarily enabling it and running each test by name, then
reverting.

## 2. Coverage matrix

Sections already present (kept, still green): `session_*` (17 tests, S3),
`stream_*` (19 tests, S4), the S5 block (`app_*`, `repl_*`, `command_*`,
`tool_call_*`, `cli_main_*`), `print_colour_escapes_exact`.

### 2.1 `cli_args` — `parse_args` + the exit-code contract (`src/cli/cli_args.h`)

| behaviour | test |
|---|---|
| every reference flag and native addition, in every accepted spelling: `-c/--clean`, `-no_color/--no_color`, `-no_think/--no_think`, `-no_yolo/--no_yolo`, `--manually-cot`, `-h/--help`, `--version`, `--dry-run/--dry_run`, `--interactive`; all other fields stay default; several flags combine | `cli_args_flags_all_spellings` (57) |
| value options in both spellings (`--opt value` / `--opt=value` / `-p=value`): `-p/--prompt` (incl. empty value, spaces, and a value that looks like a flag), `--script`, `--work-dir/--work_dir`, `--agent-file/--agent_file`, `--provider`, `--config` | `cli_args_value_options_both_spellings` (44) |
| `config_is_provider_only` (true for `--provider`, false for `--config`) and first-hit-wins for repeated occurrences | `cli_args_config_and_provider_precedence` (10) |
| `-s/--skill-dir/--skill_dir` with 0/1/N values, terminated by a flag or a subcommand, plus the documented `--skill-dir=DIR` deviation (§5.2) | `cli_args_skill_dir_arity` (27) |
| `serve`/`gui`/`ssecli`/`mcp` recognition, remaining tokens landing verbatim in `subcommand_args`, flags before the subcommand, unknown bare token ≠ subcommand, a usage error not losing the subcommand | `cli_args_subcommands_and_remaining_tokens` (24) |
| error paths: unknown option, unknown positional, malformed flag spelling, missing value for every value option, `--` handling, error accumulation | `cli_args_error_paths` (31) |
| `program_name` derivation (both separator kinds, bare name, null `argv[0]`, `argc == 0`), the usage synopsis, and the plain/coloured/extended help text (`HELP_STR` byte-exactness itself stays `gen_cli_help.py`'s job) | `cli_args_program_name_and_usage_text` (32) |
| exit-code contract at `cli_main`: `kExitOk` 0 / `kExitConfig` 1 / `kExitUsage` 2 / `kExitUnsupported` 3 / `kExitRuntime` 4 (value), usage errors → 2, all four subcommands (with their own args) → 3, missing/malformed provider, missing agent manifest, no-provider → 1 (the last one only asserted when no `default_config.json` sits in the cwd, since `cli_main` prefers that file) | `cli_args_exit_code_contract` (43) |

### 2.2 `cli_common` — shared helpers (`src/cli/cli_common.h`)

| behaviour | test |
|---|---|
| `trim`/`trim_ascii`/`is_blank`: ASCII whitespace incl. `\x1c..\x1f`, the Unicode spaces U+0085/U+00A0/U+1680/U+2000/U+2003/U+200A/U+2028/U+2029/U+202F/U+205F/U+3000, and the non-whitespace U+200B/U+201F; `trim_ascii` leaves non-ASCII bytes alone; NUL is not whitespace | `cli_common_trim_and_blank` (26) |
| `starts_with`/`ends_with`/`contains`/`find`/`rfind` (empty needle, longer prefix, `from` offset, npos) and `to_lower_ascii`/`to_upper_ascii` (byte-wise, locale independent) | `cli_common_search_and_case` (24) |
| `split` (both `keep_empty` modes, empty text), `split_lines` (`\r\n`, interior blank line, trailing newline, empty text), `join`, `replace_all` (overlap, empty needle, delete) | `cli_common_split_join_replace` (39) |
| `join_path`/`file_name`/`parent_path`/`extension`/`with_file_name`, `absolute_path` (already absolute, relative vs cwd, `..`/`.` collapse) | `cli_common_paths` (21) |
| `read_file`/`write_file`: missing file (error + cleared out), round trip incl. UTF-8 bytes, truncation, append, missing parent directory, directory target | `cli_common_file_io` (22) |
| `make_dirs` (nested, existing, empty path) and `remove_all` (recursive, missing path) | `cli_common_make_dirs_and_remove_all` (11) |
| `random_hex` shape/length/alphabet/uniqueness, `now_unix_seconds`, `format_utc` (epoch + custom fmt), `format_duration_hm` (0..>24h, negative clamp), `file_mtime_unix` (existing + missing) | `cli_common_random_hex_and_time` (25) |
| `get_env`/`set_env`/`first_env` (missing, null name, overwrite, Windows' empty-value removal, first-set-wins) | `cli_common_env` (18) |

### 2.3 `cli_tools` + `cli_config` (`src/cli/cli_tools.h`, `src/cli/cli_config.h`)

| behaviour | test |
|---|---|
| the 25 `module:attr` → registry-name entries (table self-consistency), every documented path, `default_agent_tools()` (25 unique, all registered, table order), `resolve_tool_path` for unknown/malformed inputs (`""`, no `:`, empty module, empty attr, wrong case, leading/trailing space/newline, `a:b:read`, extra colon) | `cli_tools_table_and_resolve` (416) |
| both provider dialects: flat kimix (`base_url` beats `url`, capabilities, custom_headers, `env` application, `openai_settings` defaults, services, oauth storage default, unknown-key warning) and nested kimi-cli (`provider` + `models` table wins over the flat duplicates, `[model]` table form, warnings stay empty) | `cli_config_flat_provider_dialect` (44), `cli_config_nested_provider_dialect` (28) |
| `_MODEL_DEFAULTS` limit resolution: both derived (with warnings), explicit context → `/4`, a `None` row output (grok) → `/4`, unknown model without an explicit size → error, direct resolver checks incl. the fuzzy boundary (`mini` vs `gemini` = 80) and exact numeric tokens | `cli_config_model_default_limits` (41) |
| `type` → family normalisation for every accepted type alias, `to_llm_config` + `create_llm` accepting the normalised family, unsupported types rejected by name | `cli_config_type_normalisation` (46) |
| provider error paths: missing file/JSON/object, missing `model`/`type`/`url`, unsupported type, unknown model, and the explicit-size rescue | `cli_config_error_paths` (17) |
| `api_key` fallback order (`$KIMI_API_KEY` → `$KIMIX_API_KEY` → warning) and the explicit key winning (hermetic: the outer environment is saved and restored) | `cli_config_api_key_env_fallback` (20) |
| agent manifest semantics: `tools` replaces, `allowed_tools` wins, `exclude_tools` removes, `extend:"default"` without a list → the 25 built-in tools, `system_prompt_path` resolution (`./` stripped, relative vs manifest dir, absolute kept), `system_prompt_args` passthrough, `name`/`model`/`when_to_use`/`subagents`, dropped-unknown-path warning, bare top-level object, version/JSON/type errors | `cli_config_agent_manifest_semantics` (57) |
| `provider_report`/`agent_report`: deterministic, every label variant, `api_key: present|absent` (never the value), the report of a dropped tool path | `cli_config_reports_are_secret_free` (44) |
| goldens: the 17 `_MODEL_DEFAULTS` rows reachable through their canonical name | `cli_config_golden_model_rows` (52) |
| goldens: 175 reference model names (tokenisation, resolve result, per-row match mask, first-matching-row values, separator/uppercase equivalence) | `cli_config_golden_model_corpus` (1639) |
| goldens: the five manifests' tool paths — every path resolves, is registered in `ToolRegistry`, is part of `default_agent_tools()`, and the 25 distinct paths equal the table | `cli_tools_golden_manifest_paths` (335) |

### 2.4 `cli_app` — the dry-run report over the real reference configs

| behaviour | test |
|---|---|
| `app_init` + `app_dry_run_report` for `C:/dev/ds_flash.json` + `agent_worker.json`: `LLMConfig` line, family, 1024000 explicit `/4 = 256000`, 22/22/0 tools, the `enabled:` list equals the manifest resolved through `resolve_tool_path`, the real `api_key` value never appears, no session directory is created. Skipped with a message when `KIMI_AGENT_ROOT` (default `C:/dev/kimi-agent`) or `C:/dev/ds_flash.json` is missing | `cli_app_dry_run_report_real_provider` (18) |

## 3. The golden gate

`scripts/gen_cli_config_goldens.py` imports the reference **by path**
(`<reference>/kimi-cli/src/kimi_cli/config.py`, loaded under the private module
name `kimix_cli_config_reference`; the `kimi-cli/src` + `src` directories are put
on `sys.path` so the module's own imports resolve) and derives every expectation
from the real implementation. If the current interpreter cannot import the
module's dependencies it re-execs itself under
`<KIMI_AGENT_ROOT>/.venv/Scripts/python.exe` (the
`gen_tool_pairing_goldens.py` pattern); here the plain interpreter can import
them (orjson/regex/tomlkit/pydantic/rapidfuzz/kosong all present).

**Corpus (175 model names + 5 manifests / 25 distinct tool paths):**

| group | n | content |
|---|---|---|
| `canonical` | 17 | every `_MODEL_DEFAULTS` row's keyword phrases joined with `-` |
| `separator` | 16 | the same tokens with `__` instead of `-` |
| `typo` | 51 | per row: duplicated / dropped last alphabetic char, transposed last alphabetic run |
| `version` | 16 | per row: the last numeric component incremented (or `-2` when the row has none) |
| `multi_sep` | 10 | mixed `./_/:/ /::`, collapsing separators, uppercase, surrounding whitespace |
| `unknown` | 14 | `llama-3-70b`, `qwen2.5-72b`, `sonnet`, `opus`, `grok-5`, … |
| `edge` | 16 | `""`, whitespace-only, `----`, bare numbers (`5.6`, `0`, `2`), a U+00B7/U+FF13 form, the fuzzy-boundary `gpt-5.4-gemini` |
| `fuzz` | 35 | deterministic `random.Random(0x5D6C11)`: reference vocabulary pieces joined with random separators, 30 % upper-cased |

For every name the reference's own `_tokenize_model_name`,
`_resolve_model_defaults` and the per-row `_keywords_match` are recorded
(`ctx`/`out`, `-1` for the reference's `None`, and a 17-bit match mask), plus the
canonical-name resolve result for each row so every `_MODEL_DEFAULTS` row is
reachable. The generator also refuses to write when the reference contradicts
itself (a name resolvable with an empty mask, or a row unreachable through its
canonical name).

The header records the reference checkout and `git -C <reference> rev-parse HEAD`
for both checkouts:

```
// Reference checkout: C:\dev\kimi-agent
//   repo HEAD:     86b7bf634559af34c4b4f984ca94d4a9f76849be
//   kimi-cli HEAD: 86b7bf634559af34c4b4f984ca94d4a9f76849be
```

`--check` compares the whole generated text (header, counts, commit ids and
tables) with the committed file and exits 1 when anything differs; it prints
`in sync: … (17 rows, 175 model names, 5 manifests, 25 tool paths)` on success.
The tool-path half cannot be derived from the reference (the Python CLI resolves
`module:attr` dynamically with importlib), so the `.inc` carries the **distinct
path list** and the C++ test asserts each one resolves and is registered.

`HELP_STR` is deliberately *not* duplicated here: `scripts/gen_cli_help.py`
already owns `src/cli/cli_help_text.inc`.

## 4. Verification log

### 4.1 Tooling and goldens

```
$ python scripts/py_lint.py scripts/gen_cli_config_goldens.py
[py_lint] Syntax OK: C:\dev\kimix-base\scripts\gen_cli_config_goldens.py
$ python scripts/gen_cli_config_goldens.py
wrote C:\dev\kimix-base\tests\unit\cli\cli_config_goldens.inc (17 rows, 175 model names, 5 manifests, 25 tool paths)
corpus groups: canonical=17, edge=16, fuzz=35, multi_sep=10, separator=16, typo=51, unknown=14, version=16
$ python scripts/gen_cli_config_goldens.py --check
in sync: C:\dev\kimix-base\tests\unit\cli\cli_config_goldens.inc (17 rows, 175 model names, 5 manifests, 25 tool paths)
```

### 4.2 Builds and suites

```
$ python scripts/build_locked.py --timeout 900 -- xmake build kimix_cli
[100%]: build ok, spent 5.813s
$ python scripts/build_locked.py --timeout 900 -- xmake build test_cli
[100%]: build ok, spent 5.766s
$ ./bin/debug/test_cli.exe
Suite 'global': all tests passed (3998 asserts in 79 tests)
$ ./bin/debug/test_builtin_tool.exe
Suite 'global': all tests passed (525 asserts in 28 tests)
$ ./bin/debug/test_agent.exe
Suite 'global': all tests passed (214 asserts in 19 tests)
```

Baseline before S6 (same machine, same command): **787 asserts in 50 tests** →
now **3998 asserts in 79 tests** (+3211 asserts, +29 tests, 0 removed/renamed).

### 4.3 Per-section delta (asserts measured per test by name)

| section | tests | asserts | notes |
|---|---|---|---|
| `session_*` (S3) | 17 | 295 | unchanged |
| `stream_*` + `print_colour_escapes_exact` (S4/S1) | 20 | 250 | unchanged |
| S5 block (`app_*`/`repl_*`/`command_*`/`tool_call_*`/`cli_main_*`) | 13 | 242 | unchanged (incl. the S1 `cli_main` test) |
| **`cli_args_*`** | 8 | **268** | new |
| **`cli_common_*`** | 8 | **186** | new |
| **`cli_tools_*`** | 2 | **751** | new (incl. the golden manifest test) |
| **`cli_config_golden_*`** | 2 | **1691** | new (the 175-name corpus + 17 rows) |
| **`cli_config_*`** (dialects/errors/reports) | 8 | **297** | new |
| **`cli_app_dry_run_report_real_provider`** | 1 | **18** | new |
| total | 79 | 3998 | 787 + 3211 |

### 4.4 The gate actually gates

```
$ # patch one generated expected value: grok max_output -1 -> 999999
$ python scripts/gen_cli_config_goldens.py --check
C:\dev\kimix-base\tests\unit\cli\cli_config_goldens.inc is out of date - regenerate with python scripts/gen_cli_config_goldens.py
check exit=1
$ ./bin/debug/test_cli.exe
FAILED in: tests\unit\cli\test_cli.cpp:3531 - test condition: [0 == 999999] canonical 'grok'
FAILED in: tests\unit\cli\test_cli.cpp:3548 - test condition: [0 == 999999] canonical 'grok'  (first matching row)
FAILED in: tests\unit\cli\test_cli.cpp:3565 - test condition: [0 == 999999] canonical 'grok'  via  grok
FAILED in: tests\unit\cli\test_cli.cpp:3575 - test condition: [0 == 999999] canonical 'grok'  via  GROK
tests: 79 | 1 failed
asserts: 3998 | 3994 passed | 4 failed
$ cp .kimix_cache/goldens_backup.inc tests/unit/cli/cli_config_goldens.inc
$ sha256sum -c .kimix_cache/goldens.sha
tests/unit/cli/cli_config_goldens.inc: OK
$ python scripts/gen_cli_config_goldens.py --check   # in sync again
$ ./bin/debug/test_cli.exe
Suite 'global': all tests passed (3998 asserts in 79 tests)
```

The four failures are the four independent replay paths (the row canonical name,
the first-matching-row cross-check, the token-joined spelling, the upper-cased
spelling), so a golden drift cannot pass unnoticed.

### 4.5 Worktree

```
$ git status --porcelain src/cli tests scripts
 M src/cli/cli_print.cpp      (S1, pre-existing)
 M src/cli/main.cpp           (S5, pre-existing)
 M tests/xmake.lua            (pre-existing S1-S5 registration)
?? scripts/gen_cli_config_goldens.py   (S6)
?? src/cli/cli_config.cpp … (S2-S5 files, untracked from earlier waves)
?? src/cli/reports/          (S5 report + this file)
?? tests/unit/cli/           (S3-S5 test file + the S6 .inc)
```

Only the files listed in §1 changed in this wave (mtimes: 19:52 and 20:00-20:01
for the S6 deliverables vs 19:07-19:38 for the pre-existing modifications).

## 5. Findings

### 5.1 BUG (fixed, `src/cli/cli_config.cpp`): `extend:"default"` dropped every tool

A manifest with `extend:"default"` and **no** `tools`/`allowed_tools` produced an
agent with **zero** tools and 25 `dropped unknown tool path 'Read'`-style
warnings, because the fallback assigned the *registry names*
(`default_agent_tools()` → `"Read"`, `"Bash"`, …) to the list that is then run
through `resolve_tool_path()`, which only understands `"<module>:<attr>"` paths
(no `:` → malformed → dropped). PLAN.md §4 states the opposite:
`extend:"default"` → `enabled_tools = default_agent_tools()`.

Failing case (observed before the fix, via the test harness):

```
PROBE default_enabled=0 defaults=25 warnings=25      # {"agent":{"extend":"default"}}
```

Not reachable through any of the five real `agent_*.json` manifests (they all
list their tools), which is why the S2 gate did not catch it. Fix: the default
case now requests the table's *paths* (`agent_tool_table()` first column)
instead of the registry names, so the existing resolution loop (and the
`exclude_tools` filter it applies) keeps working and the result is exactly
`default_agent_tools()` in order. After the fix:

```
$ ./bin/debug/kimix_cli.exe --dry-run --provider C:/dev/ds_flash.json \
      --agent-file <tmp>/agent.json          # {"agent":{"extend":"default"}}
  tools requested: 25
  tools enabled: 25
  tools dropped: 0
  enabled: Read, ReadImage, Glob, Grep, Edit, Write, FetchUrl, WebSearch, WritePlan,
           ReadPlan, EditPlan, Subagent, SendMessage, ListAgents, InterruptAgent,
           Workflow, TodoWrite, TodoUpdate, Retrieve, Compact, Bash, Pwsh, Run,
           Python, JobOutput
```

`cli_config_agent_manifest_semantics` now pins this (every entry of
`enabled_tools` equals `default_agent_tools()` in order).

### 5.2 Deviation (documented, not fixed): `--skill-dir=DIR` is a usage error

The reference `parser.add_argument("-s", "--skill-dir", nargs="*")` accepts
`--skill-dir=DIR` (argparse splits the explicit `=value`), while the native
`-s` branch only matches the exact tokens `-s`/`--skill-dir`/`--skill_dir`, so
`--skill-dir=a` ends in `unrecognized argument: --skill-dir=a` → exit 2. Not
fixed here (production behaviour outside S6's scope, and no test or documented
contract depended on it); pinned by `cli_args_skill_dir_arity` with a comment
naming the reference behaviour, and listed here so the deviation is visible.

### 5.3 Two siren-test traps worth knowing (test-side, no product impact)

* `_putenv_s(name, "")` **removes** the variable on Windows, so a "set but
  empty" environment state is not observable there; `cli_common_env` asserts the
  platform-correct behaviour and `cli_config_api_key_env_fallback` exercises both
  fallbacks through set-but-empty/absent states that behave identically on either
  platform.
* `std::filesystem` splits only the **last** separator: `with_file_name`/
  `system_prompt_path` keep the forward slashes of a relative segment
  (`<dir>\prompts/main.md`) and `with_file_name` joins with the native
  separator. The tests assert the real (and reference-compatible) shapes via
  `parent_path`/`join_path` instead of hard-coding one separator.

## 6. Not covered (and why)

* **`kExitRuntime` (4) through `cli_main`** — every route to it needs a failed
  LLM turn, i.e. a socket round trip (the reference and the native providers
  retry 3× with backoff: measured ≈7 s against a closed local port). The rule is
  not ducked: the enum value is asserted, the mapping
  (`app_run_prompt == false → kExitRuntime`, `cli_app.cpp`) is exercised at the
  app layer with the scripted backend, and the dry-run/provider error routes
  (1/2/3) are asserted end to end. No test performs network I/O.
* **`get_env` on a set-but-empty variable under Windows** — unobservable
  (§5.3), asserted as such.
* **The Python-side reference behaviour of `/init`, the MCP/subcommand
  front ends and the SQLite session store** — out of scope for the CLI library
  (documented reductions in `PLAN.md` §5 and `cli_commands.md`).
* **Golden coverage of the reference's *provider parser* itself** — the
  reference parses providers with pydantic models whose error strings and
  defaults are not extractable without a working kimi-cli installation; the
  provider tests therefore build temp JSON in both dialects and assert the
  native loader's documented behaviour instead (the model-defaults half *is*
  reference-derived).
* **`agent_*.json` → registry-name mapping as a golden** — not derivable from
  the reference (dynamic importlib resolution); the `.inc` carries the distinct
  path list and the C++ side pins the mapping through
  `ToolRegistry`/`resolve_tool_path` instead.
