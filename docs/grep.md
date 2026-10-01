# Grep Tool — Goal, Requirements, and Desired Behavior

This document defines what the `grep` tool is for, what it must satisfy, and how
it must behave. It is derived from the two implementations of the tool:

| Role | File | Notes |
|---|---|---|
| **Reference implementation** (source of truth) | `C:/dev/kimi-agent/kimi-cli/src/kimi_cli/tools/file/grep_local.py` (+ `grep_selectors.py`, `grep_archive.py`, `grep_output.py`, `grep_recorder.py`, `output_utils.py`, `micro_compress.py`, `utils/sensitive.py`) | The full agent-facing tool: ripgrep (`rg`) subprocess pipeline, pure-Python backup scanner, rich path selectors, archive members, rtk output-dedup wrapper, session recorder |
| **C++ port** | `src/builtin_tools/grep_tool.{h,cpp}` + `src/builtin_tools/grep_engine.{h,cpp}` | Two things: (a) byte-exact CPU-only *string kernels* mirroring the Python post-processing, and (b) a self-contained pure-C++ search engine used only by the native agent session (`Session::native_io`) that cannot shell out to `rg` |

Companion documents in this repo: `src/builtin_tools/reports/grep.md`
(function-by-function parity mapping), `issue/grep.md` (the blocked PCRE2
regex-kernel decision), `docs/ffi.md` (unrelated FFI surface, same docs dir).

---

## 1. Goal

`grep` is the agent's **content search** tool: search file contents with a
ripgrep-syntax regular expression over a workspace, and return results in a form
the LLM can act on — matching lines with line numbers, grouped by file — without
flooding the context window.

Formally, the reference tool description promises:

> Search file contents with a ripgrep regular expression. Returns matching lines
> with line numbers, grouped by file. Returns the first 250 matches inline; a
> capped result reports where the complete match list was saved. Use `read` on a
> matched file for surrounding context. Multiline patterns match across line
> boundaries.

Design goals distilled from the code:

1. **Find, then read.** Grep locates files/lines; `read`/`edit` inspect them.
   The tool therefore *records* the matched-file list into the session so a
   follow-up pass operates on exactly the files grep surfaced (`record`).
2. **Token efficiency.** Every output stage exists to bound what the model
   pays for: per-line truncation, head/tail folding, dedup, byte budget, early
   `--max-count` stops in `rg`, rtk wrapping, micro-compression.
3. **Deterministic, parseable output.** Stable line grammar
   (`path:LN:text` matches / `path-LN-text` context / `--` separators / grouped
   `# path` + `*N|` / ` N|` rendering), ordered streams, first-seen dedup,
   mtime-sorted file lists.
4. **Workspace-scoped and safe.** Searches resolve against the session work
   dir, refuse paths outside the workspace (unless absolute), never leak
   sensitive files (`.env`, keys, credentials), and are bounded in time and
   memory (subprocess buffers, timeouts, kill grace).
5. **Degrade, never hang or die.** Missing/broken `rg` or `rtk` binaries fall
   back to the pure-Python scanner; EAGAIN retries single-threaded; oversized
   or malformed inputs produce actionable messages, not crashes.
6. **Portability with byte-exact parity.** The C++ side ports only the pure
   string kernels and must produce byte-identical results to the Python
   reference — anything it cannot answer exactly (Unicode-regex corners, the
   regex matcher itself) is reported as `unsupported` so the Python shim runs
   its mirror. No silent substitutions.

---

## 2. Requirements

### 2.1 Functional requirements

* **R1 — Regex search.** `pattern` is a ripgrep-syntax regex. The rg path runs
  `rg`; the backup path uses the Python `regex` module (same flags semantics:
  `IGNORECASE`, `DOTALL`). Invalid regex → `ToolError` with the engine's
  message ("Invalid regex pattern: {e}"); empty pattern → error.
* **R2 — Flexible path input.** `path` accepts a single string, a list of
  strings, a JSON-array string (`'["a.py","b.py"]'`), or a semicolon-separated
  string (`"src; tests"`). Entries are stripped, empties dropped,
  duplicates removed **first-seen**; commas never split entries (they belong
  to selectors). Default `"."` (session workspace); relative entries resolve
  against it; `..` and `~` are supported.
* **R3 — Embedded selectors.** `path` entries may carry line-range selectors
  (`file.py:50-100`, `file.py:50+10`, `file.py:301-`, `file.py:5-16,960-973`,
  `file.py:50..100`, `file.py:10:raw`) and archive members
  (`bundle.zip:src/foo.ts`, combined `bundle.zip:src/foo.ts:50-100`). Grammar
  and semantics in §4.4–§4.5.
* **R4 — Output modes.** `output_mode` ∈ `files_with_matches` (default),
  `count_matches`, `content`. Normalized (`strip().lower()`, `-`→`_`) with a
  hard error on anything else.
* **R5 — Context & display flags (content mode).** `-A` / `-B` / `-C`
  (after/before/around context lines), `-n` line numbers (default true, always
  passed explicitly to keep `rg` and `rtk` consistent).
* **R6 — Filters.** `include`: one glob (e.g. `*.ts`, `*.{js,jsx}`) over files;
  `type`: rg file-type / the backup `_TYPE_MAP`; `ignore_case` (`-i`);
  `multiline`; `include_ignored` (search gitignored files too).
