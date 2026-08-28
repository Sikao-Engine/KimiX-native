# fetch_url builtin tool — C++ implementation report

Worktree: `C:/dev/kimix_wt/fetch_url` (branch `agent/fetch_url`, base 48bddc9).

## Scope

Port of the fetch_url built-in agent tool CPU kernels to C++ in one pair
(`src/builtin_tools/fetch_url_tool.h` / `.cpp`, namespace
`kimix::builtin_tools::fetch_url`). The tool owns every `url_safety` symbol
(normalize_url_for_request, sensitive_query_param_name, url_contains_secret,
classify_resolved_address, is_blocked_hostname, is_always_blocked_address,
is_safe_url_decision); the web_search agent must not redeclare them.

## Source of truth

The referenced plan `C:/dev/kimi-agent/plans/fetch_url.md` **does not exist**
(plans directory is empty at the base commit; only `AGENT_TASK.md` and the
cached planner prompt `C:/dev/kimi-agent/.kimix_cache/prompts/fetch_url.md`
are present). This port therefore follows `AGENT_TASK.md` and the Python
reference code:

- `C:/dev/kimi-agent/src/kimix/tools/web/web_fetcher/fetcher.py`
  (`_html_to_markdown`, `fetch_to_markdown`, `_LOGIN_PATTERNS`,
  `len(markdown.replace(" ","").replace("\n",""))`)
- `C:/dev/kimi-agent/kimi-cli/src/kimi_cli/tools/web/url_safety.py`
- `C:/dev/kimi-agent/kimi-cli/src/kimi_cli/tools/web/fetch.py` (tool-level
  block message texts)
- Installed bs4 4.15 (`html.parser` builder) + markdownify 1.2.3 in
  `C:/dev/kimi-agent/.venv` — behaviour captured via Python goldens.

## Function-by-function mapping

| C++ symbol | Python reference | Notes |
|---|---|---|
| `parse_html` (tokenizer + arena DOM) | `BeautifulSoup(html, "html.parser")` (bs4 4.15 builder + `html.parser`) | html.parser-like: lowercased tag/attr names, HTML5 void elements, self-closing `<tag/>`, end-tag pop-to-match, RAWTEXT (script/style/xmp/iframe/noembed/noframes) and RCDATA (textarea/title), named (full 2125-entry HTML5 table) + numeric character references, comments, doctype, bogus decls, PI, whitespace-only data segment collapse (`" "` / `"\n"`), and bs4's incomplete-charref close() semantics (a decimal charref followed by a hex digit swallows the rest of the document). Bounded depth (256) / node count (200 000) -> `tool_status::unsupported`. |
| `decompose` | `Tag.decompose()` | Marks + prunes subtrees for the 19-tag glue list. |
| `find_tag` / `find_attr` | `soup.find("main")` / `soup.find(role="main")` | DFS in document order. |
| `text_len_stripped` | `len(tag.get_text(strip=True))` | Concatenates stripped text nodes (comments/doctypes excluded) and counts code points. |
| `serialize_node` | `str(soup)` minimal formatter | Sorted attribute names, minimal escaping (`&`, `<`, `>`; `&quot;` inside double-quoted values), void `<tag/>`, comments `<!--...-->`, doctype `<!DOCTYPE ...>\n`, raw text inside script/style. |
| `markdownify_atx` | `markdownify(str(target), heading_style="ATX")` (markdownify 1.2.3) | Port of `MarkdownConverter` with the defaults used by fetcher.py: ATX headings, paragraphs, ol/ul/li (with `start`), pre/code/kbd/samp, blockquote, hr, tables (thead inference, colspan, empty header rows), inline code/bold/italic/links (autolink)/images/line breaks, div/article/section/dl/dt/dd, del/s, q, sub/sup, script/style removal, `\n{3,}` collapse and strip in `html_to_markdown`. |
| `html_to_markdown` | `fetcher._html_to_markdown` | Glue: decompose 18+ tags, target = `<main>` or `role="main"` (only when stripped text length >= 500) else `<body>` else document. |
| `len_without_ws` | `len(text.replace(" ", "").replace("\n", ""))` | Code-point count excluding U+0020/U+000A. |
| `has_login_wall` | `fetcher._LOGIN_PATTERNS` (regex, IGNORECASE) | Fixed-literal scan (all alternatives are literals). |
| `normalize_url_for_request` | `url_safety.normalize_url_for_request` | urlsplit/urlunsplit port, scheme-whitespace repair, IDNA host via `idna_encode_host`, urllib `quote()` safe sets. |
| `sensitive_query_param_name` | `url_safety.sensitive_query_param_name` | parse_qsl(keep_blank_values=True) over `&` pairs, non-empty values, 20-name table, returns the unquoted key. |
| `url_contains_secret` | `url_safety.url_contains_secret` | Raw + unquote + normalized candidates against the credential-prefix table (59 patterns incl. xapp-, xox, AKIA special cases) with `(?<![A-Za-z0-9_-])` / `(?![A-Za-z0-9_-])` boundaries. |
| `is_blocked_hostname` | `url_safety._BLOCKED_HOSTNAMES` | metadata.google.internal / metadata.goog (exact, lowercased, trailing-dot stripped). |
| `classify_resolved_address` | `url_safety._is_blocked_ip` + `ipaddress` | Pure IPv4/IPv6 parsers (incl. `%scope` strip, IPv4-mapped `::ffff:x.x.x.x`), private/loopback/link-local/multicast/unspecified/reserved/CGNAT/public. |
| `is_always_blocked_address` | `url_safety._ALWAYS_BLOCKED_IPS/_NETWORKS` | 169.254.0.0/16 (+ mapped), 169.254.169.254/170.2/169.253, 100.100.100.200, fd00:ec2::254, `::ffff:` variants. |
| `is_safe_url_decision` | `url_safety.is_safe_url` (pure part) | Scheme/hostname/blocked-hostname checks, DNS-failure proxy-delegation rule, per-address classification; DNS resolution itself stays Python. |
| `idna_encode_host` | `hostname.encode("idna")` (builtin codec) + RFC-3492 | ASCII labels unchanged; non-ASCII labels simple-lowercased + hand-written RFC-3492 punycode encoder (verified against Python goldens: münchen -> xn--mnchen-3ya, bücher, mañana, 例え, hätte, MÜNCHEN). |
| `pick_encoding` | — (no Python reference found; designed per AGENT_TASK §6) | Content-Type charset wins, then meta candidates; allow-list of common encodings; default utf-8. The actual decode stays Python. |

