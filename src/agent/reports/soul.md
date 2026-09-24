# KimiSoul (src/agent/soul.{h,cpp}) — parity review vs. kimi_cli/soul/kimisoul.py

Reference: `C:/dev/kimi-agent` — `kimi-cli/src/kimi_cli/soul/kimisoul.py` (2522 lines),
`soul/{__init__,stream_filter,compaction,tool_pairing,toolset,slash}.py`.
The C++ port is a deliberately minimal subset (turn loop + tool dispatch + compaction);
this note records the review of the reference's own history against that subset and what
was changed as a result.

## 1. History review (commits that touch kimisoul.py / soul/)

Newest first, with the *behaviour* each commit introduced (not just its subject line):

| Commit | Date | Behaviour |
|---|---|---|
| `43855fe` remove dead import | 09-20 | import-only |
| `eb52d6b` Fix session robust | 09-19 | moves the empty-content-block stream filter into `soul/stream_filter.py` so *every* LLM call site uses it (main step, compaction, `/btw`) |
| `db0f3a4` Update kimisoul.py | 09-19 | adds `_is_empty_content_block` / `_aiter_without_empty_parts` / `_EmptyPartFilteredStreamedMessage` / `_EmptyPartFilteredChatProvider` / `_make_empty_part_filtering_callback`; `_is_final_text_block` now skips trailing empty blocks; **disables** the text-block-continuation gate |
| `739929a` refactor, remove packages | 09-12 | `_user_input_is_empty` moves to `soul/__init__.py`; `run_soul` owns the empty-input guard |
| `7b97706` help solve empty steer message | 09-11 | **corner case**: blank user input / blank steer must not start a turn (`run`, `steer`, `request_steer` all guard) |
| `8971462` Bash null fix | 09-03 | loop-recovery prompt prose only |
| `5c1b94b` fix prompt stop | 09-03 | loop-recovery gate (`_MAX_LOOP_RECOVERY_ROUNDS`, `_make_loop_recovery_prompt`, `_synthesize_loop_recovery_text`) + `_message_has_reasoning` resets the loop detectors after a reasoned step |
| `b806631` Fix session truncate | 09-02 | toolset reminder texts; replaces the turn tool-call hard stop with a soft reminder (toolset-side) |
| `88b0331` No allow compact less than 30% usage | 09-15 | `MIN_CONTEXT_USAGE = 0.30` floor — in the *compact tool wrapper* (`src/kimix/tools/context/__init__.py`) and the compact-reminder provider, **not** in `KimiSoul.compact_context` |
| `08fe5b3` refactor session resume logic to kimisoul | 09-01 | wire replay / resume bookkeeping (Python-side) |

Also reviewed: `b3edc81`, `cd3d0c9`, `249326c`, `569c635`, `08cb7d7`, `3cac1a9`,
`2bef614`, `c8d54c2`, and the repo-wide `*corner case*` commits (`e65b0e1`, `5c50767`,
`520dd69`, `fe48c95`, `5836982`, `043c41d` — all bash/pwsh-provider work, none in soul).

## 2. What the review changed in the port

### 2.1 Empty-input guard (corner case fix, `7b97706` + `739929a`)

`KimiSoul::turn()` used to append an empty `user` message for a blank prompt and then call
the LLM, producing a spurious "you sent an empty message" turn. The reference never starts
a turn for blank input. Ported as:

* `kimix::agent::agent_user_input_is_empty()` — a port of
  `soul/__init__.py::_user_input_is_empty`'s string branch, using the *generated* Python
  `str.isspace()` table (so U+001C–U+001F, U+0085, U+00A0, U+2000–U+200A, U+2028/9,
  U+202F, U+205F, U+3000 all count as whitespace, unlike C `isspace`). Invalid UTF-8
  decodes to U+FFFD and therefore counts as content (Python `str` cannot hold it, so
  there is no reference case).
* `turn()` returns early with `TurnResult::ignored = true` — no history mutation, no LLM
  call, `steps == 0`.

