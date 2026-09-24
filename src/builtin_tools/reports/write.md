# write — builtin tool kernels (C++ port) implementation report

Worktree: D:\KimiX-native (current task branch).
Files: src/builtin_tools/write_tool.h, src/builtin_tools/write_tool.cpp,
tests/unit/builtin_tools/test_write_tool.cpp.

## What was ported

The CPU-bound / correctness-critical guards of the `write` built-in agent tool
into one self-contained kernel pair, namespace `kimix::builtin_tools::write`
(ownership per `src/builtin_tools/README.md`; the conflict-marker symbols are
owned by `write` — `read` must not declare them).

| C++ function (write_tool.h) | Python source of truth | Notes |
|---|---|---|
| `utf8_decode_error` | `write.py` `errors="strict"` path (213–269, 388–395) + `utf8_util.h` | Wrapper around shared `utf8_strict_error` (CPython DecoderError wording + offset). Formats `utf-8 decoding error: <reason>`. |
| `expected_write_size` | `write.py` 390–395 (`len(...encode("utf-8","strict"))`) | Returns nullopt when the text is invalid UTF-8 (Python would raise before writing). |
| `is_auto_generated_file_name` | `auto_generated.py` 52–63, 134–137 | 10 literal prefix/suffix matchers over the basename (hand-rolled, no regex). |
| `get_comment_styles_for_path` | `auto_generated.py` 65–131, 124–131 | Basename tables (dockerfile/makefile/justfile) then extension table, default = all four styles. |
| `detect_auto_generated_marker` | `auto_generated.py` 208–227 | Filename wins (returns basename); else extract header from first 1 KiB (BOM strip, shebang skip, per-style comment blocks, 40-line cap) and scan the 4 strong markers; returns `match.group(0).strip()`. |
| `build_auto_generated_error` | `auto_generated.py` 230–241 | Byte-exact refusal text. |
| `match_marker` / `is_separator` | `conflict_detect.py` 127–156 | Strict column-0 match, trailing `\r` stripped, `prefix + " " + label` (label must not start with a space), exact `=======`. |
| `scan_conflict_blocks` | `conflict_detect.py` 162–312 (scan state machine) | Whole-file text split on `\n` (scan_file_for_conflicts contract); idle→ours→base→theirs, nested-opener restart, malformed-phase reset + re-process, unclosed tail dropped. |
| `find_dangling_openers` | `conflict_detect.py` 315–343 + `write.py` 530 (call-site contract) | Content is CRLF-normalized then split with Python `splitlines()` semantics, matching `write.py _conflict_guard`. |
| `splice_conflict` | `conflict_detect.py` 523–675 | Signature matching (label-tolerant), anchor at `start_line-1`, nearest-occurrence fallback, `_trim_echo` (multi-line always, single-line only when delimiter balance 0), CRLF re-application, exact “no longer matches” error via `false` return. |
| `expand_content_tokens` | `conflict_detect.py` 495–516 | `@ours`/`@theirs`/`@base`/`@both` line-token expansion; exact `@base` 2-way error. |
| `conflict_regions_equal` / `conflict_region_present` | `conflict_detect.py` 682–696 | Region text equality / CRLF-normalized substring presence. |
| `render_conflict_region` | `conflict_detect.py` 703–724 | Whole region or one side (ours/theirs/base) + 1-based start line; exact 2-way/unknown-scope errors. |
| `format_conflict_summary` | `conflict_detect.py` 808–837 | Whole-file index for `:conflicts`; exact text incl. ⚠ header, label lines, byte-cap note, NOTICE footer, `(3-way)` suffix. |
| `parse_conflict_uri` | `conflict_detect.py` 443–488 | `conflict://<N>[/scope]`, `conflict://*`, `<prefix>:conflict://N`; exact scope/id errors. |
| `parse_bulk_directives` | `conflict_detect.py` 844–864 | `<id>: @side` directives; false when any line is not a directive or nothing present. |
| `run_conflict_guard` / `build_conflict_markers_error` / `build_dangling_opener_error` | `write.py` 512–608 | Mode-aware refusal + warning-note composition (append dangling-opener refusal, marker refusals, allow_conflicts notes). |
| `check_json_format` | `check_fmt.py` 10–28 (`check_json_text`) | Vendored yyjson (`yyjson_read_opts`, no flags). 1-based line/col with col counting code points (orjson colno semantics); “extra data” is `unexpected content after document`. |
| `validate_format_by_path` | `write.py` 290–300 | `.json` → check; `.yaml/.yml/.toml/.xml` → `tool_status::unsupported`; other → ok. |
| `decide_parent_dir` | `write.py` 246–261 | Pure decision kernel (parent_exists / mkdir / create_error injected as data); exact refusals. |
| `build_unified_diff` | `utils/diff.py` 23–83 (`format_unified_diff`) | Deterministic LCS unified diff ported from `runtime/diff/diff_engine` semantics (SequenceMatcher autojunk=False, 3 context lines, `--- a/`/`+++ b/` headers, `@@ -l,c +l,c @@`). |
| verification_failed_error / size_mismatch_error / success_message / conflict_resolved_message | write.py 397–420, 476–488, 689–698 | Byte-exact message composition incl. the  Verified: size matches. note and the boundary-echo note. |
| Write / operator() | write.py top-level call + plan §3.4 | Tool subclass that validates JSON parameters and dispatches to the pure kernels above. Returns a ToolParams result (status/message/new_text/expected_size/output/conflict_note/fmt_error). Does **not** perform I/O, approval, snapshots, json_repair, or conflict:// orchestration — those stay in Python. |

