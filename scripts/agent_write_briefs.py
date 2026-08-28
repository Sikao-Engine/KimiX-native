#!/usr/bin/env python3
"""Write AGENT_TASK.md into each per-tool worktree (tool-specific scope)."""
from __future__ import annotations

from pathlib import Path

WT = Path("C:/dev/kimix_wt")

COMMON = """\
# TASK: port the `{tool}` built-in agent tool kernels to C++ (kimix-base)

You work in the isolated git worktree **C:/dev/kimix_wt/{tool}** (branch
`agent/{tool}`). Everything is already wired: `src/builtin_tools/*` compiles
into the `kimix-llm` static lib and `tests/xmake.lua` has a
`builtin_tools_test(name, source)` helper at the end of the file.

## Read these FIRST (in this order)
1. `C:/dev/kimix-base/src/builtin_tools/README.md` — shared conventions,
   namespaces, unity-build rules, available vendored libs, ownership map,
   build/verify commands. **Hard requirements.**
2. `C:/dev/kimi-agent/plans/{plan}` — the design. §3 (C++ design), §7 (tests),
   §8 (risks) are the important parts; the §Source-of-truth block lists the
   Python files + line ranges you must mirror.
3. `C:/dev/kimix-base/.agents/skills/cpp/SKILL.md`,
   `.agents/skills/test/SKILL.md`, `.agents/skills/xmake/SKILL.md`.
4. Existing reference kernels for style:
   `C:/dev/kimix-base/src/runtime/tools/*.h/.cpp` (grep_pattern, shell_safety,
   security, compress) — same porting discipline (ASCII gate, byte-exact
   messages, no Python includes).

## Deliverables (exact paths, relative to your worktree root)
- `src/builtin_tools/{file}.h` + `src/builtin_tools/{file}.cpp`
  (namespace `kimix::builtin_tools::{tool}`)
- `tests/unit/builtin_tools/test_{file}.cpp` (Boost.UT, main-scope `_test`
  lambdas, covers the plan's §7 list with real behavioural assertions)
- `src/builtin_tools/reports/{tool}.md` — implementation report: what you
  ported, function-by-function mapping to the Python reference, what you left
  in Python and why (quote the plan's justification), deviations, test counts.
- If (and only if) some part is impossible with the libraries vendored in
  `src/ext`: `issue/{tool}.md` — the blocker report. **Never vendor a new
  library, never edit `src/ext/**`, `xmake.lua`, `src/xmake.lua`,
  `src/core/**`, or another tool's files.**

## Scope for THIS tool
{scope}

## Workflow
1. `cd C:/dev/kimix_wt/{tool}`
2. Read the files above. Design the header API before writing code.
3. Implement header + cpp. Add your test registration line ONLY inside the
   marker block at the end of `tests/xmake.lua`
   (`-- >>> BEGIN builtin_tools test registrations ... >>>`):
   `builtin_tools_test("test_builtin_{tool}", "unit/builtin_tools/test_{file}.cpp")`
   — this line is for local verification; **do NOT commit it** (the integrator
   collects all registrations afterwards).
4. Build + run until green:
   `xmake f -m debug -y -c` (once), then
   `xmake build kimix-llm`, `xmake build test_builtin_{tool}`,
   `./bin/debug/test_builtin_{tool}.exe`
   (On Windows build one target per command. If unity/PCH complains, use
   `xmake build -r <target>`.)
5. Fix code, not tests — but if the plan and the Python reference disagree,
   follow the **Python reference** and record the deviation in your report.
6. After the commit, restore the shared build file so your tree stays clean:
   `git checkout -- tests/xmake.lua` and confirm `git status --short` shows
   nothing but the untracked `AGENT_TASK.md`.
7. Commit ONLY your deliverable files:
   `git add src/builtin_tools/{file}.h src/builtin_tools/{file}.cpp \\
     tests/unit/builtin_tools/test_{file}.cpp src/builtin_tools/reports/{tool}.md issue/{tool}.md`
   (omit paths that do not exist; never `git add -A`), then
   `git -c user.name=kimix -c user.email=kimix@local commit -m "{tool}: port builtin tool kernels to C++"`
   and `git add AGENT_TASK.md` must NOT happen — leave it untracked.
8. Report back: worktree path, branch, commit hash, file list, test/assert
   counts, blockers.

{extra}
"""

