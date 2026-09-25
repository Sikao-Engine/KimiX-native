# cli_commands.cpp audit against the Python reference (commands.py)

Audit of every slash command handler in `src/cli/cli_commands.cpp` against
`D:\kimi-agent\src\kimix\cli_impl\commands.py` (plus the helpers the handlers
delegate to: `kimix/utils/session.py`, `kimix/utils/fix_error.py`,
`kimix/base.py`, `kimix/utils/prompt.py`, `kimi_agent_sdk/_session.py`).
Prior coverage notes: `src/cli/reports/cli_commands.md`. Line numbers are
1-based and refer to the files as of this audit (cli_commands.cpp includes
the two fixes made during the audit, noted below).

Legend: **parity** = behavior/messages match the reference (allowing for
infrastructure documented in cli_commands.md); **documented-deviation** = the
report records the difference and its reason; **gap** = accidental drift.

## Per-command table

| command | status | notes (Python → C++) |
|---|---|---|
| /help | parity | `_cmd_help` (commands.py:67-69) prints HELP_STR; C++ `cli_help_text_extended` is byte-identical (cli_commands.cpp:552-556). |
| /clear | **gap (minor, locked by test)** | Reference `clear_default_context` (session.py:452-471) re-prints via `_print_usage` (session.py:402-414) → **`Finished, context usage: P% (T tokens)`** on both the <1e-8 early path and after clearing. C++ prints `Context usage: ...` (cli_commands.cpp:566,586). Undocumented wording drift; fixing it breaks test_cli.cpp:2538 (`has_substr(out, "Context usage: 0.0% (0 tokens)")`). → needs parent decision. Core behavior (clear_context + rebind, id/dir kept) is parity. |
| /compact | parity | 1e-8 guard, `Start compacting...`, `Context usage from A to B  time: H:MM:SS` from app_compact (commands.py:77-79 + session.py:427-449 → cli_commands.cpp:590-602). Test cli:2425 locks the no-op-without-usage behavior. |
| /context | parity | `print_usage` (session.py:418-425) → `Context usage: P% (T tokens)` bright green bold = print_success (cli_commands.cpp:604-608). |
| /exit | parity | save + close(delete anonymous) + `bye!` + should_break (commands.py:407-423 → cli_commands.cpp:610-625). The reference's `cleanup_temp_folder()` (tmp_<pid> removal) is a process-teardown concern handled at the cli_main level, not in the handler. |
| /file:\<path\> | documented-deviation | Returns the file text as `next_input` (reference discards it — cli_commands.md §5.3). Error strings match: `command format error, must be /file:path`, `file not found: <raw payload, unresolved>` (commands.py:491-500 → cli_commands.cpp:693-715). |
| /txt | parity | Banner, /end//cancel loop, `_split_text` blocks queued (commands.py:482-488 → cli_commands.cpp:677-691). Both sides ignore the cancel flag in effect: reference queues the (empty) split result; C++ returns without queueing — same observable behavior. |
| /export[:path] | documented-deviation (+1 minor gap) | Guards and error strings parity: `No active session to export.`, `Command must be /export:file`, `Export failed: {e}` with `No messages to export.` from the store (commands.py:81-97 → cli_commands.cpp:717-736). Minor gap: the success message echoes the **given** path; the reference echoes the **resolved** output path incl. the directory-form default-name resolution (_session.py:787-830). Resolution lives in `session_store::export_markdown` (cli_session.cpp, out of scope). → needs parent decision. |
| /resume:\<id\> | parity | `Session <id> not found.` debug print exists in the reference too (session.py:155); `Resumed session {id}` / `Failed to resume session: {e}` (commands.py:100-128 → cli_commands.cpp:738-755). |
| /store:\<id\> | parity | Guard order (arg → no session → same name) and strings match: `No active session to store.`, `Target session name must be different from current session name.`, `Session stored as {id}` (commands.py:149-231 → cli_commands.cpp:757-779). Reference re-opens the source after copy; C++ store_as keeps the current session (cli_commands.md §5.2). On store failure the reference attempts a recovery resume; C++ leaves the untouched store open — equivalent outcome, not ported. |
| /load:\<id\> | parity | Confirmation wording, y/n loop, `Please enter y or n.`, `Load cancelled.`, EOF→cancel (documented §5.7), `Loaded session {source} into anonymous session {new}`, `Load failed: {e}`, `Loaded session but failed to resume copy: {e}` (commands.py:234-313 → cli_commands.cpp:781-847). The reference's trigger is `soul.context.token_count > 0`; C++ uses the usage token count — same signal. |
| /sessions[:\<name\>] | documented-deviation (time zone) | Named form and list form strings/layout match: `Created and switched to session: {name}`, `Failed to create session "{name}": {e}` + anonymous recovery, `No sessions found.`, header/`*`/usage(22)/`Untitled` (commands.py:316-404 → cli_commands.cpp:849-910). Source is the filesystem scan, not the in-memory cache (§5.2, documented). Drift: `updated_at` renders **UTC** (format_utc) while the reference uses **local time** (pendulum.from_timestamp, commands.py:398). The report records UTC but gives no reason; flag for parent. |
| /cmd:\<cmd\> | parity | `Command must be /cmd:xx yy`, `Done.`/`Failed.` (commands.py:432-445 → cli_commands.cpp:627-649). Documented §5.5: captured output echoed via print_raw instead of an inherited console; 600 s timeout. Spawn-failure prints the runner's error (reference prints `str(e)`) — equivalent path. |
| /fix:\<cmd\> | parity | Verified against fix_error.py + base.py: `Shell: <cmd>` per attempt (base.py:130), slice from first line containing `error` (lowercased), whole output when none, `Fix error from command \`cmd\`:\n\n<text>\n`, `No error.` on first success, max 4 attempts (commands.py:449-458 → cli_commands.cpp:651-675). Arg guards match (empty-after-strip → `Command must be /fix:<command>`). |
| /plan[:\<path\>] | reduced (documented) | Banner, default `.kimix_cache/plan_<16hex>.md` (work_dir ≡ CLI cwd), `No requirement provided.` — including the /cancel path, which the reference also funnels into the empty-requirement warning (commands.py:461-479 ignores the cancel flag; C++ likewise, cli_commands.cpp:1005-1043). The reference's `Generating plan (attempt N/3)...` bright-cyan banner (prompt.py:921) appears once via print_debug. Planner sub-session/retry/review loop not ported — documented. |
| /swarm | reduced (documented) | /cancel → silent return matches the reference (commands.py:545-547 → cli_commands.cpp:1045-1068). `No input provided for swarm.`, `Creating swarm session...`, isolated session with swarm_enabled, deleted afterwards. Minor: reference distinguishes `Failed to create swarm session: {e}` from `Swarm prompt failed: {e}`; C++ has a single `Swarm prompt failed: ` path. |
| /supervisor | **gap — FIXED** | Reference ignores the cancel flag (`text, _ = _read_multi_line`, commands.py:517), so /cancel → empty task → **`No input provided for supervisor.`**. C++ early-returned silently on cancel. Fixed: clicmd_supervisor now falls through to the empty-task warning (cli_commands.cpp:1070-1082). Reference distinguishes create-failure vs prompt-failure messages; C++ has one path (`Supervisor prompt failed: `) — same note as /swarm. Boss-manifest fallback is the documented reduction. |
| /todo:\<path\> | parity (+1 minor gap) | Suffix families, `(?<![A-Za-z0-9])TODO(?![A-Za-z0-9])` on uppercased content, single/multi prompt text byte-identical, `file not found:`, `Unsupported file type: {suffix}`, `No TODO comments found.`, `Parse failed: {e}` (commands.py:574-654 → cli_commands.cpp:942-1003). The Unicode-aware C regex-literal heuristic is not reproduced — documented §5.6. Minor gap: failure message is `Prompt failed.` vs reference `Prompt failed: {e}` — needs an error out-param on `app_run_prompt` (cli_app.* is parent-owned). → needs parent decision. |
| /init | reduced (documented) | The wizard is not ported; template + flags explanation + fresh anonymous session + `Initialized.` (commands.py:503-509 → cli_commands.cpp:912-940). Documented reason: input()/getpass + orjson wizard. |
| /reflection | reduced (documented) | Pre-checks `No active session. Start a conversation first.` and `Context is empty. /reflection requires a non-empty context.` (commands.py:903-918 → cli_commands.cpp:1111-1118). The native prompt rebuild is documented. Drift: the reference's non-empty check is 3-tier (usage > 1e-8, token count, `Session.is_empty()` wire-file check, commands.py:733-752); C++ checks `tokens == 0` only. Also `Reflection failed.` vs `Reflection failed: {e}` (same out-param gap as /todo). |
| /code:\<path\> [args…] | reduced (documented); **1 gap FIXED** | Documented: payload whitespace-split, relative-path resolution, `.py` prints `Executing <name>` and spawns `python <script> args…` (no embedded interpreter), `Script file is required.`, `Script file not found:`, `Executable not found:`, `Done (exit code 0).`/`Exited with code N.` (commands.py:657-730 → cli_commands.cpp:1171-1231). **Fixed:** the `Running: ` line printed the raw argument; the reference prints `' '.join(cmd)` with the **resolved** script path (commands.py:717-718). Remaining documented drift: `.py` failure prints `Exited with code N.` (reference prints the exec error + traceback), and any spawn failure maps to `Executable not found:` (reference only FileNotFoundError). |
| unknown | parity | `Unrecognized command.` bright yellow (commands.py:921-923 → cli_commands.cpp:1233-1236); the key/payload split rules are locked by tests (cli:2195-2225). |

