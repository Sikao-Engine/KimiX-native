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
→ strip port / IPv6 brackets). Non-ASCII hosts are ASCII-gated (per-byte -
vs Python per-code-point - — documented deviation; hosts are punycode in
practice).

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
   Defaults produce byte-identical output to the Python tool for
   non-duplicate, under-cap inputs. Numbering ("N. title, url, snippet") from
   the brief's paraphrase was not added because it is absent from the
   Python rendering.
3. ASCII gate for convert_base64_images_to_links — Python's regex
   \s matches Unicode whitespace; the C++ scanner matches ASCII whitespace.
   Callers route non-ASCII input to the Python mirror (project convention).
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
- build_search_output: 4 tests (byte-exact golden, dedup-by-url, summary +
  content cap, byte cap).
- WebSearch Tool wrapper: 4 tests (nullptr parameters, missing `items`,
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