TOOLS = {
    "glob": dict(
        plan="glob.md", file="glob_tool",
        scope="""
Implement §3.1 + §3.2 + §3.3 of the glob plan as ONE self-contained pair
(`glob_tool.h/.cpp`) — do not touch `src/runtime/glob`:
- `fnmatch` core (`fnmatch_match(pattern, name, / flags)`): Python `fnmatch`
  semantics incl. `*`, `?`, `[seq]`, `[!seq]`, escaped chars, and the
  Windows case-insensitivity rule. Port it from scratch (the runtime copy is
  in another target you cannot link).
- `path_glob_pattern` parsing: `is_unsafe_recursive_pattern`, pattern split into
  segments, `**` handling, anchored vs basename matching,
  `parse_pattern` / `match_path` per plan §3.2 (incl. the recursive-only-pattern
  rejection and the "no `/` in basename pattern" rule).
- A native directory walker (`walk_matches`) driven by an injectable
  entry-listing callback (`kimix::function` or a small `walk_source` interface)
  so tests use an in-memory tree — plus a real-filesystem convenience wrapper
  (Win32 `FindFirstFile`/`FindNextFile` on Windows, `opendir/readdir` on POSIX)
  compiled under `KIMIX_PLATFORM_WINDOWS` / else.
- gitignore-ish ignore filter as pure string kernels: `ignore_rule_match`
  (negation `!`, directory-only trailing `/`, anchored leading `/`,
  basename-only rules) reusing your fnmatch.
- Result shaping used by the Glob tool: mtime top-k ordering (caller supplies
  mtime per entry), dedup, slash normalization, prefix strip, `head_limit` /
  `offset` pagination, and the "N paths with no match" summary builder.
All of this needs no third-party library: **no issue/ report expected.**""",
        extra="""
NOTE: `kimix::hash64` is XXH3 (see core/stl/hash.h) — do not use it where the
plan demands byte-exact `xxhash.xxh64` output; call `XXH64` from the vendored
`xxhash.h` (available via `kimix-core` public include dirs) instead.
""",
    ),
    "grep": dict(
        plan="grep.md", file="grep_tool",
        scope="""
Implement the **Phase A/B-CPU** string kernels of the grep plan in ONE pair
(`grep_tool.h/.cpp`), organised in clearly separated sections:
1. selectors (`grep_selectors.py`): `parse_line_range_chunk`,
   `parse_line_ranges`, `is_line_in_ranges`, `selector_line_ranges`,
   `split_path_and_sel` (pure grammar; filesystem probe injected as a
   `kimix::function<bool(kimix::string_view)>`), `expand_path_entries`,
   `merge_ranges_into`. Error messages must match the Python `ValueError`
   texts byte for byte ("lines are 1-indexed", "end must be >= start",
   "count must be >= 1").
2. archive (`grep_archive.py` pure parts): `parse_archive_path_candidates`,
   `is_archive_path`, `safe_scratch_name`, `remap_display`.
3. output (`grep_output.py` + content-line parsing): `parse_content_line`
   (exact `^(.*?)([:\\-])(\\d+)\\2(.*)$` semantics, leftmost delimiter pair),
   `format_match_line`, `group_lines_by_file`, `format_grouped_output`,
   `group_line_indices_by_blank`, `should_group`, `range_filter_lines`,
   `reattach_single_file_prefix`, `strip_path_prefix`,
   `normalize_slashes_content`, `collect_record_files`.
4. rtk protocol: `parse_rtk_rg_output` (header `N matches in M files:`,
   per-file fold markers, files fold marker, `tail -n +K <log>` hint) →
   cleaned lines + a metadata struct.
5. recorder: `recorder_merge` (insertion-ordered dedup, cap 500).
6. sensitive: `is_sensitive_path` + `sensitive_file_warning` (port of
   `utils/sensitive.py` tables; ASCII gate → non-ASCII returns
   `tool_status::unsupported` so the shim routes to Python).
7. `pattern_has_regex_newline` / `multiline_pattern` are ALREADY ported in
   `src/runtime/tools/grep_pattern.*` (different target — you cannot link it),
   so port them again inside your namespace if the pipeline needs them
   (~30 lines, trivial).

**BLOCKED (write `issue/grep.md`)**: `grep_regex.h/.cpp` Phase B needs **PCRE2**
(plan §3 kernel 7 recommendation; std::regex and RE2 were explicitly rejected
for parity). We do not vendor PCRE2 and must not add a new ext library. Explain
in the report: what PCRE2 buys (byte-exact Python `regex` semantics,
lookaround/backrefs/atomic groups/`\\p{...}`), the rejected alternatives, the
conformance gate that would be needed, and confirm that Phase A ships the
line-offset scanner contract instead (caller supplies match offsets).
Do NOT silently substitute std::regex.
rg/rtk subprocess orchestration, downloads, VFS translation and session
persistence stay in Python per the plan (no reproc work needed here).""",
        extra="""
Your test file should include golden vectors copied from the Python behaviour
(you can verify expected strings with `python -c` against the reference
modules under `C:/dev/kimi-agent/kimi-cli/src/kimi_cli/tools/file/`).""",
    ),
    "read": dict(
        plan="read.md", file="read_tool",
        scope="""
Implement the pure-CPU read kernels in ONE pair (`read_tool.h/.cpp`):
1. **line rendering** (plan §3.1 phase 1): 1-based line-numbered rendering with
   the `\\t` gutter, `show_line_numbers`, `offset`/`limit` (including the
   negative-offset tail mode), per-line hard truncation (use shared
   `truncate_line`), `max_char` windowing with the "output window shows head
   chars a..b" note, LF/CRLF handling, and the "N lines read / End of file
   reached / file has only M lines" status notes. Byte/line accounting must be
   single-pass (reuse `runtime`-style scan semantics but implement locally).
2. **line-hash / repeated-line collapse** helpers used by read's
   dedup mode (see plan §3 / existing `runtime/tools/line_hash.cpp` for the
   contract; re-implement in your namespace).
3. **profile summarizer** (`*.cpuprofile` via vendored yyjson: node tree walk,
   self/total time aggregation, top-N hot paths, idle/network filtering; and
   the macOS `sample` text parser). Both are pure CPU and fully implementable —
   yyjson is already available.
4. **markdown_to_text**: deterministic scanner for headings, code fences,
   inline code, emphasis, links/images, lists, tables, HTML entity stripping,
   and blank-line collapse (plan §3 phase 3, `read_markit.py` markdown branch).

**BLOCKED (write `issue/read.md`)** — the plan itself keeps these in Python for
missing libraries; document precisely which third-party extractors we do not
vendor and therefore cannot port: PDF (PyMuPDF/poppler/mupdf), DOCX (needs a
DOCX/OOXML reader — `zip` + XML; note: no zip decompressor is vendored either,
see below), XLSX/XLS (openpyxl/xlrd), PPTX (python-pptx), IPYNB (nbformat),
HTML→text via `markdownify` (fetch_url owns the HTML pipeline — do not
duplicate), and image/PDF page rendering (needs a codec + renderer).
Also state the **archive** blocker: zip/tar/gz/bz2/xz member reading needs a
decompression library (libdeflate/zlib/miniz/libarchive) that is not in
`src/ext` — so `read_archive` extraction cannot be ported.

Do NOT implement conflict-marker scanning (owned by `write`).""",
        extra="""
For the yyjson-based profile summarizer, feed JSON as a `kimix::string_view`
buffer with `yyjson_read_opts` + a zero-copy input; do not use exceptions.""",
    ),
    "read_image": dict(
        plan="read_image.md", file="read_image_tool",
        scope="""
Implement the decision kernels of the read_image plan in ONE pair
(`read_image_tool.h/.cpp`) — no codec, no Pillow replacement:
1. `media_types.h`-equivalent POD structs (`image_meta`, `policy_result`,
   `ladder_step`, `payload_request`, MIME enum) — declare them in your
   namespace inside your header (do not create extra files).
2. `sniff_image_dimensions` for PNG, JPEG (SOF scan, incl. EXIF-orientation
   swap), GIF, WebP (VP8/VP8L/VP8X), BMP, and `sniff_media_from_magic` /
   `detect_file_type` from magic bytes (incl. the accepted-MIME gate that
   prevents session poisoning).
3. EXIF orientation parser (TIFF IFD walk, little/big endian, orientation tag
   only) — pure byte scanning.
4. `image_policy`: per-MIME policy tables, byte/token budget computation,
   `format_byte_size`, `parse_region_pct`, DPI ladder fallback sequence
   (150→96→72) and the "over budget" decision, conversion guidance strings,
   max-dimension clamps.
5. `compress_ladder`: `fit_dimensions`, the ladder/mip-map plan, and the
   `box_downsample_2x2` kernel on raw RGBA bytes (pure arithmetic — this one IS
   implementable without a codec).
6. `payload_builder`: `to_data_url` (needs a **base64 encoder — write a small
   local static one** in your cpp), note/error/tag string builders, text
   preview construction.

**BLOCKED (write `issue/read_image.md`)**: the actual decode→resize→encode
pipeline (phase 2 in the plan) needs image codecs: PNG (libpng/zlib),
JPEG (libjpeg-turbo/mozjpeg), WebP (libwebp), plus a PDF page rasteriser. We
vendor none of them and must not add one; `stb_image`/`stb_image_write` are not
in `src/ext` either. The plan itself defers this to a follow-up phase —
state the concrete blocker, the candidate libraries with sizes/licences, and
what ships instead (all policy/decision logic + the 2x2 box kernel on raw
RGBA).""",
        extra="",
    ),
    "write": dict(
    plan="write.md", file="write_tool",
    scope="""
Implement the write-tool guards in ONE pair (`write_tool.h/.cpp`):
1. **UTF-8 strict validation** — already provided by the shared
   `utf8_util.h` (`utf8_strict_error` gives the CPython message + offset).
   Reuse it; add only the write-specific wrapper that formats
   `utf-8 decoding error: <reason>` diagnostics like the reference.
2. **auto-generated-file guard** (`check_fmt`/auto-generated detection): the
   filename patterns (`zz_generated.*`, `*_pb.go`, `*_pb2.py`, `*.gen.ts`, ...)
   and the header-marker scan (`@generated`, `Code generated by ...`,
   `DO NOT EDIT`, ...) returning a refusal message identical to the tool's.
3. **conflict-marker scan + splice** (you OWN this — `read` must not declare
   it): detect `<<<<<<<`/`=======`/`>>>>>>>` blocks, parse side labels, assign
   conflict ids, list a whole-file index, extract one side
   (`ours`/`theirs`/`base`), and splice a resolved block back in.
4. **JSON format validation** with line/column diagnostics using the vendored
   **yyjson** (`yyjson_read` + error position → 1-based line/col, plus the
   "extra data" case). YAML/TOML/XML stay Python per the plan (no vendored
   parsers) — document that in your report, and return
   `tool_status::unsupported` for those formats rather than failing.
5. **mkdir / overwrite decision kernels**: `show_diff` unified-diff generation
   (you own the diff-emitting kernel for `write`; keep it a minimal,
   deterministic LCS/unified-diff — or port from `runtime/diff` semantics),
   the post-write size/verify check (`size matches` note), and the
   auto-generated/conflict refusal composition.

Everything above is implementable with what we vendor → **no issue/ report
expected** (just document the YAML/TOML/XML non-goals).""",
    extra="",
),
    "edit": dict(
    plan="edit.md", file="edit_tool",
    scope="""
Implement the edit kernels in ONE pair (`edit_tool.h/.cpp`) — the largest
surface; keep each mode in its own section:
1. common helpers: newline detection/normalisation, split/join keeping the
   trailing-newline flag, byte-range splicing.
2. `fuzz_ratio`: port of `rapidfuzz.fuzz.ratio`
   (independent-prefix/suffix trim + DP Levenshtein similarity, rounded like
   rapidfuzz) — pure CPU, no library needed. Golden vectors: verify against
   `python -c "from rapidfuzz import fuzz; print(fuzz.ratio(a,b))"` if
   rapidfuzz is installed; otherwise verify against the reference algorithm
   description in the plan and note the source.
3. replace modes: `exact` (first/all), `strip` (leading-whitespace-tolerant),
   `fuzzy` (best match ≥ threshold using your fuzz_ratio, ties → ambiguous),
   `similar` (whitespace/case-normalised scan), replace_all counting, and the
   exact `old_string`/`new_string` result/`edit_result` shapes and error
   messages (count of occurrences, "not found", "multiple matches", ...).
4. diff-hunk kernels: normalize unified diff, parse hunks (line numbers,
   context/match/mismatch), fuzzy hunk location search, indent adjustment,
   apply + conflict reporting — mirroring `edit/diff.py`.
5. hashline kernels: hash-anchor grammar (`<hash>:<text>` lines), anchor
   matching with dedupe/overlap handling, apply.
6. sloppy kernels: block/inline matching, `all_match`, fuzzy tolerance.
7. **Do NOT** declare conflict-marker scan symbols owned by `write`
   (`edit_conflict.h/.cpp` in the plan is the *same* feature; skip it and note
   in your report that `write` owns it).

No third-party library is required → **no issue/ report expected.**""",
    extra="""
Keep the DP Levenshtein bounded: cap the working matrix (e.g. skip fuzz matching
for strings longer than a documented limit and return
`tool_status::too_large`), and make that limit a named constant.""",
    ),
    "bash": dict(
    plan="bash.md", file="bash_tool",
    scope="""
Implement the remaining bash-tool CPU kernels in ONE pair (`bash_tool.h/.cpp`):
1. `has_top_level_pipe` (quote/heredoc-aware top-level `|` detection),
   `interpret_exit_code` (incl. the signal/SIGPIPE rules, 127/126/128+N and the
   Python-dict message tables from `output_enhance.py`), `is_expected_exit`
   (planning §3.1; byte-exact message text).
2. `find_error_line_index` (+ `_ERROR_KEYWORDS` table) and
   `truncate_lines` with error preservation (§3.3) — you OWN these two symbols
   (the python agent must not declare them).
3. `background.utils`-style status-line helpers if the plan's §3 lists them.
4. Optional §3.4 (`rtk_rewrite`): the quote-aware shell segment splitter
   (`_split_shell_segments`), `_read_shell_word`, `_rewrite_shell_segment`
   and `_maybe_rewrite_shell_command_with_rtk` decision kernel. Implement it —
   it is pure string work.

**Do NOT implement `detect_self_kill` / `self_kill_hint`** (owned by the `pwsh`
agent per the README ownership map); note the deferral in your report.
Subprocess spawning itself stays Python **except** where you need a bounded
"run and capture" contract: implement the capture/timeout/kill *policy* as a
pure state machine (input: bytes chunks + elapsed ms + timeout + pattern →
decision struct), NOT a real spawn. Document that `kimix-reproc` is now
vendored and available for a follow-up phase that moves the actual spawn into
C++ (give the concrete reproc API names in your report; do not spawn processes
in unit tests).""",
    extra="",
    ),
    "pwsh": dict(
    plan="pwsh.md", file="pwsh_tool",
    scope="""
Implement the **self-kill guard** (§3.2) in ONE pair (`pwsh_tool.h/.cpp`) — you
OWN `detect_self_kill` for both bash and pwsh:
- `_segment_text` / `_segment_tokens` / `_looks_like_flag` /
  `_numeric_pid_targets` / `_loop_pid_sources` / `_variable_pid_hit` /
  `_name_kill_hit` / `_pkill_full_match` helpers (file-static in your cpp),
- the five ordered detectors (kill/tskill, taskkill, stop-process+get-process,
  pkill/killall, wmic) with first-hit-wins semantics and the exact
  human-readable hit descriptions from `safety.py`,
- public API:
  `kimix::optional<kimix::string> detect_self_kill(cmd, protected_pids,
  image_names, cmdline)` plus a variant returning a `self_kill_result` struct
  (hit description + matched rule id) so the shim can build the hint message,
- ASCII-only contract: non-ASCII input, or a `pkill` pattern containing any
  regex metacharacter, must be reported through an out-param /
  `tool_status::unsupported` so the caller routes to the Python mirror
  (plan §8 conformance gate). Do not use `std::regex`.
- Also port `command_detection_variants` (deobfuscation variants) if the plan
  needs it inside your namespace, and `self_kill_hint` message composition as a
  pure formatter taking the agent pid list.

OS introspection (`_agent_pids`, `_agent_image_names`, `_agent_cmdline`) stays
Python per the plan → no `issue/` report needed.""",
    extra="",
    ),
    "python": dict(
    plan="python.md", file="python_tool",
    scope="""
Implement the python-tool CPU kernels in ONE pair (`python_tool.h/.cpp`):
1. `ScriptFileWriter`: deterministic temp script path/naming +
   collision-avoidance + cleanup decision (pure path arithmetic; take the base
   dir as a string; no filesystem calls in the kernel — expose a
   `plan_script_path(base_dir, code_hash_or_name, attempt)` style function).
2. `resolve_python_exe` decision kernel: given a candidate list + a probe
   callback (`kimix::function<bool(kimix::string_view)>`), pick the interpreter
   with the reference's precedence (venv > project > PATH > sys.executable).
3. `prepare_python_env` + `env_delta`: assemble child env (scrub secret-looking
   variables, inject project dir, PATH ordering) and compute the added/changed
   delta vs the parent env, matching the reference's variable-name rules.
4. `module_not_found_hint`: the import-name → pip-package hint table + message
   formatting (byte-exact).
5. `build_session_output_block`: the ordered key/value output block the tool
   returns (`task_id`, `status`, `output`, `exit_code`, `exit_code_meaning`,
   `failure_hint`, `wait_matched`, `elapsed_seconds`, `output_path`,
   `output_truncated`, `original_path`) with the reference's empty/omission
   rules — pure formatting over a plain input struct.
6. `extract_export_path`: the `[saved to ...]` / export-path scanner from
   tool output.
7. `wait_for_pattern` matching: literal + simple glob matching of a pattern
   against a chunk stream (the full Python `regex` engine case must return
   `tool_status::unsupported` so Python handles it).

Do NOT declare `truncate_lines` / `find_error_line_index` (owned by `bash`).
No new libraries needed → no `issue/` report.""",
    extra="",
    ),
    "fetch_url": dict(
    plan="fetch_url.md", file="fetch_url_tool",
    scope="""
Implement the fetch_url CPU kernels in ONE pair (`fetch_url_tool.h/.cpp`) —
you OWN all `url_safety` symbols (the web_search agent must not declare them):
1. **Tolerant HTML tokenizer + light DOM** (arena `kimix::vector<dom_node>`):
   `html.parser`-like tag/attr/text/comment/doctype handling, void elements,
   implicit closes for the supported subset, entity decoding (named + numeric),
   bounded depth/node-count → `tool_status::unsupported`.
2. `decompose`, `find_main`, `find_body`, `text_len_stripped`
   (bs4 `get_text(strip=True)` code-point count), `serialize_node`
   (canonical `str(soup)` form for the supported constructs).
3. `markdownify_atx`: headings (ATX), paragraphs, lists (ol/ul, nesting),
   pre/code blocks, blockquote, hr, tables, inline code/bold/italic/links/
   images/line breaks. Then `html_to_markdown` glue: `<main>`/`role=main`
   target else `<body>`, `\n{3,}` collapse + strip.
4. **text stats**: `len_without_ws` (code points) + `has_login_wall` keyword
   scan for the fetch-decision (which of httpx / playwright / pdf to try).
5. **url_safety**: `normalize_url_for_request` (IRI→ASCII, percent-encoding
   rules, default ports, dot-segment removal, query/fragment preservation),
   `sensitive_query_param_name` + `url_contains_secret` (credential-prefix and
   param-name tables), `classify_resolved_address` (IPv4/IPv6 →
   private/loopback/link-local/multicast/unspecified/public; pure parsing),
   `is_blocked_hostname` (metadata endpoints, suffix tables), and a
   **punycode/RFC-3492 encoder** for IDNA hosts (plan §3.1 `punycode.h`; the
   bootstring encode is ~120 lines of pure code — implement it rather than
   routing to Python, and keep the ASCII fast path).
6. Charset-decoding decision helper: given HTTP `Content-Type` + HTML `<meta>`
   candidates, return the encoding name to use (the decode itself stays
   Python).

Everything is self-contained string/byte work → **no `issue/` report expected**
(document the httpx/Playwright/SSL/retry pieces that stay Python).""",
    extra="""
Keep the HTML DOM node arena in a struct you own; do not expose raw pointers
across the API. Unicode whitespace stripping for `text_len_stripped` may be
approximated by the documented code-point ranges used in the reference — note
any approximation in the report.""",
    ),
    "web_search": dict(
    plan="web_search.md", file="web_search_tool",
    scope="""
Implement ONLY the web_search-specific kernels in ONE pair
(`web_search_tool.h/.cpp`) — `url_safety` is owned by the fetch_url agent, so
do NOT declare normalize_url / sensitive-param / IP-classification symbols:
1. `convert_base64_images_to_links`: scan markdown/HTML for
   `data:image/...;base64,` payloads, validate/limit, replace with
   `[saved](path)` style links and emit the extracted payloads as a list
   (pure scanning; base64 *decoding* may be a small local static helper).
2. `truncate_with_footer`: the byte/char-budget cut with the reference's
   footer text (reuse shared `truncate_line` / `join_with_byte_limit` where the
   semantics match, and say so in the report).
3. `make_cache_slug`: **byte-exact** `xxhash.xxh64(url_bytes).hexdigest()[:10]`
   using `XXH64` from the vendored `xxhash.h` (NOT `kimix::hash64`, which is
   XXH3 — the plan §8 flags this as a silent cache-key break; add a test that
   pins known digests you generate with Python `xxhash` if installed, else pin
   against the xxHash reference vectors in `src/ext/xxHash` docs and note it).
4. `build_search_output`: the result rendering the tool returns —
   summary answer block + source list (`N. title`, url, snippet), the
   `include_content` full-page block, ordering/dedup by url, per-item truncation
   and the overall byte cap.
5. Any pure helpers the plan's §3 lists for `search.py`
   (`web_item` struct, limit clamping, engine-name routing decision table).

No new libraries required → no `issue/` report. HTTP/transport stays Python.""",
    extra="",
    ),
}


def main() -> None:
    names = sorted(TOOLS)
    import sys
    if len(sys.argv) > 1:
        names = sys.argv[1:]
    for tool in names:
        spec = TOOLS[tool]
        wt = WT / tool
        if not wt.exists():
            print(f"!! missing worktree {wt}")
            continue
        text = COMMON.format(
            tool=tool, plan=spec["plan"], file=spec["file"],
            scope=spec["scope"].strip(), extra=spec["extra"].strip(),
        )
        (wt / "AGENT_TASK.md").write_text(text, encoding="utf-8", newline="\n")
        # ensure reports dir exists in the worktree source tree
        rep = wt / "src" / "builtin_tools" / "reports"
        rep.mkdir(parents=True, exist_ok=True)
        (wt / "issue").mkdir(parents=True, exist_ok=True)
        print(f"wrote {wt / 'AGENT_TASK.md'} ({len(text)} chars)")


if __name__ == "__main__":
    main()
