# glob — C++ port implementation report

Tool: **glob** (namespace `kimix::builtin_tools::glob`)
Files: `src/builtin_tools/glob_tool.h`, `src/builtin_tools/glob_tool.cpp`
Test: `tests/unit/builtin_tools/test_glob_tool.cpp`

Plan: `C:/dev/kimi-agent/plans/glob.md` (§3.1 fnmatch core, §3.2 path-glob
matcher, §3.3 native walker). Python source of truth:
`C:/dev/kimi-agent/kimi-cli/src/kimi_cli/tools/file/glob.py`
(`Glob.__call__` 508–698, `Params` 420–473, gitignore logic 115–417, unsafe
guard 46–67) plus the CPython `fnmatch._translate` / `pathlib.Path.glob`
semantics the tool delegates to via `kaos/local.py:102–121`.

## Result

```
Suite 'global': all tests passed (369 asserts in 32 tests)
```

## What was ported (function-by-function)

### §3.1 fnmatch core

| C++ (`glob_tool.h/.cpp`) | Python reference | Notes |
|---|---|---|
| `fnmatch_match(pattern, text, case_insensitive)` | `fnmatch.fnmatchcase` (the case-sensitive core that `fnmatch.fnmatch` calls after `os.path.normcase` on Windows) | Full re-implementation of CPython 3.14 `fnmatch._translate` semantics: `*` (consecutive collapse), `?`, `[seq]` / `[!seq]` bracket compilation incl. CPython's chunk-based `-`-range algorithm, reversed-range removal, leading/trailing `-` as literals, `]` right after `[` as a member, unterminated `[` as literal, backslash as an ordinary member character. `*` matches any char **including** `/` (segment awareness is enforced by the path layer calling per segment). |
| `fnmatch_match_default_case(...)` | `fnmatch.fnmatch` platform behavior | Applies `default_case_insensitive()`. |
| `default_case_insensitive()` | `glob.py:43` `_NATIVE_GLOB_MATCH_CASE_SENSITIVE = not sys.platform.startswith("win")` | true on Windows, false elsewhere. |

Case folding is **ASCII-only** (A–Z → a–z), mirroring the documented
limitation of the already-native `runtime/glob` kernels. On Windows the real
`fnmatch` folds via `os.path.normcase` (ASCII lowercase); the ASCII-only model
is byte-identical for ASCII names. The non-ASCII gap (`é`/`İ`/`ß`) is the same
one the plan's §8 risk table already documents and gates with the
`_NATIVE_GLOB_MATCH_CASE_SENSITIVE` toggle.

### §3.2 path-glob matcher

