# grep builtin tool — C++ port report (Phase A string kernels)

Worktree `D:/KimiX-native`.
Plan: `D:/KimiX-native/plans/grep.md`. Python source of truth:
`kimi-cli/src/kimi_cli/tools/file/` (+ `utils/sensitive.py`).

## What was ported (namespace `kimix::builtin_tools::grep`)

| C++ kernel | Python reference (file:lines) |
|---|---|
| `parse_line_range_chunk` | grep_selectors.py 79-118 — exact ValueError texts, `..` alias, `N+`→`N-` open-ended, `L` prefixes, case-insensitive |
| `parse_line_ranges` | grep_selectors.py 121-160 — comma split, stable sort by start, overlap/adjacent merge, open-ended absorption |
| `is_line_in_ranges` | grep_selectors.py 163-172 (see deviation 1) |
| `selector_line_ranges` | grep_selectors.py 175-193 — `:`-chunk walk skipping `raw`/`conflicts` |
| `split_path_and_sel` | grep_selectors.py 218-263 — filesystem probe injected as `kimix::function<bool(string_view)>`; Windows drive-letter guard (`ntpath.splitdrive` shapes) and `scheme://authority` port guard replicated; at most two chunks peeled and rejoined with `:` |
| `expand_path_entries` (string + list forms) | grep_selectors.py 266-306 — strict JSON array of strings (orjson semantics: control bytes rejected in strings, lone surrogates rejected, trailing content rejected, whitespace-tolerant), else `;` split; strip + first-seen dedup |
| `merge_ranges_into`, `ranges_map_find`, `entries_are_rich` | grep_selectors.py 309-320, grep_local.py 1124-1131 |
| `parse_archive_path_candidates` | grep_archive.py 39-61 — rightmost-first nested splits, archive-extension gate |
| `is_archive_path`, `archive_extensions` | read_archive.py 29-77 — 23-extension table, case-insensitive |
| `safe_scratch_name` | grep_archive.py 78-82 — `[^\w.-]+` → `_` on basename, `or "member"` fallbacks |
| `remap_display` | grep_local.py 1014-1031 — forward-slash prefix rewrite, original line kept on miss, empty-map early return |
| `strip_key_for` | grep_local.py 980-986 |
| `parse_content_line` | grep_local.py 284-302 — exact `^(.*?)([:\-])(\d+)\2(.*)$` leftmost-pair semantics (`re.DOTALL`), empty-path → no match, `--` separator |
| `line_path_shape` | grep_local.py `_RG_LINE_RE` (non-DOTALL: delimiter must precede the first newline) |
| `format_match_line`, `group_lines_by_file`, `format_grouped_output`, `group_line_indices_by_blank`, `should_group` | grep_output.py 22-115 |
| `range_filter_lines` | grep_local.py 1053-1078 — drop out-of-range lines, prune leading/doubled/trailing `--` |
| `reattach_single_file_prefix` | grep_local.py 836-855 — `_BARE_CONTENT_RE` `^(\d+)([:\-])`, result is `prefix + sep + line` |
| `strip_path_prefix` | grep_local.py 637-650 — replace-then-rstrip order matters (`C:\w\` → `C:/w`) |
| `normalize_slashes_content` | grep_local.py 1034-1050 |
| `collect_record_files` | grep_local.py 1081-1093 — content parse / `rfind(':')` for count_matches / whole line for fwm |
| `parse_rtk_rg_output` | output_utils.py 161-239 — header + trailing blank drop, per-file fold markers (lazy path, first qualifying ` [see remaining: `), files fold marker, `_parse_tail_hint` (`tail -n +K <log>`), tolerant passthrough |
| `rtk_fold_note` | grep_local.py 658-695 — byte-exact message text incl. `Full log:` reconstruction and `Original output:` |
| `recorder_record`, `recorder_merge` | grep_recorder.py 28-60, 81-82 — insertion-ordered dedup, empties dropped, cap-500 front-drop |
| `posix_basename`, `windows_basename`, `is_sensitive_path`, `sensitive_file_warning` | utils/sensitive.py — SENSITIVE_PATTERNS/SENSITIVE_EXEMPTIONS tables, fnmatch with normcase flavour (case-fold on Windows only), path-suffix rules for `.aws/credentials` |
| `pattern_has_regex_newline`, `multiline_pattern` | grep_local.py 106-138 (re-ported here: `kimix-llm` cannot link `runtime_py`'s copy) |
| `join_with_byte_limit` | grep_local.py 620-634 |
| `Grep` Tool subclass | plans/grep.md §4 — CallableTool2-style binding entry point; validates parameters and runs safe native preprocessing |

Tool class wrapper

The kimix::builtin_tools::grep::Grep class implements the standard
kimix::builtin_tools::Tool interface (operator()(ToolParams const *)).
It validates the required pattern and paths fields, runs the safe native
preprocessing kernels (pattern_has_regex_newline, multiline_pattern,
expand_path_entries), and serializes the preprocessed values.  Because full
grep invocation (rg/rtk subprocess orchestration, archive extraction, session
persistence, and regex matching) stays in Python, the result always carries
status: "unsupported" so the Python shim falls back to its full mirror.
This matches the plan's kernel boundary: C++ owns deterministic CPU-only text
kernels; Python owns async I/O and the session lifecycle.

native_io branch (simplified ripgrep) - NOT Python parity

When `Session::native_io` is set (only src/agent/soul.cpp does that, for the
native agent; nothing in python/ or src/runtime/ sets it, and no runtime_py
binding exposes the Tool class) the preprocessed values are ignored and
operator() runs its own search: regex_lite over a recursive filesystem walk,
returning {status, match_count, file_count, files, output, message}. Reachable,
but a different tool from kimi_cli's grep:

| Aspect | Python tool (grep_local.py) | native_io branch |
|---|---|---|
| paths in the result | `_strip_path_prefix` display paths ("a.py", "sub\\b.py") | walk paths, search base NOT stripped |
| message | builder text, e.g. `Found 2 files matching 'hit'.`, plus the sensitive/rtk/fold notes | `"{N} match(es) in {M} file(s)"` |
| file selection | rg semantics: .gitignore honoured, `include_ignored` re-enables ignored files | every non-hidden file <= 4 MiB, .gitignore never read |
| hidden entries | rg default (hidden skipped) + `-uuu` escape hatches | any '.'-prefixed entry skipped at every depth, never searched |
| sensitive files | filtered out with `sensitive_file_warning` | not filtered |
| parameters | all of Params (grouped/record/include/type/offset/-n/token_kill/fold/multiline/timeout/include_ignored) | pattern, path(s), output_mode, -i, -A/-B/-C, include, head_limit |
| rendering | `parse_content_line` pipeline, `format_grouped_output`, rtk cleanup, fold/dedup/truncate | raw `path:LN:text` lines, one `--` between non-adjacent hit runs inside a file |
| matcher | Python `regex` (PCRE-ish: lookaround, backrefs, \p{...}) | `regex_lite` (documented subset, see src/builtin_tools/regex_lite.h) |

Concrete repro (pinned by "grep_tool_native_io_branch_contract"): a temp dir
with a.py `hit\nmiss\n`, sub/b.py `miss\nhit\n`, .hidden.py `hit\n` and
pattern="hit", path=<dir>, output_mode="files_with_matches" gives

* native_io: status "ok", files=["C:\\Users\\...\\kimix_grep_native_io\\a.py",
  "...\\sub\\b.py"], message="2 match(es) in 2 file(s)";
  content mode emits "C:\\...\\a.py:1:hit" (absolute), hidden file skipped;
* Python: the same call returns the base-stripped lines "a.py" and "sub/b.py"
  (grep_local `_strip_path_prefix(lines, ctx.prefix_base)`, differentially
  covered by the goldens: base "/tmp/dir" turns "/tmp/dir/file.py" into
  "file.py") and a builder message, not "N match(es) in M file(s)".

Verdict: the branch is intended (the native agent cannot run rg/Python) but it
is a substitute, not a port, and the earlier claim that the tool "always"
answers unsupported was wrong. Documented here, in grep_tool.h's class comment
and by the pinned test; the Python side remains the reference implementation.


## Unicode parity strategy (same convention as `grep_pattern.*` / `security.*`)

- **Hard ASCII gate**: kernels whose decision depends on `\d` (Nd) or `[^\w.-]`
  inside a region containing non-ASCII bytes return `tool_status::unsupported`
  so the Python shim routes to the pure-Python mirror.
- **Bounded gate**: for content-line parsing only the bytes up to the closing
  delimiter are decisive, so lines with non-ASCII *text* (the common case) stay
  native and byte-exact; a digit run stopped at a non-ASCII byte returns
  `unsupported` instead of guessing.
- `str.strip()`/blank tests use the exact Python whitespace code-point set
  (0x09-0x0D, 0x20, 0x85, 0xA0, 0x1680, 0x2000-0x200A, 0x2028/29, 0x202F,
  0x205F, 0x3000 — note: **not** 0x1C-0x1F, which `re.\s` matches but
  `str.strip()` does not), so they need no gate.

## What stays in Python (per plan §3/§9)

rg/rtk subprocess orchestration and binary provisioning, workspace/VFS path
resolution, archive extraction I/O (`ArchiveReader`/zipfile), session
persistence (`session.custom_data`), the micro-compress driver (Phase C),
`grep_args` argv builder (Phase D), and the regex line-matcher (Phase B —
**blocked**, see `issue/grep.md`). Plan §1/§9 justify keeping orchestration in
Python: it is I/O- and lifecycle-bound (downloads, timeouts, SIGTERM→SIGKILL,
executor shutdown), not CPU-bound, and the toggle/escape-hatch
(`KIMIX_NATIVE_TOOLS=0`) must restore the exact Python path.

## Tests

Tests

tests/unit/builtin_tools/test_grep_tool.cpp — 52 main-scope Boost.UT tests,
≈1383 asserts, all passing (xmake f -m debug -y -c, xmake build kimix-llm,
xmake build test_builtin_grep, ./bin/debug/test_builtin_grep.exe). Golden
vectors were harvested by running the Python reference modules directly
(golden.json / gen_golden.py capture scripts live untracked in the worktree
root).

Golden-vector harness (parity re-verification pass)

* scripts/gen_grep_goldens.py runs the kimi-agent reference
  (kimi-cli/src/kimi_cli/tools/file/{grep_selectors,grep_local,grep_output,
  grep_recorder,output_utils}.py + utils/sensitive.py) over a corpus taken from
  its own test suite (kimi-cli/tests/tools/test_grep*.py) plus adversarial
  and writes tests/unit/builtin_tools/grep_goldens.inc (27 tables, 600
  vectors). test_grep_tool.cpp feeds each vector to the C++ kernel and
  compares a canonical rendering (TAB-separated input fields, JSON-ish output)
  — `python scripts/gen_grep_goldens.py --check` fails when the .inc is stale.
* python/tests/test_parity_grep.py covers the kernels reachable through
  runtime_py (file.pattern_has_regex_newline, tools.{pattern_has_regex_newline,
  multiline_pattern,scan_lines,scan_lines_cb,find_in_file}) against the
  reference bodies (169 passed, 1 skipped: CRLF find_in_file needs universal
  newlines, which readlines() applies in the reference only).

Kernel bugs found and fixed while verifying against Python

* expand_path_entries(raw string): the `[` JSON branch required ASCII input, so
  `'["café.py","b.py"]'` skipped orjson and fell through to the ';' split,
  returning the whole string as one entry (Python: ["café.py", "b.py"]). The
  scanner is UTF-8-clean end to end, so the ASCII precondition was dropped.
* parse_line_ranges: the adjacent-merge test used `end + 1u`, which wraps at
  UINT32_MAX, so "4294967295-4294967295,4294967295-4294967295" produced two
  ranges where Python merges them into one (compare in uint64 now).
* join_with_byte_limit: the newline separator was keyed on the joined buffer
  being non-empty instead of on the number of collected lines, so a leading
  empty line swallowed the next separator (["", "b"] gave "b", Python "\nb").
* parse_rtk_rg_output / rtk_regex_view: the protocol regexes are anchored with
  `$`, which also matches before ONE trailing newline; the native matchers
  required an exact end of line, so a line ending in '\n' was not recognized as
  a header/fold marker (Python does recognize it).
* parse_tail_hint: `(\S+)$` was emulated with a "no space and no tab" test, but
  Python's `\s` also rejects \v/\f/\n/\r, so
  "tail -n +5 lo\x0bg" wrongly parsed as start_line=5 (Python keeps the raw
  hint and no start line). The check now rejects the whole `\s` ASCII set
  (verified against the `regex` module: 0x09-0x0D and 0x20, and 0x1C-0x1F are
  NOT `\s` there, unlike stdlib `re`).

Deviations / test fixes (test contradicted Python → test fixed)


1. `is_line_in_ranges(line, {})` returns **true** (Python `None` → unfiltered).
   The C++ span cannot express `None`; the documented contract maps `None` →
   empty span (Python `is_line_in_ranges(12345, [])` is `False`, but callers
   only ever pass `None` or a non-empty per-path bucket).
2. Test `{"1-3,L4-L6,7-"}` expected `1-6|7-open`; Python merges the adjacent
   open-ended tail → `1-open`. Fixed to `1-open`.
3. `reattach_single_file_prefix` golden `["0:",…]` with prefix `f` expected
   `f0:`/`f3-`; Python produces `f:0:`/`f-3-` (`f"{prefix}{sep}{line}"`).
   Code was inserting the separator correctly; the test vector was wrong — fixed.
4. `pattern_has_regex_newline("a\\\\\\\\n")` (a + four backslashes + n) expected
   true; Python's `(?<!\\)(?:\\\\)*\\n` gives False (even backslash run). Fixed.
5. `basename_flavours`: `C:` / `C:\` posix names are `"C:"` / `"C:\"` and
   `C:foo` posix is `"C:foo"` (PurePosixPath keeps the colon component);
   `//srv/share/f` Windows name is `"f"` (PureWindowsPath). Fixed per pathlib.
6. `ascii_gate` test: `"café:1:١"` parses natively (Python gives the identical
   answer — the bounded gate keeps it on the fast path); the gate correctly
   fires on `"café:١:x"` (non-ASCII digit run) and friends. Rewrote the vectors.
7. `truncate_line("abcdefgh", 5)` → `"abcde"`: the `… [+3 chars]` marker cannot
   fit in 5 chars, so Python cuts bare without a marker. Test updated.

Kernel bugs found and fixed while verifying against Python:
`strip_text` dropped the first non-space character; `fnmatch_ascii` star
backtracking never advanced (infinite loop on `.env.*` vs `.env.local`);
`rtk_files_fold_match` hint never went through `_parse_tail_hint`;
`strip_path_prefix` rstripped before slash-normalizing; the selector shape
grammar missed the optional `L` prefix on the second number (`a.py:L5-L6`);
`json_string_array` skipped inter-item whitespace.
