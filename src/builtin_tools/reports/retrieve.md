# retrieve built-in tool — C++ implementation report

Reference: `C:/dev/kimi-agent/kimi-cli/src/kimi_cli/tools/memory/__init__.py`
(122 lines: `Params` 19-33, `retrieve.attach_history_index` 50-52,
`retrieve.__call__` 55-101, `_retrieve_by_id` 103-122) +
`kimi_cli/soul/history_index.py` (`search_with_recency` 585-615,
`get_by_id` 545-583) + `kimix/retrieval.py` (`NgramTokenizer` 52-130,
`InvertedIndex` 143-437, `BM25Scorer` 658-844).

Files touched:

- `src/builtin_tools/retrieve_tool.cpp` / `retrieve_tool.h` — the fixes below
- `tests/unit/builtin_tools/test_retrieve_tool.cpp` — reference-derived golden
  replay (48 tests / 196 asserts, was 24 tests / 48 asserts; the pre-existing
  MSVC `C2975` build failure reported in `reports/tool_types.md` is gone)
- `python/tests/test_parity_retrieve.py` — new live differential harness
  (179 tests, all green), driven against the *reference tool itself*
- `src/builtin_tools/reports/retrieve.md` — this report

Harness surface (all verified to resolve to **this** checkout's build:
`runtime_py.__file__ == C:\dev\kimix-base\bin\debug\runtime_py.pyd`):

| C++ kernel (binding) | Reference | Verdict |
|---|---|---|
| `web.format_retrieve_result` | the reference `retrieve` tool driven through its own async `__call__` with a stub `HistoryIndex` | **3 bugs fixed** (marker, `{:.2f}`, repr) |
| `web.parse_turn_reference` | `HistoryIndex.get_by_id` ref parsing (`prune_` + `int()`) | **1 bug fixed** |
| `web.apply_recency_boost` + `web.sort_and_truncate` | the real `HistoryIndex.search_with_recency` body with `time.time` pinned and `search` stubbed | verified (no change) |
| `search.bm25_idf` / `bm25_score` / `bm25_topk` | `BM25Scorer._idf` / `_accumulate` / `score_topk` over postings from a real reference `InvertedIndex` | verified (bit-exact); 2 ordering deviations pinned |
| `search.freq_lower_bound` | `bin/kimix_native/search.py::_compat_freq_lower_bound` (loaded by path) | verified (413 pairs) |
| `index.NgramTokenizer` / `InvertedIndex` | `retrieval.py` with `_native_use_native` patched off, via the shim's documented composition | verified; the documented no-stop-ngram-pruning deviation pinned |
| `parse_params` / `run_retrieve` / `Retrieve` wrapper | not reachable from Python | golden replay in the Boost.UT target, every golden re-derived from the reference by `test_unexposed_kernels_match_reference_goldens` |

Commands (all green):

```
python scripts/build_locked.py -- xmake build runtime_py          -> [100%]: build ok
python scripts/build_locked.py -- xmake build test_builtin_retrieve
./bin/debug/test_builtin_retrieve.exe
  -> Suite 'global': all tests passed (196 asserts in 48 tests)
python scripts/build_locked.py -- xmake build test_builtin_param_aliases
./bin/debug/test_builtin_param_aliases.exe
  -> Suite 'global': all tests passed (76 asserts in 27 tests)
python -m pytest python/tests/test_parity_retrieve.py -q
  -> 179 passed in 1.73s          (pre-fix: 43 failed, 136 passed)
cd C:/dev/kimi-agent && .venv/Scripts/python.exe -m pytest \
    kimi-cli/tests/tools/test_memory_retrieve.py -q
  -> 18 passed                    (the reference asserts the [current] marker)
```

## Discrepancies found and fixed

1. **Every turn was missing its `[current]` marker.** memory:92/114 build
   `marker = " [compacted]" if r.get("is_compacted") else " [current]"`; the
   port appended `" [compacted]"` for compacted turns and *nothing* otherwise.
   Repro (1 turn, `score=0.12345`, not compacted):
   Python `> **user** [current] (relevance: 0.12)\n> hello world`,
   C++ `> **user** (relevance: 0.12)\n> hello world`.
   Fixed in `retrieve_format_turns` / `retrieve_marker` (both modes; the
   reference's own `test_id_fetch_plain` asserts `"[current]" in output` and
   `test_compacted_marker` asserts `"[current]" not in output`).

