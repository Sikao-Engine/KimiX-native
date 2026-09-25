# S4 report — `cli_stream.{h,cpp}` (port of `kimix/ui/stream.py`)

Reference (read-only): `C:/dev/kimi-agent/src/kimix/ui/stream.py` @ `86b7bf6`,
`kimix/ui/printing.py` (PrintStream / colorful_text) and the derived spec
`.kimix_cache/cli_specs/03_ui.md` §2.  Line numbers below are that revision.

Files (S4 deliverables):

| file | lines | notes |
|---|---|---|
| `src/cli/cli_stream.h` | 263 | PLAN.md §3.5 verbatim + display blocks + the documented additions |
| `src/cli/cli_stream.cpp` | 1224 | renderer, incremental JSON argument lexer, display blocks, banner |
| `tests/unit/cli/test_cli.cpp` | 1617 (was 905) | 19 new test lambdas, byte-exact literals |
| `src/cli/reports/cli_stream.md` | 202 | this file |

## 1. Byte-exact format table

Colours are `printing.py`'s `Color`/`Color256` values; `\x1b[0m` always closes a
coloured run.  "literal" is the exact text (UTF-8 for the glyphs).

| # | output | literal | colour | defined by |
|---|---|---|---|---|
| 1 | tool header | `⚡ <name>` (U+26A1 + space) | 95 | stream.py:918‑920 |
| 2 | text delta | the chunk, unchanged | — | stream.py:1096‑1098 |
| 3 | reasoning banner (first chunk of a run) | `[Think] ` + chunk | 96 | stream.py:1075‑1077 |
| 4 | reasoning continuation | chunk | 96 | stream.py:1078‑1080 |
| 5 | streamed arg label | `\n` + `<key>` + `:\n` | 38;5;245 | stream.py:783‑785 (`_separator()` = `"\n"`) |
| 6 | inline arg separator (`command`) | `" "` | 38;5;245 | stream.py:778‑781 |
| 7 | compact arg (` key:value`) | `" "` + `key:value`, value cut at 60 chars + `...` | 95 | stream.py:804‑822 |
| 8 | streamed arg value | decoded chunk | `old`91 `new`92 `code`94 `command`94 `prompt/question/instruction/task`93 `content`90 `source_code`96 `text`/default 38;5;250 `context` 38;5;245 | stream.py:462‑475, 490, 844‑845 |
| 9 | result ok | `✓ <name>` (U+2713 + space) | 92 | stream.py:996, 1020‑1025 |
| 10 | result error | `✗ <name>` (U+2717 + space) | 91 | stream.py:996, 1020‑1025 |
| 11 | result detail | `  <message>` (two spaces) | 90 | stream.py:1027‑1033 |
| 12 | result fallback (no tool call) | `✓/✗ <message>` | 92/91 | stream.py:1034‑1041 |
| 13 | empty result | a bare `\n` (`print_word('', True)`) | — | stream.py:1043 |
| 14 | brief block | `block.text` (skipped when empty) | 90 | stream.py:146‑148 |
| 15 | diff header | `Diff: <path>` | 93 | stream.py:150‑151 |
| 16 | diff old line | `- <line>` | 91 | stream.py:152‑153 |
| 17 | diff new line | `+ <line>` | 92 | stream.py:154‑155 |
| 18 | todo done | `- ~~<title>~~` | 90 | stream.py:159‑161 |
| 19 | todo in progress | `- <title> ←` (U+2190) | 93 | stream.py:162‑164 |
| 20 | todo other | `- <title>` | 38;5;250 | stream.py:165‑167 |
| 21 | shell block | *nothing* | — | stream.py:168‑170 |
| 22 | background task block | `[<status>] <task_id>: <description>` | 90 | stream.py:171‑175 |
| 23 | unknown block | `str(data)` | 90 | stream.py:176‑177 |
| 24 | base block | `str(model_dump())`, skipped when empty | 38;5;250 | stream.py:178‑181 |
| 25 | block join | parts joined with `\n`, plus one trailing `\n` | pre-coloured | stream.py:182‑184 |
| 26 | compaction begin | `Compacting...` (three ASCII dots) | 95 | stream.py:1060‑1062 |
| 27 | context-usage divider | `'='*20 + " Context usage: " + percentage_and_token + " " + '='*max(80-len(left),1)` then `\n` | 38;5;245 | stream.py:117‑133 |
| 28 | `percentage_str` | `f"{x*100:.1f}%"` | — | stream.py:1183‑1184 |
| 29 | `percentage_and_token` | `f"{x*100:.1f}% ({tokens} tokens)"` | — | stream.py:1187‑1189 |
| 30 | trivial messages | `success` / `failed` / `[rtk] success` / `[rtk] failed` suppress #11 | — | stream.py:1027 |
| 31 | newline rule | a bare `\n` is inserted (never removed) when `require_new_line` is set and the previous *raw* character was not `\n` | — | printing.py:391‑405 |
| 32 | `\n` between content types | text chunks when `_state != Text`; `[Think]`/args header always `require_new_line=True`; args/values `False` | — | stream.py:1096‑1098, 1075‑1080, 918‑920, 844‑845 |
| 33 | unknown JSON escape | backslash kept verbatim (`\d` → `\d`) | — | stream.py:702‑710 |
| 34 | lone/paired surrogates | pair → one code point, lone surrogate → U+FFFD | — | stream.py:747‑761, 787‑792 |
| 35 | `finish()` | flush, then `print_word("", True, flush=True)` (terminates the line), state → Other | — | stream.py:523‑553 |