* **R7 — Bounded results.** `head_limit` (default 500; 0 = unlimited),
  `offset` (skip first N), `max_output_lines`/`fold` (final display fold,
  default 500 lines; 0 = unlimited but the byte cap still applies),
  `timeout` (default 60 s, min 1), `deduplicate_output`/`token_kill`
  (default true).
* **R8 — Session recording.** `record` (default true) persists the distinct
  matched files (relative paths, stream order) into
  `session.custom_data["grep"]["files"]`, capped at 500 (drop oldest/front),
  with `cwd` and `updated_at`; appends a "Recorded N matched file(s)…" note.
* **R9 — Grouping.** `grouped`: `None` = auto (grouped iff the search used
  line-range selectors or archive members), `True` forces `# path` headers +
  `*N|`/` N|` markers, `False` forces legacy `path:line:text` output.
* **R10 — Multiline.** Patterns containing a real newline or a regex `\n`
  escape (odd number of backslashes before `n`) automatically switch to
  multiline mode (`--multiline --multiline-dotall`, `re.DOTALL` in backup);
  explicit `multiline: true` does the same. In multiline mode `\n` constructs
  are rewritten to `\r?\n` so patterns match CRLF files.
* **R11 — Sensitive-file protection.** Matches in files identified by the
  sensitive table (§4.9) are removed from **all** output modes and replaced by
  a warning that names the skipped files.
* **R12 — Archive member search.** `archive:member` entries extract the member
  (text-only, size-capped) to scratch files, search it through the normal
  pipeline, and remap result paths back to the `archive:member` display form.
* **R13 — Equivalent-command reporting.** Every result/error carries a `brief`
  containing the shlex-joined equivalent `rtk rg …`/`rg …` command line (the
  `rtk` placeholder replaces the absolute binary path).

### 2.2 Robustness / safety requirements

* **R14 — Binary provisioning.** `rg` and `rtk` are Kimi-managed binaries in
  `<share_dir>/bin`; the global PATH copies are deliberately ignored (may be
  broken/incompatible). Missing binaries are downloaded once (versioned
  archive for `rg`, locked to prevent double download); `PATH` gets the shared
  bin dir prepended (deduped) for the subprocess so even bare `rg` resolves
  ours. `rtk` failure is non-fatal: fall back to plain `rg`. `rg` failure falls
  back to the pure-Python backup scanner.
* **R15 — Bounded subprocess I/O.** stdout/stderr streams are capped at
  `RG_MAX_BUFFER` (20 MB) and drained (discarding) past the cap so the child
  never blocks on a full pipe. A truncated buffer drops the possibly-partial
  last line and appends "Output exceeded buffer limit. Some results omitted."
* **R16 — Timeouts everywhere.** The subprocess, the post-processing (CPU-heavy
  compression), and the backup scan are each bounded by `params.timeout`;
  kills are two-phase (SIGTERM → 5 s grace → SIGKILL). Timeout with partial
  output returns the partial results with a notice; timeout with none is an
  error ("Try a more specific path or pattern.").
* **R17 — rg exit-code contract.** 0 = matches, 1 = no matches, ≥2 = error.
  On stderr containing "os error 11"/"Resource temporarily unavailable"
  (EAGAIN), retry **once** with `-j 1`; otherwise error with the raw stderr.
* **R18 — Workspace containment.** Resolved paths outside the workspace (and
  not in `additional_dirs`) fail with "`x` is outside the workspace." unless
  the user *wrote* the path as absolute. Windows reserved device names
  (`CON`, `PRN`, `AUX`, `NUL`, `COM1-9`, `LPT1-9` in any component) are
  rejected before spawning anything.
* **R19 — VFS awareness.** When a virtual file system has dirty (unsaved)
  files, the native rg subprocess would miss them → route the whole call to
  the backup scanner, which translates paths through the VFS.
* **R20 — Never crash the agent.** Any unexpected exception becomes a
  `ToolError` (`"Failed to grep. Error: …"`), logged with pattern/path.

### 2.3 Port/parity requirements (C++)

* **P1 — Byte-exact kernels.** Ported kernels (selectors, content-line
  parsing, grouped rendering, range filter, prefix strip/reattach, rtk
  protocol + fold note, recorder merge, sensitive filter, pattern newline
  kernels, byte-limit join) must reproduce Python outputs byte for byte —
  including error texts, separator/space semantics, and Python regex corner
  cases (`re.DOTALL` leftmost-pair scanning, `$` matching before one trailing
  newline, `\S` vs `str.strip()` whitespace sets).
* **P2 — Honest `unsupported`.** Wherever an exact answer is impossible
  without Unicode tables (e.g. Python `\d` = category Nd hitting non-ASCII
  bytes inside a decisive region), the kernel returns
  `tool_status::unsupported` — never a guess. Whitespace/blank tests use the
  exact 29-code-point set and need no gating; pure byte transforms are never
  gated. Content-line kernels apply the gate *per candidate delimiter*
  (bounded gate) so non-ASCII text bodies — the common case — stay native.
* **P3 — No third-party sprawl.** The native regex line matcher (plan kernel 7)
  needs PCRE2 semantics; `std::regex` and RE2 were rejected for parity
  (see `issue/grep.md`). Until PCRE2 is vendored (adding vendored ext is
  forbidden without approval), `grep_search_lines` always returns
  `unsupported` and the Python matcher stays authoritative.
* **P4 — Native-agent substitute is explicit.** The `native_io` branch
  (grep engine) is *not* a drop-in parity port; its deviations are documented
  (§6.2) and pinned by a test, and the Python tool remains the reference.