| C++ | Python reference | Notes |
|---|---|---|
| `parse_pattern(pattern, ci, out)` / `parse_pattern_default_case` | `pathlib.PurePath._parse_pattern` (via `Path.glob`) | Split on `/`, drop empty and `.` segments, record `dir_only` (trailing `/`, GH-65238) and `anchored` (contains `/`). Errors returned as `tool_error` (`invalid_input`), never thrown: empty pattern / only-`.` pattern → "Unacceptable pattern", absolute/drive/UNC pattern → "Non-relative patterns are unsupported". |
| `match_segments` / `match_path` / `match_path_pattern` | `pathlib.Path.glob` matching (segment-wise fnmatch + `**` recursion) | NFA over pattern segments: `*` matches one segment (never crosses `/`), `**` matches zero or more segments, dotfiles are **not** hidden, `\` normalized to `/` on both sides. |
| `match_basename_at_any_depth` / `is_basename_pattern` | tool *description* promise (`glob.py:425–428`) | The shipped pathlib walk anchors a `/`-free pattern at the root; the tool description promises basename-at-any-depth. Both rules are exposed; the walker uses the pathlib-faithful one. |
| `is_unsafe_recursive_pattern` | `glob.py:46–67` `_is_unsafe_recursive_pattern` | Statement-for-statement port: normalize `\`→`/`, `lstrip("./")`, split on `/`, require a `**` segment, reject when every segment is `*`/`**`. |
| `make_unsafe_pattern_error` | `glob.py:513–522` ToolError text | Byte-exact message + brief. |

### §3.3 native walker

| C++ | Python reference | Notes |
|---|---|---|
| `walk_matches(lister, stat, pattern, options)` | the walk + per-match loop in `Glob.__call__` (`glob.py:569–600`) driven by `pathlib.Path.glob` | Injectable `list_dir_fn` / `stat_fn` so unit tests use an in-memory tree. Collects matches, applies the `include_dirs` and trailing-`/` dir-only gates, gitignore filter-after-walk, `max_matches` pop-on-overflow cap (`glob.py:597–600`: exactly `max_matches` matches is **not** capped), cooperative `deadline_ms` abort, then sorts by rel path and dedups. |
| `walk_matches_fs(root, pattern, options)` (+ string overload) | the real filesystem walk | Win32 `FindFirstFileW`/`FindNextFileW` on Windows, `opendir`/`readdir` elsewhere, compiled under `KIMIX_PLATFORM_WINDOWS` / `else`. Skips `.`/`..`, never descends into symlinked directories, skips unlistable dirs silently (`skipped_dirs`), fills size/mtime when `collect_stats`. |

### gitignore-ish ignore filter

| C++ | Python reference | Notes |
|---|---|---|
| `parse_ignore_rules(content, source_dir)` | `_parse_gitignore` pure-Python fallback (`glob.py:137–166`) | `splitlines` + `rstrip`, skip blank/`#`, `!` negation, trailing `/` dir-only, anchoring on any `/`, leading `/` stripped + forces anchored. Preserves leading whitespace (Python `str.rstrip` is right-only). |
| `ignore_rule_match(rel_path, is_dir, rule, ci)` | `_gitignore_match` (`glob.py:169–238`) | dir-only descendants, the `**` / `**/` / `/**` / `/**/` special cases and the generic `**`→`*` collapse, anchored vs basename/component matching. |
| `is_ignored(rel_path, is_dir, rules, ci)` | `_is_ignored_by_gitignore` (`glob.py:241–286`) | Per-rule source-dir scoping, last-match-wins with negation. |

### Result shaping

| C++ | Python reference | Notes |
|---|---|---|
| `sort_entries` | `glob.py:605` `matches.sort()` | byte order. |
| `dedup_entries` | (defensive; pathlib yields unique paths) | first-occurrence rel-path dedup. |
| `strip_prefix` | `str(p.relative_to(dir_path))` (`glob.py:615`) | |
| `order_by_mtime_top_k` | tool description "modification-time order, up to N paths" | stable descending mtime + top-k. |
| `paginate_entries` | head_limit / offset pagination | `omitted_after` = rows dropped from the tail after the offset. |
| `shape_output` | verbose line build + 100 KiB cap + fold (`glob.py:614–645`) | per-line `truncate_line`, byte budget, `fold_lines` head+tail marker. mtime text is caller-supplied (pendulum stays Python). |
| `top_dirs_summary` | `_top_dirs_summary` (`glob.py:296–317`) | top-N by count, ties by name, root files not counted. |
| `build_result_message` | message assembly (`glob.py:660–690`) | byte-exact, including the fold/cap/timeout/byte notes in Python order. |

## What stays in Python (and why)

Per the plan, these remain Python-side orchestration and were deliberately not
ported:

- **Params validation / path resolution / VFS** (`glob.py:524–540`,
  `kaos_path_from_tool_input`, `resolve_vfs`) — tool-interface and
  workspace-policy logic, not a hot kernel.
- **gitignore rule discovery + cache** (`_find_gitignore_files`,
  `_load_gitignore_rules`, `_get_gitignore_rules`,
  `invalidate_gitignore_cache`, `glob.py:320–417`) — module-level mutable
  cache with mtime refresh; the plan keeps it Python-side (success criterion:
  `test_fs_cache_invalidation.py` keeps passing with the cache in Python).
- **stat metadata formatting** — raw size/mtime are collected natively, but the
  `pendulum` datetime string stays Python (`glob.py:621–622`), injected into
  `shape_output` via `format_mtime`.
