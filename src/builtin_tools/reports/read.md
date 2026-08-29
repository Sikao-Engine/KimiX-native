# read — C++ kernel port (implementation report)

Tool: `read` (namespace `kimix::builtin_tools::read`)
Files: `src/builtin_tools/read_tool.h` + `src/builtin_tools/read_tool.cpp`
Tests: `tests/unit/builtin_tools/test_read_tool.cpp` — 44 tests, ~220 asserts, all passing
Plan: `C:/dev/kimi-agent/plans/read.md` (§3 phases 1 & 3, §7, §8)
Registration line (already present in `tests/xmake.lua`; local verification only, not committed):

```lua
builtin_tools_test("test_builtin_read", "unit/builtin_tools/test_read_tool.cpp")
```

## What was ported (function-by-function mapping)

| C++ kernel (read_tool.h) | Python source of truth | Lines |
|---|---|---|
| `validate_int_option` | `read.py::Params._validate_value` | 278–294 |
| `truncate_line_read` | `kimi_cli/tools/utils.py::truncate_line` (the variant read.py imports: `"..."` marker, trailing line-break preserved, budget raised to `len(marker+linebreak)`) | 113–126 |
| `split_lines` | `KaosPath.read_lines(errors="replace")` semantics: LF / CRLF / lone-CR line endings normalized to a single `\n`; invalid UTF-8 → U+FFFD | — |
| `render_forward` | `read.py::_render_forward` | 1528–1578 |
| `render_tail` | `read.py::_render_tail` | 1580–1648 |
| (internal) `rd_render_result` | `read.py::_render_result` (byte-exact message incl. `f"{line_no:6d}\t"`, Python list repr `[1, 3]`, pluralization) | 1650–1697 |
| `apply_char_window` | `read.py::_apply_char_window` (head/middle/tail NOTE strings byte-exact) | 349–385 |
| `compute_line_hashes` / `compute_line_hash_strings` | `hash_line.py::compute_line_hash` + `_cumulative_hashes` (chained xxHash32, nibble seed decode, Python `str.isspace`/`str.isalnum` sets) | 55–109 |
| `line_hash_independent` + `collapse_repeated_lines` | read's dedup-mode repeated-line collapse helper (`"line  (N repeats)"` marker style, contract per `runtime/tools/line_hash.h` re-implemented locally) | — |
| `render_cpu_profile` | `read_profiles.py::render_cpu_profile` (+ `_parse_cpu_profile`, `_build_cpu_tree`, `_compute_self_times`, `_aggregate_totals`, `_promote_root`, `_prune_hot_tree`) via vendored yyjson (`yyjson_read_opts`, zero-copy input, mimalloc allocator, no exceptions) | 31–261 |
| `render_sample_profile` | `read_profiles.py::render_sample_profile` (+ `_parse_frame_text`, `_demangle_symbol`, `_is_wait_frame`, decorator stack reconstruction) | 263–430 |
| `markdown_to_text` | `read_markit.py::markdown_to_text` (the nine regex passes ported as a deterministic scanner: fenced code → `[code block: N lines]`, inline-code placeholders, `**`/`*`/`__`/word-bounded `_` emphasis, links, images, headings, `---` rules, blank-run collapse, final `strip()`) | 215–254 |
| `Read` (Tool subclass) | `read.py::ReadFile.__call__` / `_read_content` / `_read_single_file` (native side of the CallableTool2 boundary; Python still owns I/O, safety, rich-format routing) | 627–1779 |

Constants mirrored from read.py: `MAX_LINES=5000`, `MAX_LINE_LENGTH=4000` (code points),
`MAX_BYTES=100<<10`, `MAX_FILES=32`, `MAX_PROFILE_SUMMARY_BYTES=32 MiB`.

## Tool-class wrapper (new in this reconciliation)

* `class Read : public kimix::builtin_tools::Tool` is declared in `read_tool.h` and
  implemented in read_tool.cpp.  It is the CallableTool2-style binding entry
  point that the Python shim calls with already-resolved bytes/metadata.
* Expected input JSON parameters:
  * content (string, required) — file bytes/text to render.
  * display_path (string, required) — path shown in messages.
  * mode (string, optional) — "text" (default), "markdown", "cpu_profile",
    "sample_profile".
  * offset, limit, max_char, char_offset (int, optional) — text-mode budgets.
  * show_line_numbers (bool, optional, default true).
  * note (string, optional) — appended to the message in text mode.
* Output JSON fields:
  * status: "ok" | "invalid_input" | "unsupported".
  * output, message, brief: "Read file".
  * For "ok": start_line, total_lines, max_lines_reached, max_bytes_reached,
    end_of_file, truncated_line_numbers.