Exact-literal example (tested): `==================== Context usage: 12.5% (1024 tokens) ========================`
is 80 characters; the renderer emits `\x1b[38;5;245m` + that + `\n` + `\x1b[0m`.

## 2. `print_agent_json` dispatch → `stream_renderer` entry points

`print_agent_json` (stream.py:1137‑1178) runs, for **every** wire message:
`_finish_tool_call_stream` (unless ToolCall/ToolCallPart) → `_print_transition_usage`
→ dispatch.

| wire message | reference handler | renderer entry point | notes |
|---|---|---|---|
| `TextPart` | `_handle_text_part` | `on_text_delta` | + `captured_text()` accumulation |
| `ThinkPart` | `_handle_think_part` | `on_reasoning_delta` | `[Think] ` banner once per run; suppressed by `quiet()`/`show_thinking=false` |
| `ToolCall` | `_handle_tool_call` (ToolCall) | `on_tool_call_begin` | `⚡ name` header, then `call.arguments` fed to the lexer |
| `ToolCallPart` | `_handle_tool_call` (ToolCallPart) | `on_tool_call_args_delta` | feeds one raw JSON fragment |
| `ToolResult` | `_handle_tool_result` | `on_display_blocks` (display half) + `on_tool_result` (✓/✗ half) | `output_summary` is rendered as a Brief block through the display path |
| `StepBegin` | `_handle_noop` | `on_step_begin` | nothing |
| `StepInterrupted` | `_handle_noop` | `finish_turn` | nothing; the native CLI has no interrupted signal |
| `CompactionBegin` | `_handle_compaction_begin` | `on_compaction_begin` | `Compacting...` |
| `CompactionEnd` | `_handle_noop` | `on_compaction_end` | nothing, regardless of `ok` |
| `ApprovalRequest` | `_handle_approval_request` | — | auto-approve only; the native toolset has no approval gate |
| anything else | `_handle_other` | — (no output) | state → Other |

`_print_transition_usage` is driven by `_message_transition_type`
(TextPart→Text, ThinkPart→Thinking, ToolCall/Part/Result→ToolCalling, else None).
The renderer mirrors it in `transition()`: a banner is emitted only when a
previous type was recorded and differs, and the type is then stored — so the
first message of a session never produces a banner, and the value persists across
`finish_turn()` exactly like the reference's session attribute.  `show_usage=false`
suppresses the banner while keeping the type tracking.

## 3. Deliberately not ported

