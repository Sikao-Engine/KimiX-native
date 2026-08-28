# edit — builtin tool kernels implementation report

Worktree: `C:/dev/kimix_wt/edit` (branch `agent/edit`)
Deliverables: `src/builtin_tools/edit_tool.h`, `src/builtin_tools/edit_tool.cpp`,
`tests/unit/builtin_tools/test_edit_tool.cpp`.

Python source of truth (relative to `C:/dev/kimi-agent/kimi-cli/src/kimi_cli`):

- `tools/file/edit/__init__.py` (1–107) — dispatcher helpers
- `tools/file/edit/modes/replace.py` (55–223) — replace kernels
- `tools/file/edit/diff.py` (38–344) — unified-diff kernels
- `tools/file/edit/modes/sloppy.py` (55–239) — sloppy kernels
- `tools/file/edit/modes/hashline.py` (88–298) — hashline grammar parser
- `tools/file/hash_line.py` (55–87, 250–553) — line hash + apply
- `tools/file/snapshot_store.py` (62–64) — `detect_line_ending`

Design plan: `C:/dev/kimi-agent/plans/edit.md` §3.1–§3.6, §7, §8. The plan
describes separate `edit_common / fuzz_ratio / edit_replace / edit_diff_apply /
edit_hashline / edit_sloppy / edit_conflict` files; per AGENT_TASK.md this port
implements the same kernels in **one pair** `edit_tool.h/.cpp` under the single
nested namespace `kimix::builtin_tools::edit` (unity-build rule from
`src/builtin_tools/README.md`).

## What was ported

All kernels are pure, stateless, CPU-only (no filesystem, no subprocess, no
Python). Errors are returned as data (`tool_error` / `tool_status`), never
thrown across the tool boundary.

### 1. Common newline / line helpers
| C++ symbol | Python reference |
|---|---|
| `normalize_newlines` | replace.py `_normalize_line_endings` (55–57): `\r\n` → `\n` only |
| `normalize_breaks` | diff.py:40 / sloppy.py:57 / hashline.py:257: `\r\n`→`\n`, then `\r`→`\n` |
| `split_lf` | `str.split("\n")` semantics (trailing empty element; `"".split("\n")==[""]`) |
| `split_lines` / `split_lines_keepends` | `str.splitlines()` / `splitlines(keepends=True)` incl. `\r`, `\r\n`, `\v`, `\f`, U+0085, U+2028/29 boundaries |
| `join_lf` | `"\n".join(lines)` |
| `detect_line_ending` | snapshot_store.detect_line_ending (62–64) |
| `restore_trailing_newline` / `_nonempty` | diff.py:342–343 / hash_line.py:551–552 |
| `py_strip` | `str.strip()` (Python whitespace code-point set) |
| `py_repr` | `repr()` for `{...!r}` error interpolation (ASCII-exact) |

### 2. fuzz_ratio
`fuzz_ratio(a, b)` = rapidfuzz `fuzz.ratio` = 100·(1 − indel/(len_a+len_b)) with
indel = Levenshtein distance with substitution cost 2 (= (len_a+len_b) − 2·LCS).
Independent prefix/suffix trim before the DP (distance-invariant; denominator
uses the ORIGINAL lengths), rolling-row DP. Non-ASCII inputs are decoded to
code points first (byte-exact parity for UTF-8 payloads; golden vectors
include é/ß/emoji). Empty strings: ratio("","")=100, ratio("",x)=0.

Length gate (AGENT_TASK requirement): `k_fuzz_max_len` (10 000 code points per
string) and `k_fuzz_max_cells` (25 000 000 DP cells). When exceeded the kernel
returns `tool_status::too_large`; every fuzzy consumer (`find_similar`,
`best_fuzzy_match`, `find_fuzzy_match`, `find_fuzzy_block`) propagates it so
the Python shim can fall back to the pure-Python mirror.

### 3. Replace kernels (replace.py)
- `apply_edit` — `_apply_edit` (197–223) full chain: no-op on empty/same old;
  replace-all via `_apply_replace_all` (146–171, incl. `max_replacements`
  count-loop and non-overlapping `str.replace` semantics); exact single match
  returns normalized (LF) content; `match_mode=="exact"` miss → suggestion
  only; otherwise `_apply_fuzzy_fallback` (173–195): strip match →
  `best_fuzzy_match` (cutoff 75.0) with the exact
  `fuzzy-matched at {score:.0f}%: '{matched[:80]}'` suggestion text →
  `_find_similar` suggestion.
