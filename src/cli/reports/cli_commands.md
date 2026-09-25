# src/cli S5 — the REPL, the slash commands and the application layer

Step S5 of `src/cli/PLAN.md` (§3.7 frozen interface, §3.8 command table). This
report is the per-command traceability record: what is ported, what is reduced,
and why. Reference (read-only): `C:/dev/kimi-agent/src/kimix/cli_impl/{core,commands,utils,args}.py`,
`kimix/ui/{printing,stream}.py`, `kimix/utils/{__init__,session,fix_error}.py`,
`kimi_cli/session.py`; derived specs `.kimix_cache/cli_specs/01_args_and_flow.md` (§4-§5),
`03_ui.md` (§3), `04_commands_session.md` (§1, §3, §5).

Nothing outside `src/cli` was changed: in particular **`src/agent/soul.{h,cpp}`
and `src/llm/**` are untouched** (see "Tool calls and tool results" for how the
CLI drives the renderer with the callback the soul already exposes).

## 1. Files

| file | lines | content |
|---|---|---|
| `src/cli/cli_app.h` | 147 | `app_context`, `app_init/app_run_prompt/app_compact/app_dry_run_report/cli_main/app_prompt_line` + the S5 additions |
| `src/cli/cli_app.cpp` | 789 | config → LLM/soul wiring, one turn + rendering + persistence, session (re)binding, `cli_main` |
| `src/cli/cli_commands.h` | 55 | `command_result`, `command_entry`, `command_map()`, `find_command()`, `split_text_blocks()` |
| `src/cli/cli_commands.cpp` | 1335 | the 21 reference handlers + the `unknown` fallback, the `_split_text` kernel, the TODO-comment scanner |
| `src/cli/cli_repl.h` | 34 | `repl_run` |
| `src/cli/cli_repl.cpp` | 139 | the `_client_cli` loop |
| `src/cli/main.cpp` | 25 | console setup + `cli_main` + flush (the S1 `bootstrap_main` placeholder is gone) |
| `tests/unit/cli/test_cli.cpp` | 2447 (+789) | S3/S4 tests + 14 S5 tests (787 asserts in 50 tests) |

## 2. Flow mapping (reference function → native function)

