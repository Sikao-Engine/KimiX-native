// fetch_url_tool.h - fetch_url builtin agent tool kernels (C++ port).
//
// Source of truth (Python):
//   * C:/dev/kimi-agent/src/kimix/tools/web/web_fetcher/fetcher.py
//     (_html_to_markdown, fetch_to_markdown, login-wall + len-without-ws
//     statistics; uses bs4 html.parser + markdownify 1.2.3 heading_style=ATX)
//   * C:/dev/kimi-agent/kimi-cli/src/kimi_cli/tools/web/url_safety.py
//     (normalize_url_for_request, sensitive_query_param_name,
//     url_contains_secret, is_safe_url / _is_blocked_ip)
//   * C:/dev/kimi-agent/kimi-cli/src/kimi_cli/tools/web/fetch.py
//     (tool-level block messages; the HTTP/network path itself stays Python)
//
// Plan reference: C:/dev/kimi-agent/plans/fetch_url.md was expected to hold
// the C++ design (?3) and test list (?7), but the plans directory is empty at
// base commit 48bddc9. This port therefore follows AGENT_TASK.md + the Python
// sources above (recorded in src/builtin_tools/reports/fetch_url.md).
//
// Everything here is self-contained string/byte work: the DOM arena is owned
// by html_dom and no raw pointers cross the API. The httpx / Playwright /
// SSL / retry / DNS-resolution pieces stay in Python; the kernels below are
// the pure decision/transform functions the Python side calls.
//
// Namespace ownership: this tool owns every `url_safety` symbol; the
// web_search agent must not declare classify_resolved_address,
// is_blocked_hostname, normalize_url_for_request,
// sensitive_query_param_name, or url_contains_secret.

#pragma once

#include <cstdint>
#include <cstddef>

#include <core/kimix_core.h>

#include "builtin_tools/tool.h"
#include "builtin_tools/tool_types.h"

namespace kimix::builtin_tools::fetch_url {

// ---------------------------------------------------------------------------
// Limits (bounded arena -> tool_status::unsupported)
// ---------------------------------------------------------------------------
inline constexpr size_t k_max_dom_nodes = 200000u; // node arena cap
inline constexpr size_t k_max_dom_depth = 256u;    // nesting cap

// ---------------------------------------------------------------------------
// Light DOM arena (html.parser-like tree; bs4 subset)
// ---------------------------------------------------------------------------
enum class node_kind : uint8_t {
    root,     // "[document]" singleton
    element,  // tag_name + attrs + children
    text,     // decoded text content (raw for script/style RAWTEXT)
    comment,  // <!-- ... --> (ignored by get_text / markdownify)
    doctype,  // <!DOCTYPE ...> (ignored by get_text / markdownify)
};

struct dom_node {
    node_kind kind = node_kind::root;
    kimix::string tag_name;           // lowercased element name
    kimix::vector<named_value> attrs; // (name, value); names lowercased
    kimix::string text;               // text / comment / doctype content
    uint32_t parent = k_invalid_node; // arena index
    kimix::vector<uint32_t> children; // arena indices, document order
    bool removed = false;             // set by decompose()
};

struct html_dom {
    kimix::vector<dom_node> nodes;
    uint32_t root = k_invalid_node;

