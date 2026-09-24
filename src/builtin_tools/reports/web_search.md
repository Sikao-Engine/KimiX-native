web_search built-in tool — C++ implementation report

Worktree: D:/KimiX-native
Files: src/builtin_tools/web_search_tool.h, src/builtin_tools/web_search_tool.cpp,
tests/unit/builtin_tools/test_web_search_tool.cpp (this report).

1. Scope

Ported the pure web_search kernels named in the task brief to
kimix::builtin_tools::web_search:

1. convert_base64_images_to_links — data-URL scan + [IMAGE: alt] / [IMAGE]
   placeholder replacement (+ optional raw-payload collection).
2. truncate_with_footer — char-budget head+tail cut with the reference footer.
3. make_cache_slug — xxhash.xxh64(url_bytes).hexdigest()[:10] via vendored
   XXH64 (NOT kimix::hash64, which is XXH3).
4. build_search_output — the result renderer (dedup by URL, include_content
   block, per-item truncation, overall byte cap).
5. Pure helpers: web_item, clamp_search_limit, clamp_extract_char_limit,
   resolve_active_provider (engine routing decision table), plus the
   store_full_text cache-file naming/write helper that truncate_with_footer
   needs for its footer.
6. Tool class wrapper: `class WebSearch : public kimix::builtin_tools::Tool`
   exposes the search-result renderer to the binding layer through the shared
   `ToolParams` JSON contract. The provider/HTTP path stays in Python; the
   wrapper receives the pre-built `items` array and optional rendering flags
   (`include_content`, `summary`, `max_content_chars`, `max_output_bytes`) and
   returns a JSON object with `ok`, `status`, `text`, `truncated`, and
   `omitted_items`.

HTTP/transport stays Python (the providers module). url_safety symbols are
owned by fetch_url and were NOT declared here.

2. Python source of truth

The task brief pointed at C:/dev/kimi-agent/plans/web_search.md for the
design (§3/§7/§8). That file does not exist — C:/dev/kimi-agent/plans/
is empty (verified; only AGENT_TASK.md and the repo layout exist). The
brief's listed module path C:/dev/kimi-agent/src/kimix/tools/web/ also does
not exist (that directory contains only fetch_url.py + web_fetcher/); the
real reference modules live under
C:/dev/kimi-agent/kimi-cli/src/kimi_cli/tools/web/:

| C++ symbol | Python reference | Lines |
|---|---|---|
| web_item | search.py SearchResult | 41-49 |
| clamp_search_limit (bounds) | search.py Params.limit ge=1, le=20 | 27-38 |
| clamp_extract_char_limit | content.py get_extract_char_limit (floor 2000 / cap 500000 / default 15000) | 166-186 |
| resolve_active_provider | providers.py _resolve, _SEARCH_LEGACY_PREFERENCE, _EXTRACT_LEGACY_PREFERENCE | 232-300 |
| convert_base64_images_to_links | content.py | 38-65 |
| make_cache_slug / make_cache_file_name / store_full_text | content.py store_full_text (host slug + digest + write + 2,000,000-char cap) | 75-104 |
| truncate_with_footer | content.py | 107-163 |
| build_search_output | search.py SearchWeb.__call__ rendering | 112-125 |
| WebSearch Tool wrapper | search.py SearchWeb.__call__ orchestration | 81-125 |

Goldens for the tests were captured by running the reference modules under
Python (xxhash package installed; output blocks dumped with repr).

3. Function-by-function notes

convert_base64_images_to_links (content.py 38-65)
Byte-exact port of the three regex substitutions:
!\[[^\]]*\]\(\s*data:image/[^;]+;base64,[A-Za-z0-9+/=\s]+\),
\(\s*data:image/[^;]+;base64,[A-Za-z0-9+/=\s]+\),
data:image/[^;]+;base64,[A-Za-z0-9+/=]+.