- **micro-compress** (`glob.py:651–656`) — plan lists output shaping as
  orchestration that stays Python.
- **pybind11 binding / shim / `use_native` wiring** (`py_glob.cpp`,
  `kimix_native/glob.py`) — out of scope for the builtin-tools static lib.

## Deviations from the tests as first written

The delivered test file was corrected in several places where it contradicted
the Python reference (the plan's instruction: "follow the Python reference and
record the deviation"). Each was verified against CPython 3.14
`fnmatch`/`pathlib` before changing the test, never the other way around:

1. **`[a\-c]` does not match `-`.** CPython's chunk algorithm splits
   `[a\-c]` into chunks `a\` and `c`; the `-` is a chunk separator, not a
   member. The backslash itself is a member. (Test now asserts `a`,`c`,`\`
   match and `-` does not.)
2. **Leading spaces survive `rstrip`.** `"  spaced.txt  ".rstrip()` is
   `"  spaced.txt"`; the parsed pattern keeps the two leading spaces.
3. **Anchored ignore rules DO sink into sub-directories.** `_gitignore_match`
   uses `fnmatch.fnmatch`, whose `*` crosses `/`, so anchored `src/*.py`
   matches `src/deep/a.py`.
4. **`a**b` collapse does not match `aXXXb.py`.** `**`→`*` gives `a*b`, which
   requires the string to end in `b`; it matches `aXXXb` but not `aXXXb.py`.
5. **`max_matches` truncation is walk-order, then sort.** `glob.py:597–600`
   pops on overflow during collection and sorts afterwards, so the capped
   result is the first 25 *collected* entries sorted, not the head of the
   fully-sorted list.
6. **gitignore filter-after-walk expectations.** With rules
   `.venv/`,`node_modules/`, `!.venv/pyvenv.cfg` and pattern `**/*.py`, the
   only pattern match that is also ignored is `.venv/lib/site.py`
   (`ignored_count == 1`); `.gitignore`, `pyvenv.cfg` and `index.js` are not
   `**/*.py` matches. `visited_dirs == 6` (no pruning).
7. **Fold marker position.** The head+tail marker sits at index `head`
   (= `max_results/2`), not at the end.
8. **`paginate_entries` omitted count.** 10 entries, offset 2, limit 4 →
   8 available, 4 shown, 4 omitted (not 5).
9. **`build_result_message` flags.** The timeout and byte-truncation notes are
   only emitted when `timed_out` / `truncated_by_bytes` are set; the test now
   sets them before asserting the combined message.
10. **Real-FS pattern length.** The `walk_matches_fs` overload was called with
    a `string_view` length of 6 for the 7-char pattern `**/*.py`; corrected to 7.

Two **code** bugs were also found and fixed (not test changes):

- The walker's kind gate dropped directory matches for a trailing-`/`
  (dir-only) pattern when `include_dirs` was false; pathlib yields the
  directory regardless. Fixed so a dir-only pattern yields directories
  independent of `include_dirs`.
- `dedup_entries` only removed *adjacent* duplicates; its contract is
  first-occurrence dedup. Fixed with a `kimix::unordered_set` (using
  `kimix::string_hash`, since `kimix::hash` is not specialized for
  `kimix::string`).

## Test counts

- **32** main-scope `"_test"` lambdas, **369** asserts, all passing.
- Coverage spans the plan §7 list: fnmatch literals/wildcards/brackets/case,
  pattern parse shape+errors, matcher matrix (`**` zero/multi-level, leading/
  trailing `**`, dotfiles, `?`, `[...]`), case + Windows-separator handling,
  trailing-slash dir-only, basename-at-any-depth, unsafe-pattern guard, ignore
  rule parse/match/negation/multi-source, walker basic/include_dirs/
  max_matches/gitignore/pruning/symlinks/unlistable/stats/deadline/empty,
  real-filesystem wrapper, and all result-shaping helpers.

## No blockers

Everything was implementable with the vendored libraries and kimix-core; no
`issue/glob.md` was needed.