| reference item | reason |
|---|---|
| `format_tool_args()` (stream.py:387‑415) | dead for the CLI: the live printer has its own `_emit_compact`; nothing else calls it. |
| `_resolve_display_tool_name` + `_TOOL_NAME_REDIRECTS_NORM` + `normalize_tool_name` + `resolve_tool_name` fuzzy auto-correct (stream.py:294‑337, 917) | needs the ~200-entry `kosong.tooling.TOOL_NAME_REDIRECTS` table plus `_sequence_ratio` fuzzy matching; the header shows the name the caller supplies (already canonical in the native toolset). |
| Parallel-call bookkeeping: `_TOOL_CALL_MERGE_TARGET_KEY`, `_LAST_TOOL_CALL_KEY`, `_TOOL_CALL_PART_PENDING/EMITTED_LEN` coalescing (stream.py:893‑984) | those exist to keep `output_function` snapshots cheap; the native renderer has one printer per call and no structured callback. |
| `output_function` / `MessageType` callbacks (every handler) | the native CLI has no structured sink — the terminal is the only consumer. |
| `format_output=True` text buffering + `_flush_agent_json_text` / `render_markdown` (stream.py:1090‑1094, 1120‑1134) | the native CLI prints deltas live and reads `captured_text()`; markdown rendering belongs to the S5 REPL layer. |
| `ApprovalRequest.resolve("approve")` (stream.py:1052‑1053) | no approval flow in the native toolset. |
| `_reasoning_debug_log` / `KIMIX_DEBUG_REASONING` diagnostics (stream.py:83‑104) | stderr-only debug aid, no user-visible output. |
| `_broken` raw-fragment fallback (stream.py:509‑520, 563‑568) | unreachable: the C++ lexer has no exception path, so the flag stays false (kept for parity of the completion gate). |
| process-wide `_stream` singleton + `session._tmp_data` storage | the renderer owns the equivalent state per instance (header note); `set_output()` replaces the singleton's `_print_func`. |
| `session.status` (stream.py:120‑123, 1187‑1189) | replaced by `on_context_usage(ratio, tokens)`, which only records the snapshot. |
| `session._tmp_data.pop(tool_call_id)` result/name correlation (stream.py:997‑1016) | the frozen `on_tool_result` signature carries the name explicitly. |
| `base DisplayBlock.model_dump()` (stream.py:179) | no pydantic in C++: `display_block` carries the pre-rendered `str(...)` text (kind `base`, colour 250). |

Two implementation reductions (visible output identical for complete
arguments, both documented in the code):

* **Incremental lexer**: stream.py:585‑632 bulk-consumes "boring" spans with
  `str.find`/regex fast paths.  Only the per-character state machine is ported —
  the fast paths are pure performance and append the same bytes to the same
  buffers.  Every lexer state, table, escape rule, surrogate rule and flush rule
  (256-byte cadence) is ported verbatim, including `_emit_compact`'s 60-character
  truncation and `_end_bare_value`'s `true/false/null → True/False/None`.
* **Completion gate**: `_check_complete` re-parses the joined fragments with
  `orjson.loads`; the port uses yyjson + the project's mimalloc allocator
  (`kimix::llm::kYYJsonAlcMi`) plus `clist_has_surrogate_escape()`.  That extra
  check is required for byte-identical behaviour: orjson **rejects any document
  containing a `\uD800-\uDFFF` escape** ("surrogates not allowed") whereas yyjson
  combines a surrogate pair, and whether the document validates decides whether
  `finish()` terminates the streamed line (an observed, visible difference before
  the check was added: an extra trailing `\n`).
  In the unreachable `_broken` branch the gate inspects the accumulated document's
  tail instead of the last fragment's (stream.py:566).

Other notes (no behavioural impact):

* Truncation (`text[:60] + "..."`) and the 256-byte flush cadence count
  **characters** in Python and are implemented character-aware (UTF-8) / byte-based
  respectively; the flush flag never changes the emitted bytes (it only decides
  `fflush`).
* `context_usage_banner()` returns the 80-character line **without** the trailing
  `\n` (the renderer appends it, stream.py:128) so the frozen `// 80-char rule`
  comment is directly testable.  The `max(80 - len(left), 1)` clamp is
  unreachable with a ratio 0–1 and an `int64_t` token count (the widest label is
  72 characters); it is exercised with the ratio `1e30` (85 characters).
* `line state`: the renderer's `_last_char_was_newline` equivalent starts `true`
  (like `PrintStream.__init__`) and is computed from the pre-colour text, so a
  fresh renderer never emits a leading newline and `--no_color` output is plain
  text.