| reference | native |
|---|---|
| `cli_impl/main.py::cli()` (finally: `delete_session_dir`, `flush`) | `main.cpp::main` → `cli::cli_main` → `cli::flush_streams()` |
| `cli_impl/core.py::_run_cli` | `cli::cli_main` (`parse_args` → `init_printing`/`set_quiet(false)` → `--help`/`--version` → subcommand refusal → `--dry-run` → `app_init` → `-p/--prompt` \| `--script` \| REPL) |
| `cli_impl/core.py::_check_native` | not ported: the native CLI has no Python fallback to report (documented reduction) |
| `cli_impl/core.py::_client_cli` | `cli::repl_run` (+ `cli_app.cpp::app_read_input` = `_input`) |
| `cli_impl/utils.py::_input(text, text_arr)` | `cli::app_read_input(app, prompt, line)` (pending queue first, then `app.input`, prompt on `app.output`) |
| `cli_impl/utils.py::_split_text` | `cli::split_text_blocks` (verbatim algorithm, incl. the `''` join and the "known command becomes its own entry" rule) |
| `cli_impl/commands.py::_command_map` / `_cmd_unknown` | `cli::command_map()` (21 entries + `unknown`) / `cli::find_command` |
| `cli_impl/constants.py::HELP_STR` | `cli::cli_help_text_extended` (S1's generated `.inc`, `/help` byte-identical) |
| `utils/__init__.py::prompt(prompt_str, session, format_output=True)` | `cli::app_run_prompt` |
| `utils/session.py::_print_usage` (after a turn) | `cli_app.cpp::cliapp_run_turn` → `Finished, context usage: {pct} ({tokens} tokens)  time: H:MM:SS` (green/bold) |
| `utils/session.py::print_usage()` (`/context`) | `cli::app_usage_text` + `print_success("Context usage: " + …)` |
| `utils/session.py::compact_default_context` | `cli_commands.cpp::clicmd_compact` (guard + `Start compacting...`) → `cli::app_compact` (soul + `Context usage from A to B  time: H:MM:SS`) |
| `utils/session.py::clear_default_context` | `clicmd_clear` (`store.clear_context` + `app_rebind_session`) |
| `utils/session.py::{create_session,close_session}` | `cli::app_open_session` / `session_store::close` |
| `kimi_cli/session.py::Session.create/find/copy` | `session_store::{open,store_as,copy_into,list}` (S3) |
| `utils/config.py::init` + `soul/agent.py` (provider/agent → LLM + tools) | `cli::app_init` |
| `_cmd_*` (21 handlers) | `clicmd_*` (same names, same literals) |
| `/export` → `Session.export` (markdown) | `session_store::export_markdown` (S3) |

REPL semantics implemented exactly as spec 01 §5.2: prompt
`"\n>>>>>>>>> Enter your prompt or command:\n"`; empty input → `continue`
(no print, no backend call); a `/` input → `task = input.substr(1)`, the colon is
searched in `trim(task)` but the slicing uses the **unstripped** `task`
(`"/help "` → key `"help "` → unknown; `"/file: X"` → payload `" X"`); the key is
looked up verbatim (case-sensitive, no strip) with the `unknown` fallback
(`Unrecognized command.`); a non-slash token that names an existing file →
`.py` prints `Executing <name>` and is reported as **unsupported** (no embedded
Python), any other file is read and sent as the prompt (`File not executable,
consider as prompt.` in debug); EOF → `print_success("\nbye.")` and exit 0.

## 3. Command table as implemented

| command | port | note / reduction reason |
|---|---|---|
| `/help` | full | `print(cli_help_text_extended(colorful()))` = the reference text + the native additions block |
| `/clear` | full | `clear_context` + rebind; usage `< 1e-8` re-prints the usage line only (reference behaviour) |
| `/compact` | full | 1e-8 guard, `Start compacting...`, soul compaction, `Context usage from A to B  time: H:MM:SS` |
| `/context` | full | `Context usage: P% (T tokens)` from `soul->estimated_tokens()` / the provider's `max_context_size` |
| `/exit` | full | save + `close(true)` (an anonymous dir is deleted) + `bye!` + `should_break` |
| `/file:<path>` | full | returns the file text as `next_input` (the reference discards it - see §5.1) |
| `/txt` | full | multi-line until `/end` or `/cancel`, `_split_text` blocks queued back into `text_arr` |
| `/export[:path]` | full | `store.export_markdown`; `Exported N messages to P` (P = the given path; no resolved-path echo for the directory form) |
| `/resume:<id>` | full | `Session <id> not found.` (debug) before creating; rebinds store+session+soul |
| `/store:<id>` | full | `store_as` (copytree semantics, refuses an existing target), the current session stays current |
| `/load:<id>` | full | y/n confirmation above the 1e-8 epsilon, copy into a fresh **anonymous** session, rebind |
| `/sessions[:<name>]` | full | table `marker/2sp/id/2sp/updated_at(UTC)/2sp/usage(22)/2sp/title`; source = the filesystem scan (see §5.2) |
| `/cmd:<cmd>` | full | `os.system` → the process runner with the platform shell (`cmd.exe /c` / `/bin/sh -c`), captured output echoed through `print_raw`; `Done.`/`Failed.` |
| `/fix:<cmd>` | full | up to 4 attempts: `Shell: <cmd>`, output sliced from the first `error` line, prompt `Fix error from command ...`, `No error.` when the first run succeeds |
| `/todo:<path>` | full | the 7 suffix families; TODO predicate `(?<![A-Za-z0-9])TODO(?![A-Za-z0-9])` on the uppercased comment; prompt text byte-identical (single/multi forms) |
| `/init` | **reduced** | the interactive wizard is not ported: a `default_config.json` template is written into the working directory (`default_config.template.json` when one exists), the flags are explained, then a fresh anonymous session is created + `Initialized.`. Reason: the wizard is a Python `input()`/`getpass` + `orjson` writer; the template keeps the same observable effect (a config file + a new session) |
| `/plan[:path]` | **reduced** | `<path>` or `.kimix_cache/plan_<16hex>.md`, banner, requirement read to `/end`/`/cancel`, `No requirement provided.`; then **one** generation turn on the current session with `session.plan_path` set (the `plan_writing_path` custom data). Reason: the planner sub-session + `build_plan_retry_reminder` + the 3-attempt `Generating plan (attempt N/3)...` loop + the y/n review loop need the Python planner runtime; spec 04 §5.2 explicitly allows "generate once, then execute" |
| `/swarm` | **reduced** | banner, `/cancel` honoured, `No input provided for swarm.`; then one turn in an **isolated anonymous session** with `session.swarm_enabled = true` (the reference's `custom_data['is_swarm_session']`), closed (deleted) afterwards. Reason: no asyncio multi-agent supervisor; the workflow tool's swarm mode is reachable through the flag |
| `/supervisor` | **reduced** | same shape, `agent_boss.json` next to the current manifest when it loads (else the current manifest + a warning), `swarm_enabled = false`. Reason: no supervisor role runtime |
| `/reflection` | **reduced** | the two pre-checks (`No active session…`, `Context is empty…`) + `print_info(prompt)` are kept; the prompt is rebuilt against the **native** layout (src/cli, src/agent, src/builtin_tools, the session's enabled tools, the report path) instead of the Python repo + `importlib`/`inspect` tool-path listing. Reason: `importlib` + `inspect.getfile` are not available; spec 04 §5.2 allows a static/reduced prompt |
| `/code:<path> [args…]` | **reduced** | payload split on whitespace, relative path resolved against the working dir, `Script file not found:`/`Executable not found:`; non-`.py` runs the file directly (inherited output captured + `Done (exit code 0).`/`Exited with code N.`); `.py` prints `Executing <name>` and **spawns `python <script> <args…>`** instead of `exec(src, exec_ctx)`. Reason: no embedded Python interpreter / persistent `exec_ctx`; spawning is spec 04 §5.2's recommended fallback |
| *anything else* | full | `Unrecognized command.` (bright yellow) |

## 4. Config → agent wiring (`app_init`)

* `system_prompt` = the manifest's `system_prompt_path` contents with
  `${name}` → `system_prompt_args[name]` substitution; empty when unset (the
  soul's default then applies). `-s/--skill-dir` is **not** applied: the soul's
  system prompt has no skill section, so `opts.skill_dirs` is accepted and
  ignored (documented deviation; the reference appends `{SKILLS}` in
  `utils/system_prompt.py`).
* `enabled_tools` = the manifest's resolved registry names (`cli_tools.cpp`).
* `max_tokens` = the provider config's resolved value;
  `tool_call_buffer_tokens` = `max_tokens / 4` (the provider config carries no
  explicit value; the reference derives it in `LoopControl`).
* `auto_compact = true`; `max_steps`/`min|max_preserved_turns` stay at the soul
  defaults.
* `session.plan_enabled` = the enabled list contains `WritePlan`/`ReadPlan`/`EditPlan`.
* `custom_title` = the first input truncated to 60 characters, only while
  `state.json` has none (**reduced**: no LLM title pass / no `title_generate_attempts`).
* `--no_think`: `src/llm/openai/openai_chat.cpp` always sends
  `thinking:{type:enabled}` + `reasoning:{effort}` + `reasoning_effort`, and no
  provider reads `reasoning_key` (only `--dry-run` reports it), so there is **no
  provider knob** to turn reasoning off. `--no_think` therefore (a) clears the
  in-memory `provider.reasoning_key` (the reference's documented "empty
  reasoning_key disables round-tripping") and (b) constructs the renderer with
  `show_thinking = false`, which suppresses every `ThinkPart`/[Think] banner.
  The request body is unchanged - a recorded deviation.

## 5. Tool calls and tool results, session rebinding, and other freedoms taken

**5.1 Tool calls and tool results reach the renderer without touching `soul.cpp`.**
`KimiSoul::turn()` streams text / reasoning / tool-call deltas through its single
`SoulEventCallback` but executes the tools itself and never reports the tool
*results*. The CLI drives the renderer from that same callback
(`cliapp_on_chunk`: reasoning delta → `on_reasoning_delta`, text → `on_text_delta`,
a tool-call delta with a name → `on_tool_call_begin` (its arguments included),
otherwise `on_tool_call_args_delta`) and flushes the tool results lazily out of
the live session history: before the first chunk of the next step (the soul
appends the tool messages between two chat calls) and once more after the turn
returns. `cliapp_flush_tool_results` resolves each tool message's name through
its `tool_call_id` (the nearest preceding assistant message) and renders
`on_tool_result(name, ok, message, summary)` where the result JSON
(`{status, output, message, brief}`, e.g. `Read`) supplies
`ok = status == "ok"`, `message` and `summary = output`. This needed **no soul
change**: `soul.h`/`soul.cpp` are byte-identical to S2's revision.

**5.2 `/resume`, `/store`, `/load`, `/sessions:<name>`, `/clear` rebind the soul.**
The frozen `app_context` holds the store + `AgentSession` + `KimiSoul`; a
`KimiSoul` keeps a reference to its session, so `app_rebind_session`
(the S5 addition) destroys the soul first, rebuilds `AgentSession` on the
store's directory (`state_dir`, `session_id`, `plan_enabled`, todos reloaded
from `state.json`, history from `context.jsonl`) and then builds a new
`KimiSoul` from the stored `soul_options`. `/resume` opens id with
`resume = true` (creating a named session when absent, like the reference),
`/sessions:<name>` with `resume = false` (named, dir reset), `/load` opens a
fresh **anonymous** session and copies the named one into it with
`copy_into`, `/store` copies the current directory to the target id and stays on
the current session, and `/clear` empties the owned files
(`store.clear_context`) and rebinds in place - keeping the id, the directory and
the anonymous flag.

**5.3 A handler's `next_input` is fed back to the REPL.**
`command_result::has_input/next_input` is pushed to the **front** of the pending
queue (the reference assigns `input_str = new_input_str` and then discards it at
the top of the loop, spec 01 §5.2 "result is discarded [IMPORTANT]"); the
interface's comment ("feed `next_input` as the next REPL input") is followed
instead, so `/file:<path>` now really loads the file as the next input (the
documented purpose in `HELP_STR`). `text_arr` entries (`/txt`) queue behind it -
the reference's FIFO order is preserved.

**5.4 `/sessions` reads the filesystem scan, not an in-memory cache.**
The reference lists `_globals._cli_sessions` (populated in-process, `-` when the
usage is unknown). The native CLI has no such cache, so
`session_store::list(work_dir)` (sorted `updated_at` desc, title =
`custom_title` else `Untitled`, `-` when usage is unknown) is printed with the
identical column layout and the `*` marker for the current session.

**5.5 `/cmd`, `/fix`, `/code` go through `builtin_tools::proc::run_process`**
(shell argv on both platforms for `/cmd`/`/fix`), with a 600 s bound so a hung
child cannot block the CLI; the merged output is echoed through `print_raw`
instead of inheriting the console (the bytes are the same, the interleaving is
not). The child's stdout/stderr still reach the capture buffer.

**5.6 `/todo` carries its own scanner.** `src/runtime/parse/comment_scanner.h`
(`scan_comments(lang_kind, …)`) already implements the seven families, but that
translation unit is compiled into `runtime_py` only and `src/cli/xmake.lua` is
frozen (it globs `*.cpp`, so the file cannot be added to `kimix-cli`); the CLI
therefore carries a self-contained scanner (`clicmd_scan_comments`) that
implements the same span table (line/block/doc, string skipping) for the TODO
prompt. Its Unicode-sensitive C regex-literal heuristic is not reproduced.

**5.7 `/load`'s y/n confirmation treats EOF as "no".** The reference's
`_input('', text_arr)` raises `EOFError`, which the outer handler turns into a
traceback; the native CLI prints `Load cancelled.` instead.

## 6. Interface additions (all documented, S6/S7 may depend on them)

`app_context` gained: `work_dir`, `provider_path`, `agent_path`, `soul_options`,
`pending/input/output` (the borrowed queue + streams `_input` needs),
`injected`, `initialized`, `session_closed`, `title_locked`.
`cli_app.h` additionally declares: `app_prompt_line()`,
`app_read_input()`, `app_open_session()`, `app_rebind_session()`,
`app_save_session()`, `app_usage()`, `app_usage_text()`, `app_run_isolated()`;
`cli_commands.h` declares `split_text_blocks()`.
`app_init` gained the `injected` backend parameter (the soul's own test seam).

## 7. Verification (all commands actually run)

* `python scripts/build_locked.py --timeout 900 -- xmake build kimix_cli` → `build ok`.
* `python scripts/build_locked.py --timeout 900 -- xmake build test_cli` → `build ok`;
  `./bin/debug/test_cli.exe` → `Suite 'global': all tests passed (787 asserts in 50 tests)`
  (S3/S4's 545 asserts are intact; +242 S5 asserts).
* `./bin/debug/kimix_cli.exe --dry-run --provider C:/dev/ds_flash.json --agent-file
  C:/dev/kimi-agent/src/kimix/agent_worker.json` → the provider/agent report + `OK`
  (exit 0); `--help` (2408 chars, exit 0), `--version` → `kimix_cli 1.2.1 (kimix 1.2.1)`,
  an unknown flag → exit 2, `serve` → exit 3.
* Piped REPL: `/help`, `/context`, `/exit` with stdin not a console → exit 0,
  `Context usage: 0.0% (0 tokens)`, `bye!`, no LLM traffic (0 bytes on stderr, instant).
* `--script`: `/context` + `/exit` → exit 0 with **no** prompt line printed (the queue
  short-circuits `_input`, the reference's contract); `/sessions:cleanme` created
  `<work-dir>/.kimix_cache/cleanme`; `--clean` + `/resume:cleanme` + `/exit` removed it.
* Regression proofs (temporary break, then byte-identical revert, md5 checked):
  1. the slash-split rule (key/payload taken from the *stripped* string) →
     `asserts: 787 | 783 passed | 4 failed` (the 4 failures are
     `repl_slash_split_rule_and_unknown`: "no command calls the model",
     "the command key is used verbatim (no strip)", "/help must not run",
     "the payload is sliced from the unstripped string");
  2. the `unknown` fallback text → `asserts: 787 | 785 passed | 2 failed`
     ("the command key is used verbatim" via the count and the fallback-text assertion).
     Both files were restored byte-identically (md5 equal) and the suite is green again.