The C++ scanner is a single left-to-right pass that tries the three patterns
at each position. The patterns' first characters are disjoint (!, (, d),
so at most one matches per position; replacements never contain a pattern
prefix, so a single pass is equivalent to the three sequential re.sub passes
(verified against 19 Python goldens including the tricky non-matches:
![a](data:image/png;base64,ZZZZ!!) → ![a]([IMAGE]!!),
(data:image/png;base64,AAAA → ([IMAGE],
data:image/svg+xml;charset=utf-8;base64,AAAA unchanged).
\s is ASCII-only (project ASCII gate: non-ASCII input routes to the Python
mirror; Python's regex \s also matches Unicode whitespace — documented
deviation). Alt text is .strip()-equivalent (ASCII whitespace).

The second overload additionally collects the raw base64 payload of every
replaced blob in document order (extension, see §5).

make_cache_slug (content.py 89)
XXH64(url.data(), url.size(), 0) from the vendored xxhash.h
(#define XXH_INLINE_ALL before #include "xxhash.h", same pattern as
tests/unit/ext/test_xxhash.cpp), formatted as lowercase hex, first 10 chars.
Pinned against Python xxhash.xxh64(...) in 8 test vectors, including the
empty string, ASCII URLs, and a non-ASCII (UTF-8) URL. NOT kimix::hash64
(XXH3) — the plan's §8 warning.

make_cache_file_name / store_full_text (content.py 75-104)
make_cache_file_name = {slug}-{digest}.md with the exact Python pipeline:
urlparse(url).hostname or "page", .replace(":", "_"), then
re.sub(r"[^A-Za-z0-9._-]", "-", host)[:60].strip("-") or "page". The
hostname extraction is a small local helper (scheme + netloc → strip userinfo
  → strip port / IPv6 brackets). SUPERSEDED by §7: the host is ASCII-lower-cased
  (urlparse().hostname lower-cases) and the slug is built per code point, e.g.
  "köln" → "k-ln"; the earlier per-byte/ASCII-gated behaviour is gone.

store_full_text is the only real filesystem function (isolated, per the
shared brief): create_directories, write the file as UTF-8 via fopen/fwrite
(CRT), cap content at k_max_stored_text_chars = 2,000,000 code points with
the exact Python marker, best-effort false on any failure (Python
try/except → None). out_path carries the absolute path.

truncate_with_footer (content.py 107-163)
Byte-exact port. char_limit is in code points (Python len(str)); all
slicing goes through builtin_tools/utf8_util.h
(utf8_code_point_count, utf8_byte_offset_of_code_point) so byte offsets
always land on UTF-8 boundaries. The head/tail newline snaps replicate
Python's code-point-index comparisons (2*nl > head_budget ≡
nl > head_budget * 0.5 for the non-negative ints involved), including the
content[-0:] == content[0:] quirk when tail_budget == 0. The footer is
byte-identical (8/29 ─ rules, em dash in the middle marker, thousands
separators, middle_start_line = head.count("\n") + 2).

The Python body calls store_full_text(url, content) (config-driven cache
dir + file write); the C++ kernel injects that side effect as a
store_full_text_fn callback so unit tests are deterministic. The binding
layer wires the real store_full_text (or keeps Python's) — same spirit as
the shared brief's "inject existence probes" guidance. We did NOT reuse
truncate_line / join_with_byte_limit here because their semantics differ
(truncate_line appends a "… [+K chars]" marker; join_with_byte_limit
operates on lines with \n separators) — truncate_with_footer's head+tail
window + footer is a distinct algorithm.

build_search_output (search.py 112-125 + task-brief features)

  NOTE: the dedup-by-URL / 100 KiB byte-cap / include_content-gated behaviour
  described in the next three paragraphs was replaced by the parity pass in §7
  (dedup_urls is now opt-in, the cap reproduces ToolResultBuilder, and content is
  rendered whenever it is non-empty).
Pure renderer reproducing SearchWeb.__call__ byte-for-byte for the common
case:
```
Title: <title>
Date: <date>
URL: <url>
Summary: <snippet>

<content>

--- (between items)
```
plus the brief's requested features as options: optional leading summary
block, include_content full-page block, per-item max_content_chars
code-point cap, URL de-dup (first occurrence wins, order preserved), and the
overall max_output_bytes cap (default k_max_output_bytes = 100 KiB) with a
… (N item(s) omitted — output byte cap) … note when the cap bites.

WebSearch Tool wrapper (search.py SearchWeb.__call__ orchestration)
Thin `kimix::builtin_tools::Tool` subclass used by the Python binding layer.
`operator()` deserializes the `items` array into `web_item` structs, reads the
optional rendering flags, calls `build_search_output`, and serializes a JSON
result into `_last_result`. Errors (`parameters == nullptr`, missing or
invalid `items`, non-object array elements) return `ok: false` with
`status: "invalid_input"` and an `error` message. The HTTP/provider layer
stays in Python; this wrapper only handles the pure rendering step.

resolve_active_provider (providers.py 232-300)
Pure decision table: explicit configured name wins when registered + capable
(availability ignored); else the single eligible provider; else the legacy
preference walk (search: kimi → ddgs → local; extract: local →
kimi); else nullopt. Returns the provider name (the Python returns the
provider object — the kernel cannot return a live object; the binding layer
looks the object up by name).

4. What stayed in Python (and why)

- HTTP transport / provider execution (providers.py search/extract,
  aiohttp sessions): the task brief explicitly says "HTTP/transport stays
  Python". The C++ side only needs the pure routing decision table
  (resolve_active_provider), which takes precomputed availability booleans.
- is_available() evaluation: may touch config/env and provider
  construction — resolved by the Python shim before calling
  resolve_active_provider.
- Config resolution (get_share_dir() for the cache/web directory,
  load_config): stays in Python; the native store_full_text receives the
  directory as an argument.
- URL safety (url_safety.py): owned by fetch_url per the cross-tool
  ownership map — not ported here, no symbols declared.

5. Deviations / reconstructions (recorded per the task rules)

1. Missing plan file. C:/dev/kimi-agent/plans/web_search.md does not
   exist; the API was reconstructed from the task brief + the Python
   reference. The brief's §3-style description of convert_base64_images_to_links
   ("replace with [saved](path) style links and emit the extracted payloads
   as a list") contradicts the Python reference, which replaces with
   [IMAGE: alt] / [IMAGE] and returns only the text. Per the workflow rule
   ("follow the Python reference and record the deviation"), the C++ port
   matches Python exactly; the raw-payload list is offered as a separate,
   optional extension overload (convert_base64_images_to_links(text,
   payloads)) that does not change the replacement text. No base64 decoding
   helper was added because the reference never decodes payloads (the charset
   is validated by the pattern itself).
2. build_search_output is a reconstruction. No such function exists in
   the Python reference; it was designed from the brief's bullet list while
   keeping the exact SearchWeb.__call__ block rendering as the default.
   Defaults produce byte-identical output to the Python tool for EVERY input
   after the §7 parity pass (the non-default opts are extensions). Numbering
   ("N. title, url, snippet") from
   the brief's paraphrase was not added because it is absent from the
   Python rendering.
3. ASCII gate for convert_base64_images_to_links — Python's regex
   \s matches Unicode whitespace; the C++ scanner matches ASCII whitespace.
   Callers route non-ASCII input to the Python mirror (project convention).
   SUPERSEDED by §7: the kernel now implements the reference's Unicode \s /
   str.isspace() classes exactly, so the ASCII gate is gone.
4. resolve_active_provider returns a name, not a provider object.
5. store_full_text takes the cache dir as an argument instead of
   resolving get_share_dir() itself.
6. web_provider_info is named to avoid a collision with a Windows SDK
   provider_info type (compile error C2872 under MSVC).
7. kimix::optional uses std::nullopt (kimix has no kimix::nullopt).
8. The WebSearch Tool wrapper receives pre-built result items rather than the
   raw `query`/`limit` parameters. HTTP transport and provider execution stay
   in Python, so the binding layer is expected to call the provider, shape the
   items, and pass them to the native wrapper for rendering.

6. Verification

Build (isolated worktree):
```
xmake f -m debug -y -c
xmake build kimix-llm
xmake build test_builtin_web_search
./bin/debug/test_builtin_web_search.exe
```
Result: all tests passed (87 asserts in 21 tests) on the previous worktree;
this update adds 4 Tool-wrapper tests. Final assert/test counts pending run in
D:/KimiX-native.

Test coverage (Boost.UT, main-scope _test lambdas):
- convert_base64_images_to_links: 4 tests / 20 asserts (md alt stripping,
  paren/bare forms, non-matches, payload collection).
- make_cache_slug: 1 test / 8 pinned XXH64 vectors (≥3 required by the
  brief; includes empty + non-ASCII UTF-8 URL).
- make_cache_file_name: 1 test / 7 vectors (portless, port, IPv6, no-scheme
  fallback, 60-char truncation).
- store_full_text: 2 tests (temp-dir roundtrip incl. UTF-8; 2,000,001-char
  cap marker).
- truncate_with_footer: 4 tests / 4 byte-exact goldens (short passthrough,
  store-none footer, store-path footer with offset=9, Unicode code-point
  budget with 71/24/240 layout).
- build_search_output: 9 tests (byte-exact golden, content rendered without an
  include_content gate, duplicate URLs kept by default, opt-in dedup, summary +
  content cap, ToolResultBuilder cap golden, clean-boundary cut, splitlines
  boundaries, 50k default cap) -- see the §7 parity pass.
- WebSearch Tool wrapper: 7 tests (nullptr parameters, missing `items`,
  full rendering with include_content, byte-cap truncation).
- clamp_search_limit / clamp_extract_char_limit: 2 tests / 9 asserts.
- resolve_active_provider: 3 tests / 10 asserts (explicit-config, single
  eligible, legacy walk, extract capability, nullopt).

Test registration line (local verification only, restored before commit):
```lua
builtin_tools_test("test_builtin_web_search", "unit/builtin_tools/test_web_search_tool.cpp")
```
added between the marker lines in tests/xmake.lua.

No new library needed → no issue/web_search.md.

7. Parity review pass (differential harness vs the kimi-agent checkout)

Added `python/tests/test_parity_web_search.py` (174 tests, all green) which
compares every `runtime_py.builtin_tools.web` web_search kernel against the live
reference: `kimi_cli.tools.web.search.SearchWeb.__call__` driven by a stub
provider (so `kimi_cli.tools.utils.ToolResultBuilder` is exercised exactly as the
tool runs it), `kimi_cli.tools.web.content` and `kimi_cli.tools.utils`.  Corpora:
the reference suite's own cases (`tests/tools/test_web_extract.py`,
`test_web_search_dispatch.py`), an adversarial list and seeded fuzzing.

Discrepancies found and fixed (each has a regression assertion in one or both
test files):

1. `build_search_output` never read the provider's snippet.  The binding mapped
   `item["snippet"]`, but search.py renders `item["description"]` (the
   providers.py response contract), so every real result rendered
   "Summary: " (empty).  Fixed in `src/runtime/py/py_builtin_web.cpp`
   (`parse_web_item` -> "description"); the ToolParams wrapper accepts
   `snippet` (its own documented key) with a `description` fallback.
2. Duplicate URLs were dropped by default.  search.py renders every item it is
   given; de-duplication is now the opt-in `dedup_urls` extension.
3. `include_content=false` suppressed an item's content block.  The reference
   renderer prints any non-empty content (`include_content` only asks the
   *provider* for content); the flag is now accepted but does not gate.
4. The total cap was a 100 KiB *byte* cap that dropped whole items and appended a
   "… (N item(s) omitted — output byte cap) …" note that exists nowhere in the
   reference.  It now reproduces `ToolResultBuilder(max_line_length=None)`
   byte-for-byte: 50,000 *code points*, `str.splitlines(keepends=True)` lines,
   `truncate_line(line, remaining, "[...truncated]")`, stop when full, and the
   `truncated` flag mirrors the builder's `_truncation_happened` (a clean cut on
   a chunk boundary drops items without setting it).
5. `convert_base64_images_to_links` used an ASCII whitespace class; the
   reference `regex` `\s` is Unicode (25 code points) and its markdown-alt
   `str.strip()` uses `str.isspace()` (those 25 plus `\x1c-\x1f`).  Both classes
   are now implemented (and pinned).
6. Blob-scanner off-by-one: `ws_match_data_blob` advanced 12 bytes past
   `data:image/` (11), so an empty mime type (`data:image/;base64,…`) made the
   scanner treat everything after the `;` as the mime type and swallow text up
   to the next `;base64,` as one bogus blob
   (`![[IMAGE]` instead of `![data:image/;base64,[IMAGE]`).
7. `resolve_active_provider` walked a shortened legacy preference list
   (`kimi, ddgs, local`); providers.py's order is 10 entries (firecrawl,
   parallel, tavily, exa, searxng, brave-free, xai before ddgs/local), so an
   available firecrawl/tavily/… backend never won.