## Fixes applied in src/cli/cli_commands.cpp

1. **clicmd_supervisor** (cli_commands.cpp:1070-1082): removed the silent
   early-return on `/cancel`; the handler now ignores the cancel flag like the
   reference (`text, _ = _read_multi_line(...)`), so `/cancel` prints
   `No input provided for supervisor.` exactly as commands.py:517-521 does.
2. **clicmd_code** (cli_commands.cpp:1204-1213): the `Running: ` line now
   prints the resolved script path (the argv join) instead of the raw
   argument, matching `f"Running: {' '.join(cmd)}"` (commands.py:718).

Verification: `python scripts/check_cpp_syntax.py --project-root D:\KimiX-native
src/cli/cli_commands.cpp` → **0 errors** (1 pre-existing unused-include
warning for cli_stream.h, unrelated to this audit; left untouched to keep the
diff minimal). No tests were modified (hard constraint); neither fix is
covered by an existing assertion (verified by grep: no test exercises the
supervisor/code handler output), and no existing assertion matches the old
wording of either line.

## Needs parent decision

1. **/clear usage-line wording**: reference prints `Finished, context usage: …`
   (via `_print_usage`, session.py:402-414); C++ prints `Context usage: …`.
   Changing it breaks test_cli.cpp:2538, which asserts the current wording —
   test owner must decide.