2. **`f"{score:.2f}"` was emulated with an epsilon-based cents rounder, which
   disagrees with CPython's correctly-rounded formatting for values whose
   nearest double sits just above/below a `x.xx5` tie.** Repro: 0.005 → Python
   `0.01` / C++ `0.00`; 0.015 → `0.01` / `0.02`; 0.025 → `0.03` / `0.02`;
   2.675 → `2.67` / `2.68`; −0.001 → `-0.00` / `0.00`. Fixed by deleting
   `retrieve_round_cents` and using `kimix::format("{:.2f}", score)` — the same
   conclusion already documented in `job_output_tool.cpp:67` and
   `python_tool.cpp:381`. Verified over the goldens plus 2000 seeded doubles
   and a 3000-value ad-hoc sweep (0 mismatches; 6 before).

3. **The `id` in `id={ref!r}` was interpolated raw, not `repr`'d.** header
   comment of the port itself promised `f"id={ref_id!r}"`. Repro:
   `id="it's"` → Python `No turn found with id="it's".` (repr switches to double
   quotes) / C++ `No turn found with id='it's'.`; `id="a\\b"` → Python
   `'a\\\\b'` / C++ `'a\b'`; `id="a\nb"` → Python `'a\nb'` (escaped) / C++ a real
   newline; `\x01`/`\x7f` escaped by Python, raw in C++. Fixed with
   `retrieve_py_repr` (same contract as `edit_tool.h:80 py_repr`,
   `run_tool.h:198 py_repr_char`), used by the id header, the not-found text and
   `run_retrieve`'s `Found turn id=…` message.

4. **`parse_turn_reference` was a `std::from_chars` parse, not Python's
   `int()`.** The reference strips `prune_` and calls `int()` (and the shim's
   `_parse_ref` mirror does the same). Repro: `"+42"` → Python `42` / C++
   `None`; `" 42"`, `"42 "`, `"\t42\n"` → `42` / `None`; `"1_0"` → `10` /
   `None`; `"prune_+42"` → `42` / `None`. Fixed: Py_UNICODE whitespace strip +
   optional sign + ASCII digits with single `_` separators, with an int64
   overflow guard (out-of-range refs cannot be turn ids → -1). `-1` stays the
   "unparsable" sentinel, and the binding maps every negative parse to `None` —
   harmless, because turn ids are never negative.
   Still ASCII-gated: Python also accepts non-ASCII decimal digits
   (`int("１２") == 12`); pinned in `test_turn_reference_ascii_gate_is_documented`.

5. **A blank/missing query was reported as an error instead of the reference's
   guidance `ToolOk`.** Python returns
   `ToolOk(output="No query provided. Pass a `query` string or an `id`.", message="No query")`
   (memory:70-74) and `Params()` (nothing at all) reaches the same path; the
   port returned `invalid_input` + a message with no backticks and an *empty*
   `output` (an error-shaped tool result). Fixed: `parse_params` no longer
   treats "no query" as an error (`retrieve_params.query` stays empty) and
   `run_retrieve` returns `ok` / `"No query"` / the exact guidance text. The
   blank test now uses Python's `str.strip()` whitespace set (so `"\u3000"` and
   `"\xa0"` are blank), not `std::isspace`.

6. **`bool(output)`-shaped envelope for the two "no result" shapes.** Python
   returns `ToolOk` for both `No matching results found in conversation
   history.` and `No turn found with id='prune_999'.` (`is_error == False`); the
   port returned `tool_status::not_found` for the id miss, i.e. `ok:false` in the
   tool envelope. Fixed: `run_retrieve` now reports `ok` for both (message
   `"No results"`), so every outcome of the tool body matches the reference's
   `ToolReturnValue`; only params validation is an error. Deliberate exception,
   documented in the header: with **no view injected** the wrapper still answers
   `unsupported` so the Python shim can fall back to its own mirror (the
   reference's "No history index attached" guidance is a different situation —
   it always has an index object).

7. **`id=""` (and any present-but-empty id) was treated as absent.** Python
   tests `params.id is not None`, so `id=""` goes to `_retrieve_by_id("")` and
   prints `No turn found with id=''.`; the port dropped the empty id and fell
   into the query path. Fixed in `parse_params`. (The `format_retrieve_result`
   binding keeps `ref_id=""` → search mode; that binding-level ambiguity is
   pinned by `test_empty_ref_id_is_search_mode_in_the_binding` and by the C++
   golden `run_retrieve_empty_id_is_an_id_lookup`.)