* Validation errors reuse validate_int_option so the byte-exact Python
  ValueError messages are returned in the JSON message field.

## JSON allocation reconciled to the project-wide allocator

* `render_cpu_profile` now uses `kimix::llm::kYYJsonAlcMi` (defined in
  src/llm/yyjson_alc.h) instead of a private rd_yyjson_alc.  This matches the
  yyjson skill convention and guarantees all yyjson allocations route through
  the shared mimalloc heap.

Goldens were generated directly from the Python reference
(`_render_forward`/`_render_tail`, `_apply_char_window`, `truncate_line`,
`hash_line._cumulative_hashes`, `render_cpu_profile`, `render_sample_profile`,
`markdown_to_text`) and pinned byte-exactly in the tests, including realistic
inline `.cpuprofile` JSON fixtures.

## Performance properties

* Forward render: single pass, no per-line encode/alloc for the byte budget
  (byte length = truncated UTF-8 length, computed without a copy).
* Tail render: bounded `kimix::deque` window with O(1) `pop_front` — the
  reference's `list.pop(0)` was O(n²); behavior is identical (verified by a
  stress test against the reference model for windows 1/5/36/37/50).
* CPU profile: iterative post-order total aggregation + explicit DFS stack for
  hot-tree pruning (no deep recursion on large profiles).

## Deliberately left in Python (plan’s “Stays Python” justification)

Quoted from plans/read.md §3:

> * “Archives (read_archive.py): zipfile/tarfile/gzip/bz2/lzma are C-backed
>   stdlib; no archive library is vendored in kimix-base; the work is
>   I/O-bound.”
> * “SQLite (read_sqlite.py): apsw is already a C library; row sets are tiny
>   (≤1000 rows) and the ASCII table renderer is cheap next to SQL execution.”
> * “Document extraction (read_extract.py/read_markit.py document branches):
>   backed by third-party parsers (nbformat/python-docx/openpyxl/xlrd/
>   python-pptx/PyMuPDF); … html_to_text stays (markdownify).”
> * “PDF page rendering (read_pdf_pages.py): PyMuPDF + image compression
>   pipeline is already native.”

In kimix-base none of those third-party extractors exist in `src/ext`, and the
task rules forbid vendoring new libraries — see `issue/read.md` for the exact
blocker list. Path resolution/VFS/session/conflict-history logic also stays
Python per the plan (app/session logic).

Not implemented here by ownership rule: conflict-marker scanning
(`conflict_scan`) is owned by the write agent (README cross-tool ownership map).

## Deviations from the plan / reference (all recorded, tests pin the reference)

1. **Tail window container** — the plan suggests `kimix::ring_buffer`, but that
   type exposes no iteration; a `kimix::deque` window (O(1) `pop_front`) is
   used instead. Observable behavior is identical and O(n).
2. **Markdown images with alt text** — the reference applies the *link* pass
   before the *image* pass, so `![img](url)` becomes `!img (url)`; only an
   empty-alt image `![](url)` reaches the image pass (`[image: url]`). The
   plan prose (“images [image: url]”) applies to the empty-alt case only; the
   Python reference is authoritative and the test pins both cases.
3. **hitCount-only `.cpuprofile`** — the Python reference crashes on this
   shape (`read_profiles.py:213` reads `n.hitCount` on a `dict`); the C++ port
   implements the clearly intended `dict.get("hitCount")` behavior (test
   `cpu_profile_hitcount_fallback`).
4. **`end_of_file` with a byte-budget stop** — kept exactly as the reference:
   `end_of_file = len(entries) < n_lines` is true even when the byte budget
   (not EOF) stopped the read (test `render_forward_byte_budget`).
5. **`compute_line_hashes` tail convention** — matches the runtime bulk kernel
   (plan 013): lines split on `\n`, trailing empty element after a final `\n`
   dropped. Golden for `"a\r\nb\r\n"` is `["RW", "YW"]`.
6. **`\w` approximation in markdown emphasis lookarounds** — Unicode word
   characters are taken as L*/N*/Pc (Python regex `\w` also includes Mn/Mc
   marks); exact for ASCII/CJK/European text, per the plan’s ASCII-fast-path
   convention.

## Windows build notes

* Files with non-ASCII content carry a UTF-8 BOM; MSVC in a Chinese-locale
  environment otherwise decodes them as GBK and breaks the byte-exact strings.
* The inline JSON fixtures use `R"json(...)json"` delimiters because the JSON
  itself contains `)"` (e.g. `"(root)"`), which terminates a plain `R"(...)"`
  raw string early.
