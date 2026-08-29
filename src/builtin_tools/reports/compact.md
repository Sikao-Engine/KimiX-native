# Compact Tool — Implementation Report

## Ported to C++

All CPU-bound / decision kernels from `plans/compact.md` §3 are implemented in
`src/builtin_tools/compact_tool.h` and `src/builtin_tools/compact_tool.cpp` in
namespace `kimix::builtin_tools::compact`.

- Message representation (`message`, `content_part`) and helpers (`extract_text`, `has_think_part`, `is_user_or_assistant`).
- `CompactMode` / `compaction_options` / `parse_compact_mode` / `mode_guidance`.
- `should_auto_compact` — auto-trigger budget logic with saturating `int64_t` arithmetic.
- `adaptive_preserve_depth` — last-turn heuristic preserve depth (ASCII-only).
- `detect_cascade_depth` — counts compaction-summary markers.
- `build_compaction_prompt` — prompt text assembly (cascade selection, mode guidance, decision sections, custom instruction).
- `build_compact_message_text` — legacy flattened compact message text.
- `prepare_compaction_input` — history slicing + prompt/compact_message assembly.
- `compute_surface_fingerprint` — cheap surface snapshot with injectable token counter.
- `estimate_text_tokens` / `estimate_message_tokens` — language-aware token heuristic.
- `Compact` — thin `kimix::builtin_tools::Tool` subclass for Python binding integration.

## Stayed in Python

- LLM summarization, durable ledger I/O, stability/shrink checks, todo injection formatting.
- Soul / session state, context-overflow force-compact loop, provider overflow marker classification.
- Balanced tool-pairing boundary computation.
- Exact tiktoken counting.
- Pydantic validation and tool orchestration (`src/kimix/tools/context/__init__.py`).
- Compaction prompt prose source of truth (`kimi-cli/src/kimi_cli/prompts/compact.md`, `compact_cascade.md`).

## Deviations

- ASCII-only lowercasing in `adaptive_preserve_depth`; relevant keywords are ASCII.
- Token estimation is heuristic-only; injectable counter lets Python supply tiktoken counts.
- `prepare_compaction_input` assumes the caller already balanced `preserve_start_index`.
- Non-text/non-think content parts collapse to `type == "other"` with empty text.
- `todos_max_items` is carried but not enforced by C++ kernels.

## Tests

`tests/unit/builtin_tools/test_compact_tool.cpp` covers the golden vectors from
`plans/compact.md` §6 using Boost.UT.

Target name: `test_builtin_compact`.

## Verification

- `python scripts/check_cpp_syntax.py src/builtin_tools/compact_tool.h` — passed
- `python scripts/check_cpp_syntax.py src/builtin_tools/compact_tool.cpp` — passed
- `python scripts/check_cpp_syntax.py tests/unit/builtin_tools/test_compact_tool.cpp` — passed