## What stays in Python (documented, not ported)

- **Network I/O**: httpx fetch, retries/backoff, SSL context building/verification,
  TLS 1.2 pinning (`fetcher._fetch_html_http`, `fetch_html_http_with_fallback`,
  `_build_ssl_context`).
- **Headless browser**: Playwright launch/navigation/`page.content()` and the
  mobile-UA login-wall retry loop (`fetcher._fetch_html`, `fetch_to_markdown`).
- **DNS resolution**: `socket.getaddrinfo` — the C++ `is_safe_url_decision`
  consumes an injected `resolve_outcome` (dns_failed + addresses).
- **Tool wrapper**: aiohttp service fetch, ToolResultBuilder messages, micro
  compression / output export (`fetch.py`, `fetch_url.py`).
- **Raw byte decoding**: the charset decision helper returns the encoding name;
  the actual decode (response.text / UnicodeDammit) stays Python.

## Deviations / approximations

1. **Plan file missing** (`C:/dev/kimi-agent/plans/fetch_url.md`): the §3
   design and §7 test list were reconstructed from AGENT_TASK.md + Python
   sources + installed-library goldens.
2. **Numeric charref error cases**: the installed bs4 4.15 runtime (pyc)
   returns the *empty string* for numeric references 0x00, > U+10FFFF, and
   surrogates (the shipped `dammit.py` source would return U+FFFD). The C++
   mirrors the observable runtime behaviour (nothing appended) — verified
   against `BeautifulSoup("&#1114112;")`, `&#0;`, `&#xD800;`.
3. **markdownify `convert_dd`**: the installed runtime prefixes `:` to the
   already-indented first line (`: Definition`) whereas the published source
   replaces the first indent char (`:Definition`). C++ follows the runtime.
4. **IDNA case folding**: non-ASCII labels use a simple Latin-1 + ASCII
   lowercase mapping before punycode (the builtin idna codec applies full
   Nameprep). Covers all verified goldens; exotic case folds (e.g. ß) are not
   mapped.
5. **`text_len_stripped` whitespace**: Python `str.strip()` approximated by
   the documented `str.isspace()` code-point ranges (space, tab, LF, VT, FF,
   CR, 0x1C-0x1F, 0x85, 0xA0, 0x1680, 0x2000-0x200A, 0x2028/29, 0x202F,
   0x205F, 0x3000).
6. **`find_previous_sibling` (markdownify tr)**: bs4's no-arg
   `find_previous_sibling()` returns the previous sibling *Tag*; C++ mirrors
   that (skips whitespace text).
7. **MSVC source encoding**: the sources are pure ASCII (all non-ASCII
   comments/strings transliterated or `\x`-escaped) because MSVC otherwise
   misreads UTF-8 as the GBK code page (C4819 / brace-count corruption).
8. **Attr entity decoding** (`decode_attr_entities`): numeric references are
   always decoded; named references are decoded when exact + not followed by
   `=` (html.parser `_unescape_attrvalue`) — implemented without the
   `&name=`-trailing edge cases beyond the documented rule.
9. **HTML parser subset**: html.parser has no implicit-close table, so the
   tokenizer intentionally produces nested `li/td/p` trees for unclosed
   siblings (matches bs4 html.parser). Pathological malformed markup
   (e.g. `<div>x<y</div>`) may serialize differently than bs4's recovery.

## Tests

`tests/unit/builtin_tools/test_fetch_url_tool.cpp` — Boost.UT, main-scope
`"<snake_case>"_test` lambdas only, HTML fixtures as inline string literals,
punycode goldens verified against Python `idna`:

- **50 tests, 197 asserts**, all passing
  (`./bin/debug/test_builtin_fetch_url.exe` -> `Suite 'global': all tests
  passed (197 asserts in 50 tests)`).
- Coverage: tokenizer/DOM (14), DOM helpers (3), markdownify goldens (11),
  html_to_markdown glue (7), text stats (2), normalize_url_for_request (7),
  sensitive params + secret prefixes (2), SSRF classification/decision (4),
  idna/punycode (2), charset helper (1).

Local-only registration added between the markers in `tests/xmake.lua`
(restored before commit):
`builtin_tools_test("test_builtin_fetch_url", "unit/builtin_tools/test_fetch_url_tool.cpp")`

## Build/verify commands (all run in the worktree)

```
xmake f -m debug -y -c
xmake build kimix-llm
xmake build test_builtin_fetch_url
./bin/debug/test_builtin_fetch_url.exe
```