Tests: `soul_blank_input_is_ignored` (9 blank shapes + a non-blank control) in
`tests/unit/agent/test_agent.cpp`. Pre-fix the assertion set fails (history grew to 20
messages and 9 LLM calls were made).

### 2.2 Compaction preserve boundary (real logic gap + corner cases)

`compact_context()` used to hand-roll the boundary: `size - adaptive_depth`, then a scan
for the most recent user message in the late half, then "walk back over leading tool
messages", then `preserve_start == 0 → 1`. `compact_tool.h` documented the reference
versions of this as *Python-owned*:

* `kimi_cli/soul/tool_pairing.py` — `message_tool_call_delta`, `balanced_cut_indices`,
  `nearest_balanced_cut_before` (never split an assistant tool call from its results), and
* `SimpleCompaction.prepare` (`compaction.py:711-772`) — the preserve-depth walk over
  user/assistant messages, the balanced-cut snap, the **Phase-6 primacy re-insertion** of
  `messages[0]` and the re-cut that follows it.

Both are now ported into `src/builtin_tools/compact_tool.{h,cpp}` and used by the soul:

* `resolve_preserve_split(messages, depth, balanced_cuts)` returns
  `{compact, preserve_start_index, keep_first_message, unbalanced, recut_fallback}`,
  which expresses both reference tail shapes (`messages[k:]` and
  `[messages[0]] + messages[k:]`) — the non-contiguous shape a single cut index cannot.
* Notable reference details that are easy to get wrong and are now pinned:
  * the walk counts **user/assistant** messages (not raw history entries);
  * the snap is `max(cut <= walked)`;
  * an out-of-range index is clamped *before* the fold, so an unbalanced history does not
    raise for it (`nearest_balanced_cut_before`);
  * the re-cut branch **reassigns** `to_preserve` from the history, silently discarding the
    primacy copy it just inserted (so `keep_first_message` must be cleared there);
  * the pathological "no balanced cut below the preserve point" fallback is unreachable in
    both implementations (cut 0 always exists below a non-zero preserve point), which is
    why `recut_fallback` is pinned as always-`false`.
* The preserve depth now comes from `options::min_preserved_turns` /
  `max_preserved_turns` (1 / 2, mirroring `LoopControl.min/max_preserved_messages`)
  instead of the kernel's wide default of 10.
* Because this build has exceptions disabled, an unbalanced history (reference: `ValueError`
  out of `prepare`) is reported as `unbalanced = true` + `compact = false` with a clear
  error, instead of aborting the turn. This is the only intentional deviation in the ported
  boundary logic.

Pre-fix, for `[u0, a0(tc), t0, u1, a1(tc), t1, u2, a2, u3]` the old code produced
`[summary, a2, u2, u3]` (no primacy copy, different boundary); the reference produces
`[summary, u0, u3]`.

### 2.3 Compaction summary message (`compaction.py:626-653`)

The port invented its own marker
(`"[system-reminder] This session is being continued from a previous conversation ..."`),
which the reference has nowhere. It now builds the reference message: role `user`, content
`<system>Previous context has been compacted. Here is the compaction output:</system>`
followed by the summary (thinking parts dropped; the C++ message carries a single content
string, so the two reference `TextPart`s are concatenated in order).

### 2.4 Auto-compaction trigger inputs (`kimisoul.py` 2c)

`should_auto_compact` was being called without `max_tokens` /
`tool_call_buffer_tokens`, so the reserved-output boundary under-fired. The soul now
passes them from `options::max_tokens` / `options::tool_call_buffer_tokens`
(mirroring `config.max_tokens` and `_tool_call_buffer_tokens()`), and the port-only
`history.size() >= 4` shortcut is documented as a no-op optimisation (a shorter history
can never be compacted anyway).

### 2.5 Empty-content-block stream filter (`db0f3a4` / `eb52d6b`) — structurally not needed

