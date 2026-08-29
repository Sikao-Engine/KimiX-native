# fetch_url builtin tool — C++ implementation report

Worktree: D:/KimiX-native

## Plan

  plans/fetch_url.md — C++ port of the fetch_url built-in agent tool CPU
  kernels. Network I/O, DNS resolution, headless browser automation, raw-byte
  decoding, and the final tool orchestration (output export, micro-compression)
  stay in Python; the C++ side owns the deterministic HTML-to-Markdown
  conversion, URL safety classification/normalization, and fetch-decision
  helpers.

## Scope

  * src/builtin_tools/fetch_url_tool.h / .cpp (namespace
    kimix::builtin_tools::fetch_url)
  * tests/unit/builtin_tools/test_fetch_url_tool.cpp

  The tool owns every url_safety symbol (normalize_url_for_request,
  sensitive_query_param_name, url_contains_secret, classify_resolved_address,
  is_blocked_hostname, is_always_blocked_address, is_safe_url_decision); the
  web_search agent must not redeclare them.

## Source of truth (Python)

  * D:/kimi-agent/src/kimix/tools/web/web_fetcher/fetcher.py
    (_html_to_markdown, fetch_to_markdown, _LOGIN_PATTERNS,
    len(markdown.replace(" ", "").replace("\n", "")))
  * D:/kimi-agent/kimi-cli/src/kimix/tools/web/url_safety.py
  * D:/kimi-agent/kimi-cli/src/kimix/tools/web/fetch.py (tool-level block
    message texts)
  * Installed bs4 4.15 (html.parser builder) + markdownify 1.2.3 in
    D:/kimi-agent/.venv — behaviour captured via Python goldens.

## Function-by-function mapping