- `find_similar` — `_find_similar` (59–86): process.extractOne semantics over
  lines then line-windows (first-best tie-break, `>= cutoff`).
- `try_strip_match` — `_try_strip_match` (88–109): `old.strip()` inside any
  line, original terminator (`\r\n`/`\n`/`\r`) preserved, single splice.
- `best_fuzzy_match` — `_find_best_fuzzy_match` (111–144): single-line
  per-line scoring or multi-line window scoring, strict `>` best update,
  matched original text variant returned.

### 4. Unified-diff kernels (diff.py)
- `normalize_diff` (38–57), `normalize_create_content` (60–74).
- `parse_diff_hunks` (77–168): `_HEADER_RE` scanner (old/new start + optional
  `,count` on BOTH sides + trailing context), bare `@@` / anchor-only header
  with leading line number (`^(\d+)\b`), raw-prefix line classification
  (+/-/space/blank-context), malformed line ends the hunk leniently,
  unexpected-content error with the exact ApplyPatchError text
  (`Unexpected diff content outside a hunk: {line[:80]!r}`), no-header
  single-hunk fallback (kept for fidelity; unreachable for bodies with content
  because the scan raises first).
- `apply_diff_hunks` (276–344): bottom-up `(start_line or 0)` desc stable
  sort; exact match first (`_find_exact_matches`, empty pattern → [0]);
  multiple exact matches → exact error text; fuzzy line-window fallback
  (`_find_fuzzy_match`, `fuzz.ratio/100 >= threshold`, dominance gap
  `>= 0.05` — sort desc by double score, gap comparison on doubles exactly);
  `_infer_indent_adjustment` + `_apply_indent` + `_adjust_added_lines`
  (tab/space handling incl. the reference's dead `p_spaces && !p_spaces`
  branch, majority delta with CPython set-order tie break for the small
  int/char sets seen in practice); replacement assembly with
  context/delete/add accounting; first-changed line (1-based min);
  `split("\n")` trailing-empty semantics; trailing-newline restore.

### 5. Hashline kernels (hashline.py + hash_line.py)
- `compute_line_hash` / `compute_line_hashes` — hash_line.py 55–69 exact port:
  trailing `\r` strip, non-whitespace collection with Python `isspace` set,
  `has_significant` via the full Unicode alnum range table (same generated
  table as `src/runtime/tools/line_hash.cpp`), prev-hash seed decode,
  canonical XXH32, 2-char nibble output.
- `parse_hashline_input` (255–298 + `_parse_section_body` 128–252):
  `[path#tag]` headers (hand-rolled `_HEADER_RE`), PUT/PUT-paste/CUT/REM/MV
  line scanners, `+` body rows, `-`/junk-row rejection with exact messages,
  `***` preamble/postamble trimming, blank-line handling, register refs.
- `apply_hashline_edits` (hash_line.py 349–553): CRLF normalization; delete
  edits normalized to replace-with-empty (`_normalize_edit`);
  `validate_anchor_ref` (line bounds errors, exact-hash check, CR-stripped
  fuzzy fallback when any line contains `\r`); `_deduplicate_edits` key
  formula; overlap detection (interval math + append/prepend same-ref special
  case) with exact error text; stable bottom-up sort by
  `(-sort_line, -original_index)`; splice application incl. the `[""]`
  single-empty-line special cases for append/prepend at EOF/BOL; first-changed
  tracking; trailing-newline restore.
- `hashline_mismatch_message` — `HashlineMismatchError.__str__` (212–247):
  mismatch + ±2 context display with cumulative hashes and `>>>`/4-space
  prefixes.