The reference filters present-but-empty `reasoning_content` / text / tool-call-argument
deltas out of the provider stream because kosong's single-`pending_part` merge chain
force-flushes on an empty part, truncating or dropping tool-call arguments. This port's
accumulator (`src/llm/openai/openai_chat.cpp`) is index-keyed and concatenates fragments,
so an empty delta is a no-op by construction. Pinned end-to-end by
`empty_deltas_do_not_truncate_tool_arguments` in
`tests/unit/llm/test_invalid_server_json.cpp`, which streams interleaved empty deltas
around split tool-call arguments through a local one-shot HTTP server and asserts the
reassembled arguments are byte-exact. The `ToolCallPart`-in-`content` branch of
`message_tool_call_delta` (unreachable in current kosong, which no longer allows a
`ToolCallPart` in `Message.content`) is pinned separately by
`tool_pairing_toolcallpart_content_is_counted`.

## 3. Reviewed and deliberately not ported

| Reference behaviour | Why not ported |
|---|---|
| Text-block continuation gate (`_is_final_text_block`, `_MAX_TEXT_BLOCK_CONTINUATION_ROUNDS`) | **disabled in the reference itself** (`db0f3a4` comments it out); the port never had it, which now matches |
| Loop detectors + loop-recovery gate (`_MAX_LOOP_RECOVERY_ROUNDS`, `_synthesize_loop_recovery_text`) | requires `KimiToolset`'s cycle/streak/different-args detectors; the C++ soul dispatches through the static `ToolRegistry` with no per-turn dedup/loop state |
| Steers (`steer` / `request_steer` / `_consume_pending_steers`) | no C++ interrupt/UI layer; note the empty-steer guard from `7b97706` is implemented for the turn entry, where the C++ soul *does* accept input |
| Dynamic injections, context pruning, notifications, verification gate, overflow recovery, session restart, wire/hooks/ledger, resume replay | Python-side orchestration around the same kernels (see `soul.h`'s "Not ported" list) |
| `MIN_CONTEXT_USAGE = 0.30` compaction floor | lives in the Python *tool wrapper* (`src/kimix/tools/context/__init__.py`), not in `KimiSoul.compact_context`; `compact_context` correctly has no floor |
| Stability / shrink checks (`SurfaceChangedError`, `CompactionShrinkError`) | need a live call-site mutation model; the token-side kernel (`compute_surface_fingerprint`) is ported and golden-tested |

## 4. Verification

* `tests/unit/builtin_tools/tool_pairing_goldens.inc` — 61 histories / 728 nearest-cut /
  610 split vectors, generated by `scripts/gen_tool_pairing_goldens.py` **from the real
  reference** (the generator cross-checks its own mirror of `prepare` against the
  reference's `to_preserve` shape on every vector, and validates its fold against
  `tool_pairing.balanced_cut_indices`). Regenerate/verify with `--check`.
* `tests/unit/builtin_tools/test_compact_tool.cpp` replays those goldens plus hand-written
  corner cases (empty history, non-positive depth, tool-only history, orphan tool result,
  dangling assistant call, primacy duplicate, the balanced snap changing the answer,
  `balanced_cuts = false`).
* `python/tests/test_parity_tool_pairing.py` drives the same kernels **live** through the
  new `runtime_py.builtin_tools.web` bindings against `kimi_cli.soul.tool_pairing` and
  `SimpleCompaction.prepare` over the golden corpus plus a fresh seeded fuzz — this is what
  caught the clamp-before-fold divergence in `nearest_balanced_cut_before`.
* `tests/unit/agent/test_agent.cpp` — `soul_blank_input_is_ignored`,
  `soul_compaction_preserves_first_message`, `soul_compaction_keeps_tool_pairs_intact`
  (each fails against the pre-fix soul; the pre-fix run was done by temporarily restoring
  the old guard / boundary / summary text and rebuilding).
* `tests/unit/llm/test_invalid_server_json.cpp` — the empty-delta end-to-end case above.

Current results: 26 C++ targets green (54 970 asserts) and `python -m pytest python/tests`
green (5 104 passed, 2 skipped, 2 xfailed).
