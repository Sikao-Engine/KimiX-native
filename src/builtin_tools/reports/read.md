# read — C++ kernel port (implementation report)

Tool: `read` (namespace `kimix::builtin_tools::read`)
Files: `src/builtin_tools/read_tool.h` + `src/builtin_tools/read_tool.cpp`
Tests: tests/unit/builtin_tools/test_read_tool.cpp — 56 tests, 3040 asserts, all passing
Goldens: scripts/gen_read_goldens.py -> tests/unit/builtin_tools/read_goldens.inc
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

## Golden rule of this port (differential harness — scripts/gen_read_goldens.py)

Every expectation in the test file is *generated*, never transcribed:
`scripts/gen_read_goldens.py` imports the real reference from the kimi-agent
checkout (`kimi_cli.tools.file.read` / `hash_line` / `read_profiles` /
`read_markit`, `kimi_cli.tools.utils.truncate_line` plus the real Python
text-mode reader for universal-newline splitting) and writes
`tests/unit/builtin_tools/read_goldens.inc`. The file is pure ASCII by
construction (every byte outside printable ASCII is a 3-digit octal escape, so
no BOM and no MSVC code-page surprises) and every literal is chunked below the
MSVC 16 KiB string-literal limit. `python scripts/gen_read_goldens.py --check`
verifies the checked-in file is up to date.

* `_render_forward` / `_render_tail` bookkeeping (`total_lines`,
  `max_lines_reached`, `max_bytes_reached`, `end_of_file`, truncated line
  numbers) is captured by wrapping `ReadFile._render_result` and recording the
  keyword arguments read.py itself passes — no re-derivation in the generator.
* `_apply_char_window` is applied to the reference render result exactly like
  `_read_as_text` does, so `rd_tool_goldens` covers split_lines + render + char
  window + message composition end to end through `Read::operator()`.
* Large corpora (MAX_LINES / MAX_BYTES) are described by a small recipe
  (`repeat`, `numbered`) that the test expands; the generator asserts the
  expansion equals the corpus and the test asserts the total input/window byte
  length, so a recipe drift cannot silently weaken the comparison.
* Deterministic fuzz corpora (fixed seed) cover split_lines (including invalid
  UTF-8), forward/tail render, char windows, line hashes, markdown, and ~65
  random V8/macOS profile shapes.

Bugs the harness found (all silent divergences) and the fixes

1. `markdown_to_text` inline-code pass: a lone backtick dropped everything
   between the last emitted position and that backtick (`" \n\n9line ` "` lost
   `"9line "`). Now emits `[pos, open + 1)` instead of only the backtick.
2. Markdown bold `**` / `__`: after a failed attempt the scanner advanced two
   characters, losing a match that starts one character later
   (`"***a**"` -> `"***a**"` instead of `"*a"`, `"***both***"` -> `"**both**"`
   instead of `"both"`, `"___a__"` unchanged instead of `"_a"`). Now advances
   one character, like the regex engine.
3. Markdown links/images accepted an empty URL (`"[a]()"` -> `"a ()"`,
   `"![]()"` -> `"[image: ]"`); `[^)]+` requires one character. Now requires
   `paren_end > close + 2`.
4. Markdown headings were processed line by line, but `\s*` in `^#+\s*(.+)$`
   matches `'\n'` and `#+` backtracks. Reference behaviour: `"# \nx"` -> `"x"`,
   `"a\n####\nb"` -> `"a\nb"`, `"####"` -> `"#"`, `"##\n"` -> `"#"`,
   `"text\n#\nmore"` -> `"text\nmore"`, `"#\n"` unchanged. Pass 9 is now a
   faithful backtracking matcher (greedy `#+`, greedy `\s*` stepping over code
   points, `(.+)` to the end of the line it starts on).