### 6. Sloppy kernels (sloppy.py)
- `parse_sloppy_input` (123–137) + `_split_sections` (55–70) + `_parse_op`
  (73–120): §-section split (U+00A7 bytes), `§*` all-match flag, path
  inheritance for bare `§`, `»`-separated block rewrites, inline
  `⟪old│new⟫` (U+27EA/U+2502/U+27EB) selection scan with remaining-string
  rescan, exact error texts ("No sloppy operations found…", "Bare `§`
  requires…").
- `apply_block_op` (176–213): exact block find → fuzzy block find
  (line-window ratio ≥ 0.75, char-range conversion incl. +1 per line
  terminator) → pure-deletion trailing-newline swallow → all_match
  non-overlapping find-loop; exact "Could not locate MATCH block:\n…" /
  "MATCH block is empty." texts.
- `apply_inline_op` (216–239): flattened selections, all-match replace-all vs
  first-occurrence per selection, exact "Could not locate inline selection:
  {old!r}" text.
- `find_exact_block` / `find_fuzzy_block` exposed for focused testing.

### Not ported (stays Python, per plan)
- The conflict-marker scan (`edit_conflict.h/.cpp` in the plan) is **owned by
  `write`** per `src/builtin_tools/README.md` cross-tool ownership map; no
  conflict-marker symbols are declared here (AGENT_TASK scope item 7).
- Orchestration: path validation, VFS resolution, approval, staleness/mtime,
  snapshot stores, auto-generated guard, format checks + JSON repair,
  parse guard/auto-repair, hashline register clipboard, post-edit bookkeeping
  (plan §1, §2.4, §8 — "I/O and session state stay Python").

## Deviations / notes

1. **No third-party library required** — `fuzz_ratio` is a hand-rolled Indel
   DP, `compute_line_hash` embeds canonical XXH32 + the Unicode alnum table
   (identical to the already-ported `runtime/tools/line_hash`); no `issue/`
   report is needed.
2. **`too_large` gate**: rapidfuzz never fails; this port adds the documented
   length/cell cap (`k_fuzz_max_len` = 10 000 code points, `k_fuzz_max_cells`
   = 25 000 000 cells) returning `tool_status::too_large` so the Python shim
   can route oversized inputs to the mirror.
3. **`_infer_indent_adjustment` tie break**: Python `max(set(deltas),
   key=deltas.count)` iterates a CPython set; for the small int/char sets seen
   in practice the order is ascending by `hash & 7` (8-slot table), which the
   port reproduces via `set_slot_key`. This matches the golden vectors; the
   plan's §8 note about exact comparison operators is respected.
4. **`py_repr`**: ASCII-exact (`\\`, `\n`, `\r`, `\t`, `\xNN` controls, quote
   choice). Printable non-ASCII is passed through verbatim; non-printable
   non-ASCII repr edge cases are outside the kernel's ASCII gate (callers
   route such inputs to the Python mirror).
5. **Source encoding**: files are ASCII-only; the non-ASCII payload bytes
   (`§`, `»`, `⟪`, `│`, `⟫`, `é`, …) are written as `\xNN` escapes in string
   literals so MSVC never mis-decodes the source.
6. **`match_diff_header`**: the port skips the optional `,count` on the NEW
   side too (`@@ -1,3 +1,3 @@`); the plan's description only mentioned the
   header regex, which requires it (verified against rapidfuzz/diff.py
   goldens).

## Tests

`tests/unit/builtin_tools/test_edit_tool.cpp` — Boost.UT, main-scope
`"snake_case"_test` lambdas. 63 tests / 329 asserts, all passing:

- common helpers (6 tests): split_lf vs split_lines trailing-empty
  distinction, keepends, join/normalize variants, line-ending detection,
  trailing-newline restore, py_strip/py_repr.
- fuzz_ratio (6 tests): ASCII goldens, empty/equal, unicode (é/ß/emoji),
  long-prefix + CRLF, too_large gate.
- replace (13 tests): exact single, no-op cases, replace-all + overlap
  semantics, max_replacements, miss suggestions, CRLF normalization,
  fuzzy fallback suggestions (single + multi-line), strip-match terminator
  preservation (\n, \r\n, \r), unicode fuzzy.
- diff (19 tests): normalize, parse standard/bare/anchor/skips/lenient
  malformed/errors/blank-context, apply exact/bottom-up/stable-equal-keys/
  multiple-match error/no-match errors/fuzzy fallback/dominance gap/indent
  adjustments/trailing-newline semantics/all-add hunk.
- hashline (14 tests): compute_line_hash goldens, parse sections/ops,
  before/after/paste/MV, parse errors, apply replace/append/prepend/
  delete/noop/empty-file, mismatch message, CR-stripped fuzzy fallback,
  validation errors, overlap errors, dedupe, multi-context mismatch display.
- sloppy (8 tests): parse sections/modes, all-match + path inheritance,
  inline rescan, parse errors, block exact + deletion swallow, all-match loop,
  fuzzy block + errors, inline first/all + missing-selection error, dispatch.

## Verification

```
xmake f -m debug -y -c
xmake build kimix-llm
xmake build test_builtin_edit
./bin/debug/test_builtin_edit.exe
```
Result: `Suite 'global': all tests passed (329 asserts in 63 tests)`.
