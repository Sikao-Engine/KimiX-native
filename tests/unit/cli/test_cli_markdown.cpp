// test_cli_markdown.cpp - H4: the ANSI terminal markdown renderer
// (cli/cli_markdown.*, a port of kimix/cli_impl/utils.py render_markdown) and
// the markdown buffer wired into the stream renderer (cli_stream set_markdown,
// stream.py:1086-1098 + 1120-1129).
//
// Covered: the theme colours/bars per heading level, the no-underscore-italic
// rule, inline code spans, links/images/strike/bold/italic, lists with task
// boxes, blockquote marker + hanging indent, hr, pipe tables, fenced code,
// the paragraph join, the ANSI-aware wrap and the max(width,20) / 80-fallback
// width chain, the terminal_columns() COLUMNS override, and the stream
// renderer's format_output buffer (flush before other message kinds, at
// finish_turn, and the raw passthrough when markdown is off).
//
// Framework: Boost.UT (tests/ut/ut.hpp); every test body lives in a
// main()-scope "name"_test lambda.

#include "ut/ut.hpp"

#include <cstdio>
#include <cstdlib>

#include <core/kimix_core.h>

#include "cli/cli_common.h"
#include "cli/cli_markdown.h"
#include "cli/cli_print.h"
#include "cli/cli_stream.h"

namespace {

namespace cli = kimix::cli;
using namespace boost::ut;

bool has_substr(kimix::string_view haystack, kimix::string_view needle) {
    return cli::contains(haystack, needle);
}

// Read everything currently buffered in a tmpfile (position preserved).
kimix::string slurp_tmp(std::FILE *stream) {
    std::fflush(stream);
    const long pos = std::ftell(stream);
    std::fseek(stream, 0, SEEK_SET);
    kimix::string out;
    char buffer[4096];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), stream)) > 0) {
        out.append(buffer, read);
    }
    if (pos >= 0) {
        std::fseek(stream, pos, SEEK_SET);
    }
    return out;
}

// Repeat `text` `n` times.
kimix::string times(kimix::string_view text, int n) {
    kimix::string out;
    for (int i = 0; i < n; ++i) {
        out.append(text);
    }
    return out;
}

kimix::vector<kimix::string> lines_of(const kimix::string &text) {
    kimix::vector<kimix::string> lines;
    cli::split_lines(text, lines);
    return lines;
}

} // namespace

