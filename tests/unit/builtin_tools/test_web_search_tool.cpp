// Test for the web_search built-in tool kernels (builtin_tools/web_search_tool.h).
//
// This test covers the plan §7 list against byte-exact Python goldens
// (captured from kimi-cli/src/kimi_cli/tools/web/content.py / search.py /
// providers.py):
// - convert_base64_images_to_links: markdown/parenthesised/bare data-URL
//   replacement, alt stripping, whitespace payloads, non-matching forms, and
//   the payload-collection overload (document order)
// - make_cache_slug: pinned XXH64 digest vectors (xxhash.xxh64(...)[:10],
//   NOT kimix::hash64/XXH3) incl. empty, ASCII URLs, non-ASCII UTF-8 URLs
// - make_cache_file_name: host slug + digest + ".md" (port/portless, IPv6,
//   no-scheme fallback "page", 60-char slug truncation)
// - store_full_text: real temp-dir write, file naming, and the
//   MAX_STORED_TEXT_CHARS cap marker
// - truncate_with_footer: under-limit passthrough, head+tail newline snapping,
//   byte-exact footer (stored / not-stored branches), code-point budgeting for
//   non-ASCII content
// - build_search_output: byte-exact rendering, include_content block,
//   URL de-dup (first wins), per-item content cap, overall byte cap
// - clamp_search_limit / clamp_extract_char_limit
// - resolve_active_provider: explicit-config, single-eligible, legacy
//   preference walk, nullopt fallback
#include "ut/ut.hpp"

#include "builtin_tools/web_search_tool.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace boost::ut;
using namespace boost::ut::literals;
using namespace kimix::builtin_tools::web_search;

namespace {

kimix::string ks(std::string_view s) { return kimix::string(s); }

web_item make_item(kimix::string title, kimix::string date, kimix::string url,
                   kimix::string snippet, kimix::string content = {}) {
    web_item it;
    it.title = std::move(title);
    it.date = std::move(date);
    it.url = std::move(url);
    it.snippet = std::move(snippet);
    it.content = std::move(content);
    return it;
}

// U+2500 BOX DRAWINGS LIGHT HORIZONTAL (E2 94 80) repeated `n` times — the
// footer rules of content.py truncate_with_footer ("─" * 8 / 29).
kimix::string ws_box(size_t n) {
    kimix::string s;
    s.reserve(n * 3u);
    for (size_t i = 0; i < n; ++i) {
        s += "\xE2\x94\x80";
    }
    return s;
}

// Deterministic temp dir for the real-FS store_full_text test.
std::filesystem::path temp_dir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path() /
               ("kimix_ws_test_" + std::to_string(stamp));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return dir;
}

} // namespace