## 4. `cli_print` workaround (S1 bug found, file not modified)

`cli_print.cpp:205‑213` wraps text with `clip_wrap(text, ansi_prefix(...))`, and
`clip_wrap` (line 75‑87) re-adds `"\x1b[" … "m"` around the codes it is given — so
`colorful_text("\xe2\x9c\x93 bash", 92)` produces `"\x1b[\x1b[92mm✓ bash\x1b[0m"`
instead of `"\x1b[92m✓ bash\x1b[0m"`.  This is a latent S1 bug that equally
affects `print_info`, `print_success`, `print_warning`, `print_error`,
`print_debug` and `gray_text`/`gray_light_text` (no other caller exists yet).

Per the step's hard rule (`src/cli` S1–S3 files are frozen) `cli_print.cpp` was
**not** touched.  `cli_stream.cpp` therefore wraps through
`clist_colorful`/`clist_colorful_256`/`clist_colorful_styled`, which build the
identical bytes from the same cli_print building blocks — `ansi_prefix`,
`ansi_prefix_256` and the `colorful()` gate — so `--no_color` and the console
detection keep working exactly as specified.  Once `clip_wrap` is fixed (pass the
codes rather than the complete prefix, or stop re-wrapping), those three helpers
collapse into `colorful_text`/`colorful_text_256` + `gray_text`.

## 5. Native additions (frozen signatures untouched)

* `set_output(std::FILE *)` / `output()` — default `stdout`; anything else is
  written with `fwrite`/`fflush`, so tests render into `tmpfile()` and compare
  exact bytes.
* `on_display_blocks(blocks)` — the full `_format_display_blocks` path; needed
  because the frozen `on_tool_result` carries strings, not wire blocks.
* `on_error(message)` — no reference counterpart; modelled on `print_error`
  (bright red + bold).
* `reset_capture()` — clears `captured_text()` at a turn boundary.
* Display-block types (`display_block_kind`, `todo_display_item`,
  `display_block`, `display_block_parts`, `format_display_blocks`).
* The class is copyable: the argument printer holds a back-pointer to its
  renderer, so the copy constructor/assignment re-point it (a copy keeps writing
  to its own stream — asserted by `stream_renderer_copy_routes_to_own_stream`).

## 6. Verification

```
python scripts/build_locked.py --timeout 900 -- xmake build kimix_cli   -> build ok
python scripts/build_locked.py --timeout 900 -- xmake build test_cli    -> build ok
./bin/debug/test_cli.exe   -> Suite 'global': all tests passed (533 asserts in 36 tests)
                              (S3 baseline: 295 asserts in 17 tests)
```

Reference diff: the scratch harness `.kimix_cache/tmp_21512/ref_capture.py`
(gitignored, not a deliverable) drove the **real** `kimix.ui.stream` module
(`print_agent_json` over 22 wire-message sequences, `S._stream` rebound to a byte
collector) and captured the exact terminal bytes; a temporary test rendered the
same sequences through `stream_renderer` and compared them file-by-file — all 22
cases matched byte-for-byte (text run, reasoning run, tool call with compact args,
streamed `content`, inline `command`, ok/trivial/brief/fallback tool results, the
transition banner at 12.5 %/50.0 %/100.0 %, quiet mode, colour-off text/tool/result/
reasoning, tool-call supersede, 60-char truncation, surrogate + unknown escapes,
compaction, and every display-block kind).  The temporary test was then removed;
the same literals stay asserted in the permanent suite.

Regression proof (each probe applied, built, observed, reverted):

| probe | observed |
|---|---|
| `kBannerEqualCount 20 -> 19` | 11 asserts failed: the banner literal for all three ratios, the three `size() == 80` checks, the huge-ratio literal and the quiet-mixed banner (`tests\unit\cli\test_cli.cpp:1273/1305/1308/1314-1315` and the renderer cases) |
| `kCheck U+2713 -> "X"` | 3 asserts failed: `ok tool result`, `brief + ok`, `plain tool result` (`actual=[ X bash ] expected=[ ✓ bash ]`) |

After reverting both probes the suite is green again (533/533).