8. `make_cache_file_name` divergences: the hostname is lower-cased by
   `urlparse().hostname` (mixed-case URLs produced different cache keys) and the
   slug `re.sub` runs per *code point*, not per UTF-8 byte (`köln` -> `k-ln`,
   not `k--ln`).
9. `truncate_with_footer` was clamped to [2000, 500000] inside the binding; the
   reference function applies `char_limit` verbatim (its caller pre-clamps via
   `get_extract_char_limit`), so a 100-char budget silently returned the page.

Remaining documented deviations (asserted in the tests, not "fixed"):

* `store_full_text` — Python writes through `Path.write_text` (text mode), which
  translates `\n` -> `\r\n` on Windows; the native writer emits the UTF-8 bytes
  verbatim (matching CPython on POSIX).  The test asserts that this translation
  is the *only* difference and that the bytes are identical for single-line
  content.
* `clamp_search_limit` — pydantic rejects an out-of-range limit; the kernel
  clamps (in-range mapping is identical).
* Non-string item fields: search.py's duck typing would render `42`/`None`
  through f-strings; the typed `web_item` contract raises TypeError instead.
* `WebSearch`'s provider/HTTP dispatch and its error messages stay Python (the
  native wrapper only renders pre-built items).
* `resolve_active_provider` is not exposed to Python; its table is covered by
  the Boost.UT tests plus a source-level drift check against
  `providers._SEARCH_LEGACY_PREFERENCE` / `_EXTRACT_LEGACY_PREFERENCE`.

Cross-check note (not part of this tool): `python/tests/test_parity_fetch_url.py`
calls `web.truncate_with_footer(..., include_content=False, cache_dir=None)` and
expects the reference's injected `store_full_text` path to appear in the footer.
That combination cannot produce a stored path (the flag/cache_dir gate predates
this pass and is unchanged); with `include_content=True` plus a real cache dir
the native footer matches the reference byte-for-byte.

Verification:

    python -m pytest python/tests/test_parity_web_search.py -q          -> 174 passed
    python scripts/build_locked.py -- xmake build test_builtin_web_search
    ./bin/debug/test_builtin_web_search.exe                             -> 151 asserts / 35 tests

Boost.UT: `Suite 'global': all tests passed (151 asserts in 35 tests)`.
pytest: `174 passed`.