5. `rd_is_space_cp` was missing U+001C..U+001F, which Python's `str.isspace()`
   (and `re`'s `\s` for str patterns, and `str.strip()`) treat as whitespace —
   wrong line hashes for lines containing them and wrong heading/`strip()`
   handling in markdown_to_text. Added.
6. `rd_decode` (errors="replace") emitted one U+FFFD per byte of an incomplete
   sequence; CPython emits one for the whole *maximal subpart*
   (`b"\xe6\xb1"` -> 1 replacement, not 2). Now consumes the valid prefix.
7. `render_cpu_profile` root promotion treated any non-int `"root"` value as
   "promote by functionName", while `_promote_root(node_map, root_id)` only does
   that when the key is *absent* or JSON-null; a present-but-unfindable root
   selects the "top self-time functions" hot-path branch
   (`{"root": "1"}` with a `(root)` node is now byte-identical). An integral
   float root (`1.0`) resolves like Python's `dict.get` does.
8. Markdown inline-code placeholder: the literal `"\x00CODE"` is a *greedy hex
   escape*, so the C++ token was `"\x0CODE{n}"` (0x0C + "ODE") instead of
   Python's `"\x00CODE{n}\x00"`. Both insert and lookup used the same wrong
   token, so plain `` `code` `` worked, but 0x0C *is* Python whitespace while
   NUL is not: a heading or a strip next to a placeholder then ate one extra
   character (`"#` + "`x`"` produced `"ODE0"` instead of `"x"`). Fixed with
   `rd_inline_code_token()`, which appends real bytes.
9. `rd_alnum_ranges` (the generated `str.isalnum` table) was corrupt: 91 of its
   values were missing, the first at cp U+066F, so every later `(start, end)`
   pair was shifted and the binary search reported *true* for huge fake ranges
   (e.g. all of U+1EEBC..U+20000, i.e. the emoji planes). Consequences: line
   hashes used seed 0 instead of `line_num` for lines whose only significant
   characters are non-alphanumeric (`"🌍\n"`), and markdown `\w` lookarounds
   treated emoji as word characters. The table is now regenerated from
   `unicodedata` by `python scripts/gen_read_goldens.py --write-alnum-table`
   (markers `BEGIN/END GENERATED:RD-ALNUM-TABLE`).
   **The identical corruption exists in the runtime twin
   `src/runtime/tools/line_hash.cpp` (`kAlnumRanges`) - reported, not touched
   here because that kernel belongs to the runtime_py work stream.**

Performance properties

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
3. **hitCount-only `.cpuprofile`** — the Python reference raises
   `AttributeError` on this shape (`read_profiles.py:213` reads `n.hitCount` on
   a `dict`) and its fallback gate is truthiness-based
   (`all(... n.get("hitCount") ...)`, so one node with `hitCount: 0` disables
   the fallback). The C++ port implements the clearly intended
   "every node carries an int `hitCount`" semantics. Those inputs are recorded
   in `rd_cpu_python_raises` (the documented reference crash) while their
   expected summary comes from a source-patched copy of `read_profiles.py` that
   applies exactly those two intent fixes — see `scripts/gen_read_goldens.py`.
4. **`end_of_file` with a byte-budget stop** — kept exactly as the reference:
   `end_of_file = len(entries) < n_lines` is true even when the byte budget
   (not EOF) stopped the read (test `render_forward_byte_budget`).
5. **`compute_line_hashes` tail convention** — matches the runtime bulk kernel
   (plan 013): lines split on `\n`, trailing empty element after a final `\n`
   dropped. Golden for `"a\r\nb\r\n"` is `["RW", "YW"]`.
6. **`\w` approximation in markdown emphasis lookarounds** — `read_markit.py`
   does `import regex as re`, so its `\w` is the *regex* module's `\p{Word}`
   (UTS#18): L*, N* except No, Mn/Mc/Me, Pc, the join controls U+200C/U+200D and
   even unassigned code points are word characters, while `No` characters such
   as U+00B2 are not. The port keeps `str.isalnum()` + `_` + Pc, which is exact
   for ASCII/CJK/European text (including the intraword-underscore cases, covered
   by golden cases using U+203F/U+2040) but still differs for combining marks and
   the join controls. Pinned as a known approximation, not a silent one.

## Windows build notes

* Files with non-ASCII content carry a UTF-8 BOM; MSVC in a Chinese-locale
  environment otherwise decodes them as GBK and breaks the byte-exact strings.
* `tests/unit/builtin_tools/read_goldens.inc` is generated and pure ASCII by
  construction (3-digit octal escapes) — no BOM needed, and the escapes can
  never glue onto a following hex digit.
* The inline JSON fixtures use `R"json(...)json"` delimiters because the JSON
  itself contains `)"` (e.g. `"(root)"`), which terminates a plain `R"(...)"`
  raw string early.