What stays in Python (plan §3.6 justification, quoted)

- Params parsing, mode-synonym normalization, path resolution/canonicalization
  (KaosPath, resolve_vfs) — *“VFS is I/O + session state.”*
- All filesystem I/O (read_text, mkdir, write_text/append_text, stat) —
  *“async KaosPath layer; I/O-bound.”*
- Approval flow, `session.file_mtime`, snapshot store, FS-cache invalidation,
  edit parse guard, conflict-history registry — *“session/UI state.”*
- **JSON auto-repair (`json_repair`), YAML/TOML/XML validation** — *“no
  vendored parsers; stdlib tomllib/xml.etree and PyYAML”* (plan §3.4/§3.6).
  `validate_format_by_path` returns `tool_status::unsupported` for
  YAML/TOML/XML so the Python caller falls back to the mirror — this is the
  documented non-goal for this port (no `issue/` report is required; nothing
  here is impossible with the vendored libraries).
- Result message composition and conflict:// orchestration (history lookup,
  per-file grouping) — session-bound.
- The concrete `Write::operator()` dispatcher does not write bytes to disk,
  request approval, record snapshots, invalidate the FS cache, repair JSON, or
  resolve conflict:// URIs; it only runs the CPU kernels and returns a shaped
  result for the Python binding to consume.

Deviations / documented differences

(The parity review at the end of this report re-measured each row against the
Python reference; rows 1, 3, 4 and 5 were incomplete and rows 1/5 plus two
undocumented ASCII-only bugs have been fixed in the kernel.)

1. JSON message wording (plan §8 risk row). Decision parity (valid/invalid)
   is exact and test-gated. `yyjson` error wording matches orjson for every
   fixture we test (including `unexpected content after document` for extra
   data); the only difference is the zero-length document: orjson says
   `Input is a zero-length, empty document`, yyjson says `input length is 0` —
   the empty case is special-cased to orjson’s exact wording.
2. **Line-splitting contract (plan §8 splitlines-vs-split gate).** Per call
   site: `scan_conflict_blocks` uses `split("\n")` (scan_file_for_conflicts
   contract); `find_dangling_openers` / `run_conflict_guard` use
   `replace("\r\n","\n").splitlines()` (write.py `_conflict_guard` contract).
   Both strip a trailing `\r` per line while matching. The kernels implement
   the full Python `splitlines()` separator set (`\n \r \r\n \v \f \x1c-\x1e
   \x85 \u2028 \u2029`). Documented divergence: a bare-`\r`-only line ending
   (classic Mac) is a separator in `splitlines()` but not in `split("\n")`.
