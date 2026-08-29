// Test for the fetch_url builtin tool kernels (builtin_tools/fetch_url_tool.h).
//
// Covers the plan scope:
//  - tolerant HTML tokenizer + arena DOM (void elements, self-closing tags,
//    end-tag pop-to-match, entities, comments/doctype, RAWTEXT/RCDATA,
//    whitespace-only data collapse, bounded depth/node count -> unsupported)
//  - decompose / find_tag / find_attr / text_len_stripped / serialize_node
//  - markdownify_atx + html_to_markdown glue (goldens captured from
//    markdownify 1.2.3 with heading_style="ATX" and bs4 4.15 html.parser)
//  - len_without_ws + has_login_wall
//  - url_safety: normalize_url_for_request, sensitive_query_param_name,
//    url_contains_secret, classify_resolved_address, is_blocked_hostname,
//    is_always_blocked_address, is_safe_url_decision
//  - idna/RFC-3492 punycode host encoding (goldens from Python idna)
//  - pick_encoding charset decision helper
//
// All HTML fixtures are inline string literals; no file/network access.
#include "ut/ut.hpp"

#include "builtin_tools/fetch_url_tool.h"

#include <string>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::builtin_tools;
namespace fu = kimix::builtin_tools::fetch_url;

namespace {
namespace fu_using = kimix::builtin_tools::fetch_url;
using namespace fu_using;

kimix::string parse_text(kimix::string_view html, bool &ok) {
    html_dom dom;
    tool_error err = fu::parse_html(html, dom);
    ok = !err.failed();
    return fu::serialize_node(dom, dom.root);
}

kimix::string md(kimix::string_view html) {
    kimix::string out;
    tool_error err = fu::html_to_markdown(html, out);
    if (err.failed()) return kimix::string("<ERR>");
    return out;
}

kimix::string md_dom(kimix::string_view html) {
    html_dom dom;
    tool_error err = fu::parse_html(html, dom);
    if (err.failed()) return kimix::string("<ERR>");
    kimix::string out;
    err = fu::markdownify_atx(dom, dom.root, out);
    if (err.failed()) return kimix::string("<ERR>");
    return out;
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    // ---------------------------------------------------------------------
    // Tokenizer / DOM basics
    // ---------------------------------------------------------------------
    "tokenizer_basic_structure"_test = [] {
        bool ok = false;
        kimix::string s = parse_text(
            "<div class=\"x\">A <b>bold</b> C</div>", ok);
        expect(ok);
        expect(eq(s, kimix::string("<div class=\"x\">A <b>bold</b> C</div>")));
    };

    "tokenizer_lowercases_names_and_sorts_attrs"_test = [] {
        bool ok = false;
        kimix::string s = parse_text("<DIV B=\"2\" a=\"1\">x</DIV>", ok);
        expect(ok);
        expect(eq(s, kimix::string("<div a=\"1\" b=\"2\">x</div>")));
    };

    "tokenizer_void_elements_self_close"_test = [] {
        bool ok = false;
        kimix::string s = parse_text(
            "<p>a<br>b<img src=\"i.png\" alt=\"pic\"></p>", ok);
        expect(ok);
        expect(eq(s, kimix::string(
                         "<p>a<br/>b<img alt=\"pic\" src=\"i.png\"/></p>")));
    };

    "tokenizer_self_closing_non_void"_test = [] {
        bool ok = false;
        kimix::string s = parse_text("<div/>x", ok);
        expect(ok);
        expect(eq(s, kimix::string("<div></div>x")));
    };

    "tokenizer_end_tag_pops_to_match"_test = [] {
        bool ok = false;
        kimix::string s = parse_text("<ul><li>a<li>b</ul>", ok);
        expect(ok);
        // html.parser nests li (no implicit close table) and </ul> pops.
        expect(eq(s, kimix::string("<ul><li>a<li>b</li></li></ul>")));
    };

    "tokenizer_entities_named_and_numeric"_test = [] {
        bool ok = false;
        kimix::string s = parse_text(
            "<p>&amp; &#65; &#x42; &lt;tag&gt; &nbsp; &copy; &apos; &quot; "
            "&unknown;</p>",
            ok);
        expect(ok);
        expect(eq(s, kimix::string(
                         "<p>&amp; A B &lt;tag&gt; \xC2\xA0 \xC2\xA9 \' \" "
                         "&amp;unknown</p>")));
    };

    "tokenizer_numeric_charref_edges"_test = [] {
        bool ok = false;
        // 0x80 -> windows-1252 euro; 0x9 tab survives inside a non-whitespace
        // segment (bs4 collapses only all-ASCII-whitespace segments).
        expect(eq(parse_text("<p>&#x80; &#x9;</p>", ok),
                  kimix::string("<p>\xE2\x82\xAC \t</p>")));
        expect(ok);
        // A lone tab-only segment collapses to a space.
        expect(eq(parse_text("<p>&#x9;</p>", ok),
                  kimix::string("<p> </p>")));
        expect(ok);
        // null / out-of-range / surrogate decode to nothing in the installed
        // bs4 runtime; the remaining space segment collapses to " ".
        expect(eq(parse_text("<p>&#1114112; &#xD800;</p>", ok),
                  kimix::string("<p> </p>")));
        expect(ok);
        // unknown hex name stays literal
        expect(eq(parse_text("<p>&#xZZ;</p>", ok),
                  kimix::string("<p>&amp;#xZZ;</p>")));
        expect(ok);
        // incomplete decimal followed by hex digit swallows the rest (bs4)
        expect(eq(parse_text("<p>&#65f<b>y</b></p>", ok),
                  kimix::string("<p>Af&lt;b&gt;y&lt;/b&gt;&lt;/p&gt;</p>")));
        expect(ok);
    };

    "tokenizer_script_rawtext_style"_test = [] {
        bool ok = false;
        kimix::string s = parse_text(
            "<script>if (a < b) { x = '&amp;'; }</script>", ok);
        expect(ok);
        // script content is raw: entities NOT decoded, '< b' not a tag.
        expect(eq(s, kimix::string(
                         "<script>if (a < b) { x = '&amp;'; }</script>")));
    };

    "tokenizer_title_rcdata_decodes_entities"_test = [] {
        bool ok = false;
        kimix::string s = parse_text("<title>A &amp; B</title>", ok);
        expect(ok);
        expect(eq(s, kimix::string("<title>A &amp; B</title>")));
        // The stored text node is decoded.
        html_dom dom;
        tool_error err = fu::parse_html("<title>A &amp; B</title>", dom);
        expect(!err.failed());
        uint32_t title_child =
            dom.nodes[dom.nodes[dom.root].children[0]].children[0];
        expect(eq(dom.nodes[title_child].text, kimix::string("A & B")));
    };

    "tokenizer_comment_and_doctype"_test = [] {
        bool ok = false;
        kimix::string s = parse_text("<!-- c --><!DOCTYPE html><p>x</p>", ok);
        expect(ok);
        expect(eq(s, kimix::string(
                         "<!-- c --><!DOCTYPE html>\n<p>x</p>")));
    };

    "tokenizer_bogus_decl_is_comment"_test = [] {
        bool ok = false;
        kimix::string s = parse_text("<!bogus stuff>", ok);
        expect(ok);
        expect(eq(s, kimix::string("<!--bogus stuff-->")));
    };

    "tokenizer_whitespace_only_data_collapses"_test = [] {
        bool ok = false;
        // bs4 endData: an all-ASCII-whitespace segment becomes " " or "\n".
        expect(eq(parse_text("<p>\t\n\r  </p>", ok),
                  kimix::string("<p>\n</p>")));
        expect(ok);
        expect(eq(parse_text("<p>  </p>", ok),
                  kimix::string("<p> </p>")));
        expect(ok);
    };

    "tokenizer_plain_ampersand_and_unknown_entities"_test = [] {
        bool ok = false;
        expect(eq(parse_text("<p>a & b &ampx foo</p>", ok),
                  kimix::string("<p>a &amp; b &amp;ampx foo</p>")));
        expect(ok);
    };

    "tokenizer_depth_budget_unsupported"_test = [] {
        html_dom dom;
        kimix::string html;
        html.reserve(fu::k_max_dom_depth * 10);
        for (size_t i = 0; i < fu::k_max_dom_depth + 2; ++i) {
            html += "<div>";
        }
        for (size_t i = 0; i < fu::k_max_dom_depth + 2; ++i) {
            html += "</div>";
        }
        tool_error err = fu::parse_html(html, dom);
        expect(err.status == tool_status::unsupported);
    };

    // ---------------------------------------------------------------------
    // DOM navigation helpers
    // ---------------------------------------------------------------------
    "find_tag_and_find_attr"_test = [] {
        html_dom dom;
        tool_error err = fu::parse_html(
            "<html><body><div role=\"main\">M</div><main>MAIN</main></body></html>",
            dom);
        expect(!err.failed());
        uint32_t main = fu::find_tag(dom, dom.root, "main");
        expect(main != k_invalid_node);
        expect(eq(fu::serialize_node(dom, main),
                  kimix::string("<main>MAIN</main>")));
        uint32_t role = fu::find_attr(dom, dom.root, "role", "main");
        expect(role != k_invalid_node);
        expect(eq(fu::serialize_node(dom, role),
                  kimix::string("<div role=\"main\">M</div>")));
        expect(eq(fu::find_tag(dom, dom.root, "nonexistent"),
                  uint32_t(k_invalid_node)));
    };

    "decompose_removes_subtrees"_test = [] {
        html_dom dom;
        tool_error err = fu::parse_html(
            "<html><body><nav>nav</nav><p>keep</p><script>var x=1;</script></body></html>",
            dom);
        expect(!err.failed());
        static const kimix::string_view kTags[] = {"nav", "script"};
        fu::decompose(dom, kimix::span<const kimix::string_view>(kTags, 2));
        expect(eq(fu::serialize_node(dom, dom.root),
                  kimix::string("<html><body><p>keep</p></body></html>")));
    };

    "text_len_stripped_counts_code_points"_test = [] {
        html_dom dom;
        tool_error err = fu::parse_html(
            "<div>  hello \n world  <p>  x </p> </div>", dom);
        expect(!err.failed());
        uint32_t div = fu::find_tag(dom, dom.root, "div");
        // bs4 get_text(strip=True) == "hello \n worldx" (14 code points)
        expect(eq(fu::text_len_stripped(dom, div), size_t(14)));
        // Non-ASCII counts code points, not bytes: "hello" = 5
        html_dom dom2;
        err = fu::parse_html("<p>h\xC3\xA9llo</p>", dom2);
        expect(!err.failed());
        uint32_t p = fu::find_tag(dom2, dom2.root, "p");
        expect(eq(fu::text_len_stripped(dom2, p), size_t(5)));
    };

    // ---------------------------------------------------------------------
    // markdownify_atx goldens (Python markdownify 1.2.3, heading_style=ATX)
    // ---------------------------------------------------------------------
    "markdownify_headings"_test = [] {
        expect(eq(md_dom("<h1>Title</h1><h2>Sub</h2><h3>Sub3</h3><h6>six</h6>"),
                  kimix::string("# Title\n\n## Sub\n\n### Sub3\n\n###### six")));
    };

    "markdownify_paragraph_and_inline"_test = [] {
        expect(eq(md_dom("<p>Hello <b>bold</b> and <i>italic</i> and "
                         "<code>code</code>.</p>"),
                  kimix::string(
                      "Hello **bold** and *italic* and `code`.")));
    };

    "markdownify_links_and_images"_test = [] {
        expect(eq(md_dom("<p>Visit <a href=\"https://example.com\" "
                         "title=\"T\">Example</a></p>"),
                  kimix::string(
                      "Visit [Example](https://example.com \"T\")")));
        expect(eq(md_dom("<p>Visit <a href=\"https://example.com\">"
                         "https://example.com</a></p>"),
                  kimix::string("Visit <https://example.com>")));
        expect(eq(md_dom("<p><img src=\"a.png\" alt=\"Alt text\" "
                         "title=\"t\"></p>"),
                  kimix::string("![Alt text](a.png \"t\")")));
    };

    "markdownify_br_and_del"_test = [] {
        expect(eq(md_dom("<p>a<br>b</p>"),
                  kimix::string("a  \nb")));
        expect(eq(md_dom("<p>a <del>del</del> b</p>"),
                  kimix::string("a ~~del~~ b")));
    };

    "markdownify_lists"_test = [] {
        expect(eq(md_dom("<ul><li>one</li><li>two</li></ul>"),
                  kimix::string("* one\n* two")));
        expect(eq(md_dom("<ul><li>one<ul><li>inner</li></ul></li>"
                         "<li>two</li></ul>"),
                  kimix::string("* one\n  + inner\n* two")));
        expect(eq(md_dom("<ol><li>first</li><li>second</li></ol>"),
                  kimix::string("1. first\n2. second")));
        expect(eq(md_dom("<ol start=\"5\"><li>five</li><li>six</li></ol>"),
                  kimix::string("5. five\n6. six")));
    };

    "markdownify_pre_code_blockquote_hr"_test = [] {
        expect(eq(md_dom("<pre>line1\nline2</pre>"),
                  kimix::string("```\nline1\nline2\n```")));
        expect(eq(md_dom("<pre><code>int x = 1;\nreturn x;</code></pre>"),
                  kimix::string("```\nint x = 1;\nreturn x;\n```")));
        expect(eq(md_dom("<blockquote>quoted text</blockquote>"),
                  kimix::string("> quoted text")));
        expect(eq(md_dom("<blockquote>line1<br>line2</blockquote>"),
                  kimix::string("> line1  \n> line2")));
        expect(eq(md_dom("<hr>"), kimix::string("---")));
    };

    "markdownify_tables"_test = [] {
        expect(eq(md_dom("<table><thead><tr><th>H1</th><th>H2</th></tr>"
                         "</thead><tbody><tr><td>a</td><td>b</td></tr>"
                         "</tbody></table>"),
                  kimix::string("| H1 | H2 |\n| --- | --- |\n| a | b |")));
        expect(eq(md_dom("<table><tr><td>a</td><td>b</td></tr></table>"),
                  kimix::string("|  |  |\n| --- | --- |\n| a | b |")));
        expect(eq(md_dom("<table><tr><td colspan=2>a</td><td>b</td></tr>"
                         "</table>"),
                  kimix::string("|  |  |  |\n| --- | --- | --- |\n"
                                "| a | | b |")));
    };

    "markdownify_div_and_dl_and_misc"_test = [] {
        expect(eq(md_dom("<div>one</div><div>two</div>"),
                  kimix::string("one\n\ntwo")));
        expect(eq(md_dom("<dl><dt>Term</dt><dd>Definition</dd></dl>"),
                  kimix::string("Term\n: Definition")));
        expect(eq(md_dom("<p>press <kbd>Ctrl</kbd> and <samp>out</samp></p>"),
                  kimix::string("press `Ctrl` and `out`")));
        expect(eq(md_dom("<p>a <q>quote</q> b</p>"),
                  kimix::string("a \"quote\" b")));
    };

    "markdownify_script_style_removed"_test = [] {
        expect(eq(md_dom("<p>a</p><script>var x=1;</script><p>b</p>"),
                  kimix::string("a\n\nb")));
        expect(eq(md_dom("<p>a</p><style>.x{}</style><p>b</p>"),
                  kimix::string("a\n\nb")));
    };

    "markdownify_nested_p_and_heading_ws"_test = [] {
        expect(eq(md_dom("<p>a<p>b"), kimix::string("a\n\nb")));
        expect(eq(md_dom("<h1>  spaced  </h1>"),
                  kimix::string("# spaced")));
        expect(eq(md_dom("<h2><img src=\"i.png\" alt=\"alt\"></h2>"),
                  kimix::string("## alt")));
    };

    // ---------------------------------------------------------------------
    // html_to_markdown glue (fetcher._html_to_markdown port)
    // ---------------------------------------------------------------------
    "html_to_markdown_basic"_test = [] {
        expect(eq(md("<html><body><main><h1>Title</h1><p>Hello</p></main>"
                     "</body></html>"),
                  kimix::string("# Title\n\nHello")));
    };

    "html_to_markdown_main_needs_500_chars"_test = [] {
        // Short <main> falls back to <body>.
        expect(eq(md("<html><body><main>short</main><div>body content here "
                     "<b>x</b></div></body></html>"),
                  kimix::string("short\n\nbody content here **x**")));
        // Long <main> (>= 500 stripped chars) is used instead of <body>.
        kimix::string html("<html><body><main>");
        html.append(600, 'x');
        html += " word</main><div>body</div></body></html>";
        kimix::string out;
        tool_error err = fu::html_to_markdown(html, out);
        expect(!err.failed());
        expect(eq(out, kimix::string(600, 'x') + " word"));
    };

    "html_to_markdown_role_main"_test = [] {
        expect(eq(md("<html><body><div role=\"main\"><h1>Role</h1>"
                     "<p>content</p></div><div>side</div></body></html>"),
                  kimix::string("# Role\n\ncontent\n\nside")));
    };

    "html_to_markdown_no_body_uses_root"_test = [] {
        expect(eq(md("<h1>T</h1><p>para</p>"),
                  kimix::string("# T\n\npara")));
    };

    "html_to_markdown_decompose_list"_test = [] {
        expect(eq(md("<html><body><script>var x=1</script><style>.a{}</style>"
                     "<nav>nav</nav><h1>T</h1><p>P</p><img src='x.png'>"
                     "</body></html>"),
                  kimix::string("# T\n\nP")));
    };

    "html_to_markdown_newline_collapse_and_strip"_test = [] {
        expect(eq(md("<html><body><div>a</div><div>b</div><div>c</div>"
                     "</body></html>"),
                  kimix::string("a\n\nb\n\nc")));
    };

    // ---------------------------------------------------------------------
    // Text statistics
    // ---------------------------------------------------------------------
    "len_without_ws_counts_code_points"_test = [] {
        expect(eq(fu::len_without_ws("a b\nc  \n"), size_t(3)));
        expect(eq(fu::len_without_ws("h\xC3\xA9llo world"), size_t(10)));
        expect(eq(fu::len_without_ws(""), size_t(0)));
        expect(eq(fu::len_without_ws("   \n"), size_t(0)));
    };

    "has_login_wall_keywords"_test = [] {
        expect(fu::has_login_wall("Please Login to continue"));
        expect(fu::has_login_wall("please sign in here"));
        expect(fu::has_login_wall("LOG IN"));
        expect(fu::has_login_wall("Verification code required"));
        expect(fu::has_login_wall(
            "\xE9\xAA\x8C\xE8\xAF\x81\xE7\xA0\x81\xE7\x99\xBB\xE5\xBD\x95")); // ???denglu(login)
        expect(fu::has_login_wall(
            "\xE6\xB3\xA8\xE5\x86\x8C")); // register
        expect(!fu::has_login_wall("plain text"));
        expect(!fu::has_login_wall(""));
    };

    // ---------------------------------------------------------------------
    // normalize_url_for_request
    // ---------------------------------------------------------------------
    "normalize_idna_host"_test = [] {
        expect(eq(fu::normalize_url_for_request("https://m\xC3\xBCnchen.de/path"),
                  kimix::string("https://xn--mnchen-3ya.de/path")));
    };

    "normalize_non_ascii_path_percent_encoded"_test = [] {
        expect(eq(fu::normalize_url_for_request(
                      "https://wttr.in/K\xC3\xB6ln"),
                  kimix::string("https://wttr.in/K%C3%B6ln")));
    };

    "normalize_whitespace_after_scheme_repaired"_test = [] {
        expect(eq(fu::normalize_url_for_request("https:// docs.example"),
                  kimix::string("https://docs.example")));
    };

    "normalize_non_http_scheme_passthrough"_test = [] {
        expect(eq(fu::normalize_url_for_request("ftp://example.com/file"),
                  kimix::string("ftp://example.com/file")));
    };

    "normalize_query_whitespace_percent_encoded"_test = [] {
        expect(eq(fu::normalize_url_for_request("https://example.com/?q=a b&x=1"),
                  kimix::string("https://example.com/?q=a%20b&x=1")));
    };

    "normalize_empty_and_full_url"_test = [] {
        expect(eq(fu::normalize_url_for_request(""), kimix::string("")));
        expect(eq(fu::normalize_url_for_request(
                      "https://user:pass@example.com:8080/p?q=1#f"),
                  kimix::string(
                      "https://user:pass@example.com:8080/p?q=1#f")));
        expect(eq(fu::normalize_url_for_request(
                      "http://\xE4\xBE\x8B\xE3\x81\x88.jp/"
                      "\xE3\x83\x91\xE3\x82\xB9?q=\xE5\x80\xA4"),
                  kimix::string("http://xn--r8jz45g.jp/"
                                "%E3%83%91%E3%82%B9?q=%E5%80%A4")));
    };

    // ---------------------------------------------------------------------
    // sensitive_query_param_name + url_contains_secret
    // ---------------------------------------------------------------------
    "sensitive_query_param_name"_test = [] {
        auto check = [](kimix::string_view url,
                        kimix::string_view expected) {
            auto got = fu::sensitive_query_param_name(url);
            if (expected.empty()) {
                expect(!got.has_value());
            } else {
                expect(got.has_value());
                if (got) expect(eq(*got, kimix::string(expected)));
            }
        };
        check("https://x.com/?token=abc", "token");
        check("https://x.com/?api_key=abc", "api_key");
        check("https://x.com/?q=hello", "");
        check("ftp://x.com/?token=abc", "");
        check("https://x.com/noquery", "");
        // empty value is not flagged
        check("https://x.com/?token=", "");
        // percent-encoded / case-insensitive key (Python returns the
        // unquoted key with its original case)
        check("https://x.com/?%74oken=abc", "token");
        check("https://x.com/?ToKeN=abc", "ToKeN");
        check("https://x.com/?x-amz-signature=sig", "x-amz-signature");
    };

    "url_contains_secret_known_prefixes"_test = [] {
        expect(fu::url_contains_secret("https://x.com/?k=sk-ABC1234567890"));
        expect(fu::url_contains_secret("https://x.com/?k=ghp_1234567890"));
        expect(fu::url_contains_secret(
            "https://x.com/?k=AIzaSy0123456789_abcdefghijklmnopqrstuvwxyz"));
        // percent-encoded secret caught via unquote
        expect(fu::url_contains_secret(
            "https://x.com/?k=sk-%41BCDEFGHIJKL"));
        expect(!fu::url_contains_secret("https://x.com/"));
        expect(!fu::url_contains_secret("https://x.com/?q=hello"));
        expect(!fu::url_contains_secret(""));
    };

    // ---------------------------------------------------------------------
    // SSRF address classification
    // ---------------------------------------------------------------------
    "classify_resolved_address_ipv4"_test = [] {
        expect(fu::classify_resolved_address("93.184.216.34") ==
               fu::addr_class::public_addr);
        expect(fu::classify_resolved_address("127.0.0.1") ==
               fu::addr_class::loopback);
        expect(fu::classify_resolved_address("10.0.0.1") ==
               fu::addr_class::private_addr);
        expect(fu::classify_resolved_address("172.16.0.1") ==
               fu::addr_class::private_addr);
        expect(fu::classify_resolved_address("192.168.1.1") ==
               fu::addr_class::private_addr);
        expect(fu::classify_resolved_address("169.254.169.254") ==
               fu::addr_class::link_local);
        expect(fu::classify_resolved_address("100.64.0.1") ==
               fu::addr_class::cgnat);
        expect(fu::classify_resolved_address("224.0.0.1") ==
               fu::addr_class::multicast);
        expect(fu::classify_resolved_address("0.0.0.0") ==
               fu::addr_class::unspecified);
        expect(fu::classify_resolved_address("240.0.0.1") ==
               fu::addr_class::reserved);
        expect(fu::classify_resolved_address("not-an-ip") ==
               fu::addr_class::invalid);
    };

    "classify_resolved_address_ipv6"_test = [] {
        expect(fu::classify_resolved_address("::1") == fu::addr_class::loopback);
        expect(fu::classify_resolved_address("::") ==
               fu::addr_class::unspecified);
        expect(fu::classify_resolved_address("fe80::1") ==
               fu::addr_class::link_local);
        expect(fu::classify_resolved_address("fd00::1") ==
               fu::addr_class::private_addr);
        expect(fu::classify_resolved_address("ff02::1") ==
               fu::addr_class::multicast);
        expect(fu::classify_resolved_address(
                   "2606:2800:220:1:248:1893:25c8:1946") ==
               fu::addr_class::public_addr);
        expect(fu::classify_resolved_address("2001:db8::1") ==
               fu::addr_class::reserved);
        // IPv4-mapped IPv6 classifies by the embedded IPv4
        expect(fu::classify_resolved_address("::ffff:127.0.0.1") ==
               fu::addr_class::loopback);
        expect(fu::classify_resolved_address("::ffff:10.0.0.1") ==
               fu::addr_class::private_addr);
        expect(fu::classify_resolved_address("::ffff:169.254.169.254") ==
               fu::addr_class::link_local);
        expect(fu::classify_resolved_address("::ffff:93.184.216.34") ==
               fu::addr_class::public_addr);
        // scope id stripped
        expect(fu::classify_resolved_address("fe80::1%eth0") ==
               fu::addr_class::link_local);
    };

    "is_blocked_hostname_and_always_blocked"_test = [] {
        expect(fu::is_blocked_hostname("metadata.google.internal"));
        expect(fu::is_blocked_hostname("METADATA.GOOGLE.INTERNAL."));
        expect(fu::is_blocked_hostname("metadata.goog"));
        expect(!fu::is_blocked_hostname("example.com"));
        expect(fu::is_always_blocked_address("169.254.169.254"));
        expect(fu::is_always_blocked_address("169.254.170.2"));
        expect(fu::is_always_blocked_address("100.100.100.200"));
        expect(fu::is_always_blocked_address("::ffff:169.254.169.254"));
        expect(fu::is_always_blocked_address("fd00:ec2::254"));
        expect(!fu::is_always_blocked_address("93.184.216.34"));
    };

    "is_safe_url_decision"_test = [] {
        // public IP allowed
        fu::resolve_outcome pub;
        pub.addresses.push_back("93.184.216.34");
        expect(fu::is_safe_url_decision("http://93.184.216.34/", false, false,
                                        pub));
        // loopback blocked
        fu::resolve_outcome loop;
        loop.addresses.push_back("127.0.0.1");
        expect(!fu::is_safe_url_decision("http://127.0.0.1/", false, false,
                                         loop));
        // metadata blocked even with the private override
        fu::resolve_outcome meta;
        meta.addresses.push_back("169.254.169.254");
        expect(!fu::is_safe_url_decision("http://169.254.169.254/", true, false,
                                         meta));
        // private IP blocked by default, allowed with override
        fu::resolve_outcome priv;
        priv.addresses.push_back("10.0.0.1");
        expect(!fu::is_safe_url_decision("http://10.0.0.1/", false, false, priv));
        expect(fu::is_safe_url_decision("http://10.0.0.1/", true, false, priv));
        // DNS failure fails closed; proxy delegates non-literal hosts
        fu::resolve_outcome fail;
        fail.dns_failed = true;
        expect(!fu::is_safe_url_decision("https://example.com/", false, false,
                                         fail));
        expect(fu::is_safe_url_decision("https://example.com/", false, true,
                                        fail));
        expect(!fu::is_safe_url_decision("http://10.0.0.1/", false, true, fail));
        // unsupported scheme / empty host
        expect(!fu::is_safe_url_decision("ftp://example.com/file", false, false,
                                         pub));
        expect(!fu::is_safe_url_decision("https:///path", false, false, pub));
        expect(!fu::is_safe_url_decision("", false, false, pub));
        // blocked hostname always blocked
        expect(!fu::is_safe_url_decision("http://metadata.google.internal/",
                                         true, false, pub));
    };

    // ---------------------------------------------------------------------
    // IDNA / punycode
    // ---------------------------------------------------------------------
    "idna_ascii_fast_path"_test = [] {
        kimix::string out;
        expect(fu::idna_encode_host("example.com", out));
        expect(eq(out, kimix::string("example.com")));
        // Builtin idna codec keeps ASCII case.
        expect(fu::idna_encode_host("EXAMPLE.com", out));
        expect(eq(out, kimix::string("EXAMPLE.com")));
        expect(fu::idna_encode_host("www.xn--mnchen-3ya.de", out));
        expect(eq(out, kimix::string("www.xn--mnchen-3ya.de")));
    };

    "idna_punycode_goldens"_test = [] {
        kimix::string out;
        // Verified against Python: "munchen".encode("idna") == b'xn--mnchen-3ya'
        expect(fu::idna_encode_host("m\xC3\xBCnchen", out));
        expect(eq(out, kimix::string("xn--mnchen-3ya")));
        // NB: string literals are split because MSVC's \x escape consumes
        // following hex digits ("\xBCcher" would read "\xBCc").
        expect(fu::idna_encode_host("b\xC3\xBC" "cher", out));
        expect(eq(out, kimix::string("xn--bcher-kva")));
        expect(fu::idna_encode_host("ma\xC3\xB1" "ana", out));
        expect(eq(out, kimix::string("xn--maana-pta")));
        expect(fu::idna_encode_host(
                    "\xE4\xBE\x8B\xE3\x81\x88.jp", out));
        expect(eq(out, kimix::string("xn--r8jz45g.jp")));
        expect(fu::idna_encode_host("M\xC3\x9CNCHEN", out));
        expect(eq(out, kimix::string("xn--mnchen-3ya")));
        expect(fu::idna_encode_host("h\xC3\xA4tte", out));
        expect(eq(out, kimix::string("xn--htte-loa")));
    };

    // ---------------------------------------------------------------------
    // Charset decision helper
    // ---------------------------------------------------------------------
    "pick_encoding_priority"_test = [] {
        kimix::span<const kimix::string_view> kNone;
        expect(eq(fu::pick_encoding("text/html; charset=utf-8", kNone),
                  kimix::string("utf-8")));
        expect(eq(fu::pick_encoding("text/html; charset=ISO-8859-1", kNone),
                  kimix::string("iso-8859-1")));
        // Content-Type wins over meta
        static const kimix::string_view kMeta[] = {"gbk"};
        expect(eq(fu::pick_encoding(
                      "text/html; charset=windows-1252",
                      kimix::span<const kimix::string_view>(kMeta, 1)),
                  kimix::string("windows-1252")));
        // Meta used when Content-Type has no charset
        expect(eq(fu::pick_encoding(
                      "text/html",
                      kimix::span<const kimix::string_view>(kMeta, 1)),
                  kimix::string("gbk")));
        // Unknown candidates fall back to utf-8
        static const kimix::string_view kWeird[] = {"x-unknown"};
        expect(eq(fu::pick_encoding(
                      "text/html",
                      kimix::span<const kimix::string_view>(kWeird, 1)),
                  kimix::string("utf-8")));
        // http-equiv content= style candidate
        static const kimix::string_view kMeta2[] = {
            "text/html; charset=big5"};
        expect(eq(fu::pick_encoding(
                      "text/html",
                      kimix::span<const kimix::string_view>(kMeta2, 1)),
                  kimix::string("big5")));
    };

    // ---------------------------------------------------------------------
    // FetchUrl Tool class wrapper
    // ---------------------------------------------------------------------
    auto run_tool = [](fu::FetchUrl &tool, ToolParams const *params) {
        tool(params);
        kimix::vector<char> const &json = tool.last_result();
        ToolParams out;
        out.deserialize(kimix::span<char const>(json.data(), json.size()));
        return out;
    };

    "fetch_url_class_missing_parameters"_test = [&] {
        fu::FetchUrl tool(nullptr);
        ToolParams out = run_tool(tool, nullptr);
        auto const ok_el = out.get("ok");
        expect(ok_el != nullptr);
        expect(ok_el->is_bool());
        expect(!ok_el->as_bool());
        auto const err_el = out.get("error");
        expect(err_el != nullptr);
        expect(err_el->is_string());
    };

    "fetch_url_class_missing_html"_test = [&] {
        fu::FetchUrl tool(nullptr);
        ToolParams params;
        params.values["url"] =
            ValueElement::make_string(kimix::string("https://example.com"));
        ToolParams out = run_tool(tool, &params);
        auto const ok_el = out.get("ok");
        expect(ok_el != nullptr);
        expect(!ok_el->as_bool());
    };

    "fetch_url_class_basic_conversion"_test = [&] {
        fu::FetchUrl tool(nullptr);
        ToolParams params;
        params.values["html"] = ValueElement::make_string(
            kimix::string("<html><body><h1>Title</h1><p>Hello</p></body></html>"));
        ToolParams out = run_tool(tool, &params);
        auto const ok_el = out.get("ok");
        expect(ok_el != nullptr);
        expect(ok_el->as_bool());
        auto const md_el = out.get("markdown");
        expect(md_el != nullptr);
        expect(md_el->is_string());
        expect(eq(md_el->as_string(),
                  kimix::string("# Title\n\nHello")));
    };

    "fetch_url_class_max_length"_test = [&] {
        fu::FetchUrl tool(nullptr);
        ToolParams params;
        params.values["html"] = ValueElement::make_string(
            kimix::string("<html><body><h1>Title</h1><p>Hello world</p></body></html>"));
        params.values["max_length"] = ValueElement::make_int(5);
        ToolParams out = run_tool(tool, &params);
        auto const ok_el = out.get("ok");
        expect(ok_el != nullptr);
        expect(ok_el->as_bool());
        auto const md_el = out.get("markdown");
        expect(md_el != nullptr);
        expect(eq(md_el->as_string(), kimix::string("# Tit")));
    };

    "fetch_url_class_extract_false"_test = [&] {
        fu::FetchUrl tool(nullptr);
        ToolParams params;
        params.values["html"] = ValueElement::make_string(
            kimix::string("<nav>nav</nav><h1>T</h1>"));
        params.values["extract"] = ValueElement::make_bool(false);
        ToolParams out = run_tool(tool, &params);
        auto const ok_el = out.get("ok");
        expect(ok_el != nullptr);
        expect(ok_el->as_bool());
        auto const md_el = out.get("markdown");
        expect(md_el != nullptr);
        // With extract=false the <nav> element is not decomposed.
        expect(md_el->as_string().find("nav") != kimix::string::npos);
    };

    return 0;
}