| C++ symbol | Python reference | Notes |
|---|---|---|
| parse_html (tokenizer + arena DOM) | BeautifulSoup(html, "html.parser") (bs4 4.15 builder + html.parser) | html.parser-like: lowercased tag/attr names, HTML5 void elements, self-closing <tag/>, end-tag pop-to-match, RAWTEXT (script/style/xmp/iframe/noembed/noframes) and RCDATA (textarea/title), named (full 2125-entry HTML5 table) + numeric character references, comments, doctype, bogus decls, PI, whitespace-only data segment collapse (" " / "\n"), and bs4's incomplete-charref close() semantics (a decimal charref followed by a hex digit swallows the rest of the document). Bounded depth (256) / node count (200 000) -> tool_status::unsupported. |
| decompose | Tag.decompose() | Marks + prunes subtrees for the 19-tag glue list. |
| find_tag / find_attr | soup.find("main") / soup.find(role="main") | DFS in document order. |
| text_len_stripped | len(tag.get_text(strip=True)) | Concatenates stripped text nodes (comments/doctypes excluded) and counts code points. |
| serialize_node | str(soup) minimal formatter | Sorted attribute names, minimal escaping (&, <, >; &quot; inside double-quoted values), void <tag/>, comments <!--...-->, doctype <!DOCTYPE ...>\n, raw text inside script/style. |
| markdownify_atx | markdownify(str(target), heading_style="ATX") (markdownify 1.2.3) | Port of MarkdownConverter with the defaults used by fetcher.py: ATX headings, paragraphs, ol/ul/li (with start), pre/code/kbd/samp, blockquote, hr, tables (thead inference, colspan, empty header rows), inline code/bold/italic/links (autolink)/images/line breaks, div/article/section/dl/dt/dd, del/s, q, sub/sup, script/style removal, \n{3,} collapse and strip in html_to_markdown. |
| html_to_markdown | fetcher._html_to_markdown | Glue: when extract=true, decompose 18+ tags and target = <main> or role="main" (only when stripped text length >= 500) else <body> else document. When extract=false the decomposition and main/body selection are skipped and the whole document is converted. |
| len_without_ws | len(text.replace(" ", "").replace("\n", "")) | Code-point count excluding U+0020/U+000A. |
| has_login_wall | fetcher._LOGIN_PATTERNS (regex, IGNORECASE) | Fixed-literal scan (all alternatives are literals). |
| normalize_url_for_request | url_safety.normalize_url_for_request | urlsplit/urlunsplit port, scheme-whitespace repair, IDNA host via idna_encode_host, urllib quote() safe sets. |
| sensitive_query_param_name | url_safety.sensitive_query_param_name | parse_qsl(keep_blank_values=True) over & pairs, non-empty values, 20-name table, returns the unquoted key. |
| url_contains_secret | url_safety.url_contains_secret | Raw + unquote + normalized candidates against the credential-prefix table (59 patterns incl. xapp-, xox, AKIA special cases) with (?<![A-Za-z0-9_-]) / (?![A-Za-z0-9_-]) boundaries. |
| is_blocked_hostname | url_safety._BLOCKED_HOSTNAMES | metadata.google.internal / metadata.goog (exact, lowercased, trailing-dot stripped). |
| classify_resolved_address | url_safety._is_blocked_ip + ipaddress | Pure IPv4/IPv6 parsers (incl. %scope strip, IPv4-mapped ::ffff:x.x.x.x), private/loopback/link-local/multicast/unspecified/reserved/CGNAT/public. |
| is_always_blocked_address | url_safety._ALWAYS_BLOCKED_IPS/_NETWORKS | 169.254.0.0/16 (+ mapped), 169.254.169.254/170.2/169.253, 100.100.100.200, fd00:ec2::254, ::ffff: variants. |
| is_safe_url_decision | url_safety.is_safe_url (pure part) | Scheme/hostname/blocked-hostname checks, DNS-failure proxy-delegation rule, per-address classification; DNS resolution itself stays Python. |
| idna_encode_host | hostname.encode("idna") (builtin codec) + RFC-3492 | ASCII labels unchanged; non-ASCII labels simple-lowercased + hand-written RFC-3492 punycode encoder (verified against Python goldens: münchen -> xn--mnchen-3ya, bücher, mañana, 例え, hätte, MÜNCHEN). |
| pick_encoding | — (no Python reference found; designed per plan) | Content-Type charset wins, then meta candidates; allow-list of common encodings; default utf-8. The actual decode stays Python. |
| FetchUrl (class) | CallableTool2-style binding entry point | Receives already-fetched HTML and dispatches to html_to_markdown. Serializes a JSON object with ok/markdown/error fields. |

## FetchUrl class contract

  Constructor: explicit FetchUrl(Session *session)
  Override:    void operator()(ToolParams const *parameters)
  Accessor:    kimix::vector<char> const &last_result() const

  Accepted parameters (all in the input ToolParams object):
    html         required string   HTML content to convert to Markdown
    url          optional string     original URL (currently metadata only)
    extract      optional bool       true  = decompose + main/body target
                                      false = convert the whole document
                                      (default true)
    max_length   optional int/uint   code-point cap on returned markdown
                                      (0 or absent = no cap)

  Serialized result fields:
    ok           bool                true on success
    markdown     string              resulting Markdown (present only when ok)
    error        string              human-readable diagnostic when not ok

## What stays in Python (documented, not ported)

  * Network I/O: httpx fetch, retries/backoff, SSL context building/verification,
    TLS 1.2 pinning (fetcher._fetch_html_http, fetch_html_http_with_fallback,
    _build_ssl_context).
  * Headless browser: Playwright launch/navigation/page.content() and the
    mobile-UA login-wall retry loop (fetcher._fetch_html, fetch_to_markdown).
  * DNS resolution: socket.getaddrinfo — the C++ is_safe_url_decision consumes an
    injected resolve_outcome (dns_failed + addresses).
  * Tool wrapper: aiohttp service fetch, ToolResultBuilder messages, micro
    compression / output export (fetch.py, fetch_url.py).
  * Raw byte decoding: the charset decision helper returns the encoding name; the
    actual decode (response.text / UnicodeDammit) stays Python.