3. **ASCII gate (plan §8 regex-parity row).** The four auto-generated header
   markers are hand-rolled ASCII scanners: `\s` = `[ \t\n\r\f\v]`, `\w` =
   `[A-Za-z0-9_]`, `\b` = ASCII word boundary (same strategy as security.cpp /
   grep_pattern). Non-ASCII header text is not routed to Python inside this
   kernel; the ASCII boundary semantics are documented. `get_comment_styles`
   / filename detection lower-case ASCII only.
4. **`parse_conflict_uri` int parsing.** `int()`-approximation: optional ASCII
   spaces, optional sign, decimal digits, no underscores, no Unicode digits,
   int64 bounds (Python ints are arbitrary precision). For the practical id
   range this is identical; absurd ids that overflow int64 report the same
   “Invalid conflict id” error as a non-numeric id.
5. **`detect_auto_generated_marker` byte prefix.** The 1 KiB cap is applied in
   bytes and then trimmed to a UTF-8 code-point boundary (Python slices 1024
   *characters*; for ASCII headers the results are identical).
6. **No `format_conflict_warning`.** The read-side footer formatter is part of
   the conflict_detect surface but is not in this tool’s AGENT_TASK scope
   (write owns the scan/splice/index/side-extraction symbols; `read` can call
   back into these kernels). `format_conflict_summary` (the whole-file index)
   is provided.
7. Compiler-encoding independence. All sources are pure ASCII; non-ASCII
   message characters (⚠, em dash) are emitted as \xNN escapes because the
   project does not build test/builtin sources with /utf-8 (MSVC would mangle
   literal non-ASCII in narrow strings). Runtime bytes match the Python
   reference (E2 9A A0, E2 80 94).
8. Tool-class scope. `Write::operator()` accepts already-resolved parameters
   (file_path, content, mode, old_text, parent_exists, mkdir, create_error,
   allow_conflicts, allow_auto_generated, show_diff, auto_fix_json, outside,
   file_existed). It validates, runs the kernels, and returns a result object;
   it does not replicate the full async Python call path.

Test counts

./bin/debug/test_builtin_write.exe:
Suite 'global': all tests passed (4379 asserts in 58 tests).

Coverage mapped to plan §7:
- UTF-8 wrapper + expected size (2 tests)
- auto-generated filename patterns (33 vectors), header markers (21 vectors),
  header limits, comment styles, exact refusal text (5 tests)
- conflict: marker matching, two-way/diff3/multiple/offset/empty/CRLF/
  malformed scans, dangling openers, splice (basic/shifted/altered/echo-trim/
  CRLF), region semantics, token expansion, render region, whole-file index,
  URI parsing, bulk directives, write-guard refusals + notes (16 tests)
- JSON: valid + invalid line/col diagnostics (incl. multi-byte col counting),
  format dispatch incl. YAML/TOML/XML → unsupported (3 tests)
- unified diff goldens from difflib (7 cases + multi-hunk + no-header), mkdir
  decision, verification + success + resolution messages (3 tests)
- Tool class integration: null/missing/empty parameters, invalid mode/UTF-8,
  auto-generated guard, conflict-marker guard, parent-dir decision, unsupported
  format, overwrite/append success, diff output, JSON format error (added)
- 13 golden-driven parity tests over 1493 Python-derived vectors
  (tests/unit/builtin_tools/write_goldens.inc, regenerate with
  `python scripts/gen_write_goldens.py`): scan_conflict_blocks (205),
  find_dangling_openers (205), splice_conflict (242), expand_content_tokens (48),
  render_conflict_region (20), conflict_regions_equal/present (81+72),
  format_conflict_summary (30), parse_conflict_uri (35+6 deviation),
  parse_bulk_directives (21+5 deviation), run_conflict_guard (640),
  decide_parent_dir (8), expected_write_size (11), utf8_decode_error (21)
