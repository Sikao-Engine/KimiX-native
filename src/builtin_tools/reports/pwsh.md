# pwsh — implementation report (self-kill guard native kernel)

Branch `agent/pwsh`, worktree `C:/dev/kimix_wt/pwsh`.
Plan: `C:/dev/kimi-agent/plans/pwsh.md` §3.2 / §7 / §8.
Python source of truth: `C:/dev/kimi-agent/src/kimix/tools/file/bash/safety.py`
(self-kill guard lines 272–806, variants 48–70).

## What was ported

One header/source pair, namespace `kimix::builtin_tools::pwsh`:

- `src/builtin_tools/pwsh_tool.h` — public API + contract
- `src/builtin_tools/pwsh_tool.cpp` — implementation (all helpers file-static in
  the tool namespace; no `std::regex`, no Python includes)
- `tests/unit/builtin_tools/test_pwsh_tool.cpp` — Boost.UT suite
- `src/builtin_tools/reports/pwsh.md` — this report

Public API:

- `command_detection_variants(command, out)` — deobfuscation variants
- `detect_self_kill(command, protected_pids, image_names, cmdline, status)` —
  returns `kimix::optional<kimix::string>` hit description; `status` reports
  `tool_status::unsupported` for non-ASCII input / regex-metachar pkill
  patterns (caller routes to the Python mirror).
- `detect_self_kill_ex(...)` — same scan returning `self_kill_result`
  (description + matched rule id) so the shim can build the hint message.
- `self_kill_hint(command, protected_pids, image_names, cmdline, agent_pid,
  status)` — pure formatter composing the hint message; OS introspection
  (`_agent_pids` / `_agent_image_names` / `_agent_cmdline`) stays Python.
- `k_self_kill_guidance` — `safety.py` `_SELF_KILL_GUIDANCE` (773–780).

## Function-by-function mapping (safety.py → pwsh_tool.cpp)