## Deviations / approximations

  1. Numeric charref error cases: the installed bs4 4.15 runtime (pyc) returns
     the empty string for numeric references 0x00, > U+10FFFF, and surrogates
     (the shipped dammit.py source would return U+FFFD). The C++ mirrors the
     observable runtime behaviour (nothing appended) — verified against
     BeautifulSoup("&#1114112;"), &#0;, &#xD800;.
  2. markdownify convert_dd: the installed runtime prefixes : to the
     already-indented first line (: Definition) whereas the published source
     replaces the first indent char (:Definition). C++ follows the runtime.
  3. IDNA case folding: non-ASCII labels use a simple Latin-1 + ASCII lowercase
     mapping before punycode (the builtin idna codec applies full Nameprep).
     Covers all verified goldens; exotic case folds (e.g. ß) are not mapped.
  4. text_len_stripped whitespace: Python str.strip() approximated by the
     documented str.isspace() code-point ranges (space, tab, LF, VT, FF, CR,
     0x1C-0x1F, 0x85, 0xA0, 0x1680, 0x2000-0x200A, 0x2028/29, 0x202F, 0x205F,
     0x3000).
  5. find_previous_sibling (markdownify tr): bs4's no-arg find_previous_sibling()
     returns the previous sibling Tag; C++ mirrors that (skips whitespace text).
  6. MSVC source encoding: the sources are pure ASCII (all non-ASCII
     comments/strings transliterated or \x-escaped) because MSVC otherwise
     misreads UTF-8 as the GBK code page (C4819 / brace-count corruption).
  7. Attr entity decoding (decode_attr_entities): numeric references are always
     decoded; named references are decoded when exact + not followed by =
     (html.parser _unescape_attrvalue) — implemented without the
     &name=-trailing edge cases beyond the documented rule.
  8. HTML parser subset: html.parser has no implicit-close table, so the
     tokenizer intentionally produces nested li/td/p trees for unclosed siblings
     (matches bs4 html.parser). Pathological malformed markup
     (e.g. <div>x<y</div>) may serialize differently than bs4's recovery.
  9. FetchUrl class parameter schema: the plan snippet shows only url,
     extract, and max_length. The implemented class also requires an "html"
     parameter because the C++ layer is called after the Python side has already
     fetched the page. The "url" parameter is accepted but is currently metadata
     only; URL safety checks are invoked before fetching on the Python side.
  10. FetchUrl result delivery: Tool::operator() is void, so the serialized JSON
      is exposed through FetchUrl::last_result(). This matches the plan's note
      that "json_out is returned to the caller" while keeping the Tool base
      interface unchanged.

## Tests

  tests/unit/builtin_tools/test_fetch_url_tool.cpp — Boost.UT, main-scope
  "<snake_case>"_test lambdas only, HTML fixtures as inline string literals,
  punycode goldens verified against Python idna:

  * 55 tests, 217 asserts (expected after adding the FetchUrl class tests)
  * Coverage: tokenizer/DOM (14), DOM helpers (3), markdownify goldens (11),
    html_to_markdown glue (8), text stats (2), normalize_url_for_request (7),
    sensitive params + secret prefixes (2), SSRF classification/decision (4),
    idna/punycode (2), charset helper (1), FetchUrl class wrapper (5).

## Build/verify commands

  cd D:/KimiX-native
  python scripts/check_cpp_syntax.py src/builtin_tools/fetch_url_tool.h
  python scripts/check_cpp_syntax.py src/builtin_tools/fetch_url_tool.cpp
  python scripts/check_cpp_syntax.py tests/unit/builtin_tools/test_fetch_url_tool.cpp
  xmake f -m debug
  xmake build test_builtin_fetch_url
  xmake run test_builtin_fetch_url