2. **Error suffixes on prompt failures**: reference `Prompt failed: {e}`
   (/todo), `Reflection failed: {e}` (/reflection); C++ prints the bare
   sentence. Appending `{e}` needs an error out-parameter on
   `app_run_prompt` (cli_app.h/cpp is parent-owned).
3. **/export success echo**: C++ echoes the given path; the reference echoes
   the resolved path incl. the directory-form default file name
   (_session.py:803-830). The resolution lives in
   `session_store::export_markdown` (cli_session.cpp, out of scope here).
4. **/sessions time zone**: C++ renders `updated_at` in UTC
   (`format_utc`); the reference renders local time
   (pendulum.from_timestamp, commands.py:398). Needs either a local-time
   formatter or an explicit decision to keep UTC.
5. **/reflection non-empty check**: C++ tests `tokens == 0` only; the
   reference is 3-tier (usage, token count, wire-file emptiness,
   commands.py:733-752). Edge case: a session with persisted wire content but
   0 estimated tokens.
6. **/swarm, /supervisor error taxonomy**: the reference separates
   `Failed to create … session: {e}` from `… prompt failed: {e}`; the C++
   isolated-run helper exposes a single error string. Cosmetic.

## Sampling note

/todo's comment scanner was verified by code inspection against the reference's
parser families and the documented §5.6 reduction, not by execution (no xmake
builds were run, per constraints). The /fix process/filter logic was traced
through `kimix/base.py::_filter_error_output` line by line. Everything else
was diffed handler by handler as listed above.
