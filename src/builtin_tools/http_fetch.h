// http_fetch.h - Native HTTP(S) GET for the web-facing built-in tools.
//
// The fetch_url / web_search tools registered their agent-facing contracts
// ("Fetch a web page ...", "Search the web ...") but only implemented the pure
// kernels that receive already-fetched content - the actual network I/O was
// never ported (bug_tool.md items 4/5: both tools answered empty). This
// module supplies the missing transport on top of the hand-written
// kimix::net HTTP(S) client (llm/http_client.h, raw sockets + the vendored
// mbedtls - the same stack the LLM clients use):
//   * http(s) URLs only (the registered fetch_url contract),
//   * the url_safety gate before any byte is sent (scheme, blocked hostnames,
//     DNS resolution + private/loopback/metadata classification),
//   * bounded timeouts, bounded response size, redirect following.

#pragma once

#include <core/kimix_core.h>

namespace kimix::builtin_tools::http_fetch {

// Outcome of one GET. `ok` is true only for a 2xx response with a body;
// every failure carries a non-empty, model-presentable `error`.
struct fetch_result {
    bool ok = false;
    int status = 0; // HTTP status code (0 == transport failure)
    kimix::string body;
    kimix::string error;
    kimix::string final_url; // the URL actually requested (after normalization)
    // True when `error` is an EXPECTED refusal (a working tool correctly
    // rejecting the input: a non-http scheme, a blocked hostname, an SSRF
    // gate hit, or a host that does not resolve), not an unexpected runtime
    // fault. bug_tool.md item 4: the caller must not render expected
    // refusals as generic runtime errors ("the tool is probably not
    // working.").
    bool expected_refusal = false;
};

// Blocking GET with the url_safety gate applied. Follows redirects (each hop
// re-enters the safety gate). timeout_ms bounds the whole exchange.
fetch_result get(kimix::string_view url, int timeout_ms = 30000,
                 size_t max_body_bytes = 2u * 1024 * 1024);

// Percent-encode a query string component (application/x-www-form-urlencoded,
// like urllib.parse.quote_plus): spaces become '+', everything outside the
// unreserved set is %-escaped byte-wise over the UTF-8 form.
kimix::string urlencode_component(kimix::string_view value);

} // namespace kimix::builtin_tools::http_fetch