| safety.py | pwsh_tool.cpp | Notes |
|---|---|---|
| `command_detection_variants` (48–70) | `command_detection_variants` | collapse / deobfuscate / lowercase, dedupe, max 3; empty → `[]` (matches 61–62) |
| `_segment_text` (438–440) | `segment_text` + `segment_split_pos` | byte scanner: stop at `;`, `|`, `\n`, and `&&`; a single `&` does NOT split (matches `re.split(r";\|\|\|&&\|\||\n", …)`) |
| `_segment_tokens` (73–81) | `segment_tokens` | whitespace token split of one segment |
| `_looks_like_flag` (84–91) | `looks_like_flag` | `-x`/`--long` and `/s`-style; bare `/` and `-` are not flags |
| `_numeric_pid_targets` (443–460) | `numeric_pid_targets` | flags excluded; comma split; strip `"'()`; plain digits; `^(\d+)[).]` expression style; >18-digit runs skipped (can never equal an int64 protected PID) |
| `_LOOP_PID_HEADERS` (469–474) + `_loop_pid_sources` (482–505) | `build_loop_pid_sources` + `loop_source_add_pids` | bash `for v in …;` and PowerShell `foreach ($v in …) {`; `$ * ? [ ` ~` → unresolvable; literal digits only, deduped |
| `_variable_pid_hit` (508–538) | `variable_pid_hit` | `\$\{?var\}?` full-token match (optionally quoted), loop-source lookup |
| `_split_image_name` (388–398) | `split_image_name` | basename after `/`/`\`, stem drops `exe|com|bat|cmd|py|sh` |
| `_name_kill_hit` (541–561) | `name_kill_hit` | exact/stem match + trailing-`*` prefix (≥3 chars); tokens without alphanumerics ignored |
| `_pattern_kill_hit` (564–584) | `pattern_kill_hit` | **native subset**: plain case-insensitive substring only; metachar patterns route to Python (see Deviations) |
| `_pkill_full_match` (587–595) | `pkill_full_match` | `--full` or short flag containing `f` |
| `detect_self_kill` (598–770) | `detect_self_kill_ex` | five ordered detectors, first-hit-wins, byte-exact f-string descriptions |

Detector order (first hit wins, exactly safety.py:644–770):

1. `\b(?:kill|tskill)(?:\.exe)?\b` — skip when the preceding word is
   `docker|podman|kubectl|compose`; numeric PID targets + loop-var targets.
2. `\btaskkill(?:\.exe)?\b` — PID + loop-var; `/im <name>` image-name hit.
3. `\bstop-process\b` — PID + loop-var + non-flag name hit; when the text
   contains `stop-process` / `| kill` / `.kill()`, `\bget-process\b` PID/name
   hits ("piped to a kill").
4. `\b(?:pkill|killall)(?:\.exe)?\b` — pkill plain-substring over sorted image
   names (+ cmdline when `-f`/`--full`); killall exact name hit.
5. `\bwmic(?:\.exe)?\b` — segment must contain `delete`/`terminate`;
   `processid=<pid>` or `processid=$var` via loop sources.

`\b` is ASCII-only `[A-Za-z0-9_]` (matches the existing shell_safety
convention); the command is collapsed + lowercased once up front.

## What stays in Python (quoted from plan §3.2)

> **Stays in Python (justified)**:
> - `_agent_pids()` / `_agent_image_names()` / `_agent_cmdline()` — OS
>   introspection (Toolhelp32 snapshot via ctypes on Windows, /proc on POSIX),
>   computed **once** per process and cached; per-call cost is negligible and
>   platform code stays in Python.
> - `self_kill_hint` message composition (agent PID interpolation + guidance
>   text) — string formatting in the shim.
> - `command_detection_variants` is already native; the shim loops the
>   variants (max 3) and calls the kernel per variant exactly like
>   `check_hardline_blocked` does today.

The plan's §8 row on pkill also justifies the regex gate:

> **pkill patterns are full regexes** in Python (`re.search(p, haystack,
> re.IGNORECASE)` with substring fallback) | Native handles only
> plain-substring patterns; any pattern containing `[ ] ( ) { }  + ? | ^ $ \ .`
> routes the whole call to Python. Documented deviation, never a false block.

## Deviations

1. **`name_kill_hit` tie-break is deterministic (sorted) instead of Python's
   set-iteration order.** Python iterates `image_names` as a `set`, so when
   several names match a token (e.g. `python.exe` with both `python` and
   `python.exe` in the set), the reported name depends on the per-process hash
   seed: the same `taskkill /IM python.exe /F` vector produced `` `python` ``
   in one Python process and `` `python.exe` `` in another. The kernel sorts
   the lowercased names ascending and iterates in that order, so the result is
   reproducible. The unit tests assert the sorted-order result (`python`),
   which also matches the captured `.goldens.json` vector. The C++ code
   documents this at the `name_kill_hit` definition.
2. **pkill regex-metacharacter gate.** Any pkill pattern token containing a
   regex metacharacter (`[ ] ( ) { } + ? | ^ $ \ .`) returns
   `tool_status::unsupported` so the shim falls back to the Python mirror
   (per plan §8; never a false block, never `std::regex`). Non-ASCII input
   (command, cmdline, or image name) also routes to Python.
3. **Oversized numeric targets.** Python `int()` is unbounded; an ASCII digit
   run longer than 18 digits can never equal an `int64_t` protected PID, so
   those tokens are skipped (identical outcome: no hit).
4. **Test expectation corrections** (recorded per AGENT_TASK.md "fix the test
   and record it"): the loop-variable descriptions in `_variable_pid_hit` /
   wmic are `via ``kill``` / `via ``Stop-Process``` — **double** backticks —
   because Python embeds the already-backticked `via` string inside its own
   backticks. The first-draft tests asserted single backticks; both the kernel
   and Python produce double backticks, so the tests were corrected. One
   `first_hit_wins_ordering` assertion checked for `` `taskkill` `` but the
   `/IM` description contains `` `taskkill /IM` `` (Python f-string); the
   assertion was corrected to match Python.

## Verification

- `xmake f -m debug -y -c`, `xmake build kimix-llm`, `xmake build
  test_builtin_pwsh`, `./bin/debug/test_builtin_pwsh.exe` — all green.
- **Test counts:** 20 Boost.UT tests, 244 asserts, all passing
  (`Suite 'global': all tests passed (244 asserts in 20 tests)`).
- **Differential check vs Python reference:** a temporary harness ran 140
  commands through both the C++ kernel and `safety.detect_self_kill`
  (protected PIDs {4100,5000}, names {python.exe,python,kimi}, a fixed
  cmdline): 127 byte-exact matches; 8 differences are the deterministic
  `name_kill_hit` tie-break above; 13 cases correctly route to Python
  (12 regex-metachar pkill patterns + 1 non-ASCII command). The temporary
  harness and its build-file registration were removed after the check.

## Files created

- `src/builtin_tools/pwsh_tool.h`
- `src/builtin_tools/pwsh_tool.cpp`
- `tests/unit/builtin_tools/test_pwsh_tool.cpp`
- `src/builtin_tools/reports/pwsh.md`

No `issue/pwsh.md` — no missing vendored library; OS introspection stays in
Python by plan, so no blocker report is needed.