Two smaller param-validation divergences were fixed at the same time (both
verified against pydantic directly): a present `query: null` is a validation
error (`Input should be a valid string`) rather than "absent", and `k` accepts
pydantic's lax coercions (`"7"` → 7, `true` → 1, `5.0` → 5, `5.7` → error).

## Verified as correct (looked suspicious, settled by evidence)

- **Recency boosting is duplicated by design and still exact.**
  `run_retrieve` fetches `k*3` candidates and applies
  `score * (1 + 1.0 * exp(-hours/24))` + stable descending sort + truncate to
  `k`; the reference does exactly that inside `search_with_recency`, calling
  `self.search(query, top_k=k*3)` first. Differential test drives the *real*
  reference method (with `time.time` patched and `search` stubbed) against
  `web.apply_recency_boost` + `web.sort_and_truncate` over 7 turn sets × 4
  `k` values × 5 weights: identical `(turn_id, score, boosted_score)`
  sequences, bit-for-bit. Requirement this pins: the shim must bind the view's
  `search_with_recency` to the **unboosted** `HistoryIndex.search`, otherwise
  the boost (and the pool) would be applied twice.
- **BM25 is bit-exact, including the `+0.5` smoothing.** `bm25_idf` ==
  `BM25Scorer._idf` for N ∈ {0,1,2,10,100,1000,50000} × df ≤ 2N+3 (including
  the negative-idf regime, df > N — there is **no clamp** in the reference);
  `bm25_score` == `BM25Scorer.score` elementwise-identical (not just
  approximate) for every doc over 10 queries, and `bm25_topk` ==
  `score_topk` for the same corpus. The kernel's `q_weight == 1` model needs the
  caller to deduplicate query tokens, which `Searcher.search` does
  (`unique_query = list(dict.fromkeys(...))`) — pinned by
  `test_bm25_query_weight_contract` (the repeated-token path is *not*
  bit-identical, so the dedupe is load-bearing).
- **No snippet windowing / elision / per-turn or total caps exist in this
  tool.** The reference quotes the whole turn text with only
  `text.replace("\n", "\n> ")`; there is no folding, no `k` clamp beyond the
  pydantic `ge=1/le=10` bound, and no digest. Only `HistoryIndex._MAX_TURNS =
  500` caps anything, and that is Python-side.
- **The n-gram tokenizer's composition.** The kernel lowercases ASCII only and
  never strips; the shim composes `normalize() -> strip() ->
  detect_n/tokenize`, and that composition matches `retrieval.py` exactly for
  the ASCII corpus (14 texts × n ∈ {1,2,3} + auto-detect). `IndexInverted`
  doc stats (`doc_count`, `max_doc_id`, `doc_length`, `sum_doc_lengths`,
  `avg_doc_len`) and every postings list for the terms the reference keeps are
  identical.

## Known gaps / pinned deviations

- **Non-ASCII decimal digits** in `parse_turn_reference` (see fix 4) and
  **non-printable non-ASCII code points** in the repr (Python escapes U+00A0,
  U+200B, lone surrogates as `\xNN`/`\uNNNN`; bytes pass through here) — the
  project-wide ASCII gate, shared with `py_repr`/`py_repr_char`.
- **`bm25_topk` order when `top_k` ≥ the number of nonzero scores**: the
  reference's dense branch returns `np.flatnonzero` order (ascending doc id)
  while the kernel always returns (score desc, doc asc) — the ordering its own
  sparse `heapq.nlargest(key=(score, -doc_id))` path and its `top_k >= N`-dense
  siblings use. Same documents, different order, and the pipeline re-ranks by
  boosted score immediately afterwards. Pinned by
  `test_bm25_topk_dense_branch_matches_reference`.
- **Ties in the reference's dense `score_topk` branch are order-unstable**
  (`np.argpartition` + `np.argsort(-score)`), so the differential test compares
  tie sets and asserts exact order only when the scores are distinct; the kernel
  keeps the deterministic doc-ascending tie-break.
- **`InvertedIndex` stop-ngram pruning is not ported** (documented in
  `inverted_index.h`); the test pins that the reference prunes df > N/2 and
  pure-punctuation n-grams while the native index keeps them.
- **`runtime_py.index.HistoryIndex` (search/get_by_id/persistence) was not
  re-diffed here** — the C++ retrieve tool never calls it (search is injected),
  and `python/tests/test_history_index.py` already covers it against the
  reference. Likewise the FTS5/unicode61/trigram/LIKE routing and
  `sanitize_fts5_query` stay Python-side by design.
- **`format_retrieve_result(ref_id="")`** means search mode (binding default);
  `id=""` cannot be expressed through that binding entry point.