    const dom_node *at(uint32_t i) const {
        return (i < nodes.size()) ? &nodes[i] : nullptr;
    }
    dom_node *at(uint32_t i) {
        return (i < nodes.size()) ? &nodes[i] : nullptr;
    }
};

// Parse a full HTML document into the arena. `ok` on success; on
// tool_status::unsupported (depth/node budget exceeded) the caller must route
// to the Python fallback. Mirrors BeautifulSoup(html, "html.parser"):
// lowercased tag/attr names, void elements, self-closing `<tag/>`, end-tag
// pop-to-match, RAWTEXT (script/style/xmp/iframe/noembed/noframes) + RCDATA
// (textarea/title), named + numeric entity decoding, comments, doctype, and
// the whitespace-only-data collapse (ASCII whitespace chunk -> " " or "\n").
tool_error parse_html(kimix::string_view html, html_dom &out_dom);

// Remove every element whose tag is in `tag_names` together with its whole
// subtree (bs4 Tag.decompose()); removed nodes stay in the arena but are
// unreachable from `root`.
void decompose(html_dom &dom, kimix::span<const kimix::string_view> tag_names);

// First descendant element with the given tag name in document order
// (bs4 Tag.find(name)), or k_invalid_node. `root` may be any node.
uint32_t find_tag(const html_dom &dom, uint32_t root, kimix::string_view tag);

// First descendant element with an attribute `name == value`
// (bs4 Tag.find(name=value)), or k_invalid_node.
uint32_t find_attr(const html_dom &dom, uint32_t root,
                   kimix::string_view attr_name, kimix::string_view attr_value);

// Convenience aliases used by the html_to_markdown glue.
inline uint32_t find_main(const html_dom &dom, uint32_t root) {
    return find_tag(dom, root, "main");
}
inline uint32_t find_body(const html_dom &dom, uint32_t root) {
    return find_tag(dom, root, "body");
}

// Number of code points in the concatenation of descendant text nodes, with
// Python str.strip() applied to each text node before concatenation
// (bs4 get_text(strip=True) length). Comments/doctypes are excluded.
// Unicode whitespace is approximated by the Python str.isspace() code-point
// ranges (see report).
size_t text_len_stripped(const html_dom &dom, uint32_t node);

// Canonical str(soup) serialization for the supported constructs
// (bs4 minimal formatter): sorted attribute names, double-quoted values with
// `&`/`"` escaped (single-quoted when the value contains `"` only), void
// elements as `<tag/>`, comments `<!-- ... -->`, doctype `<!DOCTYPE ...>\n`.
kimix::string serialize_node(const html_dom &dom, uint32_t node);

// ---------------------------------------------------------------------------
// markdownify (ATX) + html_to_markdown glue
// ---------------------------------------------------------------------------

// Port of markdownify.MarkdownConverter with the defaults used by
// fetcher._html_to_markdown: heading_style="atx", autolinks=True,
// bullets="*+-", default_title=False, escape_* = True, escape_misc=False,
// newline_style="spaces", strip_document=strip, strip_pre=strip,
// strong_em_symbol="*", table_infer_header=False, wrap=False.
// `root` is the node to convert (the document root or a main/body target).
tool_error markdownify_atx(const html_dom &dom, uint32_t root,
                           kimix::string &out_markdown);

// Full glue: parse_html -> decompose(script/style/noscript/img/video/audio/
// source/track/iframe/embed/object/canvas/svg/picture/figure/nav/aside/
// footer/header) -> target = <main> or role="main" (only when its stripped
// text length >= 500) else <body> else document -> markdownify_atx ->
// collapse `\n{3,}` to `\n\n` -> strip.
// When `extract` is false the decomposition and main/body target selection are
// skipped and the whole document is converted.
tool_error html_to_markdown(kimix::string_view html,
                            kimix::string &out_markdown, bool extract = true);

// ---------------------------------------------------------------------------
// Text statistics used by the fetch decision (which fetcher to try)
// ---------------------------------------------------------------------------

// Number of code points excluding U+0020 and U+000A
// (Python: len(text.replace(" ", "").replace("\n", ""))).
size_t len_without_ws(kimix::string_view text);

// True when the text matches the fetcher login-wall keyword scan
// (fetcher._LOGIN_PATTERNS, re.IGNORECASE):
// denglu(login) | ??denglu(login) | ???denglu(login) | register | Sign in | Log in | Login |
// Verification code | sms-verification-code
bool has_login_wall(kimix::string_view text);

// ---------------------------------------------------------------------------
// url_safety (owned by fetch_url; web_search must not redeclare these)
// ---------------------------------------------------------------------------

// ASCII-safe HTTP URL for URL tools: strips whitespace, repairs
// "scheme:// " whitespace, lowercases nothing, converts an IRI host to
// IDNA ASCII (builtin `idna` codec semantics), percent-encodes
// path/query/fragment with the urllib quote() safe sets, and reassembles
// with urlunsplit. Non-http(s) schemes and parse failures pass through.
kimix::string normalize_url_for_request(kimix::string_view url);

// First sensitive query-parameter name (case-insensitive, unquoted) with a
// non-empty value, or nullopt. Mirrors url_safety.sensitive_query_param_name
// (parse_qsl keep_blank_values=True over '&'-separated pairs).
kimix::optional<kimix::string> sensitive_query_param_name(
    kimix::string_view url);

// True when the URL carries a recognizable credential: checks the raw URL,
// the unquoted URL, and the normalized URL against the credential-prefix
// table (url_safety.url_contains_secret / Hermes _PREFIX_RE).
bool url_contains_secret(kimix::string_view url);

// Blocked hostnames that are always refused (cloud metadata endpoints),
// matching url_safety._BLOCKED_HOSTNAMES. Comparison is exact on the
// lowercased, trailing-dot-stripped hostname.
bool is_blocked_hostname(kimix::string_view hostname);

enum class addr_class : uint8_t {
    invalid,       // unparseable
    public_addr,   // global unicast (allowed when allow_private is off)
    private_addr,  // RFC1918 / ULA (ipaddress.is_private)
    loopback,      // 127.0.0.0/8, ::1
    link_local,    // 169.254.0.0/16, fe80::/10
    multicast,     // 224.0.0.0/4, ff00::/8
    unspecified,   // 0.0.0.0, ::
    reserved,      // ipaddress.is_reserved
    cgnat,         // 100.64.0.0/10 (not covered by is_private/is_global)
};

// Pure IPv4/IPv6 classification of one sockaddr address string (a `%scope`
// suffix is stripped first). IPv4-mapped IPv6 (::ffff:x.x.x.x) is classified
// by its embedded IPv4 address, matching url_safety._is_blocked_ip.
addr_class classify_resolved_address(kimix::string_view ip);

// True when the address is in the always-blocked set even with
// KIMI_ALLOW_PRIVATE_URLS=true (cloud metadata IPs + link-local networks +
// IPv4-mapped variants, url_safety._ALWAYS_BLOCKED_IPS/_NETWORKS).
bool is_always_blocked_address(kimix::string_view ip);

// Outcome of the (Python-side) DNS resolution that is injected into the
// pure decision below.
struct resolve_outcome {
    bool dns_failed = false;                 // socket.gaierror
    kimix::vector<kimix::string> addresses;  // sockaddr[0] strings
};

// Pure port of url_safety.is_safe_url: scheme + hostname + blocked-hostname
// checks, DNS-failure handling (literal IPs never qualify for the
// proxy-delegation escape), per-address always-blocked / private checks.
// `allow_all_private` mirrors KIMI_ALLOW_PRIVATE_URLS (metadata floor still
// applies). Fail-closed on any unexpected condition.
bool is_safe_url_decision(kimix::string_view url, bool allow_all_private,
                          bool proxy_configured,
                          const resolve_outcome &resolved);

// ---------------------------------------------------------------------------
// IDNA host encoding (RFC-3492 punycode; stdlib `idna` codec semantics)
// ---------------------------------------------------------------------------

// Encode one hostname to ASCII: ASCII-only labels are returned unchanged;
// labels containing non-ASCII code points are lowercased (simple mapping) and
// RFC-3492 punycoded with the "xn--" prefix. Returns false on overflow /
// unencodable label (mirrors the UnicodeError path in normalize_url_for_request
// which then keeps the original host); on success `out` receives the result.
bool idna_encode_host(kimix::string_view host, kimix::string &out);

// ---------------------------------------------------------------------------
// Charset decision helper (decode itself stays Python)
// ---------------------------------------------------------------------------

// Given the HTTP Content-Type header value and the HTML <meta> encoding
// candidates (charset attribute values / http-equiv content=...;charset=...),
// return the encoding name to decode with (lowercased, surrounding quotes and
// whitespace removed) or an empty string to use the default. Priority: first
// valid charset from Content-Type, then the first valid candidate; the string
// "utf-8" is assumed when nothing usable is found. Only a small allow-list of
// encoding spellings is recognized (utf-8, utf8, ascii, us-ascii, latin-1,
// iso-8859-1, windows-1252, gbk, gb2312, big5, shift-jis, euc-jp, euc-kr).
kimix::string pick_encoding(
    kimix::string_view content_type,
    kimix::span<const kimix::string_view> meta_candidates);

// ---------------------------------------------------------------------------
// Tool class wrapper (CallableTool2-style binding entry point)
// ---------------------------------------------------------------------------

// Concrete built-in tool implementation used by the binding layer. It receives
// already-fetched HTML in the parameters and dispatches to the pure kernels
// above; network I/O stays in Python.
//
// Accepted parameters:
//   html         required string   HTML content to convert to Markdown
//   url          optional string     original URL (metadata only in this port)
//   extract      optional bool       when true/false select main/body target
//                                     (default true)
//   max_length   optional int/uint   code-point cap on the returned markdown
//                                     (0 or absent means no cap)
//
// Serialized result fields:
//   ok           bool                true when conversion succeeded
//   markdown     string              resulting Markdown (omitted when not ok)
//   error        string              human-readable diagnostic when not ok
class FetchUrl : public kimix::builtin_tools::Tool {
public:
    explicit FetchUrl(Session *session);
    void operator()(ToolParams const *parameters) override;

    // Access the serialized JSON produced by the last operator() invocation.
    kimix::vector<char> const &last_result() const { return _last_result; }

private:
    kimix::vector<char> _last_result;
};

} // namespace kimix::builtin_tools::fetch_url