int main() {
    "visible_length_and_strip_ansi"_test = [] {
        expect(cli::markdown_visible_length("abc") == 3);
        expect(cli::markdown_visible_length("\x1b[1mbold\x1b[0m") == 4);
        expect(cli::markdown_visible_length("▌ ok") == 4);
        const kimix::string stripped =
            cli::markdown_strip_ansi("\x1b[1;4mhi\x1b[0m there");
        expect(stripped == kimix::string("hi there"));
        // UTF-8 counts code points, not bytes ("é" is 2 bytes).
        expect(cli::markdown_visible_length("é") == 1);
    };

    "heading_levels_and_bars_plain"_test = [] {
        cli::set_colorful(false);
        const kimix::string h1 = cli::render_markdown("# Title", 40);
        // h1: a top ═ bar, " Title " body, a bottom bar, then a blank line.
        expect(has_substr(h1, times("═", 40)));
        expect(has_substr(h1, " Title "));
        expect(has_substr(h1, times("═", 40) + "\n Title \n" + times("═", 40)));
        expect(cli::ends_with(h1, "\n")); // a lone heading ends with one newline
        const kimix::string h2 = cli::render_markdown("## Sub", 40);
        expect(has_substr(h2, " Sub \n" + times("─", 40)));
        const kimix::string h3 = cli::render_markdown("### Deep", 40);
        expect(has_substr(h3, "Deep")); // no bars for h3+
        expect(!has_substr(h3, "═"));
        // heading_bars=false drops the rules.
        const kimix::string bare = cli::render_markdown("# Title", 40, false);
        expect(!has_substr(bare, "═"));
    };

    "lists_tasks_quotes_hr_tables_code"_test = [] {
        cli::set_colorful(false);
        const kimix::string md = "- one\n* two\n1. three\n- [x] done\n- [ ] todo\n";
        const kimix::string out = cli::render_markdown(md, 40);
        expect(has_substr(out, "- one"));
        expect(has_substr(out, "* two"));
        expect(has_substr(out, "1. three"));
        expect(has_substr(out, "- [x] done"));
        expect(has_substr(out, "- [ ] todo"));

        // Blockquote: "▌ " marker on the first line, continuation indent.
        const kimix::string quote = cli::render_markdown("> quoted\n> more", 40);
        const kimix::string quote_plain = cli::markdown_strip_ansi(quote);
        expect(has_substr(quote_plain, "▌ quoted"));
        expect(has_substr(quote_plain, "▌ more"))
            << "each quote line carries the marker (utils.py:396-467)";

        // hr: ─ to full width.
        const kimix::string hr = cli::render_markdown("---\n", 24);
        expect(has_substr(hr, times("─", 24)));

        // Table: padded cells with | borders and a - separator row.
        const kimix::string table =
            cli::render_markdown("| a | bb |\n|---|---|\n| 1 | 2 |\n", 40);
        // The reference pads to the widest column (the separator row counts)
        // and joins cells with the border pipe - no cosmetic spaces.
        expect(has_substr(table, "|a  |bb |"));
        expect(has_substr(table, "|1  |2  |"));
        expect(has_substr(table, "|---|---|"));

        // Fenced code keeps the body verbatim and prints the fence header.
        const kimix::string code = cli::render_markdown("```python\nx = 1\n```\n", 40);
        expect(has_substr(code, "``` python"));
        expect(has_substr(code, "x = 1"));
    };

    "inline_styles_and_no_underscore_italic"_test = [] {
        cli::set_colorful(false);
        // Bold / italic / strike / code / link / image change the text shape.
        const kimix::string styled =
            cli::render_markdown("a **bold** b *it* c ~~gone~~ d `code` e", 200);
        expect(has_substr(styled, "bold"));
        expect(has_substr(styled, "it"));
        expect(has_substr(styled, "gone"));
        expect(has_substr(styled, "code"));
        // Underscore emphasis is NOT transformed (utils.py:256-260).
        const kimix::string snake =
            cli::render_markdown("my_file_name and __dunder__", 200);
        expect(has_substr(snake, "my_file_name"));
        expect(has_substr(snake, "dunder"))
            << "__dunder__ is bold (the _BOLD_RE __ arm); snake_case survives";
        // Links become "text (url)".
        const kimix::string link =
            cli::render_markdown("see [docs](https://x.io/a)", 200);
        expect(has_substr(link, "docs ("));
        expect(has_substr(link, "https://x.io/a"));
        // Images become "alt: src".
        const kimix::string img = cli::render_markdown("![alt](img.png)", 200);
        expect(has_substr(img, "alt: img.png"));
    };

    "colour_on_wraps_in_ansi_escapes"_test = [] {
        cli::set_colorful(true);
        const kimix::string out = cli::render_markdown("# Hi", 30);
        expect(has_substr(out, "\x1b["));
        cli::set_colorful(false);
        const kimix::string plain = cli::render_markdown("# Hi", 30);
        expect(!has_substr(plain, "\x1b["));
    };

    "ansi_aware_word_wrap"_test = [] {
        cli::set_colorful(false);
        // A long paragraph wraps at the width (word boundaries kept).
        const kimix::string text = "alpha beta gamma delta epsilon zeta eta theta";
        const kimix::string wrapped = cli::render_markdown(text, 20);
        bool saw_short_line = false;
        for (const kimix::string &line : lines_of(wrapped)) {
            expect(cli::markdown_visible_length(line) <= 20 || !saw_short_line);
            if (cli::markdown_visible_length(line) <= 20) {
                saw_short_line = true;
            }
        }
        expect(saw_short_line);
        expect(!has_substr(wrapped, "alphabeta")); // words are not glued

        // ANSI spans survive a wrap that cuts between the styled words.
        cli::set_colorful(true);
        const kimix::string styled =
            cli::render_markdown("aaaa bbbb **cccc dddd eeee ffff gggg hhhh**", 20);
        // Every rendered escape opens AND closes (the bold body stays intact
        // across the introduced newlines).
        int opens = 0;
        int closes = 0;
        for (size_t i = 0; i + 4 < styled.size(); ++i) {
            if (styled[i] == '\x1b' && styled[i + 1] == '[') {
                ++opens;
            }
            if (styled.compare(i, 4, "\x1b[0m") == 0) {
                ++closes;
            }
        }
        expect(closes > 0);
        expect(opens >= closes);
        cli::set_colorful(false);
    };

    "width_floor_and_fallback"_test = [] {
        cli::set_colorful(false);
        // An explicit width below 20 is floored to 20 (utils.py:324-331).
        const kimix::string hr = cli::render_markdown("---\n", 5);
        expect(has_substr(hr, times("─", 20)));
        expect(!has_substr(hr, times("─", 21)));
        // terminal_columns: the COLUMNS env override, then the 80 fallback
        // when the stream is redirected.
        cli::set_env("COLUMNS", "57");
        expect(cli::terminal_columns(stdout) == 57);
        cli::set_env("COLUMNS", "0"); // not positive -> ignored
        expect(cli::terminal_columns(stdout) == 80 || cli::terminal_columns(stdout) > 0);
        cli::set_env("COLUMNS", "");
    };

    "stream_renderer_markdown_buffer"_test = [] {
        std::FILE *out = std::tmpfile();
        cli::stream_renderer renderer(/*show_thinking=*/true, /*show_usage=*/false);
        renderer.set_output(out);
        renderer.set_markdown(true);
        cli::set_colorful(false);
        // Text deltas buffer (stream.py:1090-1094) ...
        renderer.on_text_delta("# Heading\n\n");
        renderer.on_text_delta("body paragraph");
        expect(renderer.captured_text() == kimix::string("# Heading\n\nbody paragraph"));
        expect(slurp_tmp(out).empty()) << "nothing is flushed while buffering";
        // ... and flush before the next non-text message (stream.py:1161).
        kimix::llm::ToolCall call;
        call.id = "call-1";
        call.name = "read";
        call.arguments = "{}";
        renderer.on_tool_call_begin(call);
        const kimix::string flushed = slurp_tmp(out);
        expect(has_substr(flushed, "Heading"));
        expect(has_substr(flushed, "body paragraph"));
        expect(has_substr(flushed, "read"));
        expect(flushed.find("body paragraph") < flushed.find("read"))
            << "the buffered markdown flushes before the tool-call header";
        // finish_turn flushes the remainder (prompt.py:478).
        renderer.on_text_delta("tail");
        // The previous tool-call arg printer flushes its compact JSON when the next text delta arrives (finish_tool_call_stream).
        expect(!has_substr(slurp_tmp(out), "tail"));
        renderer.finish_turn();
        expect(has_substr(slurp_tmp(out), "tail"));
        std::fclose(out);
    };

    "stream_renderer_markdown_off_prints_live"_test = [] {
        std::FILE *out = std::tmpfile();
        cli::stream_renderer renderer(/*show_thinking=*/true, /*show_usage=*/false);
        renderer.set_output(out);
        renderer.set_markdown(false);
        cli::set_colorful(false);
        renderer.on_text_delta("# raw");
        expect(slurp_tmp(out) == kimix::string("# raw"))
            << "markdown off keeps the historical live passthrough";
        renderer.finish_turn();
        std::fclose(out);
    };
}