- python/tests/test_parity_write.py: differential vs the *real* kimi-agent code
  (10 tests): check_json_format vs orjson check_json_text over ~6000 JSON inputs
  (curated + seeded fuzz, incl. non-ASCII columns, trailing commas, escapes,
  surrogates, BOM, depth limits), validate_format_by_path dispatch,
  build_unified_diff vs the pure-Python difflib body (3046 comparisons),
  is_auto_generated_file_name / detect_auto_generated_marker vs
  auto_generated.py (65 curated + 3000 ASCII fuzz inputs).

Local verification only

tests/xmake.lua was temporarily edited to add
builtin_tools_test("test_builtin_write", "unit/builtin_tools/test_write_tool.cpp")
inside the marker block; that edit is NOT committed (restored before commit).

Verification pass (2025 parity review) — what changed

The JSON wording deviation was **understated**: yyjson's message wording only
matched orjson for a handful of cases. Measured over 54775 distinct inputs,
1829 diverged (wording and/or column): yyjson keeps the "expected …" detail,
reports the BOM diagnostic, uses "no digit after sign" for +/-/. and points at
the backslash (not the escape character / past the `\uXXXX` escape) for string
escape failures. orjson also caps container nesting at 1024 levels while
yyjson's reader is unlimited, which flipped the valid/invalid *decision* for
documents nested deeper than 1024. Fixed in check_json_format: a
yyjson→orjson message/offset translation table, a 1024-level depth gate that
reports "array and object recursion depth exceeded" at orjson's offset, and the
zero-length special case kept. Result: 54775/54775 exact matches.

Also fixed in the auto-generated guard (real, ASCII-only bugs, not documented as
deviations):
- `scan_header_markers` only tested the first `@generated` occurrence;
  `regex.search` continues past one whose next character is a word character
  (`"# @generatedx @generated\n"` → Python "@generated", port returned none).
- the 1 KiB prefix was sliced in bytes; now 1024 *code points*
  (`"#" + "é"*600 + "\n# @generated\n"` → Python detected the marker, the port
  did not).
- `basename_of` did not drop trailing separators (`"generated.py/"` → Python
  true, port false).
- Python whitespace semantics: header-line `str.strip()`, the marker patterns'
  `regex \s`, `int()` and bulk-directive parsing now use the code-point
  whitespace sets (U+00A0, U+0085, U+1680, U+2000-200A, U+2028/9, U+202F,
  U+205F, U+3000; \x1c-\x1f only for str.isspace()).
- `parse_bulk_directives` returned duplicate entries for a repeated id; it now
  applies Python dict semantics (first position, last side wins), matching
  `"1: @ours\n1: @theirs"` → {1: "theirs"}.

Still known (documented, test-pinned):
- ASCII `\b`/`\w` gate: a marker directly adjacent to a non-ASCII *word*
  character differs (`"<!--@generatedé"` → Python none, port "@generated");
  implementing it needs Unicode category tables
  (test_detect_auto_generated_marker_known_non_ascii_gap).
- int()/int approximation: ids with underscores, non-ASCII decimal digits or
  beyond int64 (URI) / int32 (bulk directives) report the "Invalid conflict id"
  error where Python accepts them (wr_uri_dev_g / wr_bulk_dev_g goldens).
- orjson quirk (not reproducible by any parser): an *unterminated* array nested
  deeper than ~370 levels reports "memory allocation failed" at an arbitrary
  column; the port reports the real "unexpected end of data" (both invalid).
- trailing-comma column for one exotic shape: `"[[1, ] ,]"` (whitespace before
  the closing bracket of a nested container plus a comma right after it) —
  orjson col 4, port col 7; decision parity holds.
- Tool class only (deviation 8): with `auto_fix_json=false` the dispatcher
  returns invalid_input *before* writing, while write.py writes and then returns
  the ToolError `"File successfully {overwritten|appended to}, but {fmt_error}
  Path: {path}"`; with `show_diff=true` the dispatcher emits the unified diff
  from `build_unified_diff` (byte-exact vs `utils/diff.py format_unified_diff`,
  which has no remaining caller in kimi-cli) while write.py emits the pydantic
  repr of the DiffDisplayBlock.
- deviation 6 (no format_conflict_warning) and 7 (ASCII sources / \xNN escapes)
  verified unchanged.