int main(int argc, char *argv[]) {
    boost::ut::detail::cfg::parse_arg_with_fallback(
        argc, const_cast<const char **>(argv));

    "convert_base64_md_alt"_test = [] {
        expect(eq(convert_base64_images_to_links(
                      "![alt text](data:image/png;base64,AAAA)"),
                  ks("[IMAGE: alt text]")));
        expect(eq(convert_base64_images_to_links(
                      "![](data:image/png;base64,AAAA)"),
                  ks("[IMAGE]")));
        expect(eq(convert_base64_images_to_links(
                      "![  spaced  ](data:image/png;base64,AAAA)"),
                  ks("[IMAGE: spaced]")));
        expect(eq(convert_base64_images_to_links(
                      "![a](  data:image/png;base64,AAAA)"),
                  ks("[IMAGE: a]")));
        expect(eq(convert_base64_images_to_links(
                      "![a](data:image/png;base64,AA AA\nBB)"),
                  ks("[IMAGE: a]")));
        expect(eq(convert_base64_images_to_links(
                      "![a](data:image/png;base64,AAAA) "
                      "![b](data:image/png;base64,BBBB)"),
                  ks("[IMAGE: a] [IMAGE: b]")));
    };

    "convert_base64_paren_bare"_test = [] {
        expect(eq(convert_base64_images_to_links(
                      "see (data:image/jpeg;base64,BBBB==) here"),
                  ks("see [IMAGE] here")));
        expect(eq(convert_base64_images_to_links(
                      "prefix data:image/gif;base64,CCCC suffix"),
                  ks("prefix [IMAGE] suffix")));
        expect(eq(convert_base64_images_to_links(
                      "(data:image/png;base64,AA\nBB)"),
                  ks("[IMAGE]")));
        expect(eq(convert_base64_images_to_links(
                      "![img](data:image/png;base64,AAAA) and "
                      "(data:image/png;base64,BBBB) and "
                      "data:image/png;base64,CCCC"),
                  ks("[IMAGE: img] and [IMAGE] and [IMAGE]")));
        expect(eq(convert_base64_images_to_links(
                      "<img src=\"data:image/png;base64,AAAA\">"),
                  ks("<img src=\"[IMAGE]\">")));
    };

    "convert_base64_non_matches"_test = [] {
        // No closing paren -> pass-2 fails, pass-3 replaces the bare blob.
        expect(eq(convert_base64_images_to_links("(data:image/png;base64,AAAA"),
                  ks("([IMAGE]")));
        // '!' is not a base64 char -> payload run stops, md/paren fail.
        expect(eq(convert_base64_images_to_links(
                      "![a](data:image/png;base64,ZZZZ!!)"),
                  ks("![a]([IMAGE]!!)")));
        // http(s) image links untouched.
        expect(eq(convert_base64_images_to_links(
                      "![a](https://example.com/x.png)"),
                  ks("![a](https://example.com/x.png)")));
        // Empty payload, non-base64 image, multi-part type: untouched.
        expect(eq(convert_base64_images_to_links("data:image/png;base64,"),
                  ks("data:image/png;base64,")));
        expect(eq(convert_base64_images_to_links("data:image/png;foo,AAAA"),
                  ks("data:image/png;foo,AAAA")));
        expect(eq(convert_base64_images_to_links(
                      "data:image/svg+xml;charset=utf-8;base64,AAAA"),
                  ks("data:image/svg+xml;charset=utf-8;base64,AAAA")));
        // Idempotent on already-replaced placeholders.
        expect(eq(convert_base64_images_to_links("[IMAGE: alt]"),
                  ks("[IMAGE: alt]")));
    };

    "convert_base64_payload_collection"_test = [] {
        kimix::vector<kimix::string> payloads;
        const auto out = convert_base64_images_to_links(
            "![img](data:image/png;base64,AAAA) and "
            "(data:image/png;base64,BBBB) and data:image/png;base64,CCCC",
            payloads);
        expect(eq(out, ks("[IMAGE: img] and [IMAGE] and [IMAGE]")));
        expect(eq(payloads.size(), size_t(3)));
        expect(eq(payloads[0], ks("AAAA")));
        expect(eq(payloads[1], ks("BBBB")));
        expect(eq(payloads[2], ks("CCCC")));
        // Whitespace inside a markdown payload is part of the raw payload.
        payloads.clear();
        convert_base64_images_to_links("![a](data:image/png;base64,AA AA\nBB)",
                                       payloads);
        expect(eq(payloads.size(), size_t(1)));
        expect(eq(payloads[0], ks("AA AA\nBB")));
    };

    "make_cache_slug_xxh64_vectors"_test = [] {
        // Pinned against python xxhash.xxh64(url.encode("utf-8")).hexdigest()[:10].
        expect(eq(make_cache_slug(""), ks("ef46db3751")));
        expect(eq(make_cache_slug("abc"), ks("44bc2cf5ad")));
        expect(eq(make_cache_slug("https://example.com"), ks("b131752760")));
        expect(eq(make_cache_slug("https://www.google.com/search?q=kimix"),
                  ks("327434108c")));
        expect(eq(make_cache_slug("https://en.wikipedia.org/wiki/XXH64"),
                  ks("998bee1c54")));
        expect(eq(make_cache_slug("https://news.ycombinator.com/item?id=12345"),
                  ks("b6cdc53f4b")));
        // Non-ASCII input is hashed as UTF-8 bytes (no ASCII gate needed).
        // "https://ru.wikipedia.org/wiki/Кёльн" (UTF-8: К=D0 9A, ё=D1 91,
        // л=D0 BB, ь=D1 8C, н=D0 BD).
        expect(eq(make_cache_slug("https://ru.wikipedia.org/wiki/"
                                  "\xD0\x9A\xD1\x91\xD0\xBB\xD1\x8C\xD0\xBD"),
                  ks("a51852ebe8")));
        expect(eq(make_cache_slug("The quick brown fox jumps over the lazy dog"),
                  ks("0b242d361f")));
    };

    "make_cache_file_name"_test = [] {
        expect(eq(make_cache_file_name("https://example.com/path"),
                  ks("example.com-399f2b812b.md")));
        expect(eq(make_cache_file_name("http://example.com:8080/x"),
                  ks("example.com-ce21fb81eb.md")));
        expect(eq(make_cache_file_name("https://[::1]:8080/x"),
                  ks("__1-cbe8568257.md")));
        expect(eq(make_cache_file_name("https://www.google.com/search?q=kimix"),
                  ks("www.google.com-327434108c.md")));
        expect(eq(make_cache_file_name("not a url"), ks("page-0bf4435f27.md")));
        expect(eq(make_cache_file_name(""), ks("page-ef46db3751.md")));
        // 60-char slug truncation.
        expect(eq(make_cache_file_name(
                      "https://verylonghostname123456789012345678901234567890"
                      "123456789012345678901234567890.example.com/"),
                  ks("verylonghostname1234567890123456789012345678901234567890"
                     "1234-bab15945f8.md")));
    };

    "store_full_text_roundtrip"_test = [] {
        const auto dir = temp_dir();
        const auto cache_dir = dir.string();
        kimix::string out_path;
        expect(store_full_text("https://example.com/path", "hello \xC3\xA9",
                               kimix::string_view(cache_dir), out_path));
        expect(!out_path.empty());
        const std::filesystem::path expected =
            std::filesystem::path(cache_dir) / "example.com-399f2b812b.md";
        expect(std::filesystem::exists(expected));
        std::ifstream in(expected, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        in.close();
        expect(eq(data, std::string("hello \xC3\xA9")));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    };

    "store_full_text_cap_marker"_test = [] {
        const auto dir = temp_dir();
        const auto cache_dir = dir.string();
        const std::string big(k_max_stored_text_chars + 1u, 'x');
        kimix::string out_path;
        expect(store_full_text("https://example.com/", big, cache_dir, out_path));
        std::ifstream in(out_path.c_str(), std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        in.close();
        expect(eq(data.size(), size_t(k_max_stored_text_chars) +
                                   std::string("\n\n[... stored copy truncated "
                                               "at 2,000,000 chars of 2,000,001; "
                                               "re-extract a more specific URL "
                                               "for the rest ...]")
                                       .size()));
        expect(data.find("[... stored copy truncated at 2,000,000 chars of "
                         "2,000,001; re-extract a more specific URL for the "
                         "rest ...]") != std::string::npos);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    };

    "truncate_with_footer_short"_test = [] {
        const auto no_store = [](kimix::string_view, kimix::string_view) {
            return kimix::optional<kimix::string>(std::nullopt);
        };
        const auto r =
            truncate_with_footer("hello world", "https://example.com", 100,
                                 no_store);
        expect(!r.was_truncated);
        expect(eq(r.text, ks("hello world")));
    };

    "truncate_with_footer_store_none"_test = [] {
        const auto no_store = [](kimix::string_view, kimix::string_view) {
            return kimix::optional<kimix::string>(std::nullopt);
        };
        kimix::string content;
        for (int i = 0; i < 60; ++i) {
            content += "line one\n";
        }
        const auto r = truncate_with_footer(content, "https://example.com", 100,
                                            no_store);
        expect(r.was_truncated);
        // Byte-exact Python golden (content.py truncate_with_footer with
        // store_full_text -> None).
        expect(eq(
            r.text,
            ks("line one\nline one\nline one\nline one\nline one\nline one\n"
               "line one\nline one\n\n[... middle omitted \xE2\x80\x94 see "
               "footer ...]\n\nline one\nline one\n\n\n\xE2\x94\x80\xE2\x94\x80"
               "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
               "\xE2\x94\x80 [TRUNCATED] \xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
               "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\n"
               "Showing 71 chars (head) + 18 chars (tail) of 540 total clean "
               "characters.\nFull text could not be stored; re-run web_extract "
               "on a more specific URL for the complete page.\n\xE2\x94\x80"
               "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
               "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
               "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
               "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
               "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
               "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80")));
    };

    "truncate_with_footer_store_path"_test = [] {
        const auto fake_store = [](kimix::string_view url,
                                   kimix::string_view) {
            expect(eq(url, kimix::string_view("https://example.com")));
            return kimix::optional<kimix::string>(
                "C:/fake/cache/web/example.com-399f2b812b.md");
        };
        kimix::string content;
        for (int i = 0; i < 60; ++i) {
            content += "line one\n";
        }
        const auto r = truncate_with_footer(content, "https://example.com", 100,
                                            fake_store);
        expect(r.was_truncated);
        // Byte-exact Python golden with store_full_text returning a path;
        // middle_start_line = head.count("\n") + 2 = 7 + 2 = 9.
        expect(eq(
            r.text,
            ks("line one\nline one\nline one\nline one\nline one\nline one\n"
               "line one\nline one\n\n[... middle omitted \xE2\x80\x94 see "
               "footer ...]\n\nline one\nline one\n\n\n\xE2\x94\x80\xE2\x94\x80"
               "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
               "\xE2\x94\x80 [TRUNCATED] \xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
               "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\n"
               "Showing 71 chars (head) + 18 chars (tail) of 540 total clean "
               "characters.\nFull text saved to: "
               "C:/fake/cache/web/example.com-399f2b812b.md\nTo read the "
               "omitted middle: read_file "
               "path=\"C:/fake/cache/web/example.com-399f2b812b.md\" "
               "offset=9 limit=200  (the file is the complete page; "
               "raise/lower offset to page through it).\n\xE2\x94\x80"
               "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
               "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
               "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
               "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
               "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
               "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80")));
    };

    "truncate_with_footer_unicode_code_points"_test = [] {
        const auto no_store = [](kimix::string_view, kimix::string_view) {
            return kimix::optional<kimix::string>(std::nullopt);
        };
        // "héllo\n" = 6 code points, 7 bytes per line; 40 lines = 240 cp.
        kimix::string content;
        for (int i = 0; i < 40; ++i) {
            content += "h\xC3\xA9llo\n";
        }
        const auto r = truncate_with_footer(content, "https://example.com", 100,
                                            no_store);
        expect(r.was_truncated);
        // Byte-exact Python golden: head_budget = 75 cp, the last newline sits
        // at code-point index 71 (> 37.5) so head snaps to 71 cp (11 lines +
        // "héllo"); tail = last 25 cp = "\n" + 4 lines, first '\n' at index 0
        // (< 12.5) so tail snaps forward to 4 full lines (24 cp).
        kimix::string expected;
        for (int i = 0; i < 11; ++i) {
            expected += "h\xC3\xA9llo\n";
        }
        expected += "h\xC3\xA9llo";
        expected += "\n\n[... middle omitted \xE2\x80\x94 see footer ...]\n\n";
        for (int i = 0; i < 4; ++i) {
            expected += "h\xC3\xA9llo\n";
        }
        expected += "\n\n" + ws_box(8) + " [TRUNCATED] " + ws_box(8) +
                    "\nShowing 71 chars (head) + 24 chars (tail) of 240 total "
                    "clean characters.\nFull text could not be stored; re-run "
                    "web_extract on a more specific URL for the complete "
                    "page.\n" +
                    ws_box(29);
        expect(eq(r.text, expected));
    };

    "build_search_output_golden"_test = [] {
        kimix::vector<web_item> items;
        items.push_back(make_item("Alpha", "2024-01-01", "https://a.example",
                                  "first snippet"));
        items.push_back(make_item("Beta", "", "https://b.example",
                                  "second snippet",
                                  "Full page content of beta."));
        items.push_back(make_item("Gamma", "2024-03-03", "https://g.example",
                                  ""));
        build_search_output_options opts;
        opts.include_content = true;
        const auto r = build_search_output(items, opts);
        expect(!r.truncated);
        expect(eq(r.omitted_items, size_t(0)));
        // Byte-exact rendering captured from search.py SearchWeb.__call__.
        expect(eq(r.text,
                  ks("Title: Alpha\nDate: 2024-01-01\nURL: https://a.example\n"
                     "Summary: first snippet\n\n---\n\nTitle: Beta\nDate: \n"
                     "URL: https://b.example\nSummary: second snippet\n\nFull "
                     "page content of beta.\n\n---\n\nTitle: Gamma\nDate: "
                     "2024-03-03\nURL: https://g.example\nSummary: \n\n")));
    };

    "build_search_output_dedup_by_url"_test = [] {
        kimix::vector<web_item> items;
        items.push_back(make_item("First", "2024-01-01", "https://a.example",
                                  "one"));
        items.push_back(make_item("Second", "2024-02-02", "https://a.example",
                                  "two"));
        items.push_back(make_item("Third", "2024-03-03", "https://b.example",
                                  "three"));
        build_search_output_options opts;
        const auto r = build_search_output(items, opts);
        expect(!r.truncated);
        expect(eq(r.omitted_items, size_t(1)));
        // First occurrence wins; the second (duplicate URL) item is dropped.
        expect(eq(r.text,
                  ks("Title: First\nDate: 2024-01-01\nURL: https://a.example\n"
                     "Summary: one\n\n---\n\nTitle: Third\nDate: 2024-03-03\n"
                     "URL: https://b.example\nSummary: three\n\n")));
    };

    "build_search_output_summary_and_content_cap"_test = [] {
        kimix::vector<web_item> items;
        items.push_back(make_item("A", "", "https://a.example", "s",
                                  "0123456789"));
        build_search_output_options opts;
        opts.summary = kimix::optional<kimix::string>("Answer summary.");
        opts.include_content = true;
        opts.max_content_chars = 4;
        const auto r = build_search_output(items, opts);
        expect(eq(r.text,
                  ks("Answer summary.\n\nTitle: A\nDate: \nURL: "
                     "https://a.example\nSummary: s\n\n0123\n\n")));
    };

    "build_search_output_byte_cap"_test = [] {
        kimix::vector<web_item> items;
        for (int i = 0; i < 10; ++i) {
            items.push_back(make_item(ks("Title" + std::to_string(i)), "",
                                      ks("https://example.com/" +
                                         std::to_string(i)),
                                      "snippet"));
        }
        build_search_output_options opts;
        opts.max_output_bytes = 120;
        const auto r = build_search_output(items, opts);
        expect(r.truncated);
        expect(r.omitted_items > size_t(0));
        expect(r.text.size() <= 120u);
        expect(r.text.find("output byte cap") != kimix::string::npos);
        expect(r.text.find("Title0") != kimix::string::npos);
    };

    "clamp_search_limit"_test = [] {
        expect(eq(clamp_search_limit(5), int32_t(5)));
        expect(eq(clamp_search_limit(50), int32_t(20)));
        expect(eq(clamp_search_limit(0), int32_t(1)));
        expect(eq(clamp_search_limit(-3), int32_t(1)));
        expect(eq(clamp_search_limit(20), int32_t(20)));
    };

    "clamp_extract_char_limit"_test = [] {
        expect(eq(clamp_extract_char_limit(15000), int64_t(15000)));
        expect(eq(clamp_extract_char_limit(100), int64_t(2000)));
        expect(eq(clamp_extract_char_limit(1'000'000), int64_t(500'000)));
        expect(eq(clamp_extract_char_limit(2000), int64_t(2000)));
        expect(eq(clamp_extract_char_limit(500'000), int64_t(500'000)));
    };

    "resolve_active_provider_rules"_test = [] {
        kimix::vector<web_provider_info> providers;
        providers.push_back({"kimi", true, false, true});
        providers.push_back({"ddgs", true, false, false});
        providers.push_back({"local", true, true, true});

        // Rule 1: explicit config wins (registered + capable), availability
        // ignored.
        expect(eq(*resolve_active_provider("ddgs", search_capability::search,
                                           providers),
                  ks("ddgs")));
        expect(eq(*resolve_active_provider("local", search_capability::search,
                                           providers),
                  ks("local")));
        // Config names an unregistered backend -> fall through (legacy walk:
        // kimi is available).
        expect(eq(*resolve_active_provider("bogus", search_capability::search,
                                           providers),
                  ks("kimi")));

        // Config names a registered provider that cannot serve the capability
        // -> fall through.
        kimix::vector<web_provider_info> p2;
        p2.push_back({"local", false, true, true}); // extract-only
        p2.push_back({"kimi", true, false, true});  // search-only
        expect(eq(*resolve_active_provider("local", search_capability::search,
                                           p2),
                  ks("kimi")));
        expect(eq(*resolve_active_provider("kimi", search_capability::extract,
                                           p2),
                  ks("local")));
    };

    "resolve_active_provider_single_eligible"_test = [] {
        kimix::vector<web_provider_info> providers;
        providers.push_back({"kimi", true, false, false});
        providers.push_back({"ddgs", true, false, true});
        providers.push_back({"local", true, false, false});
        // Only ddgs is available -> single-eligible rule returns it.
        expect(eq(*resolve_active_provider("", search_capability::search,
                                           providers),
                  ks("ddgs")));
    };

    "resolve_active_provider_legacy_and_none"_test = [] {
        kimix::vector<web_provider_info> providers;
        providers.push_back({"ddgs", true, false, true});
        providers.push_back({"local", true, false, true});
        // Two eligible -> legacy preference walk (kimi missing, ddgs first).
        expect(eq(*resolve_active_provider("", search_capability::search,
                                           providers),
                  ks("ddgs")));
        // Extract legacy: local -> kimi.
        providers[1].supports_extract = true;
        providers[0].supports_extract = false;
        expect(eq(*resolve_active_provider("", search_capability::extract,
                                           providers),
                  ks("local")));
        // Nothing available -> nullopt.
        kimix::vector<web_provider_info> none;
        none.push_back({"kimi", true, false, false});
        expect(!resolve_active_provider("", search_capability::search, none)
                    .has_value());
    };

    return 0;
}