* **P5 — Golden-vector verification.** Parity is machine-checked:
  `scripts/gen_grep_goldens.py` runs the Python reference over corpus +
  adversarial vectors into `tests/unit/builtin_tools/grep_goldens.inc`, and
  `tests/unit/builtin_tools/test_grep_tool.cpp` compares the kernels;
  `python/tests/test_parity_grep.py` covers the runtime_py-exposed kernels.

---

## 3. Parameter reference

### 3.1 Reference tool (`grep_local.py` `Params`)

| Param | Type | Default | Aliases | Semantics |
|---|---|---|---|---|
| `pattern` | `str` | — (required) | — | Regex (ripgrep syntax) to search for. |
| `path` | `str \| list[str]` | `"."` | `paths` | File/dir to search; relative resolves against the session workspace. Accepts embedded line-range selectors (`file.py:50-100`, `:50+10`, `:301-`, `:5-16,960-973`), archive members (`bundle.zip:src/foo.ts`, combined `…ts:50-100`), `..` alias, multi-entry strings (`"src; tests"`), JSON-array strings, or lists. |
| `grouped` | `bool \| None` | `None` | — | Group content-mode results with `# path` headers and `*N|`/` N|` markers. `None` = auto: grouped only when a line-range selector or archive member was used; `True` forces grouped; `False` forces legacy `path:line:text`. |
| `record` | `bool` | `True` | — | Persist the deduplicated matched-file list (relative paths) on the session for follow-up `read`/`edit`. |
| `include` | `str \| None` | `None` | `glob`, `filter`, `file_pattern` | **One** glob filter for which files to search (`*.ts`, `*.{js,jsx}`). Not a list; negation not supported. |
| `output_mode` | enum | `"files_with_matches"` | `mode`, `format`… | `files_with_matches` \| `count_matches` \| `content`; normalized via strip/lower/`-`→`_`; unknown value → validation error listing the three valid values. |
| `before_context` | `int \| None` | `None` | `-B` | Lines before each match (content mode only). |
| `after_context` | `int \| None` | `None` | `-A` | Lines after each match (content mode only). |
| `context` | `int \| None` | `None` | `-C` | Lines around each match (content mode only). |
| `line_number` | `bool` | `True` | `-n` | Show line numbers (content mode only). Always translated into an explicit `--line-number`/`--no-line-number` so bare `rg` and `rtk rg` agree. |
| `ignore_case` | `bool` | `False` | `-i` | Case-insensitive search. |
| `type` | `str \| None` | `None` | — | rg `--type` file-type filter (backup scanner maps a small built-in `_TYPE_MAP`: py, js, ts, rs, go, java, cpp, c, md, json, yaml, xml, html, css, sh, sql, lua, vim, docker, make, ruby, php, cs). |
| `head_limit` | `int \| None` | `500` (≥0) | `limit`, `max_results` | Max result lines to return; `0` = unlimited. |
| `offset` | `int` | `0` (≥0) | — | Skip the first N results (pagination). |
| `multiline` | `bool` | `False` | — | Multi-line regex mode. Auto-enabled when the pattern contains a newline or a `\n` escape. |
| `include_ignored` | `bool` | `False` | — | Search files excluded by `.gitignore` too (rg path: `--no-ignore`; backup path: don't skip the ignored-dir list). VCS dirs stay excluded either way. |
| `timeout` | `int` | `60` (≥1) | — | Max seconds for the subprocess **and** for post-processing (and the whole fallback path). |
| `deduplicate_output` | `bool` | `True` | `token_kill` | Dedup repeated lines via the external `rtk` wrapper (or the local run-collapse fallback when rtk is unavailable). `False` = raw, unfiltered output. |
| `max_output_lines` | `int` | `500` (≥0) | `fold` | Final display budget: longer outputs head+tail fold with a marker. `0` = unlimited (byte cap still applies). Applied **after** offset/head_limit pagination. |

Additional tool-level names: `CallableTool2` field aliases
(`FIELD_ALIASES_GENERAL/FILE/WEB`, `paths`→`path`, `glob`/`filter`/
`file_pattern`→`include`, `-B/-A/-C/-n/-i`→long names).

### 3.2 C++ native tool (`KIMIX_REGISTER_TOOL_NAMED_ALIASED` schema)

Registered name `grep` (aliases `Grep rg ripgrep search_files`), description
"Search file contents with a regex (ripgrep-like). Recursively walks
directories, skips hidden and binary files, and returns matching files, counts,
or content lines with context." JSON schema (required: `pattern`):

`pattern` (string), `path` (string, default work dir), `output_mode`
(`files_with_matches|count_matches|content`), `-i` (bool), `-A`/`-B`/`-C`
(int), `include` (filename glob, e.g. `*.cpp`), `head_limit` (int, max content
lines; engine default 250, `<=0` unlimited).

Fuzzy parameter-name aliases are resolved before use (`ToolParams::with_aliases`):
`pattern` ← regex/regexp/search/search_pattern/query; `path` ← dir/directory/
folder/root/search_path; `paths` ← search_paths/target_paths; `output_mode` ←
mode/output_format/format/result_mode; `-i` ← ignore_case/case_insensitive/
insensitive; `-A`/`-B`/`-C` ← long context names; `include` ← include_glob/glob/
file_pattern/include_pattern/include_files; `head_limit` ← limit/max_results/
max_count/head/max_matches. The canonical name always wins.

---

## 4. Desired behavior (reference pipeline)

### 4.1 Dispatch

```
grep(params)
 ├─ VFS dirty?                          → backup_grep (pure Python)
 ├─ rg binary unavailable (or download failed) → backup_grep
 ├─ rtk requested (deduplicate_output) → resolve abs rtk path; on failure, silently plain rg
 ├─ entries = expand_path_entries(path) or ["."]
 ├─ entries_are_rich(entries)?          → rich pipeline (_rich_call):
 │     (selector / archive-member / multi-entry searches)
 └─ else                                → plain rg pipeline (byte-identical legacy path)
```

Both rg pipelines share one post-processing function (`_postprocess`); the
backup scanner implements the same observable behavior without `rg`/`rtk`
(plus: it applies line ranges for **all** output modes, whereas the rg path
requires content mode for ranged selectors).

### 4.2 Building the `rg` argv

Fixed arguments:

* `--no-config` (never honor a user's rg config — speed + determinism).
* `--hidden` (search hidden files) — VCS metadata dirs are still excluded via
  negative globs `--glob !.git !.svn !.hg !.bzr !.jj !.sl`.
* `--max-columns 500` in non-content modes.

Conditional arguments:

| Trigger | Flags |
|---|---|
| `include_ignored` | `--no-ignore` |
| EAGAIN retry pass | `-j 1` |
| `ignore_case` | `--ignore-case` |
| `multiline` or pattern has a regex newline | `--multiline --multiline-dotall`, and the pattern is rewritten (`\r\n`→`\n`; every `\n` escape and literal newline → `\r?\n`) so it matches both LF and CRLF files |
| content mode | `--before-context N` / `--after-context N` / `--context N`; explicit `--line-number` or `--no-line-number` |
| content mode + pagination | `--max-count = offset + head_limit + 1000` (margin keeps sensitive filtering from starving the page); `0`/absent `head_limit` = no cap |
| ranged selector present | widened `--max-count = min(200_000, max(offset+head_limit+1000, max_end))` so in-range hits are not starved by earlier out-of-range matches |
| `include` | `--glob <include>` |
| `type` | `--type <name>` |
| mode | `--files-with-matches` / `--count-matches` (nothing for content) |

Then `--`, pattern, and the search target. The target is the **resolved** path
(work-dir-joined, `expanduser`, `.resolve()`d) when available, so rg's output
prefixes match the base used for stripping (fixes Windows short/long temp-path
mismatches); otherwise the user input normalized. When wrapping, argv becomes
`[<abs rtk>, rg, …]` — rtk dispatches on the wrapped executable's stem. The
subprocess runs with `cwd = work_dir` and `PATH` with `<share>/bin` prepended.

### 4.3 Subprocess handling

* Streams are incrementally read into 20 MB caps, then drained-and-discarded
  (child never blocks). Output decodes UTF-8 with `errors="replace"`.
* `asyncio.wait_for(gather(readers), timeout)`; on timeout: SIGTERM, 5 s,
  SIGKILL; `timed_out` is remembered.
* Buffer truncation → drop the last (partial) line, note
  "Output exceeded buffer limit. Some results omitted."
* Timeout → keep partial results ("Grep timed out after {t}s. Partial results
  returned.") or error when empty ("… Try a more specific path or pattern.").
* Exit codes: 0/1 fine; ≥2 → EAGAIN single retry (`-j 1`) or
  `ToolError("Failed to grep. Error: {stderr}")`.
* Post-processing itself is wrapped in the same timeout ("Grep post-processing
  timed out after {t}s. …") because micro-compression is CPU-heavy and a
  pathological single-line match could otherwise hang the call.

### 4.4 Path-entry expansion and the selector grammar

`expand_path_entries`: list → strip each, drop empties/non-strings; a
`[`-prefixed string → strict JSON array of strings (orjson semantics: control
bytes must be escaped, lone surrogates rejected, trailing content rejected)
else `;`-split; strip, drop empties, dedup first-seen. **Commas never split**
entries.

`split_path_and_sel(raw)` peels at most two `:`-suffixes (rightmost-first),
each validated against the loose shape
`^(?:raw|conflicts|L?\d+(?:(?:\.\.|[-+])L?\d*)?)$` (case-insensitive):

* Guard: an existing filesystem entry with the *exact raw name* wins — no
  peel (a real file `test:1-2` beats the selector reading).
* Guard: `scheme://authority` with no path (`ssh://host:2222`) — the chunk is
  a port, not a selector.
* Guard (Windows): never leave a bare drive (`C:`) behind — `ntpath.splitdrive`
  shapes ("X:" and `\\server\share`) are checked per peel.
* Peeled chunks are rejoined with `:` into the selector string.

`selector_line_ranges(sel)`: `:`-separated chunks; `raw` / `conflicts` are
display-mode suffixes with no search meaning (skipped); the first chunk that
parses as a comma-separated range list wins. Grammar (case-insensitive,
`L?` line prefixes allowed):

| Form | Meaning |
|---|---|
| `N`, `N-`, `N..`, `N+` | open-ended range N…EOF |
| `N-M` / `N..M` | inclusive 1-based range; `M < N` → error "Invalid range N-M: end must be >= start." |
| `N+K` | K lines from N (N…N+K-1); `K < 1` → error "Invalid range N+K: count must be >= 1." |
| `0` / `0-x` | error "Line selector 0 is invalid; lines are 1-indexed. Use :1." |
| `L?` prefixes, `..` alias | all accepted, any case (`l4-L6`, `5..16`) |

Multiple comma chunks (`5-16,960-973`) are parsed, stable-sorted by start,
**merged when overlapping or adjacent**, and an open-ended range absorbs
everything after it.

Rich-routing rules (`_resolve_selector_specs`):

* A selector/archive entry must name a single file — a glob (`*?[`) errors
  ("Line-range selector/archive member requires a single file, not a glob")
  and a directory errors ("Line-range selector requires a single file, not a
  directory").
* Ranged selectors in the rg pipeline require `output_mode='content'`
  (files/count modes have no per-line stream) — enforced with a clear error.
* Workspace validation happens per resolved path (R18).

### 4.5 Archive member search

`parse_archive_path_candidates` splits `archive:member` rightmost-first while
the left side ends with a known archive extension (23-extension table, longest
first so `.tar.gz` beats `.gz`; case-insensitive): `.tar.gz .tar.bz2 .tar.xz
.tar.zst .tgz .tbz2 .tbz .txz .zip .jar .war .ear .apk .whl .xpi .vsix .nupkg
.cbz .tar .gz .bz2 .xz .zst`. Nested archives split further.

Materialization (`materialize_archive_members`): read the innermost member via
`ArchiveReader`; reject binary (NUL) or non-strict-UTF-8 members; per-member cap
8 MB, total scratch cap 32 MB (bomb guard); write to a `kimi-grep-*` temp dir as
`{idx}-{safe_name}` (`safe_name` = basename, `[^\w.-]+` runs → `_`, fallback
`member`). Search the scratch file with the normal pipeline, then **remap**
result paths back to the written `archive:member` display form (forward-slash
prefix comparison; unmatched lines verbatim). Unreadable members never fail the
call — they are skipped with a "Skipped archive entries (text members only):
…" note, unless nothing is readable → error suggesting
`read <archive>:<member>`. The scratch dir is cleaned up in `finally`.

### 4.6 Rich pipeline execution

One rg invocation **per resolved target** (fan-out, sequential), each with the
widened `--max-count` (ranged) and with rtk wrapping **disabled for ranged
searches** (rtk's near-duplicate folding could drop in-range lines before the
range filter sees them). Bare single-file output (`2:text` without a path —
rg omits the prefix when the target is a single file) gets the target's display
key re-attached (`prefix + sep + line`, `^(\d+)([:\-])` shape) so the shared
line-stream pipeline always sees `path:LN:text`. All outputs are concatenated
into one stream and post-processed together; per-target messages (buffer
truncation, timeout) accumulate.

### 4.7 Post-processing pipeline (exact order)

Given the raw line stream and the `_GrepCtx` (prefix base, grouping, ranges
map, display map, notes):

1. **Step 0 — rtk protocol cleanup** (content mode only): strip rtk's header
   `N matches in M files:` (+ its trailing blank line), per-file fold markers
   `+K more in <path> [see remaining: <hint>]`, and the files fold marker
   `+K more files [see remaining: <hint>]` into metadata; real lines keep
   order/content verbatim; unknown lines pass through (tolerant of rtk version
   skew). If rtk truly truncated (folds or skipped files), the *original*
   stream is exported to a temp `.txt` so the model can page through it
   ("Original output: <path>").
2. **Step 1 — mtime sort** (`files_with_matches` only, skipped on timeout):
   drop blank lines; sort newest-first; when pagination applies, take only the
   top `offset + head_limit` by mtime (heapq) and remember early truncation.
3. **Step 2 — relative paths**: strip the search base prefix
   (`prefix_base`; for a single-file plain search, its parent). Compare
   forward-slash-normalized, but slice the **original** bytes (so inner
   backslashes survive: `a\b\c.py` → `b\c.py`). On Windows rich searches, also
   normalize content-line path separators to `/` and remap archive scratch
   paths to `archive:member` (before sensitive/range filtering so keys match).
4. **Step 3 — sensitive filter**: extract the file path per mode (content:
   leftmost `^(.*?)([:\-])(\d+)\2` shape — context lines use `-`, matches use
   `:`; count: text before the last `:`; fwm: whole line). Sensitive paths are
   dropped (their `--`-terminated trailing separators are pruned) and replaced
   by the warning of §4.9.
5. **Range post-filter** (rich + content): drop content lines whose path has a
   ranges map and whose line number is outside it; prune leading/doubled/
   trailing orphan `--`.
6. **Recorder** (§4.10) — note appended **early** (before summaries) so
   message-tail parsers still find "Original output:" last.
7. **Step 4 — summaries** (on full results, pre-pagination):
   * count mode: `Found {m} total occurrences across {f} files.`
   * rtk header: `Found {m} matches in {f} files.` + fold note
     `rtk folded output: {k} more lines in {path}; … / {n} more files. Full
     log: tail -n +{start} {log}. Original output: {path}` (forward slashes).
   * files mode: `Found {n} files matching {pattern!r}.`
   * archive skip note (§4.5).
8. **Step 5 — local dedup fallback**: content mode, `deduplicate_output`,
   **rtk did not run**, and not grouped → collapse runs of ≥3 identical
   consecutive lines into `line  (N repeats)` (rtk-compatible style) with
   `Removed {n} repeated line(s) via dedup.` Never double-collapse.
9. **Step 6 — pagination**: `lines[offset:]`, then `head_limit`; on cut:
   `Results truncated to {limit} lines (total: {total}). Use offset={offset+
   limit} to see more.` (also reported when mtime-sort truncated early).
10. **Grouped rendering** (after pagination, content mode, when grouped):
    parse each line; group by file in encounter order under `# <path>` headers
    separated by a blank line (none before the first); body lines render
    `*N|text` (match) / ` N|text` (context), unpadded numbers; `--` and gap
    markers pass through inside their group; leading non-content lines drop.
11. **Step 7 — display fold**: `max_output_lines` head+tail fold
    (`head = max(1, N//2)`, `tail = N-head`) with marker
    `… (K lines omitted) …` and message `Results folded to {len-1} lines
    ({K} omitted). Use max_output_lines=0 or offset to see more.`
12. **Step 7.5 — hygiene + micro-compress**: per-line truncate to 500 chars
    with `… [+K chars]` marker (cut bare if the marker can't fit) — before
    compression so one gigantic line can't blow up the O(n²) prefix-folding;
    micro-compress lines (kind `log`; lossless stages plus the annotated
    path-prefix fold — prefix fold disabled for grouped output because `#`
    headers would distort; near-duplicate collapse **disabled** — every
    distinct match must stay visible); truncate again; join with a **100 KB
    byte budget** (the line that crosses the budget is kept; counting includes
    `\n` separators once collected lines exist, even empty ones).
13. **Result assembly**: empty output (and not buffer-truncated) →
    `ok("No matches found[. {message}]")`. Otherwise `ok(message, brief=cmd)`
    with the joined output; if byte-truncated, append
    `Output truncated to {102400} bytes.`

The **legacy guarantee**: a single plain (selector-free, non-archive) entry
produces a byte-identical stream to the historical tool — grouping, remap and
normalization steps only activate for rich searches or explicit `grouped`.

### 4.8 Output line grammar

| Kind | Format | Notes |
|---|---|---|
| content match | `path:LN:text` | `:` delimiters; leftmost pair wins; path non-empty |
| content context | `path-LN-text` | `-` delimiters (rg convention) |
| block separator | `--` | between disjoint context runs |
| count | `path:count` | |
| files | `path` | one per line |
| grouped match / context | `*N|text` / ` N|text` | under `# path` headers |
| fold marker | `… (N lines omitted) …` | |
| dedup marker | `line  (N repeats)` | rtk-compatible |
| per-line cut marker | `… [+K chars]` | |

### 4.9 Sensitive files

Table (from `utils/sensitive.py`, order matters — first match wins):
patterns `.env`, `.env.*`, `id_rsa`, `id_ed25519`, `id_ecdsa`,
`.aws/credentials`, `.gcp/credentials`, `credentials`; exemptions (case-
sensitive membership, checked first): `.env.example`, `.env.sample`,
`.env.template`.

Matching: path-containing patterns (`x/y`) test `path.endswith(pattern)` or
`("/" + pattern) in path`; bare patterns `fnmatch` the basename with the
platform normcase (Windows lowercases, POSIX identity). Exemptions never match.
Behavior: hits in sensitive files are removed from output and the message
reports `Skipped {n} sensitive file(s) ({up to 5 distinct sorted basenames}
[, ... (M files total)]) to protect secrets. These files may contain
credentials or private keys.`

### 4.10 Session recorder

Distinct matched files in stream order (content mode: per parsed line path;
count mode: before last `:`; files mode: whole line), recorded into
`session.custom_data["grep"]["files"]` — merge with existing, insertion order
preserved (existing first), empties dropped, dedup, cap **500** keeping the
*tail* (drop from the front). Also stores `cwd` (workspace the paths are
relative to) and `updated_at` (ISO). Message note:
`Recorded {n} matched file(s) in session (use `read`/`edit` on them).`

### 4.11 Backup scanner (pure Python; also the VFS-dirty path)

No rg/rtk. Compiles `pattern` with the `regex` module (cached, 1024 entries);
`IGNORECASE`/`DOTALL` per params; invalid regex → error with the engine text.
Collects files itself: a single file, or `os.walk` skipping VCS dirs always
and `__pycache__ node_modules .venv venv dist build .tox .pytest_cache
.mypy_cache .egg-info .idea .vscode target out .next .nuxt` unless
`include_ignored`; ≤ 5 MB per file; binary (NUL in the read bytes) skipped;
`include` matches the file *name* via `fnmatch`; `type` via the built-in
`_TYPE_MAP`. Parallel per-file search on a thread executor (≤32 workers) with
prompt cancellation (`shutdown(wait=False, cancel_futures=True)`); line scans
prefer a native offset scanner when content splits on `\n` only (universal
`splitlines` specials like `\r`, `\x85`, `\u2028` would shift line numbers →
stay Python). Content mode emits `path:LN:text` / `path-LN-text` runs with
merged context intervals and `--` between them; ranges apply to *all* modes
(files/count scan the in-range window; content also clamps context to the
window). Then mirrors the same post-processing (sensitive filter, mtime sort,
prefix strip, remap, pagination, recorder, fold, byte cap) minus rtk/micro-
compress; `grouped` honors `params.grouped is True` only (plain path).

### 4.12 Errors and messages (catalog)

| Condition | Outcome |
|---|---|
| empty pattern (backup) / invalid regex | `ToolError("Pattern cannot be empty."` / `"Invalid regex pattern: {e}")` |
| bad `output_mode` value | pydantic validation error listing valid values |
| negative selector line / inverted range / zero count | `ToolError("Invalid line-range selector `{entry}`: {ValueError}")` |
| selector on a glob / a directory | `ToolError` (§4.4) |
| ranged selector, non-content mode (rg path) | `ToolError("Line-range selector requires output_mode='content' …")` |
| no readable archive members | `ToolError("Cannot search archive member(s): … read the member with `read <archive>:<member>` …")` |
| path outside workspace | `ToolError("`{path}` is outside the workspace.")` |
| Windows reserved device name | `ToolError("…reserved device name on Windows…")` |
| missing path (backup / native_io) | `ToolError("`{path}` does not exist.")` |
| subprocess timeout, no output | `ToolError("Grep timed out after {t}s. Try a more specific path or pattern.")` |
| subprocess timeout, partial | ok + "Partial results returned." |
| post-processing timeout | `ToolError("Grep post-processing timed out after {t}s. …")` |
| rg exit ≥2 | `ToolError("Failed to grep. Error: {stderr}")` after one EAGAIN retry |
| any unexpected exception | `ToolError("Failed to grep. Error: {str(e)}")` |

All errors carry a `brief` with the equivalent command (`_format_cmd`).

---

## 5. Constants and budgets

| Constant | Value | Role |
|---|---|---|
| `RG_MAX_BUFFER` | 20 000 000 B | rg stdout/stderr capture cap |
| `RG_KILL_GRACE` | 5 s | SIGTERM → SIGKILL |
| `MAX_BYTES` | 102 400 B (100 KiB) | final output byte budget |
| `_RG_HEAD_LIMIT_MARGIN` | 1000 | `--max-count` headroom |
| `RG_RANGE_FETCH_CAP` | 200 000 | per-file fetch cap for ranged selectors |
| `DEFAULT_MAX_LINE_LEN` | 500 | `truncate_line` hard cut |
| `RECORDER_CAP` | 500 | session file list bound |
| `_MAX_FILE_SIZE` | 5 MiB | backup scanner per-file cap |
| `MAX_ARCHIVE_MEMBER_BYTES` / `MAX_ARCHIVE_TOTAL_BYTES` | 8 MiB / 32 MiB | archive materialization guards |
| engine: binary sniff / file cap / head default | 64 KiB NUL window / 4 MiB / 250 | native_io grep engine (see §6.2) |
| VCS dirs excluded | `.git .svn .hg .bzr .jj .sl` | always, rg negative globs + backup skip list |

---

## 6. The C++ implementation (`kimix-native`)

### 6.1 Kernel library (parity mode — `unsupported` contract)

`src/builtin_tools/grep_tool.{h,cpp}` (namespace `kimix::builtin_tools::grep`)
compiles the Python tool's **pure CPU string kernels** (plan §3 kernels 1–6):

1. Selector grammar — `parse_line_range_chunk` / `parse_line_ranges` /
   `is_line_in_ranges` / `selector_line_ranges` / `split_path_and_sel` (the
   `os.path.lexists` probe is an injected `kimix::function<bool(string_view)>`)
   / `expand_path_entries` (strict JSON-array scanner with orjson semantics,
   `;` fallback) / `merge_ranges_into` / `entries_are_rich`.
2. Archive paths — extension table, `is_archive_path`,
   `parse_archive_path_candidates`, `safe_scratch_name`, `remap_display`,
   `strip_key_for`, `fnmatch_ascii` (shared with the engine).
3. Output rendering — `parse_content_line` (exact `^(.*?)([:\-])(\d+)\2(.*)$`
   re.DOTALL leftmost-pair semantics without a backtracking engine),
   `line_path_shape` (non-DOTALL variant), `format_match_line`,
   `group_lines_by_file`, `format_grouped_output`, `should_group`,
   `range_filter_lines`, `reattach_single_file_prefix`, `strip_path_prefix`,
   `normalize_slashes_content`, `collect_record_files`.
4. rtk protocol — `parse_rtk_rg_output` + `rtk_fold_note` (byte-exact message
   text incl. `tail -n +K` hint handling).
5. Recorder — `recorder_record`, `recorder_merge` (cap-500 tail-keep).
6. Sensitive files — `posix_basename` / `windows_basename` (pathlib flavour),
   `is_sensitive_path`, `sensitive_file_warning`.
7. Pattern kernels — `pattern_has_regex_newline`, `multiline_pattern`
   (re-ported for kimix-llm; runtime_py has its own copy).
8. Byte-budget join — `join_with_byte_limit`.

Contract: each kernel returns `tool_status` ∈ `ok` / `invalid_input` (carrying
Python's exact `ValueError` text) / `unsupported` (input outside the native
subset — non-ASCII decisive digits, integers past `uint32_t`, invalid UTF-8).
`unsupported` is the routing signal for the Python shim to call the pure-Python
mirror; the same convention as `grep_pattern.*` / `security.*`. The `Grep`
`Tool` class without `native_io` validates `pattern`/`paths`, runs the safe
preprocessing (`pattern_has_regex_newline`, `multiline_pattern`,
`expand_path_entries`) and always answers
`status:"unsupported"` + `"native grep tool is a kernel library; full
invocation requires Python-side orchestration"` plus the preprocessed fields —
Python owns rg/rtk orchestration, path/VFS resolution, archive I/O, session
persistence, downloads/timeouts/kill lifecycle.

Deliberate micro-semantics worth preserving: `str.strip()` uses the exact
Python whitespace code-point set (`0x09-0x0D`, `0x20`, `0x85`, `0xA0`,
`0x1680`, `0x2000-0x200A`, `0x2028/29`, `0x202F`, `0x205F`, `0x3000` — *not*
`0x1C-0x1F`); `is_line_in_ranges` maps Python `None` → empty span → true;
`$`-anchored rtk regexes also match before one trailing `\n`; the digit-run
scan stops "suspicious" at a non-ASCII byte (unsupported) but an ASCII-closed
pair is exact.

### 6.2 `native_io` branch — the pure-C++ grep engine (substitute, NOT parity)

When `Session::native_io` is set (only `src/agent/soul.cpp`, for the native
agent that has neither `rg` nor Python), `Grep::operator()` delegates to
`grep_engine.h::run_grep`: a ripgrep-inspired scan — recursive walk (roots
resolve against `work_dir`), whole-buffer reads with zero-copy `memchr('\n')`
line iteration, a literal fast path bypassing the regex engine, parallel search
on a `kimix::fiber` pool (one `regex_lite::Regex` per worker; results merged in
walk order → deterministic), 64 KiB NUL binary sniff, 4 MiB per-file cap, only
regular files, hidden entries skipped at every depth, `.gitignore` never read,
include glob = `fnmatch_ascii` over the file name, matching **lines** counted.
Honored parameters: `pattern` (regex_lite subset), `paths`/`path`,
`output_mode`, `-i`, `-A`/`-B`/`-C`, `include`, `head_limit` (default 250; the
JSON default in the reference tool is 500). Result JSON: `{status,
match_count, file_count, files, output, message}` with message
`{N} match(es) in {M} file(s)`; content lines follow the rg grammar
(`path:LN:text` / `path-LN-text` / `--`), but with **walk paths** (absolute,
never base-stripped); head_limit folds append
`[... N more match lines omitted ...]` (matches only — context lines don't
inflate the count) plus a summary suffix
`(N match lines omitted by head_limit; raise head_limit or narrow the search to
see more)`. A non-existent path is an `invalid_input` error
("`{path}` does not exist."), not an empty result set.

Known, pinned deviations from the Python tool (test
`grep_tool_native_io_branch_contract`, table in `reports/grep.md`): paths not
stripped, different message, hidden files skipped (Python searches them),
`.gitignore` ignored entirely, no sensitive filtering, no
`grouped/record/offset/fold/multiline/timeout/include_ignored`, and the
`regex_lite` pattern space instead of Python `regex`.

### 6.3 What stays blocked / in Python

The native regex **line matcher** (plan kernel 7, Phase B) is blocked:
byte-exact Python `regex` semantics need PCRE2 (lookaround, backreferences,
atomic groups, `\p{...}`…), which is not vendored and must not be added without
approval; `std::regex` (ECMAScript-only) and RE2 (no lookaround/backrefs) were
rejected — `grep_search_lines` returns `unsupported` unconditionally and the
line-offset scanner contract (caller supplies match offsets) ships instead.
Also still Python: rg/rtk subprocess orchestration and binary provisioning,
workspace/VFS path resolution, archive extraction I/O, session persistence, the
micro-compress driver (Phase C), `grep_args` argv builder (Phase D). Even a
future PCRE2 kernel must pass a compile-time feature-scan conformance gate,
routing regex-only features back to the Python mirror.

### 6.4 Verification

`tests/unit/builtin_tools/test_grep_tool.cpp` (Boost.UT, 52 tests / ≈1383
asserts) + `scripts/gen_grep_goldens.py`-harvested goldens
(`grep_goldens.inc`, 27 tables / 600 vectors, `--check` fails on staleness) +
`python/tests/test_parity_grep.py` for runtime_py-exposed kernels. Build/run:
`xmake f -m debug -y -c && xmake build test_builtin_grep &&
./bin/debug/test_builtin_grep.exe` (see the xmake/test skills).

---

## 7. Summary of the desired behavior (one paragraph)

Given a regex and a workspace-scoped path (which may name files, directories,
multiple `;`/JSON entries, line ranges, or archive members), grep searches
contents with ripgrep — downloading and pinning its own `rg`, optionally
wrapping it with `rtk` for dedup, falling back to an equivalent pure-Python
scanner when either is unavailable or the VFS is dirty — then post-processes the
match stream deterministically: strip rtk protocol lines, sort files by mtime
(files mode), make paths relative, normalize/remap rich paths, protect secrets,
apply ranges, record matched files on the session, summarize with exact counts,
deduplicate, paginate, group when rich, fold long output head+tail, truncate
long lines, and cut the whole payload at 100 KB — always telling the model in
the message what was omitted and how to see more (offset, head_limit,
max_output_lines, rtk logs, original-output temp files), and returning a brief
echoing the equivalent shell command. The C++ port reproduces every string
decision of that pipeline byte-for-byte (or honestly says `unsupported`), and
additionally offers the regex_lite-based engine so a native, rg-less agent can
still grep.

---

### Source files

* Reference: `kimi-cli/src/kimi_cli/tools/file/grep_local.py` (2430 lines),
  `grep_selectors.py`, `grep_archive.py`, `grep_output.py`, `grep_recorder.py`,
  `output_utils.py`, `micro_compress.py`, `utils/sensitive.py`,
  `_ripgrep_common.py`, `_rtk_common.py`, `install.py`, `utils/path.py`.
* Port: `src/builtin_tools/grep_tool.h`, `src/builtin_tools/grep_tool.cpp`,
  `src/builtin_tools/grep_engine.{h,cpp}`, `src/builtin_tools/regex_lite.h`,
  `src/runtime/tools/grep_pattern.*` (pattern-kernel twin).
* Reports: `src/builtin_tools/reports/grep.md`, `issue/grep.md`.
